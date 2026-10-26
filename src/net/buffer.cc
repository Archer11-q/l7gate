#include "net/buffer.h"

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <unistd.h>

namespace l7gate::net {
namespace {

// 单次 ::read 的申请上限。64 KiB 是个折中：太小会让 ET 的读循环跑很多轮、
// syscall 次数直接进入 p99 归因；太大则一个慢连接就能把 buffer 顶起来，
// 每连接内存不好解释。这个数字要进 BENCHMARK 的归因段落，所以给它名字而不是内联。
constexpr std::size_t kMaxReadPerSyscall = 64u << 10;

constexpr std::size_t kMinCapacity = 256;

}  // namespace

Buffer::Buffer() : Buffer(Config{}) {}

Buffer::Buffer(const Config& config) : config_(config) {
  if (config_.initial_capacity < kMinCapacity) config_.initial_capacity = kMinCapacity;
  if (config_.max_capacity < config_.initial_capacity) config_.max_capacity = config_.initial_capacity;
  // 高水位是"停读"的那条线，它必须落在 [initial, max] 里，否则两个旋钮互相矛盾。
  config_.high_water_mark = std::clamp(config_.high_water_mark,
                                        std::size_t{1}, config_.max_capacity);
  config_.initial_capacity = std::min(config_.initial_capacity, config_.max_capacity);
  storage_.resize(config_.initial_capacity);
  read_hint_ = std::min(config_.initial_capacity, kMaxReadPerSyscall);
  record_peak();
}

// 外联定义析构函数要带空括号：`Buffer::~Buffer = default;` 会被 GCC 15 判成
// "invalid use of destructor as a type"。类内声明可以省括号，类外不行。
Buffer::~Buffer() = default;

Buffer::Buffer(Buffer&& other) noexcept
    : config_(other.config_),
      storage_(std::move(other.storage_)),
      read_pos_(other.read_pos_),
      write_pos_(other.write_pos_),
      peak_capacity_(other.peak_capacity_),
      read_hint_(other.read_hint_) {
  other.read_pos_ = 0;
  other.write_pos_ = 0;
  other.peak_capacity_ = 0;
  other.read_hint_ = 0;
}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
  if (this != &other) {
    config_ = other.config_;
    storage_ = std::move(other.storage_);
    read_pos_ = other.read_pos_;
    write_pos_ = other.write_pos_;
    peak_capacity_ = other.peak_capacity_;
    read_hint_ = other.read_hint_;
    other.read_pos_ = 0;
    other.write_pos_ = 0;
    other.peak_capacity_ = 0;
    other.read_hint_ = 0;
  }
  return *this;
}

bool Buffer::above_high_water_mark() const noexcept {
  return readable_bytes() >= config_.high_water_mark;
}

void Buffer::record_peak() noexcept {
  peak_capacity_ = std::max(peak_capacity_, storage_.size());
}

std::pair<std::uint8_t*, std::size_t> Buffer::writable_region(std::size_t hint) {
  if (hint == 0) return {storage_.data() + write_pos_, 0};

  // 读空了就把两个指针拉回起点。这一步顺手做的收益很大：
  // 稳态 keep-alive 下 buffer 会反复"写满→读空"，不做归零就会一路 memmove/扩容。
  if (read_pos_ == write_pos_) {
    read_pos_ = 0;
    write_pos_ = 0;
    // 突发流量过后要能把容量还回去。不还的话，一条曾经传过大 body 的连接
    // 会终身占着那块内存 —— 而对 100k 长连接来说，"终身"就是稳态本身。
    // 阈值取 2 倍初始容量，避免在 1 倍上下反复 realloc 抖动。
    if (storage_.size() > config_.initial_capacity * 2) {
      storage_.resize(config_.initial_capacity);
    }
  }

  if (writable_bytes() == 0 && read_pos_ > 0) {
    // 后面空着但前面已消费 —— 挪一次，比重新分配便宜。
    const std::size_t live = readable_bytes();
    std::memmove(storage_.data(), storage_.data() + read_pos_, live);
    read_pos_ = 0;
    write_pos_ = live;
  }

  // 只在**真满了**的时候才扩容。
  // 我前两版都不是这样：第一版按 hint 预留（1 KiB 的 keep-alive 也会把容量顶到
  // 8 KiB，因为"想读 4096"被当成了"要分配 4096 空位"）；更早一版固定申请 64 KiB，
  // 直接把每连接容量下限抬到 128 KiB —— 那是 SLO #2 预算的六倍。
  // 现在 hint 只表示"本轮至多想读这么多"，能读多少由实际剩余空间决定。
  if (writable_bytes() == 0) {
    std::size_t needed = write_pos_ + hint;
    if (needed > config_.max_capacity) needed = config_.max_capacity;
    const std::size_t target =
        std::min(std::max(storage_.size() * 2, needed), config_.max_capacity);
    if (target > storage_.size()) {
      storage_.resize(target);
      record_peak();
    }
  }

  return {storage_.data() + write_pos_, std::min(hint, writable_bytes())};
}

void Buffer::commit_written(std::size_t n) {
  write_pos_ = std::min(write_pos_ + n, storage_.size());
}

Buffer::ReadResult Buffer::read_until_blocked(int fd) {
  ReadResult result;
  for (;;) {
    // 停读线优先于 syscall：已经吃满就停，把"这个 fd 暂时别读了"的判断交给调用方
    // （S1-07 的背压钩子）。注意这里 stop 之后本轮**不会**看到 EAGAIN ——
    // 这是有意的：停读发生在内核之前，不是之后。
    if (readable_bytes() >= config_.high_water_mark) {
      result.limit_reached = true;
      break;
    }

    // 单次请求量必须夹在"距水位还剩多少"之内。
    // 我第一版没夹：只在线条处做入口检查、却按 kMaxReadPerSyscall 整块去读，
    // 于是一次 syscall 就能把高水位冲过去（实测 hwm=4096 时读进 8192）。
    // 夹住之后才有那条硬不变量：本函数返回时 readable_bytes() <= high_water_mark。
    const std::size_t room = config_.high_water_mark - readable_bytes();
    const std::size_t hint = std::min(read_hint_, room);

    const auto [ptr, space] = writable_region(hint);
    if (space == 0) {
      result.limit_reached = true;
      break;
    }

    const ssize_t n = ::read(fd, ptr, space);
    ++result.read_syscalls;
    if (n > 0) {
      commit_written(static_cast<std::size_t>(n));
      result.bytes_read += static_cast<std::size_t>(n);
      // 只有把申请量**读满**了，才说明对端确实比这个数更快，值得翻倍再要。
      // 1 KiB 的 keep-alive 报文永远读不满 4 KiB 的申请量，于是容量一次都不涨。
      if (static_cast<std::size_t>(n) == space && read_hint_ < kMaxReadPerSyscall) {
        read_hint_ = std::min(kMaxReadPerSyscall, read_hint_ * 2);
      }
      continue;
    }
    if (n == 0) {
      result.peer_closed = true;
      break;
    }
    const int e = errno;
    if (e == EINTR) continue;
    if (e == EAGAIN || e == EWOULDBLOCK) {
      result.would_block = true;
      break;
    }
    result.errno_value = e;
    break;
  }
  return result;
}

bool Buffer::consume(std::size_t n) {
  if (n > readable_bytes()) return false;
  read_pos_ += n;
  return true;
}

std::size_t Buffer::retrieve(void* out, std::size_t n) {
  const std::size_t take = std::min(n, readable_bytes());
  if (take > 0) {
    std::memcpy(out, readable_data(), take);
    read_pos_ += take;
  }
  return take;
}

bool Buffer::peek_consume(std::size_t n) {
  if (n > readable_bytes()) return false;
  read_pos_ += n;
  return true;
}

void Buffer::clear() noexcept {
  read_pos_ = 0;
  write_pos_ = 0;
}

}  // namespace l7gate::net
