#include "metrics/metrics_render.h"

#include <cinttypes>
#include <string_view>
#include <utility>

namespace l7gate::metrics {
namespace {

// text format 0.0.4 的一行：`# TYPE <name> <counter|gauge>` 必须在样本之前，
// 且一个族只能有一次。这里不做去重——**每个名字恰好一次**是验收 ① 的断言之一，
// 渲染器自己不该悄悄兼容"同一个键写两遍"那种 bug。
class Emitter {
 public:
  explicit Emitter(std::string* out) noexcept : out_(out) {}

  void help(std::string_view name, std::string_view text) {
    if (text.empty()) return;
    out_->append("# HELP ");
    out_->append(name);
    out_->push_back(' ');
    out_->append(text);
    out_->push_back('\n');
  }

  void sample(std::string_view name, std::int64_t value, const char* type,
              std::string_view help_text) {
    help(name, help_text);
    out_->append("# TYPE ");
    out_->append(name);
    out_->append(type == kCounter ? " counter\n" : " gauge\n");
    out_->append(name);
    out_->push_back(' ');
    out_->append(std::to_string(value));
    out_->push_back('\n');
  }

  void counter(std::string_view name, std::uint64_t value, std::string_view help_text = {}) {
    sample(name, static_cast<std::int64_t>(value), kCounter, help_text);
  }

  // 这一族的值可以是 -1（`/proc` 读不到时不拿 0 冒充"没有泄漏"，与 `main.cc` 那条
  // 口径同源），所以签名是 int64 而不是 uint64。
  void gauge(std::string_view name, std::int64_t value, std::string_view help_text = {}) {
    sample(name, value, kGauge, help_text);
  }

 private:
  static constexpr const char* kCounter = "counter";
  static constexpr const char* kGauge = "gauge";
  std::string* out_;
};

using CounterField = std::atomic<std::uint64_t> l7gate::net::ReactorStats::*;

// `ReactorStats` 那 50 个键。**顺序 = `main.cc:report_stats` 的发序**，
// 名字一个字母不动（验收 ①：键名不许改，历史脚本与文档都靠它们）。
// gauge 只有 `live_connections` 一个：它是可上可下的当前数，其余全是只增计数。
struct StatsEntry {
  const char* name;
  CounterField field;
  bool gauge;
  const char* help;
};

constexpr StatsEntry kStatsEntries[] = {
    {"downstream_accepted", &l7gate::net::ReactorStats::downstream_accepted, false, ""},
    {"downstream_rejected_over_capacity", &l7gate::net::ReactorStats::downstream_rejected_over_capacity, false,
     "容量满时被拒的下游连接条数（不是 accept 失败）"},
    {"accept_errors", &l7gate::net::ReactorStats::accept_errors, false, ""},
    {"upstream_opened", &l7gate::net::ReactorStats::upstream_opened, false, ""},
    {"upstream_connect_failed", &l7gate::net::ReactorStats::upstream_connect_failed, false, ""},
    {"upstream_connect_timeout", &l7gate::net::ReactorStats::upstream_connect_timeout, false, ""},
    {"upstream_head_timeout", &l7gate::net::ReactorStats::upstream_head_timeout, false, ""},
    {"upstream_body_idle_timeout", &l7gate::net::ReactorStats::upstream_body_idle_timeout, false, ""},
    {"requests_parsed", &l7gate::net::ReactorStats::requests_parsed, false, ""},
    {"requests_rejected", &l7gate::net::ReactorStats::requests_rejected, false, ""},
    {"connections_constructed", &l7gate::net::ReactorStats::connections_constructed, false, ""},
    {"connections_destructed", &l7gate::net::ReactorStats::connections_destructed, false, ""},
    {"live_connections", &l7gate::net::ReactorStats::live_connections, true, ""},
    {"connections_closed", &l7gate::net::ReactorStats::connections_closed, false, ""},
    {"bytes_down_to_upstream", &l7gate::net::ReactorStats::bytes_down_to_upstream, false, ""},
    {"bytes_up_to_downstream", &l7gate::net::ReactorStats::bytes_up_to_downstream, false, ""},
    {"header_scan_bytes", &l7gate::net::ReactorStats::header_scan_bytes, false, ""},
    {"parse_calls", &l7gate::net::ReactorStats::parse_calls, false,
     "解析器被调用的次数：一个请求可能因视图作废被解析两次，那条不重复进 requests_parsed"},
    {"outbound_buffers_created", &l7gate::net::ReactorStats::outbound_buffers_created, false, ""},
    {"scratches_created", &l7gate::net::ReactorStats::scratches_created, false, ""},
    {"high_water_hits", &l7gate::net::ReactorStats::high_water_hits, false, ""},
    {"backpressure_pause_count", &l7gate::net::ReactorStats::backpressure_pause_count, false, ""},
    {"backpressure_resume_count", &l7gate::net::ReactorStats::backpressure_resume_count, false, ""},
    {"bytes_read_downstream", &l7gate::net::ReactorStats::bytes_read_downstream, false, ""},
    {"bytes_read_upstream", &l7gate::net::ReactorStats::bytes_read_upstream, false, ""},
    {"upgrades_101", &l7gate::net::ReactorStats::upgrades_101, false, ""},
    {"bytes_tunnel_down", &l7gate::net::ReactorStats::bytes_tunnel_down, false, ""},
    {"bytes_tunnel_up", &l7gate::net::ReactorStats::bytes_tunnel_up, false, ""},
    {"close_headers_honored", &l7gate::net::ReactorStats::close_headers_honored, false, ""},
    {"gateway_replies_sent", &l7gate::net::ReactorStats::gateway_replies_sent, false, ""},
    {"gateway_replies_connect_failed", &l7gate::net::ReactorStats::gateway_replies_connect_failed, false, ""},
    {"gateway_replies_all_ejected", &l7gate::net::ReactorStats::gateway_replies_all_ejected, false, ""},
    {"gateway_replies_over_capacity", &l7gate::net::ReactorStats::gateway_replies_over_capacity, false, ""},
    {"upstream_retries", &l7gate::net::ReactorStats::upstream_retries, false,
     "connect 失败后换成员重放的次数（ADR-23：只一次、只换成员、只在握手没成时）"},
    {"gateway_replies_after_retry", &l7gate::net::ReactorStats::gateway_replies_after_retry, false, ""},
    {"upstream_truncated_before_head", &l7gate::net::ReactorStats::upstream_truncated_before_head, false, ""},
    {"upstream_truncated_in_body", &l7gate::net::ReactorStats::upstream_truncated_in_body, false, ""},
    {"responses_completed", &l7gate::net::ReactorStats::responses_completed, false, ""},
    {"pool_borrows", &l7gate::net::ReactorStats::pool_borrows, false, ""},
    {"pool_returns", &l7gate::net::ReactorStats::pool_returns, false, ""},
    {"pool_evictions_eof", &l7gate::net::ReactorStats::pool_evictions_eof, false, ""},
    {"pool_evictions_full", &l7gate::net::ReactorStats::pool_evictions_full, false, ""},
    {"drain_requested", &l7gate::net::ReactorStats::drain_requested, false, ""},
    {"drain_in_flight_at_entry", &l7gate::net::ReactorStats::drain_in_flight_at_entry, false, ""},
    {"drain_closed_idle", &l7gate::net::ReactorStats::drain_closed_idle, false, ""},
    {"drain_closed_buffered", &l7gate::net::ReactorStats::drain_closed_buffered, false, ""},
    {"drain_tunnels_hard_closed", &l7gate::net::ReactorStats::drain_tunnels_hard_closed, false, ""},
    {"drain_forced_timeout", &l7gate::net::ReactorStats::drain_forced_timeout, false, ""},
    {"drain_read_disarmed_at_entry", &l7gate::net::ReactorStats::drain_read_disarmed_at_entry, false, ""},
    {"drain_rejected_new_connections", &l7gate::net::ReactorStats::drain_rejected_new_connections, false, ""},
};

// 1.31 边界⑥ 那句话必须出现在读数旁边（ADR-24 第五节：面板与 README 要有出处可指）。
constexpr std::string_view kHealthEjectedNowHelp =
    "当前被摘除的目标数；0 不等于没有坏后端：只装 health 且探活会答而业务恒 5xx 时摘不掉（ADR-12 追加三）";

struct TargetEntry {
  const char* suffix;
  std::uint64_t l7gate::route::TargetHealthSnapshot::*counter;
  const char* help;
};

// 每目标那一族的十个后缀，**顺序 = `main.cc:368-380`**：四格计数 →
// `last_ejection_epoch_ms`（gauge，可以是 0 也可以是墙钟时刻）→ `trips_rate`
// （计数，见下面那条 help）→ 半开门的四格。中间那两格的类型与前后不同，
// 所以这张表被它们切成头尾两段，而不是塞进同一张表再在循环里判类型。
constexpr TargetEntry kTargetHead[] = {
    {"ejections", &l7gate::route::TargetHealthSnapshot::ejections_total, ""},
    {"probe_ok", &l7gate::route::TargetHealthSnapshot::probe_ok_total, ""},
    {"probe_fail", &l7gate::route::TargetHealthSnapshot::probe_fail_total, ""},
    {"recoveries", &l7gate::route::TargetHealthSnapshot::recoveries_total, ""},
};

constexpr TargetEntry kTargetTail[] = {
    {"half_open_admitted", &l7gate::route::TargetHealthSnapshot::half_open_admitted_total, ""},
    {"half_open_denied", &l7gate::route::TargetHealthSnapshot::half_open_denied_total, ""},
    {"half_open_recovered", &l7gate::route::TargetHealthSnapshot::half_open_recovered_total, ""},
    {"half_open_reopened", &l7gate::route::TargetHealthSnapshot::half_open_reopened_total, ""},
};

}  // namespace

std::string sanitize_metric_name(std::string_view name) {
  std::string out(name);
  // 名字法则 `[a-zA-Z_:][a-zA-Z0-9_:]*`：点号不在集合里、冒号是保留分隔符。
  // 只换这两类，别的字符不动——地址串里除此之外不会有非法字符。
  for (char& c : out) {
    if (c == '.' || c == ':') c = '_';
  }
  return out;
}

std::string render(const Sources& sources, const ProcSample& proc, std::int64_t now_ms) {
  std::string out;
  // 一次全装配的 exposition 是 100~200 行、每行几十字节。预留到 16 KiB 是为了
  // 让"渲染"这件事不发生第二次 realloc——它在采集线程上，但分配抖动会推迟响应。
  out.reserve(16384);
  Emitter e(&out);

  if (sources.stats == nullptr) return out;
  l7gate::net::ReactorStats& st = *sources.stats;

  // --- elapsed_ms：与退出账同一个原点（`main.cc:942` 那个 started）------------------
  if (sources.started_at_ms != nullptr) {
    const std::int64_t origin = sources.started_at_ms->load(std::memory_order_relaxed);
    e.gauge("elapsed_ms", origin > 0 ? now_ms - origin : 0,
            "自 serve 循环启动以来的毫秒数；原点与退出账那一格同一个");
  }

  // --- 全 worker 共享的那张原子表 ---------------------------------------------------
  for (const StatsEntry& entry : kStatsEntries) {
    const std::uint64_t value = st.load(st.*(entry.field));
    if (entry.gauge) {
      e.gauge(entry.name, static_cast<std::int64_t>(value), entry.help);
    } else {
      e.counter(entry.name, value, entry.help);
    }
  }

  // --- 每 worker 各一份、按拍抄进原子槽的那一族 --------------------------------------
  // 求和集合与 `reactor_group.cc:305-309` 逐字一致：**只含 worker 的那几份 Epoll 实例**，
  // accept 侧的 `accept_epoll_` 与指标线程自己的 Epoll 都不进（ADR-24 第四节那条口径陷阱）。
  const l7gate::net::LoopObservationSum sum =
      l7gate::net::sum_loop_observations(sources.loop, sources.loop_count);
  e.counter("epoll_registrations", sum.epoll_registrations);
  e.counter("epoll_unregistrations", sum.epoll_unregistrations);
  e.counter("epoll_modifications", sum.epoll_modifications,
            "worker 循环里成功的 epoll_ctl(MOD) 次数；B-018 用它除以 requests_parsed 当每请求 syscall 面那把尺");
  e.gauge("read_disarmed_connections", static_cast<std::int64_t>(sum.read_disarmed_connections),
          "此刻不再消费下游可读事件的连接数；运行期读数最多滞后一个采样周期（1s），精确值仍看退出账");

  // --- 进程级那一族：`fd_end`/`rss_end_kib` 不在这里（ADR-24 第五节例外 1）-------------
  e.gauge("fd_start", sources.fd_start, "启动时的 fd 基线（与退出账同一次采样）");
  e.gauge("fd_open_now", proc.fd_open_now, "此刻的 fd 数；由采集线程读 /proc，不是 worker");
  e.gauge("rss_start_kib", sources.rss_start_kib);
  e.gauge("rss_kib_now", proc.rss_kib_now,
          "此刻 VmRSS；B-011 已证 RSS 在 128 连接这个尺度不是可用仪器，别拿它读每连接内存的构成");

  // --- 健康与熔断（未装配就不出现，与退出账同一条 if）----------------------------------
  if (sources.health != nullptr) {
    const l7gate::route::HealthTable& health = *sources.health;
    e.gauge("health_ejected_now", static_cast<std::int64_t>(health.ejected_now()), kHealthEjectedNowHelp);
    e.counter("health_ejections_blocked_by_valve", health.ejections_blocked_by_valve());
    e.gauge("circuit_half_open_now", static_cast<std::int64_t>(health.half_open_now()));
    e.counter("health_all_denied_fallback", health.all_denied_fallbacks());

    const std::uint16_t targets = static_cast<std::uint16_t>(health.target_count());
    for (std::uint16_t t = 0; t < targets; ++t) {
      const l7gate::route::TargetHealthSnapshot s = health.snapshot(t);
      std::string prefix = "health_t" + std::to_string(t) + "_";
      if (sources.target_names != nullptr && t < sources.target_names->size()) {
        prefix += sanitize_metric_name((*sources.target_names)[t]) + "_";
      }
      for (const TargetEntry& entry : kTargetHead) {
        e.counter((prefix + entry.suffix).c_str(), s.*(entry.counter), entry.help);
      }
      // 这两格的类型与前四格不同，所以夹在中间而不是并表（顺序仍与退出账逐行对齐）。
      e.gauge((prefix + "last_ejection_epoch_ms").c_str(), s.last_ejection_epoch_ms);
      // **`trips_rate` 是次数不是比率**（`health.h:119`：被错误率窗口摘掉的次数）。
      // 键名一个字母不动，TYPE 按真相当 counter，并在这里把那句话写给读的人。
      e.counter((prefix + "trips_rate").c_str(), s.rate_trips_total,
                "被错误率窗口摘除的次数（计数，不是比率）；名字里的 rate 是历史包袱，见 ADR-24 第五节");
      for (const TargetEntry& entry : kTargetTail) {
        e.counter((prefix + entry.suffix).c_str(), s.*(entry.counter), entry.help);
      }
    }
  }

  // --- 节点表（MySQL 来源）-------------------------------------------------------------
  if (sources.nodes.present) {
    e.counter("nodes_reloads_ok", sources.nodes.reloads_ok);
    e.counter("nodes_reloads_failed", sources.nodes.reloads_failed);
    e.gauge("nodes_last_publish_epoch_ms", sources.nodes.last_publish_epoch_ms);
  }

  // --- 配置热重载（S5-02，只在 arm 时存在）------------------------------------------------
  if (sources.config.present) {
    // `config_checks` 与 `config_publishes` 是两个数，不许合并成一格：查了一遍不等于
    // 发布了一次（ADR-20 第五节那对 `nodes_reloads_*` 立的口径在这里原样适用）。
    e.gauge("config_gen", sources.config.generation,
            "当前生效的配置世代号，只在成功发布时 +1；拒载不动");
    e.counter("config_checks", sources.config.checks, "到点做的那次 stat 检查的次数");
    e.counter("config_publishes", sources.config.publishes,
              "成功换代的次数 = 全仓库唯一那次原子 store 的次数");
    e.counter("config_rejects", sources.config.rejects,
              "被拒的 reload 轮次总数（= syntax + scope），旧世代继续服务");
    e.counter("config_rejects_syntax", sources.config.rejects_syntax, "JSON 语法错那一类");
    e.counter("config_rejects_scope", sources.config.rejects_scope,
              "字段越界 / 定义域外（限流参数、段出现消失、命令行来源、mysql 组）/ 超大 那一类");
    e.counter("config_snapshots_constructed", sources.config.snapshots_constructed,
              "ConfigSnapshot 构造次数");
    e.counter("config_snapshots_destructed", sources.config.snapshots_destructed,
              "ConfigSnapshot 析构次数（最后一个引用释放之后）");
    e.gauge("config_snapshots_live", static_cast<std::int64_t>(sources.config.snapshots_live),
            "在册世代数 = constructed - destructed。稳态 1；换代排水期短暂为 2 —— "
            "它就是「旧快照在最后一个引用释放前不回收」那句的可断言形态");
    e.gauge("config_last_publish_epoch_ms", sources.config.last_publish_epoch_ms,
            "最后一次成功换代的墙钟毫秒（与脚本时间线同一根轴，B-015 那两格同法）");
    e.gauge("config_last_publish_duration_ms", sources.config.last_publish_duration_ms,
            "那一次从读到发布的耗时，全在 watcher 那一拍里");
  }

  // --- 限流：`report_rate_limit` 那一行的四键，一行一键 ----------------------------------
  if (sources.rate_limit.present) {
    e.counter("rate_limit_allowed", sources.rate_limit.allowed);
    e.counter("rate_limit_rejected", sources.rate_limit.rejected,
              "被本地桶拒掉的条数=429 的直接证据（S5-03 面板那一格要它与状态码分布互相背书）");
    e.counter("rate_limit_overflow_keys", sources.rate_limit.overflow_keys);
    e.gauge("rate_limit_keys", static_cast<std::int64_t>(sources.rate_limit.keys),
            "桶表当前键数；O(1) 取值但要拿桶表的锁，每条请求也拿同一把（ADR-24 第三节 4）");
  }

  // --- Redis：`report_redis` 的五键（退出账要求 settler 与 limiter 同时在位）-------------
  if (sources.redis.present) {
    e.counter("redis_settles_ok", sources.redis.settles_ok);
    e.counter("redis_settles_failed", sources.redis.settles_failed);
    e.gauge("redis_degraded", sources.redis.degraded ? 1 : 0);
    e.counter("redis_settle_commands", sources.redis.settle_commands);
    e.counter("rate_limit_capped_rejects", sources.rate_limit.capped_rejects,
              "Redis 授权的夹子比本地桶更紧时挡下的条数（免锁聚合读法，ADR-24 第三节）");
  }

  // --- 进程形态与仪器自己的账 -------------------------------------------------------------
  if (sources.expose_workers_key) {
    e.gauge("workers", sources.workers, "业务 worker 循环条数；指标暴露面那条线程不进这一格");
  }
  // `event_batches` 不发（ADR-24 第五节例外 2）。
  e.counter("metrics_scrapes_ok", sources.scrapes_ok,
            "被成功服务的抓取次数（下界证据：计数不动之前先证明这条路真的通过）");
  e.counter("metrics_scrapes_failed", sources.scrapes_failed,
            "坏请求行 / 超限 / 超时 / 写不出的抓取次数");
  e.counter("metrics_render_bytes", sources.render_bytes, "已发出的 exposition 字节数");

  return out;
}

}  // namespace l7gate::metrics
