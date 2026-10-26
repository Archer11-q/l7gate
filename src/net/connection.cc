#include "net/connection.h"

#include <algorithm>
#include <cstring>
#include <string_view>

#include "http/response_head.h"

namespace l7gate::net {

namespace {
constexpr std::uint8_t kCR = 0x0D;
constexpr std::uint8_t kLF = 0x0A;
// 头部区终止串的长度（CRLFCRLF）。游标回退量由它决定，见 advance_header_scan。
constexpr std::size_t kTerminatorLen = 4;
}  // namespace

Connection::Connection(UniqueFd fd, Role role, const Config& config, ReactorStats& stats)
    : fd_(std::move(fd)),
      role_(role),
      config_(config),
      stats_(&stats),
      inbound_(config.inbound) {
  state_ = (role == Role::kUpstream) ? State::kConnecting : State::kOpen;
  stats_->connections_constructed.fetch_add(1, std::memory_order_relaxed);
  stats_->live_connections.fetch_add(1, std::memory_order_relaxed);
}

Connection::~Connection() {
  // fd 正常已由 reactor 在关闭流程里关掉；这里兜住"对象被直接丢弃"的路径
  // （测试里就有：构造一个连接、喂字节、断言解析结果，然后让它出作用域）。
  close_fd();
  stats_->live_connections.fetch_sub(1, std::memory_order_relaxed);
  stats_->connections_destructed.fetch_add(1, std::memory_order_relaxed);
}

void Connection::close_fd() noexcept { fd_.close_now(); }

Buffer* Connection::outbound_or_create() noexcept {
  if (!outbound_) {
    outbound_ = std::make_unique<Buffer>(config_.outbound);
    stats_->outbound_buffers_created.fetch_add(1, std::memory_order_relaxed);
  }
  return outbound_.get();
}

bool Connection::queue_bytes(const void* data, std::size_t len) noexcept {
  if (len == 0) return true;
  Buffer* out = outbound_or_create();
  // 先整块判断能不能装下，再决定装不装。半装的状态比拒绝更难处理：
  // 排进来的是一整个响应或一段透传字节，截断它等于我造出一个畸形报文。
  // 返回 false 就是"到出向水位了"，调用方据此停止从对端读（S1-07 的入口）。
  if (out->readable_bytes() + len > out->high_water_mark()) return false;

  const auto* src = static_cast<const std::uint8_t*>(data);
  std::size_t off = 0;
  while (off < len) {
    const auto [ptr, space] = out->writable_region(len - off);
    if (space == 0) return false;  // 理论上到不了：上面的整块判断已经保证容量够
    const std::size_t take = std::min(space, len - off);
    std::memcpy(ptr, src + off, take);
    out->commit_written(take);
    off += take;
  }
  return true;
}

bool Connection::advance_header_scan() noexcept {
  const std::uint8_t* d = inbound_.readable_data();
  const std::size_t n = inbound_.readable_bytes();

  // 回退最多 (终止串长度 - 1) 字节重扫：上一轮完全可能停在 "\r"、"\r\n"、
  // "\r\n\r" 这种跨界位置，那 1~3 个字节不重看就会漏掉终止串。
  // 回退量是常数而不是"整个已扫区"，所以总量仍是线性的 —— 这正是这笔债的
  // 全部还款额：S1-03 里每轮重扫整个头部区，这里每轮只扫新来的字节 + 3。
  std::size_t i = (scan_pos_ > kTerminatorLen - 1) ? scan_pos_ - (kTerminatorLen - 1) : 0;
  if (i < n) {
    header_scan_bytes_ += n - i;
    stats_->header_scan_bytes.fetch_add(n - i, std::memory_order_relaxed);
  }

  for (; i + kTerminatorLen <= n; ++i) {
    if (d[i] == kCR && d[i + 1] == kLF && d[i + 2] == kCR && d[i + 3] == kLF) {
      scan_pos_ = i + kTerminatorLen;
      headers_complete_ = true;
      return true;
    }
  }
  // 没找到终止串。scan_pos_ 推到当前末尾，下一轮只从这里往后看。
  scan_pos_ = n;
  return false;
}

Connection::ReadRound Connection::on_readable() {
  if (tunneling_) {
    // 隧道里没有"请求"这个单位（ADR-09）：字节只负责搬，不负责被解析。
    // 上游角色本来就跳过解析，这里是下游侧的等价短路——少了它，101 之后的
    // 第一帧二进制字节会被当请求解析、判成畸形、整条连接被我自己的透传逻辑杀掉。
    ReadRound round;
    round.read = read_raw();
    return round;
  }
  ReadRound round;
  round.read = inbound_.read_until_blocked(fd_.get());
  if (round.read.limit_reached) {
    stats_->high_water_hits.fetch_add(1, std::memory_order_relaxed);
  }

  if (role_ == Role::kUpstream) {
    // 上游方向不解析请求。响应解析（状态行、框架）是 S1-06 的范围，
    // 本任务只把字节搬过去，所以这里连 parse_calls 都不该记。
    return round;
  }

  if (round.read.errno_value != 0) {
    return round;
  }

  if (!headers_complete_) {
    if (!advance_header_scan()) {
      // 还没看到终止串。"没看到"分两种：字节没到齐（继续等，半包不是错误 ——
      // 这条在 S1-03 是全称断言，在这里是同一条判断）；或者已到的字节数已经超过
      // max_headers_bytes，那它再也不可能合法。注意后者是**容量**裁决不是**合法性**
      // 裁决：CRLF 成不成对、obs-fold、裸 LF 一律由 parse_request 说，这里不重复判。
      if (inbound_.readable_bytes() > config_.parser.max_headers_bytes) {
        round.reject = true;
        round.error = http::ParseError::kHeadersTooLarge;
        stats_->requests_rejected.fetch_add(1, std::memory_order_relaxed);
      }
      return round;
    }
  }

  if (request_ready_) {
    if (request_views_valid()) {
      // 上一轮解析出的请求还没被 finish_request 收走。这里必须挡住，否则下一批
      // 字节到达时会把同一个请求再解析一遍、requests_parsed 再加一次 ——
      // 计数器虚高的表现是"吞吐看起来比实际高"，是最难发现的那类错误。
      return round;
    }
    // 另一种状态：请求还挂在手上，但它的 view 已经作废（这一轮的读触发了压缩或
    // 扩容，storage_ 换了地址）。**请求字节一个都没被消费**，所以正确的动作不是
    // 丢掉它，而是重解析一次，让新 view 指向当前的内存。
    // 不处理的话这条路径会卡死：request_ready_ 挡住新的解析，作废的 view 又没人
    // 能用，连接安静停在这里，而客户端正在等响应——表现是偶发挂住，不是报错。
    // 重解析是幂等的：游标 scan_pos_ 是相对可读区起点的，压缩和扩容都不改相对位置。
    request_.reset();
    request_ready_ = false;
  }

  http::ParseScratch* sc = scratch_or_create();
  http::ParsedRequest parsed{};
  ++parse_calls_;
  stats_->parse_calls.fetch_add(1, std::memory_order_relaxed);
  const http::ParseOutcome outcome =
      http::parse_request(inbound_.readable_data(), inbound_.readable_bytes(), *sc,
                          config_.parser, parsed);

  if (outcome.status == http::ParseStatus::kOk) {
    request_ = parsed;
    request_ready_ = true;
    request_inbound_generation_ = inbound_.generation();
    request_scratch_epoch_ = scratch_epoch_;
    round.request_ready = true;
    round.request_bytes = parsed.consumed;
    // view 作废后的重解析**不**重复计入 requests_parsed：同一个请求解析两次
    // 是两次真的工作（parse_calls 记它），但在业务上仍然只是一个请求。
    if (!request_parsed_counted_) {
      request_parsed_counted_ = true;
      stats_->requests_parsed.fetch_add(1, std::memory_order_relaxed);
    }
  } else if (outcome.status == http::ParseStatus::kError) {
    round.reject = true;
    round.error = outcome.error;
    stats_->requests_rejected.fetch_add(1, std::memory_order_relaxed);
  }
  return round;
}

Connection::WriteRound Connection::on_writable() {
  WriteRound round;
  if (!outbound_) {
    round.drained = true;
    return round;
  }
  round.write = outbound_->write_to_fd(fd_.get());
  round.drained = outbound_->readable_bytes() == 0;
  if (round.drained) {
    // 刷空了就把容量还回去：出向缓冲是被突发流量顶起来的，
    // 突发过后不还就等于把峰值当稳态收费（和 Buffer 的归还策略同一条理由）。
    outbound_->clear();
  }
  return round;
}

const http::ParsedRequest* Connection::request() const noexcept {
  if (!request_views_valid()) return nullptr;
  return &*request_;
}

bool Connection::request_views_valid() const noexcept {
  if (!request_.has_value()) return false;
  if (inbound_.generation() != request_inbound_generation_) return false;
  if (scratch_ == nullptr) return false;
  return scratch_epoch_ == request_scratch_epoch_;
}

void Connection::finish_request(std::size_t request_bytes) noexcept {
  const std::size_t available = inbound_.readable_bytes();
  const std::size_t take = std::min(request_bytes, available);
  if (take > 0) {
    inbound_.consume(take);
  }
  request_.reset();
  request_ready_ = false;
  request_parsed_counted_ = false;
  scan_pos_ = 0;
  headers_complete_ = false;
}

// 惰性分配。走到 make_unique 抛 bad_alloc 时，本函数的 noexcept 边界会直接
// terminate —— 那是**有意**的选择而不是疏漏：S1 没有"内存不足时优雅降级"这条需求，
// 而把 OOM 编成分支只会让它看起来被处理过、实际从没被执行过。真到需要谈 OOM
// 策略的时候，它属于一次带 ADR 的立项，不是这里的一个 if。
http::ParseScratch* Connection::scratch_or_create() noexcept {
  if (!scratch_) {
    scratch_ = std::make_unique<http::ParseScratch>(config_.max_header_count);
    ++scratch_epoch_;
    stats_->scratches_created.fetch_add(1, std::memory_order_relaxed);
  }
  return scratch_.get();
}

void Connection::release_scratch() noexcept {
  scratch_.reset();
  // 每次释放都推进 epoch，让任何还挂着的解析结果立刻判定为作废。
  ++scratch_epoch_;
  request_.reset();
  request_ready_ = false;
  request_parsed_counted_ = false;
}

Buffer::ReadResult Connection::read_raw() noexcept {
  Buffer::ReadResult read = inbound_.read_until_blocked(fd_.get());
  if (read.limit_reached) {
    stats_->high_water_hits.fetch_add(1, std::memory_order_relaxed);
  }
  return read;
}

std::size_t Connection::detect_response_head_101() noexcept {
  const std::uint8_t* d = inbound_.readable_data();
  const std::size_t n = inbound_.readable_bytes();
  std::size_t from = head_scan_pos_;
  if (from > 0) from -= 3;  // 回退一个终止串的重叠窗口，跨界形态不放过（同下游游标那条）
  const std::size_t term = http::find_header_terminator(d, n, from);
  if (term == std::string_view::npos) {
    head_scan_pos_ = (n > 3) ? n : 0;
    return std::string_view::npos;
  }
  // 101 是三条字节的定长前缀比较，合法响应头不可能大到让"重复判定"成为成本，
  // 所以这里不留"已判定非 101"的缓存位：一个需要记得失效的缓存位，
  // 就是 ARCHITECTURE 1.4 第四条说的那种"看起来有两个旋钮"的东西。
  head_scan_pos_ = 0;
  if (!http::looks_like_101_response_head(d, term + 4)) {
    return std::string_view::npos;  // 完整响应头但不是 101：隧道语义不成立
  }
  return term + 4;
}

}  // namespace l7gate::net
