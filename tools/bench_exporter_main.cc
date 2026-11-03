// S5-03 / ADR-25：把已落盘的压测摘要发成 Prometheus 能抓的 gauge。
//
// 这个进程存在的唯一理由：面板六格里「p99/p999」那一格在网关侧没有来源（ADR-24 裁的是
// 「分位由客户端自己算」，网关那 101 个键全是整数计数）。所以那一格的数据由**旁路**供给：
// 只读 `scripts/bench/s1-p99.sh` / `s1-p999.sh` 已经写在盘上的摘要，把它们发成 time series。
//
// 三条结构性的规矩写在代码形状里，不写在注释里：
//   1. **不链 l7gate_core**。产品代码一行都不进这个进程，谈何进被测方的计数。
//   2. **缺字段就不发那一格，绝不补 0**（ADR-25 第三节第 1 条）：解析不出来的行整个不出现，
//      因为把「没读到」画成 0 的曲线比没有曲线更坏。
//   3. **每个数带着它的来路**：`batch` 是 `build/bench/` 下的相对目录名，`canonic`/`conns`/
//      `preset` 原样抄摘要自己写的那几个字，另发 `l7gate_bench_sample_time_seconds` 取摘要
//      首行的时间戳。任何人能在盘上 grep 到同一个数。
//
// 它不产生负载、不读被测方内存、不改任何测量条件（禁令二）。
//
// 用法：
//   l7gate_bench_exporter [--root build/bench] [--listen 127.0.0.1:18090] [--once]

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <string>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
#include <vector>

namespace {

std::atomic<int> g_stop{0};
void on_signal(int) { g_stop.store(1); }

struct Reading {
  std::string tool;      // wrk / hey / hdr
  std::string arm;       // direct / proxy；空 = 增量那一格（它跨两臂算出来，没有 arm）
  std::string kind;      // p50/p75/p90/p99/p999/p9999 | qps | window_ms | observer_cores
  double value = 0.0;    // 已经统一到这一族自己的单位（延迟是 µs）
  bool increment = false;
};

struct Summary {
  std::string batch;          // build/bench 下的相对目录
  std::string canonic;        // "0" / "1"，原样抄摘要
  std::string conns;          // "1000" / "100"
  std::string preset;         // "release"
  std::string sample_time;    // 摘要首行那个 ISO 时间戳换成的 epoch 秒
  std::vector<Reading> readings;
};

std::string fmt_num(double v) {
  char buf[40];
  if (v == static_cast<double>(static_cast<long long>(v))) {
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
    return std::string(buf);
  }
  std::snprintf(buf, sizeof(buf), "%.3f", v);
  std::string s(buf);
  while (!s.empty() && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  return s;
}

std::optional<double> to_d(const std::string& s) {
  try {
    std::size_t used = 0;
    const double v = std::stod(s, &used);
    if (used == 0) return std::nullopt;
    return v;
  } catch (...) {
    return std::nullopt;
  }
}

// ISO `2026-10-26T14:48:35+08:00` → epoch 秒。摘要有两套时间：UTC 与本地带时区，
// 这里一律按时区偏移折回 UTC，面板上两族的 x 轴才是同一根轴。
std::optional<std::string> iso_to_epoch_s(const std::string& iso) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, tz_h = 0, tz_m = 0;
  char sign = '+';
  const int n = std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d%c%d:%d",
                            &y, &mo, &d, &h, &mi, &s, &sign, &tz_h, &tz_m);
  if (n < 6) return std::nullopt;
  std::tm tm{};
  tm.tm_year = y - 1900;
  tm.tm_mon = mo - 1;
  tm.tm_mday = d;
  tm.tm_hour = h;
  tm.tm_min = mi;
  tm.tm_sec = s;
  const long long epoch = static_cast<long long>(timegm(&tm));
  if (epoch < 0) return std::nullopt;
  long long value = epoch;
  if (n >= 9) {
    const long long offset = static_cast<long long>(tz_h) * 3600 + static_cast<long long>(tz_m) * 60;
    value -= (sign == '-') ? -offset : offset;
  }
  return fmt_num(static_cast<double>(value));
}

const std::regex kIsoRe(R"(([0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(\+[0-9]{2}:[0-9]{2})?))");
const std::regex kCanonicRe(R"(canonic=([01]))");
const std::regex kConnsRe(R"(conns=([0-9]+))");
const std::regex kPresetRe(R"(preset=([a-z_]+))");

// ---- s1-p999.summary.txt：自研 HDR 客户端，单位微秒 ----
//   # SLO #1 · p999 增量（s1-p999.sh）canonic=0 —— 2026-11-03T16:41:42+08:00
//   参数: body=1024B conns=100 ... workers=1(自报 1) steady_min_ms=50000 ...
//   直连          548         1056         1440         2240        66041       181705.3       0.98
//   增量 p999 = 4.832 ms（经网关 − 直连，SLO 判的就是这个差值；目标 ≤ 5.0 ms）
const std::regex kHdrRow(R"(^(直连|经网关)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+([0-9.]+)\s+([0-9.]+)$)");
const std::regex kHdrInc(R"(^增量 p(999|99)\s*=\s*([0-9.]+)\s*ms)");

bool parse_hdr(const std::string& path, Summary& out) {
  std::ifstream in(path);
  if (!in) return false;
  std::string line;
  while (std::getline(in, line)) {
    std::smatch m;
    if (line.rfind("# SLO", 0) == 0) {
      if (std::regex_search(line, m, kCanonicRe) && m.size() >= 2) out.canonic = m[1].str();
      if (std::regex_search(line, m, kIsoRe) && m.size() >= 2) {
        if (auto t = iso_to_epoch_s(m[1].str())) out.sample_time = *t;
      }
      continue;
    }
    if (line.rfind("参数:", 0) == 0) {
      if (std::regex_search(line, m, kConnsRe) && m.size() >= 2) out.conns = m[1].str();
      continue;
    }
    if (std::regex_match(line, m, kHdrRow)) {
      const std::string arm = m[1].str() == "直连" ? "direct" : "proxy";
      const char* names[4] = {"p50", "p99", "p999", "p9999"};
      for (int i = 0; i < 4; ++i) {
        if (auto v = to_d(m[static_cast<std::size_t>(i) + 2].str())) {
          out.readings.push_back({"hdr", arm, names[i], *v, false});
        }
      }
      if (auto w = to_d(m[6].str())) out.readings.push_back({"hdr", arm, "window_ms", *w, false});
      if (auto q = to_d(m[7].str())) out.readings.push_back({"hdr", arm, "qps", *q, false});
      // 观察者占单核那一列必须一起发（B-009：卡点可能是观察者而不是被测方）。
      if (auto c = to_d(m[8].str())) out.readings.push_back({"hdr", arm, "observer_cores", *c, false});
      continue;
    }
    if (std::regex_search(line, m, kHdrInc)) {
      if (auto v = to_d(m[2].str())) {
        out.readings.push_back({"hdr", "", "p" + m[1].str(), *v * 1000.0, true});
      }
    }
  }
  return !out.readings.empty();
}

// ---- s1-p99.summary.txt：wrk 主口径 + hey 交叉，单位毫秒 ----
//   口径 canonic=1  preset=release  body=1024B  conns=1000  duration=60s ...
//   ## wrk（主）
//   直连       16.350     17.330     18.430     24.850     59860.19
//   增量 p99=57.540ms（...）
//   ## hey（交叉验证，连接模型不同，只看量级）
//   直连       12.300     14.800     22.100   77557.6601
const std::regex kWrkRow(R"(^(直连|经网关)\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)$)");
const std::regex kHeyRow(R"(^(直连|经网关)\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)$)");
const std::regex kWrkInc(R"(^增量 p(999|99)\s*=\s*([0-9.]+)ms)");

bool parse_wrk(const std::string& path, Summary& out) {
  std::ifstream in(path);
  if (!in) return false;
  std::string line;
  std::string section = "pre";
  bool wrk_seen = false;
  while (std::getline(in, line)) {
    std::smatch m;
    if (line.rfind("# SLO", 0) == 0) {
      if (std::regex_search(line, m, kIsoRe) && m.size() >= 2) {
        if (auto t = iso_to_epoch_s(m[1].str())) out.sample_time = *t;
      }
      continue;
    }
    if (line.rfind("口径 canonic=", 0) == 0) {
      if (std::regex_search(line, m, kCanonicRe) && m.size() >= 2) out.canonic = m[1].str();
      if (std::regex_search(line, m, kPresetRe) && m.size() >= 2) out.preset = m[1].str();
      if (std::regex_search(line, m, kConnsRe) && m.size() >= 2) out.conns = m[1].str();
      continue;
    }
    if (line.rfind("## ", 0) == 0) {
      section = (line.find("wrk") != std::string::npos)   ? "wrk"
               : (line.find("hey") != std::string::npos) ? "hey"
                                                          : "other";
      continue;
    }
    const bool is_wrk = section == "wrk";
    if (!is_wrk && section != "hey") continue;
    if (std::regex_match(line, m, is_wrk ? kWrkRow : kHeyRow)) {
      wrk_seen = wrk_seen || is_wrk;
      const std::string tool = is_wrk ? "wrk" : "hey";
      const std::string arm = m[1].str() == "直连" ? "direct" : "proxy";
      const char* names_wrk[4] = {"p50", "p75", "p90", "p99"};
      const char* names_hey[3] = {"p50", "p90", "p99"};
      const char** names = is_wrk ? names_wrk : names_hey;
      const int nq = is_wrk ? 4 : 3;
      for (int i = 0; i < nq; ++i) {
        if (auto v = to_d(m[static_cast<std::size_t>(i) + 2].str())) {
          out.readings.push_back({tool, arm, names[i], *v * 1000.0, false});  // ms → µs
        }
      }
      if (auto q = to_d(m[static_cast<std::size_t>(nq) + 2].str())) {
        out.readings.push_back({tool, arm, "qps", *q, false});
      }
      continue;
    }
    if (std::regex_search(line, m, kWrkInc)) {
      if (auto v = to_d(m[2].str())) {
        out.readings.push_back({is_wrk ? "wrk" : "hey", "", "p" + m[1].str(), *v * 1000.0, true});
      }
    }
  }
  // 只有 wrk 那一节算数：hey 的分位表同样只到 99%，且它是交叉验证不是主口径。
  return wrk_seen;
}

std::vector<Summary> collect(const std::string& root) {
  std::vector<Summary> all;
  std::error_code ec;
  if (!std::filesystem::is_directory(root, ec)) return all;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    const std::string name = entry.path().filename().string();
    if (name != "s1-p99.summary.txt" && name != "s1-p999.summary.txt") continue;
    Summary s;
    s.batch = std::filesystem::relative(entry.path().parent_path(), root, ec).string();
    if (s.batch.empty()) s.batch = ".";
    s.preset = "release";
    const std::string file = entry.path().string();
    const bool ok = (name == "s1-p999.summary.txt") ? parse_hdr(file, s) : parse_wrk(file, s);
    if (ok && !s.batch.empty()) all.push_back(std::move(s));
  }
  std::sort(all.begin(), all.end(), [](const Summary& a, const Summary& b) {
    return a.batch < b.batch;
  });
  return all;
}

// 一族的标签。`canonic`/`conns` 缺字段时整条标签不出现——发一个空值会让面板上
// `canonic=""` 与 `canonic="0"` 读成两个东西。
std::string labels(const Summary& s, const Reading& r) {
  std::string l = "tool=\"" + r.tool + "\",batch=\"" + s.batch + "\"";
  if (!r.arm.empty()) l += ",arm=\"" + r.arm + "\"";
  if (!r.kind.empty()) l += ",quantile=\"" + r.kind + "\"";
  if (!s.canonic.empty()) l += ",canonic=\"" + s.canonic + "\"";
  if (!s.conns.empty()) l += ",conns=\"" + s.conns + "\"";
  if (!s.preset.empty()) l += ",preset=\"" + s.preset + "\"";
  return "{" + l + "}";
}

struct Family {
  const char* name;
  const char* help;
  const char* type;
};

std::string render(const std::vector<Summary>& all) {
  static const Family kFams[] = {
      {"l7gate_bench_exporter_up",
       "1 = exporter 在位。它只读 build/bench 下的已落盘摘要，不产生负载、不读被测方。", "gauge"},
      {"l7gate_bench_latency_microseconds",
       "每请求分位延迟，来自 build/bench/<batch> 的落盘摘要，不是网关自报：网关的 /metrics 里没有时延键（ADR-24）。wrk/hey 的分位表只到 p99；p999 与 p9999 只存在于 tool=\"hdr\" 那一族（l7gate_c100k 的 HDR 桶，本档分辨率 ≈128 µs/请求）。canonic=0 的批次不得当 SLO 判定读。", "gauge"},
      {"l7gate_bench_increment_microseconds",
       "同一负载下「经网关 − 直连」的分位增量，SLO #1 判的就是这个差值（目标 p99 ≤ 1.0 ms、p999 ≤ 5.0 ms）。HDR 那一族的 p99 增量是同一条分布的副产物、只作参照，主口径看 tool=\"wrk\"。", "gauge"},
      {"l7gate_bench_qps",
       "摘要里那一臂自己的 QPS（工具侧读数，不是网关计数）。", "gauge"},
      {"l7gate_bench_window_ms",
       "稳态窗口毫秒数，只有 HDR 那一族落盘里有这一列。", "gauge"},
      {"l7gate_bench_observer_cores",
       "观察者（压测客户端那一侧）自己占掉的单核比例。B-009 的教训：这一格贴到 1.0 时那一轮的 p999 只能当下界，甚至判「取不到」。", "gauge"},
      {"l7gate_bench_canonic",
       "摘要首行自己写的 canonic（0/1）：这一批的参数是不是 SLO 写死的那五条。非 1 的批次不进 SLO 判定。", "gauge"},
      {"l7gate_bench_sample_time_seconds",
       "摘要首行那个 ISO 时间戳折成 UTC 秒。面板上每一条 series 的来路时刻，配合 batch 标签可以在盘上 grep 到同一个数。", "gauge"},
  };
  // 一个族要么整族带着 HELP/TYPE 出现、要么根本不出现：空族发出去会让 Prometheus
  // 那边读到「有这个名字、没有数」，正是我在 ADR-25 第三节要挡的那个形状。
  std::string out;
  out += std::string("# HELP ") + kFams[0].name + " " + kFams[0].help + "\n";
  out += std::string("# TYPE ") + kFams[0].name + " gauge\nl7gate_bench_exporter_up 1\n";
  for (const auto& f : kFams) {
    const std::string fam = f.name;
    if (fam == "l7gate_bench_exporter_up") continue;
    std::string body;
    for (const auto& s : all) {
      if (fam == "l7gate_bench_canonic") {
        if (!s.canonic.empty()) {
          body += fam + "{batch=\"" + s.batch + "\"} " + s.canonic + "\n";
        }
        continue;
      }
      if (fam == "l7gate_bench_sample_time_seconds") {
        if (!s.sample_time.empty()) {
          body += fam + "{batch=\"" + s.batch + "\"} " + s.sample_time + "\n";
        }
        continue;
      }
      for (const auto& r : s.readings) {
        const bool is_qps = r.kind == "qps";
        const bool is_window = r.kind == "window_ms";
        const bool is_cores = r.kind == "observer_cores";
        if (fam == "l7gate_bench_latency_microseconds" &&
            (r.increment || is_qps || is_window || is_cores)) continue;
        if (fam == "l7gate_bench_increment_microseconds" && !r.increment) continue;
        if (fam == "l7gate_bench_qps" && !is_qps) continue;
        if (fam == "l7gate_bench_window_ms" && !is_window) continue;
        if (fam == "l7gate_bench_observer_cores" && !is_cores) continue;
        Reading key_only = r;
        key_only.kind = is_qps || is_window || is_cores ? "" : r.kind;
        body += fam + labels(s, key_only) + " " + fmt_num(r.value) + "\n";
      }
    }
    if (body.empty()) continue;
    out += std::string("# HELP ") + f.name + " " + f.help + "\n";
    out += std::string("# TYPE ") + f.name + " " + f.type + "\n";
    out += body;
  }
  return out;
}

bool send_all(int fd, const std::string& data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
    if (n <= 0) {
      if (n < 0 && (errno == EINTR)) continue;
      return false;
    }
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

void serve(int listen_fd, const std::string& root) {
  for (;;) {
    if (g_stop.load()) break;
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(listen_fd, &rfds);
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 200000;
    const int ready = ::select(listen_fd + 1, &rfds, nullptr, nullptr, &tv);
    if (ready < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (ready == 0) continue;
    const int client = ::accept(listen_fd, nullptr, nullptr);
    if (client < 0) {
      if (errno == EINTR || errno == EAGAIN) continue;
      break;
    }
    char buf[2048];
    std::string req;
    for (;;) {
      const ssize_t n = ::recv(client, buf, sizeof(buf), 0);
      if (n <= 0) break;
      req.append(buf, static_cast<std::size_t>(n));
      if (req.find("\r\n\r\n") != std::string::npos) break;
      if (req.size() > 8192) break;
    }
    const bool wants_metrics = req.rfind("GET ", 0) == 0 && req.find("/metrics") != std::string::npos;
    if (!wants_metrics) {
      const std::string body = "not found\n";
      std::string resp = "HTTP/1.1 404 Not Found\r\nContent-Length: " + std::to_string(body.size()) +
                         "\r\nConnection: close\r\n\r\n" + body;
      send_all(client, resp);
    } else {
      const std::string body = render(collect(root));
      std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: text/plain; version=0.0.4; charset=utf-8\r\n";
      resp += "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
      resp += body;
      send_all(client, resp);
    }
    ::close(client);
  }
}

void usage(std::FILE* out) {
  std::fprintf(out,
      "用法: l7gate_bench_exporter [--root DIR] [--listen IPV4:PORT] [--once]\n"
      "  --root DIR          要扫的压测落盘根目录（缺省 build/bench）。只读 s1-p99.summary.txt\n"
      "                     与 s1-p999.summary.txt 两种摘要；读不到就那一族整个不发。\n"
      "  --listen A:P       监听地址（缺省 127.0.0.1:18090）。容器要抓它就得绑 0.0.0.0。\n"
      "  --once             把一次 exposition 打到 stdout 后退出（冒烟用它逐数对落盘，不开 socket）。\n");
}

}  // namespace

int main(int argc, char** argv) {
  std::string root = "build/bench";
  std::string listen = "127.0.0.1:18090";
  bool once = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--once") {
      once = true;
    } else if (arg == "--help" || arg == "-h") {
      usage(stdout);
      return 0;
    } else if ((arg == "--root" || arg == "--listen") && i + 1 < argc) {
      if (arg == "--root") root = argv[++i];
      else listen = argv[++i];
    } else {
      std::fprintf(stderr, "参数错误: %s\n", arg.c_str());
      usage(stderr);
      return 2;
    }
  }

  if (once) {
    std::fputs(render(collect(root)).c_str(), stdout);
    return 0;
  }

  const auto colon = listen.rfind(':');
  if (colon == std::string::npos) {
    std::fprintf(stderr, "参数错误: --listen 要的是 <ipv4:port>，收到 %s\n", listen.c_str());
    return 2;
  }
  const std::string addr = listen.substr(0, colon);
  const long port = std::strtol(listen.substr(colon + 1).c_str(), nullptr, 10);
  if (port <= 0 || port > 65535) {
    std::fprintf(stderr, "参数错误: --listen 的端口要落在 1..65535，收到 %s\n", listen.c_str());
    return 2;
  }

  std::signal(SIGTERM, on_signal);
  std::signal(SIGINT, on_signal);
  std::signal(SIGPIPE, SIG_IGN);

  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    std::perror("socket");
    return 1;
  }
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(static_cast<uint16_t>(port));
  if (::inet_pton(AF_INET, addr.c_str(), &sa.sin_addr) != 1) {
    std::fprintf(stderr, "参数错误: --listen 的 IPv4 解析失败：%s\n", addr.c_str());
    ::close(fd);
    return 2;
  }
  if (::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
    std::perror("bind");
    ::close(fd);
    return 1;
  }
  if (::listen(fd, 16) != 0) {
    std::perror("listen");
    ::close(fd);
    return 1;
  }
  std::fprintf(stderr, "[exporter] listen=%s root=%s 只读落盘摘要，不产生负载\n",
               listen.c_str(), root.c_str());
  serve(fd, root);
  ::close(fd);
  return 0;
}
