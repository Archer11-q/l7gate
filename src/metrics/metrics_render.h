#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "net/loop_observation.h"
#include "net/stats.h"
#include "route/health.h"

namespace l7gate::metrics {

// S5-01（ADR-24）：退出账那套计数的**运行期**渲染。
//
// 这一层是纯函数：给它一份 `Sources` 与一次进程采样，它返回一段 text format 0.0.4 的
// 文本，不碰 socket、不碰线程。理由不是可测性（那也确实是可测性的来源——验收 ① 的
// 键名清单就在这一层断言），而是**格式正确性没有第三方兜底**（ADR-24 第一节记的那笔
// 代价），所以它必须是被单独钉住的那一块。
//
// 键集合的规矩只有一条：**与同一装配下的退出账逐键相同**（`main.cc:report_stats`）。
// 三段显式例外写在 ADR-24 第五节，这里只点名：
//   1. `fd_end` / `rss_end_kib` 不发——那是"析构之前"那一次采样，运行期发同名键
//      等于对名字撒谎；运行期发 `fd_open_now` / `rss_kib_now`（同一对函数、不同时点）。
//   2. `event_batches` 不发——它是 `run()` 的返回值（accept 循环那一侧的批次数），
//      口径是"这一次 run 总共跑了多少批"，不是可以被跨线程求和的量。
//   3. 每目标那一族的键名含点号（`health_t0_127.0.0.1_18081_ejections`），在 Prometheus
//      的名字法则 `[a-zA-Z_:][a-zA-Z0-9_:]*` 里非法；这里按 `main.cc:355` 注释与
//      `main.cc:759` 示例本来就写着的下划线形态发（`.` 与 `:` → `_`），不改用 label，
//      因为 label 形态是把 10×N 个键并成 10 个族，那是对"键名不许改"的结构性重写。

// 限流那一段（`report_rate_limit` + `report_redis` 里的 `rate_limit_capped_rejects`）。
// 由采集侧一次读齐再传进来：`present=false` 就是没装限流 filter，那几键不出现。
struct RateLimitAccounts {
  bool present = false;
  std::uint64_t allowed = 0;
  std::uint64_t rejected = 0;
  std::uint64_t overflow_keys = 0;
  // 键数。这是 O(1) 的 `buckets_.size()`，但**要拿桶表的锁**，而每条请求都拿同一把锁
  // （`rate_limit.cc:71`）——所以只在采集线程拿、且只做一次浅读。
  std::size_t keys = 0;
  // 夹子咬到的拒绝数：走 limiter 级聚合的**免锁**读法（ADR-24 第三节），
  // 不在 scrape 路径上做全表走查。
  std::uint64_t capped_rejects = 0;
};

struct RedisAccounts {
  bool present = false;
  std::uint64_t settles_ok = 0;
  std::uint64_t settles_failed = 0;
  std::uint64_t settle_commands = 0;
  bool degraded = false;
};

struct NodeAccounts {
  bool present = false;
  std::uint64_t reloads_ok = 0;
  std::uint64_t reloads_failed = 0;
  std::int64_t last_publish_epoch_ms = 0;
};

// 一次进程内采样（`/proc` 读的那两格）。放在这里是为了让渲染保持纯函数：
// 谁来读 /proc 是采集线程的事，不是格式的事。
struct ProcSample {
  std::int64_t fd_open_now = -1;
  std::int64_t rss_kib_now = -1;
};

struct Sources {
  // 业务计数：全 worker 共享的那张原子表。**对它"汇聚"这个说法不成立**（ADR-24 第四节）。
  l7gate::net::ReactorStats* stats = nullptr;
  // 每 worker 各一份的非原子量抄进的那排原子槽（`--metrics-port` 关着的时候
  // `loop_count == 0`，求和返回全零）。
  const l7gate::net::LoopObservation* loop = nullptr;
  std::size_t loop_count = 0;
  const l7gate::route::HealthTable* health = nullptr;
  // 与退出账同一个来源（`main.cc:763` 那个 `address_port` 串），渲染时做名字变换。
  const std::vector<std::string>* target_names = nullptr;
  RateLimitAccounts rate_limit;
  RedisAccounts redis;
  NodeAccounts nodes;
  // 业务 worker 条数。**指标暴露面自己那条线程不进这一格**——它不是 worker
  // （ADR-24 第六节：`workers` 的口径不变，否则 B-008/B-018 那些按 worker 数归因的
  // 历史读数会在这里读成另一个数）。
  int workers = 0;
  // 退出账发 `workers` 那一格只在多 Reactor 臂；单 Reactor 臂今天不发这一键。
  // 跟着装配走，不自己决定。
  bool expose_workers_key = false;
  std::int64_t fd_start = -1;
  std::int64_t rss_start_kib = -1;
  // `elapsed_ms` 的时钟原点：与退出账用**同一个** `started`（`main.cc:942` 那个）。
  // 原点不共享的话，两个同名键量的就不是同一段时长了。
  const std::atomic<std::int64_t>* started_at_ms = nullptr;
  // 仪器自己的三格（只有开启时才存在）。
  std::uint64_t scrapes_ok = 0;
  std::uint64_t scrapes_failed = 0;
  std::uint64_t render_bytes = 0;
};

// metric 名字法则里不合法的字符换成下划线（见文件头例外 3）。纯函数，单独可测。
std::string sanitize_metric_name(std::string_view name);

// 渲染一次完整的 exposition。`now_ms` 是采集时刻的单调毫秒（只用于 `elapsed_ms`）。
std::string render(const Sources& sources, const ProcSample& proc, std::int64_t now_ms);

}  // namespace l7gate::metrics
