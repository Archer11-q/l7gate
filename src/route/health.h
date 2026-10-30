#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

// 上游健康状态表（S3-03，ADR-12 落地 + ADR-19 追加第四条）。
//
// 这里只放**状态与决策**，不放探测：主动探活的连接机在 `Reactor` 里（它要 epoll 与
// 非阻塞 socket），被动失败判据的采集点也在 `Reactor`（connect 失败、5xx、截断）。
// 这份表回答的问题是 ADR-19 第三条留给 S3-03 的那一个——"这一组里**现在谁可用**"。
//
// 归属口径（为什么它放在 route/ 而不是 net/）：摘除改变的是"组里谁可被选中"，
// 它的第一个读者是 `LoadBalanceFilter` 的候选集；连接对象与 fd 一概不进这张表，
// 所以 ADR-14 的接触面不因它变大。
//
// 并发口径：**全原子**，被同组全部 worker 共享一份（先例是 `ReactorStats`——共享的
// 原子计数面）。一个后端死没死是**关于后端的事实**，不是策略；策略（游标、配对数）
// 才是 per-worker 的。多 worker 各自记各自的成功/失败、各自 CAS 摘除/恢复，
// TSan 的可证伪命题照旧成立：`tsan` preset 零 data race。
namespace l7gate::route {

// 健康检查策略。缺省值即 ADR-19 追加第四条定下的缺省，配置文件里每个字段都可覆盖。
struct HealthPolicy {
  // 连续失败多少次判摘（nginx `max_fails` 同义）。nginx 缺省 1，这里取 3：
  // 探活场景下单次失败就摘会把一次网络抖动读成一台后端死了——误摘率那格替我付账。
  std::uint32_t max_fails = 3;
  // 摘除时长（nginx `fail_timeout` 同义，nginx 缺省 10s）。
  std::int64_t fail_timeout_ms = 10000;
  // 主动探活间隔；**0 = 不主动探活**（只跑被动）。
  std::int64_t probe_interval_ms = 1000;
  // 单次探活的超时（连接 + 响应头共用这一个预算）。
  std::int64_t probe_timeout_ms = 500;
  // 最多摘除比例（ADR-12，缺省 10% 一字未动）。分母 = 目标表去重后的成员数，
  // 可摘除上限 = floor(成员数 × 比例 / 100)——"被摘除容量 ≤ X%" 的字面读法。
  std::uint32_t max_ejection_percent = 10;
};

// 一次失败记录的结局。可观测、可断言——ADR-12 要的"触发时有可观测计数"落在
// 这四个值加上下面的计数器上，不是落在日志里。
enum class FailureVerdict : std::uint8_t {
  kRecorded,       // 计了失败，还没到阈值
  kEjected,        // 这次失败触发了摘除
  kValveBlocked,   // 到阈值了，但安全阀满了（ADR-12：故意保留一段错误）
  kAlreadyEjected, // 已在摘除期内的又一次失败：不计摘、不重复计摘
};

// 单个目标的可观测读数。全原子，运行中可读（拿它当同步条件仍然犯规——stats.h
// 那条纪律在这里同样成立，测试里等它之前先拿内核级证据）。
struct TargetHealthSnapshot {
  std::uint64_t failures_total = 0;     // 记进来的失败总次数（连续计数是内部状态，不在这里）
  std::uint64_t ejections_total = 0;    // 被摘除总次数（**误摘率与摘除耗时的分子来源**）
  std::uint64_t recoveries_total = 0;   // 恢复总次数（到期恢复与探活成功都算）
  std::uint64_t probe_ok_total = 0;     // 探活判活次数
  std::uint64_t probe_fail_total = 0;   // 探活判死次数
  std::int64_t ejected_until_ms = 0;    // 当前摘除的到期时刻（单调毫秒；0 = 没被摘）
  std::int64_t last_ejection_epoch_ms = 0;  // 最近一次摘除的墙钟时刻（0 = 从未）；摘除耗时的"网关自报那一端"
};

class HealthTable {
 public:
  HealthTable(std::size_t target_count, HealthPolicy policy) noexcept;

  // 失败判据成立时调用（被动：业务请求撞上失败；主动：探活判死）。
  // `now_ms` 是单调毫秒（决策纯靠它，测试可以喂任意时间线）；`epoch_ms` 是墙钟毫秒，
  // 只进 `last_ejection_epoch_ms` 那格观测。
  FailureVerdict note_failure(std::uint16_t target, std::int64_t now_ms,
                              std::int64_t epoch_ms) noexcept;
  // 活着判据成立时调用（业务响应 < 500 完整头、探活判活）：连续失败计数清零。
  // **不解除摘除**——解除摘除只有两条路：fail_timeout 到期、探活成功（`revive`）。
  void note_success(std::uint16_t target) noexcept;
  // 主动探活一拍的结算（探活连接机的唯一入口）：判活 = probe_ok+1 + 计数清零 +
  // 立即恢复；判死 = probe_fail+1 + 走与被动同一条 note_failure 路径
  // （探活失败和业务失败汇入同一个连续计数——摘除的判据只认"连续失败了几次"，
  // 不认失败是从哪条路上来的，两把尺迟早量出两个答案）。
  void note_probe_result(std::uint16_t target, bool alive, std::int64_t now_ms,
                         std::int64_t epoch_ms) noexcept;
  // 立即恢复（探活成功这条路的唯一入口）。返回是否由本次调用完成恢复
  // （多 worker 同时探活成功时 CAS 只有一个赢家；输家拿到 false，状态仍是"已恢复"）。
  bool revive(std::uint16_t target) noexcept;
  // 把已到期的摘除恢复掉（每轮循环 tick 扫一遍）。返回本次恢复的个数。
  std::size_t sweep_expired(std::int64_t now_ms) noexcept;

  // "现在能不能被选中"——`LoadBalanceFilter` 每请求读的只有这一个问句。
  // 到期但还没被 sweep 的摘除在这里**按健康算**（读侧不付状态迁移的账，
  // 状态迁移只属于 note_failure/revive/sweep_expired 三个写入点）。
  bool is_ejected(std::uint16_t target, std::int64_t now_ms) const noexcept;

  // 安全阀：可摘除上限 = floor(成员数 × max_ejection_percent / 100)。
  std::size_t max_ejected_count() const noexcept;

  const HealthPolicy& policy() const noexcept { return policy_; }
  std::size_t target_count() const noexcept { return entries_.size(); }

  // --- 观测面（全原子，聚合由读者自己做）-------------------------------------
  TargetHealthSnapshot snapshot(std::uint16_t target) const noexcept;
  // 此刻被摘除的目标数（安全阀的账本余额；与 max_ejected_count() 比对着读）。
  std::uint32_t ejected_now() const noexcept { return ejected_now_.load(std::memory_order_relaxed); }
  // 到阈值但被安全阀挡住的次数（ADR-12 的"触发时有可观测计数"第二半）。
  std::uint64_t ejections_blocked_by_valve() const noexcept {
    return blocked_by_valve_.load(std::memory_order_relaxed);
  }

 private:
  // 摘除的 CAS：0 → now+fail_timeout。返回是否赢家。
  // 赢家负责 ejected_now_++ 与计数器；输家说明别人刚摘了或刚恢复了。
  bool try_mark_ejected(std::uint16_t target, std::int64_t now_ms, std::int64_t epoch_ms) noexcept;
  // 恢复的 CAS：把一个非零到期时刻打回 0。赢家负责 ejected_now_-- 与恢复计数。
  bool try_mark_healthy(std::uint16_t target, std::int64_t expected_until) noexcept;

  struct Entry {
    std::atomic<std::uint32_t> consecutive_failures{0};
    std::atomic<std::uint64_t> failures_total{0};
    std::atomic<std::uint64_t> ejections_total{0};
    std::atomic<std::uint64_t> recoveries_total{0};
    std::atomic<std::uint64_t> probe_ok_total{0};
    std::atomic<std::uint64_t> probe_fail_total{0};
    // 摘除的**唯一状态位**：0 = 健康，>0 = 摘除到期时刻（单调毫秒）。所有状态迁移
    // 都是对它的 CAS，所以"摘了没"永远不会有两个答案。
    std::atomic<std::int64_t> ejected_until_ms{0};
    std::atomic<std::int64_t> last_ejection_epoch_ms{0};
  };

  HealthPolicy policy_;
  std::vector<Entry> entries_;
  std::atomic<std::uint32_t> ejected_now_{0};
  std::atomic<std::uint64_t> blocked_by_valve_{0};
};

}  // namespace l7gate::route
