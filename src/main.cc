// 命令行入口。
//
// S1-01 时这里只有一个 --self-check，因为我拒绝塞一个"能监听端口但什么都不做"的
// 假壳：那会让除了 --self-check 之外的一切输出看起来像进度，而它不是。
// S1-04 之后 --serve 有了实质内容——一条真的会把字节从下游搬到上游、再搬回来的路径。
// 它仍然不做 HTTP 语义：不回 4xx、不改写响应、没有路由和 filter（那些分别归
// S1-06/S1-08），所以它的定位是**冒烟与验收的载体**，不是一个可用的网关。
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <string_view>

#include "core/build_info.h"
#include "net/reactor.h"

namespace {

void usage(std::FILE* out) {
  std::fprintf(out,
      "l7gate —— L7 流量治理网关（开发中）\n"
      "用法: l7gate --self-check\n"
      "      l7gate --serve --listen <ipv4:port> --upstream <ipv4:port>\n"
      "               [--serve-requests N] [--max-connections N]\n"
      "\n"
      "--serve 是 S1-04 的透传路径：epoll 边缘触发单线程循环，下游请求原样搬到上游，\n"
      "上游响应原样搬回来，keep-alive 复用同一条回源连接。它不做状态码映射、不改写\n"
      "响应、不解析 Upgrade（S1-06/S1-05），畸形请求只关连接不回 400。\n"
      "N 为正整数。--serve-requests 是「解析满 N 条就停循环」，它可能在最后一条响应\n"
      "送达之前就 break —— 要「把 N 条全部送完再停」就用信号，冒烟脚本就是这么做的。\n"
      "SIGINT/SIGTERM 走同一条优雅停止：事件循环停下、统计打印出来、连接按六步关闭。\n"
      "统计每行一个 [serve] key=value，给脚本读，不是给人看的漂亮表格。\n");
}

// --- /proc 自报 ---------------------------------------------------------
// 冒烟脚本要的是「10000 请求之后 fd 回到基线、每连接内存是多少」，而这两个数
// 只有进程自己看 /proc 才拿得准：外部工具在别的时刻看，看到的可能是别的连接。
// 读不到就报 -1，绝不拿 0 冒充"没有泄漏"。
std::int64_t count_open_fds() {
  DIR* dir = ::opendir("/proc/self/fd");
  if (dir == nullptr) return -1;
  std::int64_t n = 0;
  while (const dirent* entry = ::readdir(dir)) {
    if (entry->d_name[0] == '.') continue;
    ++n;
  }
  ::closedir(dir);
  return n;
}

std::int64_t current_rss_kib() {
  std::FILE* file = std::fopen("/proc/self/status", "r");
  if (file == nullptr) return -1;
  char line[256];
  std::int64_t rss = -1;
  while (std::fgets(line, sizeof(line), file)) {
    if (std::strncmp(line, "VmRSS:", 6) == 0) {
      rss = std::strtoll(line + 6, nullptr, 10);
      break;
    }
  }
  std::fclose(file);
  return rss;
}

// --- 停止信号 -----------------------------------------------------------
// 冒烟脚本要在 curl 收完最后一条响应之后让网关**自己**收尾：kill -TERM 的默认
// 处置是直接终止进程，那样统计打印不出来、连接也不会走那六步关闭——而"六步关闭
// 在真实进程里也走得通"恰恰是这一步要的证据。
//
// 处理函数里只做两件跨信号边界的事：读一个全局原子指针、调 Reactor::stop()。
// stop() 内部是一次 relaxed 原子 store 加一个 ::write(eventfd)——write(2) 在
// POSIX 的 async-signal-safe 名单上，而 lock-free 原子的 store 在 x86-64 上就是
// 一条 mov。没有锁、没有分配、没有 stdio。连续两个 SIGTERM 时第二个可能打断
// 第一个的 write，最坏结果是 eventfd 计数变 2、循环多醒一次，不是坏状态。
std::atomic<l7gate::net::Reactor*> g_reactor_for_signal{nullptr};

void on_stop_signal(int) {
  l7gate::net::Reactor* reactor = g_reactor_for_signal.load(std::memory_order_relaxed);
  if (reactor != nullptr) {
    reactor->stop();
  }
}

bool parse_addr_port(std::string_view spec, std::string* addr, std::uint16_t* port) {
  // 按最后一个 ':' 切：IPv4 字面量里没有冒号，这个规则够用，而且将来换成
  // "[::1]:80" 这种形态时，需要改的只有这一处（S3 的 IPv6 决定之后再动）。
  const std::size_t sep = spec.rfind(':');
  if (sep == std::string_view::npos || sep + 1 >= spec.size()) return false;
  *addr = std::string(spec.substr(0, sep));
  const std::string digits(spec.substr(sep + 1));
  errno = 0;
  char* end = nullptr;
  const long value = std::strtol(digits.c_str(), &end, 10);
  if (errno != 0 || end == digits.c_str() || *end != '\0' || value <= 0 || value > 65535) {
    return false;
  }
  *port = static_cast<std::uint16_t>(value);
  return !addr->empty();
}

bool parse_number(const char* text, long long* out) {
  errno = 0;
  char* end = nullptr;
  const long long value = std::strtoll(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0') return false;
  *out = value;
  return true;
}

// 只在循环退出之后调用：ReactorStats 全原子，但这份快照要的是"这一次运行的最终账"，
// 在循环还在跑的时候打印会得到一个中间态，那种数字最容易被当成结论抄进文档。
void report_stats(l7gate::net::Reactor& reactor, std::int64_t fd_start, std::int64_t rss_start,
                  long long elapsed_ms) {
  l7gate::net::ReactorStats& stats = reactor.stats();
  const std::int64_t fd_now = count_open_fds();
  const std::int64_t rss_now = current_rss_kib();
  // 一行一个 key=value，键名用 ASCII。理由不是审美：这份输出是冒烟脚本的输入，
  // 一旦一行里塞两个数，脚本就只能靠"哪个中文标签先出现"去猜，而猜出来的解析
  // 在中文标签改字的那一天会静悄悄地把错数字读走。人类可读的分组版留给文档抄写。
  auto put = [](std::string_view key, long long value) {
    std::printf("[serve] %s=%lld\n", std::string(key).c_str(), value);
  };
  put("elapsed_ms", elapsed_ms);
  put("downstream_accepted", static_cast<long long>(stats.load(stats.downstream_accepted)));
  put("downstream_rejected_over_capacity",
      static_cast<long long>(stats.load(stats.downstream_rejected_over_capacity)));
  put("accept_errors", static_cast<long long>(stats.load(stats.accept_errors)));
  put("upstream_opened", static_cast<long long>(stats.load(stats.upstream_opened)));
  put("upstream_connect_failed", static_cast<long long>(stats.load(stats.upstream_connect_failed)));
  put("requests_parsed", static_cast<long long>(stats.load(stats.requests_parsed)));
  put("requests_rejected", static_cast<long long>(stats.load(stats.requests_rejected)));
  put("connections_constructed", static_cast<long long>(stats.load(stats.connections_constructed)));
  put("connections_destructed", static_cast<long long>(stats.load(stats.connections_destructed)));
  // 这个数在循环已经不再处理事件之后读，所以它就是"没人管的连接"——冒烟脚本要
  // 断言的正是它等于 0。
  put("live_connections", static_cast<long long>(stats.load(stats.live_connections)));
  put("connections_closed", static_cast<long long>(stats.load(stats.connections_closed)));
  put("bytes_down_to_upstream", static_cast<long long>(stats.load(stats.bytes_down_to_upstream)));
  put("bytes_up_to_downstream", static_cast<long long>(stats.load(stats.bytes_up_to_downstream)));
  put("header_scan_bytes", static_cast<long long>(stats.load(stats.header_scan_bytes)));
  put("parse_calls", static_cast<long long>(stats.load(stats.parse_calls)));
  put("outbound_buffers_created",
      static_cast<long long>(stats.load(stats.outbound_buffers_created)));
  put("scratches_created", static_cast<long long>(stats.load(stats.scratches_created)));
  put("high_water_hits", static_cast<long long>(stats.load(stats.high_water_hits)));
  put("epoll_registrations", static_cast<long long>(reactor.epoll_registrations()));
  put("epoll_unregistrations", static_cast<long long>(reactor.epoll_unregistrations()));
  put("epoll_modifications", static_cast<long long>(reactor.epoll_modifications()));
  // fd 这里含本进程自己打开的 stdout 之类，所以看的是**增量**，不是绝对值；
  // 而且这个数在 Reactor 析构之前读，监听 fd/epoll/eventfd 三个常驻项两边都在。
  put("fd_start", static_cast<long long>(fd_start));
  put("fd_end", static_cast<long long>(fd_now));
  put("rss_start_kib", static_cast<long long>(rss_start));
  put("rss_end_kib", static_cast<long long>(rss_now));
}

int serve(int argc, char** argv) {
  l7gate::net::Reactor::Config config;
  std::size_t stop_after_requests = 0;
  bool have_listen = false;
  bool have_upstream = false;

  for (int i = 2; i < argc; ++i) {
    const std::string_view flag = argv[i];
    const char* value = (i + 1 < argc) ? argv[i + 1] : nullptr;
    if (flag == "--listen" && value != nullptr) {
      if (!parse_addr_port(value, &config.listen_address, &config.listen_port)) {
        std::fprintf(stderr, "参数错误: --listen 要的是 <ipv4:port>，收到 %s\n", value);
        return 2;
      }
      have_listen = true;
      ++i;
    } else if (flag == "--upstream" && value != nullptr) {
      if (!parse_addr_port(value, &config.upstream_address, &config.upstream_port)) {
        std::fprintf(stderr, "参数错误: --upstream 要的是 <ipv4:port>，收到 %s\n", value);
        return 2;
      }
      have_upstream = true;
      ++i;
    } else if (flag == "--serve-requests" && value != nullptr) {
      long long n = 0;
      if (!parse_number(value, &n) || n < 0) {
        std::fprintf(stderr, "参数错误: --serve-requests 要的是非负整数，收到 %s\n", value);
        return 2;
      }
      stop_after_requests = static_cast<std::size_t>(n);
      ++i;
    } else if (flag == "--max-connections" && value != nullptr) {
      long long n = 0;
      if (!parse_number(value, &n) || n <= 0) {
        std::fprintf(stderr, "参数错误: --max-connections 要的是正整数，收到 %s\n", value);
        return 2;
      }
      config.max_connections = static_cast<std::size_t>(n);
      ++i;
    } else {
      std::fprintf(stderr, "参数错误: 无法识别的选项 %s\n", std::string(flag).c_str());
      usage(stderr);
      return 2;
    }
  }
  config.stop_after_requests = stop_after_requests;

  // 没有 --idle-timeout 这一项，是我故意不加的：S1-04 还没有空闲连接回收
  // （超时策略归 S4），而现在这个 wait_timeout_ms 控制的是 epoll_wait 的醒来间隔，
  // 也就是"停循环最坏等多久"。把一个内部参数包装成"--idle-timeout-ms"给用户，
  // 等于用命令行给还没实现的能力发一张存在的证明。
  if (!have_listen || !have_upstream) {
    std::fprintf(stderr, "参数错误: --serve 需要同时给 --listen 和 --upstream\n");
    return 2;
  }

  l7gate::net::Reactor reactor(config);
  int error = 0;
  if (!reactor.start_listen(&error)) {
    std::fprintf(stderr, "监听 %s:%u 失败: errno=%d (%s)\n", config.listen_address.c_str(),
                 config.listen_port, error, std::strerror(error));
    return 1;
  }
  // 指针先于 handler 安装为空，反过来也一样安全：on_stop_signal 里判过空。
  g_reactor_for_signal.store(&reactor, std::memory_order_relaxed);
  std::signal(SIGINT, on_stop_signal);
  std::signal(SIGTERM, on_stop_signal);

  const std::int64_t fd_start = count_open_fds();
  const std::int64_t rss_start = current_rss_kib();
  std::printf("[serve] 监听 %s:%d（backlog=%d max_connections=%zu upstream=%s:%u "
              "stop_after_requests=%zu wait_timeout_ms=%d）\n",
              config.listen_address.c_str(), reactor.listen_port(), config.listen_backlog,
              config.max_connections, config.upstream_address.c_str(), config.upstream_port,
              stop_after_requests, config.wait_timeout_ms);
  std::fflush(stdout);

  const auto started = std::chrono::steady_clock::now();
  const std::uint64_t batches = reactor.run();
  g_reactor_for_signal.store(nullptr, std::memory_order_relaxed);
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - started)
                              .count();

  // 这里还没析构 Reactor，所以"存活连接数"是这次运行真正的终局读数：
  // 循环已经不再处理事件，剩下的每一个都是没人管的。
  report_stats(reactor, fd_start, rss_start, static_cast<long long>(elapsed_ms));
  std::printf("[serve] event_batches=%llu\n", static_cast<unsigned long long>(batches));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--self-check") == 0) {
    // 压测脚本用它把环境事实落到 BENCHMARK.md，避免手抄。
    std::printf("%s\n", l7gate::core::render_one_line(l7gate::core::current()).c_str());
    return 0;
  }
  if (argc >= 2 && std::strcmp(argv[1], "--serve") == 0) {
    return serve(argc, argv);
  }
  if (argc == 2 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0)) {
    usage(stdout);
    return 0;
  }

  usage(stderr);
  std::fprintf(stderr, "状态: --serve 是 S1-04 的透传路径，尚无 HTTP 语义（见 --help）\n");
  return 2;
}
