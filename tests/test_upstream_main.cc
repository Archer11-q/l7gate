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
#include <fstream>
#include <iterator>
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
      "               [--fixed-body TEXT] [--stall-reads-ms N] [--path-status PREFIX:CODE]\n"
      "               [--ws-echo] [--html PATH]\n"
      "行为: 凑齐一个完整请求（\\r\\n\\r\\n 再加 Content-Length 那么多 body）就回一段\n"
      "      Content-Length 已知的 200，正文形如 seq=<累计条数>....，序号用来发现串号。\n"
      "--fixed-body 把正文钉成同一个字符串：S1-06 要证\"直连与经网关逐字节一致\"，\n"
      "      带 seq 的两次取值本来就不同，钉死之后\"一致\"才是可比命题。\n"
      "路径前缀决定响应形态（/dies 不回话就关、/dies-mid-head 回半条响应头就关、\n"
      "      /dies-mid-body 回完整头 + 半条 body 就关、/chunked 回 chunked、\n"
      "      /bad-chunk 回坏框架、/endless-head 回永不齐的头），\n"
      "      其余走默认模板。上游只需要\"看路径回不同字节\"，聪明留给网关。\n"
      "--stall-reads-ms N：启动后前 N 毫秒**一个客户端 fd 都不读**（照旧 accept、照旧持有\n"
      "      连接）。这是\"上游突然不消费了\"的形状，S1-07 拿它把网关的入向缓冲顶到\n"
      "      停读线，再看水位回落后能不能自己恢复——注意 stall 期间不判存活，否则\n"
      "      \"慢\"会被这个夹具自己读成\"死\"，网关看到的是级联关闭而不是背压。\n"
      "--ws-echo（S5-04 演示页用）：101 里的 Sec-WebSocket-Accept 按 Sec-WebSocket-Key 现算，\n"
      "      进隧道之后解掩码取载荷、回一条未掩码帧。默认**不开**——不开时握手回原来那格固定\n"
      "      假 accept、隧道仍是原样吐回字节，S1-05/S2-05 那批\"对端越笨、透传越无处藏\"的\n"
      "      断言靠的就是笨档，两档不可互换。真浏览器要连上来才必须开（理由与实测在 ADR-26）。\n"
      "--html PATH：非升级的普通请求一律回这个文件。存在的理由只有一个——演示页必须\n"
      "      经网关同源提供，它自己的状态码探针才读得到 429；开了它会盖掉上面那些路径前缀档，\n"
      "      所以同一次启动里不要混用。\n"
      "单线程 + poll：--delay-ms 会同时拖住它的所有客户端，这正是\"上游比下游慢\"\n"
      "那个场景要的行为，不是缺陷。\n");
}

}  // namespace

int main(int argc, char** argv) {
  std::string listen_address = "127.0.0.1";
  std::uint16_t listen_port = 0;
  long body_bytes = 64;
  long delay_ms = 0;
  long stall_reads_ms = 0;
  bool close_after = false;
  std::string fixed_body;  // 空 = 不钉，继续用带 seq 的模板体
  std::string path_status_prefix;
  int path_status_code = 0;
  // S5-04 演示页的两档：默认全关，关的时候这台上游的行为与本任务之前逐字节相同。
  bool ws_echo = false;
  std::string html_body;

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
    } else if (flag == "--fixed-body" && value != nullptr) {
      fixed_body = value;
      ++i;
    } else if (flag == "--stall-reads-ms" && value != nullptr) {
      if (!parse_bounded(value, 0, 60000, &stall_reads_ms)) {
        std::fprintf(stderr, "参数错误: --stall-reads-ms 要的是 0..60000，收到 %s\n", value);
        return 2;
      }
      ++i;
    } else if (flag == "--close-after-first-response") {
      close_after = true;
    } else if (flag == "--path-status" && value != nullptr) {
      // 形如 /gone:404：命中该前缀的请求回固定状态码（S3-03 探活与被动 5xx 的可控上游形态）。
      const std::string_view spec = value;
      const std::size_t sep = spec.rfind(':');
      long code = 0;
      if (sep == std::string_view::npos || spec.empty() || spec[0] != '/' ||
          !parse_bounded(std::string(spec.substr(sep + 1)).c_str(), 100, 599, &code)) {
        std::fprintf(stderr, "参数错误: --path-status 要的是 <前缀>:<100..599>，收到 %s\n", value);
        return 2;
      }
      path_status_prefix = std::string(spec.substr(0, sep));
      path_status_code = static_cast<int>(code);
      ++i;
    } else if (flag == "--ws-echo") {
      // S5-04：握手 accept 现算 + 隧道里解掩码重组回显。默认不开（ADR-26 第二节第 1 条）。
      ws_echo = true;
    } else if (flag == "--html" && value != nullptr) {
      // S5-04：把演示页发出去，好让页面与它的状态码探针同源（否则 CORS 挡死，实测在 ADR-26）。
      std::ifstream in(value, std::ios::binary);
      if (!in) {
        std::fprintf(stderr, "参数错误: --html 读不到文件 %s\n", value);
        return 2;
      }
      html_body = std::string((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
      ++i;
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
  upstream.set_stall_reads_first_ms(static_cast<int>(stall_reads_ms));
  upstream.set_close_after_first_response(close_after);
  if (!fixed_body.empty()) upstream.set_fixed_response_body(fixed_body);
  if (!path_status_prefix.empty()) {
    upstream.set_path_status(path_status_prefix, path_status_code);
  }
  upstream.set_ws_echo(ws_echo);
  if (!html_body.empty()) upstream.set_html_body(html_body);
  std::string failure;
  if (!upstream.start(&failure, listen_address, listen_port)) {
    std::fprintf(stderr, "上游启动失败: %s\n", failure.c_str());
    return 1;
  }
  std::printf("[upstream] 监听 %s:%u body=%ld delay=%ldms close_after=%d fixed=%d "
              "stall_reads_ms=%ld ws_echo=%d html_bytes=%zu\n",
              listen_address.c_str(), static_cast<unsigned int>(upstream.port()), body_bytes,
              delay_ms, close_after ? 1 : 0, fixed_body.empty() ? 0 : 1, stall_reads_ms,
              ws_echo ? 1 : 0, html_body.size());
  std::fflush(stdout);

  while (!g_stop_requested.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  upstream.stop_and_join();
  std::printf("[upstream] 已服务请求数=%llu\n",
              static_cast<unsigned long long>(upstream.requests_served()));
  // S4-05：`--path-status` 那条分支不进上面那个数（老口径，S4_02 的用例注释按它写着）。
  // 冒烟要数"这台后端一共收到几条"就必须把两笔相加——只打第一行会让回 503 的那台
  // 读数恒为 0，而 0 在判定里长得像"它一条都没接到"（口径④）。
  std::printf("[upstream] 固定状态码响应数=%llu\n",
              static_cast<unsigned long long>(upstream.path_status_responses_served()));
  // S5-04：演示页那条路上，"上游自报"是分三方对账的一方（页面 DOM 的条数 / 网关的
  // upgrades_101 与隧道字节 / 这里回出去的帧数）。握手单独一笔账，因为它不是响应。
  std::printf("[upstream] 握手101数=%llu 回显帧数=%llu\n",
              static_cast<unsigned long long>(upstream.upgrades_101_served()),
              static_cast<unsigned long long>(upstream.ws_frames_echoed()));
  return 0;
}
