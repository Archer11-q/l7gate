// S3-03（ADR-12 落地 + ADR-19 追加第三/四条）：健康状态表与候选集那一层的用例。
//
// 与 balance_test.cc 同一条分工：**这个文件一个 socket 都不建**。HealthTable 的决策、
// 候选集的构造、平手集合内轮询、环查找跳过被摘除成员——全部能在不建连接的形状里断。
// 探活连接机（真 connect/GET/超时）与被动记账点（真 5xx/截断）在 reactor_test.cc。
//
// 判据形状的教训来自 ADR-19 追加二：**只看上界的断言挡不住退化实现**。所以这一批用例
// 每一条"最多/不超过"旁边都有一条"确实发生了"的下界。
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "filter/chain.h"
#include "filter/lb_filter.h"
#include "http/request_parser.h"
#include "lb_fixture.h"
#include "route/balance.h"
#include "route/health.h"
#include "route/router.h"

namespace {

using l7gate::filter::Decision;
using l7gate::filter::LoadBalanceFilter;
using l7gate::filter::RequestInfo;
using l7gate::route::Admission;
using l7gate::route::Balance;
using l7gate::route::FailureVerdict;
using l7gate::route::GroupTable;
using l7gate::route::HealthPolicy;
using l7gate::route::HealthTable;
using l7gate::route::kNoUpstreamGroup;
using l7gate::route::kNoUpstreamTarget;
using l7gate::route::LbState;
using l7gate::route::select_from_ring;
using l7gate::route::UpstreamGroup;

RequestInfo plain_request(std::string_view target) {
  RequestInfo info;
  info.method = "GET";
  info.target = target;
  info.request_bytes = 40;
  info.downstream_fd = 7;
  return info;
}

std::uint16_t make_group(GroupTable* table, std::string_view name, Balance balance,
                         const std::vector<std::uint16_t>& members,
                         const std::vector<std::string>& endpoint_keys) {
  std::string error;
  std::uint16_t group = kNoUpstreamGroup;
  const bool ok = l7gate::testing::add_balanced_group(table, name, balance, members, endpoint_keys,
                                                      &group, &error);
  EXPECT_TRUE(ok) << error;
  return group;
}

HealthPolicy policy_with(std::uint32_t max_fails, std::int64_t fail_timeout_ms,
                         std::uint32_t percent) {
  HealthPolicy policy;
  policy.max_fails = max_fails;
  policy.fail_timeout_ms = fail_timeout_ms;
  policy.probe_interval_ms = 0;  // 决策用例不探活；探活连接机归 reactor_test
  policy.probe_timeout_ms = 500;
  policy.max_ejection_percent = percent;
  return policy;
}

// 候选集用例的时钟：LoadBalanceFilter 读的是真实 steady_clock（它没有时钟注入口，
// 这是刻意的——filter 的签名不为测试多一个参数），所以摘除动作也必须打在同一根轴上，
// 否则"摘到 1 万毫秒后到期"在真实时钟下等于从没摘过。
std::int64_t now_ms() noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

// --- 一、摘除与恢复的状态机 ----------------------------------------------------------

// 连续失败没到阈值不摘：摘除的判据是"连续 max_fails 次"，一次网络抖动不该变成一台死机
// （nginx 缺省 max_fails=1，这里取 3 的理由写在 ADR-19 追加四）。
TEST(S3_03_Health, BelowThresholdDoesNotEject) {
  HealthTable health(2, policy_with(3, 10000, 100));
  EXPECT_EQ(health.note_failure(0, 1000, 0), FailureVerdict::kRecorded);
  EXPECT_EQ(health.note_failure(0, 1100, 0), FailureVerdict::kRecorded);
  EXPECT_FALSE(health.is_ejected(0, 1100));
  EXPECT_EQ(health.ejected_now(), 0u);
  EXPECT_EQ(health.snapshot(0).ejections_total, 0u);
}

// 第 max_fails 次失败触发摘除，时长 = fail_timeout；到期之后读侧按健康算。
TEST(S3_03_Health, ReachingMaxFailsEjectsForFailTimeout) {
  HealthTable health(2, policy_with(3, 5000, 100));
  EXPECT_EQ(health.note_failure(0, 1000, 0), FailureVerdict::kRecorded);
  EXPECT_EQ(health.note_failure(0, 1100, 0), FailureVerdict::kRecorded);
  EXPECT_EQ(health.note_failure(0, 1200, 77'777), FailureVerdict::kEjected);
  EXPECT_TRUE(health.is_ejected(0, 1200));
  EXPECT_TRUE(health.is_ejected(0, 6199));   // 到期时刻之前（到期 = 最后一次失败的 now+fail_timeout = 6200）
  EXPECT_FALSE(health.is_ejected(0, 6200));
  // 下界（ADR-12 追加节）：摘除**确实发生过**，且墙钟时刻被记了观测格。
  EXPECT_EQ(health.snapshot(0).ejections_total, 1u);
  EXPECT_EQ(health.snapshot(0).last_ejection_epoch_ms, 77'777);
  EXPECT_EQ(health.ejected_now(), 1u);
}

// 活着的证据清连续计数：两次失败 + 一次成功 + 两次失败不该摘。
// （S4-05 起 `note_success` 多一个时钟参数——成功样本要进错误率窗口。本用例的策略里
// `circuit` 没装配，所以窗口那条路整个不走，判据与今天逐字相同。）
TEST(S3_03_Health, SuccessResetsConsecutiveFailures) {
  HealthTable health(1, policy_with(3, 10000, 100));
  (void)health.note_failure(0, 1000, 0);
  (void)health.note_failure(0, 1100, 0);
  health.note_success(0, 1150);
  EXPECT_EQ(health.note_failure(0, 1200, 0), FailureVerdict::kRecorded);
  EXPECT_EQ(health.note_failure(0, 1300, 0), FailureVerdict::kRecorded);
  EXPECT_FALSE(health.is_ejected(0, 1300));
}

// 已在摘除期内的失败不重复计摘：kAlreadyEjected，ejections_total 不动。
TEST(S3_03_Health, FailureDuringEjectionDoesNotReEject) {
  HealthTable health(1, policy_with(1, 10000, 100));
  EXPECT_EQ(health.note_failure(0, 1000, 0), FailureVerdict::kEjected);
  EXPECT_EQ(health.note_failure(0, 2000, 0), FailureVerdict::kAlreadyEjected);
  EXPECT_EQ(health.snapshot(0).ejections_total, 1u);
  EXPECT_EQ(health.ejected_now(), 1u);
}

// 到期恢复由 sweep 兑现状态账；恢复后连续计数归零（重新攒满才再摘）。
TEST(S3_03_Health, SweepExpiredRevivesAndResetsStrikes) {
  HealthTable health(1, policy_with(1, 1000, 100));
  EXPECT_EQ(health.note_failure(0, 1000, 0), FailureVerdict::kEjected);
  EXPECT_EQ(health.sweep_expired(1500), 0u) << "没到期不许恢复";
  EXPECT_EQ(health.sweep_expired(2000), 1u);
  EXPECT_FALSE(health.is_ejected(0, 2000));
  EXPECT_EQ(health.ejected_now(), 0u);
  EXPECT_EQ(health.snapshot(0).recoveries_total, 1u);
  // 恢复后一次失败不该立刻再摘（max_fails=1 的策略下会——这里就让它立刻再摘，
  // 证的是"计数真的归零了、不是又被加到天上"）。
  EXPECT_EQ(health.note_failure(0, 2100, 0), FailureVerdict::kEjected);
}

// 探活成功立即恢复（不等 fail_timeout）；探活失败走与被动同一条失败路径。
TEST(S3_03_Health, ProbeResultRevivesOrCountsFailure) {
  HealthTable health(1, policy_with(1, 60000, 100));
  EXPECT_EQ(health.note_failure(0, 1000, 0), FailureVerdict::kEjected);
  // 还在摘除期内，探活成功就回来。
  health.note_probe_result(0, /*alive=*/true, 1500, 0);
  EXPECT_FALSE(health.is_ejected(0, 1500));
  EXPECT_EQ(health.snapshot(0).probe_ok_total, 1u);
  EXPECT_EQ(health.snapshot(0).recoveries_total, 1u);
  // 探活失败汇入同一个连续计数（max_fails=1 → 一拍就摘）。
  health.note_probe_result(0, /*alive=*/false, 2000, 0);
  EXPECT_TRUE(health.is_ejected(0, 2000));
  EXPECT_EQ(health.snapshot(0).probe_fail_total, 1u);
}

// 探活成功打在没被摘过的目标上：不该凭空造出一笔恢复账。
TEST(S3_03_Health, ProbeSuccessOnHealthyTargetCreatesNoRecovery) {
  HealthTable health(1, policy_with(3, 10000, 100));
  health.note_probe_result(0, true, 1000, 0);
  EXPECT_EQ(health.snapshot(0).recoveries_total, 0u);
  EXPECT_EQ(health.ejected_now(), 0u);
}

// --- 二、安全阀（ADR-12 第一次真正落地的那一格）--------------------------------------

// 10 台全挂、上限 10%：**恰好**停在 floor(10×10/100)=1 台，其余全部 kValveBlocked，
// 且"被挡"这件事有可观测计数。断言形状是"最坏只损失 10% 容量"，不是"我没摘除"。
TEST(S3_03_Health, ValveCapsEjectionAtConfiguredPercent) {
  HealthTable health(10, policy_with(1, 10000, 10));
  int ejected = 0;
  int blocked = 0;
  for (std::uint16_t t = 0; t < 10; ++t) {
    const FailureVerdict verdict = health.note_failure(t, 1000, 0);
    if (verdict == FailureVerdict::kEjected) {
      ++ejected;
    } else {
      EXPECT_EQ(verdict, FailureVerdict::kValveBlocked);
      ++blocked;
    }
  }
  EXPECT_EQ(ejected, 1);
  EXPECT_EQ(blocked, 9);
  EXPECT_EQ(health.ejected_now(), 1u);
  EXPECT_EQ(health.ejections_blocked_by_valve(), 9u);
}

// floor 语义：3 台 × 50% = 1 台（不是 1.5 进成 2）；3 台 × 10% = 0 台（一个都不能摘，
// 想要摘除能力的配置该调比例，我不偷偷放宽成 ceil——ADR-12 追加节）。
TEST(S3_03_Health, ValveCountIsFloorSemantics) {
  EXPECT_EQ(HealthTable(3, policy_with(1, 1000, 50)).max_ejected_count(), 1u);
  EXPECT_EQ(HealthTable(3, policy_with(1, 1000, 10)).max_ejected_count(), 0u);
  EXPECT_EQ(HealthTable(1, policy_with(1, 1000, 100)).max_ejected_count(), 1u);
}

// 比例配 0 = 阀门全关：判据永远成立也永远不摘（"被摘除容量 ≤ 0%"的字面读法）。
TEST(S3_03_Health, ValveZeroPercentMeansNeverEject) {
  HealthTable health(3, policy_with(1, 1000, 0));
  EXPECT_EQ(health.note_failure(0, 1000, 0), FailureVerdict::kValveBlocked);
  EXPECT_EQ(health.note_failure(1, 1000, 0), FailureVerdict::kValveBlocked);
  EXPECT_EQ(health.ejected_now(), 0u);
}

// 阀门满时被挡的后端仍攒着计数，等阀门腾出格子（别的后端恢复）就轮到它。
TEST(S3_03_Health, ValveFreesUpWhenAnEjectedTargetRecovers) {
  HealthTable health(2, policy_with(1, 1000, 50));  // 可摘 1 台
  EXPECT_EQ(health.note_failure(0, 1000, 0), FailureVerdict::kEjected);
  EXPECT_EQ(health.note_failure(1, 1100, 0), FailureVerdict::kValveBlocked);
  EXPECT_EQ(health.sweep_expired(3000), 1u);  // 0 号到期恢复，格子腾出来
  EXPECT_EQ(health.note_failure(1, 3100, 0), FailureVerdict::kEjected) << "被挡的账丢了";
  EXPECT_EQ(health.ejected_now(), 1u);
}

// --- 三、候选集与三个策略 ------------------------------------------------------------
//
// 候选集过滤的形状：被摘除的成员不被选中；全员被摘时回退全员（ADR-12 的"故意保留一段
// 错误"，请求无去处是 S4 的空格子，我不在这里造第二次故障）。

// 轮询只在候选集内轮：2 号被摘后，M 条请求在 0/1 之间对半。
TEST(S3_03_Candidates, RoundRobinRotatesOnlyWithinCandidates) {
  GroupTable table;
  const std::uint16_t group = make_group(&table, "svc", Balance::kRoundRobin, {0, 1, 2},
                                         l7gate::testing::endpoint_keys_for(3));
  auto state = std::make_shared<LbState>(3, table.size());
  auto health = std::make_shared<HealthTable>(3, policy_with(1, 10000, 100));
  EXPECT_EQ(health->note_failure(2, now_ms(), 0), FailureVerdict::kEjected);
  LoadBalanceFilter filter(table, state, health);

  int first = 0;
  int second = 0;
  for (int i = 0; i < 6; ++i) {
    RequestInfo info = plain_request("/x");
    info.upstream_group = group;
    ASSERT_EQ(filter.on_request(info), Decision::kContinue);
    ASSERT_NE(info.upstream_target, 2u) << "被摘除的成员接到了流量";
    (info.upstream_target == 0u ? first : second)++;
  }
  EXPECT_EQ(first, 3);
  EXPECT_EQ(second, 3);
}

// 最少连接：被摘除的成员即使"最闲"也不在候选集里。
TEST(S3_03_Candidates, LeastConnSkipsEjectedEvenWhenIdlest) {
  GroupTable table;
  const std::uint16_t group = make_group(&table, "svc", Balance::kLeastConnections, {0, 1, 2},
                                         l7gate::testing::endpoint_keys_for(3));
  auto state = std::make_shared<LbState>(3, table.size());
  auto health = std::make_shared<HealthTable>(3, policy_with(1, 10000, 100));
  EXPECT_EQ(health->note_failure(1, now_ms(), 0), FailureVerdict::kEjected);
  state->note_paired(0);  // 0 号忙、2 号闲、1 号"最闲"但被摘
  LoadBalanceFilter filter(table, state, health);

  RequestInfo info = plain_request("/x");
  info.upstream_group = group;
  ASSERT_EQ(filter.on_request(info), Decision::kContinue);
  EXPECT_EQ(info.upstream_target, 2u);
}

// 一致性哈希：key 的落点被摘了就顺时针走到下一个候选成员；候选集 = 全员时与两参版本逐点同解。
TEST(S3_03_Candidates, RingWalksToNextCandidateWhenOwnerEjected) {
  GroupTable table;
  const std::uint16_t group = make_group(&table, "svc", Balance::kConsistentHash, {0, 1, 2},
                                         l7gate::testing::endpoint_keys_for(3));
  const UpstreamGroup* built = table.group_at(group);
  ASSERT_NE(built, nullptr);
  const std::uint16_t owner = *select_from_ring(*built, "alice");
  const std::vector<std::uint16_t> all{0, 1, 2};

  // 全员候选：与两参版本同解（快路径）。
  EXPECT_EQ(select_from_ring(*built, "alice", all), *select_from_ring(*built, "alice"));

  // 摘掉 owner：同一个 key 落到环上顺时针的下一个候选成员（不是回到 owner，也不是乱选）。
  std::vector<std::uint16_t> reduced;
  for (const std::uint16_t t : all) {
    if (t != owner) reduced.push_back(t);
  }
  const std::uint16_t next = *select_from_ring(*built, "alice", reduced);
  EXPECT_NE(next, owner);
}

// 全员被摘 → 回退全员：摘除安全阀的世界里不允许"请求无去处"。
TEST(S3_03_Candidates, AllEjectedFallsBackToFullMemberSet) {
  GroupTable table;
  const std::uint16_t group = make_group(&table, "svc", Balance::kLeastConnections, {0, 1},
                                         l7gate::testing::endpoint_keys_for(2));
  auto state = std::make_shared<LbState>(2, table.size());
  auto health = std::make_shared<HealthTable>(2, policy_with(1, 10000, 100));
  EXPECT_EQ(health->note_failure(0, 1000, 0), FailureVerdict::kEjected);
  EXPECT_EQ(health->note_failure(1, 1000, 0), FailureVerdict::kEjected);
  LoadBalanceFilter filter(table, state, health);

  RequestInfo info = plain_request("/x");
  info.upstream_group = group;
  ASSERT_EQ(filter.on_request(info), Decision::kContinue);
  EXPECT_NE(info.upstream_target, kNoUpstreamTarget) << "请求不许无去处";
  EXPECT_EQ(info.reject_status, 0u);
}

// --- 四、least_conn 平手集合内轮询（ADR-19 追加三，S3-03 开工裁决①）------------------
//
// 这是 B-013 那条落盘今天缺的断言：**全员平手时流量不许全落在成员 0**。
// 旧规则（取成员顺序第一条）的实测形状是 98.3% 吸附在成员 0；新规则下 12 条请求
// 应当在 3 个成员之间对半。阳性验证：把平手分支改回"取第一条"，这条当场红。
TEST(S3_03_LeastConnTie, FullTieRotatesWithinTheTiedSet) {
  GroupTable table;
  const std::uint16_t group = make_group(&table, "svc", Balance::kLeastConnections, {0, 1, 2},
                                         l7gate::testing::endpoint_keys_for(3));
  auto state = std::make_shared<LbState>(3, table.size());
  LoadBalanceFilter filter(table, state);  // 无健康表：全员候选、全员配对数为 0

  std::size_t hits[3] = {0, 0, 0};
  for (int i = 0; i < 12; ++i) {
    RequestInfo info = plain_request("/x");
    info.upstream_group = group;
    ASSERT_EQ(filter.on_request(info), Decision::kContinue);
    ASSERT_LT(info.upstream_target, 3u);
    ++hits[info.upstream_target];
  }
  EXPECT_EQ(hits[0], 4u);
  EXPECT_EQ(hits[1], 4u);
  EXPECT_EQ(hits[2], 4u) << "平手集合内没有轮转：退回了'取成员顺序第一条'的旧规则";
}

// 独占最小值的成员必须被选中且**不消耗游标**：平手规则不能把"方向"那条语义挤掉
// （它是 ADR-19 阳性验证第三条的形状，比较符翻转就是这条红）。
TEST(S3_03_LeastConnTie, StrictlyLeastLoadedWinsWithoutMovingTheCursor) {
  GroupTable table;
  const std::uint16_t group = make_group(&table, "svc", Balance::kLeastConnections, {0, 1, 2},
                                         l7gate::testing::endpoint_keys_for(3));
  auto state = std::make_shared<LbState>(3, table.size());
  state->note_paired(0);
  state->note_paired(0);
  state->note_paired(1);  // 2 号独占最小
  LoadBalanceFilter filter(table, state);

  RequestInfo info = plain_request("/x");
  info.upstream_group = group;
  ASSERT_EQ(filter.on_request(info), Decision::kContinue);
  EXPECT_EQ(info.upstream_target, 2u);
  EXPECT_EQ(state->advance_rr(group, 3), 0u)
      << "独占最小值的调用不该消耗平手游标（游标是下一个平手时刻的账）";
}

// ============ S4-05（ADR-12 追加二）：熔断的第二个输入与半开门 ============
//
// 这批用例的判据全部**成对**写（ADR-12 追加一 + ADR-19 追加二那条教训）：每条
// "最多/不超过"旁边都有一条"确实发生了"，否则"从不熔断""半开从不放行""从不恢复"
// 这三种退化实现都能过只看上界的断言。
namespace {

// 熔断策略。两个隔离旋钮要说清楚，不然用例证的不是它自称的那件事：
//  - `max_fails` 与窗口阈值各自"够不着"的那一种，用来把摘除**归因到唯一一条判据**；
//  - `minimum_requests` 给到 1'000'000 时窗口那条判等式永远不成立，于是这批用例里
//    "摘了"只可能来自连续失败或半开规则；反过来 `max_fails=99` 时只可能来自窗口。
HealthPolicy circuit_policy(std::uint32_t max_fails, std::int64_t fail_timeout_ms,
                            std::int64_t window_ms, std::uint64_t minimum_requests,
                            std::uint32_t error_rate_percent, std::uint32_t half_open_requests,
                            std::uint32_t half_open_successes, std::int64_t half_open_timeout_ms,
                            std::uint32_t ejection_percent = 100) {
  HealthPolicy policy;
  policy.max_fails = max_fails;
  policy.fail_timeout_ms = fail_timeout_ms;
  policy.probe_interval_ms = 0;  // 决策用例不跑探活连接机（那台在 reactor_test）
  policy.probe_timeout_ms = 500;
  policy.max_ejection_percent = ejection_percent;
  policy.circuit.enabled = true;
  policy.circuit.window_ms = window_ms;
  policy.circuit.minimum_requests = minimum_requests;
  policy.circuit.error_rate_percent = error_rate_percent;
  policy.circuit.half_open_requests = half_open_requests;
  policy.circuit.half_open_successes = half_open_successes;
  policy.circuit.half_open_timeout_ms = half_open_timeout_ms;
  return policy;
}

// 窗口判据专场的策略：连续失败那条够不着（max_fails=99）。
HealthPolicy rate_only_policy(std::uint64_t minimum_requests = 10, std::uint32_t percent = 50) {
  return circuit_policy(/*max_fails=*/99, /*fail_timeout_ms=*/60'000, /*window_ms=*/1000,
                        minimum_requests, percent, /*half_open_requests=*/3,
                        /*half_open_successes=*/2, /*half_open_timeout_ms=*/5000);
}

// 半开门专场的策略：窗口那条够不着（样本下界给到 100 万）。
HealthPolicy gate_policy(std::uint32_t max_fails = 1, std::int64_t fail_timeout_ms = 1000,
                         std::uint32_t half_open_requests = 3,
                         std::uint32_t half_open_successes = 2,
                         std::int64_t half_open_timeout_ms = 5000,
                         std::uint32_t ejection_percent = 100) {
  return circuit_policy(max_fails, fail_timeout_ms, /*window_ms=*/1000,
                        /*minimum_requests=*/1'000'000, /*error_rate_percent=*/50,
                        half_open_requests, half_open_successes, half_open_timeout_ms,
                        ejection_percent);
}

}  // namespace

// --- 一、第二个判据：窗口内错误率 ----------------------------------------------------

// 40%~70% 的失败率夹着成功时，连续计数永远清零、永远不摘（S3-03 那把尺看不见的形状），
// 窗口必须替它摘。配对的两半：样本没到 `minimum_requests` 时**不许摘**（上界），
// 到了就**确实摘了**（下界）。
TEST(S4_05_Circuit, ErrorRateTripsWhereConsecutiveCannot) {
  HealthTable health(1, rate_only_policy());
  for (int i = 0; i < 6; ++i) {
    EXPECT_EQ(health.note_failure(0, 100 + 10 * i, 0), FailureVerdict::kRecorded) << i;
  }
  for (int i = 0; i < 3; ++i) health.note_success(0, 160 + 10 * i);
  EXPECT_EQ(health.snapshot(0).ejections_total, 0u)
      << "只有 9 条样本（`minimum_requests`=10）就摘除：体积下界没生效";

  // 第 10 条样本：10 条里 7 条失败 = 70% ≥ 50% → 摘。
  EXPECT_EQ(health.note_failure(0, 190, 0), FailureVerdict::kEjected);
  EXPECT_TRUE(health.is_ejected(0, 190));
  EXPECT_EQ(health.snapshot(0).ejections_total, 1u);
  EXPECT_EQ(health.snapshot(0).rate_trips_total, 1u) << "摘除没有归因到错误率这一格";
  EXPECT_EQ(health.ejected_now(), 1u);
}

// 同样的 100% 失败率，9 条样本不摘、第 10 条摘：判据是"体积 + 比率"两条与，
// 不是"有失败就摘"。这条也是上一条的下界反面——它证明窗口判据不是恒假。
TEST(S4_05_Circuit, FullErrorRateBelowVolumeFloorDoesNotTrip) {
  HealthTable health(1, rate_only_policy());
  for (int i = 0; i < 9; ++i) {
    EXPECT_EQ(health.note_failure(0, 100 + 100 * i, 0), FailureVerdict::kRecorded) << i;
    EXPECT_EQ(health.snapshot(0).ejections_total, 0u) << i;
  }
  EXPECT_EQ(health.note_failure(0, 950, 0), FailureVerdict::kEjected);
  EXPECT_EQ(health.snapshot(0).rate_trips_total, 1u);
}

// 窗口是**滑动**的，不是累计的：一整个窗口之前的旧失败必须出账。
// 形状：9 条失败（不足样本 → 不摘）→ 时间跨过整个窗口 → 再 6 条失败。
// 窗口内 = 6 条样本 < 10 → 不摘；而"不清旧桶"的实现会看到 15 条样本 15 条失败 → 当场红。
// 再补 4 条失败：窗口内 10 条全败 → 摘（下界：出账之后判据仍然活着）。
TEST(S4_05_Circuit, OldErrorsSlideOutOfTheWindow) {
  HealthTable health(1, rate_only_policy());
  for (int i = 0; i < 9; ++i) (void)health.note_failure(0, 100 + 100 * i, 0);
  EXPECT_EQ(health.snapshot(0).ejections_total, 0u);

  for (int i = 0; i < 6; ++i) {
    EXPECT_EQ(health.note_failure(0, 3000 + 50 * i, 0), FailureVerdict::kRecorded) << i;
    EXPECT_EQ(health.snapshot(0).ejections_total, 0u) << i
        << "窗口外的旧失败还算在判据里：那是累计计数，不是滑动窗口";
  }
  for (int i = 6; i < 10; ++i) (void)health.note_failure(0, 3000 + 50 * i, 0);
  EXPECT_EQ(health.snapshot(0).ejections_total, 1u);
  EXPECT_EQ(health.snapshot(0).rate_trips_total, 1u);
}

// 摘除那一刻窗口必须清零，否则刚被放回来的成员会被自己的旧账立刻再摘（横跳）。
// **整条时间线刻意压在同一个桶里**（window_ms=100'000 → bucket_ms=12'500，全程 t<1200）：
// 不然恢复的那一刻会因为自然滑窗把旧账清掉，这条用例就变成"恒真"——第一版就是这么写的，
// 阳性验证把 `window_clear` 短路之后 318 条全绿，那才是这条用例真正的红。
TEST(S4_05_Circuit, EjectionClearsTheWindowSoRecoveryIsNotInstantlyRejudged) {
  HealthTable health(1, circuit_policy(/*max_fails=*/99, /*fail_timeout_ms=*/500,
                                       /*window_ms=*/100'000, /*minimum_requests=*/10,
                                       /*error_rate_percent=*/50, /*half_open_requests=*/3,
                                       /*half_open_successes=*/2,
                                       /*half_open_timeout_ms=*/100'000));
  // 10 条失败全在同一个桶里（t=100..550 < bucket_ms）：第 10 条判据成立。
  for (int i = 0; i < 10; ++i) (void)health.note_failure(0, 100 + 50 * i, 0);
  ASSERT_EQ(health.snapshot(0).ejections_total, 1u) << "前置：窗口判据先摘一次";

  // 到期 → 半开 → 两条业务成功 → 全量恢复（全程没跨过任何一个桶）。
  ASSERT_EQ(health.admit(0, 1100), Admission::kProbeSlot);
  health.note_success(0, 1110);
  ASSERT_EQ(health.admit(0, 1120), Admission::kProbeSlot);
  health.note_success(0, 1130);
  ASSERT_EQ(health.snapshot(0).half_open_recovered_total, 1u);
  ASSERT_EQ(health.ejected_now(), 0u);

  // 恢复后的第一条失败：窗口里只有"半开期两条成功 + 这一条"= 3 条样本，不足 10 → 不许摘。
  // 没清零的话这里是 13 条样本 / 11 条失败 = 84% ≥ 50% 且 ≥ 10 → 当场再摘。
  EXPECT_EQ(health.note_failure(0, 1140, 0), FailureVerdict::kRecorded);
  EXPECT_EQ(health.snapshot(0).ejections_total, 1u)
      << "旧账把刚回来的成员立刻再摘了：摘除时窗口没清（或者这条用例压不住桶边界）";

  // 下界：清零不等于判据失效——攒满样本仍然要摘。
  for (int i = 1; i <= 7; ++i) (void)health.note_failure(0, 1150 + 10 * i, 0);
  EXPECT_EQ(health.snapshot(0).ejections_total, 2u);
  EXPECT_EQ(health.snapshot(0).rate_trips_total, 2u);
}

// --- 二、半开门：预算、恢复、回摘、到期 ----------------------------------------------

// 摘除到期不再"全量放行"：进半开，一个周期内只放 `half_open_requests` 条，
// 第 B+1 条在门口被挡。配的下界：三条**真的放进去了**（"半开从不放行"的实现会红）。
TEST(S4_05_Circuit, ExpiryEntersHalfOpenWithACappedBudget) {
  HealthTable health(1, gate_policy(/*max_fails=*/1, /*fail_timeout_ms=*/1000,
                                    /*half_open_requests=*/3, /*half_open_successes=*/2));
  EXPECT_EQ(health.note_failure(0, 1000, 0), FailureVerdict::kEjected);
  EXPECT_EQ(health.admit(0, 1500), Admission::kDeny) << "还在摘除冷却期就放了行";

  EXPECT_EQ(health.admit(0, 2100), Admission::kProbeSlot) << "到期没进半开（旧语义是到点全量）";
  EXPECT_EQ(health.admit(0, 2101), Admission::kProbeSlot);
  EXPECT_EQ(health.admit(0, 2102), Admission::kProbeSlot);
  EXPECT_EQ(health.admit(0, 2103), Admission::kDeny) << "预算 B=3 之外还放得进第 4 条";

  const auto snap = health.snapshot(0);
  EXPECT_EQ(snap.half_open_admitted_total, 3u);
  EXPECT_GE(snap.half_open_denied_total, 1u);
  EXPECT_TRUE(snap.half_open);
  EXPECT_FALSE(health.is_ejected(0, 2103)) << "半开不算摘除中（S4-02 靠这一格分 502 与 503）";
  EXPECT_EQ(snap.ejections_total, 1u) << "进半开不该算第二次摘除";
  EXPECT_EQ(snap.recoveries_total, 0u) << "进半开不是恢复";
  EXPECT_EQ(health.ejected_now(), 1u) << "半开必须继续占着安全阀的位（ADR-12 追加二第二节第 5 条）";
  EXPECT_EQ(health.half_open_now(), 1u);
}

// 攒满 K 条业务成功 → 全量恢复；恢复之后不再限流。
// 配对的两半：攒不满 K 不许恢复（上界），攒满了**确实**恢复（下界）。
TEST(S4_05_Circuit, HalfOpenNeedsKBusinessSuccessesAndThenGivesFullTraffic) {
  HealthTable health(1, gate_policy(/*max_fails=*/1, /*fail_timeout_ms=*/1000,
                                    /*half_open_requests=*/3, /*half_open_successes=*/2));
  (void)health.note_failure(0, 1000, 0);
  ASSERT_EQ(health.admit(0, 2100), Admission::kProbeSlot);

  health.note_success(0, 2101);
  EXPECT_TRUE(health.snapshot(0).half_open) << "一条成功就转正：K=2 没生效";
  EXPECT_EQ(health.snapshot(0).half_open_recovered_total, 0u);

  ASSERT_EQ(health.admit(0, 2102), Admission::kProbeSlot);
  health.note_success(0, 2103);
  EXPECT_FALSE(health.snapshot(0).half_open);
  EXPECT_EQ(health.snapshot(0).half_open_recovered_total, 1u);
  EXPECT_EQ(health.snapshot(0).recoveries_total, 1u);
  EXPECT_EQ(health.ejected_now(), 0u) << "恢复了就该把阀位退还出去";
  EXPECT_EQ(health.half_open_now(), 0u);
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(health.admit(0, 2200 + i), Admission::kAllow) << i;
  }
}

// 半开期内一次业务失败 → **立即**回摘，不等 `max_fails`。
// 归因是干净的：策略里 `max_fails=2`，而进半开之前那条失败被清零过一次，
// 所以"回摘"只能来自半开规则那一条分支（`max_fails` 那条要 2 条连着）。
TEST(S4_05_Circuit, HalfOpenFailureReEjectsOnTheFirstProbeThatFails) {
  HealthTable health(1, gate_policy(/*max_fails=*/2, /*fail_timeout_ms=*/1000));
  EXPECT_EQ(health.note_failure(0, 1000, 0), FailureVerdict::kRecorded);
  EXPECT_EQ(health.note_failure(0, 1001, 0), FailureVerdict::kEjected);
  ASSERT_EQ(health.admit(0, 2100), Admission::kProbeSlot);

  EXPECT_EQ(health.note_failure(0, 2101, 0), FailureVerdict::kEjected)
      << "半开期内失败没有立即回摘（连续计数那条在这里够不着）";
  const auto snap = health.snapshot(0);
  EXPECT_EQ(snap.half_open_reopened_total, 1u);
  EXPECT_EQ(snap.ejections_total, 2u);
  EXPECT_FALSE(snap.half_open);
  EXPECT_TRUE(health.is_ejected(0, 2101));
  EXPECT_EQ(health.ejected_now(), 1u) << "半开回摘不该再占一个阀位";
  EXPECT_EQ(health.half_open_now(), 0u);
}

// 半开不是终点站：期内没攒够证据（探测请求挂着、或根本没流量）→ 到期回摘。
// 没有这一格的话，"半开预算耗尽 + 无人回话"就是一种永久的低效率状态。
TEST(S4_05_Circuit, HalfOpenDeadlineWithoutEvidenceReOpensIt) {
  HealthTable health(1, gate_policy(/*max_fails=*/1, /*fail_timeout_ms=*/1000,
                                    /*half_open_requests=*/3, /*half_open_successes=*/2,
                                    /*half_open_timeout_ms=*/500));
  EXPECT_EQ(health.note_failure(0, 1000, 0), FailureVerdict::kEjected);
  ASSERT_EQ(health.admit(0, 2100), Admission::kProbeSlot);

  EXPECT_EQ(health.sweep_expired(2300), 0u) << "半开还没到期就动了它";
  EXPECT_EQ(health.sweep_expired(2650), 1u);
  EXPECT_EQ(health.snapshot(0).half_open_reopened_total, 1u);
  EXPECT_EQ(health.snapshot(0).ejections_total, 2u);
  EXPECT_EQ(health.half_open_now(), 0u);
  EXPECT_EQ(health.admit(0, 2650), Admission::kDeny) << "回摘之后应该重新冷却";
}

// 主动探活判活在熔断装配时的作用从"立即全量恢复"改成"提前进半开"（ADR-12 追加二第七节
// 写的那条已知弱点，本轮只升级到期那条路的**另一半**：探活叫醒到半开，不再直接放回全量）。
TEST(S4_05_Circuit, ProbeSuccessWakesIntoHalfOpenNotFullTraffic) {
  HealthTable health(1, gate_policy(/*max_fails=*/1, /*fail_timeout_ms=*/60'000));
  EXPECT_EQ(health.note_failure(0, 1000, 0), FailureVerdict::kEjected);

  health.note_probe_result(0, /*alive=*/true, 2000, 0);
  const auto snap = health.snapshot(0);
  EXPECT_FALSE(health.is_ejected(0, 2000));
  EXPECT_TRUE(snap.half_open) << "探活成功仍然直接全量恢复（旧语义）";
  EXPECT_EQ(snap.recoveries_total, 0u);
  EXPECT_EQ(snap.half_open_recovered_total, 0u);
  EXPECT_EQ(snap.half_open_admitted_total, 0u) << "探活不该消耗半开的业务预算";
  EXPECT_EQ(health.ejected_now(), 1u) << "半开继续占阀位";
  EXPECT_EQ(health.half_open_now(), 1u);
  EXPECT_EQ(health.admit(0, 2001), Admission::kProbeSlot);
}

// 出半开**只认业务请求的成功证据**：探活成功十次也不算。反例形状是"一台只肯答
// GET /health、业务全回 500"的后端被自己的探活推回全量流量。
TEST(S4_05_Circuit, ProbeEvidenceAloneNeverClosesTheHalfOpen) {
  HealthTable health(1, gate_policy(/*max_fails=*/1, /*fail_timeout_ms=*/60'000,
                                    /*half_open_requests=*/3, /*half_open_successes=*/2));
  (void)health.note_failure(0, 1000, 0);
  health.note_probe_result(0, true, 2000, 0);
  ASSERT_TRUE(health.snapshot(0).half_open);

  for (int i = 0; i < 10; ++i) {
    health.note_probe_result(0, true, 2100 + i, 0);
    EXPECT_TRUE(health.snapshot(0).half_open) << i;
    EXPECT_EQ(health.snapshot(0).half_open_recovered_total, 0u) << i;
  }
  EXPECT_EQ(health.snapshot(0).probe_ok_total, 11u);

  (void)health.admit(0, 2200);
  health.note_success(0, 2201);
  (void)health.admit(0, 2202);
  health.note_success(0, 2203);
  EXPECT_FALSE(health.snapshot(0).half_open) << "两条业务成功该把它转正";
  EXPECT_EQ(health.snapshot(0).half_open_recovered_total, 1u);
}

// 故意的不对称：探活**判死**在半开期内仍然立即回摘（成员资格上它一票否决，
// S3-05 的口径"探活是成员资格的唯一真源"），方向是 fail-closed。
TEST(S4_05_Circuit, ProbeFailureDuringHalfOpenReOpens) {
  HealthTable health(1, gate_policy(/*max_fails=*/1, /*fail_timeout_ms=*/60'000));
  (void)health.note_failure(0, 1000, 0);
  health.note_probe_result(0, true, 2000, 0);
  ASSERT_TRUE(health.snapshot(0).half_open);

  health.note_probe_result(0, /*alive=*/false, 2100, 0);
  EXPECT_EQ(health.snapshot(0).half_open_reopened_total, 1u);
  EXPECT_EQ(health.snapshot(0).probe_fail_total, 1u);
  EXPECT_TRUE(health.is_ejected(0, 2100));
  EXPECT_EQ(health.snapshot(0).ejections_total, 2u);
}

// 半开占着阀位这件事的另一半：10 台、上限 10% → 只有一台能"不接全量流量"，
// 那台进半开之后第二台仍然被阀按住（否则 10 台可以各自半开、全组容量≈0 而账上写
// 着"损失 0 台"——ADR-12 量的是容量不是状态名）。
TEST(S4_05_Circuit, HalfOpenStillHoldsTheSafetyValveSlot) {
  HealthTable health(10, gate_policy(/*max_fails=*/1, /*fail_timeout_ms=*/1000, /*B=*/3, /*K=*/2,
                                     /*half_open_timeout_ms=*/5000, /*ejection_percent=*/10));
  ASSERT_EQ(health.max_ejected_count(), 1u);
  EXPECT_EQ(health.note_failure(0, 1000, 0), FailureVerdict::kEjected);
  ASSERT_EQ(health.admit(0, 2100), Admission::kProbeSlot) << "前置：0 号已在半开";

  EXPECT_EQ(health.note_failure(1, 2100, 0), FailureVerdict::kValveBlocked)
      << "半开的成员不再算占用容量，第二台就能被摘——阀白装了";
  EXPECT_EQ(health.ejections_blocked_by_valve(), 1u);
  EXPECT_EQ(health.ejected_now(), 1u);
}

// **段不存在 = 今天的语义逐字节不变**（ADR-12 追加二第三节那条承诺）：到期仍由 sweep
// 全量恢复，半开的四格账一笔都没有。
TEST(S4_05_Circuit, WithoutCircuitSectionExpiryStillRevivesFully) {
  HealthTable health(1, policy_with(/*max_fails=*/1, /*fail_timeout_ms=*/1000, /*percent=*/100));
  EXPECT_EQ(health.note_failure(0, 1000, 0), FailureVerdict::kEjected);
  EXPECT_EQ(health.admit(0, 1500), Admission::kDeny);
  // 熔断未装配时读侧不顺手改状态（S3-03 的"到期未扫按健康算"是纯读），状态账留给 sweep。
  EXPECT_EQ(health.admit(0, 2100), Admission::kAllow);
  EXPECT_EQ(health.snapshot(0).half_open_admitted_total, 0u);
  EXPECT_EQ(health.snapshot(0).rate_trips_total, 0u);
  EXPECT_EQ(health.sweep_expired(2100), 1u);
  EXPECT_EQ(health.snapshot(0).recoveries_total, 1u);
  EXPECT_EQ(health.ejected_now(), 0u);
  EXPECT_FALSE(health.snapshot(0).half_open);
}

// 探活样本**不进错误率窗口**（口径③"仪器不进被测方"在熔断这一层的形状）。
// 这条断的是判死的探活：连续计数照旧吃它（已裁③④），窗口不吃——否则一台只是
// 探活路径配错的后端会被自己那一拍拍成"业务错误率 100%"。
TEST(S4_05_Circuit, ProbeFailuresFeedStrikesButNeverTheWindow) {
  HealthTable health(1, circuit_policy(/*max_fails=*/99, /*fail_timeout_ms=*/60'000,
                                       /*window_ms=*/1000, /*minimum_requests=*/6,
                                       /*error_rate_percent=*/50, /*B=*/3, /*K=*/2,
                                       /*half_open_timeout_ms=*/5000));
  for (int i = 0; i < 10; ++i) {
    health.note_probe_result(0, /*alive=*/false, 100 + 50 * i, 0);
    EXPECT_EQ(health.snapshot(0).ejections_total, 0u) << i
        << "探活失败进了错误率窗口：仪器写进了被测方";
  }
  EXPECT_EQ(health.snapshot(0).probe_fail_total, 10u);
  // 下界：同一批样本换成业务来源，判据立刻成立（上面那十条不是恒真的空转）。
  for (int i = 0; i < 6; ++i) {
    (void)health.note_failure(0, 700 + 10 * i, 0);
  }
  EXPECT_EQ(health.snapshot(0).ejections_total, 1u);
  EXPECT_EQ(health.snapshot(0).rate_trips_total, 1u);
}

// 判活的探活同样不进窗口——它会把分母稀释掉，让真坏的后端逃过摘除。
// 形状：9 条业务失败 + 10 条探活判活 + 1 条业务失败 = 窗口里 10 条样本全败（100% ≥ 60%）
// → 摘。若探活成功也进窗口，分母变成 20、比率 50% < 60% → 不摘，这条就红。
TEST(S4_05_Circuit, ProbeSuccessesNeverDiluteTheWindow) {
  HealthTable health(1, circuit_policy(/*max_fails=*/99, /*fail_timeout_ms=*/60'000,
                                       /*window_ms=*/1000, /*minimum_requests=*/10,
                                       /*error_rate_percent=*/60, /*B=*/3, /*K=*/2,
                                       /*half_open_timeout_ms=*/5000));
  for (int i = 0; i < 9; ++i) (void)health.note_failure(0, 100 + 20 * i, 0);
  for (int i = 0; i < 10; ++i) health.note_probe_result(0, /*alive=*/true, 300 + 20 * i, 0);
  EXPECT_EQ(health.snapshot(0).ejections_total, 0u);
  EXPECT_EQ(health.note_failure(0, 520, 0), FailureVerdict::kEjected)
      << "探活成功稀释了窗口分母：真坏的后端靠仪器的样本逃过摘除";
  EXPECT_EQ(health.snapshot(0).rate_trips_total, 1u);
}

// --- 三、候选集：半开放行的那一条必须真的落到它 --------------------------------------

// 门给名额的那一条请求被**钉**给半开成员（本轮跳过策略选择），预算耗尽之后它就不再
// 接到流量。这条断的是 ADR-12 追加二第二节第 2 条那件事：只"允许被选中"等于摆设
// ——B-013 实测 98.3% 的流量吸附在成员 0，健康成员会把探测挤光。
TEST(S4_05_Candidates, HalfOpenMemberIsPinnedWhenTheGateGrants) {
  GroupTable table;
  const std::uint16_t group = make_group(&table, "svc", Balance::kRoundRobin, {0, 1},
                                         l7gate::testing::endpoint_keys_for(2));
  auto state = std::make_shared<LbState>(2, table.size());
  // fail_timeout=1ms：摘掉之后睡一小段就到期，让候选集自己把它推进半开。
  auto health = std::make_shared<HealthTable>(2, gate_policy(/*max_fails=*/1,
                                                            /*fail_timeout_ms=*/1,
                                                            /*B=*/2, /*K=*/2,
                                                            /*half_open_timeout_ms=*/5000));
  EXPECT_EQ(health->note_failure(1, now_ms(), 0), FailureVerdict::kEjected);
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  LoadBalanceFilter filter(table, state, health);

  for (int i = 0; i < 2; ++i) {
    RequestInfo info = plain_request("/x");
    info.upstream_group = group;
    ASSERT_EQ(filter.on_request(info), Decision::kContinue) << i;
    EXPECT_EQ(info.upstream_target, 1u) << i << "预算内的探测没被钉给半开成员";
  }
  for (int i = 0; i < 6; ++i) {
    RequestInfo info = plain_request("/x");
    info.upstream_group = group;
    ASSERT_EQ(filter.on_request(info), Decision::kContinue) << i;
    EXPECT_EQ(info.upstream_target, 0u) << i << "预算耗尽之后半开成员还在接全量流量";
  }
  EXPECT_EQ(health->snapshot(1).half_open_admitted_total, 2u);
  EXPECT_GE(health->snapshot(1).half_open_denied_total, 1u);
  EXPECT_EQ(health->snapshot(0).half_open_admitted_total, 0u) << "健康的成员不该被门打扰";
}

// 全员半开且预算耗尽 → 候选集全空 → 回退全员（S3-03 那条最后防线原样留着），
// 并且"回退发生了"这一格有账（ADR-12 追加二第二节第 6 条的代价要能读出来）。
TEST(S4_05_Candidates, AllHalfOpenExhaustedFallsBackAndCountsIt) {
  GroupTable table;
  const std::uint16_t group = make_group(&table, "svc", Balance::kRoundRobin, {0, 1},
                                         l7gate::testing::endpoint_keys_for(2));
  auto state = std::make_shared<LbState>(2, table.size());
  auto health = std::make_shared<HealthTable>(2, gate_policy(/*max_fails=*/1,
                                                            /*fail_timeout_ms=*/1,
                                                            /*B=*/1, /*K=*/1,
                                                            /*half_open_timeout_ms=*/5000));
  EXPECT_EQ(health->note_failure(0, now_ms(), 0), FailureVerdict::kEjected);
  EXPECT_EQ(health->note_failure(1, now_ms(), 0), FailureVerdict::kEjected);
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  LoadBalanceFilter filter(table, state, health);

  std::vector<std::uint16_t> picked;
  for (int i = 0; i < 3; ++i) {
    RequestInfo info = plain_request("/x");
    info.upstream_group = group;
    ASSERT_EQ(filter.on_request(info), Decision::kContinue) << i;
    picked.push_back(info.upstream_target);
  }
  EXPECT_EQ(picked[0], 0u) << "第一条该钉给第一个拿到名额的成员";
  EXPECT_EQ(picked[1], 1u) << "第二条该钉给第二个（各自的预算是各自的）";
  EXPECT_LT(picked[2], 2u) << "全员预算耗尽也必须有个落点（回退全员）";
  EXPECT_EQ(health->all_denied_fallbacks(), 1u);
}
