#include "route/health.h"

#include <algorithm>
#include <exception>

namespace l7gate::route {

namespace {

// 半开周期那个打包字的两个半边（低 32 = 已放行，高 32 = 已攒到的业务成功）。
constexpr std::uint64_t kProbeOkStep = 1ull << 32;
constexpr std::uint64_t kProbeUsedMask = 0xffffffffull;

}  // namespace

HealthTable::HealthTable(std::size_t target_count, HealthPolicy policy,
                         std::size_t capacity, bool policy_reloadable) noexcept
    : policy_(policy),
      policy_reloadable_(policy_reloadable),
      entries_(capacity == 0 ? target_count : std::max(target_count, capacity)),
      active_(target_count) {
  // ever-seen 上界给小于初始启用数是装配错误：宁可这里崩（构造期）也别让 reload
  // 在运行期把启用数抬进一个不存在的槽位。
  if (capacity != 0 && capacity < target_count) {
    std::terminate();
  }
}

// 一次策略读取（S5-02，ADR-10 追加节第四节末）。
PolicyView HealthTable::policy_view() const noexcept {
  if (!policy_reloadable_) return PolicyView{&policy_, nullptr};
  // arm 了：把当前世代钉在返回值里。`live_policy_` 为空只可能出现在"表建好了但首份
  // 世代还没发布"那一瞬——按构造期那份读，与未 arm 同义，不是猜。
  std::shared_ptr<const HealthPolicy> pin = live_policy_.load(std::memory_order_acquire);
  if (pin == nullptr) return PolicyView{&policy_, nullptr};
  return PolicyView{pin.get(), std::move(pin)};
}

void HealthTable::set_policy(std::shared_ptr<const HealthPolicy> pin) noexcept {
  if (!policy_reloadable_ || pin == nullptr) return;
  live_policy_.store(std::move(pin), std::memory_order_release);
}

std::uint64_t HealthTable::bucket_ms_of(const HealthPolicy& policy) noexcept {
  return policy.circuit.enabled
             ? static_cast<std::uint64_t>(
                   std::max<std::int64_t>(1, policy.circuit.window_ms /
                                                    static_cast<std::int64_t>(kWindowBuckets)))
             : 1;
}

FailureVerdict HealthTable::note_failure(std::uint16_t target, std::int64_t now_ms,
                                         std::int64_t epoch_ms, FailureSource source) noexcept {
  const bool business = (source == FailureSource::kBusiness);
  if (target >= active_.load(std::memory_order_relaxed)) return FailureVerdict::kRecorded;  // 越界只可能是装配漏项；不猜、不扩表
  // 整个判据读同一代策略（S5-02）：`pv` 必须是有名字的对象——把 `*policy_view()` 直接
  // 绑给引用会在整句结束时释放那根针，那就是一个只在换代那一拍才出现的悬垂。
  const PolicyView pv = policy_view();
  const HealthPolicy& policy = *pv;
  Entry& e = entries_[target];

  std::int64_t state = e.state_ms.load(std::memory_order_relaxed);
  if (state != 0 && deadline_of(state) <= now_ms) {
    // 到期未扫（sweep 还没轮到）：读侧顺手推进。S3-03 在这里做的是"顺手恢复"，
    // S4-05 装配熔断后做的是"顺手进半开"——形状一模一样：CAS 赢家做状态账，
    // 输家说明别的 worker 已经做了，等价。
    if (!policy.circuit.enabled) {
      if (try_mark_healthy(target, state)) e.consecutive_failures.store(0, std::memory_order_relaxed);
      state = 0;
    } else if (is_half_open(state)) {
      // 半开周期到期还没攒够证据（攒够的那一刻就转健康了，留在这儿就是没攒够）→ 回摘。
      if (try_mark_ejected(target, now_ms, epoch_ms, true, policy)) {
        e.half_open_reopened_total.fetch_add(1, std::memory_order_relaxed);
        return FailureVerdict::kEjected;
      }
      state = e.state_ms.load(std::memory_order_relaxed);
    } else {
      try_enter_half_open(target, state, now_ms, policy);
      state = e.state_ms.load(std::memory_order_relaxed);
    }
  }

  const bool in_half_open = policy.circuit.enabled && state != 0 && is_half_open(state);
  // 摘除期内（未到期）的又一次失败：不计摘、不重复计摘（S3-03 原样）。
  // **半开不算摘除期内**——那是门主动放进去的探测，它的失败是一条证据（下面走回摘）。
  if (state != 0 && !in_half_open && deadline_of(state) > now_ms) {
    return FailureVerdict::kAlreadyEjected;
  }

  e.failures_total.fetch_add(1, std::memory_order_relaxed);
  const std::uint32_t strikes =
      e.consecutive_failures.fetch_add(1, std::memory_order_relaxed) + 1;

  if (in_half_open) {
    // 半开期内一次业务失败 → 立即回摘，不等 `max_fails`（ADR-12 追加二第二节第 3 条）：
    // "试一条就死"不需要再攒三次失败才承认它死。错误率窗口在这条路上不判（半开期内
    // 不判错误率，第一节），但样本照旧进窗口——回摘那一刻窗口会被清掉，所以这里记的
    // 不会变成下一轮的旧账。
    // 探活来源不进窗口（仪器不进被测方），但半开期内**探活判死仍然立即回摘**——
    // 这一侧的不对称是 fail-closed 方向，见 health.h 里 `note_probe_result` 那段。
    if (business) window_record(target, now_ms, true, nullptr, nullptr, policy);
    if (try_mark_ejected(target, now_ms, epoch_ms, true, policy)) {
      e.half_open_reopened_total.fetch_add(1, std::memory_order_relaxed);
      return FailureVerdict::kEjected;
    }
    return FailureVerdict::kRecorded;  // CAS 输家：别人已经把它摘回去了
  }

  std::uint64_t window_requests = 0;
  std::uint64_t window_failures = 0;
  if (policy.circuit.enabled && business) {
    window_record(target, now_ms, true, &window_requests, &window_failures, policy);
  }

  // 判据一（S3-03）：连续失败到阈值。判据二（S4-05）：窗口内错误率。两条判据共用
  // 同一条摘除路径与同一只阀（已裁第一条）——不另起第二种"死"。
  if (strikes >= policy.max_fails) {
    if (ejected_now_.load(std::memory_order_relaxed) >=
        static_cast<std::uint32_t>(max_ejected_count_for(policy))) {
      blocked_by_valve_.fetch_add(1, std::memory_order_relaxed);
      return FailureVerdict::kValveBlocked;
    }
    if (!try_mark_ejected(target, now_ms, epoch_ms, false, policy)) return FailureVerdict::kRecorded;
    return FailureVerdict::kEjected;
  }

  const CircuitPolicy& circuit = policy.circuit;
  if (circuit.enabled && window_requests >= circuit.minimum_requests &&
      window_failures * 100ull >= window_requests * static_cast<std::uint64_t>(circuit.error_rate_percent)) {
    if (ejected_now_.load(std::memory_order_relaxed) >=
        static_cast<std::uint32_t>(max_ejected_count_for(policy))) {
      blocked_by_valve_.fetch_add(1, std::memory_order_relaxed);
      return FailureVerdict::kValveBlocked;
    }
    if (!try_mark_ejected(target, now_ms, epoch_ms, false, policy)) return FailureVerdict::kRecorded;
    e.rate_trips_total.fetch_add(1, std::memory_order_relaxed);
    return FailureVerdict::kEjected;
  }
  return FailureVerdict::kRecorded;
}

void HealthTable::note_success(std::uint16_t target, std::int64_t now_ms) noexcept {
  const PolicyView pv = policy_view();
  const HealthPolicy& policy = *pv;
  if (target >= active_.load(std::memory_order_relaxed)) return;
  Entry& e = entries_[target];
  e.consecutive_failures.store(0, std::memory_order_relaxed);
  if (!policy.circuit.enabled) return;  // 段不存在 = S3-03 的行为，一条指令都不多走
  window_record(target, now_ms, false, nullptr, nullptr, policy);
  const std::int64_t state = e.state_ms.load(std::memory_order_relaxed);
  // 半开期内的一条业务成功 = 一份证据（ADR-12 追加二第二节第 3 条）。摘除期内的成功
  // 不解除摘除——那还是 S3-03 立的口径，本轮没动。
  if (state != 0 && is_half_open(state) && deadline_of(state) > now_ms) {
    note_half_open_success(target, policy);
  }
}

void HealthTable::note_probe_result(std::uint16_t target, bool alive, std::int64_t now_ms,
                                    std::int64_t epoch_ms) noexcept {
  if (target >= active_.load(std::memory_order_relaxed)) return;
  Entry& e = entries_[target];
  if (alive) {
    e.probe_ok_total.fetch_add(1, std::memory_order_relaxed);
    // 与 note_success 的区别（ADR-12 追加二第一节）：探活是仪器不是流量。它清连续计数、
    // 它解除摘除（解除到半开还是到全量由 `circuit` 装配决定），但它**不进错误率窗口**，
    // 也不充当半开的成功证据——出半开只认业务请求的成功，否则"只肯答 GET /health、
    // 业务全回 500"的后端会被自己的探活推回全量流量。
    e.consecutive_failures.store(0, std::memory_order_relaxed);
    revive(target, now_ms);
    return;
  }
  e.probe_fail_total.fetch_add(1, std::memory_order_relaxed);
  note_failure(target, now_ms, epoch_ms, FailureSource::kProbe);
}

bool HealthTable::revive(std::uint16_t target, std::int64_t now_ms) noexcept {
  const PolicyView pv = policy_view();
  const HealthPolicy& policy = *pv;
  if (target >= active_.load(std::memory_order_relaxed)) return false;
  Entry& e = entries_[target];
  const std::int64_t state = e.state_ms.load(std::memory_order_relaxed);
  if (state == 0) return false;  // 没被摘过：探活成功没有账要还
  if (is_half_open(state)) return false;  // 已经在半开了：探活不重复推进，也不给证据
                                          // （出半开只认业务请求的成功证据，第一节）
  if (policy.circuit.enabled) {
    // 未到期也解除到"半开"——这就是 ADR-12 追加二第七节写明的第二条恢复出口：
    // 探活把成员从摘除态叫醒到半开，而不是直接放回全量流量。
    return try_enter_half_open(target, state, now_ms, policy);
  }
  if (!try_mark_healthy(target, state)) return false;
  e.consecutive_failures.store(0, std::memory_order_relaxed);
  return true;
}

std::size_t HealthTable::sweep_expired(std::int64_t now_ms, std::int64_t epoch_ms) noexcept {
  const PolicyView pv = policy_view();
  const HealthPolicy& policy = *pv;
  std::size_t moved = 0;
  for (std::size_t i = 0; i < active_.load(std::memory_order_relaxed); ++i) {
    const auto target = static_cast<std::uint16_t>(i);
    Entry& e = entries_[i];
    const std::int64_t state = e.state_ms.load(std::memory_order_relaxed);
    if (state == 0 || deadline_of(state) > now_ms) continue;
    if (!policy.circuit.enabled) {
      // S3-03 的原样：到期即全量恢复。
      if (try_mark_healthy(target, state)) {
        e.consecutive_failures.store(0, std::memory_order_relaxed);
        ++moved;
      }
      continue;
    }
    if (is_half_open(state)) {
      // 半开到期：证据已经够了却还没转健康，说明并发了慢一步——补上那一步；
      // 不够就回摘。两条都是"半开不会悬在那儿"的兜底（第二节第 4 条）。
      const std::uint64_t cycle = e.half_open_cycle.load(std::memory_order_relaxed);
      if ((cycle >> 32) >= policy.circuit.half_open_successes) {
        if (try_mark_healthy(target, state)) {
          e.half_open_recovered_total.fetch_add(1, std::memory_order_relaxed);
          e.half_open_cycle.store(0, std::memory_order_relaxed);
          ++moved;
        }
      } else if (try_mark_ejected(target, now_ms, epoch_ms, true, policy)) {
        e.half_open_reopened_total.fetch_add(1, std::memory_order_relaxed);
        ++moved;
      }
      continue;
    }
    if (try_enter_half_open(target, state, now_ms, policy)) ++moved;
  }
  return moved;
}

Admission HealthTable::admit(std::uint16_t target, std::int64_t now_ms) noexcept {
  const PolicyView pv = policy_view();
  const HealthPolicy& policy = *pv;
  if (target >= active_.load(std::memory_order_relaxed)) return Admission::kAllow;
  Entry& e = entries_[target];
  std::int64_t state = e.state_ms.load(std::memory_order_relaxed);
  if (state == 0) return Admission::kAllow;
  if (deadline_of(state) > now_ms) {
    if (!is_half_open(state)) return Admission::kDeny;  // 还在摘除冷却里
    return try_take_probe_slot(target, policy) ? Admission::kProbeSlot : Admission::kDeny;
  }
  if (!policy.circuit.enabled) {
    // 到期未扫按健康算，且**读侧不顺手改状态**——S3-03 的 `is_ejected` 就是这个口径，
    // 状态推进留给循环里的 sweep。熔断未装配时一个字节都不改。
    return Admission::kAllow;
  }
  if (is_half_open(state)) return Admission::kDeny;  // 半开到期：结论交给 sweep，这一条不放
  if (try_enter_half_open(target, state, now_ms, policy)) {
    return try_take_probe_slot(target, policy) ? Admission::kProbeSlot : Admission::kDeny;
  }
  // CAS 输家：别人刚推进过（可能是半开、也可能是回摘/恢复），按它留下的状态读一次。
  state = e.state_ms.load(std::memory_order_relaxed);
  if (state == 0) return Admission::kAllow;
  if (is_half_open(state) && deadline_of(state) > now_ms) {
    return try_take_probe_slot(target, policy) ? Admission::kProbeSlot : Admission::kDeny;
  }
  return Admission::kDeny;
}

bool HealthTable::is_ejected(std::uint16_t target, std::int64_t now_ms) const noexcept {
  if (target >= active_.load(std::memory_order_relaxed)) return false;
  const std::int64_t state = entries_[target].state_ms.load(std::memory_order_relaxed);
  // 半开不是"摘除中"：S4-02 用它分 502 与 503，而半开成员失败是一条普通的上游失败。
  return state != 0 && !is_half_open(state) && deadline_of(state) > now_ms;
}

bool HealthTable::retry_eligible(std::uint16_t target, std::int64_t now_ms) const noexcept {
  const PolicyView pv = policy_view();
  const HealthPolicy& policy = *pv;
  // 与 `admit` 同一条"没登记过的槽位按健康读"的前置（越界不猜状态，也保证 S3-04
  // ever-seen 扩容期间读到新成员时不会因为容量与启用数的差而拒掉一次重试）。
  if (target >= active_.load(std::memory_order_relaxed)) return true;
  const std::int64_t state = entries_[target].state_ms.load(std::memory_order_relaxed);
  if (state == 0) return true;                                    // 健康
  if (is_half_open(state)) return false;                          // 名额归 admit 管，重试不抢
  if (deadline_of(state) > now_ms) return false;                  // 摘除冷却中：绕不过
  // 到期未扫：未装配熔断时这就是健康（S3-03 的读侧口径，`admit` 第 215 行同一句）；
  // 装配了熔断时它已经归半开门管，推进状态是 `admit`/`sweep_expired` 的活，不是判卷的活。
  return !policy.circuit.enabled;
}

std::size_t HealthTable::max_ejected_count() const noexcept {
  return max_ejected_count_for(*policy_view());
}

std::size_t HealthTable::max_ejected_count_for(const HealthPolicy& policy) const noexcept {
  // floor 语义："被摘除容量 ≤ 比例%" 的字面读法。N<比例的小组按它就是"一个都不能摘"，
  // 想要摘除能力的配置该把比例调大，而不是我在这里偷偷放宽成 ceil。
  // 分母 = **当前启用的成员数**（ADR-12 追加节：去重目标表），不是 ever-seen 的
  // 槽位容量——被摘除比例量的是"现在的容量"，不是"历史上见过的总量"。
  const std::uint64_t count = active_.load(std::memory_order_relaxed);
  return static_cast<std::size_t>(count * policy.max_ejection_percent / 100u);
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

bool HealthTable::try_mark_ejected(std::uint16_t target, std::int64_t now_ms,
                                   std::int64_t epoch_ms, bool from_half_open, const HealthPolicy& policy) noexcept {
  Entry& e = entries_[target];
  const std::int64_t next = encode_open(now_ms + policy.fail_timeout_ms);
  for (int attempt = 0; attempt < 4; ++attempt) {
    std::int64_t cur = e.state_ms.load(std::memory_order_relaxed);
    if (from_half_open) {
      if (cur == 0 || !is_half_open(cur)) return false;  // 已经不是半开：那次账不归我
    } else if (cur != 0) {
      return false;  // 已被摘着/已在半开：不是我的账
    }
    if (e.state_ms.compare_exchange_weak(cur, next, std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
      e.ejections_total.fetch_add(1, std::memory_order_relaxed);
      // epoch_ms = 0 表示这次迁移没有墙钟可报（比如循环里 sweep 推的那一次，它的
      // 时间线由脚本自己的墙钟钉），不许把"从未"写回这一格去盖掉真实的摘除时刻。
      if (epoch_ms != 0) e.last_ejection_epoch_ms.store(epoch_ms, std::memory_order_relaxed);
      if (from_half_open) {
        // 半开 → 摘除：阀位本来就占着，只把"半开"这一格退掉（ADR-12 追加二第二节第 5 条）。
        half_open_now_.fetch_sub(1, std::memory_order_relaxed);
      } else {
        ejected_now_.fetch_add(1, std::memory_order_relaxed);
      }
      e.consecutive_failures.store(0, std::memory_order_relaxed);
      e.half_open_cycle.store(0, std::memory_order_relaxed);
      // 摘除那一刻窗口清零：不清的话，刚被放回来的成员会被自己上一周期的旧账立刻再摘
      // ——那正是 S3-03 已知边界④替我挡过一次的横跳。
      window_clear(target, now_ms, policy);
      return true;
    }
  }
  return false;
}

bool HealthTable::try_enter_half_open(std::uint16_t target, std::int64_t expected_state,
                                      std::int64_t now_ms, const HealthPolicy& policy) noexcept {
  Entry& e = entries_[target];
  if (expected_state == 0 || is_half_open(expected_state)) return false;
  const std::int64_t next = encode_half_open(now_ms + policy.circuit.half_open_timeout_ms);
  std::int64_t cur = expected_state;
  if (!e.state_ms.compare_exchange_strong(cur, next, std::memory_order_relaxed,
                                          std::memory_order_relaxed)) {
    return false;
  }
  e.consecutive_failures.store(0, std::memory_order_relaxed);
  e.half_open_cycle.store(0, std::memory_order_relaxed);
  // **不进 `ejected_now_` 也不从它里出来**：半开继续占着安全阀的位（第二节第 5 条），
  // 动的只有"其中半开有几台"这一格。
  half_open_now_.fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool HealthTable::try_mark_healthy(std::uint16_t target, std::int64_t expected_state) noexcept {
  Entry& e = entries_[target];
  if (!e.state_ms.compare_exchange_strong(expected_state, 0, std::memory_order_relaxed,
                                          std::memory_order_relaxed)) {
    return false;  // 别人已经推进过，或又摘了一轮：状态账都不归我
  }
  e.recoveries_total.fetch_add(1, std::memory_order_relaxed);
  // 从这一刻起这一台重新接全量流量，阀位退还一只。半开来的和摘除来的都要退。
  ejected_now_.fetch_sub(1, std::memory_order_relaxed);
  if (is_half_open(expected_state)) half_open_now_.fetch_sub(1, std::memory_order_relaxed);
  return true;
}

bool HealthTable::try_take_probe_slot(std::uint16_t target, const HealthPolicy& policy) noexcept {
  Entry& e = entries_[target];
  const std::uint64_t budget = policy.circuit.half_open_requests;
  std::uint64_t cur = e.half_open_cycle.load(std::memory_order_relaxed);
  for (int attempt = 0; attempt < 8; ++attempt) {
    if ((cur & kProbeUsedMask) >= budget) {
      e.half_open_denied_total.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    if (e.half_open_cycle.compare_exchange_weak(cur, cur + 1, std::memory_order_relaxed,
                                                std::memory_order_relaxed)) {
      e.half_open_admitted_total.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
  }
  return false;  // 抢了八次没抢到：按"没名额"处理。方向是少放一条探测，不是多放
}

void HealthTable::note_half_open_success(std::uint16_t target, const HealthPolicy& policy) noexcept {
  Entry& e = entries_[target];
  const std::uint64_t cycle = e.half_open_cycle.fetch_add(kProbeOkStep, std::memory_order_relaxed) +
                              kProbeOkStep;
  if ((cycle >> 32) < policy.circuit.half_open_successes) return;
  std::int64_t cur = e.state_ms.load(std::memory_order_relaxed);
  for (int attempt = 0; attempt < 4 && cur != 0 && is_half_open(cur); ++attempt) {
    if (e.state_ms.compare_exchange_weak(cur, 0, std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
      e.recoveries_total.fetch_add(1, std::memory_order_relaxed);
      e.half_open_recovered_total.fetch_add(1, std::memory_order_relaxed);
      e.consecutive_failures.store(0, std::memory_order_relaxed);
      e.half_open_cycle.store(0, std::memory_order_relaxed);
      ejected_now_.fetch_sub(1, std::memory_order_relaxed);
      half_open_now_.fetch_sub(1, std::memory_order_relaxed);
      return;
    }
  }
}

void HealthTable::window_record(std::uint16_t target, std::int64_t now_ms, bool failure,
                                std::uint64_t* requests, std::uint64_t* failures,
                                const HealthPolicy& policy) noexcept {
  Entry& e = entries_[target];
  // `now_ms` 来自单调钟（steady_clock），不会倒退；测试喂的是自己构造的时间线，
  // 也不许倒退。这里仍加一道"落后就当同一格"的保护，因为环形推进是 `while (seq != 目标)`，
  // 一旦目标在身后就是死循环——宁可把异常时间当成同一桶，也不留一个能挂死的环。
  const std::uint64_t raw_seq =
      now_ms > 0 ? static_cast<std::uint64_t>(now_ms) / bucket_ms_of(policy) : 0;
  std::lock_guard<std::mutex> lock(e.window_mu);
  if (raw_seq > e.window_seq) {
    const std::uint64_t gap = raw_seq - e.window_seq;
    if (gap >= kWindowBuckets) {
      // 落后超过一整个窗口：窗内已经没有仍在有效期内的样本，整表清零。
      for (auto& bucket : e.window_buckets) bucket = WindowBucket{};
      e.window_requests = 0;
      e.window_failures = 0;
    } else {
      for (std::uint64_t step = 1; step <= gap; ++step) {
        const std::uint64_t seq = e.window_seq + step;
        WindowBucket& bucket = e.window_buckets[seq & (kWindowBuckets - 1)];
        // 这一格原来装的是 `seq - 8`，它现在出窗了：从窗口总数里扣掉。
        e.window_requests -= bucket.requests;
        e.window_failures -= bucket.failures;
        bucket.requests = 0;
        bucket.failures = 0;
      }
    }
    e.window_seq = raw_seq;
  }
  WindowBucket& current = e.window_buckets[e.window_seq & (kWindowBuckets - 1)];
  ++e.window_requests;
  ++current.requests;
  if (failure) {
    ++e.window_failures;
    ++current.failures;
  }
  if (requests != nullptr) *requests = e.window_requests;
  if (failures != nullptr) *failures = e.window_failures;
}

void HealthTable::window_clear(std::uint16_t target, std::int64_t now_ms,
                         const HealthPolicy& policy) noexcept {
  Entry& e = entries_[target];
  const std::uint64_t raw_seq =
      now_ms > 0 ? static_cast<std::uint64_t>(now_ms) / bucket_ms_of(policy) : 0;
  std::lock_guard<std::mutex> lock(e.window_mu);
  for (auto& bucket : e.window_buckets) bucket = WindowBucket{};
  e.window_seq = raw_seq;
  e.window_requests = 0;
  e.window_failures = 0;
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
  const std::int64_t state = e.state_ms.load(std::memory_order_relaxed);
  out.ejected_until_ms = deadline_of(state);
  out.half_open = state != 0 && is_half_open(state);
  out.last_ejection_epoch_ms = e.last_ejection_epoch_ms.load(std::memory_order_relaxed);
  out.rate_trips_total = e.rate_trips_total.load(std::memory_order_relaxed);
  out.half_open_admitted_total = e.half_open_admitted_total.load(std::memory_order_relaxed);
  out.half_open_denied_total = e.half_open_denied_total.load(std::memory_order_relaxed);
  out.half_open_recovered_total = e.half_open_recovered_total.load(std::memory_order_relaxed);
  out.half_open_reopened_total = e.half_open_reopened_total.load(std::memory_order_relaxed);
  return out;
}

}  // namespace l7gate::route
