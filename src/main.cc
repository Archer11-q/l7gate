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
#include <thread>
#include <vector>

#include "config/gateway_config.h"
#include "config/nodes_store.h"
#include "core/build_info.h"
#include "filter/lb_filter.h"
#include "filter/route_filter.h"
#include "net/reactor.h"
#include "net/reactor_group.h"
#include "route/nodes.h"
#include "route/router.h"

namespace {

void usage(std::FILE* out) {
  std::fprintf(out,
      "l7gate —— L7 流量治理网关（开发中）\n"
      "用法: l7gate --self-check\n"
      "      l7gate --serve --listen <ipv4:port> --upstream <ipv4:port>[,<ipv4:port>...]\n"
      "               [--serve-requests N] [--max-connections N]\n"
      "               [--high-water-mark BYTES] [--workers N] [--drain-timeout-ms MS]\n"
      "               [--config <path>]\n"
      "               [--nodes-db <host:port>] [--nodes-user U] [--nodes-password P]\n"
      "               [--nodes-schema DB] [--nodes-reload-ms MS] [--nodes-timeout-ms MS]\n"
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
      "--config <path>（S3-02，ADR-19）读一份 JSON 子集配置：upstreams（组名、balance、成员后端）与\n"
      "routes（prefix/regex → 组名）。**只有策略进文件**：监听地址、监听端口、--workers 这些进程形态\n"
      "仍然只走命令行，两边之间没有「谁覆盖谁」这回事。给了 --config 就不必给 --upstream，两个都给会被\n"
      "拒绝——两个来源同时存在时，「哪一份是真相」这件事本身就是一份会漂移的配置。\n"
      "解析或校验不过一律退出码 2 并指出是哪一项，不退回默认值继续跑。\n"
      "路由：前缀取最长命中、正则全串锚定且优先于前缀，匹配原始 origin-form（含 query、不解码、不规范化）；\n"
      "无匹配回 404 并关连接。balance 只认 round_robin / least_conn / consistent_hash。\n"
      "不给 --config 时一个 filter 都不装，链是空的，行为与 S2-06a 逐字节同形。\n"
      "--nodes-*（S3-04，ADR-20）：节点表的 MySQL 来源。文件里声明了 \"source\": \"mysql\"\n"
      "的组，其成员从 <schema>.l7gate_nodes 表读取（组名/address/port/health_path），\n"
      "每 --nodes-reload-ms（默认 5000，0 = 只在启动时读一次）reload 一次。reload 失败\n"
      "保留上一份成员继续服务（不拒绝、不 500），恢复后自动换上新表。MySQL 连不上时\n"
      "mysql 组按空成员启动。配置声明了 source=mysql 但没给 --nodes-schema 是配置矛盾，\n"
      "退出码 2。\n"
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

// 墙钟毫秒。与 reactor.cc 里那份同一口径（system_clock）：健康表的
// last_ejection_epoch_ms 和节点表的 last_publish_epoch_ms 都走这根轴，
// 压测脚本才能把自己的时间线和它们钉在一起。
std::int64_t wall_ms() noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
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
                  std::int64_t rss_start, long long elapsed_ms,
                  const l7gate::route::HealthTable* health = nullptr,
                  const std::vector<std::string>& target_names = {},
                  const l7gate::route::NodesPublisher* nodes = nullptr) {
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
  if (health != nullptr) {
    // S3-03 的观测面。逐目标三行：ejections（误摘率与摘除耗时下界的分子）、
    // probe_ok/probe_fail（探活轮数的分母来源）、last_ejection_epoch_ms（摘除耗时的
    // "网关自报那一端"，与脚本时间线同一根墙钟轴）。键名里的地址冒号换成下划线，
    // 让 grep 不用处理两套分隔符。
    put("health_ejected_now", static_cast<long long>(health->ejected_now()));
    put("health_ejections_blocked_by_valve",
        static_cast<long long>(health->ejections_blocked_by_valve()));
    for (std::uint16_t t = 0; t < health->target_count(); ++t) {
      const l7gate::route::TargetHealthSnapshot s = health->snapshot(t);
      std::string key = "health_t" + std::to_string(t) + "_";
      if (t < target_names.size()) key += target_names[t] + "_";
      put(key + "ejections", static_cast<long long>(s.ejections_total));
      put(key + "probe_ok", static_cast<long long>(s.probe_ok_total));
      put(key + "probe_fail", static_cast<long long>(s.probe_fail_total));
      put(key + "recoveries", static_cast<long long>(s.recoveries_total));
      put(key + "last_ejection_epoch_ms", static_cast<long long>(s.last_ejection_epoch_ms));
    }
  }
  if (nodes != nullptr) {
    // S3-04 节点表的观测面（B-015 的"网关自报那一端"）。
    put("nodes_reloads_ok", static_cast<long long>(nodes->reloads_ok()));
    put("nodes_reloads_failed", static_cast<long long>(nodes->reloads_failed()));
    put("nodes_last_publish_epoch_ms", static_cast<long long>(nodes->last_publish_epoch_ms()));
  }
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
  // S3-02（ADR-19）：策略配置。没给 `--config` 时它整个是空的——**一个 filter 都不装**，
  // 链是空的，每个 worker 走自己的默认目标，那一条路径与 S2-06a 逐字节同形
  // （八份 canonic 档 + B-007 + B-012 不作废的凭据就在这句上）。
  l7gate::config::GatewayConfig gateway_config;
  std::string config_path;
  // S3-04（ADR-20）：节点表的 MySQL 来源。--nodes-schema 给了才启用。
  bool have_nodes_schema = false;
  l7gate::config::NodesDbParams nodes_params;
  long long nodes_reload_ms = 5000;

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
    } else if (flag == "--config" && value != nullptr) {
      // S3-02（ADR-19）。它取代的是 S3-01 那对临时开关 `--route` / `--route-regex`，
      // **不留兼容层**：同一件事有两种配法时，成本对照与故障注入会各测一种，
      // 而这两种测的是不同的代码路径。传了就是无法识别的选项、退出码 2。
      if (!config_path.empty()) {
        std::fprintf(stderr, "参数错误: --config 只能给一次，第二个是 %s\n", value);
        return 2;
      }
      config_path = value;
      ++i;
    } else if (flag == "--nodes-db" && value != nullptr) {
      if (!parse_addr_port(value, &nodes_params.host, &nodes_params.port)) {
        std::fprintf(stderr, "参数错误: --nodes-db 要的是 <host:port>，收到 %s\n", value);
        return 2;
      }
      ++i;
    } else if (flag == "--nodes-user" && value != nullptr) {
      nodes_params.user = value;
      ++i;
    } else if (flag == "--nodes-password" && value != nullptr) {
      nodes_params.password = value;
      ++i;
    } else if (flag == "--nodes-schema" && value != nullptr) {
      nodes_params.database = value;
      have_nodes_schema = true;
      ++i;
    } else if (flag == "--nodes-reload-ms" && value != nullptr) {
      long long n = 0;
      if (!parse_number(value, &n) || n < 0 || n > 86400000) {
        std::fprintf(stderr, "参数错误: --nodes-reload-ms 要的是 0..86400000，收到 %s\n", value);
        return 2;
      }
      nodes_reload_ms = n;
      ++i;
    } else if (flag == "--nodes-timeout-ms" && value != nullptr) {
      long long n = 0;
      if (!parse_number(value, &n) || n < 1 || n > 60000) {
        std::fprintf(stderr, "参数错误: --nodes-timeout-ms 要的是 1..60000，收到 %s\n", value);
        return 2;
      }
      nodes_params.timeout_ms = n;
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
  if (!have_listen) {
    std::fprintf(stderr, "参数错误: --serve 需要 --listen\n");
    return 2;
  }
  // 目标表的来源**必须只有一个**：要么命令行的 `--upstream`，要么 `--config` 里的 `upstreams`。
  // 两个都给就拒绝——不选优先级。一旦允许共存，"哪一份是真相"这件事就成了一段合并规则，
  // 而合并规则是迟早被人改错的地方（ADR-19 第一节那条切分的另一半）。
  if (!config_path.empty() && have_upstream) {
    std::fprintf(stderr, "参数错误: --config 与 --upstream 只能给一个（后端表有两个来源）\n");
    return 2;
  }
  if (config_path.empty() && !have_upstream) {
    std::fprintf(stderr, "参数错误: --serve 需要 --upstream 或 --config 之一\n");
    return 2;
  }
  if (!config_path.empty()) {
    std::string why;
    if (!l7gate::config::load_gateway_config(config_path, &gateway_config, &why)) {
      // 整体失败，不"退回默认配置继续跑"：一个安静少配了一条路由的网关，从外面看只是
      // 负载分布不对，而它的真实原因是我自己把一份坏文件启动了。
      std::fprintf(stderr, "配置错误: %s（来自 %s）\n", why.c_str(), config_path.c_str());
      return 2;
    }
    for (const l7gate::config::Endpoint& endpoint : gateway_config.endpoints()) {
      upstream_endpoints.push_back(
          UpstreamEndpoint{endpoint.address, endpoint.port, endpoint.health_path});
    }
  }

  // --- S3-04（ADR-20）：节点表装配 -----------------------------------------------
  // 文件里声明了 source=mysql 的组，成员从 l7gate_nodes 读。启动时 MySQL 连不上
  // **不退出**（mysql 组按空成员启动、LB 回 502，reload 恢复后补上——ADR-20 第五节）；
  // 但"配置声明了 mysql 而网关没给 --nodes-schema"是配置矛盾，退出码 2。
  std::shared_ptr<l7gate::route::NodesPublisher> nodes_publisher;
  auto nodes_registry = std::make_shared<l7gate::route::TargetRegistry>();
  std::vector<std::string> mysql_group_names;
  for (std::uint16_t g = 0; g < static_cast<std::uint16_t>(gateway_config.groups().size()); ++g) {
    const auto* group = gateway_config.groups().group_at(g);
    if (group != nullptr && group->source_mysql) mysql_group_names.push_back(group->name);
  }
  if (!mysql_group_names.empty() && !have_nodes_schema) {
    std::fprintf(stderr,
                 "配置错误: 有组声明了 source=mysql 但没给 --nodes-schema（组成员没有来源）\n");
    return 2;
  }
  if (!mysql_group_names.empty()) {
    nodes_publisher = std::make_shared<l7gate::route::NodesPublisher>();
    // 静态端点先 intern：它们的下标必须与文件端点表一一对应（0..E-1），
    // ever-seen 的追加从这之后开始。
    for (const l7gate::config::Endpoint& endpoint : gateway_config.endpoints()) {
      nodes_registry->intern(
          l7gate::route::NodeTarget{endpoint.address, endpoint.port, endpoint.health_path});
    }
    l7gate::config::NodesStore store{nodes_params};
    l7gate::route::NodesFetch fetch;
    std::string why;
    if (store.fetch(mysql_group_names, &fetch, &why)) {
      auto view = l7gate::route::build_nodes_view(gateway_config.groups(), fetch,
                                                  *nodes_registry, &why);
      if (view != nullptr) {
        nodes_publisher->publish(view);
        nodes_publisher->note_reload(true, wall_ms(), 0);
      } else {
        std::fprintf(stderr, "节点表装配失败（按空成员启动）: %s\n", why.c_str());
      }
    } else {
      std::fprintf(stderr, "启动时 MySQL 不可用（mysql 组按空成员启动，reload 恢复后补上）: %s\n",
                   why.c_str());
    }
    // worker 的初始目标表 = 注册表现状（静态 + 已到岗成员）；之后的增员走对账 tick。
    upstream_endpoints.clear();
    for (std::uint16_t i = 0; i < nodes_registry->size(); ++i) {
      const auto* node = nodes_registry->at(i);
      if (node == nullptr) break;
      upstream_endpoints.push_back(
          UpstreamEndpoint{node->address, node->port, node->health_path});
    }
  }
  // 单 Reactor 路径（workers < 2）只有一条循环。它的默认目标是第 0 个端点；装了路由/均衡
  // 就会按请求连到策略指的那个。不给它做"一个 worker 自己轮换多端点"的机制照旧成立——
  // 轮换就是没有理由的选择，理由是策略给的，不是循环自己编的。
  config.upstream_targets = upstream_endpoints;
  config.default_target = 0;
  const std::string upstream_list = join_upstream_list(upstream_endpoints);
  // 有没有装任何 filter，只看文件里有没有对应的那一段。
  const bool has_routes = gateway_config.has_routes;
  const bool has_groups = !gateway_config.groups().empty();
  // S3-03（ADR-19 追加第四条）：health 段在才建健康表。**必须在这里建而不是在两条路径里
  // 各建一份**——共享表的读者是全部 worker + LB filter + report_stats，三处拿的必须是
  // 同一份，摘除的账才只有一个答案。
  std::shared_ptr<l7gate::route::HealthTable> health_table;
  if (gateway_config.health().present) {
    // 节点表启用时健康表按**下标空间全量**开容量（ever-seen：新成员只"启用"新槽位，
    // 不 resize 共享表——ADR-20 第四节）。纯文件配置维持"容量 = 成员数"的现状，
    // 一字节不多花。
    const std::size_t capacity =
        nodes_publisher != nullptr
            ? static_cast<std::size_t>(l7gate::route::kMaxUpstreamTarget) + 1
            : 0;
    health_table = std::make_shared<l7gate::route::HealthTable>(
        upstream_endpoints.size(), gateway_config.health().policy, capacity);
  }
  // 逐目标健康计数器的键名后缀（health_t0_127_0_0_1_18081_ejections=…）。
  std::vector<std::string> health_target_names;
  health_target_names.reserve(upstream_endpoints.size());
  for (const UpstreamEndpoint& endpoint : upstream_endpoints) {
    health_target_names.push_back(endpoint.address + "_" + std::to_string(endpoint.port));
  }

  // S3-04（ADR-20 第四节）：reload 线程。MySQL 查询是阻塞 IO，不进 worker 事件循环；
  // 它与 worker 的唯一接触面是 NodesPublisher 那根原子快照指针。成员没变就不发布：
  // 查了一遍不等于发布了一次，观测面不把两者混成同一个数。
  std::thread nodes_reload_thread;
  std::atomic<bool> nodes_stop{false};
  if (nodes_publisher != nullptr && nodes_reload_ms > 0) {
    nodes_reload_thread = std::thread([&] {
      const l7gate::config::NodesStore store{nodes_params};
      std::string last_signature;
      while (!nodes_stop.load(std::memory_order_relaxed)) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(nodes_reload_ms);
        while (!nodes_stop.load(std::memory_order_relaxed) &&
               std::chrono::steady_clock::now() < deadline) {
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (nodes_stop.load(std::memory_order_relaxed)) break;
        l7gate::route::NodesFetch fetch;
        std::string why;
        const std::int64_t started = wall_ms();
        if (!store.fetch(mysql_group_names, &fetch, &why)) {
          nodes_publisher->note_reload(false, started, wall_ms() - started);
          std::fprintf(stderr, "节点表 reload 失败（保留上一份）: %s\n", why.c_str());
          continue;
        }
        std::string signature;
        for (const auto& group : fetch.groups) {
          for (const auto& member : group.members) {
            signature += group.name + "|" + member.endpoint_key() + "|" + member.health_path + ";";
          }
        }
        if (signature == last_signature) continue;
        auto view = l7gate::route::build_nodes_view(gateway_config.groups(), fetch,
                                                    *nodes_registry, &why);
        if (view == nullptr) {
          nodes_publisher->note_reload(false, started, wall_ms() - started);
          std::fprintf(stderr, "节点表快照装配失败（保留上一份）: %s\n", why.c_str());
          continue;
        }
        nodes_publisher->publish(view);
        last_signature = signature;
        nodes_publisher->note_reload(true, wall_ms(), wall_ms() - started);
      }
    });
  }

  // --- 多 Reactor 路径（S2-01，workers ≥ 2）-------------------------------
  if (workers >= 2) {
    l7gate::net::ReactorGroup::Config gconfig;
    gconfig.listen_address = config.listen_address;
    gconfig.listen_port = config.listen_port;
    gconfig.upstream_endpoints = upstream_endpoints;
    // 没给 --config 时这两个都是 nullptr：**一个 filter 都不装，链保持空**，于是这一臂就是
    // S2-06a 那一臂（B-012 成本对照的"空链"那一臂也是这么来的，不是靠我另编一个 binary）。
    gconfig.route_table = has_routes ? &gateway_config.routes() : nullptr;
    gconfig.groups = has_groups ? &gateway_config.groups() : nullptr;
    gconfig.health = health_table;
    gconfig.nodes = nodes_publisher;
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
    if (has_routes || has_groups) {
      // **单独一行，不动上面那条横幅**：`scripts/bench/s1-p99.sh` 是靠横幅里的键名
      // （多 Reactor 有 `workers=`、单 Reactor 有 `max_connections=`）当场判"这一臂到底是
      // 哪条路径"的，改那些键等于让历史脚本对不上路。策略的信息量新起一行给。
      std::printf("[serve] policy=on config=%s upstream_groups=%zu route_rules=%zu\n",
                  config_path.c_str(), gateway_config.groups().size(),
                  gateway_config.routes().size());
      if (health_table != nullptr) {
        // 健康检查装了什么，一条都不省：误摘率与摘除耗时的分母（探活轮数由间隔与
        // 运行时长决定、健康后端数由这行横幅的成员数背书）从这里对账。
        const l7gate::route::HealthPolicy& hp = gateway_config.health().policy;
        std::printf("[serve] health=on max_fails=%u fail_timeout_ms=%lld probe_interval_ms=%lld "
                    "probe_timeout_ms=%lld max_ejection_percent=%u targets=%zu\n",
                    hp.max_fails, static_cast<long long>(hp.fail_timeout_ms),
                    static_cast<long long>(hp.probe_interval_ms),
                    static_cast<long long>(hp.probe_timeout_ms), hp.max_ejection_percent,
                    health_table->target_count());
      }
      if (nodes_publisher != nullptr) {
        std::printf("[serve] nodes=on schema=%s host=%s:%u reload_ms=%lld timeout_ms=%lld "
                    "mysql_groups=%zu\n",
                    nodes_params.database.c_str(), nodes_params.host.c_str(), nodes_params.port,
                    nodes_reload_ms, static_cast<long long>(nodes_params.timeout_ms), mysql_group_names.size());
      }
    }
    std::fflush(stdout);

    const auto started = std::chrono::steady_clock::now();
    const std::uint64_t batches = group.run();
    g_group_for_signal.store(nullptr, std::memory_order_relaxed);
    nodes_stop.store(true, std::memory_order_relaxed);
    if (nodes_reload_thread.joinable()) nodes_reload_thread.join();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();

    report_stats(group.stats(), group.epoll_registrations(), group.epoll_unregistrations(),
                 group.epoll_modifications(), group.connections_with_read_disarmed(), fd_start,
                 rss_start, static_cast<long long>(elapsed_ms), health_table.get(),
                 health_target_names, nodes_publisher.get());
    std::printf("[serve] event_batches=%llu\n", static_cast<unsigned long long>(batches));
    std::printf("[serve] workers=%d\n", workers);
    return 0;
  }

  // 单 Reactor 这一臂的均衡状态：必须**在 Reactor 构造之前**就进 config，因为 Reactor 存的
  // 是 config 的一份副本——构造之后再写就等于没写，那份配对账会只加不减，
  // 而 least_conn 的失败形态是"永远选第一个成员"，看起来完全像在正常工作。
  std::shared_ptr<l7gate::route::LbState> lb_state;
  if (has_groups) {
    lb_state = std::make_shared<l7gate::route::LbState>(upstream_endpoints.size(),
                                                        gateway_config.groups().size());
    config.lb_state = lb_state;
  }
  // 同一条构造期纪律：健康表在 Reactor 构造之前进 config（Reactor 存 config 的副本）。
  config.health = health_table;
  config.nodes = nodes_publisher;
  l7gate::net::Reactor reactor(config);
  // 链只能在 run() 之前改（S1-08 那条时序纪律：链在循环线程里被逐条读、没有锁）。
  // 顺序 = 路由在前、均衡在后——均衡读的那个组下标正是路由这一环写下的。
  if (has_routes) {
    reactor.add_request_filter(
        std::make_unique<l7gate::filter::RouteFilter>(gateway_config.routes()));
  }
  if (lb_state != nullptr) {
    reactor.add_request_filter(std::make_unique<l7gate::filter::LoadBalanceFilter>(
        gateway_config.groups(), std::move(lb_state), health_table, nodes_publisher));
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
  if (has_routes || has_groups) {
    // 同多 Reactor 那一臂：新起一行，不碰上面那条签名行。
    std::printf("[serve] policy=on config=%s upstream_groups=%zu route_rules=%zu default_target=%u\n",
                config_path.c_str(), gateway_config.groups().size(),
                gateway_config.routes().size(), config.default_target);
    if (health_table != nullptr) {
      const l7gate::route::HealthPolicy& hp = gateway_config.health().policy;
      std::printf("[serve] health=on max_fails=%u fail_timeout_ms=%lld probe_interval_ms=%lld "
                  "probe_timeout_ms=%lld max_ejection_percent=%u targets=%zu\n",
                  hp.max_fails, static_cast<long long>(hp.fail_timeout_ms),
                  static_cast<long long>(hp.probe_interval_ms),
                  static_cast<long long>(hp.probe_timeout_ms), hp.max_ejection_percent,
                  health_table->target_count());
    }
    if (nodes_publisher != nullptr) {
      std::printf("[serve] nodes=on schema=%s host=%s:%u reload_ms=%lld timeout_ms=%lld "
                  "mysql_groups=%zu\n",
                  nodes_params.database.c_str(), nodes_params.host.c_str(), nodes_params.port,
                  nodes_reload_ms, static_cast<long long>(nodes_params.timeout_ms), mysql_group_names.size());
    }
  }
  std::fflush(stdout);

  const auto started = std::chrono::steady_clock::now();
  const std::uint64_t batches = reactor.run();
  g_reactor_for_signal.store(nullptr, std::memory_order_relaxed);
  nodes_stop.store(true, std::memory_order_relaxed);
  if (nodes_reload_thread.joinable()) nodes_reload_thread.join();
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - started)
                              .count();

  // 这里还没析构 Reactor，所以"存活连接数"是这次运行真正的终局读数：
  // 循环已经不再处理事件，剩下的每一个都是没人管的。
  report_stats(reactor.stats(), reactor.epoll_registrations(), reactor.epoll_unregistrations(),
               reactor.epoll_modifications(), reactor.connections_with_read_disarmed(), fd_start,
               rss_start, static_cast<long long>(elapsed_ms), health_table.get(),
               health_target_names, nodes_publisher.get());
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
