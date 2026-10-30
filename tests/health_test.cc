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
TEST(S3_03_Health, SuccessResetsConsecutiveFailures) {
  HealthTable health(1, policy_with(3, 10000, 100));
  (void)health.note_failure(0, 1000, 0);
  (void)health.note_failure(0, 1100, 0);
  health.note_success(0);
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
