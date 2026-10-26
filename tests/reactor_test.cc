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

#include "net/reactor.h"
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
      config.upstream_port = upstream_.port();
    }
    // 端口一律给 0 让内核选：测试之间并行跑、失败重跑都不该因为端口占用而随机失败。
    config.listen_port = 0;
    reactor_ = std::make_unique<Reactor>(config);
    int error = 0;
    if (!reactor_->start_listen(&error)) {
      *why = "start_listen failed errno=" + std::to_string(error);
      return false;
    }
    port_ = reactor_->listen_port();
    thread_ = std::thread([this] { batches_ = reactor_->run(); });
    return true;
  }

  void stop_loop() {
    if (reactor_ != nullptr) reactor_->stop();
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
  // 这是内核给的第一手证据，live_connections 归零只是它的副产品。
  client.shutdown_write();
  ASSERT_TRUE(client.wait_for_eof(&why))
      << "客户端只发了 FIN，网关没有把这侧连接关掉：" << why;
  ReactorStats& stats = harness.reactor()->stats();
  // 这里的顺序是刻意的：live_connections 是"已构造 - 已析构"，它在延后析构下
  // 会短暂停在 2（fd 已关、对象还挂在 retiring_ 里）。所以等的不是"有没有关"，
  // 而是同一批事件处理里那最后一步收尾 —— 关掉这件事我已经用 wait_for_eof
  // 从内核那里看到证据了。
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.live_connections) == 0u; }, 5000))
      << "客户端已经关闭，但还有连接对象没析构：关闭路径漏了某一步，或两个对象互相抱着";

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
  EXPECT_EQ(opened, 1u) << "全程一条回源连接，没有每请求新建";
  EXPECT_EQ(constructed, 2u) << "下游 1 + 上游 1";
  EXPECT_EQ(destructed, 2u) << "两个都析构了：shared_ptr 引用计数确实归零";
  EXPECT_EQ(live, 0u);
  EXPECT_EQ(closed, 2u);

  harness.stop_loop();  // join 之后才允许读非原子的 epoll 计数
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
  ASSERT_TRUE(wait_until([&] { return stats.load(stats.live_connections) == 0u; }, 5000));
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

}  // namespace
}  // namespace l7gate::net

