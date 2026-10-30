// S1-04：事件循环 + 两端连接对象 + 转发泵。
//
// 这个文件断言的是"循环自己会不会漏东西 / 会不会挂住 / 会不会留下没人管的 fd"。
// 响应的 HTTP 语义、状态码映射、chunked 都不在这里（归 S1-06）；这里的响应只被
// 当成"一段已知长度的字节"来用，好让泵的存在性可以被证明而不是被我声称。
//
// 三条我觉得最值钱的：
//   一、TenThousandSequentialRequestsLeaveNothingAlive —— 验收句的可执行版本。
//   二、PipelinedRequestsInOneSegmentAreAllForwarded —— 边缘触发下"一次读带进多条
//       请求"必须在同一个事件里走完，少一条就是偶发挂住而不是报错。
//   三、IdleConnectionDoesNotSpinOnEpollOut —— EPOLLOUT 刷完不摘掉，空闲 CPU 直接
//       被一个自旋的循环吃掉；这条用"事件批次数"把它变成可断言的东西。
#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "filter/lb_filter.h"    // S3-02：链上的第二环（组 → 目标）
#include "filter/route_filter.h"  // S3-01：端到端那组用例装的就是它
#include "lb_fixture.h"           // S3-02：测试侧的组装配（含建环）
#include "net/reactor.h"
#include "net/reactor_group.h"
#include "route/balance.h"
#include "route/router.h"
#include "test_upstream.h"

namespace l7gate::net {
namespace {

using l7gate::testing::TestUpstream;

constexpr std::size_t kBodyBytes = 64;

// Reactor 常驻的 fd 数：监听 socket、epoll 实例、自唤醒 eventfd。
// 它们只在 ~Reactor 里关掉，所以"fd 回到基线"这类断言必须把它们算进去 ——
// 不然每条用例都会失败在"少了 3 个"这种我一眼看不出的差异上。
constexpr std::size_t kReactorPersistentFds = 3;

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
// GCC 的 -fsanitize=address / thread 会定义这两个宏。
constexpr bool k_rss_is_instrumented = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
// clang 走 __has_feature。
constexpr bool k_rss_is_instrumented = true;
#else
constexpr bool k_rss_is_instrumented = false;
#endif
#else
constexpr bool k_rss_is_instrumented = false;
#endif

// --- 进程侧的两个观测工具 -------------------------------------------------
// 用 /proc 而不是靠"我以为 close 了"：验收要的是「10000 请求后存活连接数 == 0」
// 和「fd 不增长」，这两个只有内核的记录能作证。
std::size_t count_open_fds() {
  DIR* dir = ::opendir("/proc/self/fd");
  if (dir == nullptr) return 0;  // 读不到就是没测到，不拿 0 当"没有泄漏"
  std::size_t n = 0;
  while (const dirent* entry = ::readdir(dir)) {
    if (entry->d_name[0] == '.') continue;
    ++n;
  }
  ::closedir(dir);
  return n;
}

std::uint64_t current_rss_kib() {
  FILE* file = ::fopen("/proc/self/status", "r");
  if (file == nullptr) return 0;
  char line[256];
  std::uint64_t rss = 0;
  while (std::fgets(line, sizeof(line), file)) {
    if (std::strncmp(line, "VmRSS:", 6) == 0) {
      rss = std::strtoull(line + 6, nullptr, 10);
      break;
    }
  }
  std::fclose(file);
  return rss;
}

// 有界等待一个**原子**观测收敛。
//
// 用法受一条自定的纪律约束（也写进了 src/net/stats.h）：这些计数不是同步手段。
// 这里每个 wait_until 之前都已经有了内核级观测 —— read() 返回 0、响应已经收到、
// 或连接已经建立成功。所以等的不是"事情有没有发生"，而是"同一批事件处理里
// 紧跟其后的那几步收尾有没有走完"。拿计数当"等到它就往下走"的**生产逻辑**，
// 才是那条被禁止的用法：表现是偶发挂住，而不是报错。
// 非原子的观测（epoll 计数、live_connection_count()）一律不在循环期间读，
// 只在 join 之后读。
template <typename Pred>
bool wait_until(Pred pred, int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return pred();
}

// 等一个原子计数**进入平台期**（连续两次采样之间不再变化，即静止满 2×60ms），
// 返回静止时的那个累计值；*reached 告诉你到底停住了没有。
//
// 为什么不写成"sleep 200ms 然后比两次"：内核侧还有在途字节要先被吸收完，平台期
// 才可能到达。固定 sleep 要么落在吸收期里（假红：字节明明停止了却还在涨），
// 要么赌一个没人保证的时长。S1-07 请求方向那条第一次跑就是这么红的 ——
// 2 MB 里有 1.86 MB 被接收缓冲吃掉，网关压根没机会持续停在停读线上。
//
// 这仍然不是"拿计数当同步手段"：调用点都先有一个内核级观测（写线程还卡在 write()
// 上、客户端一个字节都还没读），等的是"停住这件事有没有持续下去"。
std::uint64_t wait_for_plateau(std::atomic<std::uint64_t>& counter, int timeout_ms,
                               bool* reached) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  std::uint64_t previous = counter.load(std::memory_order_relaxed);
  int flat_samples = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    const std::uint64_t current = counter.load(std::memory_order_relaxed);
    flat_samples = (current == previous) ? flat_samples + 1 : 0;
    if (flat_samples >= 2) {
      *reached = true;
      return current;
    }
    previous = current;
  }
  *reached = false;
  return previous;
}

std::string request_text(std::size_t i) {
  return "GET /r" + std::to_string(i) + " HTTP/1.1\r\nHost: gateway.test\r\n\r\n";
}

// S1-05 的两种请求形态。close 那条带显式 `Connection: close`；
// upgrade 那条按 RFC 9113 §4.1 的握手形态来（Connection 含 upgrade 记号 +
// Upgrade 头 + Sec-WebSocket-Key），网关判定认的就是这两个记号。
std::string close_request_text(std::size_t i) {
  return "GET /r" + std::to_string(i) + " HTTP/1.1\r\nHost: gateway.test\r\n"
         "Connection: close\r\n\r\n";
}

std::string upgrade_request_text() {
  return "GET /ws HTTP/1.1\r\nHost: gateway.test\r\n"
         "Connection: keep-alive, Upgrade\r\n"
         "Upgrade: websocket\r\n"
         "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
         "Sec-WebSocket-Version: 13\r\n\r\n";
}

std::string expected_body(std::size_t seq) {
  std::string body = "seq=" + std::to_string(seq);
  while (body.size() < kBodyBytes) body += '.';
  body.resize(kBodyBytes);
  return body;
}

// --- 客户端 -------------------------------------------------------------
// 一个带内部缓存的最小 HTTP 客户端：一次 read 可能带回两条响应（粘包），
//  leftovers 必须留在客户端自己手里，否则后面的用例读到的就是上一条的尾巴。
class Client {
 public:
  Client() = default;
  ~Client() { close(); }
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  bool connect_to(int port, std::string* why) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
      *why = "socket() failed errno=" + std::to_string(errno);
      return false;
    }
    // 读超时是唯一能证明"卡住"的机制：没有它，一条挂住的连接会让整轮 ctest
    // 撞上 30 秒 TIMEOUT，失败信息是"超时"而不是"卡在哪一步"。
    const timeval tv{kReadTimeoutMs / 1000, (kReadTimeoutMs % 1000) * 1000};
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      *why = "connect() failed errno=" + std::to_string(errno);
      return false;
    }
    return true;
  }

  int fd() const noexcept { return fd_; }
  void close() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }
  // 半关闭：我只说不发了，但还留着读端等对面的收尾。
  // 用 close() 也能达到效果，但那样我就没有"我观察到网关关掉了这条连接"的
  // 第一手证据了 —— 而 live_connections 那个数只是这次关闭的**副产品**。
  void shutdown_write() {
    if (fd_ >= 0) ::shutdown(fd_, SHUT_WR);
  }

  bool write_all(std::string_view data) {
    std::size_t off = 0;
    while (off < data.size()) {
      const ssize_t n = ::write(fd_, data.data() + off, data.size() - off);
      if (n > 0) {
        off += static_cast<std::size_t>(n);
        continue;
      }
      if (n < 0 && errno == EINTR) continue;
      return false;
    }
    return true;
  }

  bool read_response(std::string* status, std::string* body, std::string* why) {
    if (!fill([](const std::string& s) { return s.find("\r\n\r\n") != std::string::npos; }, why)) {
      return false;
    }
    const std::size_t head_end = carry_.find("\r\n\r\n");
    const std::size_t content_length = content_length_of(carry_.substr(0, head_end));

    const std::size_t total = head_end + 4 + content_length;
    if (!fill([total](const std::string& s) { return s.size() >= total; }, why)) return false;

    const std::size_t first_cr = carry_.find("\r\n");
    *status = carry_.substr(0, first_cr);
    *body = carry_.substr(total - content_length, content_length);
    carry_.erase(0, total);
    return true;
  }

  // 对端收尾：read() 返回 0。超时返回 false（那是"没关"，不是"关得慢一点"）。
  bool wait_for_eof(std::string* why) {
    char buf[64];
    while (true) {
      const ssize_t n = ::read(fd_, buf, sizeof(buf));
      if (n == 0) return true;
      if (n > 0) continue;  // 还有响应体没读完，读完才算 EOF
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        *why = "读超时：对端没有关闭";
        return false;
      }
      *why = "read errno=" + std::to_string(errno);
      return false;
    }
  }

  // --- S1-05 的三种读法 ---------------------------------------------------
  // 隧道之后"响应"这个单位不存在了，read_response 的 Content-Length 口径失效，
  // 所以下面三个函数都只认**字节**：这是刻意贴近验收句的写法——
  // "帧内容不被解析、不被改写"只能用字节比对来证明，不能用语义解析来证明。

  // 读到下一个 CRLFCRLF 为止，返回含终止串的头部区（101 的形态断言用）。
  bool read_head(std::string* out, std::string* why) {
    if (!fill([](const std::string& s) { return s.find("\r\n\r\n") != std::string::npos; },
              why)) {
      return false;
    }
    const std::size_t term = carry_.find("\r\n\r\n");
    out->assign(carry_.substr(0, term + 4));
    carry_.erase(0, term + 4);
    return true;
  }

  // 恰好读 n 个字节（多读到的塞回 carry_）：透传"一块进一块出"的形状用它来证明。
  bool read_exact(std::string* out, std::size_t n, std::string* why) {
    char buf[4096];
    while (out->size() < n) {
      // 长度必须夹在 sizeof(buf) 以内。我第一版直接传 `n - out->size()`，S1-07 的
      // 1 MiB 响应于是让内核往一片 4 KiB 的栈上拷最多 1 MiB 字节——先以 segfault 的
      // 形态出现，再以 `read errno=14`（EFAULT）出现，两次都看着像"网关没把字节送完"。
      // 之前所有调用者的响应都不足 4 KiB，所以这个错一直藏在 helper 里没露头。
      const std::size_t want = std::min(n - out->size(), sizeof(buf));
      const ssize_t got = ::read(fd_, buf, want);
      if (got > 0) {
        out->append(buf, static_cast<std::size_t>(got));
        continue;
      }
      if (got == 0) {
        *why = "对端在凑满之前关闭";
        return false;
      }
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        *why = "读超时：字节没有透传回来";
        return false;
      }
      *why = "read errno=" + std::to_string(errno);
      return false;
    }
    return true;
  }

  // EOF 或"任何数据"先到算哪个。close 用例用它：隧道已关、只剩一个 FIN，
  // 拿到的必须是 0，不是又一次超时。
  bool wait_eof_or_data(std::string* why) {
    char buf[64];
    while (true) {
      const ssize_t n = ::read(fd_, buf, sizeof(buf));
      if (n == 0) return true;
      if (n > 0) {
        *why = "不该有字节回来，实际回来了 " + std::to_string(n);
        return false;
      }
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        *why = "读超时：EOF 没等到";
        return false;
      }
      *why = "read errno=" + std::to_string(errno);
      return false;
    }
  }

  // 读到 EOF 为止，把**全部**收到的字节带回来。S1-06 的截断用例用它：
  // 截断的响应按定义读不完整，任何"按框架齐了才算完"的读法都会把它判成失败。
  // 返回 false 只有一种可能——超时且没断，那是"网关根本没表态"，必须红。
  bool drain_to_eof(std::string* out, std::string* why) {
    char buf[4096];
    while (true) {
      const ssize_t n = ::read(fd_, buf, sizeof(buf));
      if (n > 0) {
        out->append(buf, static_cast<std::size_t>(n));
        continue;
      }
      if (n == 0) return true;
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        *why = "读超时：网关既没发完也没关闭";
        return false;
      }
      *why = "read errno=" + std::to_string(errno);
      return false;
    }
  }

 private:
  static constexpr int kReadTimeoutMs = 5000;

  bool fill(const std::function<bool(const std::string&)>& pred, std::string* why) {
    char buf[8192];
    while (!pred(carry_)) {
      const ssize_t n = ::read(fd_, buf, sizeof(buf));
      if (n > 0) {
        carry_.append(buf, static_cast<std::size_t>(n));
        continue;
      }
      if (n == 0) {
        *why = "对端在读完这条响应之前就关闭了";
        return false;
      }
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        *why = "读超时（" + std::to_string(kReadTimeoutMs) + "ms）：请求没有换来响应";
        return false;
      }
      *why = "read errno=" + std::to_string(errno);
      return false;
    }
    return true;
  }

  static std::size_t content_length_of(const std::string& headers) {
    std::string lower = headers;
    for (auto& c : lower) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    const std::size_t pos = lower.find("content-length:");
    if (pos == std::string::npos) return 0;
    return static_cast<std::size_t>(std::strtoull(headers.c_str() + pos + 15, nullptr, 10));
  }

  int fd_ = -1;
  std::string carry_;
};

// --- 夹具 ---------------------------------------------------------------
class Harness {
 public:
  struct Options {
    Reactor::Config config;
    bool with_upstream = true;
  };

  bool start(const Options& options, std::string* why) {
    Reactor::Config config = options.config;
    if (options.with_upstream) {
      if (!upstream_.start(why)) return false;
      upstream_.set_response_body_bytes(kBodyBytes);
      // S3-01：`Reactor::Config` 拿的是一张**目标表**（S2 那两个字段没了）。
      // 这个夹具只有一台测试上游，所以表里就一个目标、默认目标就是它——与 S2 的
      // 单端点形状完全等价，S1/S2 那 130 多条用例因此不需要各自重写一遍装配。
      // **调用方自己塞了表的时候不动它**：那是多端点用例（S2_06a / S3_01）在配自己的
      // 后端集，这里覆盖上去会把它们的表抹平。
      if (config.upstream_targets.empty()) {
        Reactor::Config::Target target;
        target.address = "127.0.0.1";
        target.port = static_cast<std::uint16_t>(upstream_.port());
        config.upstream_targets.push_back(std::move(target));
        config.default_target = 0;
      }
    }
    // 端口一律给 0 让内核选：测试之间并行跑、失败重跑都不该因为端口占用而随机失败。
    config.listen_port = 0;
    reactor_ = std::make_unique<Reactor>(config);
    // S1-08：filter 在**循环线程启动之前**装好。reactor.h 写的是"只能在 run() 之前添加"，
    // 测试如果直接调 reactor()->add_request_filter() 就是在 run() 之后改一条被循环线程
    // 逐条读的链——那是我自己写下不许做的事，TSan 抓到才算它尽职。
    for (auto& pending_filter : pending_filters_) {
      reactor_->add_request_filter(std::move(pending_filter));
    }
    pending_filters_.clear();
    int error = 0;
    if (!reactor_->start_listen(&error)) {
      *why = "start_listen failed errno=" + std::to_string(error);
      return false;
    }
    port_ = reactor_->listen_port();
    thread_ = std::thread([this] { batches_ = reactor_->run(); });
    return true;
  }

  // 给下一步 start() 用的 filter 队列。必须在 start() **之前**调用。
  void add_filter(std::unique_ptr<filter::RequestFilter> filter) {
    pending_filters_.push_back(std::move(filter));
  }

  void stop_loop() {
    if (reactor_ != nullptr) reactor_->stop();
    if (thread_.joinable()) thread_.join();
  }
  // S2-05：只 join、不发硬停。排水路径的"退出"是循环自己完成的（排干了才置
  // stop_requested_），用 stop_loop() 会把还没排完的水直接放掉——那测的是硬停，
  // 而硬停本来就是另一条路（上面的 stop_loop 保留给既有用例）。
  void join_loop() {
    if (thread_.joinable()) thread_.join();
  }
  // 停循环 + 销毁 Reactor（连带关掉监听 fd、epoll fd、eventfd）。
  // fd 复原那条断言必须在它之后才成立。
  void destroy() {
    stop_loop();
    reactor_.reset();
  }
  // 把夹具开过的东西全部关掉：Reactor 的三个常驻 fd + 测试上游的监听 fd。
  // fd 复原那条断言要的是"和我开始之前一模一样"，所以两边都得关——
  // 只关一半的话，等式右边就得写成一串"加上上游那个监听 socket"，
  // 那种算式正是迟早会算错的东西。
  void shutdown_all() {
    destroy();
    upstream_.stop_and_join();
  }

  Reactor* reactor() noexcept { return reactor_.get(); }
  TestUpstream& upstream() noexcept { return upstream_; }
  std::uint64_t batches() const noexcept { return batches_; }
  int port() const noexcept { return port_; }

  ~Harness() { destroy(); }

 private:
  // 声明顺序 = 销毁顺序的反面：reactor_（连同它手里的连接）必须先于上游析构，
  // 否则上游的监听线程会在网关还带着连接时被拆掉，关闭路径上的事件就不可确定了。
  TestUpstream upstream_;
  std::unique_ptr<Reactor> reactor_;
  std::thread thread_;
  std::uint64_t batches_ = 0;
  int port_ = -1;
  std::vector<std::unique_ptr<filter::RequestFilter>> pending_filters_;
};

Reactor::Config base_config() {
  Reactor::Config config;
  config.wait_timeout_ms = 50;  // 测试里缩短超时：停循环更快，卡死也暴露得更快
  return config;
}

Harness::Options default_options() {
  Harness::Options options;
  options.config = base_config();
  return options;
}

// --- 用例 ---------------------------------------------------------------

TEST(S1_04_Reactor, StopFromAnotherThreadEndsRun) {
  Harness harness;
  std::string why;
  ASSERT_TRUE(harness.start(default_options(), &why)) << why;
  EXPECT_GT(harness.port(), 0);

  // 先把循环"用起来"再停。不这么做的话，主线程 spawn 完立刻 stop() 可能赶在循环
  // 线程进入 run() 之前 —— run() 一次事件等待都没进就返回，"0ms 停止延迟"测的
  // 就成了"根本没在等"，而不是 eventfd 把阻塞中的 epoll_wait 叫醒。
  // 我第一次跑这条就是这个情况：断言挂在 batches 上（0 vs 1），而延迟好看得很。
  // 一条完整往返让"循环确实在跑"变成因果事实（响应只有它处理过才拿得到），
  // 下面那条 batches >= 1 也就不是在赌时序。
  Client client;
  ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;
  ASSERT_TRUE(client.write_all(request_text(1)));
  std::string status;
  std::string body;
  ASSERT_TRUE(client.read_response(&status, &body, &why)) << why;
  ASSERT_TRUE(harness.reactor()->running()) << "run() 还没起来就测停止延迟，测的是个空循环";

  const auto started = std::chrono::steady_clock::now();
  harness.reactor()->stop();
  harness.stop_loop();  // join 返回就是循环线程已经退出，这是唯一可靠的观测点
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - started)
                      .count();

  // 自唤醒通道存在的意义就在这里：没有 eventfd，stop() 只能等下一次 epoll_wait
  // 超时（这里是 50ms，生产配置里是 200ms）才生效。
  std::printf("[S1-04 stop] 一条往返之后 stop() 到循环退出实测 %lld ms（wait_timeout_ms=50）\n",
              static_cast<long long>(ms));
  EXPECT_LT(ms, 50) << "eventfd 自唤醒没起作用，停循环是靠超时兜的";
  EXPECT_GE(harness.batches(), 1u);
}

TEST(S1_04_Reactor, StopBeforeRunIsNotSwallowed) {
  // run() 开头那次 `running_ = true` 曾经把先到一步的 stop() 抹掉，于是循环
  // 永远停不下来 —— 我第一次跑这条用例时它是 TIMEOUT，不是断言失败。
  // 这条不 spawn 线程：先 stop() 再 run()，run() 必须一次都不进事件等待就返回。
  Reactor::Config config = base_config();
  Reactor reactor(config);
  int error = 0;
  ASSERT_TRUE(reactor.start_listen(&error)) << "start_listen errno=" << error;

  reactor.stop();
  const auto started = std::chrono::steady_clock::now();
  const std::uint64_t batches = reactor.run();
  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - started)
                      .count();

  std::printf("[S1-04 stop-before-run] run() 返回耗时=%lldus 事件批次=%llu\n",
              static_cast<long long>(us), static_cast<unsigned long long>(batches));
  EXPECT_LT(us, 50'000) << "已经送达的停止请求被 run() 的启动赋值抹掉了";
  EXPECT_EQ(batches, 0u) << "停过之后再进循环，一个事件都不该处理";
  EXPECT_FALSE(reactor.running());
}

TEST(S1_04_Reactor, ForwardsRequestAndResponseOverKeepAliveConnection) {
  Harness harness;
  std::string why;
  ASSERT_TRUE(harness.start(default_options(), &why)) << why;

  Client client;
  ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;

  for (std::size_t i = 1; i <= 5; ++i) {
    ASSERT_TRUE(client.write_all(request_text(i)));
    std::string status;
    std::string body;
    ASSERT_TRUE(client.read_response(&status, &body, &why)) << "第 " << i << " 条：" << why;
    EXPECT_EQ(status, "HTTP/1.1 200 OK");
    EXPECT_EQ(body, expected_body(i));
  }

  ReactorStats& stats = harness.reactor()->stats();
  EXPECT_EQ(stats.load(stats.requests_parsed), 5u);
  EXPECT_EQ(stats.load(stats.downstream_accepted), 1u) << "五条请求共用一条下游连接：keep-alive 生效";
  EXPECT_EQ(stats.load(stats.upstream_opened), 1u) << "回源连接是一条，不是每请求一条";
  EXPECT_EQ(stats.load(stats.requests_rejected), 0u);

  // 字节数对得上才说明"转发"不是只转发了解析结果：走的是原始字节。
  std::size_t request_bytes = 0;
  for (std::size_t i = 1; i <= 5; ++i) request_bytes += request_text(i).size();
  EXPECT_EQ(stats.load(stats.bytes_down_to_upstream), static_cast<std::uint64_t>(request_bytes));
  EXPECT_EQ(harness.upstream().requests_served(), 5u);

  // 注册表尺寸是非原子的，只能在循环线程退出之后读 —— 这也是"连接数"这个口径
  // 唯一没有竞争的读法。
  harness.stop_loop();
  EXPECT_EQ(harness.reactor()->live_connection_count(), 2u) << "上下游两个连接对象都在注册表里";
  EXPECT_EQ(stats.load(stats.live_connections), 2u);
}

TEST(S1_04_Reactor, PipelinedRequestsInOneSegmentAreAllForwarded) {
  Harness harness;
  std::string why;
  ASSERT_TRUE(harness.start(default_options(), &why)) << why;

  Client client;
  ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;

  // 三个请求一次写完：一个 TCP 段、一次 EPOLLIN。
  // 边缘触发不会把"还有数据"再说第二遍，所以这三条必须在同一个事件里走完；
  // 少处理一条的话，第三条永远躺在入向缓冲里，而客户端正等着它的响应 —— 挂住，不报错。
  const std::string batch = request_text(1) + request_text(2) + request_text(3);
  ASSERT_TRUE(client.write_all(batch));

  for (std::size_t i = 1; i <= 3; ++i) {
    std::string status;
    std::string body;
    ASSERT_TRUE(client.read_response(&status, &body, &why))
        << "粘包里的第 " << i << " 条没换来响应：可读事件里只处理了第一条请求。" << why;
    EXPECT_EQ(body, expected_body(i));
  }

  ReactorStats& stats = harness.reactor()->stats();
  EXPECT_EQ(stats.load(stats.requests_parsed), 3u);
  EXPECT_EQ(stats.load(stats.downstream_accepted), 1u);
}

TEST(S1_04_Reactor, TenThousandSequentialRequestsLeaveNothingAlive) {
  // 这条就是 TASKS.md 里那句改写过的验收标准的可执行版本：
  // 「10000 请求后存活连接数 == 0，引用计数归零并析构，ASan/LSan 报告零泄漏」。
  // 10000 次**串行**往返（发一条、收一条），和 curl 在一条 keep-alive 上的行为一致。
  constexpr std::size_t kRequests = 10000;

  const std::size_t fd_before = count_open_fds();
  Harness harness;
  std::string why;
  ASSERT_TRUE(harness.start(default_options(), &why)) << why;

  const auto started = std::chrono::steady_clock::now();
  Client client;
  ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;
  for (std::size_t i = 1; i <= kRequests; ++i) {
    ASSERT_TRUE(client.write_all(request_text(i))) << "第 " << i << " 条写不出去";
    std::string status;
    std::string body;
    ASSERT_TRUE(client.read_response(&status, &body, &why)) << "第 " << i << " 条：" << why;
    ASSERT_EQ(body, expected_body(i)) << "第 " << i << " 条响应串号了：响应被安到了别的请求上";
  }
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - started)
                      .count();

  // 收尾从半关闭开始：我只发 FIN，然后看网关把这侧连接关掉（read() 返回 0）——
  // 这是内核给的第一手证据。
  client.shutdown_write();
  ASSERT_TRUE(client.wait_for_eof(&why))
      << "客户端只发了 FIN，网关没有把这侧连接关掉：" << why;
  ReactorStats& stats = harness.reactor()->stats();
  // **S2-02（ADR-15）改了这条用例的期望，改的是行为不是 bug**：下游关闭后，它配对的
  // 上游不再级联销毁，而是**归还进池**、活得比下游久（等下一条下游来借）。所以客户端
  // 关闭这一刻 live==1（池里那条热上游），不再是 0。先确认回收发生（pool_returns==1），
  // 再 stop_loop 排空池——排空之后才有 live==0 / constructed==destructed。
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.pool_returns) == 1u; }, 5000))
      << "下游关闭后上游没有归还进池（ADR-15 的回收没发生）";
  // 这里必须**等**，不能"看到 pool_returns 就立刻读 live"。`live_connections` 是对象
  // 计数的实时仪表，而下游对象进 `retiring_` 之后要到**事件批次末**的 `retire_closed()`
  // 才真正析构（六步关闭的第 5 步）——所以 `pool_returns==1` 成立的这一刻，"下游对象
  // 还没析构"是一个合法中间态，读它会得到 2。串行跑几乎每次都能侥幸读到 1，
  // `ctest -j 8` 下必红（S3-01 那次并行复跑第一次把它撞出来，原样记进 ARCHITECTURE §5）。
  // 期望值一个字没改（池里正好 1 条热上游），改的是取样方式：非稳态量只能等它进稳态。
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.live_connections) == 1u; }, 2000))
      << "回收后池里应当正好留着那 1 条热上游（等到 retiring_ 兑现之后仍然是别的数，才是真问题）";
  EXPECT_EQ(stats.load(stats.pool_borrows), 0u) << "单条下游全程独占它的上游，从没借过池";

  harness.stop_loop();  // 排空池 + join；之后才允许读非原子的 epoll 计数
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.live_connections) == 0u; }, 1000))
      << "stop 排空池后仍有连接对象没析构：关闭路径漏了某一步，或两个对象互相抱着";

  // 所有计数在这一行之后就被复制成局部变量：后面的 shutdown_all() 会销毁 Reactor，
  // `stats` 这个引用届时悬空，拿它做断言就是读到什么算什么。
  const std::uint64_t parsed = stats.load(stats.requests_parsed);
  const std::uint64_t accepted = stats.load(stats.downstream_accepted);
  const std::uint64_t opened = stats.load(stats.upstream_opened);
  const std::uint64_t constructed = stats.load(stats.connections_constructed);
  const std::uint64_t destructed = stats.load(stats.connections_destructed);
  const std::uint64_t live = stats.load(stats.live_connections);
  const std::uint64_t closed = stats.load(stats.connections_closed);
  const std::uint64_t served = harness.upstream().requests_served();

  EXPECT_EQ(parsed, static_cast<std::uint64_t>(kRequests));
  EXPECT_EQ(served, static_cast<std::uint64_t>(kRequests)) << "上游实际处理的条数也要是 10000";
  EXPECT_EQ(accepted, 1u) << "全程一条下游连接";
  EXPECT_EQ(opened, 1u) << "全程一条回源连接，没有每请求新建（池空时才建了这一次）";
  EXPECT_EQ(constructed, 2u) << "下游 1 + 上游 1";
  EXPECT_EQ(destructed, 2u) << "两个都析构了：上游在 stop 排空池时析构，shared_ptr 引用计数归零";
  EXPECT_EQ(live, 0u);
  EXPECT_EQ(closed, 2u);
  const std::uint64_t registrations = harness.reactor()->epoll_registrations();
  const std::uint64_t unregistrations = harness.reactor()->epoll_unregistrations();
  const std::uint64_t modifications = harness.reactor()->epoll_modifications();

  // 「注册/注销次数与连接建立/关闭次数逐项相等」。数字来自 Epoll 的计数器
  // （只在成功时 ++），不是我另写一遍的 ++，所以相等不是"我写对称了"的错觉。
  // 监听 fd 和 eventfd 各注册一次、由 ~Reactor 注销，所以注册侧要 +2。
  EXPECT_EQ(registrations, 2u + accepted + opened);
  EXPECT_EQ(unregistrations, closed)
      << "注销次数少于关闭次数 = 有 fd 还挂在 interest list 上";

  // fd 复原的口径要"和我开始之前一模一样"，所以等式右边不留任何加减项：
  // 客户端那个 socket 也算一个 fd，必须先关；Reactor 的三个常驻 fd 和测试上游
  // 的监听 fd 由 shutdown_all() 一起收掉。只关一半的话，右边就得写成
  // "减去监听 fd、再加上我可能忘关的那个"，那种算式正是迟早会算错的东西。
  client.close();
  harness.shutdown_all();
  const std::size_t fd_after = count_open_fds();
  std::printf("[S1-04 10000] 串行往返=%zu 耗时=%lldms fd: 起=%zu 终=%zu "
              "epoll 注册=%llu 注销=%llu 修改=%llu\n",
              kRequests, static_cast<long long>(ms), fd_before, fd_after,
              static_cast<unsigned long long>(registrations),
              static_cast<unsigned long long>(unregistrations),
              static_cast<unsigned long long>(modifications));
  EXPECT_EQ(fd_after, fd_before) << "10000 请求之后 fd 没回到基线：有连接没被真正关掉";
}

TEST(S1_04_Reactor, MalformedRequestClosesConnectionWithoutLeak) {
  const std::size_t fd_before = count_open_fds();
  Harness harness;
  std::string why;
  ASSERT_TRUE(harness.start(default_options(), &why)) << why;

  Client client;
  ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;
  ASSERT_TRUE(client.write_all("NOT HTTP AT ALL\r\n\r\n"));

  // S1-04 当年这条只断"关连接"；S1-06 把 reject 映射成 4xx 之后，这里必须
  // **先读到那条 400 再看到 EOF**。我特意改写而不是留着让它继续绿：留着的写法
  // 是 `wait_for_eof`，而它遇到"先有字节、后是 FIN"会照样返回 true ——
  // 一条绿着却什么都不证明的用例，比一条红的更糟。
  std::string status;
  std::string body;
  ASSERT_TRUE(client.read_response(&status, &body, &why))
      << "畸形请求没有换来可定义的 4xx：" << why;
  EXPECT_EQ(status, "HTTP/1.1 400 Bad Request");
  ASSERT_TRUE(client.wait_for_eof(&why)) << "400 之后连接必须收尾（自产响应带 close）：" << why;

  ReactorStats& stats = harness.reactor()->stats();
  EXPECT_EQ(stats.load(stats.requests_rejected), 1u);
  EXPECT_EQ(stats.load(stats.requests_parsed), 0u);
  EXPECT_EQ(stats.load(stats.upstream_opened), 0u) << "畸形请求不该换来一条回源连接";
  EXPECT_EQ(stats.load(stats.gateway_replies_sent), 1u);
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.live_connections) == 0u; }, 5000));
  EXPECT_EQ(stats.load(stats.connections_constructed), 1u) << "只建了下游对象";
  EXPECT_EQ(stats.load(stats.connections_destructed), 1u);

  client.close();
  harness.shutdown_all();
  EXPECT_EQ(count_open_fds(), fd_before) << "畸形请求那条连接之外还有 fd 没释放";
}

TEST(S1_04_Reactor, UpstreamBreakClosesBothSidesAfterFlushingResponse) {
  // 上游"回完第一条就断"。这条测的是两件事：
  //   一、上游结束会带着关掉下游（S1-04 没有连接池，S2 建池后这里改成只关上游）；
  //   二、响应已经在下游的出向缓冲里了，**关闭必须等它刷完**。
  // 第二条是我在写这条用例时发现的问题：原来的关连接直接 close(fd)，
  // 于是"上游回完就断"这个场景一定会把刚泵过来的响应一起吃掉。
  Harness harness;
  std::string why;
  ASSERT_TRUE(harness.start(default_options(), &why)) << why;
  harness.upstream().set_close_after_first_response(true);

  Client client;
  ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;
  ASSERT_TRUE(client.write_all(request_text(1)));

  std::string status;
  std::string body;
  ASSERT_TRUE(client.read_response(&status, &body, &why))
      << "上游关闭把还在出向缓冲里的响应一起关掉了：" << why;
  EXPECT_EQ(body, expected_body(1));

  ASSERT_TRUE(client.wait_for_eof(&why)) << "上游断了之后下游没跟着结束：" << why;

  ReactorStats& stats = harness.reactor()->stats();
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.live_connections) == 0u; }, 5000));
  EXPECT_EQ(stats.load(stats.downstream_accepted), 1u);
  EXPECT_EQ(stats.load(stats.upstream_opened), 1u);
  EXPECT_EQ(stats.load(stats.connections_closed), 2u) << "两侧都关：不能只关上游留下下游";
  EXPECT_EQ(stats.load(stats.connections_constructed), 2u);
  EXPECT_EQ(stats.load(stats.connections_destructed), 2u);

  harness.stop_loop();  // join 之后才允许读注册表尺寸
  EXPECT_EQ(harness.reactor()->live_connection_count(), 0u) << "注册表里一条连接都不该留";
}

TEST(S1_04_Reactor, OverCapacityRejectionDoesNotStallTheListener) {
  // max_connections=1 时多出来的连接被直接关掉。这条用例真正盯的是**之后**：
  // 拒绝时我必须继续把 accept 队列排空（accept_all 里那个 continue 而不是 return），
  // 否则边缘触发不会再投递监听事件，后面所有新连接都只滞留在 backlog 里 ——
  // 客户端 connect() 照样成功（内核替它握完了手），但网关永远不 accept。
  Harness harness;
  std::string why;
  Harness::Options options;
  options.config = base_config();
  options.config.max_connections = 1;
  ASSERT_TRUE(harness.start(options, &why)) << why;

  Client first;
  ASSERT_TRUE(first.connect_to(harness.port(), &why)) << why;
  ReactorStats& stats = harness.reactor()->stats();
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.downstream_accepted) == 1u; }, 5000))
      << "第一条连接没被 accept";

  Client extra;
  ASSERT_TRUE(extra.connect_to(harness.port(), &why)) << why;
  ASSERT_TRUE(extra.wait_for_eof(&why)) << "超出预算的连接没有被关掉：" << why;
  EXPECT_EQ(stats.load(stats.downstream_rejected_over_capacity), 1u);
  EXPECT_EQ(stats.load(stats.downstream_accepted), 1u) << "被拒的那条不该进 accepted";

  first.shutdown_write();
  ASSERT_TRUE(first.wait_for_eof(&why)) << "第一条连接收尾时网关没把它关掉：" << why;
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.live_connections) == 0u; }, 5000));

  Client after;
  ASSERT_TRUE(after.connect_to(harness.port(), &why)) << why;
  const bool accepted =
      wait_until([&] { return stats.load(stats.downstream_accepted) == 2u; }, 3000);
  // 失败信息把病因说出来：这一条挂掉时，症状是"过一段时间新连接进不来了"，
  // 而不是报错 —— 典型的边缘触发滞留。
  EXPECT_TRUE(accepted) << "拒绝超额连接之后监听事件不再投递：accept 队列没被排空（ET 滞留）";
  EXPECT_EQ(stats.load(stats.downstream_accepted), 2u);

  // 超额被拒的那条**不进**构造计数：accept 出来的 fd 由 AcceptOutcome 出作用域时
  // 直接关掉，连接对象根本没建 —— 这正是 downstream_rejected_over_capacity 必须
  // 和 connections_closed 分开记的原因，混在一起这笔账就无从对起了。
  //
  // 顺序不能换：这里必须先关连接、再停循环。反过来做的话循环线程已经退出，
  // 关闭事件没人处理，"存活数归零"这条断言等到的就是它自己 5 秒的超时 ——
  // 我第一次就把 stop_loop() 写在了 close() 前面，失败信息看起来像"对象没析构"，
  // 其实是"没人在看了"。
  after.shutdown_write();
  ASSERT_TRUE(after.wait_for_eof(&why)) << "最后一条连接没被关闭端的 EOF：" << why;
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.live_connections) == 0u; }, 5000))
      << "被拒或已关闭的连接留下了没析构的对象";
  EXPECT_EQ(stats.load(stats.connections_constructed), 2u) << "first + after，被拒的那条没建对象";
  EXPECT_EQ(stats.load(stats.connections_destructed), stats.load(stats.connections_constructed));
  EXPECT_EQ(stats.load(stats.connections_closed), 2u);
  harness.stop_loop();
}

TEST(S1_04_Reactor, IdleConnectionDoesNotSpinOnEpollOut) {
  Harness harness;
  std::string why;
  ASSERT_TRUE(harness.start(default_options(), &why)) << why;

  Client client;
  ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;
  for (std::size_t i = 1; i <= 3; ++i) {
    ASSERT_TRUE(client.write_all(request_text(i)));
    std::string status;
    std::string body;
    ASSERT_TRUE(client.read_response(&status, &body, &why)) << why;
  }

  // 连接保持空闲 400ms。三条请求正常也就二十来个事件批次；如果刷完之后 EPOLLOUT
  // 没被摘掉，发送缓冲"随时可写"会让 epoll_wait 每次立刻返回，400ms 能攒出几万个批次。
  // 这就是「空闲 CPU < 5%」那条 SLO 最容易先撞上的实现缺陷。
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  client.shutdown_write();
  ASSERT_TRUE(client.wait_for_eof(&why)) << why;
  ReactorStats& stats = harness.reactor()->stats();
  // S2-02（ADR-15）：下游关闭后上游归还进池、live==1（不再是 0）。这条用例真正要断的
  // 是下面那句"空闲期不自旋"，生命周期只是收尾核对——确认回收发生后 stop_loop 排空池。
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.pool_returns) == 1u; }, 5000));
  harness.stop_loop();

  std::printf("[S1-04 idle] 3 请求 + 400ms 空闲：事件批次=%llu（wait_timeout_ms=50，"
              "自旋的话会是几万这个量级）\n",
              static_cast<unsigned long long>(harness.batches()));
  EXPECT_LT(harness.batches(), 100u) << "空闲期还在批量收事件：EPOLLOUT 刷完没摘掉，循环在自旋";
}

TEST(S1_04_Reactor, MemoryPerIdleConnectionIsMeasured) {
  // 卡点四那笔账的数据来源。口径写清楚：这里量的是**只连接、不发请求**的下游连接，
  // 一条连接 = 一个连接对象。真实场景里一条客户端连接对应**两个**连接对象
  // （下游 + 上游），所以 SLO #2 的分母到底取哪个仍然归我决策，这条用例只给数。
  // 它不是 SLO #2 的达标验证（那要 100k 长连接 + WebSocket 帧，未测），
  // 只是一道早期指示性检查：别让每连接内存在还没开始优化时就已经超预算。
  constexpr int kIdleConnections = 128;

  // fd 基线取在 Harness 起来**之前**，RSS 基线取在之后：两者口径不同是有意的。
  // fd 要的是"进程回到原样"，所以起点必须什么都没有；RSS 要的是"每条空闲连接多花
  // 多少"，把 Reactor 自己的 events_ 数组算进第一笔连接里会让这个数偏高。
  const std::size_t fd_before = count_open_fds();
  Harness harness;
  std::string why;
  Harness::Options options;
  options.config = base_config();
  options.config.max_connections = 1024;
  options.with_upstream = false;  // 不发请求，不需要上游
  ASSERT_TRUE(harness.start(options, &why)) << why;

  // 常驻的那几个 fd 先被显式算出来：监听 socket、epoll 实例、eventfd。
  // 少算或多算这里就先红，而不是留到"fd 回到基线"那条上以一个看不出来的差值出现。
  const std::size_t fd_with_reactor = count_open_fds();
  EXPECT_EQ(fd_with_reactor, fd_before + kReactorPersistentFds)
      << "Reactor 起来之后占的 fd 数不是注释里说的那 3 个";
  const std::uint64_t rss_before = current_rss_kib();

  std::vector<Client> clients(kIdleConnections);
  int connected = 0;
  for (auto& client : clients) {
    if (!client.connect_to(harness.port(), &why)) break;
    ++connected;
  }
  ASSERT_EQ(connected, kIdleConnections) << why;

  ReactorStats& stats = harness.reactor()->stats();
  ASSERT_TRUE(wait_until(
      [&] {
        return stats.load(stats.downstream_accepted) == static_cast<std::uint64_t>(kIdleConnections);
      },
      8000))
      << "只 accept 到 " << stats.load(stats.downstream_accepted);

  // 惰性的兑现：一条请求都没解析，所以 scratch 一个都没建；一个字都没回，
  // 所以出向 Buffer 也一个都没分配。每连接内存只剩"连接对象 + 入向 buffer"。
  EXPECT_EQ(stats.load(stats.scratches_created), 0u);
  EXPECT_EQ(stats.load(stats.outbound_buffers_created), 0u);

  // fd 这一侧做同一笔对照。注意 /proc/self/fd 看到的是**整个进程**，而这条测试
  // 把客户端和服务端开在同一个进程里，所以 128 条连接对应 256 个 fd：一半是我
  // 自己 connect() 出来的 socket，一半是网关 accept 出来的。等式写成两边之和，
  // 而不是只算网关那一半 —— 我第一次就漏了客户端那 128 个，于是"多开了东西"
  // 这个报错指向了一个根本不存在的泄漏。
  EXPECT_EQ(count_open_fds(),
            fd_with_reactor + 2u * static_cast<std::size_t>(kIdleConnections))
      << "空闲连接的 fd 数和「网关侧 + 客户端侧」对不上：accept 路径多开了东西";

  const std::uint64_t rss_after = current_rss_kib();
  const std::uint64_t delta_kib = rss_after > rss_before ? rss_after - rss_before : 0;
  const double per_conn_kib = static_cast<double>(delta_kib) / static_cast<double>(connected);
  std::printf("[S1-04 内存] 空闲下游连接=%zu RSS %llu→%llu KiB 增量=%llu 每连接=%.2f KiB "
              "fd(Reactor 已起)=%zu 峰值=%zu（sizeof(Connection)=%zu，入向 buffer 初始容量=4096）\n",
              static_cast<std::size_t>(connected),
              static_cast<unsigned long long>(rss_before),
              static_cast<unsigned long long>(rss_after),
              static_cast<unsigned long long>(delta_kib), per_conn_kib, fd_with_reactor,
              count_open_fds(), sizeof(Connection));

  // 内存预算这条只在**没有插桩**的构建里判。tsan 实测把每连接 RSS 抬到 20.81–22.69 KiB
  // （asan-ubsan 11.31–16.28 KiB，没超但同样是抬高的），那是 sanitizer 的分配器元数据和
  // shadow memory，不是我的 Connection 变大。把这条断言留在 sanitizer 构建里有两个坏处：
  // 一是它红的时候我第一反应会是"连接对象胖了"而去看错的地方；二是它逼我把阈值
  // 调到能容纳 sanitizer 的开销——而那个阈值就不再是 SLO #2 的 20 KiB 了。
  // 数字照样打印，sanitizer 下的 RSS 增量对"有没有异常增长"仍有对照价值。
  if (k_rss_is_instrumented) {
    std::printf("[S1-04 内存] 当前是 sanitizer 构建，每连接 RSS 含插桩开销，"
                "20 KiB 预算这条不在这里判（判定归 debug/release）\n");
  } else {
    EXPECT_LT(per_conn_kib, 20.0) << "每连接内存已超 SLO #2 的 20 KiB 预算，后面的优化没有起点";
  }

  // 先全体半关闭、再逐个等 EOF：反过来写（关一个等一个）会让这 128 次往返排成
  // 一串，而网关是一批事件里同时看到这 128 个 FIN 的 —— 我要观测的是它那一批
  // 有没有把每条都收尾干净，不是我一个一个问它答一次的耐心。
  for (auto& client : clients) client.shutdown_write();
  for (auto& client : clients) {
    ASSERT_TRUE(client.wait_for_eof(&why)) << "有一条空闲连接没被网关关掉：" << why;
  }
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.live_connections) == 0u; }, 8000));
  EXPECT_EQ(stats.load(stats.connections_destructed), stats.load(stats.connections_constructed));

  for (auto& client : clients) client.close();
  harness.shutdown_all();
  EXPECT_EQ(count_open_fds(), fd_before)
      << "循环和上游都关掉之后 fd 没回到基线：连接对象或 socket 泄漏了";
}

// --- S1-05：keep-alive 复用与 Upgrade 透传 --------------------------------
// 验收句逐条对应：
//   「同一 curl 连接发 3 个顺序请求，全部成功且各自响应内容正确对应」
//     → ThreeSequentialRequestsGetTheirOwnResponsesInOrder
//   「请求间不重建下游连接（fd 计数器可断言）」
//     → 同一条用例里的 accepted/opened/registrations/fd 四组断言
//   「对 keep-alive 与 close 分别按语义处理」
//     → CloseRequestIsHonoredEndToEnd
//   「带 Upgrade: websocket 的请求在 101 之后走纯透传，帧内容不被解析、不被改写」
//     → UpgradeRequestSwitchesToRawTunnelAfterThe101（字节比对，不是语义比对）
//     UpgradeHeaderWithoutConnectionTokenIsNotATunnel（双条件判定的 negative 面）
//     TunnelSurvivesSplitResponseHead（半包的 101 不许提前点燃隧道）
//   交接段点名的"意图是一次性的"：
//     → 在连接层直测（connection_test.cc 的 UpgradeIntentSurvivesRequestAndIsTakenOnce）——
//       本文件的用例都跑在 TestUpstream 这个**宽松对端**上：它见 Upgrade 头就回 101
//       并进 echo 模式，同一条连接上追加第二条普通请求会被它当隧道字节原样弹回，
//       在 reactor 层断"第二条拿回自己的 200"测的是对端的脾气不是网关的意图语义。
//       take-once 的时序责任在 Connection 的三个接口上，就该在那三个接口上钉死。

TEST(S1_05_Reactor, ThreeSequentialRequestsGetTheirOwnResponsesInOrder) {
  const std::size_t fd_before = count_open_fds();
  Harness harness;
  std::string why;
  ASSERT_TRUE(harness.start(default_options(), &why)) << why;

  Client client;
  ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;
  for (std::size_t i = 1; i <= 3; ++i) {
    ASSERT_TRUE(client.write_all(request_text(i)));
    std::string status;
    std::string body;
    ASSERT_TRUE(client.read_response(&status, &body, &why)) << "第 " << i << " 条：" << why;
    EXPECT_EQ(status, "HTTP/1.1 200 OK");
    EXPECT_EQ(body, expected_body(i)) << "第 " << i << " 条的响应串号了";
  }

  ReactorStats& stats = harness.reactor()->stats();
  EXPECT_EQ(stats.load(stats.requests_parsed), 3u);
  EXPECT_EQ(stats.load(stats.downstream_accepted), 1u) << "三条请求共用一条下游连接";
  EXPECT_EQ(stats.load(stats.upstream_opened), 1u) << "回源连接没有随请求重建";
  std::size_t request_bytes = 0;
  for (std::size_t i = 1; i <= 3; ++i) request_bytes += request_text(i).size();
  EXPECT_EQ(stats.load(stats.bytes_down_to_upstream), static_cast<std::uint64_t>(request_bytes));
  EXPECT_EQ(harness.upstream().requests_served(), 3u);

  harness.stop_loop();
  EXPECT_EQ(harness.reactor()->epoll_registrations(), 4u)
      << "监听 + eventfd + 1 下游 + 1 上游；多一次注册就是请求之间重建了连接";
  client.close();
  harness.shutdown_all();
  EXPECT_EQ(count_open_fds(), fd_before) << "三条请求跑完 fd 没回到基线";
}

TEST(S1_05_Reactor, CloseRequestIsHonoredEndToEnd) {
  Harness harness;
  std::string why;
  ASSERT_TRUE(harness.start(default_options(), &why)) << why;

  Client client;
  ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;
  ASSERT_TRUE(client.write_all(close_request_text(1)));
  std::string status;
  std::string body;
  ASSERT_TRUE(client.read_response(&status, &body, &why)) << why;
  EXPECT_EQ(status, "HTTP/1.1 200 OK");
  EXPECT_EQ(body, expected_body(1));

  ReactorStats& stats = harness.reactor()->stats();
  EXPECT_EQ(stats.load(stats.close_headers_honored), 1u) << "close 意图要被兑现，兑现要留计数";

  // 上游按 close 语义收尾之后，EOF 级联必须把客户端这侧也关掉（内核级证据：
  // read() 返回 0）。先等到这一行，才允许去碰收尾计数 —— 观测纪律那条。
  ASSERT_TRUE(client.wait_for_eof(&why)) << "close 的响应送达后连接没有收尾：" << why;
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.live_connections) == 0u; }, 5000))
      << "close 兑现之后还有连接对象活着";
  EXPECT_EQ(stats.load(stats.upstream_opened), 1u) << "一次性配对不等于新建配对";
  EXPECT_EQ(stats.load(stats.requests_parsed), 1u);
  EXPECT_EQ(stats.load(stats.upgrades_101), 0u) << "close 不是握手，不许蹭隧道的计数";
}

TEST(S1_05_Reactor, UpgradeRequestSwitchesToRawTunnelAfterThe101) {
  Harness harness;
  std::string why;
  ASSERT_TRUE(harness.start(default_options(), &why)) << why;

  Client client;
  ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;
  const std::string req = upgrade_request_text();
  ASSERT_TRUE(client.write_all(req));

  std::string head;
  ASSERT_TRUE(client.read_head(&head, &why)) << "101 没有回来：" << why;
  EXPECT_EQ(head.find("HTTP/1.1 101"), 0u) << "回来的不是 101: " << head;
  EXPECT_NE(head.find("Upgrade: websocket"), std::string::npos);

  ReactorStats& stats = harness.reactor()->stats();
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.upgrades_101) == 1u; }, 3000))
      << "101 已经送达客户端，但隧道计数还没落——进入动作与字节交付的时序串了";
  EXPECT_EQ(stats.load(stats.upstream_opened), 1u);
  EXPECT_EQ(stats.load(stats.bytes_down_to_upstream), static_cast<std::uint64_t>(req.size()))
      << "只有握手请求走过 HTTP 路径，帧字节一律记在隧道账上";

  // 双向回显闭环。两条刻意"不像合法 HTTP"的帧：
  //   帧一 0x81 0x83 0x37 0xfa 0x76 —— 开头就是 CRLF 家族的裸 0x0D 形态，
  //          被当请求解析必然 reject（畸形 → 关连接），被透传则毫发无损；
  //   帧二 "\r\n\r\nGARBAGE" —— 头部区终止串打头，后面跟"永远读不齐的半截头"。
  // 断言全部是**字节比对**：验收句是"不被解析、不被改写"，那它就只能用字节证明，
  // 拿语义断言（比如"状态码是 101"）去证它，等于用我的理解印证我的理解。
  const std::string frame1("\x81\x83\x37\xfa\x76", 5);
  const std::string frame2("\r\n\r\nGARBAGE", 9);
  ASSERT_TRUE(client.write_all(frame1));
  std::string echoed1;
  ASSERT_TRUE(client.read_exact(&echoed1, frame1.size(), &why))
      << "帧一没有原样回显（一块进一块出）：" << why;
  EXPECT_EQ(echoed1, frame1);

  ASSERT_TRUE(client.write_all(frame2));
  std::string echoed2;
  ASSERT_TRUE(client.read_exact(&echoed2, frame2.size(), &why))
      << "帧二（终止串打头的二进制）没有原样回显：" << why;
  EXPECT_EQ(echoed2, frame2);

  EXPECT_EQ(stats.load(stats.upgrades_101), 1u) << "帧的往返不许再点燃第二条隧道";
  EXPECT_GE(stats.load(stats.bytes_tunnel_down),
            static_cast<std::uint64_t>(frame1.size() + frame2.size()));
  EXPECT_GE(stats.load(stats.bytes_tunnel_up),
            static_cast<std::uint64_t>(frame1.size() + frame2.size()));
  // requests_parsed 停在 1 是"不被改写"的另一半：改写的前提是我读懂过它。
  EXPECT_EQ(stats.load(stats.requests_parsed), 1u);

  // 隧道关（客户端 FIN）级联两侧收尾，fd 回基线 —— 与 S1-04 同一条纪律。
  client.shutdown_write();
  ASSERT_TRUE(client.wait_for_eof(&why)) << "隧道没有随客户端半关闭收尾：" << why;
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.live_connections) == 0u; }, 5000));
  const std::uint64_t parsed = stats.load(stats.requests_parsed);
  const std::uint64_t constructed = stats.load(stats.connections_constructed);
  const std::uint64_t destructed = stats.load(stats.connections_destructed);
  harness.stop_loop();
  EXPECT_EQ(constructed, destructed) << "隧道两端有对象抱着不死（帧状态不该延长生命周期）";
  EXPECT_EQ(parsed, 1u);
  client.close();
  harness.shutdown_all();
}

TEST(S1_05_Reactor, UpgradeHeaderWithoutConnectionTokenIsNotATunnel) {
  Harness harness;
  std::string why;
  ASSERT_TRUE(harness.start(default_options(), &why)) << why;

  // 只有 Upgrade 头、Connection 里没有 upgrade 记号 —— 网关的判定是双条件
  // （见 reactor.cc 的 is_upgrade_request），这条不配进隧道。测试上游是故意
  // 宽松的（看到 Upgrade: 就回 101），于是 101 会作为**普通响应字节**漏下来：
  // 这正是"宽松对端 + 严格网关"要测的组合 —— 网关若单条件就点燃隧道，这里
  // 会多出一个 upgrades_101 并且后续帧被透传，断言当场红。
  Client client;
  ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;
  const std::string req =
      "GET /ws HTTP/1.1\r\nHost: gateway.test\r\nConnection: keep-alive\r\n"
      "Upgrade: websocket\r\n\r\n";
  ASSERT_TRUE(client.write_all(req));

  std::string status;
  std::string body;
  ASSERT_TRUE(client.read_response(&status, &body, &why)) << why;
  EXPECT_EQ(status, "HTTP/1.1 101 Switching Protocols")
      << "网关不认隧道，就必须把对端的 101 当普通字节逐样交给客户端（不改写）";

  ReactorStats& stats = harness.reactor()->stats();
  EXPECT_EQ(stats.load(stats.upgrades_101), 0u);
  EXPECT_EQ(stats.load(stats.bytes_tunnel_up), 0u);
  EXPECT_EQ(stats.load(stats.bytes_tunnel_down), 0u);
  EXPECT_EQ(stats.load(stats.requests_parsed), 1u);
  EXPECT_EQ(stats.load(stats.bytes_down_to_upstream), static_cast<std::uint64_t>(req.size()));
}

TEST(S1_05_Reactor, TunnelSurvivesSplitResponseHead) {
  Harness harness;
  std::string why;
  ASSERT_TRUE(harness.start(default_options(), &why)) << why;

  Client client;
  ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;
  const std::string req = upgrade_request_text();
  ASSERT_TRUE(client.write_all(req));

  std::string head;
  ASSERT_TRUE(client.read_head(&head, &why)) << why;
  EXPECT_EQ(head.find("HTTP/1.1 101"), 0u);
  ReactorStats& stats = harness.reactor()->stats();
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.upgrades_101) == 1u; }, 3000)) << why;

  // 上游的响应头在这里已经整块送达（TestUpstream 一次性 send）。把它的**字节数**
  // 再分成三段喂给隧道之后的帧流：证明隧道对"一条消息被 TCP 切开"免疫——
  // 半包在隧道里根本不是概念，因为这里没有"包"可齐。
  std::string split;
  split.append("\x00\x01\x02", 3);
  split.append("tunnel-half");
  split.append("\x0D\x0A\x0D\x0A", 4);
  ASSERT_TRUE(client.write_all(split.substr(0, 3)));
  std::string got;
  ASSERT_TRUE(client.read_exact(&got, 3, &why)) << "隧道第一段没有回显：" << why;
  ASSERT_TRUE(client.write_all(split.substr(3)));
  ASSERT_TRUE(client.read_exact(&got, split.size(), &why))
      << "被切开的第二段没有拼回原样：" << why;
  EXPECT_EQ(got, split) << "透传改写了字节";

  EXPECT_EQ(stats.load(stats.upgrades_101), 1u) << "分段到达的帧不是新握手";
  EXPECT_EQ(stats.load(stats.requests_parsed), 1u);
  EXPECT_EQ(stats.load(stats.parse_calls), 1u)
      << "隧道之后的字节连一次解析尝试都不许发生（release 后也不许重建 scratch 再解析）";
}

// --- S1-06：响应语义面（逐字节一致 / 截断 / reject 映射 / close 边界） --------
// 验收句逐条对应：
//   「curl -v 经网关与直连上游：状态码、Content-Length、body 逐字节一致」
//     → ProxiedResponseIsByteIdenticalToDirectUpstream（同一上游、同一请求字节，
//       两条连接各取一次全响应，比对到**字节**而不是比对到"解析结果"）
//   「上游返回 chunked 时，转发结果与直连一致」
//     → ChunkedResponseIsByteIdenticalAndLeavesTheNextResponseAlone
//       （顺带证明框架游标停在边界上：chunked 之后的下一条响应还能被完整取回）
//   「上游中途断开时，网关返回可定义的 5xx，且不残留半包」
//     → UpstreamDyingBeforeAnyResponseGetsDefined502
//     → UpstreamDyingMidBodyIsTruncatedByEofWithoutASecondReply（分工见下）
//     → EndlessResponseHeadIsKilledAtTheBudget
//   「第一份 p99」→ 不在这里（wrk 跑在真实进程上，脚本取数后记进 TASKS.md）
//
// 截断的两条分支必须**分别**有用例，因为它们的表态方式根本不同：
// 最终头没出去时网关还有资格自产 502；头已经出去了就只能以 EOF 表态——
// 那时候再回一条 5xx 等于给同一个请求两条响应，那才是真正的"残留"。

namespace {

// 固定体的两种期望响应模板。写在这里而不是从上游的构造函数抄：用例必须
// 能独立说出"我期望的字节长什么样"，否则上游改了模板我用例跟着改，
// 一致就只是同一份代码的自证。
std::string expected_cl_response(std::string_view body) {
  return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
         "\r\nConnection: keep-alive\r\n\r\n" + std::string(body);
}

std::string fixed_body_text() { return "0123456789"; }

std::string fixed_request_text(std::size_t i) {
  return "GET /r" + std::to_string(i) + " HTTP/1.1\r\nHost: gateway.test\r\n\r\n";
}

}  // namespace

TEST(S1_06_Reactor, ProxiedResponseIsByteIdenticalToDirectUpstream) {
  Harness harness;
  std::string why;
  ASSERT_TRUE(harness.start(default_options(), &why)) << why;
  const std::string body = fixed_body_text();
  harness.upstream().set_fixed_response_body(body);
  const std::string expected = expected_cl_response(body);

  // 基线：直连上游，发**同一串请求字节**。
  Client direct;
  ASSERT_TRUE(direct.connect_to(harness.upstream().port(), &why)) << why;
  ASSERT_TRUE(direct.write_all(fixed_request_text(1)));
  std::string direct_bytes;
  ASSERT_TRUE(direct.read_exact(&direct_bytes, expected.size(), &why))
      << "直连基线就没取满一条完整响应：" << why;
  EXPECT_EQ(direct_bytes, expected) << "期望模板和对端的实际输出不一致：用例先失效了";

  // 经网关。
  Client via;
  ASSERT_TRUE(via.connect_to(harness.port(), &why)) << why;
  ASSERT_TRUE(via.write_all(fixed_request_text(1)));
  std::string via_bytes;
  ASSERT_TRUE(via.read_exact(&via_bytes, expected.size(), &why))
      << "经网关没有取满同样长度的响应：" << why;

  EXPECT_EQ(via_bytes, direct_bytes) << "验收句原文：与直连上游逐字节一致";
  EXPECT_EQ(via_bytes, expected);

  ReactorStats& stats = harness.reactor()->stats();
  EXPECT_EQ(stats.load(stats.responses_completed), 1u) << "框架判定要给这条响应落下完结点";
  EXPECT_EQ(stats.load(stats.gateway_replies_sent), 0u) << "正常转发不许掺自产响应";
  EXPECT_EQ(stats.load(stats.bytes_up_to_downstream),
            static_cast<std::uint64_t>(expected.size()));

  direct.close();
  via.close();
}

TEST(S1_06_Reactor, ChunkedResponseIsByteIdenticalAndLeavesTheNextResponseAlone) {
  Harness harness;
  std::string why;
  ASSERT_TRUE(harness.start(default_options(), &why)) << why;
  const std::string body = fixed_body_text();
  harness.upstream().set_fixed_response_body(body);

  // 上游的 chunked 形态：10 字节的体切成 5 + 5，last-chunk 后跟空行。
  const std::string expected =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: keep-alive\r\n\r\n"
      "5\r\n01234\r\n5\r\n56789\r\n0\r\n\r\n";
  const std::string chunked_request =
      "GET /chunked HTTP/1.1\r\nHost: gateway.test\r\n\r\n";

  Client direct;
  ASSERT_TRUE(direct.connect_to(harness.upstream().port(), &why)) << why;
  ASSERT_TRUE(direct.write_all(chunked_request));
  std::string direct_bytes;
  ASSERT_TRUE(direct.read_exact(&direct_bytes, expected.size(), &why)) << why;
  EXPECT_EQ(direct_bytes, expected);

  Client via;
  ASSERT_TRUE(via.connect_to(harness.port(), &why)) << why;
  ASSERT_TRUE(via.write_all(chunked_request));
  std::string via_bytes;
  ASSERT_TRUE(via.read_exact(&via_bytes, expected.size(), &why))
      << "chunked 没有逐字节透传：" << why;
  EXPECT_EQ(via_bytes, direct_bytes) << "上游回 chunked 时转发结果必须与直连一致";

  // 关键的第二半：chunked 的**终止空行之后**还有字节可读的话，那些字节属于
  // 下一条响应。游标若多走一格，下一条响应的头就会被喂进框架判定并被吃掉，
  // 表现是第二个请求挂住（不是报错）。这里用同一条连接再要一次来钉住它。
  ReactorStats& stats = harness.reactor()->stats();
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.responses_completed) == 1u; }, 3000))
      << "chunked 流的边界没有被认成完结点";
  const std::uint64_t completed_at_boundary = stats.load(stats.responses_completed);

  ASSERT_TRUE(via.write_all(fixed_request_text(2)));
  std::string status;
  std::string second_body;
  ASSERT_TRUE(via.read_response(&status, &second_body, &why))
      << "chunked 之后的第二条请求没有换来响应：框架游标越界吃掉了下一条响应的头";
  EXPECT_EQ(status, "HTTP/1.1 200 OK");
  EXPECT_EQ(second_body, body);
  EXPECT_EQ(stats.load(stats.responses_completed), completed_at_boundary + 1u);
  EXPECT_EQ(stats.load(stats.downstream_accepted), 1u) << "全程一条下游连接";
  EXPECT_EQ(stats.load(stats.upstream_opened), 1u) << "chunked 之后复用同一条回源连接";

  direct.close();
  via.close();
}

TEST(S1_06_Reactor, UpstreamDyingBeforeAnyResponseGetsDefined502) {
  const std::size_t fd_before = count_open_fds();
  Harness harness;
  std::string why;
  ASSERT_TRUE(harness.start(default_options(), &why)) << why;

  Client client;
  ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;
  ASSERT_TRUE(client.write_all("GET /dies HTTP/1.1\r\nHost: gateway.test\r\n\r\n"));

  std::string status;
  std::string body;
  ASSERT_TRUE(client.read_response(&status, &body, &why))
      << "上游一个字节的响应都没回，网关没有给出可定义的 5xx：" << why;
  EXPECT_EQ(status, "HTTP/1.1 502 Bad Gateway");
  ASSERT_TRUE(client.wait_for_eof(&why)) << "502 之后这条配对连接必须收尾：" << why;

  ReactorStats& stats = harness.reactor()->stats();
  EXPECT_EQ(stats.load(stats.upstream_truncated_before_head), 1u);
  EXPECT_EQ(stats.load(stats.gateway_replies_sent), 1u);
  EXPECT_EQ(stats.load(stats.upstream_truncated_in_body), 0u) << "两个截断类别互斥";
  EXPECT_EQ(stats.load(stats.responses_completed), 0u) << "自产的 502 不是上游的响应";

  ASSERT_TRUE(wait_until([&] { return stats.load(stats.live_connections) == 0u; }, 5000));
  EXPECT_EQ(stats.load(stats.connections_constructed), 2u) << "下游 + 一条已断的上游";
  EXPECT_EQ(stats.load(stats.connections_destructed), 2u);
  client.close();
  harness.shutdown_all();
  EXPECT_EQ(count_open_fds(), fd_before) << "502 那条配对留下没关的 fd";
}

TEST(S1_06_Reactor, UpstreamDyingMidBodyIsTruncatedByEofWithoutASecondReply) {
  Harness harness;
  std::string why;
  ASSERT_TRUE(harness.start(default_options(), &why)) << why;
  const std::string body = fixed_body_text();
  harness.upstream().set_fixed_response_body(body);
  const std::string full = expected_cl_response(body);
  const std::size_t head_bytes = full.size() - body.size();
  // 头 + 半条 body 之后断：最终头**已经**在客户端手里，网关此时没有任何资格
  // 再产一条响应，只能让连接以 EOF 结束（客户端按 CL 判定"没读满"就是截断）。
  harness.upstream().set_die_after_response_bytes(head_bytes + 4);

  Client client;
  ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;
  ASSERT_TRUE(client.write_all(fixed_request_text(1)));

  std::string got;
  ASSERT_TRUE(client.drain_to_eof(&got, &why)) << why;
  EXPECT_EQ(got, full.substr(0, head_bytes + 4))
      << "已到达的字节必须原样交付；网关一个字节都不许多、不许少";
  EXPECT_EQ(got.find("502"), std::string::npos)
      << "头已透传还追加 5xx，就是给同一个请求两条响应";

  ReactorStats& stats = harness.reactor()->stats();
  EXPECT_EQ(stats.load(stats.upstream_truncated_in_body), 1u);
  EXPECT_EQ(stats.load(stats.upstream_truncated_before_head), 0u);
  EXPECT_EQ(stats.load(stats.gateway_replies_sent), 0u) << "这一支网关不该自产任何响应";
  EXPECT_EQ(stats.load(stats.responses_completed), 0u) << "CL 没读满不算完结";

  ASSERT_TRUE(wait_until([&] { return stats.load(stats.live_connections) == 0u; }, 5000));
  EXPECT_EQ(stats.load(stats.connections_constructed), 2u);
  EXPECT_EQ(stats.load(stats.connections_destructed), 2u);
  client.close();
}

TEST(S1_06_Reactor, EndlessResponseHeadIsKilledAtTheBudget) {
  // 半包不是错误——但"大到不可能是合法头"是。这条用例把那条分界线钉住：
  // 上游发 70 KiB 还没有终止串、而且**不关连接**。网关要么无限把字节往入向
  // 缓冲里堆（内存放大口开给上游），要么在预算处判死。判死之后 502 依然合法，
  // 因为最终头从没排给下游。
  Harness harness;
  std::string why;
  ASSERT_TRUE(harness.start(default_options(), &why)) << why;

  Client client;
  ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;
  ASSERT_TRUE(client.write_all("GET /endless-head HTTP/1.1\r\nHost: gateway.test\r\n\r\n"));

  std::string status;
  std::string body;
  ASSERT_TRUE(client.read_response(&status, &body, &why))
      << "永不完结的响应头没有被在预算处判死：" << why;
  EXPECT_EQ(status, "HTTP/1.1 502 Bad Gateway");

  ReactorStats& stats = harness.reactor()->stats();
  EXPECT_EQ(stats.load(stats.upstream_truncated_before_head), 1u);
  EXPECT_EQ(stats.load(stats.gateway_replies_sent), 1u);
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.live_connections) == 0u; }, 5000))
      << "判死之后配对连接没有收尾";
  EXPECT_EQ(stats.load(stats.connections_constructed), 2u);
  EXPECT_EQ(stats.load(stats.connections_destructed), 2u);
  client.close();
}

TEST(S1_06_Reactor, RejectCodesKeepTheirOwnStatusCodesEndToEnd) {
  // 映射表在** reactor 层**的可观测版本：framing_test 证的是表本身，这里证的是
  // "表里的状态码真的从网关流到了客户端"，而且不同错误码给出不同状态码。
  struct Case {
    std::string request;
    std::string_view expected_status;
  };
  const std::vector<Case> cases = {
      {"NOT HTTP AT ALL\r\n\r\n", "HTTP/1.1 400 Bad Request"},
      {"GET / HTTP/1.1\r\nHost: gateway.test\r\nHost: other\r\n\r\n", "HTTP/1.1 400 Bad Request"},
      {"GET / HTTP/9.9\r\nHost: gateway.test\r\n\r\n", "HTTP/1.1 400 Bad Request"},
      {"CONNECT example.com:443 HTTP/1.1\r\nHost: example.com\r\n\r\n",
       "HTTP/1.1 501 Not Implemented"},
      {"GET / HTTP/1.1\r\nHost: gateway.test\r\nX-Big: " + std::string(9000, 'a') +
       "\r\n\r\n",
       "HTTP/1.1 413 Content Too Large"},
  };

  Harness harness;
  std::string why;
  ASSERT_TRUE(harness.start(default_options(), &why)) << why;

  for (std::size_t i = 0; i < cases.size(); ++i) {
    Client client;
    ASSERT_TRUE(client.connect_to(harness.port(), &why)) << "第 " << i << " 条连接：" << why;
    ASSERT_TRUE(client.write_all(cases[i].request));
    std::string status;
    std::string body;
    ASSERT_TRUE(client.read_response(&status, &body, &why))
        << "第 " << i << " 条畸形请求没有换来响应：" << why;
    EXPECT_EQ(status, cases[i].expected_status) << "第 " << i << " 条错误码映射错了";
    ASSERT_TRUE(client.wait_for_eof(&why)) << "第 " << i << " 条：错误响应之后必须收尾";
    ReactorStats& stats = harness.reactor()->stats();
    EXPECT_EQ(stats.load(stats.gateway_replies_sent), i + 1) << "自产响应要留可断言的计数";
    EXPECT_EQ(stats.load(stats.upstream_opened), 0u) << "畸形请求一条都不该回源";
    client.close();
  }

  ReactorStats& stats = harness.reactor()->stats();
  EXPECT_EQ(stats.load(stats.requests_rejected), static_cast<std::uint64_t>(cases.size()));
  EXPECT_EQ(stats.load(stats.requests_parsed), 0u);
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.live_connections) == 0u; }, 5000));
}

TEST(S1_06_Reactor, ClosePairCompletesAtTheFramingBoundary) {
  // S1-05 把 close 落成"意图 + EOF 级联"，并把"什么时候算响应完结"留给 S1-06。
  // 这里兑现的是**完结点本身**：CL 读满那一刻 responses_completed 落 1，
  // 截断计数器保持 0（读满不是截断），随后配对在这一点上收尾。
  // 我不在这里断"是网关先关还是上游先关"——TestUpstream 认得 `Connection: close`
  // 并会在这条响应后自己收尾，两种顺序都是合法时序，断言它们中的某一种
  // 只会把测试绑到对端的脾气上（S1-05 的 take-once 教训，同一课）。
  Harness harness;
  std::string why;
  ASSERT_TRUE(harness.start(default_options(), &why)) << why;

  Client client;
  ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;
  ASSERT_TRUE(client.write_all(close_request_text(1)));
  std::string status;
  std::string body;
  ASSERT_TRUE(client.read_response(&status, &body, &why)) << why;
  EXPECT_EQ(status, "HTTP/1.1 200 OK");
  EXPECT_EQ(body, expected_body(1));

  ReactorStats& stats = harness.reactor()->stats();
  // 内核级证据先行（响应已经收到），之后才等收尾计数 —— 观测纪律那条。
  ASSERT_TRUE(client.wait_for_eof(&why)) << "close 的响应完结后网关没收尾这条连接：" << why;
  EXPECT_EQ(stats.load(stats.responses_completed), 1u) << "完结点必须由框架判定给出";
  EXPECT_EQ(stats.load(stats.close_headers_honored), 1u);
  EXPECT_EQ(stats.load(stats.upstream_truncated_in_body), 0u) << "读满 CL 不是截断";
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.live_connections) == 0u; }, 5000));
  EXPECT_EQ(stats.load(stats.connections_constructed), 2u);
  EXPECT_EQ(stats.load(stats.connections_destructed), 2u);
  client.close();
}

// --- S1-07：背压雏形（撞停读线 → 摘 EPOLLIN → 平台期 → 回落 → 探测恢复） ------
// 验收句逐条对应：
//   「停止后能观测到网关不再消费下游可读事件（epoll 侧可断言）」
//     → 循环跑着的时候断的是 pause 计数 + **入向读取字节数的平台期**（原子观测面），
//       停循环之后断的是 `armed_events()` 回执里那个没有 EPOLLIN 的掩码（非原子观测）。
//       两个时刻分别断，不是因为我想多看一眼，是因为后者的合法读法只有那一个时刻。
//   「且内存不继续增长」
//     → 平台期内存增长的可断言代理就是"字节没有进网关"：入向读取累计数不动。
//       RSS 只打印不判断（sanitizer 下它是噪声，见文件头那条宏）。
//   「水位回落后自动恢复读取」
//     → resume 计数 + **恢复之后不需要客户端再写一个新请求就能继续消费**（探测读在
//       起作用），最后一条响应逐字节齐 —— 自动恢复如果只挂在"下一次事件"上，
//       这条就会红在超时上，而那正是边沿触发的睡死形态。
//
// 四个方向各一条：请求方向（上游不消费）、响应方向（客户端不消费）、隧道方向，
// 再加一条"正常流量不许因此多打 epoll_ctl"的回归——背压省的是 syscall，
// 如果它自己在 keep-alive 稳态里制造 syscall，那就是把 B-001 的病因换了个名字。

namespace {

// 测试用的停读线：32 KiB。它卡在两个实测出来的约束中间，不是"越小越狠"。
//   上限：转发的最小单位是"整条请求 / 整个响应头"（`Connection::queue_bytes` 的整块
//     判断），所以单条请求必须明显小于这条线，否则它永远装不进出向缓冲。
//     这里的测试请求是 8.3 KiB（见 kPostBodyBytes），留得下也转得出。
//   下限：线必须**远小于内核在这段路径上能吸收的量**。第一次跑用的是 64 KiB，
//     结果响应方向跑满 6 秒 `pause=0`、`high_water_hits=2`，请求方向的平台期
//     出现在 1.86 MB 处 —— 字节全被"对端发送缓冲 + 网关接收缓冲"那两段吃掉了，
//     应用的线一次都没成为第一道。32 KiB 才让线先于内核撞。
// 这条实测关系本身就是结论，不只是取证的麻烦：TCP 自己的流控替网关挡掉了大部分
// 压力，应用侧停读线只在"对端塞进来的量超过内核这段路径能吸收的量"时才有活干。
// 默认那条 256 KiB 因此是**真实链路**（跨机、带宽延迟积大）上的量级，
// 不是回环上的；这条差异要记进 ARCHITECTURE 1.15，不能靠调小默认值掩盖。
constexpr std::size_t kWaterMark = 32u * 1024u;
constexpr std::size_t kPostBodyBytes = 8192u;

// 一条**完整**的 POST 请求（头 + 声明长度的 body）。请求方向要用它，理由见上：
// 半条请求会停在停读线上永远转不出去，那测的是边界不是恢复。
std::string post_request_text(std::size_t i, std::size_t body_bytes) {
  return "POST /b" + std::to_string(i) + " HTTP/1.1\r\nHost: gateway.test\r\n"
         "Content-Length: " + std::to_string(body_bytes) + "\r\n\r\n" +
         std::string(body_bytes, 'x');
}

// 把缓冲尺寸钉到内核允许的下限（Linux 会把它翻倍记账，所以写 4096 实际拿到 8192）。
// 只在**测试自己的 socket** 上做：要的是"这一侧吸收不了多少字节"，让水位成为
// 先撞到的那一道，而不是内核缓冲池。网关侧的接收缓冲由网关自己拥有，不归测试调。
bool shrink_socket_buffer(int fd, int optname, std::string* why) {
  const int bytes = 4096;
  if (::setsockopt(fd, SOL_SOCKET, optname, &bytes, sizeof(bytes)) != 0) {
    *why = "setsockopt(" + std::to_string(optname) + ") failed errno=" + std::to_string(errno);
    return false;
  }
  return true;
}

// 写线程的收尾。写成 RAII 而不是"记得 join"：ASSERT_* 展开成 return，而 return 出去时
// 线程还卡在对端窗口上，一个可 join 的 std::thread 析构就直接 terminate——
// 隧道那条第一次跑就是这样崩的，现场连一行断言信息都没留下。
// 顺序也定死：先放开上游（让写线程有机会写完），再 join。反过来就是拿一个
// 永远不会到来的条件去等一个永远不会返回的线程。
struct StallReleasingJoiner {
  TestUpstream& upstream;
  std::thread& writer;
  ~StallReleasingJoiner() {
    upstream.set_stall_reads(false);
    if (writer.joinable()) writer.join();
  }
};

}  // namespace

TEST(S1_07_Backpressure, RequestDirectionStopsReadingAndRecoversWithoutLoss) {
  Harness harness;
  std::string why;
  Harness::Options options = default_options();
  options.config.connection.inbound.high_water_mark = kWaterMark;
  options.config.connection.outbound.high_water_mark = kWaterMark;
  ASSERT_TRUE(harness.start(options, &why)) << why;

  // 上游从用例开始就不消费任何字节。它照旧 accept、照旧持有连接——"连接还在但没人
  // 读"才是背压的真实形态；如果这里改成"上游关掉连接"，测的就是 S1-06 的截断而不是 S1-07。
  harness.upstream().set_stall_reads(true);

  Client client;
  ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;
  ASSERT_TRUE(shrink_socket_buffer(client.fd(), SO_SNDBUF, &why)) << why;

  // 写 384 条完整 POST（≈3.17 MiB）。这个线程会**阻塞在 write() 上**，而阻塞本身就是
  // 内核给的证据：网关不读了，窗口关死了。断言因此可以站在另一侧做。
  // 384 是量出来的，不是随手翻倍的：256 条（2.11 MiB）在 asan-ubsan 下平台期恰好落在
  // 2,113,684 = **整个载荷**上——那不是停读线挡住了字节，是写侧把货全部倒进了内核路径
  // （回环上"网关→上游"这段实测吸收 ≈1.9 MB），断言于是为一个不成立的理由通过。
  // 载荷必须明显超过那段吸收量，平台期才只能由"网关不再 read"产生；debug 同一条
  // 是 1,906,999（真墙），asan 是 2,113,684（假墙）——两条都要成立就得把载荷抬上去。
  // Client 的写路径只碰 fd_，读路径只碰 carry_，所以这里"另一线程写、主线程读同一个
  // fd"不共享可变状态——不是"应该没事"，是这两个字段确实不相交。
  constexpr std::size_t kRequestCount = 384;
  std::atomic<bool> writer_blocked{true};
  std::atomic<bool> write_failed{false};
  std::thread writer([&] {
    for (std::size_t i = 1; i <= kRequestCount; ++i) {
      if (!client.write_all(post_request_text(i, kPostBodyBytes))) {
        write_failed.store(true, std::memory_order_relaxed);
        break;
      }
    }
    writer_blocked.store(false, std::memory_order_relaxed);
  });
  const StallReleasingJoiner joiner{harness.upstream(), writer};

  ReactorStats& stats = harness.reactor()->stats();
  // 先有内核级观测（写线程还卡在 write 上），再等 pause 落地 —— stats.h 那条纪律的形状。
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.backpressure_pause_count) >= 1u; }, 6000))
      << "3.2 MiB 已经压到网关入向，却没有一次 EPOLLIN 摘除：停读动作没生效";
  EXPECT_TRUE(writer_blocked.load(std::memory_order_relaxed))
      << "pause 计数到了，客户端却没被卡住——那这轮顶到的可能是内核缓冲而不是网关的停读线";
  EXPECT_GE(stats.load(stats.high_water_hits), 1u) << "S1-02 的撞线口径必须仍是这条链的第一环";

  // 平台期：入向读取累计数进入静止。这是"内存不继续增长"在网关这一侧唯一不靠
  // 猜的代理量——字节没进网关，就没有任何一块缓冲会为它长大。
  bool plateau_reached = false;
  const std::uint64_t plateau_bytes =
      wait_for_plateau(stats.bytes_read_downstream, 4000, &plateau_reached);
  const std::uint64_t rss_at_plateau = current_rss_kib();
  EXPECT_TRUE(plateau_reached)
      << "摘掉 EPOLLIN 之后入向读取仍在增长：停读只是没读到，实际还在消费下游";
  EXPECT_GE(plateau_bytes, kWaterMark) << "平台期观测点采得太早，还没到水位就先断上了";
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const std::uint64_t rss_after_plateau = current_rss_kib();
  // RSS 只打印不判断：sanitizer 下它是噪声，把它写成断言得到的是一条随机红的用例。
  std::printf("[S1-07 请求方向] 平台期入向读取累计=%llu 字节（停住=%d）；RSS %llu→%llu KiB\n",
              static_cast<unsigned long long>(plateau_bytes), plateau_reached ? 1 : 0,
              static_cast<unsigned long long>(rss_at_plateau),
              static_cast<unsigned long long>(rss_after_plateau));

  // 水位回落的驱动：上游开始消费。网关这边**不做任何新动作**，也没有客户端的新请求，
  // 恢复只能来自"腾出空间 → 重算掩码 → 挂回 EPOLLIN → 主动探测读"这条链。
  harness.upstream().set_stall_reads(false);

  // 逐条取回全部响应。任何一条没回来，这条都红在超时上（5s 读超时）而不是红在计数上——
  // 这正是"边沿触发睡死"应有的表现方式。
  for (std::size_t i = 1; i <= kRequestCount; ++i) {
    std::string status;
    std::string body;
    ASSERT_TRUE(client.read_response(&status, &body, &why))
        << "第 " << i << " 条响应没回来：恢复读取没有真的恢复消费：" << why;
    ASSERT_TRUE(status == "HTTP/1.1 200 OK") << "第 " << i << " 条状态码不对：" << status;
    ASSERT_TRUE(body == expected_body(i)) << "第 " << i << " 条 body 串号或截断";
  }
  writer.join();
  EXPECT_FALSE(write_failed.load(std::memory_order_relaxed)) << "写线程遇到真错误，用例前提塌了";

  EXPECT_EQ(stats.load(stats.requests_parsed), static_cast<std::uint64_t>(kRequestCount))
      << "字节零丢失：每条请求都被解析并转发过";
  EXPECT_EQ(harness.upstream().requests_served(), static_cast<std::uint64_t>(kRequestCount));
  const std::uint64_t pause_count = stats.load(stats.backpressure_pause_count);
  const std::uint64_t resume_count = stats.load(stats.backpressure_resume_count);
  EXPECT_GE(resume_count, 1u) << "恢复没有发生，前面那次 pause 是永久睡死的前一刻";
  EXPECT_LE(pause_count, resume_count + 1u)
      << "pause/resume 应当成对（至多差一次收尾中的暂停）；差得多说明水位线在横跳";
  std::printf("[S1-07 请求方向] pause=%llu resume=%llu 高水位撞线=%llu 入向读取累计=%llu\n",
              static_cast<unsigned long long>(pause_count),
              static_cast<unsigned long long>(resume_count),
              static_cast<unsigned long long>(stats.load(stats.high_water_hits)),
              static_cast<unsigned long long>(stats.load(stats.bytes_read_downstream)));

  // 收尾之后才读非原子的注册表观测：此刻掩码里已经没有 EPOLLIN 了（摘掉是收尾的一部分），
  // 所以"停读过"这件事的证据是上面那两个原子计数，这里只断"最后没有连接挂在半停状态"。
  harness.stop_loop();
  EXPECT_EQ(harness.reactor()->connections_with_read_disarmed(), 0u)
      << "循环已停，还有 kOpen 连接处于停读态：这条连接的恢复条件永远等不到了";
  client.close();
  harness.upstream().set_stall_reads(false);
  harness.shutdown_all();
}

TEST(S1_07_Backpressure, ResponseDirectionSurvivesASlowClientByteForByte) {
  Harness harness;
  std::string why;
  Harness::Options options = default_options();
  options.config.connection.inbound.high_water_mark = kWaterMark;
  options.config.connection.outbound.high_water_mark = kWaterMark;
  ASSERT_TRUE(harness.start(options, &why)) << why;

  // 一条 1 MiB 的固定体响应。尺寸是量出来的，不是猜的：这条路径上的吸收能力是
  // 「网关出向水位 32 KiB + 网关自己的发送缓冲 + 客户端接收缓冲」，而响应还得先从
  // 上游走到网关（上游发送缓冲 + 网关接收缓冲）。256 KiB 实测**一次都撞不到停读线**
  // （全部被这几段缓冲吃掉），1 MiB 实测能撞（pause≥1，撞线 2 次）。
  //
  // 这里刻意**不夹客户端的接收窗口**。第一版夹过（SO_RCVBUF 4 KiB），夹完之后线确实
  // 撞得更狠，但补齐那 1 MiB 变成 25 秒：小窗口把发送侧推进了 zero-window 探测的节奏，
  // 每 32 KiB 一档、每档等一次 TCP 的 persist 计时器（RTO 下限 200 ms）——
  // 测到的是内核的计时器，不是网关的补齐能力。放开夹口之后同一批断言 240 ms 跑完。
  // 同一条理由后来把夹具侧上游的钉窗也一并删掉了：实测撞线位置没变，而放开是无效的
  // （window_clamp 不回涨），见 test_upstream.h 里那段删除记录。
  const std::string body(1024u * 1024u, 'k');
  harness.upstream().set_fixed_response_body(body);
  const std::string expected = expected_cl_response(body);

  Client client;
  ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;
  ASSERT_TRUE(client.write_all(fixed_request_text(1)));

  ReactorStats& stats = harness.reactor()->stats();
  // 内核级观测先行：客户端**还没有 read 过任何一个字节**，所以此刻它收到的是 0 字节，
  // 而网关已经尽力往这条没人读的连接上压了。等 pause 落地只是等同一批处理里的收尾。
  const bool paused =
      wait_until([&] { return stats.load(stats.backpressure_pause_count) >= 1u; }, 6000);
  std::printf("[S1-07 响应方向] pause=%llu resume=%llu 撞线=%llu 上游入向=%llu 已排下游=%llu\n",
              static_cast<unsigned long long>(stats.load(stats.backpressure_pause_count)),
              static_cast<unsigned long long>(stats.load(stats.backpressure_resume_count)),
              static_cast<unsigned long long>(stats.load(stats.high_water_hits)),
              static_cast<unsigned long long>(stats.load(stats.bytes_read_upstream)),
              static_cast<unsigned long long>(stats.load(stats.bytes_up_to_downstream)));
  ASSERT_TRUE(paused) << "慢客户端 + 1 MiB 响应没让网关停止读上游：响应方向的背压缺失";
  bool plateau_reached = false;
  const std::uint64_t plateau_bytes = wait_for_plateau(stats.bytes_read_upstream, 4000,
                                                       &plateau_reached);
  EXPECT_TRUE(plateau_reached) << "上游入向仍在增长：网关在客户端没读的时候继续搬响应";
  std::printf("[S1-07 响应方向] 平台期上游入向读取累计=%llu 字节（响应全长 %zu）\n",
              static_cast<unsigned long long>(plateau_bytes), expected.size());

  // 这条平台期的**证据强度不如请求方向那条**，写清楚：1 MiB 的响应最后是被网关
  // 全部读走的（回环上"网关→客户端"这段内核路径能吸收的量够大），所以读累计停住
  // 有一部分是因为上游已经全部交完。响应方向真正算数的是上面那对 pause/resume——
  // 摘过 EPOLLIN 就是摘过。平台期在这里只担保一件事：源已经穷了之后网关不再空转读。
  // 内存那一条要分成两笔记：网关**自己的堆**（Buffer）由停读线担保，
  // 而内核的 socket 缓冲归 TCP 管、由发送窗口自己收口——这条区分记在 ARCHITECTURE 1.15。

  // 客户端开始消费 —— 水位回落的唯一驱动。之后没有任何新的请求，所以后续的搬运
  // 全靠"腾出空间 → 重算掩码 → 挂回 EPOLLIN → 探测读"这条链把剩下的字节接完。
  std::string got;
  ASSERT_TRUE(client.read_exact(&got, expected.size(), &why))
      << "慢客户端开始读之后响应没有补齐：" << why;
  EXPECT_EQ(got, expected) << "背压期间的响应字节必须逐字节一致（不丢、不重、不改序）";

  EXPECT_GE(stats.load(stats.backpressure_resume_count), 1u);
  EXPECT_EQ(stats.load(stats.responses_completed), 1u) << "补齐的那条响应必须落下完结点";
  EXPECT_EQ(stats.load(stats.gateway_replies_sent), 0u) << "背压不是错误，不许自产响应";
  EXPECT_EQ(stats.load(stats.upstream_truncated_in_body), 0u) << "慢不是截断";

  client.close();
  harness.shutdown_all();
}

TEST(S1_07_Backpressure, KeepAliveWorkloadWithoutPressureAddsNoEpollCtl) {
  // 这条是**反证**：背压省的是 syscall，它自己不许在稳态里花 syscall。
  // 滞回带 + `apply_events` 的相等即返回，是 BENCHMARK 给 p99 归因的那笔
  // 「每请求 5.0 次 epoll_ctl」的唯一削减来源；如果稳态流量上这个数不降，
  // 说明去重只在测试里成立。断言取"每请求不超过 4 次"：
  // 一条 keep-alive 请求的最短路径是 上游挂EPOLLOUT / 上游摘EPOLLOUT /
  // 下游挂EPOLLOUT / 下游摘EPOLLOUT = 4 次，第 5 次只能是重复提交或抖动。
  Harness harness;
  std::string why;
  ASSERT_TRUE(harness.start(default_options(), &why)) << why;

  Client client;
  ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;
  constexpr std::size_t kRequests = 1000;
  for (std::size_t i = 1; i <= kRequests; ++i) {
    ASSERT_TRUE(client.write_all(request_text(i)));
    std::string status;
    std::string body;
    ASSERT_TRUE(client.read_response(&status, &body, &why)) << "第 " << i << " 条：" << why;
    ASSERT_TRUE(body == expected_body(i)) << "第 " << i << " 条 body 不对";
  }

  ReactorStats& stats = harness.reactor()->stats();
  EXPECT_EQ(stats.load(stats.backpressure_pause_count), 0u) << "64 字节的请求不该撞停读线";
  EXPECT_EQ(stats.load(stats.backpressure_resume_count), 0u);
  EXPECT_EQ(stats.load(stats.high_water_hits), 0u) << "撞线口径（S1-02）必须仍是 0";

  harness.stop_loop();
  const std::uint64_t mods = harness.reactor()->epoll_modifications();
  const double per_request = static_cast<double>(mods) / static_cast<double>(kRequests);
  std::printf("[S1-07 稳态] %zu 条 keep-alive 请求：epoll_modifications=%llu（%.2f 次/请求）"
              " registrations=%llu\n",
              kRequests, static_cast<unsigned long long>(mods), per_request,
              static_cast<unsigned long long>(harness.reactor()->epoll_registrations()));
  EXPECT_LE(mods, kRequests * 4 + 8) << "每请求超过 4 次 epoll_ctl：去重没起作用";
  client.close();
  harness.shutdown_all();
}

TEST(S1_07_Backpressure, TunnelDirectionPausesAtTheWaterLineAndDeliversEveryByte) {
  Harness harness;
  std::string why;
  Harness::Options options = default_options();
  options.config.connection.inbound.high_water_mark = kWaterMark;
  options.config.connection.outbound.high_water_mark = kWaterMark;
  ASSERT_TRUE(harness.start(options, &why)) << why;

  // 先把握手走完（上游得先读到 Upgrade 请求才回 101），再让它停止消费。
  Client client;
  ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;
  ASSERT_TRUE(client.write_all(upgrade_request_text()));
  std::string head;
  ASSERT_TRUE(client.read_head(&head, &why)) << "101 没有回来：" << why;
  ASSERT_TRUE(head.find("HTTP/1.1 101") == 0u) << head;

  ReactorStats& stats = harness.reactor()->stats();
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.upgrades_101) == 1u; }, 3000));
  harness.upstream().set_stall_reads(true);

  // 隧道里的"流量"是一坨不透明字节：这里用约 2.7 MiB 非 HTTP 形态的载荷
  // （0x00/0x0D/0x0A 混在里面）。如果这段被当请求解析，requests_parsed 会当场跳起来——
  // 所以它同时是 ADR-09 的一次复查。尺寸下限由内核路径决定：上游停读后这条路径实测
  // 吸收到 1,898,661 B（debug）/ 1,897,637 B（asan）就停住——那堵墙是网关自己的
  // SO_SNDBUF，不是应用的停读线，所以载荷必须明显超过它，平台期才只能由"网关不再 read"
  // 产生（同一个道理把请求方向那条的 kRequestCount 从 256 抬到 384）。
  std::string blob;
  blob.reserve(2048u * 1024u);
  for (std::size_t i = 0; i < 2048u * 1024u; i += 13) {
    blob.append("\x00\x0D\x0A\x21\x81\xFE", 6);
    blob.append(4, static_cast<char>('a' + static_cast<int>(i % 26)));
    blob.append("\r\n\r\n", 4);
    blob.append(3, static_cast<char>('0' + static_cast<int>(i % 10)));
  }
  // 这里同样**不夹**客户端的发送缓冲。夹过一版（SO_SNDBUF 4 KiB），停读线撞得更狠，
  // 但恢复期变成每 32 KiB 等一次 TCP 的 persist 计时器：asan-ubsan 下这条用例跑到
  // 34 秒，超过 ctest 的 TIMEOUT 30。理由与响应方向那条一样——夹口测的是内核计时器，
  // 不夹才测得到网关自己。
  std::atomic<bool> writer_finished{false};
  std::thread writer([&] {
    (void)client.write_all(blob);
    writer_finished.store(true, std::memory_order_relaxed);
  });
  const StallReleasingJoiner joiner{harness.upstream(), writer};
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.backpressure_pause_count) >= 1u; }, 6000))
      << "隧道下游入向顶到 MiB 量级仍未摘读：透传路径绕过了背压判定";
  // 写线程此刻有没有被卡住，这条**只打印不判断**（变量叫 writer_finished 而不是
  // writer_blocked 就是为了不让名字替我下结论）：不夹发送缓冲之后，回环的内核路径
  // 可能先替网关把整段载荷咽下去，"客户端被卡住"就不再是停读线生效的必要证据——
  // 这条真正的证据是上面的 pause 计数加下面的平台期。
  // 请求方向那条反过来保留断言：那里客户端自己的 SO_SNDBUF 被夹到下限，写侧卡住是
  // 网关摘读的内核级旁证。同一个观测在两条用例里取不同强度，是因为两条的压强来源不同：
  // 一条墙由测试主动摆（客户端发送缓冲），一条墙本来就在路上（网关→上游的内核路径）。
  std::printf("[S1-07 隧道方向] 摘读落地时写线程已写完=%d\n",
              writer_finished.load(std::memory_order_relaxed) ? 1 : 0);
  bool plateau_reached = false;
  const std::uint64_t plateau_bytes =
      wait_for_plateau(stats.bytes_read_downstream, 4000, &plateau_reached);
  EXPECT_TRUE(plateau_reached) << "隧道方向的停读只是没生效：字节还在进网关";
  std::printf("[S1-07 隧道方向] 平台期入向读取累计=%llu 字节（载荷 %zu 字节，停住=%d）\n",
              static_cast<unsigned long long>(plateau_bytes), blob.size(),
              plateau_reached ? 1 : 0);

  // 恢复：上游开始消费 → 隧道把下游入向的字节搬走 → 水位回落 → 挂回 EPOLLIN + 探测读。
  harness.upstream().set_stall_reads(false);
  // 回显量 = 载荷量（测试上游"不解析帧"地原样退回）。客户端一边读一边等写线程把
  // 剩下的字节灌完，这条顺序和真实隧道里双向同时跑流量的形态一致。
  std::string got;
  ASSERT_TRUE(client.read_exact(&got, blob.size(), &why))
      << "恢复之后隧道没有把全部字节搬到：" << why;
  writer.join();
  EXPECT_EQ(got, blob) << "透传在背压期间改写了字节序";

  EXPECT_GE(stats.load(stats.backpressure_resume_count), 1u);
  EXPECT_EQ(stats.load(stats.requests_parsed), 1u) << "帧字节一次都不许进解析器";
  EXPECT_EQ(stats.load(stats.bytes_tunnel_down), static_cast<std::uint64_t>(blob.size()))
      << "隧道账上必须记满整段载荷（不重、不漏）";
  // 上游→下游这一本的口径**含 101 响应头**（stats.h 上写死了），所以基数是
  // "载荷 + 握手头"而不是载荷。握手头我用读回来的那份现算，不写常量：写死 141
  // 的话，将来夹具换一行头就变成一条需要考古才能修红的用例。
  EXPECT_EQ(stats.load(stats.bytes_tunnel_up),
            static_cast<std::uint64_t>(blob.size() + head.size()));

  client.close();
  harness.upstream().set_stall_reads(false);
  harness.shutdown_all();
}

// --- S1-08：filter 链骨架在真实事件循环上的两处证据 ---------------------------
// 纯链的性质（顺序、短路、状态存活）在 filter_chain_test.cc 里，这里只补那句
// 光看纯函数证不了的话：**「新增 filter 不修改转发主路径的任何一行代码」**。
// 它的可执行形式不是去数源码行数，而是同一套流量在"空链"与"两条放行 filter"下
// 必须逐段相同、且**每一项计数都相等**——syscall 计数变了就说明主路径上确实多走了路。
namespace {

class ReactorMarkerFilter : public filter::RequestFilter {
 public:
  ReactorMarkerFilter(std::string name, std::atomic<int>* calls, filter::Decision decision)
      : name_(std::move(name)), calls_(calls), decision_(decision) {}

  std::string_view name() const noexcept override { return name_; }

  filter::Decision on_request(filter::RequestInfo& info) override {
    // 计数只在"递给它的确实是一条真请求的可见面"时加：方法、目标、至少一个头、
    // 一个合法的 fd。少了这层核对，"主路径把 ParsedRequest 递给链"那一步可以是个
    // 空壳（全空的 RequestInfo 照样跑完链）而没人发现。
    if (!info.method.empty() && !info.target.empty() && !info.headers.empty() &&
        info.downstream_fd >= 0 && info.request_bytes > 0) {
      calls_->fetch_add(1, std::memory_order_relaxed);
    }
    return decision_;
  }

 private:
  std::string name_;
  std::atomic<int>* calls_;
  filter::Decision decision_;
};

// 一次流量：同一条 keep-alive 上三条 GET，各自等自己的响应。
struct DriveResult {
  bool ok = false;
  std::string transcript;  // 每条响应的状态行 + body 依次拼接
  std::uint64_t parsed = 0;
  std::uint64_t down_bytes = 0;
  std::uint64_t completed = 0;
  std::uint64_t opened = 0;
  std::uint64_t registrations = 0;
  std::uint64_t modifications = 0;
  std::uint64_t hits = 0;
};

DriveResult drive_three_requests(Harness& harness, std::string* why) {
  DriveResult result;
  Client client;
  if (!client.connect_to(harness.port(), why)) return result;
  for (std::size_t i = 1; i <= 3; ++i) {
    if (!client.write_all(fixed_request_text(i))) {
      *why = "第 " + std::to_string(i) + " 条请求写不出去";
      return result;
    }
    std::string status;
    std::string body;
    if (!client.read_response(&status, &body, why)) return result;
    result.transcript += status;
    result.transcript += '\n';
    result.transcript += body;
  }
  client.close();
  // 先停循环，再读非原子的那两个 epoll 计数器 —— stats.h 那条纪律在这一版被我自己的
  // 用例违反了：TSan 当场抓到"循环线程正在 ++ epoll_ 的普通 uint64、主线程同时在读它"。
  // 那不是误报，是我读法不对：那个数只在循环停下来之后才是一个数。
  harness.stop_loop();
  ReactorStats& stats = harness.reactor()->stats();
  result.parsed = stats.load(stats.requests_parsed);
  result.down_bytes = stats.load(stats.bytes_down_to_upstream);
  result.completed = stats.load(stats.responses_completed);
  result.opened = stats.load(stats.upstream_opened);
  result.registrations = harness.reactor()->epoll_registrations();
  result.modifications = harness.reactor()->epoll_modifications();
  result.hits = stats.load(stats.high_water_hits);
  result.ok = true;
  return result;
}

}  // namespace

TEST(S1_08_FilterReactor, PassThroughFiltersChangeNothingInTheForwardingPath) {
  std::string why;

  DriveResult bare;
  {
    Harness harness;
    ASSERT_TRUE(harness.start(default_options(), &why)) << "空链那一轮起不来：" << why;
    bare = drive_three_requests(harness, &why);
    ASSERT_TRUE(bare.ok) << "空链那一轮流量没跑完：" << why;
    harness.shutdown_all();
  }

  DriveResult filtered;
  {
    Harness harness;
    std::atomic<int> first_calls{0};
    std::atomic<int> second_calls{0};
    harness.add_filter(std::make_unique<ReactorMarkerFilter>(
        "first", &first_calls, filter::Decision::kContinue));
    harness.add_filter(std::make_unique<ReactorMarkerFilter>(
        "second", &second_calls, filter::Decision::kContinue));
    ASSERT_TRUE(harness.start(default_options(), &why)) << "带 filter 那一轮起不来：" << why;
    ASSERT_TRUE(harness.reactor()->request_filter_count() == 2u)
        << "filter 没在 run() 之前装进去（Harness::add_filter 的时序前提塌了）";
    filtered = drive_three_requests(harness, &why);
    ASSERT_TRUE(filtered.ok) << "带 filter 那一轮流量没跑完：" << why;
    EXPECT_EQ(first_calls.load(std::memory_order_relaxed), 3);
    EXPECT_EQ(second_calls.load(std::memory_order_relaxed), 3);
    harness.shutdown_all();
  }

  // 响应的可见内容逐段相同（状态行与 body 一条不差，顺序不错位）。
  EXPECT_EQ(filtered.transcript, bare.transcript);
  // 计数逐项相等。这一条才是"不改主路径"的硬证据：多一个 filter 就多一串 syscall、
  // 就多一次解析、就多一段字节的话，主路径已经被动了。
  EXPECT_EQ(filtered.parsed, bare.parsed) << "解析次数被链改动了";
  EXPECT_EQ(filtered.down_bytes, bare.down_bytes) << "转发的字节数被链改动了";
  EXPECT_EQ(filtered.completed, bare.completed) << "响应完结数被链改动了";
  EXPECT_EQ(filtered.opened, bare.opened) << "回源连接数被链改动了";
  EXPECT_EQ(filtered.registrations, bare.registrations) << "epoll 注册数被链改动了";
  EXPECT_EQ(filtered.modifications, bare.modifications) << "epoll 改掩码次数被链改动了";
  EXPECT_EQ(filtered.hits, bare.hits) << "撞水位次数被链改动了";
  EXPECT_EQ(bare.parsed, 3u) << "对照那一轮本身就没跑 3 条，比较无从谈起";
}

TEST(S1_08_FilterReactor, StoppedRequestNeverReachesUpstream) {
  Harness harness;
  std::string why;
  std::atomic<int> calls{0};
  harness.add_filter(std::make_unique<ReactorMarkerFilter>("stopper", &calls,
                                                          filter::Decision::kStop));
  ASSERT_TRUE(harness.start(default_options(), &why)) << why;

  Client client;
  ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;
  ASSERT_TRUE(client.write_all(fixed_request_text(1)));

  ReactorStats& stats = harness.reactor()->stats();
  // 先要看到"链确实被叫过一次"，再断"一条字节都没往上游走"。反过来的顺序会让我
  // 分不清是链拦下的，还是主路径本来就没走到（比如请求压根没解析完）。
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.requests_parsed) == 1u; }, 3000))
      << "请求根本没被解析出来，链没机会看到它";
  ASSERT_TRUE(wait_until([&] { return calls.load(std::memory_order_relaxed) == 1; }, 3000))
      << "链没被调用：主路径那处调用点没生效";

  // 停下来的形态是"什么都没发生"：没回源、没自产表态、连接还活着。
  // 留一点时间让"如果它其实转出去了"这件事发生，再取数——否则这条断言只是跑得快。
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_EQ(stats.load(stats.bytes_down_to_upstream), 0u) << "被拦下的请求还是把字节排进了上游";
  EXPECT_EQ(stats.load(stats.upstream_opened), 0u) << "被拦下的请求还是回源了";
  EXPECT_EQ(stats.load(stats.gateway_replies_sent), 0u) << "链停下之后网关自己编了一条响应（那不是 S1-08 的语义）";
  EXPECT_EQ(stats.load(stats.responses_completed), 0u);
  EXPECT_EQ(stats.load(stats.live_connections), 1u)
      << "拦下一条请求把连接也关掉了：客户端会把它读成上游故障，而不是"
         "被网关拒绝——两者必须分得开（今天先分给『什么都不做』）";

  client.close();
  harness.shutdown_all();
}

// ===========================================================================
// S2-01：多 Reactor 骨架 + 单向派发（ADR-03 多线程 Reactor + ADR-14 所有权模型）
// ===========================================================================
//
// 这一组断言的是 ADR-14 的所有权模型在结构上成立，五条各打一面：
//   一、round-robin 派发均衡（每 worker 接到数 == ⌈/⌊M/N⌉）；
//   二、10000 请求跨 worker 跑完，生命周期归零（live=0、constructed==destructed、
//       fd 回基线、聚合 epoll 注册/注销逐项对得上）；
//   三、16 线程并发压力下响应正确 + 收尾干净 —— **这条的真正门禁在 tsan preset**：
//       断言全绿且 TSan 零 data race，才证明"连接单线程拥有"在并发下成立；
//   四、stop() 让 accept 循环与所有 worker 都退出；
//   五、handoff 之后补的探测读把"注册前已缓冲的请求"读了出来（ADR-14 第 5 条）。
//
// "连接对象只被 owner worker 碰"这件事没法用一个 EXPECT 直接断——它的可证伪形式
// 就是 TSan：若两个线程碰了同一条连接或同一个 worker 的循环内状态，tsan 会红。
// 所以这组用例一半价值在断言、一半在"它们在 tsan 下跑过且零报告"。

class GroupHarness {
 public:
  struct Options {
    ReactorGroup::Config config;
    bool with_upstream = true;
  };

  bool start(const Options& options, std::string* why) {
    ReactorGroup::Config config = options.config;
    if (options.with_upstream) {
      if (!upstream_.start(why)) return false;
      upstream_.set_response_body_bytes(kBodyBytes);
      // S2-06a：端点进了列表，一个 worker 拿一个（i mod E）。这里给 1 个端点，
      // 所以既有 S2-01…S2-05 用例的拓扑与改动前逐字节相同。
      config.upstream_endpoints.push_back(
          ReactorGroup::Config::UpstreamEndpoint{"127.0.0.1", upstream_.port()});
    }
    config.listen_port = 0;  // 内核选端口：测试并行/重跑都不该撞车
    group_ = std::make_unique<ReactorGroup>(config);
    int error = 0;
    if (!group_->start_listen(&error)) {
      *why = "start_listen failed errno=" + std::to_string(error);
      return false;
    }
    port_ = group_->listen_port();
    // run() 在**这条线程**上跑 accept 循环，并在内部 spawn N 条 worker 线程；
    // stop_loop() 里 group_->stop() 让 accept 循环退出，run() 随后 stop+join 所有
    // worker，再 join 这条线程——所以 thread_ join 返回时，全部 1+N 条线程都已停。
    thread_ = std::thread([this] { batches_ = group_->run(); });
    return true;
  }

  void stop_loop() {
    if (group_ != nullptr) group_->stop();
    if (thread_.joinable()) thread_.join();
  }
  // S2-05：只 join。run() 里的 accept 循环会在**每个 worker 自己报"排干了"**之后才退出
  // 并 join 它们，所以这条 join 返回 = 全部 1+N 条线程都已停。
  void join_loop() {
    if (thread_.joinable()) thread_.join();
  }
  void destroy() {
    stop_loop();
    group_.reset();
  }
  void shutdown_all() {
    destroy();
    upstream_.stop_and_join();
  }

  ReactorGroup* group() noexcept { return group_.get(); }
  TestUpstream& upstream() noexcept { return upstream_; }
  std::uint64_t batches() const noexcept { return batches_; }
  int port() const noexcept { return port_; }

  ~GroupHarness() { destroy(); }

 private:
  // 声明顺序 = 销毁顺序的反面：group_（连同它的 worker 与连接）必须先于上游析构。
  TestUpstream upstream_;
  std::unique_ptr<ReactorGroup> group_;
  std::thread thread_;
  std::uint64_t batches_ = 0;
  int port_ = -1;
};

ReactorGroup::Config group_base_config(int workers) {
  ReactorGroup::Config config;
  config.worker_count = workers;
  config.wait_timeout_ms = 50;  // 测试里缩短：停循环更快，卡死暴露得更快
  return config;
}

// 一、派发均衡。用**空闲**连接（不发请求）测，所以不触发 ensure_upstream、
// 不碰 TestUpstream 的 32 客户端上限——本用例测的是 acceptor 的 round-robin，
// 不是转发。8 条派发到 4 worker，每个恰好 2 条（整除，断言不留余数歧义）。
TEST(S2_01_MultiReactor, DispatchIsRoundRobinBalanced) {
  constexpr int kWorkers = 4;
  constexpr int kConns = 8;
  GroupHarness harness;
  std::string why;
  GroupHarness::Options options;
  options.config = group_base_config(kWorkers);
  options.with_upstream = false;
  ASSERT_TRUE(harness.start(options, &why)) << why;

  std::vector<Client> clients(static_cast<std::size_t>(kConns));
  for (int i = 0; i < kConns; ++i) {
    ASSERT_TRUE(clients[static_cast<std::size_t>(i)].connect_to(harness.port(), &why)) << why;
  }
  ReactorStats& stats = harness.group()->stats();
  ASSERT_TRUE(wait_until(
      [&] { return stats.load(stats.downstream_accepted) == static_cast<std::uint64_t>(kConns); },
      3000))
      << "8 条连接没有全部被 acceptor 派发并注册";

  // 停循环后才读每 worker 的注册表（非原子观测，stats.h 第 25 行那条纪律）。
  harness.stop_loop();
  std::size_t total = 0;
  for (int i = 0; i < kWorkers; ++i) {
    const std::size_t n = harness.group()->worker_live_count(i);
    total += n;
    EXPECT_EQ(n, static_cast<std::size_t>(kConns / kWorkers))
        << "worker " << i << " 接到的连接数不是 round-robin 的均衡值";
  }
  EXPECT_EQ(total, static_cast<std::size_t>(kConns));
  EXPECT_EQ(stats.load(stats.live_connections), static_cast<std::uint64_t>(kConns))
      << "空闲连接此刻都该还在注册表里（stop 不排水，排水归 S2-05）";
  std::printf("[S2-01 balance] %d 条空闲连接派发到 %d worker：每个 %zu 条\n", kConns, kWorkers,
              static_cast<std::size_t>(kConns / kWorkers));

  for (auto& c : clients) c.close();
  harness.shutdown_all();
}

// 二、10000 请求跨 worker + 生命周期归零。顺序驱动（一条连接的 1250 条走完再走
// 下一条），所以上游那个**全局** seq 计数器是确定的（第 k 条请求 → seq=k），
// 响应体可以逐条精确比对——并发正确性交给用例三，这条要的是"跨 worker 的转发
// 内容正确 + 收尾干净"。8 条连接 round-robin 落到 4 个 worker，每个 worker 2 条。
TEST(S2_01_MultiReactor, TenThousandRequestsAcrossWorkersLeaveNothingAlive) {
  constexpr int kWorkers = 4;
  constexpr int kConns = 8;       // 8 下游 + 8 上游 = 16 ≤ TestUpstream 的 32 上限
  constexpr int kPerConn = 1250;  // 8 × 1250 = 10000
  constexpr std::uint64_t kRequests = static_cast<std::uint64_t>(kConns) * kPerConn;

  const std::size_t fd_before = count_open_fds();
  GroupHarness harness;
  std::string why;
  GroupHarness::Options options;
  options.config = group_base_config(kWorkers);
  ASSERT_TRUE(harness.start(options, &why)) << why;

  std::vector<Client> clients(static_cast<std::size_t>(kConns));
  for (int i = 0; i < kConns; ++i) {
    ASSERT_TRUE(clients[static_cast<std::size_t>(i)].connect_to(harness.port(), &why)) << why;
  }

  const auto started = std::chrono::steady_clock::now();
  std::size_t global_seq = 0;
  for (int i = 0; i < kConns; ++i) {
    for (int j = 0; j < kPerConn; ++j) {
      ++global_seq;
      Client& c = clients[static_cast<std::size_t>(i)];
      ASSERT_TRUE(c.write_all(request_text(global_seq))) << "conn " << i << " 第 " << global_seq;
      std::string status;
      std::string body;
      ASSERT_TRUE(c.read_response(&status, &body, &why)) << "conn " << i << "：" << why;
      ASSERT_EQ(status, "HTTP/1.1 200 OK");
      ASSERT_EQ(body, expected_body(global_seq))
          << "第 " << global_seq << " 条响应串号了：多 worker 下响应被安到了别的请求上";
    }
  }
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                            started)
          .count();

  // 收尾：先半关闭拿内核级证据（read() 返回 0），再等 live_connections 归零。
  for (auto& c : clients) c.shutdown_write();
  for (auto& c : clients) {
    ASSERT_TRUE(c.wait_for_eof(&why)) << "客户端发了 FIN，网关没把这侧连接关掉：" << why;
  }
  ReactorStats& stats = harness.group()->stats();
  // S2-02（ADR-15）：每条下游关闭后它的上游**归还进池**、活得比下游久，所以 8 条上游
  // 此刻都在池里（live==8），不再是 0。先确认 8 次回收都发生，再 stop_loop 排空池。
  ASSERT_TRUE(wait_until(
      [&] { return stats.load(stats.pool_returns) == static_cast<std::uint64_t>(kConns); }, 5000))
      << "8 条下游关闭后上游没有全部归还进池";

  harness.stop_loop();  // 排空池 + join；之后才允许读非原子的聚合 epoll 计数
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.live_connections) == 0u; }, 1000))
      << "stop 排空池后仍有连接对象没析构";

  // 复制成局部量再用：shutdown_all() 会销毁 group_，stats 引用届时悬空。
  const std::uint64_t parsed = stats.load(stats.requests_parsed);
  const std::uint64_t completed = stats.load(stats.responses_completed);
  const std::uint64_t accepted = stats.load(stats.downstream_accepted);
  const std::uint64_t opened = stats.load(stats.upstream_opened);
  const std::uint64_t constructed = stats.load(stats.connections_constructed);
  const std::uint64_t destructed = stats.load(stats.connections_destructed);
  const std::uint64_t closed = stats.load(stats.connections_closed);
  const std::uint64_t served = harness.upstream().requests_served();

  EXPECT_EQ(parsed, kRequests);
  EXPECT_EQ(completed, kRequests);
  EXPECT_EQ(served, kRequests) << "上游实际处理的条数也要是 10000";
  EXPECT_EQ(accepted, static_cast<std::uint64_t>(kConns));
  EXPECT_EQ(opened, static_cast<std::uint64_t>(kConns)) << "keep-alive 下每条下游只开一条上游";
  EXPECT_EQ(constructed, 2u * static_cast<std::uint64_t>(kConns)) << "8 下游 + 8 上游 = 16";
  EXPECT_EQ(constructed, destructed) << "构造与析构逐项相等（上游在 stop 排空池时析构）";
  EXPECT_EQ(closed, 2u * static_cast<std::uint64_t>(kConns));
  // 池的证据：顺序驱动、每条下游全程独占它的上游（无并发借还）→ borrows=0、returns=8。
  EXPECT_EQ(stats.load(stats.pool_borrows), 0u);
  EXPECT_EQ(stats.load(stats.pool_returns), static_cast<std::uint64_t>(kConns));

  const std::uint64_t reg = harness.group()->epoll_registrations();
  const std::uint64_t unreg = harness.group()->epoll_unregistrations();
  // 聚合注册数 = N 个 worker 各自的常驻 wake_fd（各 1，~Reactor 才注销）+ 16 条连接；
  // 注销数 = 16（连接关闭时注销）。acceptor 侧的 epoll 不计入（epoll_* 只汇总 worker）。
  EXPECT_EQ(reg, static_cast<std::uint64_t>(kWorkers) + accepted + opened);
  EXPECT_EQ(unreg, closed) << "注销数少于关闭数 = 有 fd 还挂在某个 worker 的 interest list 上";
  EXPECT_EQ(harness.group()->live_connection_count(), 0u);

  for (auto& c : clients) c.close();
  harness.shutdown_all();
  const std::size_t fd_after = count_open_fds();
  std::printf("[S2-01 10000] %d worker 串行往返=%llu 耗时=%lldms fd: 起=%zu 终=%zu "
              "epoll 注册=%llu 注销=%llu\n",
              kWorkers, static_cast<unsigned long long>(kRequests), static_cast<long long>(ms),
              fd_before, fd_after, static_cast<unsigned long long>(reg),
              static_cast<unsigned long long>(unreg));
  EXPECT_EQ(fd_after, fd_before) << "10000 请求之后 fd 没回到基线：有连接没被真正关掉";
}

// 三、并发压力 + TSan 门禁。16 条线程各开一条连接、各发 50 条请求（16 下游 +
// 16 上游 = 16 ≤ 32 上限），真正并发地穿过 accept→handoff→转发→收尾。并发下上游
// 的全局 seq 不可预测，所以只断"200 + 体长 + seq= 前缀"（证明响应没串号/没截断），
// 精确比对交给用例二。**这条的价值在 tsan preset 下零 data race 报告。**
TEST(S2_01_MultiReactor, ConcurrentLoadAcrossWorkersIsRaceFreeAndCorrect) {
  constexpr int kWorkers = 4;
  constexpr int kThreads = 16;
  constexpr int kReqsPerThread = 50;
  GroupHarness harness;
  std::string why;
  GroupHarness::Options options;
  options.config = group_base_config(kWorkers);
  ASSERT_TRUE(harness.start(options, &why)) << why;

  std::atomic<int> failures{0};
  std::atomic<int> bad_responses{0};
  std::vector<std::thread> threads;
  threads.reserve(static_cast<std::size_t>(kThreads));
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      Client client;
      std::string w;
      if (!client.connect_to(harness.port(), &w)) {
        failures.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      for (int j = 0; j < kReqsPerThread; ++j) {
        const std::size_t seq = static_cast<std::size_t>(t * kReqsPerThread + j);
        if (!client.write_all(request_text(seq))) {
          failures.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        std::string status;
        std::string body;
        if (!client.read_response(&status, &body, &w)) {
          failures.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        if (status != "HTTP/1.1 200 OK" || body.size() != kBodyBytes ||
            body.rfind("seq=", 0) != 0) {
          bad_responses.fetch_add(1, std::memory_order_relaxed);
        }
      }
      client.shutdown_write();
      (void)client.wait_for_eof(&w);  // 内核级证据：网关把这侧关了
    });
  }
  for (auto& th : threads) th.join();

  EXPECT_EQ(failures.load(std::memory_order_relaxed), 0) << "有客户端连接/读写失败";
  EXPECT_EQ(bad_responses.load(std::memory_order_relaxed), 0) << "有响应串号或截断";

  ReactorStats& stats = harness.group()->stats();
  // S2-02（ADR-15）：16 条下游关闭后各自的上游**归还进池**（live==16），不再是 0；
  // 并发下晚启动的线程还可能借走早结束线程归还的热上游（pool_borrows>0），所以新建数
  // 不确定，但"16 次回收"与"最终全部析构"这两条不变。先确认回收，再 stop_loop 排空池。
  ASSERT_TRUE(wait_until(
      [&] { return stats.load(stats.pool_returns) >= static_cast<std::uint64_t>(kThreads); }, 5000))
      << "并发收尾后 16 条上游没有全部归还进池";
  const std::uint64_t parsed = stats.load(stats.requests_parsed);
  const std::uint64_t completed = stats.load(stats.responses_completed);
  EXPECT_EQ(parsed, static_cast<std::uint64_t>(kThreads) * kReqsPerThread);
  EXPECT_EQ(completed, parsed);

  harness.stop_loop();  // 排空池 → 全部上游析构
  const std::uint64_t constructed = stats.load(stats.connections_constructed);
  const std::uint64_t destructed = stats.load(stats.connections_destructed);
  EXPECT_EQ(constructed, destructed) << "排空池后构造与析构逐项相等";
  EXPECT_EQ(stats.load(stats.live_connections), 0u) << "并发负载收尾后仍有连接活着";
  std::printf("[S2-01 concurrent] %d 线程 × %d 请求穿过 %d worker：parsed=%llu 收尾干净\n",
              kThreads, kReqsPerThread, kWorkers, static_cast<unsigned long long>(parsed));

  harness.shutdown_all();
}

// 四、stop() 让 accept 循环与所有 worker 都退出，且靠 eventfd 自唤醒而不是等超时。
TEST(S2_01_MultiReactor, StopEndsAcceptLoopAndAllWorkers) {
  constexpr int kWorkers = 4;
  GroupHarness harness;
  std::string why;
  GroupHarness::Options options;
  options.config = group_base_config(kWorkers);
  options.with_upstream = false;
  ASSERT_TRUE(harness.start(options, &why)) << why;

  // 先把循环"用起来"再停：一条连接被派发注册，证明 accept 循环与至少一个 worker
  // 确实在跑（否则测的是空循环，stop 延迟再好看也没意义）。
  Client client;
  ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;
  ReactorStats& stats = harness.group()->stats();
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.downstream_accepted) == 1u; }, 3000))
      << "连接没被派发注册，worker 循环可能没起来";

  const auto started = std::chrono::steady_clock::now();
  harness.group()->stop();
  harness.stop_loop();  // join 返回 = accept 循环 + N worker 全部退出
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                            started)
          .count();
  EXPECT_LT(ms, 1000) << "stop 到全部线程退出超过 1s：eventfd 自唤醒没起作用，靠超时兜的";
  EXPECT_FALSE(harness.group()->running());
  EXPECT_TRUE(harness.group()->stop_requested());
  std::printf("[S2-01 stop] %d worker：stop() 到全部退出 %lld ms（wait_timeout_ms=50）\n",
              kWorkers, static_cast<long long>(ms));

  client.close();
  harness.shutdown_all();
}

// 五、handoff 探测读（ADR-14 第 5 条）的回归。ET 不为"注册之前已缓冲的字节"负责：
// fd 从 acceptor accept、排队、到 worker 注册这段窗口里，客户端可能已经把请求写进了
// 内核接收缓冲，而注册那一刻没有新跳变。worker 注册完补一次探测读把已缓冲的字节读
// 干净——少了它，"连上就立刻发"的连接会睡死到客户端再写一次为止。这里用 100 条
// "连接后立即发一条请求"最大化撞上那个窗口；若哪天探测被删掉且内核不在注册时投边，
// 其中若干条会读超时（红在 timeout 上，不是红在计数上）。
TEST(S2_01_MultiReactor, HandoffConnectionWithImmediateRequestIsServed) {
  constexpr int kWorkers = 4;
  constexpr int kIterations = 100;
  GroupHarness harness;
  std::string why;
  GroupHarness::Options options;
  options.config = group_base_config(kWorkers);
  ASSERT_TRUE(harness.start(options, &why)) << why;

  for (int i = 0; i < kIterations; ++i) {
    Client client;
    ASSERT_TRUE(client.connect_to(harness.port(), &why)) << "第 " << i << " 条连接：" << why;
    // 连接成功后**立刻**整条写出：字节很可能在 worker 注册这条 fd 之前就到达内核。
    ASSERT_TRUE(client.write_all(request_text(static_cast<std::size_t>(i) + 1)))
        << "第 " << i << " 条写不出去";
    std::string status;
    std::string body;
    ASSERT_TRUE(client.read_response(&status, &body, &why))
        << "第 " << i << " 条没拿到响应（探测读没把注册前已缓冲的请求读出来？）：" << why;
    ASSERT_EQ(status, "HTTP/1.1 200 OK") << "第 " << i;
    ASSERT_EQ(body.size(), kBodyBytes) << "第 " << i;
    client.close();
  }

  ReactorStats& stats = harness.group()->stats();
  EXPECT_EQ(stats.load(stats.requests_parsed), static_cast<std::uint64_t>(kIterations));
  EXPECT_EQ(stats.load(stats.downstream_accepted), static_cast<std::uint64_t>(kIterations));
  harness.shutdown_all();
}

// ===========================================================================
// S2-02：per-worker 上游连接池（ADR-15）
// ===========================================================================
//
// 池把上游连接数与下游**生命周期**解耦：下游关闭时它的上游归还进池、被下一条下游
// 借走复用，而不是随之销毁。这三条各钉一面：
//   一、核心价值——N 条短命下游只新建 1 条上游，其余全靠借池（churn 场景省握手）；
//   二、ADR-15 点名的边界——空闲池内上游被对端关闭是**正常回收**，绝不许误计成
//       upstream_truncated_before_head、也不许给一条没有下游的空闲上游发 502；
//   三、pool_max_idle 的硬上界——池满时归还改成直接关闭（每连接内存的闸门，SLO #2）。
// 池是 per-worker 的，多 worker 下的复用已由 S2_01 的 88/89 两条覆盖（它们现在也断
// pool_returns），所以这里用单 Reactor 把池机制本身测清楚。

// 一、N 条短命下游复用同一条热上游：这是池存在的意义。
TEST(S2_02_UpstreamPool, ShortLivedDownstreamsReuseOnePooledUpstream) {
  constexpr int kIterations = 20;
  Harness harness;
  std::string why;
  ASSERT_TRUE(harness.start(default_options(), &why)) << why;
  ReactorStats& stats = harness.reactor()->stats();

  for (int i = 1; i <= kIterations; ++i) {
    Client client;
    ASSERT_TRUE(client.connect_to(harness.port(), &why)) << "第 " << i << " 条连接：" << why;
    ASSERT_TRUE(client.write_all(request_text(static_cast<std::size_t>(i))));
    std::string status;
    std::string body;
    ASSERT_TRUE(client.read_response(&status, &body, &why)) << "第 " << i << " 条：" << why;
    ASSERT_EQ(status, "HTTP/1.1 200 OK");
    // 借来的热上游必须把请求/响应原样带过去：body 逐条精确比对（seq 是上游的全局计数，
    // 顺序驱动下第 i 条请求就是第 i 个被服务的）。
    ASSERT_EQ(body, expected_body(static_cast<std::size_t>(i))) << "第 " << i << " 条响应串号";
    // 关闭下游 → 网关看到 EOF → 下游关闭 → 上游归还进池。
    client.shutdown_write();
    ASSERT_TRUE(client.wait_for_eof(&why)) << "第 " << i << " 条：" << why;
    ASSERT_TRUE(wait_until(
        [&] { return stats.load(stats.pool_returns) == static_cast<std::uint64_t>(i); }, 3000))
        << "第 " << i << " 条下游关闭后上游没归还进池";
  }

  // 20 条短命下游，只新建了 1 条上游（第一条），第 2..20 条各借了一次池。
  EXPECT_EQ(stats.load(stats.downstream_accepted), static_cast<std::uint64_t>(kIterations));
  EXPECT_EQ(stats.load(stats.upstream_opened), 1u)
      << "池复用失效：N 条短命下游本该只新建 1 条上游，实际新建了更多";
  EXPECT_EQ(stats.load(stats.pool_borrows), static_cast<std::uint64_t>(kIterations - 1))
      << "第 2..N 条下游各借一次池";
  EXPECT_EQ(stats.load(stats.pool_returns), static_cast<std::uint64_t>(kIterations));
  EXPECT_EQ(stats.load(stats.requests_parsed), static_cast<std::uint64_t>(kIterations));
  std::printf("[S2-02 churn] %d 条短命下游复用 upstream_opened=%llu pool_borrows=%llu\n",
              kIterations, static_cast<unsigned long long>(stats.load(stats.upstream_opened)),
              static_cast<unsigned long long>(stats.load(stats.pool_borrows)));

  harness.stop_loop();  // 排空池 → 那条热上游析构
  // N 条下游对象 + 1 条上游对象 = N+1 构造，全部析构。
  EXPECT_EQ(stats.load(stats.connections_constructed), stats.load(stats.connections_destructed));
  EXPECT_EQ(stats.load(stats.live_connections), 0u);
  harness.shutdown_all();
}

// 二、空闲池内上游被对端关闭 = 正常回收，不是截断（ADR-15 点名的边界）。
TEST(S2_02_UpstreamPool, IdlePooledUpstreamClosedByPeerIsEvictedNotTruncation) {
  Harness harness;
  std::string why;
  ASSERT_TRUE(harness.start(default_options(), &why)) << why;
  ReactorStats& stats = harness.reactor()->stats();

  // 一条下游：请求 → 响应 → 关闭，上游被回收进池（此刻它空闲、未配对）。
  {
    Client client;
    ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;
    ASSERT_TRUE(client.write_all(request_text(1)));
    std::string status;
    std::string body;
    ASSERT_TRUE(client.read_response(&status, &body, &why)) << why;
    client.shutdown_write();
    ASSERT_TRUE(client.wait_for_eof(&why)) << why;
  }
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.pool_returns) == 1u; }, 3000))
      << "上游没归还进池";
  ASSERT_EQ(stats.load(stats.upstream_truncated_before_head), 0u);

  // 让上游对端关掉这条**空闲**连接：停掉测试上游（它会关掉所有客户端 fd）。
  harness.upstream().stop_and_join();
  // 网关 worker 读到空闲池内上游的 EOF → 淘汰（pool_evictions_eof），而不是截断表态。
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.pool_evictions_eof) == 1u; }, 3000))
      << "空闲池内上游被对端关闭，没走淘汰路径";

  EXPECT_EQ(stats.load(stats.upstream_truncated_before_head), 0u)
      << "空闲池内上游的正常关闭被误计成截断（ADR-15 点名禁止的边界）";
  EXPECT_EQ(stats.load(stats.gateway_replies_sent), 0u)
      << "给一条没有配对下游的空闲上游发了 502（它没有下游可表态）";

  harness.stop_loop();
  EXPECT_EQ(stats.load(stats.live_connections), 0u);
  EXPECT_EQ(stats.load(stats.connections_constructed), stats.load(stats.connections_destructed));
  harness.shutdown_all();
}

// 三、pool_max_idle 硬上界：池满时归还改成直接关闭。
TEST(S2_02_UpstreamPool, PoolFullClosesExcessInsteadOfReturning) {
  constexpr int kConns = 3;    // 3 > pool_max_idle=2
  Harness harness;
  std::string why;
  Harness::Options options;
  options.config = base_config();
  options.config.pool_max_idle = 2;
  ASSERT_TRUE(harness.start(options, &why)) << why;
  ReactorStats& stats = harness.reactor()->stats();

  // 3 条下游**同时**在飞（各建各的上游，池此刻是空的），全部拿到响应后再一起关。
  std::vector<Client> clients(static_cast<std::size_t>(kConns));
  for (int i = 0; i < kConns; ++i) {
    ASSERT_TRUE(clients[static_cast<std::size_t>(i)].connect_to(harness.port(), &why)) << why;
    ASSERT_TRUE(clients[static_cast<std::size_t>(i)].write_all(
        request_text(static_cast<std::size_t>(i) + 1)));
    std::string status;
    std::string body;
    ASSERT_TRUE(clients[static_cast<std::size_t>(i)].read_response(&status, &body, &why)) << why;
  }
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.upstream_opened) == kConns; }, 3000))
      << "3 条并发下游本该各建一条上游";

  // 一起关：前 2 条的上游归还进池，第 3 条撞上 pool_max_idle → 直接关闭。
  for (auto& c : clients) c.shutdown_write();
  for (auto& c : clients) {
    ASSERT_TRUE(c.wait_for_eof(&why)) << "下游没被网关关闭";
  }
  ASSERT_TRUE(wait_until(
      [&] {
        return stats.load(stats.pool_returns) == 2u && stats.load(stats.pool_evictions_full) == 1u;
      },
      3000))
      << "池满时第 3 条上游没走 evictions_full（pool_max_idle 的上界没生效）";
  EXPECT_EQ(stats.load(stats.pool_returns), 2u);
  EXPECT_EQ(stats.load(stats.pool_evictions_full), 1u);

  for (auto& c : clients) c.close();
  harness.stop_loop();  // 排空池（那 2 条）
  EXPECT_EQ(stats.load(stats.live_connections), 0u);
  EXPECT_EQ(stats.load(stats.connections_constructed), stats.load(stats.connections_destructed));
  harness.shutdown_all();
}

// ===========================================================================
// S2-03：跨线程传递与异步销毁的正确性 —— TSan 门禁正面攻坚（ADR-14 销毁协议定稿）
// ===========================================================================
//
// per-worker 池（ADR-15）让"连接被非 owner 线程持有"这类 harder 竞态**没有出现**（所有
// shared_ptr 持有者——注册表/retiring_/探测队列/上游池——都在同一个 worker 线程内，跨
// 线程只流动裸 fd）。所以这里攻的是**仍然存在的两个跨线程面**：
//   (a) stop 与活跃负载并发：acceptor 正在 dispatch、worker 正在处理、客户端正在连时停；
//   (c) handoff 队列里的 fd 在 worker 取出注册之前，对端就已经关了。
// 断言的是**不变量**（fd 回基线、不挂死、sanitizer 干净），不是精确计数（churn 下计数本就抖）。
// 每条用例的真正门禁在 tsan/asan preset：race 让 tsan 红、UAF/泄漏让 asan/LSan 红——而
// "连接只在 owner worker 的 retire 点析构"这条 ADR-14 断言，可证伪的形式就是它们在 churn
// 下零报告（若非 owner 线程碰了连接或它的析构，tsan 当场抓）。

// (a) 负载活跃时停网关：连接在 acceptor→handoff→worker→转发→EOF→关闭 全链路上被并发创建销毁。
TEST(S2_03_CrossThreadDestruction, StopDuringActiveChurnShutsDownCleanly) {
  constexpr int kWorkers = 4;
  constexpr int kClientThreads = 12;
  const std::size_t fd_before = count_open_fds();
  GroupHarness harness;
  std::string why;
  GroupHarness::Options options;
  options.config = group_base_config(kWorkers);
  ASSERT_TRUE(harness.start(options, &why)) << why;

  std::atomic<bool> client_stop{false};
  std::atomic<long> churn{0};
  std::vector<std::thread> threads;
  threads.reserve(static_cast<std::size_t>(kClientThreads));
  for (int t = 0; t < kClientThreads; ++t) {
    threads.emplace_back([&, t] {
      while (!client_stop.load(std::memory_order_relaxed)) {
        Client client;
        std::string w;
        if (!client.connect_to(harness.port(), &w)) break;  // 网关停了、listener 关了 → 连不上 → 收工
        // 发一条就立刻关（**不读响应**）：制造快速 churn，且客户端永不阻塞在 read 上，
        // 停止时收尾快（不撞 SO_RCVTIMEO 的 5s）。网关这侧照样走完 accept→handoff→注册→
        // 探测读→转发→（客户端已关）写失败→关闭 的全链路。
        (void)client.write_all(request_text(static_cast<std::size_t>(t) + 1));
        client.close();
        churn.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  harness.group()->stop();  // **负载活跃时**停网关：这就是 (a) 的竞态窗口
  client_stop.store(true, std::memory_order_relaxed);
  for (auto& th : threads) th.join();
  harness.stop_loop();  // join run 线程（accept 循环退出 → stop+join 全部 4 worker）

  ReactorStats& stats = harness.group()->stats();
  const std::uint64_t accepted = stats.load(stats.downstream_accepted);
  const std::uint64_t constructed = stats.load(stats.connections_constructed);
  EXPECT_GT(accepted, 0u) << "churn 期间一条连接都没接进来，这条用例没测到东西";
  EXPECT_GT(churn.load(), 0L);
  std::printf("[S2-03 stop-under-load] churn=%ld 次连接、accepted=%llu、constructed=%llu，停止未挂死\n",
              churn.load(), static_cast<unsigned long long>(accepted),
              static_cast<unsigned long long>(constructed));

  // 在飞连接按 S1 停止语义不排水（排水归 S2-05），它们由 ~Reactor 关闭；所以这里查的是
  // "析构之后 fd 回基线"——它同时证明了 handoff 队列里没被取走的 fd（UniqueFd）也关掉了。
  harness.shutdown_all();
  EXPECT_EQ(count_open_fds(), fd_before)
      << "停止 + 析构后 fd 没回基线：在飞连接或 handoff 队列里的 fd 漏了";
}

// (c) handoff 队列里的 fd 在 worker 注册前对端就关了：worker 取出 → 构造 → 注册 → 探测读到
// EOF → 干净关闭。300 次"连上立刻关"最大化撞上那个窗口（fd 还在队列里、对端已 FIN）。
TEST(S2_03_CrossThreadDestruction, HandoffFdClosedByPeerBeforeRegistrationIsHandled) {
  constexpr int kWorkers = 4;
  constexpr int kIterations = 300;
  const std::size_t fd_before = count_open_fds();
  GroupHarness harness;
  std::string why;
  GroupHarness::Options options;
  options.config = group_base_config(kWorkers);
  options.with_upstream = false;  // 不发请求、不需要上游；专测 handoff 后立刻 EOF 的收尾
  ASSERT_TRUE(harness.start(options, &why)) << why;

  for (int i = 0; i < kIterations; ++i) {
    Client client;
    ASSERT_TRUE(client.connect_to(harness.port(), &why)) << "第 " << i << " 条：" << why;
    client.close();  // 立刻关：fd 很可能还在 handoff 队列里、worker 还没注册，对端就 FIN 了
  }
  ReactorStats& stats = harness.group()->stats();
  // worker 把队列里的 fd 取出、注册、探测读到 EOF、关闭——最终全部析构。等的不是"有没有关"
  // （客户端 close 是内核级证据），而是每条被接受连接的收尾有没有走完。
  ASSERT_TRUE(wait_until(
      [&] {
        return stats.load(stats.connections_constructed) == stats.load(stats.connections_destructed);
      },
      5000))
      << "立刻关闭的连接没被干净析构：handoff 后 EOF 的收尾路径漏了某一步";
  EXPECT_GT(stats.load(stats.downstream_accepted), 0u);

  harness.stop_loop();
  EXPECT_EQ(stats.load(stats.live_connections), 0u);
  harness.shutdown_all();
  EXPECT_EQ(count_open_fds(), fd_before) << "fd 没回基线";
}

// 连接风暴（有序收尾）：16 线程各做 30 轮"连→发→收→半关→等 EOF→关"，跨 4 worker。
// 与 (a) 的区别是这条**不在负载中停**，所以能断精确的收尾不变量（constructed==destructed、
// fd 回基线）；churn 的并发面由它和 (a) 一起压，race 由 tsan preset 兜。
TEST(S2_03_CrossThreadDestruction, ConnectDisconnectStormIsRaceFreeAndLeavesNothingAlive) {
  constexpr int kWorkers = 4;
  constexpr int kThreads = 16;
  constexpr int kPerThread = 30;
  const std::size_t fd_before = count_open_fds();
  GroupHarness harness;
  std::string why;
  GroupHarness::Options options;
  options.config = group_base_config(kWorkers);
  ASSERT_TRUE(harness.start(options, &why)) << why;

  std::atomic<int> ok{0};
  std::vector<std::thread> threads;
  threads.reserve(static_cast<std::size_t>(kThreads));
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&] {
      for (int j = 0; j < kPerThread; ++j) {
        Client client;
        std::string w;
        if (!client.connect_to(harness.port(), &w)) continue;
        if (!client.write_all(request_text(static_cast<std::size_t>(j) + 1))) {
          client.close();
          continue;
        }
        std::string status;
        std::string body;
        if (client.read_response(&status, &body, &w) && status == "HTTP/1.1 200 OK") {
          ok.fetch_add(1, std::memory_order_relaxed);
        }
        client.shutdown_write();
        (void)client.wait_for_eof(&w);  // 让网关把这侧关掉（触发上游回收）再走
        client.close();
      }
    });
  }
  for (auto& th : threads) th.join();

  ReactorStats& stats = harness.group()->stats();
  EXPECT_EQ(ok.load(std::memory_order_relaxed), kThreads * kPerThread) << "有请求没拿到 200";

  harness.stop_loop();  // 排空池 → 全部上游析构
  EXPECT_EQ(stats.load(stats.live_connections), 0u) << "风暴收尾后仍有连接活着";
  EXPECT_EQ(stats.load(stats.connections_constructed), stats.load(stats.connections_destructed))
      << "构造与析构不逐项相等：churn 下有连接对象漏析构";
  EXPECT_GT(stats.load(stats.pool_borrows), 0u) << "16 线程churn 下池一次都没被借过（复用没发生）";
  harness.shutdown_all();
  EXPECT_EQ(count_open_fds(), fd_before) << "fd 没回基线";
}

// ===========================================================================
// S2-04：请求-响应配对模型 —— pipeline + Connection: close 的兑现时机（ADR-16）
// ===========================================================================
//
// S1-07 的债：一批 pipelined 请求里带 `Connection: close` 的那条会在**第一条响应**完结时
// 就兑现（400 条只回 1 条）。ADR-16 把它修正到"close 在它自己那条请求的响应之后兑现"，
// 靠的是给每条配对加一个在飞请求计数（转发 +1 / 响应完结 −1），close 请求是最后转发的
// 那条，所以"在飞归零"就是它的响应完结点。这三条分别打：close 在批次中间、close 在最后、
// 整批无 close。

// close 在 pipeline 中间：r1 keep-alive、r2 close、r3/r4 跟在 close 之后。
// 修复前 → 客户端只读得到 1 条响应（r1 完结就关）。修复后 → r1、r2 两条都完整回来，
// close 兑现后连接收尾，r3/r4 因落在 close 边界之后不回源、随关闭丢弃（客户端违规）。
TEST(S2_04_Pairing, CloseInMiddleOfPipelineDeliversUpToAndIncludingThatResponse) {
  Harness harness;
  std::string why;
  ASSERT_TRUE(harness.start(default_options(), &why)) << why;
  ReactorStats& stats = harness.reactor()->stats();

  Client client;
  ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;
  const std::string r1 = request_text(1);
  const std::string r2 = close_request_text(2);  // 这条带 Connection: close
  const std::string batch = r1 + r2 + request_text(3) + request_text(4);
  ASSERT_TRUE(client.write_all(batch)) << "整批 pipeline 写不出去";

  std::string status;
  std::string body;
  ASSERT_TRUE(client.read_response(&status, &body, &why)) << "r1 响应：" << why;
  EXPECT_EQ(status, "HTTP/1.1 200 OK");
  // 关键：close 那条（r2）的响应也要完整回来——这正是 S2-04 前会被提前砍掉的第二条。
  ASSERT_TRUE(client.read_response(&status, &body, &why))
      << "r2（close 请求）的响应没回来，close 仍兑现得太早：" << why;
  EXPECT_EQ(status, "HTTP/1.1 200 OK");
  // 之后配对收尾：客户端读到 EOF。
  ASSERT_TRUE(client.wait_for_eof(&why)) << "close 兑现后连接没关：" << why;

  ASSERT_TRUE(wait_until([&] { return stats.load(stats.responses_completed) == 2u; }, 3000))
      << "该兑现 2 条响应（r1、r2）";
  EXPECT_EQ(stats.load(stats.requests_parsed), 2u)
      << "close 之后的 r3/r4 不该被解析/转发（落在 close 边界后，随关闭丢弃）";
  EXPECT_EQ(stats.load(stats.bytes_down_to_upstream),
            static_cast<std::uint64_t>(r1.size() + r2.size()))
      << "只该把 r1、r2 两条转给上游";
  EXPECT_EQ(stats.load(stats.close_headers_honored), 1u);
  harness.shutdown_all();
}

// close 在 pipeline 末尾：r1/r2/r3 全 keep-alive 语义、只有最后一条 r3 带 close。
// 三条响应都要完整回来，之后才关。这条锁"在飞计数对每一条都 −1、只在最后归零时关"。
TEST(S2_04_Pairing, CloseAtEndOfPipelineDeliversEveryResponseThenCloses) {
  constexpr int kN = 3;
  Harness harness;
  std::string why;
  ASSERT_TRUE(harness.start(default_options(), &why)) << why;
  ReactorStats& stats = harness.reactor()->stats();

  Client client;
  ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;
  const std::string batch =
      request_text(1) + request_text(2) + close_request_text(3);  // close 在最后
  ASSERT_TRUE(client.write_all(batch));

  for (int i = 1; i <= kN; ++i) {
    std::string status;
    std::string body;
    ASSERT_TRUE(client.read_response(&status, &body, &why)) << "第 " << i << " 条响应没回来：" << why;
    EXPECT_EQ(status, "HTTP/1.1 200 OK") << "第 " << i << " 条";
  }
  ASSERT_TRUE(client.wait_for_eof(&why)) << "close（最后一条）的响应回来后连接才该关：" << why;
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.responses_completed) == kN; }, 3000));
  EXPECT_EQ(stats.load(stats.requests_parsed), static_cast<std::uint64_t>(kN));
  harness.shutdown_all();
}

// 整批无 close：全部转发、全部回响应、连接不因 close 逻辑被提前关。这条是 ADR-16 的
// 负向守卫——证明"close 兑现"只在真有 close 时才砍管线，在飞计数不会误伤普通 pipeline。
TEST(S2_04_Pairing, PipelineWithoutCloseDeliversAllAndKeepsConnectionOpen) {
  constexpr int kN = 4;
  Harness harness;
  std::string why;
  ASSERT_TRUE(harness.start(default_options(), &why)) << why;
  ReactorStats& stats = harness.reactor()->stats();

  Client client;
  ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;
  std::string batch;
  for (int i = 1; i <= kN; ++i) batch += request_text(static_cast<std::size_t>(i));
  ASSERT_TRUE(client.write_all(batch));

  for (int i = 1; i <= kN; ++i) {
    std::string status;
    std::string body;
    ASSERT_TRUE(client.read_response(&status, &body, &why)) << "第 " << i << " 条：" << why;
    EXPECT_EQ(status, "HTTP/1.1 200 OK") << "第 " << i << " 条";
    EXPECT_EQ(body, expected_body(static_cast<std::size_t>(i))) << "第 " << i << " 条串号";
  }
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.responses_completed) == kN; }, 3000));
  EXPECT_EQ(stats.load(stats.requests_parsed), static_cast<std::uint64_t>(kN))
      << "无 close 时整批都该被解析转发";
  EXPECT_EQ(stats.load(stats.close_headers_honored), 0u) << "没有 close 却兑现了关闭意图";

  // 连接还活着（没被 close 逻辑误砍）：再发一条仍能拿到响应。
  ASSERT_TRUE(client.write_all(request_text(99)));
  std::string status;
  std::string body;
  ASSERT_TRUE(client.read_response(&status, &body, &why)) << "close 之后连接被误关了：" << why;
  EXPECT_EQ(status, "HTTP/1.1 200 OK");
  harness.shutdown_all();
}

// ===========================================================================
// S2-05：优雅停止（排水）。ARCHITECTURE 1.20。
//
// 这组用例各自钉住裁决里的一句话，而每一条的"能红"形态都写在注释里：
//   一、在飞请求被逐条答完才退出（不是被砍掉）——用**停读上游**造出一个"请求已转发、
//        响应还没来"的状态，然后才下排水指令；响应必须在排水里送达客户端。
//   二、空闲连接立即关（不等兜底）——兜底给到 60s，退出却必须在几十毫秒级，
//        时间差本身就是判据（比"等一个 100ms 看它关没关"要硬）。
//   三、兜底强关（卡死的客户端不许无限拖住下线）——半条请求既不是空闲也没有在飞，
//        只能等到 deadline；断言 elapsed 落在 deadline 之后，才算兜底真的在守时限。
//   四、隧道只能硬关（ADR-09）——钉两件事：配对是被**硬关分支**收掉的（不是挂到兜底
//        才关的：隧道的在飞计数永不为零，因为 101 不走 pump_response 的 kComplete，
//        所以"隧道先于在飞判据"是承重结构，摘掉那条分支这两个 EXPECT 一起红），
//        以及 EOF 之前**一个额外字节都不许收到**（收到任何东西都是网关在伪造它从没
//        解析过的东西）。"硬关之前先刷完已缓冲字节"这句话不在本用例的可证伪范围里，
//        归 S1-04 的 UpstreamBreakClosesBothSidesAfterFlushingResponse（那条能红）；
//        我试过把载荷抬到 3 MiB 想把它钉住，结果正确实现也整段送不完，理由写在用例里。
//   五、排水期间新连接不被服务（accept 层就拒），且有一份可 grep 的计数。
//   六、多 worker 协调：每条 worker 各自排干、各自置 stop_requested_，accept 循环
//        读到"全都在停止状态"才 join——这条测的是协调，不是单循环的收尾。
// ===========================================================================

TEST(S2_05_GracefulStop, InFlightRequestIsAnsweredAfterTheDrainStarts) {
  Harness harness;
  std::string why;
  ASSERT_TRUE(harness.start(default_options(), &why)) << why;
  ReactorStats& stats = harness.reactor()->stats();

  // 上游一个字节都不读（S1-07 的原子开关）：请求会被网关转发出去、卡在"已接受、
  // 没响应"那一格，这正是排水要等的那一格。
  harness.upstream().set_stall_reads(true);

  Client client;
  ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;
  const std::string req = request_text(1);
  ASSERT_TRUE(client.write_all(req)) << "请求写不出去";
  // 先等**前置条件**成立（请求已经排进上游出向 = 已被接受）。这里不是拿计数当锁：
  // 万一排水抢在这之前就到了，那条请求会被判成"尚未接受"而根本不该有响应，
  // 于是下面那次 read_response 直接红——用例不会为不成立的理由通过。
  ASSERT_TRUE(wait_until(
      [&] { return stats.load(stats.bytes_down_to_upstream) == static_cast<std::uint64_t>(req.size()); },
      3000))
      << "请求没转到上游出向，前置条件不成立";

  const auto started = std::chrono::steady_clock::now();
  harness.reactor()->request_drain(60000);  // 兜底给到一分钟：走兜底就是这条用例红了
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.drain_in_flight_at_entry) == 1u; }, 3000))
      << "排水的第一次扫描没抓到那条在飞请求";

  // 放开上游。响应必须在**排水期间**送完，而不是被兜底砍掉。
  harness.upstream().set_stall_reads(false);

  std::string status;
  std::string body;
  ASSERT_TRUE(client.read_response(&status, &body, &why))
      << "排水期间在飞的请求没有被逐条答完（这是裁决第一条的反面）：" << why;
  EXPECT_EQ(status, "HTTP/1.1 200 OK");
  EXPECT_EQ(body, expected_body(1));
  ASSERT_TRUE(client.wait_for_eof(&why)) << "答完之后连接没收尾：" << why;

  harness.join_loop();  // 排水完成后 run() 自己返回
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - started)
                              .count();

  EXPECT_EQ(stats.load(stats.drain_forced_timeout), 0u)
      << "兜底给的是 60000ms，而 elapsed=" << elapsed_ms << "ms——真走了兜底就不该这么快";
  EXPECT_EQ(stats.load(stats.responses_completed), 1u);
  EXPECT_EQ(stats.load(stats.drain_closed_idle), 1u) << "答完之后它就成空闲下游，该被立即关";
  EXPECT_EQ(stats.load(stats.drain_requested), 1u);
  EXPECT_LT(elapsed_ms, 30000) << "elapsed=" << elapsed_ms << "ms，兜底 60000ms 没被用上才对";
  harness.stop_loop();  // 已经退出了，这一句只是幂等兜底
  EXPECT_EQ(harness.reactor()->live_connection_count(), 0u) << "排水退出后注册表必须是空的";
  EXPECT_EQ(stats.load(stats.connections_constructed), stats.load(stats.connections_destructed))
      << "下游+配对上游（或进池的上游）没走完成六步关闭";
  std::printf("[S2-05 in-flight] 排水期间答完 1 条在飞请求，elapsed=%lldms，兜底 0 次\n",
              static_cast<long long>(elapsed_ms));
  harness.shutdown_all();
}

TEST(S2_05_GracefulStop, IdleConnectionsAreClosedAtOnceNotAtTheBackstop) {
  constexpr int kIdle = 3;
  Harness harness;
  std::string why;
  Harness::Options options = default_options();
  options.with_upstream = false;  // 空闲连接不发请求，不该需要上游
  ASSERT_TRUE(harness.start(options, &why)) << why;
  ReactorStats& stats = harness.reactor()->stats();

  std::vector<Client> clients(kIdle);
  for (int i = 0; i < kIdle; ++i) {
    ASSERT_TRUE(clients[static_cast<std::size_t>(i)].connect_to(harness.port(), &why)) << why;
  }
  ASSERT_TRUE(wait_until(
      [&] { return stats.load(stats.downstream_accepted) == static_cast<std::uint64_t>(kIdle); },
      3000))
      << "3 条连接没全部建起来（内核级证据是 connect 已经成功）";

  const auto started = std::chrono::steady_clock::now();
  harness.reactor()->request_drain(60000);
  for (auto& c : clients) {
    ASSERT_TRUE(c.wait_for_eof(&why)) << "空闲连接没被立即关：" << why;
  }
  harness.join_loop();
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - started)
                              .count();

  // 判据是**时间差**：兜底写着 60000ms，而三条空闲连接几十毫秒内就该走完。
  // 拿"等 100ms 看关没关"当断言会随机器快慢变形，这个等式不会。
  EXPECT_LT(elapsed_ms, 5000) << "空闲连接没立即关、而是等到兜底才走：elapsed=" << elapsed_ms << "ms";
  EXPECT_EQ(stats.load(stats.drain_closed_idle), static_cast<std::uint64_t>(kIdle));
  EXPECT_EQ(stats.load(stats.drain_closed_buffered), 0u);
  EXPECT_EQ(stats.load(stats.drain_forced_timeout), 0u);
  harness.stop_loop();
  EXPECT_EQ(harness.reactor()->live_connection_count(), 0u);
  EXPECT_EQ(stats.load(stats.connections_constructed), stats.load(stats.connections_destructed));
  std::printf("[S2-05 idle] %d 条空闲连接立即关，elapsed=%lldms（兜底 60000ms 没用上）\n", kIdle,
              static_cast<long long>(elapsed_ms));
  harness.shutdown_all();
}

TEST(S2_05_GracefulStop, StuckHalfRequestIsForceClosedAtTheDeadline) {
  constexpr int kDeadlineMs = 400;
  Harness harness;
  std::string why;
  ASSERT_TRUE(harness.start(default_options(), &why)) << why;
  ReactorStats& stats = harness.reactor()->stats();

  Client client;
  ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;
  // 半条请求：头部区没有 CRLFCRLF，永远拼不完整。它既不是空闲（缓冲里有字节）、
  // 也没有在飞请求（没被接受过），所以按口径只能交给兜底——这条测的就是兜底。
  const std::string half = "GET /x HTTP/1.1\r\nHost: gateway.test\r\nX: partial";
  ASSERT_TRUE(client.write_all(half));
  ASSERT_TRUE(wait_until(
      [&] { return stats.load(stats.bytes_read_downstream) == static_cast<std::uint64_t>(half.size()); },
      3000))
      << "半条请求没被网关读进去，前置条件不成立";
  EXPECT_EQ(stats.load(stats.requests_parsed), 0u) << "半包不该被算成一条请求";

  const auto started = std::chrono::steady_clock::now();
  harness.reactor()->request_drain(kDeadlineMs);
  ASSERT_TRUE(client.wait_for_eof(&why)) << "兜底到点没把这条卡死的连接强关：" << why;
  harness.join_loop();
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - started)
                              .count();

  // 兜底是**时限**不是"尽快"：早于 deadline 就关，等于无声丢掉一条还在路上的连接。
  EXPECT_GE(elapsed_ms, kDeadlineMs - 50)
      << "比 deadline 还早就强关了：elapsed=" << elapsed_ms << "ms, deadline=" << kDeadlineMs << "ms";
  EXPECT_LT(elapsed_ms, 5000) << "拖到 5s 才走，兜底没守住时限：elapsed=" << elapsed_ms << "ms";
  EXPECT_EQ(stats.load(stats.drain_forced_timeout), 1u);
  EXPECT_EQ(stats.load(stats.drain_closed_idle), 0u) << "这条不空闲（缓冲里有半包），不该落进空闲格";
  EXPECT_EQ(stats.load(stats.drain_closed_buffered), 0u)
      << "半包不是「一条完整的、未被接受的请求」，它走的是兜底格而不是拒绝格";
  EXPECT_EQ(stats.load(stats.responses_completed), 0u);
  EXPECT_EQ(stats.load(stats.gateway_replies_sent), 0u) << "兜底强关不替客户端编一条错误响应";
  harness.stop_loop();
  EXPECT_EQ(harness.reactor()->live_connection_count(), 0u);
  EXPECT_EQ(stats.load(stats.connections_constructed), stats.load(stats.connections_destructed));
  std::printf("[S2-05 backstop] 半包连接在 deadline=%dms 之后被强关，elapsed=%lldms\n", kDeadlineMs,
              static_cast<long long>(elapsed_ms));
  harness.shutdown_all();
}

TEST(S2_05_GracefulStop, TunnelIsHardClosedWithoutAFabricatedCloseHandshake) {
  Harness harness;
  std::string why;
  ASSERT_TRUE(harness.start(default_options(), &why)) << why;
  ReactorStats& stats = harness.reactor()->stats();

  Client client;
  ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;
  ASSERT_TRUE(client.write_all(upgrade_request_text()));
  std::string head;
  ASSERT_TRUE(client.read_head(&head, &why)) << "101 没有回来：" << why;
  EXPECT_EQ(head.find("HTTP/1.1 101"), 0u);
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.upgrades_101) == 1u; }, 3000))
      << "隧道没建立，这条用例测不到隧道的收尾";

  // 帧一走一次完整往返（内核级证据：字节已经回到客户端手里），证明隧道是活的。
  const std::string frame1("\x81\x83\x37\xfa\x76", 5);
  ASSERT_TRUE(client.write_all(frame1));
  std::string echoed1;
  ASSERT_TRUE(client.read_exact(&echoed1, frame1.size(), &why)) << "帧一没有原样回显：" << why;
  EXPECT_EQ(echoed1, frame1);

  // 帧二刻意**不先读**：它要作为"排水那一刻手上还有货"的那一段被刷出去。
  // 载荷取 4096 字节是有意的上限——我先试过 3 MiB（想让它一定压在网关出向里），
  // 结果**正确实现也整段送不完**：配对开始收尾后上游侧的 EPOLLIN 已摘，还压在
  // socket 里的那段没人再读。所以"先刷完已缓冲字节再关"这句话不在本用例的可证伪
  // 范围里（它归 S1-04 的 UpstreamBreakClosesBothSidesAfterFlushingResponse，那条能红）；
  // 本用例钉的是另外两件事，写在下面两条 EXPECT 上，我不给它们配大于其证据的说法。
  std::string frame2(4096, '\x5a');
  frame2[0] = '\x82';  // 让它一眼就不是 HTTP（二进制帧头的形状）
  const std::uint64_t want_up =
      static_cast<std::uint64_t>(head.size() + frame1.size() + frame2.size());
  ASSERT_TRUE(client.write_all(frame2));
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.bytes_tunnel_up) >= want_up; }, 5000))
      << "帧二的回显没进网关，前置条件不成立（这一条就没东西可刷）";

  // 兜底给 1200ms：隧道的在飞计数**永远不会归零**（101 不走 pump_response 的
  // kComplete，ADR-16 那个 −1 对它从不发生），所以扫描里"隧道先于在飞判据分流"是
  // 承重的——把那条分支摘掉，这一对就只能挂到兜底，于是下面两个 EXPECT 一起变红
  // （forced=1、hard_closed=0）。这是我给这条用例找到的可证伪形式。
  harness.reactor()->request_drain(1200);

  std::string echoed2;
  ASSERT_TRUE(client.read_exact(&echoed2, frame2.size(), &why))
      << "帧二没能送达：" << why;
  EXPECT_EQ(echoed2, frame2) << "透传的字节被改写了：ADR-09 的红线";
  // 之后**一个额外字节都不许收到**：收到任何东西（尤其"看起来像 WS close 握手"的字节）
  // 就是网关在伪造它从来没有解析过的东西（ADR-09）。红线是伪造，不是粗糙。
  ASSERT_TRUE(client.wait_eof_or_data(&why))
      << "刷完之后不是干净的 socket 关闭，而是多发了字节：" << why;

  harness.join_loop();
  EXPECT_EQ(stats.load(stats.drain_tunnels_hard_closed), 1u) << "隧道配对没走硬关这条路";
  EXPECT_EQ(stats.load(stats.drain_forced_timeout), 0u)
      << "它是被硬关分支收掉的，不是挂到兜底才被关的（隧道的在飞永不为零，靠的就是那条分支）";
  EXPECT_EQ(stats.load(stats.upgrades_101), 1u);
  EXPECT_EQ(stats.load(stats.requests_parsed), 1u) << "帧字节一次都不该进解析器（ADR-09 复查）";
  EXPECT_EQ(stats.load(stats.drain_closed_idle), 0u) << "隧道不是「空闲下游」，它落硬关那一格";
  harness.stop_loop();
  EXPECT_EQ(harness.reactor()->live_connection_count(), 0u);
  EXPECT_EQ(stats.load(stats.connections_constructed), stats.load(stats.connections_destructed));
  std::printf("[S2-05 tunnel] 硬关分支收掉配对（没走兜底），EOF 前零个伪造字节，帧二 4096 字节逐字节相同\n");
  harness.shutdown_all();
}

TEST(S2_05_GracefulStop, NewConnectionsDuringDrainAreRefusedAndCounted) {
  Harness harness;
  std::string why;
  ASSERT_TRUE(harness.start(default_options(), &why)) << why;
  ReactorStats& stats = harness.reactor()->stats();

  // 用一条在飞请求把排水窗口撑开（上游停读 → 响应回不来 → 循环还在排）。
  harness.upstream().set_stall_reads(true);
  Client first;
  ASSERT_TRUE(first.connect_to(harness.port(), &why)) << why;
  const std::string req = request_text(1);
  ASSERT_TRUE(first.write_all(req));
  ASSERT_TRUE(wait_until(
      [&] { return stats.load(stats.bytes_down_to_upstream) == static_cast<std::uint64_t>(req.size()); },
      3000))
      << "第一条请求没被接受，窗口撑不起来";
  harness.reactor()->request_drain(60000);
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.drain_in_flight_at_entry) == 1u; }, 3000))
      << "排水没进入扫描，窗口还没开";

  // 窗口开着：新连接应当**被拒**，而且是"连上就被关"而不是"接进来挂着不理"。
  // 监听 fd 按设计留到析构（ARCHITECTURE 1.20 的 fd_start==fd_end 那段），所以
  // connect() 本身会成功——判据在服务层：一条响应都不许有，且连接被关掉。
  Client late;
  ASSERT_TRUE(late.connect_to(harness.port(), &why))
      << "connect 都失败了？监听 fd 是留到析构的，这里应当能连上";
  ASSERT_TRUE(late.write_all(request_text(2)));
  // 被拒的形态有两种，都算"没被服务就拆掉"：FIN（read()==0）与 RST（ECONNRESET，
  // errno 104）。RST 是**内核**对我"写了字节而对端关 socket 时那些字节还没被读走"
  // 的回答，不是网关发的响应；把它当成失败会把一条正确的行为读成 bug。
  // 只有"收到字节"或"一直挂着没人理"才是这条用例要红的两种形态。
  char probe[64];
  const ssize_t got = ::read(late.fd(), probe, sizeof(probe));
  if (got > 0) {
    FAIL() << "排水期间的新连接被服务了：收到 " << got << " 字节";
  }
  if (got < 0 && errno != ECONNRESET && errno != EAGAIN && errno != EWOULDBLOCK) {
    FAIL() << "read errno=" << errno << "（预期 0=FIN 或 104=RST）";
  }
  if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
    FAIL() << "读超时：这条新连接既没被拒也没被关，就是挂着没人理";
  }
  EXPECT_EQ(stats.load(stats.requests_parsed), 1u) << "被拒的请求不该进解析器、更不该回源";
  EXPECT_EQ(stats.load(stats.gateway_replies_sent), 0u) << "网关不替正在下线的自己伪造 5xx";

  // 放开上游，让第一条在飞请求答完，排水才收得干净。
  harness.upstream().set_stall_reads(false);
  std::string status;
  std::string body;
  ASSERT_TRUE(first.read_response(&status, &body, &why)) << "在飞那条没被答完：" << why;
  harness.join_loop();
  EXPECT_GE(stats.load(stats.drain_rejected_new_connections), 1u)
      << "「停止接受新连接」没有可断言的形态——只剩我一句「我拒了」";
  EXPECT_EQ(stats.load(stats.downstream_accepted), 1u) << "被拒的连接不该构造 Connection 对象";
  harness.stop_loop();
  EXPECT_EQ(harness.reactor()->live_connection_count(), 0u);
  EXPECT_EQ(stats.load(stats.connections_constructed), stats.load(stats.connections_destructed));
  std::printf("[S2-05 refuse] 排水期间新连接被拒 %llu 条，requests_parsed 停在 1\n",
              static_cast<unsigned long long>(stats.load(stats.drain_rejected_new_connections)));
  harness.shutdown_all();
}

TEST(S2_05_GracefulStop, GroupDrainQuiescesEveryWorkerAndDeliversEveryInFlightResponse) {
  constexpr int kWorkers = 3;
  constexpr int kClients = 6;  // 6 下游 + 6 上游 = 12 ≤ TestUpstream 的 32 客户端上限
  GroupHarness harness;
  std::string why;
  GroupHarness::Options options;
  options.config = group_base_config(kWorkers);
  ASSERT_TRUE(harness.start(options, &why)) << why;
  ReactorStats& stats = harness.group()->stats();

  harness.upstream().set_stall_reads(true);
  std::vector<Client> clients(static_cast<std::size_t>(kClients));
  for (int i = 0; i < kClients; ++i) {
    ASSERT_TRUE(clients[static_cast<std::size_t>(i)].connect_to(harness.port(), &why)) << why;
    ASSERT_TRUE(clients[static_cast<std::size_t>(i)].write_all(
        request_text(static_cast<std::size_t>(i) + 1)))
        << "第 " << i << " 条请求写不出去";
  }
  ASSERT_TRUE(wait_until(
      [&] { return stats.load(stats.requests_parsed) == static_cast<std::uint64_t>(kClients); },
      5000))
      << "6 条请求没有全部被接受（前置条件），用例撑不起排水";

  harness.group()->request_drain(60000);
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.drain_in_flight_at_entry) == static_cast<std::uint64_t>(kClients); },
                          5000))
      << "各 worker 的第一次扫描加起来没抓到 6 条在飞请求（跨 worker 聚合口径错了）";
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.drain_requested) == static_cast<std::uint64_t>(kWorkers); },
                          5000))
      << "不是每条 worker 都进入了排水";

  harness.upstream().set_stall_reads(false);
  for (int i = 0; i < kClients; ++i) {
    std::string status;
    std::string body;
    ASSERT_TRUE(clients[static_cast<std::size_t>(i)].read_response(&status, &body, &why))
        << "worker 上的第 " << i << " 条在飞请求没被答完：" << why;
    EXPECT_EQ(status, "HTTP/1.1 200 OK");
    ASSERT_TRUE(clients[static_cast<std::size_t>(i)].wait_for_eof(&why))
        << "答完之后这条没关：" << why;
  }
  harness.join_loop();  // run() 在"所有 worker 都自己报了停止"之后才 join 完返回

  EXPECT_EQ(stats.load(stats.responses_completed), static_cast<std::uint64_t>(kClients));
  EXPECT_EQ(stats.load(stats.drain_closed_idle), static_cast<std::uint64_t>(kClients));
  EXPECT_EQ(stats.load(stats.drain_forced_timeout), 0u) << "一条都没卡死，兜底不该动过一次";
  harness.stop_loop();  // 幂等兜底
  EXPECT_EQ(harness.group()->live_connection_count(), 0u) << "跨 worker 的注册表没排空";
  for (int i = 0; i < kWorkers; ++i) {
    EXPECT_EQ(harness.group()->worker_live_count(i), 0u) << "worker " << i << " 还留着连接";
  }
  EXPECT_EQ(stats.load(stats.connections_constructed), stats.load(stats.connections_destructed))
      << "某条 worker 的下游或池里上游没走完成六步关闭";
  std::printf("[S2-05 group] %d worker 各自排干并 join，%d 条在飞请求全部答完，兜底 0 次\n", kWorkers,
              kClients);
  harness.shutdown_all();
}

// ===========================================================================
// S2-06a：回源多端点 + per-worker 静态映射（ADR-17 已裁候选 A）
// 一个不同值，客户端拿到多长的体就是被派到了哪个端点，不需要任何新的观测计数。
//
// 客户端落到哪条 worker 由 S2-01 的 round-robin 决定（`DispatchIsRoundRobinBalanced` 已把
// 那条等式钉住），所以这里逐条 connect 并等 `downstream_accepted` 到位，把到达顺序也钉住，
// 配对才不会变成"看起来对但顺序随机"的那种用例。

// 起一个只回定制体长的上游，返回它的端口。两个端点靠 127.0.0.1 上的不同端口区分：
// 被测的是"目标地址怎么指派"，不是"环回别名怎么解析"，用端口更干净。
bool start_sized_upstream(TestUpstream* upstream, std::size_t body_bytes, std::string* why) {
  if (!upstream->start(why)) return false;
  upstream->set_response_body_bytes(body_bytes);
  return true;
}

// 依次建 k 条连接、每条发一条请求、收一条响应，把每条的响应体长度交回来。
bool drive_one_request_each(GroupHarness* harness, std::vector<Client>* clients,
                            std::vector<std::size_t>* body_sizes, std::string* why) {
  ReactorStats& stats = harness->group()->stats();
  for (std::size_t i = 0; i < clients->size(); ++i) {
    Client& c = (*clients)[i];
    if (!c.connect_to(harness->port(), why)) return false;
    // 钉住到达顺序：这条被 accept 之后才连下一条，所以 worker = i mod N 是确定的。
    const std::uint64_t want = static_cast<std::uint64_t>(i + 1);
    if (!wait_until([&] { return stats.load(stats.downstream_accepted) == want; }, 3000)) {
      *why = "第 " + std::to_string(i) + " 条连接没有被 accept";
      return false;
    }
  }
  for (std::size_t i = 0; i < clients->size(); ++i) {
    Client& c = (*clients)[i];
    if (!c.write_all(request_text(i + 1))) {
      *why = "conn " + std::to_string(i) + " 写请求失败";
      return false;
    }
    std::string status;
    std::string body;
    if (!c.read_response(&status, &body, why)) return false;
    if (status != "HTTP/1.1 200 OK") {
      *why = "conn " + std::to_string(i) + " 状态码不是 200：" + status;
      return false;
    }
    body_sizes->push_back(body.size());
  }
  return true;
}

}  // namespace

// 一、最基本的形状：2 worker × 2 端点 → worker0 打端点 0、worker1 打端点 1。
// 这条红了意味着映射错位（或者根本没生效，全部落到第 0 个端点——那正是改动前唯一的形态）。
TEST(S2_06a_MultiEndpoint, EachWorkerUsesItsAssignedEndpoint) {
  constexpr int kWorkers = 2;
  constexpr std::size_t kFirstBody = 64;
  constexpr std::size_t kSecondBody = 128;

  std::string why;
  TestUpstream first;
  TestUpstream second;
  ASSERT_TRUE(start_sized_upstream(&first, kFirstBody, &why)) << why;
  ASSERT_TRUE(start_sized_upstream(&second, kSecondBody, &why)) << why;

  GroupHarness harness;
  GroupHarness::Options options;
  options.config = group_base_config(kWorkers);
  options.with_upstream = false;
  options.config.upstream_endpoints.push_back(
      ReactorGroup::Config::UpstreamEndpoint{"127.0.0.1", first.port()});
  options.config.upstream_endpoints.push_back(
      ReactorGroup::Config::UpstreamEndpoint{"127.0.0.1", second.port()});
  ASSERT_TRUE(harness.start(options, &why)) << why;

  std::vector<Client> clients(2);
  std::vector<std::size_t> sizes;
  ASSERT_TRUE(drive_one_request_each(&harness, &clients, &sizes, &why)) << why;

  ASSERT_EQ(sizes.size(), 2u);
  EXPECT_EQ(sizes[0], kFirstBody) << "worker0 的下游拿到了别的端点的响应：静态指派错位";
  EXPECT_EQ(sizes[1], kSecondBody) << "worker1 的下游拿到了别的端点的响应：静态指派错位";
  EXPECT_EQ(first.requests_served(), 1u);
  EXPECT_EQ(second.requests_served(), 1u);
  ReactorStats& stats = harness.group()->stats();
  EXPECT_EQ(stats.load(stats.upstream_opened), 2u) << "两条 worker 各开一条上游，池不该在这里摊薄";

  for (auto& c : clients) c.close();
  harness.stop_loop();
  EXPECT_EQ(stats.load(stats.live_connections), 0u);
  EXPECT_EQ(stats.load(stats.connections_constructed), stats.load(stats.connections_destructed));
  harness.shutdown_all();
  first.stop_and_join();
  second.stop_and_join();
}

// 二、`i mod E` 而不只是"前 E 条 worker 各拿一个"：4 worker × 2 端点 → 端点 0 拿到
// worker 0 与 2，端点 1 拿到 worker 1 与 3。每端点恰好 2 条，容量公式的那个"均匀"
// 因此不是我以为，是被断言过的。
TEST(S2_06a_MultiEndpoint, MappingIsModuloSoEndpointsStayBalanced) {
  constexpr int kWorkers = 4;
  constexpr std::size_t kFirstBody = 64;
  constexpr std::size_t kSecondBody = 128;

  std::string why;
  TestUpstream first;
  TestUpstream second;
  ASSERT_TRUE(start_sized_upstream(&first, kFirstBody, &why)) << why;
  ASSERT_TRUE(start_sized_upstream(&second, kSecondBody, &why)) << why;

  GroupHarness harness;
  GroupHarness::Options options;
  options.config = group_base_config(kWorkers);
  options.with_upstream = false;
  options.config.upstream_endpoints.push_back(
      ReactorGroup::Config::UpstreamEndpoint{"127.0.0.1", first.port()});
  options.config.upstream_endpoints.push_back(
      ReactorGroup::Config::UpstreamEndpoint{"127.0.0.1", second.port()});
  ASSERT_TRUE(harness.start(options, &why)) << why;

  std::vector<Client> clients(kWorkers);
  std::vector<std::size_t> sizes;
  ASSERT_TRUE(drive_one_request_each(&harness, &clients, &sizes, &why)) << why;

  ASSERT_EQ(sizes.size(), static_cast<std::size_t>(kWorkers));
  for (int i = 0; i < kWorkers; ++i) {
    const std::size_t expected = (i % 2 == 0) ? kFirstBody : kSecondBody;
    EXPECT_EQ(sizes[static_cast<std::size_t>(i)], expected)
        << "worker " << i << " 应该用第 " << (i % 2) << " 个端点";
  }
  EXPECT_EQ(first.requests_served(), 2u);
  EXPECT_EQ(second.requests_served(), 2u);

  for (auto& c : clients) c.close();
  harness.stop_loop();
  harness.group()->stats();
  harness.shutdown_all();
  first.stop_and_join();
  second.stop_and_join();
}

// 三、容量公式里那个不好看的边界：**端点数超过 worker 数时，多出来的端点一条都收不到**。
// 2 worker × 3 端点 → 第 3 个端点服务数必须为 0（"配了但用不上"这件事要有账，否则报告里
// 写 E=3 会让人以为容量是 3×28,232，而实际按 min(N, E) 算）。
TEST(S2_06a_MultiEndpoint, EndpointsBeyondWorkerCountAreNeverUsed) {
  constexpr int kWorkers = 2;
  constexpr std::size_t kFirstBody = 64;
  constexpr std::size_t kSecondBody = 128;
  constexpr std::size_t kThirdBody = 192;

  std::string why;
  TestUpstream first;
  TestUpstream second;
  TestUpstream third;
  ASSERT_TRUE(start_sized_upstream(&first, kFirstBody, &why)) << why;
  ASSERT_TRUE(start_sized_upstream(&second, kSecondBody, &why)) << why;
  ASSERT_TRUE(start_sized_upstream(&third, kThirdBody, &why)) << why;

  GroupHarness harness;
  GroupHarness::Options options;
  options.config = group_base_config(kWorkers);
  options.with_upstream = false;
  options.config.upstream_endpoints.push_back(
      ReactorGroup::Config::UpstreamEndpoint{"127.0.0.1", first.port()});
  options.config.upstream_endpoints.push_back(
      ReactorGroup::Config::UpstreamEndpoint{"127.0.0.1", second.port()});
  options.config.upstream_endpoints.push_back(
      ReactorGroup::Config::UpstreamEndpoint{"127.0.0.1", third.port()});
  ASSERT_TRUE(harness.start(options, &why)) << why;

  std::vector<Client> clients(kWorkers);
  std::vector<std::size_t> sizes;
  ASSERT_TRUE(drive_one_request_each(&harness, &clients, &sizes, &why)) << why;

  ASSERT_EQ(sizes.size(), 2u);
  EXPECT_EQ(sizes[0], kFirstBody);
  EXPECT_EQ(sizes[1], kSecondBody);
  EXPECT_EQ(third.requests_served(), 0u)
      << "端点数 > worker 数时第 3 个端点不该被用到：容量按 min(N, E) 算，这条就是那句边界";
  std::printf("[S2-06a capacity] workers=%d endpoints=3 → 只用上第 %d 个，第 3 个收到 %llu 条\n",
              kWorkers, kWorkers, static_cast<unsigned long long>(third.requests_served()));

  for (auto& c : clients) c.close();
  harness.stop_loop();
  harness.shutdown_all();
  first.stop_and_join();
  second.stop_and_join();
  third.stop_and_join();
}

// ===========================================================================
// S3-01：路由作为链上的一环 + per-worker 池按目标分桶（ADR-18 / ADR-15 追加节）
// ===========================================================================
//
// 这一组测的是四件事，每件都对应验收句里的一句（`TASKS.md` 的 S3-01 行）：
//   一、**路由 filter 改过的目标真的被 `ensure_upstream` 读到**——端到端一条请求落到指定
//        后端，而且 `upstream_opened` 只增加那一个目标的连接。可见性靠**两个 TestUpstream
//        各自自报** accepted/requests（沿用 B-007 那句"每个后端收多少条从这里取，不拿
//        总数 ÷ 后端数算"的立场），网关侧不新增任何观测计数。
//   二、**桶是分开的**：归还进的是自己那条目标的桶，借出的也只从那个桶借。
//   三、**目标切换只发生在干净边界**，切换时欠的那一次重驱动是真的发生了
//        （第五节那条"会挂死的环"——摘掉它就红，见下面的阳性验证说明）。
//   四、**无匹配 / 越界下标都要表态**，不许退化成"请求安静地压在缓冲里"。
//
// 后端靠**响应体长度**区分（64 B vs 128 B，`start_sized_upstream`），客户端拿到多长的体
// 就是被派到了哪个后端——这件事不需要任何新的网关计数，和 S2_06a 那三条同一个做法。
namespace {

// 目标表：两个 TestUpstream、两个端口，下标 0/1 就是路由规则里写的那个下标。
// 注意这里**不走** harness 自己的那台 upstream_（with_upstream=false），
// 因为一台上游配不出两个目标；单目标用例继续用 with_upstream=true 的那条老路。
struct TwoTargets {
  TestUpstream first;
  TestUpstream second;

  bool start(std::string* why) {
    if (!start_sized_upstream(&first, 64, why)) return false;
    return start_sized_upstream(&second, 128, why);
  }
  void stop() {
    first.stop_and_join();
    second.stop_and_join();
  }
  void fill(Harness::Options* options) const {
    options->with_upstream = false;
    options->config.upstream_targets.clear();
    options->config.upstream_targets.push_back(
        Reactor::Config::Target{std::string("127.0.0.1"), static_cast<std::uint16_t>(first.port())});
    options->config.upstream_targets.push_back(
        Reactor::Config::Target{std::string("127.0.0.1"), static_cast<std::uint16_t>(second.port())});
    options->config.default_target = 0;
  }
};

std::string get_request(std::string_view path) {
  return "GET " + std::string(path) + " HTTP/1.1\r\nHost: gateway.test\r\n\r\n";
}

// 均衡这一环的装配具：组表 + 每 worker 一份的状态（ADR-19「per-worker 均衡」在测试里的形状）。
// 组表必须比 reactor 活得久——`LoadBalanceFilter` 拿的是引用，和 RouteFilter 同一条命期纪律，
// 所以用例里它和路由表都声明在 Harness **之前**。
//
// `arm()` 与 `install()` 分成两步不是啰嗦：状态里的配对账按目标数开长度、轮询游标按组数开
// 长度，而调用这两步之前组数还不知道。**装链必须在 start() 之前**（1.16 那条"运行期不许改链"），
// `config.lb_state` 也必须在 start() 之前进 options——Reactor 存的是 config 的一份副本，
// 构造之后再写等于没写，后果不是崩而是 `least_conn` 的账只加不减、永远选第一个成员，
// 看起来完全在正常工作（这条我自己踩过，写在 S3-02 的交接段里）。
struct LbRig {
  l7gate::route::GroupTable groups;
  std::shared_ptr<l7gate::route::LbState> state;

  // 建一个**只有一个成员**的轮询组，返回它的组下标（装配顺序就是下标顺序）。
  // 一成员组 + 轮询在数值上等于"命中哪条规则就去那个端点"，所以 S3-01 那 8 条期望的
  // 数字一个都不用改——变的只是这个下标现在要经过均衡这一环才拿到。
  std::uint16_t add_single_member_group(std::string_view name, std::uint16_t target) {
    std::uint16_t group = l7gate::route::kNoUpstreamGroup;
    EXPECT_TRUE(groups.add_group(name, l7gate::route::Balance::kRoundRobin, &group));
    EXPECT_TRUE(groups.add_member(group, target));
    return group;
  }

  void arm(std::size_t target_count) {
    state = std::make_shared<l7gate::route::LbState>(target_count, groups.size());
  }

  // 装到链上（第二环）+ 把同一份状态交给 reactor 记账。**调用点必须在 add_filter(RouteFilter)
  // 之后、start() 之前**——顺序 = 添加顺序，而均衡读的那个组下标正是路由这一环写下的。
  void install(Harness::Options* options, Harness* harness) {
    options->config.lb_state = state;
    harness->add_filter(std::make_unique<l7gate::filter::LoadBalanceFilter>(groups, state));
  }
};

// 一条"命中 /a 去组 0（端点 0）、命中 /b 去组 1（端点 1）"的最小表 + 两个一成员组。
// 每个用例自己持有它们，而且必须**声明在 Harness 之前**。
void add_ab_routes(l7gate::route::RouteTable* table, LbRig* rig) {
  ASSERT_TRUE(table->add_prefix("/a", rig->add_single_member_group("a", 0)));
  ASSERT_TRUE(table->add_prefix("/b", rig->add_single_member_group("b", 1)));
}

// 一个故意写下越界下标的 filter：它不是"坏输入测试"的装饰，它是 reactor 里那条
// 边界检查（`upstream_target >= config_.upstream_targets.size()`）**唯一的读者**。
// 没有这条用例，那条检查就是一段没人走过的分支——而它保护的是一次 vector 越界索引。
class BadTargetFilter : public filter::RequestFilter {
 public:
  std::string_view name() const noexcept override { return "bad-target"; }
  filter::Decision on_request(filter::RequestInfo& info) override {
    info.upstream_target = 99;  // 目标表只有 2 个条目
    return filter::Decision::kContinue;
  }
};

}  // namespace

// 一、最基本的端到端：/b 那条请求只碰端点 1，端点 0 一条连接都不该被开。
// 这条红了意味着两种错之一：路由没被读（全部落到默认目标 0），或者"读到了但回源
// 仍然用配置里那一个地址"——后者正是 1.16 记的那笔债的形态（字段写了、没人读）。
TEST(S3_01_Routing, RequestLandsOnTheRoutedTargetOnly) {
  std::string why;
  l7gate::route::RouteTable table;
  LbRig rig;
  add_ab_routes(&table, &rig);

  TwoTargets targets;
  ASSERT_TRUE(targets.start(&why)) << why;

  Harness harness;
  Harness::Options options = default_options();
  targets.fill(&options);
  // add_filter 只是把 filter 存进 pending 队列，真正挂链发生在 start() 里 spawn 循环线程
  // **之前**（1.16 那条"运行期不许改链"的纪律，测试不能自己先违反它要证的那条）。
  // 所以顺序只能是先 add 再 start——反过来的话 filter 根本不在链上，请求会安静地落到
  // 默认目标 0，而这条用例的失败形态就是"body 64 而不是 128"。
  harness.add_filter(std::make_unique<l7gate::filter::RouteFilter>(table));
  rig.arm(2);
  rig.install(&options, &harness);  // 第二环：组 → 目标（没有它，路由写下的组没人翻成下标）
  ASSERT_TRUE(harness.start(options, &why)) << why;
  ReactorStats& stats = harness.reactor()->stats();

  Client client;
  ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;
  ASSERT_TRUE(client.write_all(get_request("/b/x")));
  std::string status;
  std::string body;
  ASSERT_TRUE(client.read_response(&status, &body, &why)) << why;
  EXPECT_EQ(status, "HTTP/1.1 200 OK");
  EXPECT_EQ(body.size(), 128u) << "路由给的是组 1、组 1 里只有端点 1，客户端却拿到别的后端的体";

  EXPECT_EQ(stats.load(stats.upstream_opened), 1u)
      << "只增加那一个目标的连接：新建数必须是 1，且那条连接属于端点 1";
  EXPECT_EQ(targets.first.connections_accepted(), 0u) << "端点 0 一条都不该被连";
  EXPECT_EQ(targets.second.connections_accepted(), 1u);
  EXPECT_EQ(targets.first.requests_served(), 0u);
  EXPECT_EQ(targets.second.requests_served(), 1u);

  client.close();
  harness.stop_loop();
  harness.shutdown_all();
  targets.stop();
}

// 二、无匹配 → 404，而且**一条上游都不开**。
// 后半句才是这条用例的主语：1.16 写的是"拦下来必须真的没回源，不是回了源再假装没回"。
// 如果哪天链调用点被挪到 forward_request 之后，前端仍然看到 404、用例的前半句照样绿，
// 但 upstream_opened 会变成 1——所以那一格必须断。
TEST(S3_01_Routing, NoMatchRepliesFourOhFourAndNeverOpensAnUpstream) {
  std::string why;
  l7gate::route::RouteTable table;
  LbRig rig;
  ASSERT_TRUE(table.add_prefix("/a", rig.add_single_member_group("a", 0)));  // 只有 /a，/zz 谁都不挨着

  TwoTargets targets;
  ASSERT_TRUE(targets.start(&why)) << why;

  Harness harness;
  Harness::Options options = default_options();
  targets.fill(&options);
  harness.add_filter(std::make_unique<l7gate::filter::RouteFilter>(table));
  rig.arm(2);
  rig.install(&options, &harness);
  ASSERT_TRUE(harness.start(options, &why)) << why;
  ReactorStats& stats = harness.reactor()->stats();

  Client client;
  ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;
  ASSERT_TRUE(client.write_all(get_request("/zz")));
  std::string status;
  std::string body;
  ASSERT_TRUE(client.read_response(&status, &body, &why)) << why;
  EXPECT_EQ(status, "HTTP/1.1 404 Not Found");
  EXPECT_EQ(body, "Not Found\n");

  EXPECT_EQ(stats.load(stats.upstream_opened), 0u) << "拦下来就真的没回源：一条上游都不许开";
  EXPECT_EQ(stats.load(stats.gateway_replies_sent), 1u) << "自产响应要有账";
  EXPECT_EQ(targets.first.connections_accepted(), 0u);
  EXPECT_EQ(targets.second.connections_accepted(), 0u);

  // 404 那条响应自带 `Connection: close`（这张表里每一条都带），所以配对收尾、
  // 客户端随后看到 EOF。等不到 EOF 就是"回了响应但连接挂着"，那是另一类失效。
  ASSERT_TRUE(client.wait_for_eof(&why)) << why;
  harness.stop_loop();
  EXPECT_EQ(stats.load(stats.live_connections), 0u);
  harness.shutdown_all();
  targets.stop();
}

// 三、越界下标必须**表态**而不是把请求永久压住。
// 这条是 reactor 里那条索引前边界检查的读者（也是 `pool_bucket()` 返回 nullptr 那一格
// 能被走到的唯一途径）。形状：filter 写了 99、目标表只有 2 个条目 → 502 + 不开上游。
// 为什么不是 404：404 说"你要的东西没有路由"，这一格说的是"有路由、目的地没配"——
// 那是网关自己的毛病，把它读成客户端的错会让运维去查一条根本没坏的路径。
TEST(S3_01_Routing, OutOfRangeTargetRejectsFiveOhTwoInsteadOfStalling) {
  std::string why;
  TwoTargets targets;
  ASSERT_TRUE(targets.start(&why)) << why;

  Harness harness;
  Harness::Options options = default_options();
  targets.fill(&options);
  harness.add_filter(std::make_unique<BadTargetFilter>());
  ASSERT_TRUE(harness.start(options, &why)) << why;
  ReactorStats& stats = harness.reactor()->stats();

  Client client;
  ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;
  ASSERT_TRUE(client.write_all(get_request("/a")));
  std::string status;
  std::string body;
  ASSERT_TRUE(client.read_response(&status, &body, &why))
      << "越界的目标下标不该让请求安静地压在缓冲里等一个不会出现的桶（那就是挂死）";
  EXPECT_EQ(status, "HTTP/1.1 502 Bad Gateway");
  EXPECT_EQ(stats.load(stats.upstream_opened), 0u);
  EXPECT_EQ(targets.first.connections_accepted(), 0u);
  EXPECT_EQ(targets.second.connections_accepted(), 0u);

  client.close();
  harness.stop_loop();
  harness.shutdown_all();
  targets.stop();
}

// 四、**桶是分开的**：/a 的热上游还回 0 号桶，/b 的下游绝不许把它借走。
// 这条是"分桶"这个改动本身的证据。把池退回成一个不带键的 vector（S2 的形状），
// 第 3 步就会从池里捞出端点 0 那条热连接、请求被送到错误的后端——客户端会拿到 64 B
// 而不是 128 B 的体，而 `upstream_opened` 会停在 1。两种错法都逃不掉。
TEST(S3_01_Routing, PoolBucketsKeepTargetsApart) {
  std::string why;
  l7gate::route::RouteTable table;
  LbRig rig;
  add_ab_routes(&table, &rig);

  TwoTargets targets;
  ASSERT_TRUE(targets.start(&why)) << why;

  Harness harness;
  Harness::Options options = default_options();
  targets.fill(&options);
  harness.add_filter(std::make_unique<l7gate::filter::RouteFilter>(table));
  rig.arm(2);
  rig.install(&options, &harness);
  ASSERT_TRUE(harness.start(options, &why)) << why;
  ReactorStats& stats = harness.reactor()->stats();

  // 1) 第一条 /a 下游：新建端点 0 的连接，关闭 → 归还进 0 号桶。
  {
    Client client;
    ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;
    ASSERT_TRUE(client.write_all(get_request("/a/1")));
    std::string status;
    std::string body;
    ASSERT_TRUE(client.read_response(&status, &body, &why)) << why;
    ASSERT_EQ(body.size(), 64u);
    client.shutdown_write();
    ASSERT_TRUE(client.wait_for_eof(&why)) << why;
    ASSERT_TRUE(wait_until([&] { return stats.load(stats.pool_returns) == 1u; }, 3000))
        << "下游关闭后上游没有归还进池";
  }
  EXPECT_EQ(stats.load(stats.upstream_opened), 1u);

  // 2) 第二条 /a 下游：应当从 0 号桶**借到**那条热上游，不新建。
  {
    Client client;
    ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;
    ASSERT_TRUE(client.write_all(get_request("/a/2")));
    std::string status;
    std::string body;
    ASSERT_TRUE(client.read_response(&status, &body, &why)) << why;
    EXPECT_EQ(body.size(), 64u);
    ASSERT_TRUE(wait_until([&] { return stats.load(stats.pool_borrows) == 1u; }, 3000))
        << "同目标的第二条下游没有复用 0 号桶里的热上游";
    client.shutdown_write();
    ASSERT_TRUE(client.wait_for_eof(&why)) << why;
    ASSERT_TRUE(wait_until([&] { return stats.load(stats.pool_returns) == 2u; }, 3000));
  }
  EXPECT_EQ(stats.load(stats.upstream_opened), 1u) << "0 号桶里有热的，不该再开新的";

  // 3) /b 的下游：0 号桶里那条热连接**不归它用**，必须为端点 1 新建一条。
  {
    Client client;
    ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;
    ASSERT_TRUE(client.write_all(get_request("/b/1")));
    std::string status;
    std::string body;
    ASSERT_TRUE(client.read_response(&status, &body, &why)) << why;
    EXPECT_EQ(body.size(), 128u)
        << "借到了别的目标的池化上游：桶没分开，或者归还时走错了桶";
    client.shutdown_write();
    ASSERT_TRUE(client.wait_for_eof(&why)) << why;
    ASSERT_TRUE(wait_until([&] { return stats.load(stats.pool_returns) == 3u; }, 3000));
  }
  EXPECT_EQ(stats.load(stats.upstream_opened), 2u) << "两个目标各开一条，池不跨目标摊薄";
  EXPECT_EQ(stats.load(stats.pool_borrows), 1u) << "/b 那条没有从 0 号桶借到东西（也不该借到）";
  EXPECT_EQ(targets.first.connections_accepted(), 1u);
  EXPECT_EQ(targets.second.connections_accepted(), 1u);

  // 4) **这一步才是"桶真的分开了"的那一格**。上面三步还漏着一个形状：如果归还时不看
  //    连接自己那条目标、一律进同一个桶（我第一次跑阳性验证时就是这么打的补丁），
  //    前三步照样全绿——因为没有任何后续请求会去碰那个被放错位置的条目。
  //    现在两条目标的池里各有一条热的，再来一条 `/a` 的下游：0 号桶里应当只有连到
  //    端点 0 的那条，借到它就是一次复用（`upstream_opened` 不涨、体长 64）。
  //    **实测到的红不是"拿到 128"而是挂到读超时**（2026-10-29 那次阳性验证）：放错桶的
  //    那条被 `take_pooled_upstream(0)` 捞出来后，`forward_request` 认出它的标签不是 0,
  //    于是按"目标要换但手上这条是配对"的口径把请求压住——压住本身是对的（那是 pipeline
  //    那一格的机制），错的是它永远等不到干净边界。失败形态从"发错后端"变成"不动",
  //    两样都不能接受，但注释必须按观察到的那个写。
  {
    Client client;
    ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;
    ASSERT_TRUE(client.write_all(get_request("/a/3")));
    std::string status;
    std::string body;
    ASSERT_TRUE(client.read_response(&status, &body, &why)) << "第三条 /a：" << why;
    EXPECT_EQ(body.size(), 64u)
        << "从 0 号桶借到了连向端点 1 的热上游：归还时没按连接自己的目标选桶";
    ASSERT_TRUE(wait_until([&] { return stats.load(stats.pool_borrows) == 2u; }, 3000))
        << "0 号桶里有热的却没借到，说明桶的键不对";
    client.shutdown_write();
    ASSERT_TRUE(client.wait_for_eof(&why)) << why;
    ASSERT_TRUE(wait_until([&] { return stats.load(stats.pool_returns) == 4u; }, 3000));
  }
  EXPECT_EQ(stats.load(stats.upstream_opened), 2u) << "复用命中就不该再开新的";
  EXPECT_EQ(targets.first.requests_served(), 3u);
  EXPECT_EQ(targets.second.requests_served(), 1u);

  harness.stop_loop();
  // 排空池要排**所有桶**。只排一个桶的写法在这里会露出来：池里还剩一条热上游，
  // live_connections 不归零、constructed != destructed，而 `pool_idle_count()`
  // 也不是 0（它是各桶之和，S3-01 改的那个口径就在这个数上）。
  EXPECT_EQ(harness.reactor()->pool_idle_count(), 0u) << "停环之后仍有桶没排空";
  EXPECT_EQ(stats.load(stats.live_connections), 0u);
  EXPECT_EQ(stats.load(stats.connections_constructed), stats.load(stats.connections_destructed));
  harness.shutdown_all();
  targets.stop();
}

// 五、`pool_max_idle` 的上界现在是**每 worker 每目标**的（ADR-15 追加节改的那一格）。
// 池上限 1、两个目标各归还一条：旧的"每 worker 一个池"会把第二条判成满了并关掉
// （pool_evictions_full==1），新口径下两条都留在各自桶里（evictions_full==0、returns==2）。
// 这条用例的存在理由：这条上界直接进 SLO #2 的内存账（E×pool_max_idle），
// 口径变了而读数不变，那笔账就是错的。
TEST(S3_01_Routing, PoolMaxIdleIsPerBucketNotPerWorker) {
  std::string why;
  l7gate::route::RouteTable table;
  LbRig rig;
  add_ab_routes(&table, &rig);

  TwoTargets targets;
  ASSERT_TRUE(targets.start(&why)) << why;

  Harness harness;
  Harness::Options options = default_options();
  targets.fill(&options);
  options.config.pool_max_idle = 1;  // 每 worker 每目标 1 条
  harness.add_filter(std::make_unique<l7gate::filter::RouteFilter>(table));
  rig.arm(2);
  rig.install(&options, &harness);
  ASSERT_TRUE(harness.start(options, &why)) << why;
  ReactorStats& stats = harness.reactor()->stats();

  for (std::string_view path : {std::string_view("/a"), std::string_view("/b")}) {
    Client client;
    ASSERT_TRUE(client.connect_to(harness.port(), &why)) << path;
    ASSERT_TRUE(client.write_all(get_request(path)));
    std::string status;
    std::string body;
    ASSERT_TRUE(client.read_response(&status, &body, &why)) << path;
    client.shutdown_write();
    ASSERT_TRUE(client.wait_for_eof(&why)) << path;
  }
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.pool_returns) == 2u; }, 3000))
      << "两条分别属于两个目标的空闲上游没能同时留在池里";
  EXPECT_EQ(stats.load(stats.pool_evictions_full), 0u)
      << "上界被当成『每 worker 总量』算了：第二条会被判成池满关掉，那 SLO #2 的账就少记一格";
  EXPECT_EQ(stats.load(stats.upstream_opened), 2u);

  harness.stop_loop();
  harness.shutdown_all();
  targets.stop();
}

// 六、同一条 keep-alive 下游**换目标**：/a 答完之后再发 /b。
// 换的时机是干净边界（在飞 0 + 出向空 + upstream_reusable），换法是把旧上游按它自己
// 那条目标归还进 0 号桶（不是关掉它），再为端点 1 新建一条。
TEST(S3_01_Routing, TargetSwitchRecyclesIntoItsOwnBucket) {
  std::string why;
  l7gate::route::RouteTable table;
  LbRig rig;
  add_ab_routes(&table, &rig);

  TwoTargets targets;
  ASSERT_TRUE(targets.start(&why)) << why;

  Harness harness;
  Harness::Options options = default_options();
  targets.fill(&options);
  harness.add_filter(std::make_unique<l7gate::filter::RouteFilter>(table));
  rig.arm(2);
  rig.install(&options, &harness);
  ASSERT_TRUE(harness.start(options, &why)) << why;
  ReactorStats& stats = harness.reactor()->stats();

  Client client;
  ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;

  std::string status;
  std::string body;
  ASSERT_TRUE(client.write_all(get_request("/a/first")));
  ASSERT_TRUE(client.read_response(&status, &body, &why)) << "第一条：" << why;
  ASSERT_EQ(body.size(), 64u);
  ASSERT_EQ(stats.load(stats.upstream_opened), 1u);

  // 同一条下游上的第二条请求，路由指向另一个目标。此刻手上没有在飞请求 → 允许换。
  ASSERT_TRUE(client.write_all(get_request("/b/second")));
  ASSERT_TRUE(client.read_response(&status, &body, &why)) << "换目标那一条：" << why;
  EXPECT_EQ(body.size(), 128u);
  EXPECT_EQ(stats.load(stats.upstream_opened), 2u) << "新目标是新建的，不是从别的桶借的";
  EXPECT_EQ(stats.load(stats.pool_returns), 1u)
      << "换下来的旧上游应当按它自己那条目标归还进 0 号桶，而不是被关掉";
  EXPECT_EQ(stats.load(stats.pool_evictions_full), 0u) << "0 号桶才用掉 1 格，不该判成满";
  EXPECT_EQ(targets.first.requests_served(), 1u);
  EXPECT_EQ(targets.second.requests_served(), 1u);

  client.shutdown_write();
  ASSERT_TRUE(client.wait_for_eof(&why)) << why;
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.pool_returns) == 2u; }, 3000))
      << "下游关闭后第二条上游也该进桶";
  harness.stop_loop();
  EXPECT_EQ(harness.reactor()->pool_idle_count(), 0u) << "所有桶都该在停环时排空";
  EXPECT_EQ(stats.load(stats.connections_constructed), stats.load(stats.connections_destructed));
  harness.shutdown_all();
  targets.stop();
}

// 七、**这条是 ADR-18 第五节那笔代价的收据**。
// 形状：一条下游在**同一次可读里**pipeline 两条请求，第一条去端点 0、第二条去端点 1，
// 而端点 0 故意晚 200 ms 才回。于是第二条请求第一次被考虑时，第一条还在飞（in_flight=1）
// → 按口径必须**压住**它。压住之后唯一的出路是响应完结那一刻由 `pump_response` 补的
// 那次重驱动：**把那次重驱动摘掉，这条用例不会变快也不会变慢，它是挂住**
// （第二次 read_response 撞 2 s 读超时）。这就是"能红才叫绿"。
// 顺带把 ADR-16 那条 FIFO 也钉住：响应顺序必须等于请求顺序（先 64 B 后 128 B）。
TEST(S3_01_Routing, PipelinedTargetSwitchIsHeldThenRedriven) {
  std::string why;
  l7gate::route::RouteTable table;
  LbRig rig;
  add_ab_routes(&table, &rig);

  TwoTargets targets;
  ASSERT_TRUE(targets.start(&why)) << why;
  targets.first.set_delay_before_response_ms(200);  // 端点 0 慢，把第二条请求压在闸内

  Harness harness;
  Harness::Options options = default_options();
  targets.fill(&options);
  harness.add_filter(std::make_unique<l7gate::filter::RouteFilter>(table));
  rig.arm(2);
  rig.install(&options, &harness);
  ASSERT_TRUE(harness.start(options, &why)) << why;
  ReactorStats& stats = harness.reactor()->stats();

  Client client;
  ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;
  ASSERT_TRUE(client.write_all(get_request("/a/slow") + get_request("/b/held")))
      << "两条请求一次写完：一个段、一次 EPOLLIN";

  std::string status;
  std::string body;
  ASSERT_TRUE(client.read_response(&status, &body, &why)) << "第一条（慢上游）：" << why;
  EXPECT_EQ(body.size(), 64u);
  ASSERT_TRUE(client.read_response(&status, &body, &why))
      << "第二条被压住之后没有等来重驱动：这就是 ADR-18 第五节那条会挂死的环";
  EXPECT_EQ(body.size(), 128u) << "响应顺序或目标错位：pipeline 的 FIFO 被打乱了";

  EXPECT_EQ(stats.load(stats.requests_parsed), 2u);
  EXPECT_EQ(stats.load(stats.responses_completed), 2u);
  EXPECT_EQ(stats.load(stats.upstream_opened), 2u) << "两个目标各新建一条";
  EXPECT_EQ(stats.load(stats.pool_returns), 1u) << "换下来的 0 号上游进了 0 号桶";
  EXPECT_EQ(targets.first.requests_served(), 1u);
  EXPECT_EQ(targets.second.requests_served(), 1u);

  client.shutdown_write();
  ASSERT_TRUE(client.wait_for_eof(&why)) << why;
  harness.stop_loop();
  EXPECT_EQ(stats.load(stats.connections_constructed), stats.load(stats.connections_destructed));
  harness.shutdown_all();
  targets.stop();
}

// 八、**没有路由 filter 时，worker 用的就是它的默认目标**——这一格是 S2-06a 那三条
// 用例不作废的根据（它们在多 Reactor 下断 `i mod E`，这条在单 Reactor 下断同一个形状：
// 表里第 0 个条目），也是压测"空链那一臂"的含义：链是空的，不是"装了 filter 但没命中"。
TEST(S3_01_Routing, NoFilterUsesDefaultTargetExactlyLikeS2) {
  std::string why;
  TwoTargets targets;
  ASSERT_TRUE(targets.start(&why)) << why;

  Harness harness;
  Harness::Options options = default_options();
  targets.fill(&options);  // default_target = 0
  ASSERT_TRUE(harness.start(options, &why)) << why;
  ASSERT_EQ(harness.reactor()->request_filter_count(), 0u) << "这一臂的链必须是空的";
  ReactorStats& stats = harness.reactor()->stats();

  Client client;
  ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;
  ASSERT_TRUE(client.write_all(get_request("/b/not-routed-anyway")));
  std::string status;
  std::string body;
  ASSERT_TRUE(client.read_response(&status, &body, &why)) << why;
  EXPECT_EQ(body.size(), 64u) << "空链时该走默认目标（端点 0），路由不参与";
  EXPECT_EQ(targets.first.connections_accepted(), 1u);
  EXPECT_EQ(targets.second.connections_accepted(), 0u);
  EXPECT_EQ(stats.load(stats.upstream_opened), 1u);

  client.close();
  harness.stop_loop();
  harness.shutdown_all();
  targets.stop();
}

}  // namespace l7gate::net

// ===========================================================================
// S3-02：负载均衡三策略的端到端形状（ADR-19 验收句的那三条 + 交接段第 ⑦ 条坑的收据）
// ===========================================================================
//
// 可见性沿用 S3-01 的立场：**每个后端各收多少条由被连的那一侧自报**（`TestUpstream::
// connections_accepted()` / `requests_served()`），不给网关加"每目标请求数"这组分位——
// 它的第一个真读者是 S3-03 的误摘率分母，那时才按 ADR 引入（ADR-19「我放弃什么」最后一条）。
// 后端仍然靠**响应体长度**区分（64/128/192）：客户端拿到多长的体，就是被派到了哪个后端。
//
// 三条策略各断一件事，形状互不替换：
//   一、轮询断**分布**（12 ÷ 3 = 各 4 条，而且是在同一条 keep-alive 上——这条同时是
//        ADR-19 后果段那句"均衡让换目标从边角变成主路径"的第一次真实负载）；
//   二、最少连接断**方向**，不断绝对值（绝对值被这台机器的时序牵着走）；
//   三、一致哈希断**同一个 key 换一条连接也落在同一个后端**，这是它对客户端的承诺。
namespace l7gate::net {
namespace {

// 三个后端 = 一个组三个成员的最小可用形状。两个成员断不出轮询周期，也断不出
// "三个里挑最闲的那个"这种方向性判断。
struct ThreeTargets {
  TestUpstream first;
  TestUpstream second;
  TestUpstream third;

  bool start(std::string* why) {
    if (!start_sized_upstream(&first, 64, why)) return false;
    if (!start_sized_upstream(&second, 128, why)) return false;
    return start_sized_upstream(&third, 192, why);
  }
  void stop() {
    first.stop_and_join();
    second.stop_and_join();
    third.stop_and_join();
  }
  void fill(Harness::Options* options) const {
    options->with_upstream = false;
    options->config.upstream_targets.clear();
    options->config.upstream_targets.push_back(
        Reactor::Config::Target{std::string("127.0.0.1"), static_cast<std::uint16_t>(first.port())});
    options->config.upstream_targets.push_back(
        Reactor::Config::Target{std::string("127.0.0.1"), static_cast<std::uint16_t>(second.port())});
    options->config.upstream_targets.push_back(
        Reactor::Config::Target{std::string("127.0.0.1"), static_cast<std::uint16_t>(third.port())});
    options->config.default_target = 0;
  }
  // 环上的点键**从这三台后端现场取**，不另写一份常量：环的位置和"reactor 真的会连的那个
  // socket"必须是同一份地址，否则"这台后端在环上的位置"和"这台后端是谁"就是两份真相。
  std::vector<std::string> endpoint_keys() const {
    return {"127.0.0.1:" + std::to_string(first.port()), "127.0.0.1:" + std::to_string(second.port()),
            "127.0.0.1:" + std::to_string(third.port())};
  }
  std::size_t size(std::uint16_t index) const {
    switch (index) {
      case 0: return 64;
      case 1: return 128;
      default: return 192;
    }
  }
};

// 一条"什么都命中"的路由 + 一个三成员的组：这一组用例的主语是**组里选哪一个**，
// 不是哪条路由，所以路由层给一条 catch-all 就够（空前缀那条语义用例在 route_test.cc）。
void install_group(l7gate::route::GroupTable* groups, const ThreeTargets& targets,
                   l7gate::route::Balance balance, std::uint16_t* group, std::string* why,
                   const l7gate::route::HashKey& hash_key = {}) {
  const bool ok = l7gate::testing::add_balanced_group(groups, "all", balance, {0, 1, 2},
                                                     targets.endpoint_keys(), group, why,
                                                     l7gate::route::kDefaultVnodes, hash_key);
  EXPECT_TRUE(ok) << *why;
}

std::uint16_t catch_all_to_route_table(l7gate::route::RouteTable* table, std::uint16_t group) {
  EXPECT_TRUE(table->add_prefix("", group));
  return group;
}

}  // namespace

// 一、**轮询按后端计数**：12 条请求、3 个后端，各收 4 条（⌊12/3⌋…⌈12/3⌉），而且这 12 条
// 全在**同一条 keep-alive 下游**上。选这个形状有两个理由：一是 ADR-19「我放弃什么」第一条
// 明写的——per-worker 的游标在"多条短连接跨 worker"时是 16 个计数器的叠加，那条不是本用例
// 能证的；二是同一条连接上连续换目标，正是 ADR-18 第五节那条重驱动从边角变成主路径的负载。
// `upstream_opened` 应当停在 3：第二轮起每一台都是**从自己的桶里借**，不是重开。
TEST(S3_02_LoadBalance, RoundRobinCyclesAcrossBackendsOnOneKeepAliveConnection) {
  std::string why;
  ThreeTargets targets;
  ASSERT_TRUE(targets.start(&why)) << why;

  l7gate::route::RouteTable table;
  LbRig rig;
  std::uint16_t group = l7gate::route::kNoUpstreamGroup;
  install_group(&rig.groups, targets, l7gate::route::Balance::kRoundRobin, &group, &why);
  ASSERT_NE(group, l7gate::route::kNoUpstreamGroup) << "组装配失败：" << why;
  catch_all_to_route_table(&table, group);

  Harness harness;
  Harness::Options options = default_options();
  targets.fill(&options);
  harness.add_filter(std::make_unique<l7gate::filter::RouteFilter>(table));
  rig.arm(3);
  rig.install(&options, &harness);
  ASSERT_TRUE(harness.start(options, &why)) << why;
  ReactorStats& stats = harness.reactor()->stats();

  Client client;
  ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;
  const std::vector<std::size_t> expected_cycle{64, 128, 192};
  for (int i = 0; i < 12; ++i) {
    ASSERT_TRUE(client.write_all(get_request("/api/req" + std::to_string(i)))) << i << ": " << why;
    std::string status;
    std::string body;
    ASSERT_TRUE(client.read_response(&status, &body, &why)) << "第 " << i << " 条";
    EXPECT_EQ(body.size(), expected_cycle[static_cast<std::size_t>(i) % 3])
        << "第 " << i << " 条没落在轮询该给的那个后端上";
  }
  EXPECT_EQ(targets.first.requests_served(), 4u);
  EXPECT_EQ(targets.second.requests_served(), 4u);
  EXPECT_EQ(targets.third.requests_served(), 4u);
  EXPECT_EQ(targets.first.connections_accepted(), 1u) << "第二轮起应当从桶里借，不是重开一条";
  EXPECT_EQ(targets.second.connections_accepted(), 1u);
  EXPECT_EQ(targets.third.connections_accepted(), 1u);
  EXPECT_EQ(stats.load(stats.upstream_opened), 3u) << "12 条请求开了 3 条以上的上游：桶没被用上";
  EXPECT_EQ(stats.load(stats.responses_completed), 12u);

  // 12 条请求在同一条下游上换了 11 次目标，加上下游自己关闭时归还的那一条，池归还总数
  // 应当是 12。**先等循环把 FIN 处理完再停环**（口径⑤：`pool_returns` 是非稳态量，
  // 要 `wait_until` 等它进稳态，不能"看到条件成立就立刻读"，更不能停环之前抢读）。
  client.shutdown_write();
  ASSERT_TRUE(client.wait_for_eof(&why)) << why;
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.pool_returns) == 12u; }, 3000))
      << "11 次换目标 + 收尾归还一次，池归还总数应当是 12";

  harness.stop_loop();
  EXPECT_EQ(harness.reactor()->pool_idle_count(), 0u) << "停环后要排空所有三个桶";
  EXPECT_EQ(stats.load(stats.live_connections), 0u);
  EXPECT_EQ(stats.load(stats.connections_constructed), stats.load(stats.connections_destructed));
  harness.shutdown_all();
  targets.stop();
}

// 二、**最少连接断方向**：人为把三个后端都压成"收到请求不回"（`set_stall_reads`，S2-05 同一个
// 手法），让两条下游各占住一个后端的配对连接。平手规则是 ADR-19 追加第三条（S3-03 开工裁决①）
// 改过的：**平手集合内轮询**，不再"取成员顺序第一条"——所以头两条的落点是轮询的确定值
// （0 号、2 号），唯一没被占住的是 1 号。放开 1 号之后新请求必须去它：被占住的后端**确实
// 少分到**。**比较符翻转（`<` 改成 `>`）就是这条红**（ADR-19 阳性验证第三条）。
TEST(S3_02_LoadBalance, LeastConnectionsSendsNewWorkToTheIdleBackend) {
  std::string why;
  ThreeTargets targets;
  ASSERT_TRUE(targets.start(&why)) << why;
  targets.first.set_stall_reads(true);   // 0 号：收了请求不回
  targets.second.set_stall_reads(true);  // 1 号：收了请求不回
  targets.third.set_stall_reads(true);   // 2 号：收了请求不回

  l7gate::route::RouteTable table;
  LbRig rig;
  std::uint16_t group = l7gate::route::kNoUpstreamGroup;
  install_group(&rig.groups, targets, l7gate::route::Balance::kLeastConnections, &group, &why);
  ASSERT_NE(group, l7gate::route::kNoUpstreamGroup) << "组装配失败：" << why;
  catch_all_to_route_table(&table, group);

  Harness harness;
  Harness::Options options = default_options();
  targets.fill(&options);
  harness.add_filter(std::make_unique<l7gate::filter::RouteFilter>(table));
  rig.arm(3);
  rig.install(&options, &harness);
  ASSERT_TRUE(harness.start(options, &why)) << why;
  ReactorStats& stats = harness.reactor()->stats();

  // 头两条各占一个后端；等到"确实连上了"再发下一条，否则两条会在全零的账上叠在同一个后端。
  // 平手集合内轮询的确定落点：第一条（游标初值 0）→ 0 号；第二条（平手集合 {1,2}、游标推进）
  // → 2 号。**这条断言同时就是平手规则的下界**（ADR-19 追加三：全零的账不许全落成员 0）。
  Client held_first;
  ASSERT_TRUE(held_first.connect_to(harness.port(), &why)) << why;
  ASSERT_TRUE(held_first.write_all(get_request("/hold/1")));
  ASSERT_TRUE(wait_until([&] { return targets.first.connections_accepted() == 1u; }, 3000))
      << "第一条没有配对到 0 号后端";

  Client held_second;
  ASSERT_TRUE(held_second.connect_to(harness.port(), &why)) << why;
  ASSERT_TRUE(held_second.write_all(get_request("/hold/2")));
  ASSERT_TRUE(wait_until([&] { return targets.third.connections_accepted() == 1u; }, 3000))
      << "第二条没有配对到 2 号后端：平手集合内轮询没有轮转（退回了'取第一条'的旧规则）";

  // 此刻 0 号和 2 号各挂着一条在飞、1 号一条都没有。放开 1 号，新请求必须去它。
  targets.second.set_stall_reads(false);
  Client fresh;
  ASSERT_TRUE(fresh.connect_to(harness.port(), &why)) << why;
  ASSERT_TRUE(fresh.write_all(get_request("/now")));
  std::string status;
  std::string body;
  ASSERT_TRUE(fresh.read_response(&status, &body, &why)) << why;
  EXPECT_EQ(body.size(), 128u) << "把活派给了正忙的后端：最少连接的比较方向是反的";
  EXPECT_EQ(targets.second.requests_served(), 1u);
  EXPECT_EQ(stats.load(stats.upstream_opened), 3u) << "三个后端各一条，没有多开";

  // 收尾：放开 stall，让两条压着的下游把响应收完，否则"停环之后 live==0"就靠硬关而不是排空。
  targets.first.set_stall_reads(false);
  targets.third.set_stall_reads(false);
  std::string released_body;
  ASSERT_TRUE(held_first.read_response(&status, &released_body, &why)) << "0 号放开后该补响应";
  ASSERT_TRUE(held_second.read_response(&status, &released_body, &why)) << "2 号放开后该补响应";
  fresh.close();
  held_first.close();
  held_second.close();
  harness.stop_loop();
  harness.shutdown_all();
  targets.stop();
}

// 三、**交接段第 ⑦ 条坑的收据**：那条坑的表现不是崩，是"账只加不减、永远选第一个成员"，
// 看起来完全在正常工作。这里把它变成可断言的形状：三个后端各配一条**活着但空闲**的下游，
// 然后只关掉中间那一条——它的上游归还进 1 号桶、账回落到 0。下一条请求必须去 1 号。
// 如果 `note_unpaired` 没被调（或者调错了目标），三个读数一样 → 平手取第一个 → 拿到 64 B。
// 这条用例还顺手证明"归还进桶的那条热上游真的被下一个人借走"：1 号后端的
// `connections_accepted` 应当停在 1，而 `requests_served` 涨到 2。
TEST(S3_02_LoadBalance, LeastConnectionsReleasesCapacityWhenTheDownstreamGoesAway) {
  std::string why;
  ThreeTargets targets;
  ASSERT_TRUE(targets.start(&why)) << why;

  l7gate::route::RouteTable table;
  LbRig rig;
  std::uint16_t group = l7gate::route::kNoUpstreamGroup;
  install_group(&rig.groups, targets, l7gate::route::Balance::kLeastConnections, &group, &why);
  ASSERT_NE(group, l7gate::route::kNoUpstreamGroup) << "组装配失败：" << why;
  catch_all_to_route_table(&table, group);

  Harness harness;
  Harness::Options options = default_options();
  targets.fill(&options);
  harness.add_filter(std::make_unique<l7gate::filter::RouteFilter>(table));
  rig.arm(3);
  rig.install(&options, &harness);
  ASSERT_TRUE(harness.start(options, &why)) << why;
  ReactorStats& stats = harness.reactor()->stats();

  std::string status;
  std::string body;
  // 三条各自活着的下游。平手规则是 ADR-19 追加第三条改过的（平手集合内轮询，不再
  // "取成员顺序第一条"），所以落点是轮询的确定值：0 号、2 号、1 号——每次都是
  // "当前最闲（且平手时轮到谁就是谁）"，三台各一条。
  Client keep_first;
  Client keep_second;
  Client keep_third;
  ASSERT_TRUE(keep_first.connect_to(harness.port(), &why)) << why;
  ASSERT_TRUE(keep_first.write_all(get_request("/a")));
  ASSERT_TRUE(keep_first.read_response(&status, &body, &why)) << why;
  ASSERT_EQ(body.size(), 64u) << "第一条在全零的账上应当取游标的第一个（0 号）";
  ASSERT_TRUE(keep_second.connect_to(harness.port(), &why)) << why;
  ASSERT_TRUE(keep_second.write_all(get_request("/b")));
  ASSERT_TRUE(keep_second.read_response(&status, &body, &why)) << why;
  ASSERT_EQ(body.size(), 192u) << "0 号已经挂了一条，平手集合 {1,2} 内轮询该给 2 号";
  ASSERT_TRUE(keep_third.connect_to(harness.port(), &why)) << why;
  ASSERT_TRUE(keep_third.write_all(get_request("/c")));
  ASSERT_TRUE(keep_third.read_response(&status, &body, &why)) << why;
  ASSERT_EQ(body.size(), 128u) << "两条都挂着，第三条该去唯一空闲的 1 号";

  // 只关掉 2 号上那条（keep_second 拿到的是 192 B 那条配对）。它的上游按 ADR-15
  // 归还进 2 号桶，账回落。
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.pool_returns) == 0u; }, 200))
      << "三条都还空闲在配对，此刻不该有归还";
  keep_second.shutdown_write();
  ASSERT_TRUE(keep_second.wait_for_eof(&why)) << why;
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.pool_returns) == 1u; }, 3000))
      << "关掉的那条下游没有把它的上游归还进桶：账就再也回不来了";

  Client next;
  ASSERT_TRUE(next.connect_to(harness.port(), &why)) << why;
  ASSERT_TRUE(next.write_all(get_request("/d")));
  ASSERT_TRUE(next.read_response(&status, &body, &why)) << why;
  EXPECT_EQ(body.size(), 192u)
      << "释放掉的那条容量没被重新算进去：这就是'账只加不减、永远选第一个成员'那一格";
  EXPECT_EQ(targets.third.connections_accepted(), 1u) << "2 号桶里有热上游，不该再开新的";
  EXPECT_EQ(targets.third.requests_served(), 2u);
  EXPECT_EQ(stats.load(stats.pool_borrows), 1u) << "借那一步没发生：说明它是重开的而不是复用的";

  // 收尾必须**等循环侧真的处理完 FIN**再停环：三条下游各归还一条上游（连同前面 keep_second
  // 那一条，池归还总计 4 次），`constructed == destructed` 才有意义。硬 `close()` 之后立刻
  // `stop_loop()` 会让这条断言读到一个还没被循环看到的连接数——那是收尾顺序错，不是泄漏。
  next.shutdown_write();
  keep_first.shutdown_write();
  keep_third.shutdown_write();
  ASSERT_TRUE(next.wait_for_eof(&why)) << why;
  ASSERT_TRUE(keep_first.wait_for_eof(&why)) << why;
  ASSERT_TRUE(keep_third.wait_for_eof(&why)) << why;
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.pool_returns) == 4u; }, 3000))
      << "四条下游没有各自把上游归还进桶";

  harness.stop_loop();
  EXPECT_EQ(harness.reactor()->pool_idle_count(), 0u) << "停环后排空所有桶";
  EXPECT_EQ(stats.load(stats.live_connections), 0u);
  EXPECT_EQ(stats.load(stats.connections_constructed), stats.load(stats.connections_destructed));
  harness.shutdown_all();
  targets.stop();
}

// ======================= S3-03：健康检查端到端（探活 / 被动 / 安全阀）=======================
//
// 纯函数那一半在 health_test.cc；这里的四条要的是真 socket：
// 探活连接机真的在 owner 循环里发 GET、真的判 404 活 / 5xx 死、真的在摘除后把流量
// 挪到别的成员、真的被安全阀按停。可见性沿用 S3-01 的立场：后端收到什么由
// `TestUpstream` 自报，摘除的账由 HealthTable 的原子计数自报（它的第一个读者就是它）。

namespace {

// 两台后端 + 一个单组 + catch-all 路由 + 健康表的装配具。成员数随 targets 走，
// 不写死 3——安全阀那条要用"两台全死"的最小形状。
struct HealthRig {
  l7gate::route::RouteTable table;
  l7gate::route::GroupTable groups;
  std::shared_ptr<l7gate::route::LbState> state;
  std::shared_ptr<l7gate::route::HealthTable> health;

  void install(Harness::Options* options, Harness* harness, std::size_t target_count,
               l7gate::route::Balance balance, const std::vector<std::string>& endpoint_keys,
               const l7gate::route::HealthPolicy& policy) {
    health = std::make_shared<l7gate::route::HealthTable>(target_count, policy);
    state = std::make_shared<l7gate::route::LbState>(target_count, 1);
    std::uint16_t group = l7gate::route::kNoUpstreamGroup;
    std::vector<std::uint16_t> members;
    for (std::size_t i = 0; i < target_count; ++i) {
      members.push_back(static_cast<std::uint16_t>(i));
    }
    std::string why;
    const bool ok = l7gate::testing::add_balanced_group(&groups, "all", balance, members,
                                                        endpoint_keys, &group, &why);
    EXPECT_TRUE(ok) << why;
    catch_all_to_route_table(&table, group);
    // 健康表必须在 Reactor 构造之前进 config（S3-02 那条"构造期副本"的教训同一格）。
    options->config.health = health;
    options->config.lb_state = state;
    harness->add_filter(std::make_unique<l7gate::filter::RouteFilter>(table));
    harness->add_filter(
        std::make_unique<l7gate::filter::LoadBalanceFilter>(groups, state, health));
  }
};

}  // namespace

// 一、**探活不污染业务计数 + 404 后端活着**（验收两条判据各自的钉子）：
// 探活打在一个固定回 404 的路径上，判据是"任何 4xx 算活着"——这条样本就是
// 误摘率最坏情形的阳性载体。跑若干轮之后：探活有账（下界：探活真的发生了）、
// 没有任何摘除、`requests_parsed == responses_completed == 0`（零业务流量）。
TEST(S3_03_HealthE2E, ProbesKeepA404BackendAliveWithoutTouchingBusinessCounters) {
  std::string why;
  TestUpstream backend;
  ASSERT_TRUE(start_sized_upstream(&backend, 64, &why)) << why;
  backend.set_path_status("/gone", 404);

  Harness harness;
  Harness::Options options = default_options();
  options.with_upstream = false;
  Reactor::Config::Target target;
  target.address = "127.0.0.1";
  target.port = static_cast<std::uint16_t>(backend.port());
  target.health_path = "/gone";
  options.config.upstream_targets.push_back(target);
  options.config.default_target = 0;

  l7gate::route::HealthPolicy policy;
  policy.probe_interval_ms = 30;
  policy.probe_timeout_ms = 500;
  policy.max_fails = 1;
  policy.fail_timeout_ms = 60000;
  policy.max_ejection_percent = 10;
  auto health = std::make_shared<l7gate::route::HealthTable>(1, policy);
  options.config.health = health;

  ASSERT_TRUE(harness.start(options, &why)) << why;
  ReactorStats& stats = harness.reactor()->stats();

  ASSERT_TRUE(wait_until([&] { return health->snapshot(0).probe_ok_total >= 5; }, 3000))
      << why << "（30ms 间隔 3 秒该跑出几十轮，5 轮都没有说明探活机没在转）";
  EXPECT_EQ(health->snapshot(0).ejections_total, 0u) << "404 被判死：一次路径配错引发全量摘除";
  EXPECT_EQ(health->ejected_now(), 0u);
  EXPECT_EQ(stats.load(stats.requests_parsed), 0u);
  EXPECT_EQ(stats.load(stats.responses_completed), 0u) << "探活不许进业务计数";

  // 业务请求照常 200，且业务计数从这一条才开始动。
  Client client;
  ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;
  ASSERT_TRUE(client.write_all(get_request("/business")));
  std::string status;
  std::string body;
  ASSERT_TRUE(client.read_response(&status, &body, &why)) << why;
  EXPECT_EQ(status, "HTTP/1.1 200 OK");
  EXPECT_EQ(body.size(), 64u);
  EXPECT_EQ(stats.load(stats.requests_parsed), 1u);
  EXPECT_EQ(stats.load(stats.responses_completed), 1u);

  client.close();
  harness.stop_loop();
  harness.shutdown_all();
  backend.stop_and_join();
}

// 二、**杀死后端 → 探活判死 → 摘除 → 流量全去活的那台 → 同端口复活 → 探活判活 → 流量回来**。
// fail_timeout 配 60s：恢复只能走探活成功这条路，这条用例因此是"探活能摘也能放"两个方向的
// 共同证据。摘除耗时在这台机器上的量级由 wait_until 的 3 秒预算兜底（正式读数归压测脚本）。
TEST(S3_03_HealthE2E, DeadBackendIsEjectedAndProbeSuccessRevivesIt) {
  std::string why;
  TestUpstream backend0;
  TestUpstream backend1;
  ASSERT_TRUE(start_sized_upstream(&backend0, 64, &why)) << why;
  ASSERT_TRUE(backend1.start(&why)) << why;
  backend1.set_response_body_bytes(128);
  const std::uint16_t port1 = backend1.port();

  Harness harness;
  Harness::Options options = default_options();
  options.with_upstream = false;
  options.config.upstream_targets.push_back(Reactor::Config::Target{
      std::string("127.0.0.1"), static_cast<std::uint16_t>(backend0.port())});
  options.config.upstream_targets.push_back(
      Reactor::Config::Target{std::string("127.0.0.1"), port1});
  options.config.default_target = 0;

  l7gate::route::HealthPolicy policy;
  policy.probe_interval_ms = 30;
  policy.probe_timeout_ms = 500;
  policy.max_fails = 2;
  policy.fail_timeout_ms = 60000;  // 恢复只能靠探活成功
  policy.max_ejection_percent = 50;
  HealthRig rig;
  rig.install(&options, &harness, 2, l7gate::route::Balance::kRoundRobin,
              {"127.0.0.1:" + std::to_string(backend0.port()),
               "127.0.0.1:" + std::to_string(port1)},
              policy);
  ASSERT_TRUE(harness.start(options, &why)) << why;
  auto& health = *rig.health;

  // 预热一条（落到 0 号，游标初值），确认转发面正常。
  Client warm;
  ASSERT_TRUE(warm.connect_to(harness.port(), &why)) << why;
  ASSERT_TRUE(warm.write_all(get_request("/warm")));
  std::string status;
  std::string body;
  ASSERT_TRUE(warm.read_response(&status, &body, &why)) << why;
  ASSERT_EQ(body.size(), 64u);

  const auto killed_at = std::chrono::steady_clock::now();
  backend1.stop_and_join();
  ASSERT_TRUE(wait_until([&] { return health.snapshot(1).ejections_total >= 1; }, 3000))
      << "后端死了却没有被摘除：探活判死或摘除链路至少一头是断的";
  const auto eject_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - killed_at)
                            .count();
  EXPECT_LT(eject_ms, 3000) << "摘除等了太久：30ms 间隔 × 2 次失败不该用秒计";

  // 摘除之后流量只能去 0 号：连发四条全部成功，且没有一条是 128 B（1 号已经死了）。
  for (int i = 0; i < 4; ++i) {
    Client c;
    ASSERT_TRUE(c.connect_to(harness.port(), &why)) << why;
    ASSERT_TRUE(c.write_all(get_request("/after-eject")));
    ASSERT_TRUE(c.read_response(&status, &body, &why)) << why;
    ASSERT_EQ(status, "HTTP/1.1 200 OK");
    ASSERT_EQ(body.size(), 64u) << "第 " << i << " 条被派给了死后端";
    c.close();
  }
  EXPECT_EQ(health.snapshot(1).ejections_total, 1u);

  // 同端口复活：探活成功 → 立即恢复（不等 60s 的 fail_timeout）。
  ASSERT_TRUE(backend1.start(&why, "127.0.0.1", port1)) << why;
  ASSERT_TRUE(wait_until([&] { return health.snapshot(1).recoveries_total >= 1; }, 3000))
      << "探活成功没有把摘除的后端捞回来";
  // 恢复后轮询回到它：连发四条里至少一条是 128 B。
  bool saw_revived = false;
  for (int i = 0; i < 4 && !saw_revived; ++i) {
    Client c;
    ASSERT_TRUE(c.connect_to(harness.port(), &why)) << why;
    ASSERT_TRUE(c.write_all(get_request("/after-revive")));
    ASSERT_TRUE(c.read_response(&status, &body, &why)) << why;
    ASSERT_EQ(status, "HTTP/1.1 200 OK");
    saw_revived = body.size() == 128u;
    c.close();
  }
  EXPECT_TRUE(saw_revived) << "恢复的后端接不到流量：'恢复了'不等于'恢复后接得到流量'";

  warm.close();
  harness.stop_loop();
  harness.shutdown_all();
  backend0.stop_and_join();
  backend1.stop_and_join();
}

// 三、**安全阀**（ADR-12 第一次真正落地的那格）：两台后端都死着、上限 50%
// （floor(2×50/100)=1 台）——摘除必须停在 1 台、第二次摘除请求被挡且有账。
// 断言形状是"最坏只损失 50% 容量"，不是"我没摘除"；挡掉的次数有可观测计数。
TEST(S3_03_HealthE2E, ValveCapsEjectionWhenEveryBackendFails) {
  std::string why;
  // 两台"后端"先起再停：拿到的端口确定没人监听（比写死端口少一次撞车），探活必然
  // connect 失败。
  TestUpstream dead0;
  TestUpstream dead1;
  ASSERT_TRUE(dead0.start(&why)) << why;
  ASSERT_TRUE(dead1.start(&why)) << why;
  const std::uint16_t port0 = dead0.port();
  const std::uint16_t port1 = dead1.port();
  dead0.stop_and_join();
  dead1.stop_and_join();

  Harness harness;
  Harness::Options options = default_options();
  options.with_upstream = false;
  options.config.upstream_targets.push_back(
      Reactor::Config::Target{std::string("127.0.0.1"), port0});
  options.config.upstream_targets.push_back(
      Reactor::Config::Target{std::string("127.0.0.1"), port1});
  options.config.default_target = 0;

  l7gate::route::HealthPolicy policy;
  policy.probe_interval_ms = 30;
  policy.probe_timeout_ms = 200;
  policy.max_fails = 1;
  policy.fail_timeout_ms = 60000;
  policy.max_ejection_percent = 50;
  HealthRig rig;
  rig.install(&options, &harness, 2, l7gate::route::Balance::kRoundRobin,
              {"127.0.0.1:" + std::to_string(port0), "127.0.0.1:" + std::to_string(port1)},
              policy);
  ASSERT_TRUE(harness.start(options, &why)) << why;
  auto& health = *rig.health;

  // 等到第一台被摘、并且第二台的摘除请求至少被挡过两次（两拍探活）。
  ASSERT_TRUE(
      wait_until([&] { return health.ejections_blocked_by_valve() >= 2; }, 3000))
      << "两台全死却没有触发阀：要么没在摘，要么挡了没记账";
  EXPECT_EQ(health.ejected_now(), 1u) << "被摘除容量越过了 floor(N×50%)";
  std::uint64_t total = 0;
  for (std::uint16_t t = 0; t < 2; ++t) total += health.snapshot(t).ejections_total;
  EXPECT_EQ(total, 1u);

  harness.stop_loop();
  harness.shutdown_all();
}

// 四、**被动摘除与到期恢复**（探活关着：interval=0）：业务 5xx 是被动判据的来源，
// max_fails 次之后摘；fail_timeout 到期由循环 sweep 恢复，恢复后流量回来。
// 50% 上限、两台成员 → 两台都允许摘（percent=100 的语义就是"全摘也没关系"）。
TEST(S3_03_HealthE2E, PassiveFiveXxEjectsAndFailTimeoutRevives) {
  std::string why;
  TestUpstream backend0;
  TestUpstream backend1;
  ASSERT_TRUE(start_sized_upstream(&backend0, 64, &why)) << why;
  ASSERT_TRUE(start_sized_upstream(&backend1, 128, &why)) << why;
  backend0.set_path_status("/boom", 503);
  backend1.set_path_status("/boom", 503);

  Harness harness;
  Harness::Options options = default_options();
  options.with_upstream = false;
  options.config.upstream_targets.push_back(Reactor::Config::Target{
      std::string("127.0.0.1"), static_cast<std::uint16_t>(backend0.port())});
  options.config.upstream_targets.push_back(Reactor::Config::Target{
      std::string("127.0.0.1"), static_cast<std::uint16_t>(backend1.port())});
  options.config.default_target = 0;

  l7gate::route::HealthPolicy policy;
  policy.probe_interval_ms = 0;  // 被动专场：不探活
  policy.probe_timeout_ms = 500;
  policy.max_fails = 2;
  policy.fail_timeout_ms = 400;
  policy.max_ejection_percent = 100;
  HealthRig rig;
  rig.install(&options, &harness, 2, l7gate::route::Balance::kRoundRobin,
              {"127.0.0.1:" + std::to_string(backend0.port()),
               "127.0.0.1:" + std::to_string(backend1.port())},
              policy);
  ASSERT_TRUE(harness.start(options, &why)) << why;
  auto& health = *rig.health;

  // 四条 /boom：轮询轮流派，每台各吃 2 次 503 → 各被摘一次。
  Client client;
  ASSERT_TRUE(client.connect_to(harness.port(), &why)) << why;
  std::string status;
  std::string body;
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(client.write_all(get_request("/boom"))) << i;
    ASSERT_TRUE(client.read_response(&status, &body, &why)) << why;
    ASSERT_EQ(status, "HTTP/1.1 503 Probe") << i;
  }
  ASSERT_TRUE(wait_until([&] {
    return health.snapshot(0).ejections_total == 1 && health.snapshot(1).ejections_total == 1;
  }, 3000)) << "两次 5xx 没有把这台后端摘掉：被动判据没接上";
  EXPECT_EQ(health.ejected_now(), 2u);

  // 全员被摘 → 回退全员（ADR-12 的"故意保留一段错误"）：/ok 照样有人接。
  ASSERT_TRUE(client.write_all(get_request("/ok")));
  ASSERT_TRUE(client.read_response(&status, &body, &why)) << why;
  EXPECT_EQ(status, "HTTP/1.1 200 OK");

  // fail_timeout=400ms 到期，循环 sweep 恢复两台。
  ASSERT_TRUE(wait_until([&] {
    return health.snapshot(0).recoveries_total == 1 && health.snapshot(1).recoveries_total == 1;
  }, 3000)) << "到期没有恢复：摘除变成单程票";
  EXPECT_EQ(health.ejected_now(), 0u);

  client.close();
  harness.stop_loop();
  harness.shutdown_all();
  backend0.stop_and_join();
  backend1.stop_and_join();
}

// 四、**一致性哈希端到端**：同一个 key 换一条连接也落在同一个后端。环本身的性质
// （同一 key 恒定、N→N+1 迁移率）在 balance_test.cc 的纯函数那半边断，这一条断的是
// "这一环真的接在链上、key 真的从请求里取"——把 `select_from_ring` 接错参数（比如误用
// 游标）只有这条会红。顺带钉住 ADR-19 警告过的那次倾斜：**一批 key 不许全落在同一个后端上**
// （为什么是 24 个而不是 6 个，见下面那段注释——端点键带着临时端口，样本太小就变成掷硬币）。
TEST(S3_02_LoadBalance, ConsistentHashPinsTheSameKeyAcrossConnections) {
  std::string why;
  ThreeTargets targets;
  ASSERT_TRUE(targets.start(&why)) << why;

  l7gate::route::RouteTable table;
  LbRig rig;
  std::uint16_t group = l7gate::route::kNoUpstreamGroup;
  install_group(&rig.groups, targets, l7gate::route::Balance::kConsistentHash, &group, &why);
  ASSERT_NE(group, l7gate::route::kNoUpstreamGroup) << "组装配失败：" << why;
  catch_all_to_route_table(&table, group);

  Harness harness;
  Harness::Options options = default_options();
  targets.fill(&options);
  harness.add_filter(std::make_unique<l7gate::filter::RouteFilter>(table));
  rig.arm(3);
  rig.install(&options, &harness);
  ASSERT_TRUE(harness.start(options, &why)) << why;
  ReactorStats& stats = harness.reactor()->stats();

  const std::vector<std::string> paths{"/orders/1", "/orders/2", "/orders/3", "/orders/4",
                                       "/orders/5", "/orders/6", "/orders/7", "/orders/8",
                                       "/users/1",  "/users/2",  "/users/3",  "/users/4",
                                       "/users/5",  "/users/6",  "/users/7",  "/users/8",
                                       "/api/1",    "/api/2",    "/api/3",    "/api/4",
                                       "/api/5",    "/api/6",    "/health",   "/healthz"};
  std::vector<std::size_t> first_round;
  for (int round = 0; round < 2; ++round) {
    for (std::size_t i = 0; i < paths.size(); ++i) {
      Client client;  // **每条都换一条新连接**：粘住的是 key，不是连接
      ASSERT_TRUE(client.connect_to(harness.port(), &why)) << paths[i];
      ASSERT_TRUE(client.write_all(get_request(paths[i]))) << paths[i];
      std::string status;
      std::string body;
      ASSERT_TRUE(client.read_response(&status, &body, &why)) << paths[i];
      if (round == 0) {
        first_round.push_back(body.size());
      } else {
        EXPECT_EQ(body.size(), first_round[i])
            << paths[i] << " 第二轮换了后端：粘住的不是 key（`select_from_ring` 接错了参数）";
      }
      client.close();
    }
  }
  std::size_t distinct = 0;
  for (const std::size_t size : {std::size_t{64}, std::size_t{128}, std::size_t{192}}) {
    if (std::find(first_round.begin(), first_round.end(), size) != first_round.end()) ++distinct;
  }
  // **为什么是 24 个 key**：端点键里有内核分配的临时端口，所以环的位置每轮都不一样，
  // 6 个 key 的"别全钉一台"会变成一枚硬币（全部落到同一台的概率按均匀弧算是 (1/3)^6 ≈ 0.1%，
  // 而弧长并不均匀）。24 个 key 把这个概率压到可以当成断言用。**分布本身只打印不裁决**：
  // 弧长天然不等，这里的倾斜不是缺陷的证据；统计意义上的判据是 balance_test.cc 那条
  // 10,000 个 key 的迁移率（那边端点键是写死的，所以读数可复现）。
  std::printf("[S3-02 hash] %zu 个 key 的落点：64B=%zu 128B=%zu 192B=%zu，用到 %zu 台后端\n",
              first_round.size(),
              static_cast<std::size_t>(std::count(first_round.begin(), first_round.end(), 64)),
              static_cast<std::size_t>(std::count(first_round.begin(), first_round.end(), 128)),
              static_cast<std::size_t>(std::count(first_round.begin(), first_round.end(), 192)),
              distinct);
  EXPECT_GE(distinct, 2u) << "24 个 key 全落在同一个后端上：这就是 ADR-19 说的那次哈希倾斜";
  // 开过几条上游 = 用了几台后端。第二轮一条都不该新开（key 没变、桶里有热的）。
  EXPECT_EQ(stats.load(stats.upstream_opened), distinct) << "同一个 key 的第二轮重开了上游连接";

  // 48 次请求（两轮 × 24 个 key），每次都在一条独立的下游上，收尾各归还一条上游。
  // 等这个非稳态量进稳态再停环（口径⑤）。**不能等 `live_connections == 0`**：池里那几条
  // 热上游本身就计入 live，它们在停环排空池之前归不到 0——那是 1.18 的语义不是漏收尾。
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.pool_returns) == paths.size() * 2; }, 3000))
      << "48 次请求没有各自把上游归还进桶";

  harness.stop_loop();
  EXPECT_EQ(harness.reactor()->pool_idle_count(), 0u) << "停环后仍有桶没排空";
  EXPECT_EQ(stats.load(stats.live_connections), 0u);
  EXPECT_EQ(stats.load(stats.connections_constructed), stats.load(stats.connections_destructed));
  harness.shutdown_all();
  targets.stop();
}

// ===========================================================================
//
// 这一组只测一件事：**第 i 条 worker 的回源目的地址是第 i mod E 个端点，而且它一辈子只用
// 那一个**。端点靠"响应体长度不同"来区分——两个 TestUpstream 各自 set_response_body_bytes
// ======================= S3-04：节点表 reload 端到端（不依赖 Docker）=======================
//
// reload 全链路里不依赖 MySQL 的那一段：快照发布 → worker 对账 tick 追加目标 →
// 新成员接得到流量 → 删除的成员不再接流量 → ever-seen 下标复用。真 MySQL 的
// fetch 与行变更在首次连通取证与 scripts/smoke-s3-04.sh 里。

namespace {

// 把 registry 的当前目标表翻成 Reactor::Config::Target（main.cc 同一个搬运，
// 测试里再抄一份是因为夹具不经过 main.cc）。
std::vector<Reactor::Config::Target> targets_from_registry(const l7gate::route::TargetRegistry& registry) {
  std::vector<Reactor::Config::Target> out;
  for (std::uint16_t i = 0; i < registry.size(); ++i) {
    const auto* node = registry.at(i);
    Reactor::Config::Target target;
    if (node == nullptr) break;
    target.address = node->address;
    target.port = node->port;
    target.health_path = node->health_path;
    out.push_back(target);
  }
  return out;
}

}  // namespace

TEST(S3_04_NodesE2E, ReloadAddsRemovesAndReAddsMembersWithoutRestart) {
  std::string why;
  TestUpstream backend0;
  TestUpstream backend1;
  TestUpstream backend2;
  ASSERT_TRUE(start_sized_upstream(&backend0, 64, &why)) << why;
  ASSERT_TRUE(start_sized_upstream(&backend1, 128, &why)) << why;
  ASSERT_TRUE(start_sized_upstream(&backend2, 192, &why)) << why;

  // 文件侧的组（catch-all 指一个 rr 组）+ 注册表初始只有 0/1 两台。
  l7gate::route::RouteTable table;
  l7gate::route::GroupTable file_groups;
  std::uint16_t group = l7gate::route::kNoUpstreamGroup;
  ASSERT_TRUE(file_groups.add_group("all", l7gate::route::Balance::kRoundRobin, &group));
  auto publisher = std::make_shared<l7gate::route::NodesPublisher>();
  auto registry = std::make_shared<l7gate::route::TargetRegistry>();
  registry->intern({"127.0.0.1", static_cast<std::uint16_t>(backend0.port())});
  registry->intern({"127.0.0.1", static_cast<std::uint16_t>(backend1.port())});
  auto publish_members = [&](const std::vector<std::uint16_t>& ports) {
    l7gate::route::NodesFetch fetch;
    l7gate::route::NodesFetch::GroupMembers members;
    members.name = "all";
    for (const std::uint16_t port : ports) {
      members.members.push_back({"127.0.0.1", port});
    }
    fetch.groups.push_back(std::move(members));
    std::string build_error;
    auto view = l7gate::route::build_nodes_view(file_groups, fetch, *registry, &build_error);
    EXPECT_NE(view, nullptr) << build_error;
    publisher->publish(std::move(view));
  };

  Harness harness;
  Harness::Options options = default_options();
  options.with_upstream = false;
  options.config.upstream_targets = targets_from_registry(*registry);
  options.config.default_target = 0;
  l7gate::route::HealthPolicy policy;  // 探活关着：这一条只证 reload 的成员增删
  policy.probe_interval_ms = 0;
  auto health = std::make_shared<l7gate::route::HealthTable>(options.config.upstream_targets.size(),
                                                             policy, /*capacity=*/16);
  auto state = std::make_shared<l7gate::route::LbState>(options.config.upstream_targets.size(), 1);
  options.config.health = health;
  options.config.nodes = publisher;
  options.config.lb_state = state;
  ASSERT_TRUE(table.add_prefix("", group));
  harness.add_filter(std::make_unique<l7gate::filter::RouteFilter>(table));
  harness.add_filter(std::make_unique<l7gate::filter::LoadBalanceFilter>(
      file_groups, state, health, publisher));
  ASSERT_TRUE(harness.start(options, &why)) << why;
  publish_members({static_cast<std::uint16_t>(backend0.port()),
                   static_cast<std::uint16_t>(backend1.port())});

  // 基线：成员 {0,1}，轮询两条都通。
  Client c0;
  ASSERT_TRUE(c0.connect_to(harness.port(), &why)) << why;
  ASSERT_TRUE(c0.write_all(get_request("/warm")));
  std::string status;
  std::string body;
  ASSERT_TRUE(c0.read_response(&status, &body, &why)) << why;
  ASSERT_TRUE(body.size() == 64u || body.size() == 128u) << "基线响应不属于已知成员";
  c0.close();

  // 增员：第三个成员进表 → 不重启，流量必须到达它（对账 tick + ensure_upstream 懒对账）。
  publish_members({static_cast<std::uint16_t>(backend0.port()),
                   static_cast<std::uint16_t>(backend1.port()),
                   static_cast<std::uint16_t>(backend2.port())});
  bool reached_new = false;
  for (int i = 0; i < 50 && !reached_new; ++i) {
    Client c;
    ASSERT_TRUE(c.connect_to(harness.port(), &why)) << why;
    ASSERT_TRUE(c.write_all(get_request("/grow")));
    ASSERT_TRUE(c.read_response(&status, &body, &why)) << why;
    ASSERT_EQ(status, "HTTP/1.1 200 OK");
    reached_new = body.size() == 192u;
    c.close();
  }
  EXPECT_TRUE(reached_new) << "reload 50 轮之后新成员仍接不到流量：发布/对账链路有一头断了";
  EXPECT_EQ(backend2.connections_accepted() >= 1, true);

  // 删员：0 号出表 → 它不再接流量（1/2 继续服务）。
  publish_members({static_cast<std::uint16_t>(backend1.port()),
                   static_cast<std::uint16_t>(backend2.port())});
  const std::uint64_t served0_before = backend0.requests_served();
  for (int i = 0; i < 10; ++i) {
    Client c;
    ASSERT_TRUE(c.connect_to(harness.port(), &why)) << why;
    ASSERT_TRUE(c.write_all(get_request("/shrink")));
    ASSERT_TRUE(c.read_response(&status, &body, &why)) << why;
    ASSERT_EQ(status, "HTTP/1.1 200 OK");
    ASSERT_NE(body.size(), 64u) << "被删的成员还在接流量";
    c.close();
  }
  EXPECT_EQ(backend0.requests_served(), served0_before) << "被删成员的计数不该再涨";

  // 复员：0 号回到表里 → ever-seen 下标复用，流量回到它。
  publish_members({static_cast<std::uint16_t>(backend0.port()),
                   static_cast<std::uint16_t>(backend1.port()),
                   static_cast<std::uint16_t>(backend2.port())});
  bool re_reached = false;
  for (int i = 0; i < 50 && !re_reached; ++i) {
    Client c;
    ASSERT_TRUE(c.connect_to(harness.port(), &why)) << why;
    ASSERT_TRUE(c.write_all(get_request("/regrow")));
    ASSERT_TRUE(c.read_response(&status, &body, &why)) << why;
    ASSERT_EQ(status, "HTTP/1.1 200 OK");
    re_reached = body.size() == 64u;
    c.close();
  }
  EXPECT_TRUE(re_reached) << "重新到岗的成员接不到流量：ever-seen 复用断了";

  harness.stop_loop();
  harness.shutdown_all();
  backend0.stop_and_join();
  backend1.stop_and_join();
  backend2.stop_and_join();
}



// ======================= S3-05：故障注入矩阵——ctest 里能跑的两格 =======================
//
// 五类故障里，"connect refused / 5xx / 探活失败"的机制已由 S3_03_HealthE2E 三条钉过
// （connect 失败、被动 5xx、404 活）；这里补 S3-05 矩阵缺的两格：
//   半开（连上不回字节）——探活超时判死的路径 + 解除后恢复；
//   慢响应但在探活超时内——**不摘**（"慢"与"死"的分界线就是 probe_timeout）。
// 每类的真进程摘除/恢复耗时实测在 scripts/fault/s3-05-matrix.sh 与 FAULT-INJECTION §12。

TEST(S3_05_FaultMatrix, HalfOpenStallTimesOutProbesAndRecoversOnStallExpiry) {
  std::string why;
  TestUpstream backend0;
  TestUpstream backend1;
  ASSERT_TRUE(start_sized_upstream(&backend0, 64, &why)) << why;
  ASSERT_TRUE(start_sized_upstream(&backend1, 128, &why)) << why;

  Harness harness;
  Harness::Options options = default_options();
  options.with_upstream = false;
  options.config.upstream_targets.push_back(Reactor::Config::Target{
      std::string("127.0.0.1"), static_cast<std::uint16_t>(backend0.port())});
  options.config.upstream_targets.push_back(Reactor::Config::Target{
      std::string("127.0.0.1"), static_cast<std::uint16_t>(backend1.port())});
  options.config.default_target = 0;

  l7gate::route::HealthPolicy policy;
  policy.probe_interval_ms = 50;
  policy.probe_timeout_ms = 200;
  policy.max_fails = 2;
  policy.fail_timeout_ms = 60000;  // 恢复只能靠探活成功：耗时测的就是这条链
  policy.max_ejection_percent = 50;
  HealthRig rig;
  rig.install(&options, &harness, 2, l7gate::route::Balance::kRoundRobin,
              {"127.0.0.1:" + std::to_string(backend0.port()),
               "127.0.0.1:" + std::to_string(backend1.port())},
              policy);
  ASSERT_TRUE(harness.start(options, &why)) << why;
  auto& health = *rig.health;

  // 半开：accept 照旧、一个字节都不读不回——探活 connect 成功但等不到响应，
  // 每次探活吃满 probe_timeout 再判死。max_fails=2 → 约 2×(200+50)ms 后摘除。
  backend1.set_stall_reads(true);
  const auto stalled_at = std::chrono::steady_clock::now();
  ASSERT_TRUE(wait_until([&] { return health.snapshot(1).ejections_total >= 1; }, 5000))
      << "半开的后端没有被摘除：探活超时判死这条路断了";
  const auto eject_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - stalled_at)
                            .count();
  EXPECT_LT(eject_ms, 5000);
  // 摘除后流量只去 0 号：连发四条全部 64 B。
  std::string status;
  std::string body;
  for (int i = 0; i < 4; ++i) {
    Client c;
    ASSERT_TRUE(c.connect_to(harness.port(), &why)) << why;
    ASSERT_TRUE(c.write_all(get_request("/after-eject")));
    ASSERT_TRUE(c.read_response(&status, &body, &why)) << why;
    ASSERT_EQ(body.size(), 64u) << "半开后端还在接流量";
    c.close();
  }

  // 解除 stall：下一次探活成功立即恢复（不等 60s 的 fail_timeout）。
  backend1.set_stall_reads(false);
  ASSERT_TRUE(wait_until([&] { return health.snapshot(1).recoveries_total >= 1; }, 5000))
      << "解除后探活成功没有恢复：恢复链路断了";
  bool resumed = false;
  for (int i = 0; i < 50 && !resumed; ++i) {
    Client c;
    ASSERT_TRUE(c.connect_to(harness.port(), &why)) << why;
    ASSERT_TRUE(c.write_all(get_request("/after-recover")));
    ASSERT_TRUE(c.read_response(&status, &body, &why)) << why;
    ASSERT_EQ(status, "HTTP/1.1 200 OK");
    resumed = body.size() == 128u;
    c.close();
  }
  EXPECT_TRUE(resumed) << "恢复的成员接不到流量";

  harness.stop_loop();
  harness.shutdown_all();
  backend0.stop_and_join();
  backend1.stop_and_join();
}

TEST(S3_05_FaultMatrix, SlowWithinProbeTimeoutStaysAlive) {
  std::string why;
  TestUpstream backend0;
  TestUpstream backend1;
  ASSERT_TRUE(start_sized_upstream(&backend0, 64, &why)) << why;
  ASSERT_TRUE(start_sized_upstream(&backend1, 128, &why)) << why;
  // 慢 100ms：探活等得起（probe_timeout 500ms），业务请求也等得起。
  backend1.set_delay_before_response_ms(100);

  Harness harness;
  Harness::Options options = default_options();
  options.with_upstream = false;
  options.config.upstream_targets.push_back(Reactor::Config::Target{
      std::string("127.0.0.1"), static_cast<std::uint16_t>(backend0.port())});
  options.config.upstream_targets.push_back(Reactor::Config::Target{
      std::string("127.0.0.1"), static_cast<std::uint16_t>(backend1.port())});
  options.config.default_target = 0;

  l7gate::route::HealthPolicy policy;
  policy.probe_interval_ms = 50;
  policy.probe_timeout_ms = 500;
  policy.max_fails = 2;
  policy.fail_timeout_ms = 60000;
  policy.max_ejection_percent = 50;
  HealthRig rig;
  rig.install(&options, &harness, 2, l7gate::route::Balance::kRoundRobin,
              {"127.0.0.1:" + std::to_string(backend0.port()),
               "127.0.0.1:" + std::to_string(backend1.port())},
              policy);
  ASSERT_TRUE(harness.start(options, &why)) << why;
  auto& health = *rig.health;

  // 跑 ≥10 轮探活（500ms 以上），慢后端一次都不该被摘——"慢"与"死"的分界线就是
  // probe_timeout，这条线之内网关不表态（判断留给 S4 的请求超时格子）。
  ASSERT_TRUE(wait_until([&] {
    return health.snapshot(1).probe_ok_total >= 10;
  }, 5000)) << "探活对慢后端没有成功过：delay 100ms 不该撞 500ms 的超时";
  EXPECT_EQ(health.snapshot(1).ejections_total, 0u) << "超时线之内的慢被当成死了";
  EXPECT_EQ(health.ejected_now(), 0u);
  // 业务照样通：轮询两条都拿到响应。
  std::string status;
  std::string body;
  for (int i = 0; i < 4; ++i) {
    Client c;
    ASSERT_TRUE(c.connect_to(harness.port(), &why)) << why;
    ASSERT_TRUE(c.write_all(get_request("/slow-but-alive")));
    ASSERT_TRUE(c.read_response(&status, &body, &why)) << why;
    ASSERT_EQ(status, "HTTP/1.1 200 OK");
    c.close();
  }

  harness.stop_loop();
  harness.shutdown_all();
  backend0.stop_and_join();
  backend1.stop_and_join();
}

}  // namespace l7gate::net

