#include "route/health.h"

#include <algorithm>
#include <exception>

namespace l7gate::route {

HealthTable::HealthTable(std::size_t target_count, HealthPolicy policy,
                         std::size_t capacity) noexcept
    : policy_(policy),
      entries_(capacity == 0 ? target_count : std::max(target_count, capacity)),
      active_(target_count) {
  // ever-seen 上界给小于初始启用数是装配错误：宁可这里崩（构造期）也别让 reload
  // 在运行期把启用数抬进一个不存在的槽位。
  if (capacity != 0 && capacity < target_count) {
    std::terminate();
  }
}

FailureVerdict HealthTable::note_failure(std::uint16_t target, std::int64_t now_ms,
                                         std::int64_t epoch_ms) noexcept {
  if (target >= active_.load(std::memory_order_relaxed)) return FailureVerdict::kRecorded;  // 越界只可能是装配漏项；不猜、不扩表
  Entry& e = entries_[target];

  const std::int64_t until = e.ejected_until_ms.load(std::memory_order_relaxed);
  if (until > now_ms) return FailureVerdict::kAlreadyEjected;  // 摘除期内不重复计摘
  if (until != 0) {
    // 到期未扫（sweep 还没轮到）：这里顺手恢复掉，让下面按健康路径走。
    // CAS 赢家做状态账；输家说明 sweep 或别的 worker 已经做了，等价。
    if (try_mark_healthy(target, until)) e.consecutive_failures.store(0, std::memory_order_relaxed);
  }

  e.failures_total.fetch_add(1, std::memory_order_relaxed);
  const std::uint32_t strikes =
      e.consecutive_failures.fetch_add(1, std::memory_order_relaxed) + 1;
  if (strikes < policy_.max_fails) return FailureVerdict::kRecorded;
  // 到阈值：请求摘除。安全阀满 → 计数并放行（故意保留一段错误，ADR-12）；
  // CAS 输家说明另一个 worker 同时摘了/恢复了——那次摘除已经有人记账，这里不重复。
  if (ejected_now_.load(std::memory_order_relaxed) >=
      static_cast<std::uint32_t>(max_ejected_count())) {
    blocked_by_valve_.fetch_add(1, std::memory_order_relaxed);
    return FailureVerdict::kValveBlocked;
  }
  return try_mark_ejected(target, now_ms, epoch_ms) ? FailureVerdict::kEjected
                                                    : FailureVerdict::kRecorded;
}

void HealthTable::note_success(std::uint16_t target) noexcept {
  if (target >= active_.load(std::memory_order_relaxed)) return;
  entries_[target].consecutive_failures.store(0, std::memory_order_relaxed);
}

void HealthTable::note_probe_result(std::uint16_t target, bool alive, std::int64_t now_ms,
                                    std::int64_t epoch_ms) noexcept {
  if (target >= active_.load(std::memory_order_relaxed)) return;
  Entry& e = entries_[target];
  if (alive) {
    e.probe_ok_total.fetch_add(1, std::memory_order_relaxed);
    note_success(target);
    revive(target);
    return;
  }
  e.probe_fail_total.fetch_add(1, std::memory_order_relaxed);
  note_failure(target, now_ms, epoch_ms);
}

bool HealthTable::revive(std::uint16_t target) noexcept {
  if (target >= active_.load(std::memory_order_relaxed)) return false;
  Entry& e = entries_[target];
  const std::int64_t until = e.ejected_until_ms.load(std::memory_order_relaxed);
  if (until == 0) return false;  // 没被摘过：探活成功没有账要还
  if (!try_mark_healthy(target, until)) return false;
  e.consecutive_failures.store(0, std::memory_order_relaxed);
  return true;
}

std::size_t HealthTable::sweep_expired(std::int64_t now_ms) noexcept {
  std::size_t revived = 0;
  for (std::size_t i = 0; i < active_.load(std::memory_order_relaxed); ++i) {
    Entry& e = entries_[i];
    const std::int64_t until = e.ejected_until_ms.load(std::memory_order_relaxed);
    if (until == 0 || until > now_ms) continue;
    if (try_mark_healthy(static_cast<std::uint16_t>(i), until)) {
      e.consecutive_failures.store(0, std::memory_order_relaxed);
      ++revived;
    }
  }
  return revived;
}

bool HealthTable::is_ejected(std::uint16_t target, std::int64_t now_ms) const noexcept {
  if (target >= active_.load(std::memory_order_relaxed)) return false;
  return entries_[target].ejected_until_ms.load(std::memory_order_relaxed) > now_ms;
}

std::size_t HealthTable::max_ejected_count() const noexcept {
  // floor 语义："被摘除容量 ≤ 比例%" 的字面读法。N<比例的小组按它就是"一个都不能摘"，
  // 想要摘除能力的配置该把比例调大，而不是我在这里偷偷放宽成 ceil。
  // 分母 = **当前启用的成员数**（ADR-12 追加节：去重目标表），不是 ever-seen 的
  // 槽位容量——被摘除比例量的是"现在的容量"，不是"历史上见过的总量"。
  const std::uint64_t count = active_.load(std::memory_order_relaxed);
  return static_cast<std::size_t>(count * policy_.max_ejection_percent / 100u);
}

bool HealthTable::try_mark_ejected(std::uint16_t target, std::int64_t now_ms,
                                   std::int64_t epoch_ms) noexcept {
  Entry& e = entries_[target];
  std::int64_t expected = 0;
  // 摘除只能从"健康（0）"出发。循环是为了处理 CAS 与 revive/expiry 撞车：
  // 输家重读一次，仍然健康就再试一次，别人已摘/仍摘着就放弃（账在赢家那里）。
  for (int attempt = 0; attempt < 4; ++attempt) {
    expected = 0;
    if (e.ejected_until_ms.compare_exchange_weak(expected, now_ms + policy_.fail_timeout_ms,
                                                 std::memory_order_relaxed,
                                                 std::memory_order_relaxed)) {
      e.ejections_total.fetch_add(1, std::memory_order_relaxed);
      e.last_ejection_epoch_ms.store(epoch_ms, std::memory_order_relaxed);
      ejected_now_.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    if (expected != 0) return false;  // 已被别人摘着：不是我的账
  }
  return false;
}

bool HealthTable::try_mark_healthy(std::uint16_t target, std::int64_t expected_until) noexcept {
  Entry& e = entries_[target];
  std::int64_t expected = expected_until;
  if (!e.ejected_until_ms.compare_exchange_strong(expected, 0, std::memory_order_relaxed,
                                                  std::memory_order_relaxed)) {
    return false;  // 别人已经恢复过，或又摘了一轮：状态账都不归我
  }
  e.recoveries_total.fetch_add(1, std::memory_order_relaxed);
  ejected_now_.fetch_sub(1, std::memory_order_relaxed);
  return true;
}

void HealthTable::ensure_active(std::size_t n) noexcept {
  // fetch_max：多个 worker 同时看到同一份新快照时，抬到同一个数，赢家无所谓。
  std::size_t cur = active_.load(std::memory_order_relaxed);
  while (cur < n && n <= entries_.size()) {
    if (active_.compare_exchange_weak(cur, n, std::memory_order_relaxed,
                                      std::memory_order_relaxed)) {
      return;
    }
  }
}

TargetHealthSnapshot HealthTable::snapshot(std::uint16_t target) const noexcept {
  TargetHealthSnapshot out;
  if (target >= active_.load(std::memory_order_relaxed)) return out;
  const Entry& e = entries_[target];
  out.failures_total = e.failures_total.load(std::memory_order_relaxed);
  out.ejections_total = e.ejections_total.load(std::memory_order_relaxed);
  out.recoveries_total = e.recoveries_total.load(std::memory_order_relaxed);
  out.probe_ok_total = e.probe_ok_total.load(std::memory_order_relaxed);
  out.probe_fail_total = e.probe_fail_total.load(std::memory_order_relaxed);
  out.ejected_until_ms = e.ejected_until_ms.load(std::memory_order_relaxed);
  out.last_ejection_epoch_ms = e.last_ejection_epoch_ms.load(std::memory_order_relaxed);
  return out;
}

}  // namespace l7gate::route
