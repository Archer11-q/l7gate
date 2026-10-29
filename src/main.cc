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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <string_view>
#include <vector>

#include "core/build_info.h"
#include "filter/route_filter.h"
#include "net/reactor.h"
#include "net/reactor_group.h"
#include "route/router.h"

namespace {

void usage(std::FILE* out) {
  std::fprintf(out,
      "l7gate —— L7 流量治理网关（开发中）\n"
      "用法: l7gate --self-check\n"
      "      l7gate --serve --listen <ipv4:port> --upstream <ipv4:port>[,<ipv4:port>...]\n"
      "               [--serve-requests N] [--max-connections N]\n"
      "               [--high-water-mark BYTES] [--workers N] [--drain-timeout-ms MS]\n"
      "               [--route <前缀>=<端点下标>]... [--route-regex <锚定正则>=<端点下标>]...\n"
      "\n"
      "--serve 的透传路径（S1-04 + S1-05）：epoll 边缘触发单线程循环，下游请求原样\n"
      "搬到上游，上游响应原样搬回来，keep-alive 复用同一条回源连接；带 Connection:\n"
      "upgrade + Upgrade 的请求在拿到完整 101 响应头后切纯透传（ADR-09，不解析帧），\n"
      "Connection: close 的请求按语义收尾配对连接。它不做状态码映射、不改写响应\n"
      "（S1-06），畸形请求只关连接不回 400。\n"
      "--workers N（S2-01）：N=1（缺省）走单 Reactor；N≥2 起 N 条 worker 循环线程，\n"
      "主线程 accept 后 round-robin 派发，连接对象单线程拥有（ADR-03/ADR-14）。\n"
      "N 为正整数。--serve-requests 是「解析满 N 条就停循环」，它可能在最后一条响应\n"
      "送达之前就 break —— 要「把 N 条全部送完再停」就用信号，冒烟脚本就是这么做的。\n"
      "--high-water-mark 把**两个方向**的停读线钉到 BYTES（4096..1048576，默认 256 KiB）。\n"
      "它是 S1-07 的取证旋钮，不是调优入口：转发的最小单位是「整条请求 / 整个响应头」，\n"
      "线压得比一条请求还小，那条请求就永远转不出去——它会停在停读线上等人来处理，\n"
      "不是被丢弃，也不是被报错。S1 的「整块才转」边界见 ARCHITECTURE 1.15。\n"
      "--drain-timeout-ms 是优雅停止的**兜底**窗口（0..600000，默认 5000）：到点就把还没\n"
      "收尾的连接强关，代价是那条连接的出向字节被丢弃、客户端看到截断。它是兜底不是承诺\n"
      "（一条卡死的客户端不该能无限拖住一次下线），且按 ARCHITECTURE 1.20 必须 ≥ S4 的请求超时。\n"
      "--route / --route-regex（S3-01，ADR-18）把请求按前缀或锚定正则分到 --upstream 列表里的\n"
      "第 N 个端点：前缀取**最长命中**优先，正则取**配置顺序第一条全串命中**且优先于前缀，\n"
      "匹配的是原始请求行里的 origin-form（含 query、不解码、不做路径规范化）。没有任何规则\n"
      "命中回 404 并关连接。一条规则都不给 = 不装路由 filter = 每个 worker 只用它的默认目标\n"
      "（第 i mod E 个端点），那一条路径与 S2-06a 逐字节同形。\n"
      "**这一对开关是临时的**：路由与节点表的配置文件格式归 S3-02，那时它被文件取代、不留兼容。\n"
      "SIGINT/SIGTERM 走同一条优雅停止：摘掉监听（新连接从此拿 RST）→ 不再接受新请求 →\n"
      "在飞请求逐条响应完 + 刷完出向 → 空闲连接立即关 → 到点兜底强关 → 退出循环、join worker。\n"
      "带 upgrade 的隧道**只能硬关**：ADR-09 不解析帧就发不出 WS close 握手，刷完已缓冲的\n"
      "隧道字节即关 socket，客户端断线重连到下一个实例。\n"
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
// S2-05 起这里走的是**优雅停止**（排水）而不是硬停：handler 调 request_drain()，
// 循环不立刻退出，而是摘掉监听、把手上在飞的请求逐条答完 + 刷完出向再关，空闲连接
// 立即关，`--drain-timeout-ms` 到点后兜底强关（1.20）。
//
// 处理函数里只做两件跨信号边界的事：读两个全局原子、调 request_drain()。后者内部是
// 一次 relaxed/release 原子 store 加一个 ::write(eventfd)——write(2) 在 POSIX 的
// async-signal-safe 名单上，而 lock-free 原子的 store 在 x86-64 上就是一条 mov。
// 没有锁、没有分配、没有 stdio。连续两个 SIGTERM 时第二个可能打断第一个的 write，
// 最坏结果是 eventfd 计数变 2、循环多醒一次，不是坏状态；而排水窗口**只从第一次信号
// 算起**（request_drain 的 exchange 守卫），所以"多敲几次铃"拖不长一次下线。
std::atomic<l7gate::net::Reactor*> g_reactor_for_signal{nullptr};
// S2-01：多 Reactor 路径用 ReactorGroup。两个全局二选一非空——单 Reactor 模式设
// g_reactor_for_signal，多 Reactor 模式设 g_group_for_signal。handler 各判一次空。
std::atomic<l7gate::net::ReactorGroup*> g_group_for_signal{nullptr};
// 兜底窗口由命令行决定，但 handler 不能读普通变量（那是跨信号边界的非原子共享）。
// 原子 relaxed load 是信号安全的，且这个值在装 handler 之前就已经写定。
std::atomic<std::int64_t> g_drain_timeout_ms{5000};

void on_stop_signal(int) {
  const std::int64_t timeout_ms = g_drain_timeout_ms.load(std::memory_order_relaxed);
  l7gate::net::Reactor* reactor = g_reactor_for_signal.load(std::memory_order_relaxed);
  if (reactor != nullptr) {
    reactor->request_drain(timeout_ms);
  }
  l7gate::net::ReactorGroup* group = g_group_for_signal.load(std::memory_order_relaxed);
  if (group != nullptr) {
    group->request_drain(timeout_ms);
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

using UpstreamEndpoint = l7gate::net::ReactorGroup::Config::UpstreamEndpoint;

// S2-06a（ADR-17 候选 A）：`--upstream a:1,b:1,c:1` → 端点列表。任何一段非法就整体失败：
// 配置写错不该退化成"少连一个上游"那种安静形态，而它在观测上和"回源元组耗尽"长得太像了
// （两者最后都表现为 established < planned）。
bool parse_upstream_list(std::string_view spec, std::vector<UpstreamEndpoint>* out) {
  for (;;) {
    const std::size_t comma = spec.find(',');
    const std::string_view item = comma == std::string_view::npos ? spec : spec.substr(0, comma);
    std::string address;
    std::uint16_t port = 0;
    if (!parse_addr_port(item, &address, &port)) return false;
    out->push_back(UpstreamEndpoint{std::move(address), port});
    if (comma == std::string_view::npos) return true;
    spec = spec.substr(comma + 1);
  }
}

std::string join_upstream_list(const std::vector<UpstreamEndpoint>& endpoints) {
  std::string text;
  for (const UpstreamEndpoint& endpoint : endpoints) {
    if (!text.empty()) text += ',';
    text += endpoint.address + ':' + std::to_string(endpoint.port);
  }
  return text;
}

bool parse_number(const char* text, long long* out) {
  errno = 0;
  char* end = nullptr;
  const long long value = std::strtoll(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0') return false;
  *out = value;
  return true;
}

// S3-01（ADR-18）：`--route /api=0` / `--route-regex ^/health$=1` 的单条解析。
// 分隔符取**最后一个** '='：前缀本身可以含 '='（`/search?q=` 是真的），正则更需要它；
// 从右边切一次是这两种形态都成立的唯一切法。
// 这里**不**校验下标是否落在端点表里——端点表要到收完参数才知道长度，而命令行是边收边校。
// 收完之后那一次 `validate_route_targets` 才是那条检查，也是唯一一份（两处各判一遍
// 就是"同一个限制有两个抄本"，原型见 `max_header_count` 那次）。
bool parse_route_spec(std::string_view spec, bool regex_kind,
                      l7gate::route::RouteTable* table) {
  const std::size_t sep = spec.rfind('=');
  if (sep == std::string_view::npos) return false;
  const std::string index_text(spec.substr(sep + 1));
  long long index = -1;
  if (!parse_number(index_text.c_str(), &index)) return false;
  if (index < 0 || index > static_cast<long long>(l7gate::route::kMaxUpstreamTarget)) {
    return false;
  }
  const auto target = static_cast<std::uint16_t>(index);
  // 正则那一支还会因**模式语法非法**返回 false（RouteTable::add_regex 在编译期挡）。
  // 两种失败在命令行上都必须整体退出：一条没生效的规则和一条编译不过的规则，
  // 从外面看都是"网关按一张我以为不是这样的表在跑"。
  return regex_kind ? table->add_regex(spec.substr(0, sep), target)
                    : table->add_prefix(spec.substr(0, sep), target);
}

// 规则指向一个不存在的端点 = 配置写错，必须**整体失败退出**。理由和 --upstream 那条
// 一字不差：配置写错不该退化成一种安静形态，而它退化之后的观测形态恰好和"网关坏了"
// 长得一样（请求被压住或得到 502，从外面看不出是我自己把一张坏表启动了）。
bool validate_route_targets(const l7gate::route::RouteTable& table, std::size_t endpoint_count,
                            std::uint16_t* bad_target) {
  for (std::size_t i = 0; i < table.size(); ++i) {
    const l7gate::route::RouteTable::KindView rule = table.rule_at(i);
    if (rule.upstream_target >= endpoint_count) {
      *bad_target = rule.upstream_target;
      return false;
    }
  }
  return true;
}

// 只在循环退出之后调用：ReactorStats 全原子，但这份快照要的是"这一次运行的最终账"，
// 在循环还在跑的时候打印会得到一个中间态，那种数字最容易被当成结论抄进文档。
// S2-01 起它接收**显式的聚合量**而不是 Reactor&：单 Reactor 传自己的、ReactorGroup
// 传跨 worker 求和的，两条路复用同一份 [serve] key=value 输出（冒烟脚本 grep 的键
// 因此不必区分单/多 Reactor）。epoll 三计数与 read_disarmed 都是非原子观测，
// 调用方保证只在循环停下后取。
void report_stats(l7gate::net::ReactorStats& stats, std::uint64_t epoll_registrations,
                  std::uint64_t epoll_unregistrations, std::uint64_t epoll_modifications,
                  std::size_t read_disarmed_connections, std::int64_t fd_start,
                  std::int64_t rss_start, long long elapsed_ms) {
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
  // S1-07 的背压动作与"平台期"证据链：pause/resume 是滞回跳变，两个字节数分别对应
  // 两个方向的入向读取。验收句里「内存不继续增长」在进程侧只能靠 RSS 粗看，
  // 真正可断言的是这两个累计数在停读期间**不再增长**。
  put("backpressure_pause_count",
      static_cast<long long>(stats.load(stats.backpressure_pause_count)));
  put("backpressure_resume_count",
      static_cast<long long>(stats.load(stats.backpressure_resume_count)));
  put("bytes_read_downstream", static_cast<long long>(stats.load(stats.bytes_read_downstream)));
  put("bytes_read_upstream", static_cast<long long>(stats.load(stats.bytes_read_upstream)));
  // S1-05 的三条观测线：握手次数（应恰好等于隧道数）、两个方向的透传字节、
  // close 意图被兑现的次数。
  put("upgrades_101", static_cast<long long>(stats.load(stats.upgrades_101)));
  put("bytes_tunnel_down", static_cast<long long>(stats.load(stats.bytes_tunnel_down)));
  put("bytes_tunnel_up", static_cast<long long>(stats.load(stats.bytes_tunnel_up)));
  put("close_headers_honored", static_cast<long long>(stats.load(stats.close_headers_honored)));
  // S1-06 的四条观测线：自产错误响应、截断的两个互斥类别、框架判定的完结数。
  put("gateway_replies_sent", static_cast<long long>(stats.load(stats.gateway_replies_sent)));
  put("upstream_truncated_before_head",
      static_cast<long long>(stats.load(stats.upstream_truncated_before_head)));
  put("upstream_truncated_in_body",
      static_cast<long long>(stats.load(stats.upstream_truncated_in_body)));
  put("responses_completed", static_cast<long long>(stats.load(stats.responses_completed)));
  // S2-02（ADR-15）上游连接池：借出/归还/两类淘汰。多 Reactor 下这是共享 stats 的
  // 自动聚合（全原子）。"复用了没有"看 borrows>0；"上游活得比下游久"看 returns>0。
  put("pool_borrows", static_cast<long long>(stats.load(stats.pool_borrows)));
  put("pool_returns", static_cast<long long>(stats.load(stats.pool_returns)));
  put("pool_evictions_eof", static_cast<long long>(stats.load(stats.pool_evictions_eof)));
  put("pool_evictions_full", static_cast<long long>(stats.load(stats.pool_evictions_full)));
  // S2-05（1.20）优雅停止的观测面。at_entry 那两条是**排水第一次扫描那一刻**的快照，
  // 口径写死在 stats.h：它们是"时刻"读数不是累加。drain_forced_timeout 不为 0 就
  // 意味着有客户端卡死到兜底才走，那是兜底生效的证据，不是正常路径。
  put("drain_requested", static_cast<long long>(stats.load(stats.drain_requested)));
  put("drain_in_flight_at_entry",
      static_cast<long long>(stats.load(stats.drain_in_flight_at_entry)));
  put("drain_closed_idle", static_cast<long long>(stats.load(stats.drain_closed_idle)));
  put("drain_closed_buffered", static_cast<long long>(stats.load(stats.drain_closed_buffered)));
  put("drain_tunnels_hard_closed",
      static_cast<long long>(stats.load(stats.drain_tunnels_hard_closed)));
  put("drain_forced_timeout", static_cast<long long>(stats.load(stats.drain_forced_timeout)));
  put("drain_read_disarmed_at_entry",
      static_cast<long long>(stats.load(stats.drain_read_disarmed_at_entry)));
  put("drain_rejected_new_connections",
      static_cast<long long>(stats.load(stats.drain_rejected_new_connections)));
  put("epoll_registrations", static_cast<long long>(epoll_registrations));
  put("epoll_unregistrations", static_cast<long long>(epoll_unregistrations));
  put("epoll_modifications", static_cast<long long>(epoll_modifications));
  // 「网关不再消费下游可读事件」这条验收在进程侧的直读证据：循环已经停下，
  // 剩下的每一个存活连接如果还带着没挂 EPOLLIN 的掩码，就是被背压按住的那一个。
  // 非原子观测，所以只在这个时刻读。
  put("read_disarmed_connections", static_cast<long long>(read_disarmed_connections));
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
  // S2-01：worker 线程数。缺省 1 = 单 Reactor（S1 路径逐字节不变，冒烟脚本断言的
  // epoll_registrations=4 等精确计数因此不作废）；≥2 走 ReactorGroup（ADR-14 后果段）。
  int workers = 1;
  // S2-05：排水兜底窗口（毫秒）。默认 5000，与 ARCHITECTURE 1.20 里那条"兜底不是承诺"
  // 一起看；它必须 ≥ S4 的请求超时，S4 没定义之前这是占位中值。
  long long drain_timeout_ms = 5000;
  bool have_listen = false;
  bool have_upstream = false;
  std::vector<UpstreamEndpoint> upstream_endpoints;
  // S3-01（ADR-18）：路由表。空表 = 不装路由 filter = 每一个 worker 只用它的默认目标，
  // 那一条路径与 S2-06a 逐字节同形（八份 canonic 档不作废的凭据就在这一句上）。
  l7gate::route::RouteTable route_table;

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
      if (!parse_upstream_list(value, &upstream_endpoints)) {
        std::fprintf(stderr, "参数错误: --upstream 要的是 <ipv4:port>[,<ipv4:port>...]，收到 %s\n", value);
        return 2;
      }
      have_upstream = true;
      ++i;
    } else if ((flag == "--route" || flag == "--route-regex") && value != nullptr) {
      // S3-01 的最小装配面。它是**临时**的：路由与节点表的配置文件格式归 S3-02（ADR-19
      // 之前先定格式），那时这一对开关会被文件里的 routes 段取代、不留兼容层。
      // 它必须存在的最小理由很硬：S3-01 验收第四条要在**真实进程**上跑"空链 vs 路由链"的
      // 每请求成本对照，而一个无法打开路由的 binary 只能测出空链那一臂。
      const bool regex_kind = flag == "--route-regex";
      if (!parse_route_spec(value, regex_kind, &route_table)) {
        std::fprintf(stderr, "参数错误: %s 要的是 <%s>=<端点下标>，收到 %s\n",
                     std::string(flag).c_str(), regex_kind ? "锚定正则" : "前缀", value);
        return 2;
      }
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
    } else if (flag == "--high-water-mark" && value != nullptr) {
      // S1-07 的停读线，两个方向一起设。它是**验收与冒烟的取证旋钮**，不是调优入口：
      // 默认的 256 KiB 在脚本里要靠几百 KB 的真实流量才顶得动，取证时不稳定；
      // 把它调小，"撞线 → 摘读 → 平台期 → 回落 → 探测恢复"这条链才能被确定性地打出。
      // 下限 4 KiB 不是随手取的：转发的单位是"整条请求 / 整个响应头"，
      // 低于这个尺寸的大头部请求会永远装不进出向缓冲，那是配置错误不是背压。
      long long n = 0;
      if (!parse_number(value, &n) || n < 4096 || n > (1 << 20)) {
        std::fprintf(stderr, "参数错误: --high-water-mark 要的是 4096..1048576，收到 %s\n", value);
        return 2;
      }
      config.connection.inbound.high_water_mark = static_cast<std::size_t>(n);
      config.connection.outbound.high_water_mark = static_cast<std::size_t>(n);
      ++i;
    } else if (flag == "--workers" && value != nullptr) {
      // S2-01 的多 Reactor 开关。1 = 单 Reactor（S1 路径），≥2 = ReactorGroup。
      // 上限不设死：真正的闸门是 fd 预算与临时端口（S2-06 的 c100k 口径），
      // 这里只挡掉明显非法的值（<1 或大到 int 溢出）。
      long long n = 0;
      if (!parse_number(value, &n) || n < 1 || n > 4096) {
        std::fprintf(stderr, "参数错误: --workers 要的是 1..4096，收到 %s\n", value);
        return 2;
      }
      workers = static_cast<int>(n);
      ++i;
    } else if (flag == "--drain-timeout-ms" && value != nullptr) {
      // S2-05 的兜底窗口。它是**兜底不是承诺**：一条卡死的连接不该无限拖住一次下线，
      // 所以到点就强关（代价是那条连接的出向字节被丢弃，客户端看到截断）。
      // 上限 600000（10 分钟）只是把荒谬值挡在门外，真正的对齐要求写在 1.20：
      // **它必须 ≥ S4 的请求超时**，否则正常在飞的请求会先撞上排水线而不是撞上超时。
      long long n = 0;
      if (!parse_number(value, &n) || n < 0 || n > 600000) {
        std::fprintf(stderr, "参数错误: --drain-timeout-ms 要的是 0..600000，收到 %s\n", value);
        return 2;
      }
      drain_timeout_ms = n;
      ++i;
    } else {
      std::fprintf(stderr, "参数错误: 无法识别的选项 %s\n", std::string(flag).c_str());
      usage(stderr);
      return 2;
    }
  }
  config.stop_after_requests = stop_after_requests;
  // 兜底窗口先落到全局原子，**再**装 handler：反过来的话存在一个窗口——信号在装好
  // handler 之后、这一行之前到达，handler 读到的就是默认值而不是命令行给的值。
  // 这里给的是同一个 5000 所以后果一样，但顺序错了就是"迟早会错"的那种写法。
  g_drain_timeout_ms.store(static_cast<std::int64_t>(drain_timeout_ms), std::memory_order_relaxed);

  // 没有 --idle-timeout 这一项，是我故意不加的：S1-04 还没有空闲连接回收
  // （超时策略归 S4），而现在这个 wait_timeout_ms 控制的是 epoll_wait 的醒来间隔，
  // 也就是"停循环最坏等多久"。把一个内部参数包装成"--idle-timeout-ms"给用户，
  // 等于用命令行给还没实现的能力发一张存在的证明。
  if (!have_listen || !have_upstream) {
    std::fprintf(stderr, "参数错误: --serve 需要同时给 --listen 和 --upstream\n");
    return 2;
  }
  // 单 Reactor 路径（workers < 2）只有一条循环。S2-06a 时它的说法是"只会用到第 0 个端点，
  // 容量还是 28,232"；S3-01 之后说法要精确一点：**它的默认目标是第 0 个端点，装了路由 filter
  // 就会按请求连到路由指的那个**。不给它做"一个 worker 自己轮换多端点"的机制照旧成立——
  // 轮换就是没有理由的选择，而理由是路由表给的，不是循环自己编的。
  config.upstream_targets = upstream_endpoints;
  config.default_target = 0;
  if (route_table.size() > 0) {
    std::uint16_t bad_target = 0;
    if (!validate_route_targets(route_table, upstream_endpoints.size(), &bad_target)) {
      std::fprintf(stderr,
                   "参数错误: 路由规则指向端点 %u，而 --upstream 只给了 %zu 个（下标 0..%zu）\n",
                   bad_target, upstream_endpoints.size(), upstream_endpoints.size() - 1);
      return 2;
    }
  }
  const std::string upstream_list = join_upstream_list(upstream_endpoints);

  // --- 多 Reactor 路径（S2-01，workers ≥ 2）-------------------------------
  if (workers >= 2) {
    l7gate::net::ReactorGroup::Config gconfig;
    gconfig.listen_address = config.listen_address;
    gconfig.listen_port = config.listen_port;
    gconfig.upstream_endpoints = upstream_endpoints;
    // 空表就是 nullptr：**不装 filter，链保持空**，于是这一臂就是 S2-06a 那一臂
    // （成本对照的"空链"那一臂也是这么来的，不是靠我另编一个 binary）。
    gconfig.route_table = route_table.empty() ? nullptr : &route_table;
    gconfig.listen_backlog = config.listen_backlog;
    gconfig.worker_count = workers;
    gconfig.max_connections_per_worker = config.max_connections;
    gconfig.connection = config.connection;
    gconfig.stop_after_requests = stop_after_requests;
    gconfig.wait_timeout_ms = config.wait_timeout_ms;

    l7gate::net::ReactorGroup group(gconfig);
    int error = 0;
    if (!group.start_listen(&error)) {
      std::fprintf(stderr, "监听 %s:%u 失败: errno=%d (%s)\n", gconfig.listen_address.c_str(),
                   gconfig.listen_port, error, std::strerror(error));
      return 1;
    }
    g_group_for_signal.store(&group, std::memory_order_relaxed);
    std::signal(SIGINT, on_stop_signal);
    std::signal(SIGTERM, on_stop_signal);

    // fd_start 在 start_listen 之后量：此刻监听 fd + accept 侧 epoll/eventfd +
    // N×(worker epoll/eventfd) 都已打开，与 fd_end（析构之前量）同一视野，
    // 所以"首尾相等"判的仍然是"连接 fd 有没有漏"，常驻项两边都抵消。
    const std::int64_t fd_start = count_open_fds();
    const std::int64_t rss_start = current_rss_kib();
    std::printf("[serve] 监听 %s:%d（workers=%d backlog=%d max_connections_per_worker=%zu "
                "upstream=%s upstream_endpoints=%zu stop_after_requests=%zu wait_timeout_ms=%d "
                "high_water_mark=%zu drain_timeout_ms=%lld）\n",
                gconfig.listen_address.c_str(), group.listen_port(), workers, gconfig.listen_backlog,
                gconfig.max_connections_per_worker, upstream_list.c_str(), upstream_endpoints.size(),
                stop_after_requests, gconfig.wait_timeout_ms,
                gconfig.connection.inbound.high_water_mark, drain_timeout_ms);
    if (!route_table.empty()) {
      // **单独一行，不动上面那条横幅**：`scripts/bench/s1-p99.sh` 是靠横幅里的键名
      // （多 Reactor 有 `workers=`、单 Reactor 有 `max_connections=`）当场判"这一臂到底是
      // 哪条路径"的，改那些键等于让历史脚本对不上路。路由的信息量新起一行给。
      std::printf("[serve] route_rules=%zu no_match_status=404\n", route_table.size());
    }
    std::fflush(stdout);

    const auto started = std::chrono::steady_clock::now();
    const std::uint64_t batches = group.run();
    g_group_for_signal.store(nullptr, std::memory_order_relaxed);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();

    report_stats(group.stats(), group.epoll_registrations(), group.epoll_unregistrations(),
                 group.epoll_modifications(), group.connections_with_read_disarmed(), fd_start,
                 rss_start, static_cast<long long>(elapsed_ms));
    std::printf("[serve] event_batches=%llu\n", static_cast<unsigned long long>(batches));
    std::printf("[serve] workers=%d\n", workers);
    return 0;
  }

  l7gate::net::Reactor reactor(config);
  // 链只能在 run() 之前改（S1-08 那条时序纪律：链在循环线程里被逐条读、没有锁）。
  // 这一行必须在 start_listen/run 之前，而不是"反正现在还没跑"。
  if (!route_table.empty()) {
    reactor.add_request_filter(
        std::make_unique<l7gate::filter::RouteFilter>(route_table));
  }
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
  // 横幅里那个 `upstream=127.0.0.1:PORT` 打的是**默认目标**——单端点配置下它与 S2 时代
  // 逐字节相同（`s1-p99.sh` 与四个冒烟脚本都 grep 这一行，键名与值的形状都不能动）。
  const l7gate::net::Reactor::Config::Target& shown_target =
      config.upstream_targets[config.default_target];
  std::printf("[serve] 监听 %s:%d（backlog=%d max_connections=%zu upstream=%s:%u "
              "stop_after_requests=%zu wait_timeout_ms=%d high_water_mark=%zu drain_timeout_ms=%lld）\n",
              config.listen_address.c_str(), reactor.listen_port(), config.listen_backlog,
              config.max_connections, shown_target.address.c_str(), shown_target.port,
              stop_after_requests, config.wait_timeout_ms,
              config.connection.inbound.high_water_mark, drain_timeout_ms);
  if (!route_table.empty()) {
    // 同多 Reactor 那一臂：新起一行，不碰上面那条签名行。
    std::printf("[serve] route_rules=%zu no_match_status=404 default_target=%u\n",
                route_table.size(), config.default_target);
  }
  std::fflush(stdout);

  const auto started = std::chrono::steady_clock::now();
  const std::uint64_t batches = reactor.run();
  g_reactor_for_signal.store(nullptr, std::memory_order_relaxed);
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - started)
                              .count();

  // 这里还没析构 Reactor，所以"存活连接数"是这次运行真正的终局读数：
  // 循环已经不再处理事件，剩下的每一个都是没人管的。
  report_stats(reactor.stats(), reactor.epoll_registrations(), reactor.epoll_unregistrations(),
               reactor.epoll_modifications(), reactor.connections_with_read_disarmed(), fd_start,
               rss_start, static_cast<long long>(elapsed_ms));
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
