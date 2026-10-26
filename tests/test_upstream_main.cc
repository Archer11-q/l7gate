// 把仓库内的测试上游单独编译成一个可执行文件，给 scripts/smoke-s1-04.sh 用。
//
// 为什么不直接用 python3 -m http.server 或 nginx：
//   一、nginx 没装，而且它属于 S3 基线的锁定项，提前装只会让 S1-04 的上游行为
//       跟着一个还没到决策点的版本走；
//   二、http.server 的 keep-alive 语义和连接关闭时机不由我控制，冒烟失败时我
//       分不清是网关坏还是它坏——那正是测试对端最不该有的性质；
//   三、这个文件用的类和单元测试是同一个（tests/test_upstream.h），所以不会出现
//       "测试通过、冒烟失败"这种两边上游语义不一致的分裂。
//
// 端口默认给 0（内核选），启动后把**实际端口**打印出来，由脚本去读。我自己挑一个
// "看起来没人用"的端口，迟早会被别的进程 or 上一次没清干净的残留占用。
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>

#include "test_upstream.h"

namespace {

std::atomic<bool> g_stop_requested{false};

void on_signal(int) { g_stop_requested.store(true, std::memory_order_relaxed); }

bool parse_addr_port(std::string_view spec, std::string* addr, std::uint16_t* port) {
  const std::size_t sep = spec.rfind(':');
  if (sep == std::string_view::npos) return false;
  *addr = std::string(spec.substr(0, sep));
  const std::string digits(spec.substr(sep + 1));
  errno = 0;
  char* end = nullptr;
  const long value = std::strtol(digits.c_str(), &end, 10);
  if (errno != 0 || end == digits.c_str() || *end != '\0' || value < 0 || value > 65535) {
    return false;
  }
  *port = static_cast<std::uint16_t>(value);  // 0 是合法值：交给内核
  return !addr->empty();
}

bool parse_bounded(const char* text, long min, long max, long* out) {
  errno = 0;
  char* end = nullptr;
  const long value = std::strtol(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || value < min || value > max) return false;
  *out = value;
  return true;
}

void usage(std::FILE* out) {
  std::fprintf(out,
      "l7gate_test_upstream —— 冒烟/联调用极简上游（只服务测试，不参与任何 SLO 测量）\n"
      "用法: l7gate_test_upstream [--listen <ipv4:port>]   (端口 0 = 内核选，默认)\n"
      "               [--body-bytes N] [--delay-ms N] [--close-after-first-response]\n"
      "行为: 凑齐一个完整请求（\\r\\n\\r\\n 再加 Content-Length 那么多 body）就回一段\n"
      "      Content-Length 已知的 200，正文形如 seq=<累计条数>....，序号用来发现串号。\n"
      "单线程 + poll：--delay-ms 会同时拖住它的所有客户端，这正是\"上游比下游慢\"\n"
      "那个场景要的行为，不是缺陷。\n");
}

}  // namespace

int main(int argc, char** argv) {
  std::string listen_address = "127.0.0.1";
  std::uint16_t listen_port = 0;
  long body_bytes = 64;
  long delay_ms = 0;
  bool close_after = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view flag = argv[i];
    const char* value = (i + 1 < argc) ? argv[i + 1] : nullptr;
    if (flag == "--listen" && value != nullptr) {
      if (!parse_addr_port(value, &listen_address, &listen_port)) {
        std::fprintf(stderr, "参数错误: --listen 要的是 <ipv4:port>，收到 %s\n", value);
        return 2;
      }
      ++i;
    } else if (flag == "--body-bytes" && value != nullptr) {
      if (!parse_bounded(value, 0, 1 << 20, &body_bytes)) {
        std::fprintf(stderr, "参数错误: --body-bytes 要的是 0..1048576，收到 %s\n", value);
        return 2;
      }
      ++i;
    } else if (flag == "--delay-ms" && value != nullptr) {
      if (!parse_bounded(value, 0, 60000, &delay_ms)) {
        std::fprintf(stderr, "参数错误: --delay-ms 要的是 0..60000，收到 %s\n", value);
        return 2;
      }
      ++i;
    } else if (flag == "--close-after-first-response") {
      close_after = true;
    } else if (flag == "--help" || flag == "-h") {
      usage(stdout);
      return 0;
    } else {
      std::fprintf(stderr, "参数错误: 无法识别的选项 %s\n", std::string(flag).c_str());
      usage(stderr);
      return 2;
    }
  }

  // 信号只置一个原子标志，主循环看到它之后再去 join 上游线程。
  // 处理函数里不做任何带锁或带分配的事：async-signal-safety 不是风格，
  // 是"信号在 malloc 中途到达时会不会自锁"那一类死法的唯一防线。
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  l7gate::testing::TestUpstream upstream;
  upstream.set_response_body_bytes(static_cast<std::size_t>(body_bytes));
  upstream.set_delay_before_response_ms(static_cast<int>(delay_ms));
  upstream.set_close_after_first_response(close_after);
  std::string failure;
  if (!upstream.start(&failure, listen_address, listen_port)) {
    std::fprintf(stderr, "上游启动失败: %s\n", failure.c_str());
    return 1;
  }
  std::printf("[upstream] 监听 %s:%u body=%ld delay=%ldms close_after=%d\n", listen_address.c_str(),
              static_cast<unsigned int>(upstream.port()), body_bytes, delay_ms,
              close_after ? 1 : 0);
  std::fflush(stdout);

  while (!g_stop_requested.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  upstream.stop_and_join();
  std::printf("[upstream] 已服务请求数=%llu\n",
              static_cast<unsigned long long>(upstream.requests_served()));
  return 0;
}
