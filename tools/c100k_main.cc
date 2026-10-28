// SLO #2（100k 长连接资源占用）与 SLO #1 的 p999 半边所用的**自研长连接客户端**。
// 契约 1.3 只允许"长连接场景"自研压测客户端，这里就是那唯一一处许可落地；
// 手段变更的记录在 docs/adr/ADR-17-c100k-hdr-measurement.md。
// 它**不是**产品的一部分，也不进 ctest（tools/ 的语义：被构建的、不是被断言的）。
//
// 为什么必须自研：wrk 4.1.0 与 hey 0.1.4 的分位表都只到 99%（B-001/B-004 记过），
// 而 SLO #1 要 p999；更关键的是它们都不能"握手完就挂着一动不动"——短连接测 C100K
// 是假的（契约 4.2 原话），只有长连接真考验每连接内存。
//
// 三条设计约束，每条都有代价，写在代码边上而不是心里：
//
// **一、每连接零缓冲。** 入向只在 12 字节的 `HTTP/1.1 101` 前缀上做逐字节匹配，
// 终止串用滚动自动机找 CRLFCRLF，头一收齐就**停止读**（空闲期不消费任何字节，这正是
// SLO #2 的"握手后空闲"口径）。所以本工具自己的每连接成本是 O(1)，而且**自报**
// RSS/fd——测量工具自己的内存绝不能混进被测进程的数里。
//
// **二、分位数存直方图而不是样本。** 100k × 60s 的样本量会让工具的内存变成被测对象。
// 桶两级：[0,1024µs) 每 1µs 一桶，其上每倍频程 16 桶。代价是分位数是**有界估计**，
// 所以输出一律带误差棒（`±x.xx%`），不给单点。
//
// **三、建连限速。** 本机 `somaxconn`=4096、监听 backlog 同量级，100k 条 SYN 一次性
// 砸下去测的是内核队列而不是网关。`--ramp-ms` 把 connect 摊开；脚本另外读网关侧的
// accept_errors / downstream_rejected_over_capacity / upstream_connect_failed 三个数，
// 任一非 0 就作废那一轮（工具丢了负载的那一轮不能记数）。
//
// 退出码：0 = 计划连接全部握手成功且零失败；非 0 = 这一轮的数没有资格进文档。
#include <dirent.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "net/epoll.h"
#include "net/fd.h"
#include "net/socket.h"

namespace net = l7gate::net;

namespace {

std::atomic<bool> g_stop_requested{false};
void on_signal(int) { g_stop_requested.store(true, std::memory_order_relaxed); }

constexpr int kTickMs = 50;
// 每轮建连封顶（见 run() 里那条"速率无关 = 没有速率"的实测教训）。
constexpr std::uint64_t kMaxOpenPerIteration = 512;
constexpr std::int64_t kPendingSweepMs = 250;
constexpr std::size_t kReadBufBytes = 16384;  // 整轮复用的一块读缓冲，不是每连接一份
constexpr std::string_view kExpectedPrefix = "HTTP/1.1 101";

// 握手请求。刻意不带任何业务帧：SLO #2 的口径是"握手完成后进入空闲"。
const char kUpgradeRequest[] =
    "GET /ws HTTP/1.1\r\n"
    "Host: c100k.test\r\n"
    "Connection: keep-alive, Upgrade\r\n"
    "Upgrade: websocket\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "\r\n";
constexpr std::size_t kUpgradeRequestLen = sizeof(kUpgradeRequest) - 1;

// 请求形态（`--requests-per-conn`）用的普通 keep-alive GET。SLO #1 的 p999 要的是
// **每请求**转发延迟，而这个数只有在"连接已经建好、请求一条接一条"的形态下才存在。
const char kGetRequest[] =
    "GET / HTTP/1.1\r\n"
    "Host: c100k.test\r\n"
    "Connection: keep-alive\r\n"
    "\r\n";
constexpr std::size_t kGetRequestLen = sizeof(kGetRequest) - 1;

std::int64_t monotonic_us() noexcept {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// --- HDR 风格直方图 -------------------------------------------------------
//
// [0, 1024 µs)      每 1 µs 一桶            → 亚毫秒段绝对精度 1 µs
// [1024 µs, ~67s)   每倍频程线性切 16 桶     → 每桶宽度 = 该倍频程下界 / 16
// 一共 1024 + 16×16 = 1280 个桶（10 KB 的计数数组），覆盖从 µs 到分钟级。
// 报分位时给"桶中点 + 半宽/中点"，所以读者看见的是估计值而不是假装的精确值。
class Histogram {
 public:
  static constexpr std::int32_t kLinearBuckets = 1024;
  static constexpr std::int32_t kSubBuckets = 16;
  static constexpr std::int32_t kOctaves = 16;
  static constexpr std::size_t kBucketCount =
      static_cast<std::size_t>(kLinearBuckets) + static_cast<std::size_t>(kSubBuckets) * kOctaves;

  struct Quantile {
    double midpoint_us = 0.0;
    double halfwidth_pct = 0.0;
    bool empty = true;
  };

  void record(std::int64_t value_us) noexcept {
    ++buckets_[bucket_of(value_us)];
    ++count_;
    sum_us_ += value_us;
    if (value_us > max_us_) max_us_ = value_us;
  }

  std::uint64_t count() const noexcept { return count_; }
  std::int64_t max_us() const noexcept { return max_us_; }
  double mean_us() const noexcept {
    return count_ == 0 ? 0.0 : static_cast<double>(sum_us_) / static_cast<double>(count_);
  }

  Quantile quantile(double q) const noexcept {
    if (count_ == 0) return {};
    // 第 ceil(q × N) 小的值（1-based）。q=1 时就是最大值。
    std::uint64_t want = 1;
    if (q < 1.0) {
      want = static_cast<std::uint64_t>(q * static_cast<double>(count_));
      if (want == 0) want = 1;
    }
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < kBucketCount; ++i) {
      if (buckets_[i] == 0) continue;
      seen += buckets_[i];
      if (seen < want) continue;
      const std::int64_t lo = bucket_low(static_cast<std::int32_t>(i));
      const std::int64_t hi = bucket_low(static_cast<std::int32_t>(i) + 1);
      Quantile out;
      out.empty = false;
      out.midpoint_us = static_cast<double>(lo) + static_cast<double>(hi - lo) / 2.0;
      out.halfwidth_pct = out.midpoint_us > 0.0
                              ? static_cast<double>(hi - lo) / 2.0 * 100.0 / out.midpoint_us
                              : 0.0;
      return out;
    }
    Quantile out;
    out.empty = false;
    out.midpoint_us = static_cast<double>(max_us_);
    return out;
  }

 private:
  static std::size_t bucket_of(std::int64_t v) noexcept {
    if (v < kLinearBuckets) return static_cast<std::size_t>(v < 0 ? 0 : v);
    std::int32_t octave = 0;
    std::int64_t base = kLinearBuckets;
    while (octave + 1 < kOctaves && v >= (base << 1)) {
      base <<= 1;
      ++octave;
    }
    const std::int64_t step = base / kSubBuckets;
    std::int32_t sub = static_cast<std::int32_t>((v - base) / (step == 0 ? 1 : step));
    if (sub >= kSubBuckets) sub = kSubBuckets - 1;
    if (sub < 0) sub = 0;
    return static_cast<std::size_t>(kLinearBuckets + octave * kSubBuckets + sub);
  }

  static std::int64_t bucket_low(std::int32_t idx) noexcept {
    if (idx <= 0) return 0;
    if (idx < kLinearBuckets) return idx;
    if (idx >= kLinearBuckets + kSubBuckets * kOctaves) return kLinearBucketsLL << kOctaves;
    const std::int32_t rel = idx - kLinearBuckets;
    const std::int32_t octave = rel / kSubBuckets;
    const std::int32_t sub = rel % kSubBuckets;
    const std::int64_t base = kLinearBucketsLL << octave;
    return base + (base / kSubBuckets) * sub;
  }

  static constexpr std::int64_t kLinearBucketsLL = 1024;

  std::array<std::uint64_t, kBucketCount> buckets_{};
  std::uint64_t count_ = 0;
  std::int64_t sum_us_ = 0;
  std::int64_t max_us_ = 0;
};

struct Options {
  std::vector<std::pair<std::string, int>> targets;  // (ip, port)，round-robin
  std::uint64_t connections = 1000;
  std::int64_t ramp_ms = 30000;
  std::int64_t hold_ms = 60000;
  std::int64_t handshake_timeout_ms = 10000;
  std::int64_t report_every_ms = 10000;
  // 0 = 隧道空闲形态（SLO #2 的口径：握手后一条业务帧都不发）。
  // >0 = 每条连接**闭环**发这么多条 keep-alive 请求，逐条记「请求写完 → 响应收完」的
  //      延迟。SLO #1 的 p999 要的是**每请求**转发延迟，握手那一个数不是同一个物理量，
  //      所以这一格必须有；不加这个形态，"HDR 分位是 p999 的取数手段"就是空话。
  std::uint64_t requests_per_conn = 0;
  // 响应完结判据的另一半：夹具（`l7gate_bench_upstream`）的固定体长。本工具不解析
  // Content-Length（那要引一套响应框架），所以完结 = 头终止串 + 这个字节数。
  // 它只对那个夹具成立，写在用法里而不是藏在代码里。
  std::size_t expect_body_bytes = 1024;
};

enum class Phase : std::uint8_t {
  kConnecting,    // 非阻塞 connect 已发起，等可写 + SO_ERROR
  kHandshaking,   // 升级请求已发（或部分已发），在读 101 头
  kRequesting,    // keep-alive 请求已发，在读响应（闭环：一次只在飞一条）
  kIdle,          // 隧道形态：101 收齐后空闲；请求形态：配额跑完，挂着不再发
  kFailed,        // 已计入失败，等被摘出 epoll
};

struct Conn {
  net::UniqueFd fd;
  Phase phase = Phase::kConnecting;
  std::size_t target_index = 0;
  std::int64_t started_us = 0;
  // 闭环请求的起点：**这条**请求写完那一刻（不是 connect 那一刻）。
  std::int64_t request_started_us = 0;
  std::uint64_t requests_done = 0;
  bool head_seen = false;      // 本轮响应的 CRLFCRLF 到了
  std::size_t body_remaining = 0;
  std::size_t sent_bytes = 0;
  // 请求是否已经写完过一次：计时起点只在该请求**刚写完**那一刻打一次。
  // 不加这个标志的话，注册时带上的那个 EPOLLOUT（电平意义下永远就绪）会让每一轮事件
  // 都再走一次"写完"分支，于是时间戳被打在**读响应的前一微秒**上——实测签名是
  // mean 4.2µs，而同一个窗口里每条请求的平均周期是 468µs，两个数互相打脸。
  bool request_written = false;
  // interest list 上现在还有没有 EPOLLOUT。注册时为了拿 connect 结果必须带上它，
  // 请求写完就该摘掉：留着它，事件循环会退化成"每次 epoll_wait 立刻返回"的自旋，
  // 而自旋的测量工具会同时污染它自己要测的那个 CPU 口径。
  bool mask_has_write = false;
  // 101 前缀只比 12 个字节，不存任何东西。对不上就记"不是 101"，仍然等头收齐才判，
  // 因为"回 200 的普通响应"也是合法的到达——只是它没有建立隧道。
  std::uint8_t prefix_pos = 0;
  bool prefix_ok = true;
  std::uint8_t match_state = 0;  // CRLFCRLF 滚动状态 0..3
  bool write_armed = false;
};

class Client {
 public:
  explicit Client(Options options) : options_(std::move(options)) {
    per_target_opened_.assign(options_.targets.size(), 0);
    per_target_established_.assign(options_.targets.size(), 0);
  }

  void run() {
    raise_fd_limit();
    const std::int64_t began_us = monotonic_us();
    began_us_ = began_us;
    last_tick_us_ = began_us;
    // 总预算 = 建连窗口 + 保持时长 + 握手上限 + 一点余量。超了就打印进度并以非 0 退出：
    // "挂着没人管"永远不该被读成"跑完了"。
    const std::int64_t hard_deadline_us =
        began_us + (options_.ramp_ms + options_.hold_ms + options_.handshake_timeout_ms + 30000) * 1000;
    std::int64_t next_sweep_us = monotonic_us() + kPendingSweepMs * 1000;
    std::int64_t next_report_us = monotonic_us() + options_.report_every_ms * 1000;

    while (true) {
      const std::int64_t now = monotonic_us();
      last_tick_us_ = now;
      if (g_stop_requested.load(std::memory_order_relaxed)) {
        stop_reason_ = "收到信号，提前结束（这一轮的数不进文档）";
        break;
      }
      // 1) 建连：按**经过的时间**摊开，不按"每轮一个 tick"。
      //    旧写法是 budget = remaining × 50ms / 剩余窗口，它假设每一轮循环都睡满
      //    kTickMs；可 epoll_wait 一有事件就立刻返回，于是整批连接在最初的几十毫秒里
      //    一次性砸出去——ramp 形同不存在。实测签名很干净：握手 p50 只随**总量**线性
      //    增长（250→18ms、500→34ms、1000→72ms、2000→143ms），把窗口从 2s 拉到 10s
      //    数字一模一样。速率无关 = 没有速率。
      if (started_ < options_.connections) {
        const std::int64_t ramp_us = options_.ramp_ms * 1000;
        const std::int64_t elapsed_us = now - began_us;
        std::uint64_t due = options_.connections;
        if (ramp_us > 0 && elapsed_us < ramp_us) {
          due = options_.connections * static_cast<std::uint64_t>(elapsed_us) /
                static_cast<std::uint64_t>(ramp_us);
        }
        std::uint64_t to_open = due > started_ ? due - started_ : 0;
        // 每轮封顶：被信号或长阻塞拖了一大截时，宁可晚几轮追平，也不在这一轮变成 burst。
        if (to_open > kMaxOpenPerIteration) to_open = kMaxOpenPerIteration;
        for (std::uint64_t i = 0; i < to_open; ++i) {
          open_one();
          ++started_;
        }
      }
      // 吞吐窗口的起点 = **所有连接都活着**那一刻，不是进程启动那一刻。用后者算 QPS 会把
      // ramp 那几百毫秒摊进分母，于是 1000 条连接 × 200 条请求报出 18k/s 而真实窗口只有
      // 41ms（244k/s）——差一个数量级的两个数，第一个还是"看起来更保守"的那个。
      if (traffic_begin_us_ == 0 && started_ == options_.connections &&
          established_ == options_.connections) {
        traffic_begin_us_ = now;
      }
      // 2) 达标之后才开始静置计时（SLO #2 的稳态判定是"连接数达标后静置 ≥60s"，
      //    不是"脚本睡了 60s"——这两句话在握手很慢的那一轮里会得到不同的数）。
      //    只对隧道形态成立；请求形态的终点是"配额跑完"，静置没有意义。
      if (options_.requests_per_conn == 0 && hold_end_us_ == 0 &&
          established_ == options_.connections) {
        hold_end_us_ = now + options_.hold_ms * 1000;
      }
      if (hold_end_us_ != 0 && now >= hold_end_us_) break;
      if (options_.requests_per_conn > 0 && connections_finished_ == options_.connections) {
        break;  // 每条连接的闭环配额都跑完了
      }
      if (now >= hard_deadline_us) {
        stop_reason_ = "超过总预算仍未达成（见下面的失败分类）";
        break;
      }
      if (now >= next_report_us) {
        report("progress");
        next_report_us = now + options_.report_every_ms * 1000;
      }
      if (now >= next_sweep_us) {
        sweep_pending(now);
        next_sweep_us = now + kPendingSweepMs * 1000;
      }

      int error = 0;
      const int ready = epoll_.wait(kTickMs, &events_, &error);
      if (ready < 0) {
        stop_reason_ = "epoll_wait 失败 errno=" + std::to_string(error);
        break;
      }
      for (const auto& ev : events_) {
        auto* raw = static_cast<Conn*>(ev.user_data);
        const int fd = raw->fd.get();
        auto it = by_fd_.find(fd);
        if (it == by_fd_.end() || it->second.get() != raw) continue;  // 批内已被摘掉
        const std::shared_ptr<Conn> conn = it->second;
        if (conn->phase == Phase::kConnecting) {
          if ((ev.events & (EPOLLOUT | EPOLLERR | EPOLLHUP)) == 0) continue;
          on_connectable(conn);
          continue;
        }
        if (conn->phase == Phase::kHandshaking || conn->phase == Phase::kRequesting) {
          if ((ev.events & EPOLLOUT) != 0 && !send_handshake(conn)) continue;
          if ((ev.events & (EPOLLIN | EPOLLHUP)) != 0) read_head(conn);
          continue;
        }
        if (conn->phase == Phase::kIdle && (ev.events & (EPOLLIN | EPOLLHUP)) != 0) {
          // 空闲期唯一的职责：看见对端收尾。网关在静置中途关掉任何一条连接都必须响——
          // 那正是"100k 条连接被稳定持有"这句话的反例。
          char probe[64];
          const ssize_t n = ::recv(conn->fd.get(), probe, sizeof(probe), MSG_DONTWAIT);
          if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            ++peer_closed_;
            drop(conn);
          }
        }
      }
      drop_dead();
    }
    last_tick_us_ = monotonic_us();  // QPS 的分母要含最后一批已完成的请求，不能停在上一轮循环顶上
    report("final");
  }

  std::uint64_t established() const noexcept { return established_; }
  std::uint64_t planned() const noexcept { return options_.connections; }
  std::uint64_t failed() const noexcept { return failed_; }
  std::uint64_t requests_done() const noexcept { return requests_done_; }
  std::uint64_t requests_planned() const noexcept { return planned_requests(); }
  std::uint64_t connections_finished() const noexcept { return connections_finished_; }
  std::uint64_t elapsed_ms() const noexcept {
    return static_cast<std::uint64_t>((last_tick_us_ - began_us_) / 1000);
  }
  const std::string& stop_reason() const noexcept { return stop_reason_; }

 private:
  // 闭环形态的总量：connections × requests_per_conn。单独成一个函数是因为它出现在
  // 三个地方（打印、退出码、QPS 分母），写错任何一处的代价都不一样。
  std::uint64_t planned_requests() const noexcept {
    return options_.requests_per_conn * options_.connections;
  }

  void raise_fd_limit() {
    rlimit limit{};
    if (::getrlimit(RLIMIT_NOFILE, &limit) != 0) {
      stop_reason_ = "getrlimit 失败 errno=" + std::to_string(errno);
      return;
    }
    fd_soft_before_ = static_cast<std::int64_t>(limit.rlim_cur);
    fd_hard_ = static_cast<std::int64_t>(limit.rlim_max);
    const std::uint64_t need = options_.connections + 64;  // + 常驻项（stdout/epoll/dirfd）
    if (limit.rlim_cur >= need) {
      fd_soft_after_ = static_cast<std::int64_t>(limit.rlim_cur);
      return;
    }
    limit.rlim_cur = static_cast<rlim_t>(std::min<std::uint64_t>(need, limit.rlim_max));
    if (::setrlimit(RLIMIT_NOFILE, &limit) != 0) {
      stop_reason_ = "setrlimit 失败 errno=" + std::to_string(errno);
      return;
    }
    fd_soft_after_ = static_cast<std::int64_t>(limit.rlim_cur);
    if (static_cast<std::uint64_t>(fd_soft_after_) < need) {
      stop_reason_ = "fd 上限抬不到需要值（soft=" + std::to_string(fd_soft_after_) +
                     " hard=" + std::to_string(fd_hard_) + " 需要=" + std::to_string(need) + "）";
    }
  }

  void open_one() {
    const std::size_t target_index = started_ % options_.targets.size();
    const auto& [addr, port] = options_.targets[target_index];
    net::ConnectOutcome outcome = net::start_connect(addr, static_cast<std::uint16_t>(port));
    if (outcome.status == net::ConnectStatus::kFailed) {
      ++failed_;
      ++failures_["connect-e" + std::to_string(outcome.error)];
      return;
    }
    auto conn = std::make_unique<Conn>();
    conn->fd = std::move(outcome.fd);
    conn->target_index = target_index;
    conn->started_us = monotonic_us();
    ++per_target_opened_[target_index];
    Conn* raw = conn.get();
    const int fd = raw->fd.get();
    std::shared_ptr<Conn> holder(std::move(conn));
    by_fd_.emplace(fd, holder);
    pending_.push_back(holder);  // 握手超时的判据要能扫到它（sweep_pending）
    // 可写不等于连上了（net/socket.h 那条注释说的就是这件事），所以 kInProgress 订
    // EPOLLOUT、后面还要问 SO_ERROR；回环上立刻连成的那条也走同一条路，不分叉。
    int error = 0;
    if (!epoll_.add(fd, EPOLLIN | EPOLLOUT, raw, &error)) {
      ++add_failures_;
      ++failed_;
      ++failures_["epoll-add-e" + std::to_string(error)];
      by_fd_.erase(fd);
      return;
    }
    raw->mask_has_write = true;  // 上面那条注册带上了可写；请求写完时由 send_handshake 摘掉
    on_connectable(holder);  // 立刻可写时省掉一个 tick；不满足时它会原样返回
  }

  void on_connectable(const std::shared_ptr<Conn>& conn) {
    int so_error = 0;
    if (!net::get_so_error(conn->fd.get(), &so_error)) {
      fail(conn, "so_error-e" + std::to_string(errno));
      return;
    }
    if (so_error != 0) {
      fail(conn, "so-e" + std::to_string(so_error));
      return;
    }
    conn->phase = options_.requests_per_conn == 0 ? Phase::kHandshaking : Phase::kRequesting;
    (void)send_handshake(conn);
  }

  // 当前该发哪一段字节：隧道形态是那张升级请求，请求形态是那条普通 GET。
  std::string_view current_request() const noexcept {
    return options_.requests_per_conn == 0 ? std::string_view(kUpgradeRequest, kUpgradeRequestLen)
                                           : std::string_view(kGetRequest, kGetRequestLen);
  }
  std::string_view expected_prefix() const noexcept {
    return options_.requests_per_conn == 0 ? kExpectedPrefix : std::string_view("HTTP/1.1 200");
  }

  // 把当前这条请求（可能一次写不完）写完。false = 这条连接已计入失败。
  bool send_handshake(const std::shared_ptr<Conn>& conn) {
    const std::string_view payload = current_request();
    while (conn->sent_bytes < payload.size()) {
      const ssize_t n = ::send(conn->fd.get(), payload.data() + conn->sent_bytes,
                               payload.size() - conn->sent_bytes, MSG_NOSIGNAL | MSG_DONTWAIT);
      if (n > 0) {
        conn->sent_bytes += static_cast<std::size_t>(n);
        continue;
      }
      if (n < 0 && errno == EINTR) continue;
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        if (!conn->write_armed) {
          conn->write_armed = true;
          int error = 0;
          epoll_.modify(conn->fd.get(), EPOLLIN | EPOLLOUT, conn.get(), &error);
        }
        return true;  // 等下一次可写
      }
      fail(conn, "send-e" + std::to_string(errno));
      return false;
    }
    if (conn->write_armed || conn->mask_has_write) {
      // 请求写完了，可写这个条件就没有存在必要了：摘掉。见 Conn::mask_has_write 那段。
      conn->write_armed = false;
      conn->mask_has_write = false;
      int error = 0;
      epoll_.modify(conn->fd.get(), EPOLLIN, conn.get(), &error);
    }
    // 延迟的起点是**这条请求刚写完那一刻**，且每条请求只打一次：SLO #1 要的是每请求
    // 转发延迟，把建连算进去就成了和握手延迟同一个数的两份拷贝；而每轮事件都重打一次
    // 的话，量出来的就是"读缓冲区的耗时"（几微秒），不是往返。
    if (!conn->request_written) {
      conn->request_written = true;
      conn->request_started_us = monotonic_us();
    }
    return true;
  }

  // 读到 EAGAIN 为止（ET 契约对测量工具同样成立）。只认三件事：状态行前缀、CRLFCRLF、
  // 以及（请求形态）头之后那么多个定长 body 字节。
  void read_head(const std::shared_ptr<Conn>& conn) {
    if (scratch_.empty()) scratch_.resize(kReadBufBytes);
    const std::string_view prefix = expected_prefix();
    while (true) {
      const ssize_t n = ::recv(conn->fd.get(), scratch_.data(), scratch_.size(), MSG_DONTWAIT);
      if (n > 0) {
        for (std::size_t i = 0; i < static_cast<std::size_t>(n); ++i) {
          const char c = scratch_[i];
          if (!conn->head_seen && conn->prefix_pos < prefix.size()) {
            if (c != prefix[conn->prefix_pos]) conn->prefix_ok = false;
            ++conn->prefix_pos;
          }
          if (conn->head_seen) {
            // 头齐了之后按体计：完结 = 读满 `--expect-body-bytes` 个字节。这里**不解析
            // Content-Length**，因为那要把响应框架搬进测量工具（ADR-17：工具越薄，
            // 它自己的内存越不可能是被测对象的一部分）。代价写在用法里：这一段只对
            // 那个固定体夹具成立。
            if (conn->body_remaining > 0) --conn->body_remaining;
            if (conn->body_remaining == 0) {
              if (!finish_response(conn)) return;  // 这条连接已收尾/失败
            }
            continue;
          }
          switch (conn->match_state) {
            case 0:
              conn->match_state = (c == '\r') ? 1 : 0;
              break;
            case 1:
              conn->match_state = (c == '\n') ? 2 : (c == '\r' ? 1 : 0);
              break;
            case 2:
              conn->match_state = (c == '\r') ? 3 : 0;
              break;
            default:  // 3：已吃掉 CRLF CR
              if (c == '\n') {
                if (options_.requests_per_conn == 0) {
                  finish_handshake(conn);
                  return;  // 头收齐。之后**不再读**：空闲口径就是"不消费任何字节"
                }
                conn->head_seen = true;
                conn->body_remaining = options_.expect_body_bytes;
                if (conn->body_remaining == 0) {
                  if (!finish_response(conn)) return;
                }
              } else {
                conn->match_state = (c == '\r') ? 1 : 0;
              }
              break;
          }
        }
        continue;
      }
      if (n == 0) {
        fail(conn, "peer-closed-mid-response");
        return;
      }
      const int e = errno;
      if (e == EINTR) continue;
      if (e == EAGAIN || e == EWOULDBLOCK) return;
      fail(conn, "recv-e" + std::to_string(e));
      return;
    }
  }

  // 一条请求-响应闭环跑完。false = 这条连接不再参与后续事件（失败或配额到顶后已摘读）。
  bool finish_response(const std::shared_ptr<Conn>& conn) {
    if (!conn->prefix_ok) {
      fail(conn, "status-not-200");  // 回错状态码还照样计延迟，是把故障读成性能
      return false;
    }
    request_hist_.record(monotonic_us() - conn->request_started_us);
    ++requests_done_;
    if (conn->requests_done == 0) {
      ++established_;  // 请求形态下"这条连接活着"的证据 = 第一条响应收完
      ++per_target_established_[conn->target_index];
    }
    ++conn->requests_done;
    if (conn->requests_done >= options_.requests_per_conn) {
      ++connections_finished_;
      conn->phase = Phase::kIdle;
      return true;
    }
    // 闭环：上一条收完才发下一条。一次只在飞一条，延迟数才是"每请求"而不是吞吐。
    conn->sent_bytes = 0;
    conn->request_written = false;  // 下一条请求要重新打一次时间戳（只此一次）
    conn->head_seen = false;
    conn->prefix_pos = 0;
    conn->prefix_ok = true;
    conn->match_state = 0;
    conn->body_remaining = 0;
    return send_handshake(conn);
  }

  void finish_handshake(const std::shared_ptr<Conn>& conn) {
    if (!conn->prefix_ok) {
      // 头齐了但不是 101：那是"没建立隧道"，不是慢。单独计数——混进 failed 会把
      // "对端不接受 upgrade" 与 "网络坏了" 两件事说成一件。
      ++not_101_;
      ++failed_;
      conn->phase = Phase::kFailed;
      ++dead_;
      return;
    }
    handshake_hist_.record(monotonic_us() - conn->started_us);
    ++established_;
    ++per_target_established_[conn->target_index];
    conn->phase = Phase::kIdle;
    if (conn->write_armed) {
      conn->write_armed = false;
      int error = 0;
      epoll_.modify(conn->fd.get(), EPOLLIN, conn.get(), &error);
    }
  }

  void fail(const std::shared_ptr<Conn>& conn, const std::string& reason) {
    if (conn->phase == Phase::kFailed) return;
    ++failed_;
    ++failures_[reason];
    conn->phase = Phase::kFailed;
    ++dead_;
  }

  void drop(const std::shared_ptr<Conn>& conn) {
    const int fd = conn->fd.get();
    if (fd >= 0) {
      int error = 0;
      epoll_.remove(fd, &error);  // 与 Epoll 的注销计数保持一致（网关侧同一条理由）
    }
    by_fd_.erase(fd);
  }

  // 批内被标记为 dead/failed 的连接，统一摘出 epoll 与注册表。同一批里同一个 fd 可能
  // 出现两次事件，边处理边 erase 就是给自己造悬垂指针（网关那边 retiring_ 防的就是它）。
  void drop_dead() {
    if (dead_ == 0) return;
    dead_ = 0;
    std::vector<std::shared_ptr<Conn>> still;
    still.reserve(by_fd_.size());
    for (auto& [fd, conn] : by_fd_) {
      if (conn->phase == Phase::kFailed) {
        int error = 0;
        epoll_.remove(fd, &error);
        continue;
      }
      if (conn->phase != Phase::kIdle) still.push_back(conn);
    }
    pending_ = std::move(still);
  }

  // 握手/响应超时：pending_ 只含"还没完成也没失败"的连接，每 250ms 扫一次。
  void sweep_pending(std::int64_t now) {
    for (const std::shared_ptr<Conn>& conn : pending_) {
      if (conn->phase != Phase::kHandshaking && conn->phase != Phase::kConnecting &&
          conn->phase != Phase::kRequesting) {
        continue;
      }
      const std::int64_t since = conn->phase == Phase::kRequesting ? conn->request_started_us
                                                                    : conn->started_us;
      if (now - since > options_.handshake_timeout_ms * 1000) {
        fail(conn, conn->phase == Phase::kRequesting ? "response-timeout" : "handshake-timeout");
      }
    }
  }

  // 本工具自己的 CPU 消耗（utime+stime，clock ticks）。读 /proc/self/stat 的第 14+15 项，
  // 按空白切分——这个可执行文件的 comm 里没有空格，所以不需要处理"括号里可能有空格"那一层
  // 麻烦；真要改 comm 名的话这行会算错，所以把前提写在这而不是藏在代码里。
  // 它存在的理由：闭环那一档里工具自己就是主要负载之一，不报它就等于让读者以为
  // "被测进程旁边站着一个不耗 CPU 的观察者"。
  std::int64_t self_cpu_ticks() const {
    std::int64_t ticks = -1;
    if (FILE* f = ::fopen("/proc/self/stat", "r")) {
      char line[1024];
      if (std::fgets(line, sizeof(line), f)) {
        std::int64_t fields[16] = {0};
        int n = 0;
        const char* p = line;
        while (*p != '\0' && n < 16) {
          while (*p == ' ') ++p;
          if (*p == '\0') break;
          fields[n++] = std::strtoll(p, nullptr, 10);
          while (*p != '\0' && *p != ' ' && *p != '\n') ++p;
        }
        if (n >= 15) ticks = fields[13] + fields[14];  // 第 14、15 项，0-based 就是 13、14
      }
      std::fclose(f);
    }
    return ticks;
  }

  void report(const char* tag) const {
    std::int64_t open_fds = -1;
    if (DIR* dir = ::opendir("/proc/self/fd")) {
      open_fds = 0;
      while (const dirent* e = ::readdir(dir)) {
        if (e->d_name[0] != '.') ++open_fds;
      }
      ::closedir(dir);
    }
    std::int64_t rss = -1;
    if (FILE* f = ::fopen("/proc/self/status", "r")) {
      char line[256];
      while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "VmRSS:", 6) == 0) {
          rss = std::strtoll(line + 6, nullptr, 10);
          break;
        }
      }
      std::fclose(f);
    }
    std::printf("[c100k] tag=%s mode=%s planned=%" PRIu64 " started=%" PRIu64 " established=%" PRIu64
                " failed=%" PRIu64 " not_101=%" PRIu64 " peer_closed=%" PRIu64
                " add_failures=%" PRIu64 " requests_done=%" PRIu64 " requests_planned=%" PRIu64
                " conns_finished=%" PRIu64 " self_rss_kib=%" PRId64 " self_open_fds=%" PRId64
                " fd_soft_before=%" PRId64 " fd_soft_after=%" PRId64 " fd_hard=%" PRId64
                " targets=%zu\n",
                tag, options_.requests_per_conn == 0 ? "tunnel-idle" : "closed-loop",
                options_.connections, started_, established_, failed_, not_101_, peer_closed_,
                add_failures_, requests_done_, planned_requests(), connections_finished_,
                rss, open_fds, fd_soft_before_, fd_soft_after_, fd_hard_, options_.targets.size());
    for (std::size_t i = 0; i < options_.targets.size(); ++i) {
      std::printf("[c100k] target=%zu addr=%s:%d opened=%" PRIu64 " established=%" PRIu64 "\n", i,
                  options_.targets[i].first.c_str(), options_.targets[i].second,
                  per_target_opened_[i], per_target_established_[i]);
    }
    // 两套分位共用一个打印器：隧道形态只有 handshake，闭环形态两套都有。
    // 名字带前缀，是为了让脚本按键精确取值而不是"猜这一行是哪个直方图的"。
    auto dump = [](const char* name, const Histogram& hist) {
      std::printf("[c100k] %s_samples=%" PRIu64 " %s_mean_us=%.1f %s_max_us=%" PRId64 "\n", name,
                  hist.count(), name, hist.mean_us(), name, hist.max_us());
      struct { const char* q_name; double q; } percentiles[] = {
          {"p50", 0.50}, {"p99", 0.99}, {"p999", 0.999}, {"p9999", 0.9999}};
      for (const auto& entry : percentiles) {
        const auto q = hist.quantile(entry.q);
        if (q.empty) {
          std::printf("[c100k] %s_%s_us=missing\n", name, entry.q_name);
        } else {
          std::printf("[c100k] %s_%s_us=%.0f %s_%s_halfwidth_pct=%.2f\n", name, entry.q_name,
                      q.midpoint_us, name, entry.q_name, q.halfwidth_pct);
        }
      }
    };
    dump("handshake", handshake_hist_);
    if (options_.requests_per_conn == 0) {
      // 观察者自己的账，隧道形态也要打印（闭环形态下面那一支本来就有这两个键）。
      // 起因是一轮 100k **直连上游**的探针：established 冻在 60,958 达 75 秒、失败一路涨到
      // 35,718，而上游那边 accepted=100000 / upgrades=100000 全服务成功——这一轮的结论本该
      // 是"客户端算不过来"，可当时只有进度行的差分能反推它。ADR-17 写着"工具自报
      // RSS/fd/CPU"，这一行才是把那句话落到两种形态上都成立。
      std::printf("[c100k] elapsed_ms=%" PRId64 " self_cpu_ticks=%" PRId64 "\n",
                  static_cast<std::int64_t>((last_tick_us_ - began_us_) / 1000), self_cpu_ticks());
    }
    if (options_.requests_per_conn > 0) {
      // QPS 的分母是**吞吐窗口**（所有连接都活着之后的那段），不是进程活过的总时长：
      // 后者把 ramp 摊进去，1000 条连接那档会报出 18k/s，而真实窗口是 244k/s——
      // 差一个数量级。两个都打印，窗口算不出来（没达标过）就退回总时长并标注。
      const std::int64_t from = traffic_begin_us_ != 0 ? traffic_begin_us_ : began_us_;
      const std::int64_t traffic_ms = std::max<std::int64_t>(1, (last_tick_us_ - from) / 1000);
      std::printf("[c100k] elapsed_ms=%" PRId64 " traffic_ms=%" PRId64
                  " requests_per_second=%.1f self_cpu_ticks=%" PRId64 "\n",
                  static_cast<std::int64_t>((last_tick_us_ - began_us_) / 1000), traffic_ms,
                  static_cast<double>(requests_done_) * 1000.0 / static_cast<double>(traffic_ms),
                  self_cpu_ticks());
      dump("request", request_hist_);
    }
    for (const auto& [reason, count] : failures_) {
      std::printf("[c100k] failure reason=%s count=%" PRIu64 "\n", reason.c_str(), count);
    }
    if (!stop_reason_.empty()) std::printf("[c100k] stop_reason=%s\n", stop_reason_.c_str());
    std::fflush(stdout);
  }

  Options options_;
  net::Epoll epoll_;
  std::unordered_map<int, std::shared_ptr<Conn>> by_fd_;
  std::vector<std::shared_ptr<Conn>> pending_;
  std::vector<net::Epoll::Event> events_{std::vector<net::Epoll::Event>(2048)};
  std::vector<char> scratch_;
  std::size_t started_ = 0;
  std::uint64_t established_ = 0;
  std::uint64_t failed_ = 0;
  std::uint64_t not_101_ = 0;
  std::uint64_t peer_closed_ = 0;
  std::uint64_t add_failures_ = 0;
  std::uint64_t dead_ = 0;
  std::uint64_t requests_done_ = 0;      // 闭环形态：收完的响应条数
  std::uint64_t connections_finished_ = 0;  // 闭环形态：配额跑完的连接条数
  std::int64_t began_us_ = 0;
  std::int64_t last_tick_us_ = 0;
  std::int64_t traffic_begin_us_ = 0;  // 所有连接都活着的那一刻（QPS 的分母起点）
  std::int64_t hold_end_us_ = 0;
  std::int64_t fd_soft_before_ = -1;
  std::int64_t fd_soft_after_ = -1;
  std::int64_t fd_hard_ = -1;
  std::string stop_reason_;
  Histogram handshake_hist_;
  Histogram request_hist_;  // 闭环形态的每请求延迟（SLO #1 的 p999 就是这一套分位）
  std::vector<std::uint64_t> per_target_opened_;
  std::vector<std::uint64_t> per_target_established_;
  std::unordered_map<std::string, std::uint64_t> failures_;
};

bool parse_targets(const char* text, std::vector<std::pair<std::string, int>>* out, std::string* why) {
  std::string_view rest(text);
  while (!rest.empty()) {
    const std::size_t comma = rest.find(',');
    const std::string_view item = comma == std::string_view::npos ? rest : rest.substr(0, comma);
    const std::size_t sep = item.rfind(':');
    if (sep == std::string_view::npos || sep + 1 >= item.size()) {
      *why = "目标要的是 <ipv4:port>，收到 " + std::string(item);
      return false;
    }
    const std::string addr(item.substr(0, sep));
    int port = 0;
    for (std::size_t i = sep + 1; i < item.size(); ++i) {
      const char c = item[i];
      if (c < '0' || c > '9') {
        *why = "端口不是十进制: " + std::string(item.substr(sep + 1));
        return false;
      }
      port = port * 10 + (c - '0');
      if (port > 65535) {
        *why = "端口越界: " + std::string(item.substr(sep + 1));
        return false;
      }
    }
    if (port <= 0) {
      *why = "端口不能为 0（客户端不猜端口，脚本读上游/网关打印的实际端口）";
      return false;
    }
    if (addr.empty()) {
      *why = "目标地址为空";
      return false;
    }
    out->emplace_back(addr, port);
    if (comma == std::string_view::npos) break;
    rest = rest.substr(comma + 1);
  }
  return !out->empty();
}

bool parse_i64(std::string_view text, std::int64_t* out) noexcept {
  errno = 0;
  char* end = nullptr;
  const std::string z(text);
  const long long v = std::strtoll(z.c_str(), &end, 10);
  if (errno != 0 || end == z.c_str() || *end != '\0' || v < 0) return false;
  *out = v;
  return true;
}

void usage(std::FILE* out) {
  std::fprintf(out,
      "l7gate_c100k —— 长连接测量客户端（自研；契约 1.3 唯一许可的场景，ADR-17）\n"
      "用法: l7gate_c100k --targets <ipv4:port>[,<ipv4:port>...] [选项]\n"
      "  --connections N          计划连接数（默认 1000）\n"
      "  --ramp-ms N              建连窗口（默认 30000；本机 somaxconn=4096，\n"
      "                           100k 条 SYN 一次性砸下去测的是内核队列不是网关）\n"
      "  --hold-seconds N         握手全部完成后再空闲保持的秒数（默认 60；SLO #2 的\n"
      "                           稳态判定要 ≥60，且计时起点是「连接数达标」那一瞬）\n"
      "  --handshake-timeout-ms N 单条握手上限（默认 10000）\n"
      "  --report-every-seconds N 中途进度（默认 10）\n"
      "  --requests-per-conn N    0=隧道空闲形态（默认，SLO #2）；>0=闭环请求形态：每条\n"
      "                           连接串行发 N 条 keep-alive GET，逐条记「请求写完 → 响应\n"
      "                           收完」的**每请求**延迟。SLO #1 的 p999 只有这个形态才存\n"
      "                           在——握手延迟不是同一个物理量，拿它填那一格是换口径\n"
      "  --expect-body-bytes N    闭环形态下响应体的定长（默认 1024 = 本仓库压测夹具的\n"
      "                           --body-bytes）。本工具不解析 Content-Length，完结判据是\n"
      "                           「头终止串 + 这个字节数」，所以它**只对该夹具成立**\n"
      "\n"
      "行为（默认形态）：非阻塞 connect → 发 HTTP/1.1 升级请求 → 收到完整 101 头记一次握手延迟 →\n"
      "之后不再消费任何字节（只保留「对端把我关了」的可见性）。\n"
      "每连接零缓冲，所以本工具的 RSS 与被测进程的 RSS 是两栏，报告里各自打印。\n"
      "延迟是 HDR 风格直方图（1µs 桶到 1ms、其后每倍频程 16 桶），分位数带误差棒。\n"
      "退出码：0 = 计划连接全部建立、零失败、闭环配额全部跑完；非 0 = 这一轮没有资格进文档。\n");
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string_view flag = argv[i];
    const char* value = (i + 1 < argc) ? argv[i + 1] : nullptr;
    std::int64_t n = 0;
    std::string why;
    if (flag == "--targets" && value != nullptr) {
      if (!parse_targets(value, &options.targets, &why)) {
        std::fprintf(stderr, "参数错误: %s\n", why.c_str());
        return 2;
      }
      ++i;
    } else if (flag == "--connections" && value != nullptr && parse_i64(value, &n) && n > 0) {
      options.connections = static_cast<std::uint64_t>(n);
      ++i;
    } else if (flag == "--ramp-ms" && value != nullptr && parse_i64(value, &n)) {
      options.ramp_ms = n == 0 ? 1 : n;
      ++i;
    } else if (flag == "--hold-seconds" && value != nullptr && parse_i64(value, &n)) {
      options.hold_ms = n * 1000;
      ++i;
    } else if (flag == "--handshake-timeout-ms" && value != nullptr && parse_i64(value, &n)) {
      options.handshake_timeout_ms = n;
      ++i;
    } else if (flag == "--report-every-seconds" && value != nullptr && parse_i64(value, &n)) {
      // 选项名是"秒"、字段是"毫秒"：这里漏乘 1000 的话进度行会每 5ms 打一次，
      // 而每条进度都要 opendir(/proc/self/fd) + 读 VmRSS——在 100k 连接那一档这就是
      // 每秒几十次全量 fd 扫描，直接把事件循环压死（实测：握手 p50 从亚毫秒变成 50ms）。
      options.report_every_ms = (n == 0 ? 1 : n) * 1000;
      ++i;
    } else if (flag == "--requests-per-conn" && value != nullptr && parse_i64(value, &n)) {
      options.requests_per_conn = static_cast<std::uint64_t>(n);
      ++i;
    } else if (flag == "--expect-body-bytes" && value != nullptr && parse_i64(value, &n)) {
      options.expect_body_bytes = static_cast<std::size_t>(n);
      ++i;
    } else if (flag == "--help" || flag == "-h") {
      usage(stdout);
      return 0;
    } else {
      std::fprintf(stderr, "参数错误: %s\n", std::string(flag).c_str());
      usage(stderr);
      return 2;
    }
  }
  if (options.targets.empty()) {
    std::fprintf(stderr, "参数错误: 必须给 --targets\n");
    usage(stderr);
    return 2;
  }
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  Client client(std::move(options));
  client.run();
  // 退出码的四个条件，缺一个就把这一轮作废：计划连接全部建立、零失败、没有异常停止原因、
  // 闭环配额全部跑完（隧道形态下 planned_requests()==requests_done()==0，自动成立）。
  // "少了几条但看着挺正常"是这类工具最危险的失败形态，所以它必须是非 0，不是警告。
  const bool passed = client.stop_reason().empty() && client.failed() == 0 &&
                      client.established() == client.planned() &&
                      client.requests_done() == client.requests_planned();
  if (!passed) {
    std::fprintf(stderr,
                 "[c100k] 这一轮没有资格进文档：established=%llu / planned=%llu failed=%llu "
                 "requests_done=%llu / requests_planned=%llu\n",
                 static_cast<unsigned long long>(client.established()),
                 static_cast<unsigned long long>(client.planned()),
                 static_cast<unsigned long long>(client.failed()),
                 static_cast<unsigned long long>(client.requests_done()),
                 static_cast<unsigned long long>(client.requests_planned()));
  }
  return passed ? 0 : 1;
}
