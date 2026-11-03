// S5-01（ADR-24）：指标暴露面——**格式层**的用例。
//
// 这一份只测渲染，不碰 socket 也不碰事件循环：文本格式的正确性没有第三方兜底
// （ADR-24 第一节记的那笔代价），所以它必须是能被单独钉住的那一块。
// 端到端那三条（抓取不进业务计数 / 关闭态不写槽 / 坏请求进失败账）在
// `reactor_test.cc` 的 `S5_01_MetricsE2E` 里，那里才有真的循环与真的 fd。
#include <cstdint>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "filter/rate_limit.h"
#include "metrics/metrics_render.h"
#include "net/loop_observation.h"
#include "net/stats.h"
#include "route/health.h"

namespace {

using l7gate::metrics::ProcSample;
using l7gate::metrics::Sources;
using l7gate::metrics::render;
using l7gate::metrics::sanitize_metric_name;
using l7gate::net::ReactorStats;
using l7gate::route::HealthPolicy;
using l7gate::route::HealthTable;

constexpr char kTargetName[] = "127.0.0.1_18081";  // 与 `main.cc:763` 同一形状（带点）

// 一次"全装配"的渲染：health 1 个目标、限流、Redis、节点表、多 Reactor 那一臂。
struct FullAssembly {
  static constexpr std::size_t kWorkers = 2;
  ReactorStats stats;
  // 连续数组（与 `ReactorGroup` 持有的那份同形）：`LoopObservation` 不可拷也不可搬，
  // 所以这里只能是 `unique_ptr<T[]>`，不能是 `vector`。
  std::unique_ptr<l7gate::net::LoopObservation[]> slots =
      std::make_unique<l7gate::net::LoopObservation[]>(kWorkers);
  HealthPolicy policy;
  std::shared_ptr<HealthTable> health = std::make_shared<HealthTable>(1, policy);
  std::vector<std::string> target_names{kTargetName};
  std::atomic<std::int64_t> started_at_ms{1'000'000};

  Sources sources() {
    slots[0].epoll_registrations.store(6, std::memory_order_relaxed);
    slots[1].epoll_registrations.store(4, std::memory_order_relaxed);
    slots[0].epoll_modifications.store(11, std::memory_order_relaxed);
    Sources s;
    s.stats = &stats;
    s.loop = slots.get();
    s.loop_count = kWorkers;
    s.health = health.get();
    s.target_names = &target_names;
    s.workers = static_cast<int>(kWorkers);
    s.expose_workers_key = true;
    s.fd_start = 12;
    s.rss_start_kib = 3'456;
    s.started_at_ms = &started_at_ms;
    s.rate_limit.present = true;
    s.rate_limit.allowed = 7;
    s.rate_limit.rejected = 3;
    s.rate_limit.overflow_keys = 0;
    s.rate_limit.keys = 2;
    s.rate_limit.capped_rejects = 1;
    s.redis.present = true;
    s.redis.settles_ok = 5;
    s.redis.settles_failed = 1;
    s.redis.settle_commands = 6;
    s.redis.degraded = true;
    s.nodes.present = true;
    s.nodes.reloads_ok = 4;
    s.nodes.reloads_failed = 0;
    s.nodes.last_publish_epoch_ms = 1'700'000'000;
    return s;
  }
};

// `# TYPE` 全表。这份清单是**手写**的，不是从渲染器的表里生成的——它的用处正是
// "有人改了键名或翻了 TYPE，这里就红"（验收 ①：键名不许改）。
// 期望的顺序 = 渲染顺序 = `main.cc:report_stats` 的发序。
struct Expected {
  const char* name;
  const char* type;
};

const std::vector<Expected>& expected_full_assembly() {
  static const std::vector<Expected> kList = {
      {"elapsed_ms", "gauge"},
      {"downstream_accepted", "counter"},
      {"downstream_rejected_over_capacity", "counter"},
      {"accept_errors", "counter"},
      {"upstream_opened", "counter"},
      {"upstream_connect_failed", "counter"},
      {"upstream_connect_timeout", "counter"},
      {"upstream_head_timeout", "counter"},
      {"upstream_body_idle_timeout", "counter"},
      {"requests_parsed", "counter"},
      {"requests_rejected", "counter"},
      {"connections_constructed", "counter"},
      {"connections_destructed", "counter"},
      {"live_connections", "gauge"},
      {"connections_closed", "counter"},
      {"bytes_down_to_upstream", "counter"},
      {"bytes_up_to_downstream", "counter"},
      {"header_scan_bytes", "counter"},
      {"parse_calls", "counter"},
      {"outbound_buffers_created", "counter"},
      {"scratches_created", "counter"},
      {"high_water_hits", "counter"},
      {"backpressure_pause_count", "counter"},
      {"backpressure_resume_count", "counter"},
      {"bytes_read_downstream", "counter"},
      {"bytes_read_upstream", "counter"},
      {"upgrades_101", "counter"},
      {"bytes_tunnel_down", "counter"},
      {"bytes_tunnel_up", "counter"},
      {"close_headers_honored", "counter"},
      {"gateway_replies_sent", "counter"},
      {"gateway_replies_connect_failed", "counter"},
      {"gateway_replies_all_ejected", "counter"},
      {"gateway_replies_over_capacity", "counter"},
      {"upstream_retries", "counter"},
      {"gateway_replies_after_retry", "counter"},
      {"upstream_truncated_before_head", "counter"},
      {"upstream_truncated_in_body", "counter"},
      {"responses_completed", "counter"},
      {"pool_borrows", "counter"},
      {"pool_returns", "counter"},
      {"pool_evictions_eof", "counter"},
      {"pool_evictions_full", "counter"},
      {"drain_requested", "counter"},
      {"drain_in_flight_at_entry", "counter"},
      {"drain_closed_idle", "counter"},
      {"drain_closed_buffered", "counter"},
      {"drain_tunnels_hard_closed", "counter"},
      {"drain_forced_timeout", "counter"},
      {"drain_read_disarmed_at_entry", "counter"},
      {"drain_rejected_new_connections", "counter"},
      {"epoll_registrations", "counter"},
      {"epoll_unregistrations", "counter"},
      {"epoll_modifications", "counter"},
      {"read_disarmed_connections", "gauge"},
      // ADR-24 第五节例外 1：`fd_end` / `rss_end_kib` 不在运行期发，另发这两格。
      {"fd_start", "gauge"},
      {"fd_open_now", "gauge"},
      {"rss_start_kib", "gauge"},
      {"rss_kib_now", "gauge"},
      {"health_ejected_now", "gauge"},
      {"health_ejections_blocked_by_valve", "counter"},
      {"circuit_half_open_now", "gauge"},
      {"health_all_denied_fallback", "counter"},
      // 点号地址按文档原意换成下划线（第五节例外 3）。
      {"health_t0_127_0_0_1_18081_ejections", "counter"},
      {"health_t0_127_0_0_1_18081_probe_ok", "counter"},
      {"health_t0_127_0_0_1_18081_probe_fail", "counter"},
      {"health_t0_127_0_0_1_18081_recoveries", "counter"},
      {"health_t0_127_0_0_1_18081_last_ejection_epoch_ms", "gauge"},
      {"health_t0_127_0_0_1_18081_trips_rate", "counter"},
      {"health_t0_127_0_0_1_18081_half_open_admitted", "counter"},
      {"health_t0_127_0_0_1_18081_half_open_denied", "counter"},
      {"health_t0_127_0_0_1_18081_half_open_recovered", "counter"},
      {"health_t0_127_0_0_1_18081_half_open_reopened", "counter"},
      {"nodes_reloads_ok", "counter"},
      {"nodes_reloads_failed", "counter"},
      {"nodes_last_publish_epoch_ms", "gauge"},
      {"rate_limit_allowed", "counter"},
      {"rate_limit_rejected", "counter"},
      {"rate_limit_overflow_keys", "counter"},
      {"rate_limit_keys", "gauge"},
      {"redis_settles_ok", "counter"},
      {"redis_settles_failed", "counter"},
      {"redis_degraded", "gauge"},
      {"redis_settle_commands", "counter"},
      {"rate_limit_capped_rejects", "counter"},
      {"workers", "gauge"},
      {"metrics_scrapes_ok", "counter"},
      {"metrics_scrapes_failed", "counter"},
      {"metrics_render_bytes", "counter"},
  };
  return kList;
}

// 把 exposition 拆成 (名字, TYPE) 与样本行，顺手判格式法则。
struct Parsed {
  std::vector<std::pair<std::string, std::string>> families;  // 出现顺序
  std::set<std::string> names;
  std::vector<std::string> sample_lines;
  std::string error;  // 非空 = 这段文本按 0.0.4 就不该发出去
};

Parsed parse(const std::string& text) {
  Parsed out;
  const std::regex kMetricName("[a-zA-Z_:][a-zA-Z0-9_:]*");
  std::size_t pos = 0;
  std::string pending_type;
  std::string pending_name;
  while (pos < text.size()) {
    const std::size_t eol = text.find('\n', pos);
    const std::string line = text.substr(pos, eol == std::string::npos ? std::string::npos
                                                                       : eol - pos);
    pos = (eol == std::string::npos) ? text.size() : eol + 1;
    if (line.empty()) continue;
    if (line.rfind("# HELP ", 0) == 0) {
      continue;  // HELP 不参与判定，上面有专门的用例看它的内容
    }
    if (line.rfind("# TYPE ", 0) == 0) {
      // 按词切，不用偏移量找空格——"# TYPE" 里第 7 位本身就是那个空格，拿
      // `find(' ', 7)` 会返回 7，于是名字切成空串。这类"看起来对了"的下标算错
      // 是格式类断言最常见的假绿来源，所以这里改用流抽取。
      std::istringstream words(line);
      std::string hash, kind, name, type;
      std::getline(words, hash, ' ');  // "#"
      words >> kind >> name >> type;
      if (kind != "TYPE" || name.empty() || type.empty()) {
        out.error = "# TYPE 行形状不对：" + line;
        return out;
      }
      pending_name = name;
      pending_type = type;
      if (!std::regex_match(pending_name, kMetricName)) {
        out.error = "非法 metric 名：" + pending_name;
        return out;
      }
      if (pending_type != "counter" && pending_type != "gauge") {
        out.error = "非法 TYPE：" + pending_type;
        return out;
      }
      if (out.names.count(pending_name) != 0) {
        out.error = "同一个族出现两次 TYPE：" + pending_name;
        return out;
      }
      out.families.emplace_back(pending_name, pending_type);
      continue;
    }
    if (line[0] == '#') {
      out.error = "不认识的注释行：" + line;
      return out;
    }
    const std::size_t sp = line.find(' ');
    if (sp == std::string::npos) {
      out.error = "样本行没有空格：" + line;
      return out;
    }
    const std::string name = line.substr(0, sp);
    const std::string value = line.substr(sp + 1);
    if (pending_name != name) {
      out.error = "样本行 " + name + " 前面不是它自己的 # TYPE（是 " + pending_name + "）";
      return out;
    }
    if (!std::regex_match(value, std::regex("-?[0-9]+"))) {
      out.error = "样本值不是整数：" + line;
      return out;
    }
    if (out.names.count(name) != 0) {
      out.error = "同一个键被发了两遍：" + name;
      return out;
    }
    out.names.insert(name);
    out.sample_lines.push_back(line);
    pending_name.clear();
  }
  return out;
}

TEST(S5_01_MetricsCatalog, FullAssemblyEmitsEveryKeyOnceWithItsOwnTypeLine) {
  FullAssembly fixture;
  Sources sources = fixture.sources();
  const std::string text = render(sources, ProcSample{42, 1'000}, 1'000'640);
  const Parsed parsed = parse(text);
  ASSERT_TRUE(parsed.error.empty()) << parsed.error;

  const std::vector<Expected>& expected = expected_full_assembly();
  ASSERT_EQ(parsed.families.size(), expected.size())
      << "族的数量对不上：清单是手写的，渲染器多一个或少一个键都会在这里红（验收 ①）";
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(parsed.families[i].first, expected[i].name) << "第 " << i << " 格";
    EXPECT_EQ(parsed.families[i].second, expected[i].type) << expected[i].name << " 的 TYPE";
    EXPECT_NE(parsed.names.count(expected[i].name), 0u) << "缺键 " << expected[i].name;
  }
  // ADR-24 第五节那三条例外必须**不在**文本里，否则等于把退出账的口径搬到运行期撒谎。
  for (const char* forbidden : {"fd_end", "rss_end_kib", "event_batches"}) {
    EXPECT_EQ(parsed.names.count(forbidden), 0u) << forbidden << " 是退出账专属键，运行期不该出现";
  }
  // `elapsed_ms` 的原点必须是 `started_at_ms`，不是渲染时刻自己造的。
  EXPECT_NE(text.find("elapsed_ms 640\n"), std::string::npos)
      << "elapsed_ms 量的不是同一段时长（原点没共用）";
}

TEST(S5_01_MetricsCatalog, UnassembledSectionsStayAbsentExactlyLikeTheExitDump) {
  ReactorStats stats;
  std::atomic<std::int64_t> origin{10};
  Sources sources;
  sources.stats = &stats;
  sources.started_at_ms = &origin;
  const std::string text = render(sources, ProcSample{7, 8}, 110);
  const Parsed parsed = parse(text);
  ASSERT_TRUE(parsed.error.empty()) << parsed.error;
  for (const char* absent : {"health_ejected_now", "circuit_half_open_now", "nodes_reloads_ok",
                             "rate_limit_allowed", "redis_settles_ok", "rate_limit_capped_rejects",
                             "workers"}) {
    EXPECT_EQ(parsed.names.count(absent), 0u) << absent << " 没装配就不该出现（与退出账同一条 if）";
  }
  // 但那张原子表的 50 格必须一格不少——它不依赖任何装配开关。
  EXPECT_NE(parsed.names.count("requests_parsed"), 0u);
  EXPECT_NE(parsed.names.count("responses_completed"), 0u);
  EXPECT_NE(parsed.names.count("drain_rejected_new_connections"), 0u);
}

TEST(S5_01_MetricsCatalog, DottedAndColonTargetNamesBecomeUnderscores) {
  EXPECT_EQ(sanitize_metric_name("127.0.0.1_18081"), "127_0_0_1_18081");
  EXPECT_EQ(sanitize_metric_name("10.0.0.5:8080"), "10_0_0_5_8080");
  // 合法名字不该被无谓地改写：退出账那 66 个固定键全部原样通过。
  EXPECT_EQ(sanitize_metric_name("epoll_modifications"), "epoll_modifications");

  // 全文本再扫一遍：任何一个 metric 名里都不许残留点号（0.0.4 的名字法则）。
  FullAssembly fixture;
  const std::string text = render(fixture.sources(), ProcSample{1, 2}, 3'000'000);
  std::size_t pos = 0;
  int samples = 0;
  while (pos < text.size()) {
    const std::size_t eol = text.find('\n', pos);
    const std::string line = text.substr(pos, eol == std::string::npos ? 0 : eol - pos);
    pos = (eol == std::string::npos) ? text.size() : eol + 1;
    if (line.empty() || line[0] == '#') continue;
    ++samples;
    const std::string name = line.substr(0, line.find(' '));
    EXPECT_EQ(name.find('.'), std::string::npos) << "样本行名字里还有点号：" << name;
  }
  EXPECT_GT(samples, 70) << "一条样本都没扫到，说明这段文本根本没渲染出来";
}

TEST(S5_01_MetricsCatalog, TripsRateIsTypedAsTheCountItReallyIs) {
  // `trips_rate` 这个名字读起来像比率，底层是 `rate_trips_total`＝**被错误率窗口摘掉的
  // 次数**（`health.h:119`）。键名不许改，所以这一格必须靠 TYPE 与 HELP 说真话。
  FullAssembly fixture;
  const std::string text = render(fixture.sources(), ProcSample{1, 2}, 3'000'000);
  const std::string name = "health_t0_127_0_0_1_18081_trips_rate";
  EXPECT_NE(text.find("# TYPE " + name + " counter\n"), std::string::npos)
      << "TYPE 报成了 gauge 或别的——那一格在 Grafana 上会把计数当瞬时值画";
  const std::size_t help_at = text.find("# HELP " + name + " ");
  ASSERT_NE(help_at, std::string::npos) << "这一格没有 HELP，名字里的 rate 会骗读的人";
  const std::size_t help_eol = text.find('\n', help_at);
  const std::string help = text.substr(help_at, help_eol - help_at);
  EXPECT_NE(help.find("次数"), std::string::npos) << "HELP 里没写明这是次数不是比率：" << help;
}

TEST(S5_01_MetricsCatalog, EjectedNowHelpCarriesTheBoundaryQualifier) {
  // ROADMAP 债表第一行：`health_ejected_now=0` 不能读成"没有坏后端"。
  // 这句必须在读数旁边出现，S5-03 的面板与 README 才有出处可指，而不是各写各的。
  FullAssembly fixture;
  const std::string text = render(fixture.sources(), ProcSample{1, 2}, 3'000'000);
  const std::size_t at = text.find("# HELP health_ejected_now ");
  ASSERT_NE(at, std::string::npos) << "这一格没有 HELP，面板就只能自己编一句";
  const std::size_t eol = text.find('\n', at);
  const std::string help = text.substr(at, eol - at);
  EXPECT_NE(help.find("0 不等于没有坏后端"), std::string::npos) << help;
  EXPECT_NE(help.find("ADR-12"), std::string::npos) << help;
}

TEST(S5_01_LoopObservation, SumMatchesTheExitAggregationShapeAndIgnoresNothing) {
  // 关闭态：没有槽，求和是全零而不是崩。
  EXPECT_EQ(l7gate::net::sum_loop_observations(nullptr, 0).epoll_modifications, 0u);

  l7gate::net::LoopObservation slots[3];
  slots[0].epoll_registrations.store(10, std::memory_order_relaxed);
  slots[1].epoll_registrations.store(5, std::memory_order_relaxed);
  slots[2].epoll_modifications.store(7, std::memory_order_relaxed);
  slots[0].read_disarmed_connections.store(2, std::memory_order_relaxed);
  const auto sum = l7gate::net::sum_loop_observations(slots, 3);
  EXPECT_EQ(sum.epoll_registrations, 15u);
  EXPECT_EQ(sum.epoll_unregistrations, 0u);
  EXPECT_EQ(sum.epoll_modifications, 7u);
  EXPECT_EQ(sum.read_disarmed_connections, 2u);
  // 一根 cache line 一个槽：三条 worker 不能挤在同一根线上，否则采集线程每次渲染都要
  // 把热循环的 line 挨个拉过来（那是仪器在被测方身上踩成本）。
  EXPECT_EQ(sizeof(l7gate::net::LoopObservation) % 64, 0u);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(&slots[1]) % 64, 0u) << "槽没对齐到 64 字节";
  EXPECT_NE(&slots[0], &slots[1]);
}

TEST(S5_01_RateLimitAccounts, LockFreeAggregateEqualsTheLockedWalk) {
  // 两条读法必须同源（ADR-24 第三节）。漂移一旦发生，`/metrics` 与退出账就各说一套。
  l7gate::filter::RateLimiter limiter(100, 100);
  EXPECT_EQ(limiter.capped_rejects_now(), limiter.capped_rejects_total());

  // 制造"夹子咬到"：本地桶还有 token，但 cap 压到 0。
  ASSERT_TRUE(limiter.try_acquire("/a", 1'000));
  auto views = limiter.snapshot_buckets();
  ASSERT_GE(views.size(), 1u);
  for (auto& view : views) {
    if (view.bucket != nullptr) view.bucket->set_cap_milli(0);
  }
  EXPECT_FALSE(limiter.try_acquire("/a", 2'000));
  EXPECT_EQ(limiter.capped_rejects_now(), 1u);
  EXPECT_EQ(limiter.capped_rejects_now(), limiter.capped_rejects_total())
      << "免锁聚合与全表走查读出了两个数——两条路必须同源";
  // 快路径不被这件事拖累：一次**放行**既不碰桶级也不碰聚合级。
  limiter.release_all_caps();
  ASSERT_TRUE(limiter.try_acquire("/a", 3'000));
  EXPECT_EQ(limiter.capped_rejects_now(), 1u) << "放行的一格不该给 capped_rejects 加账";
}

}  // namespace
