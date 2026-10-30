// S3-02（ADR-19）：负载均衡那一层的用例——组表、FNV-1a、哈希环、轮询游标、配对计数，
// 以及 `LoadBalanceFilter` 在链上写回什么。
//
// 这个文件**一个 socket 都不建**，和 route_test.cc 是同一条分工（1.23 第二节：判定逻辑一旦
// 和 socket 状态混在一起，我就只能用真连接去测它，测出来的失败分不清"算法错了"还是"时序没对上"）。
// "某个后端到底收到几条请求"那种按后端计数的断言在 reactor_test.cc 的 `S3_02_LoadBalance`，
// 因为那需要真连接，而且可见性沿用 S3-01 的立场：由**被连的那一侧自报**（`TestUpstream`），
// 不给网关加新的产品侧计数（ADR-19「我放弃什么」最后一条）。
//
// 迁移率那一条是本文件的主语，也是本任务最容易糊过去的一格：教科书说"N→N+1 只迁移
// 1/(N+1)"，而这里断的是**我自己量出来的差集比例**，不是那句话（ADR-19 第五条末段：
// 如果 160 vnodes 下不成立，我改 vnodes 或者改哈希函数，不改判据）。
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "filter/chain.h"
#include "filter/lb_filter.h"
#include "http/request_parser.h"
#include "http/response_framing.h"
#include "lb_fixture.h"
#include "route/balance.h"
#include "route/router.h"

namespace {

using l7gate::filter::Decision;
using l7gate::filter::LoadBalanceFilter;
using l7gate::filter::RequestInfo;
using l7gate::route::Balance;
using l7gate::route::build_hash_ring;
using l7gate::route::fnv1a64;
using l7gate::route::GroupTable;
using l7gate::route::HashKey;
using l7gate::route::kDefaultVnodes;
using l7gate::route::kNoUpstreamGroup;
using l7gate::route::kNoUpstreamTarget;
using l7gate::route::LbState;
using l7gate::route::parse_balance;
using l7gate::route::ring_hash;
using l7gate::route::select_from_ring;
using l7gate::route::UpstreamGroup;

// 三个后端 + 与它们一一对应的端点键。多数用例用这一套，因为 3 是唯一一个既能看出轮询
// 周期、又能让 N→N+1 的界（1/4）落在可测范围内的最小值。
std::vector<std::string> three_keys() { return l7gate::testing::endpoint_keys_for(3); }

// 建一个装配好的组（含环）。失败就当场红——**装配没成的用例给不出有意义的分布或迁移率**，
// 让它继续跑下去比让它红更糟。
std::uint16_t make_group(GroupTable* table, std::string_view name, Balance balance,
                         const std::vector<std::uint16_t>& members,
                         const std::vector<std::string>& endpoint_keys,
                         std::uint32_t vnodes = kDefaultVnodes, const HashKey& hash_key = {}) {
  std::string error;
  std::uint16_t group = kNoUpstreamGroup;
  const bool ok = l7gate::testing::add_balanced_group(table, name, balance, members, endpoint_keys,
                                                      &group, &error, vnodes, hash_key);
  EXPECT_TRUE(ok) << error;
  return group;
}

const UpstreamGroup* group_of(const GroupTable& table, std::uint16_t group) {
  return table.group_at(group);
}

// 一条只用于喂均衡 filter 的请求；headers 指向字面量，活得比断言久（同 route_test.cc 的口径：
// RequestInfo 里的 view 全是借来的，用例只读不存）。
RequestInfo plain_request(std::string_view target) {
  RequestInfo info;
  info.method = "GET";
  info.target = target;
  info.request_bytes = 40;
  info.downstream_fd = 7;
  return info;
}

}  // namespace

// --- 一、策略名与组表：装配期的严格性 ---------------------------------------------

// `balance` 拼错必须让启动失败，不能退化成"用了缺省策略"（ADR-19 第二条的严格性立场，
// 原型是 `--upstream` 那条"配置写错不该变成少连一个上游"）。这里测解析函数本身：
// 安静接受 `round-robin` 或 `Least_Conn` 的实现，会让报告里写的策略和跑着的策略不是同一个。
TEST(S3_02_BalanceParse, AcceptsOnlyTheThreeLiterals) {
  Balance parsed = Balance::kConsistentHash;
  EXPECT_TRUE(parse_balance("round_robin", &parsed));
  EXPECT_EQ(parsed, Balance::kRoundRobin);
  EXPECT_TRUE(parse_balance("least_conn", &parsed));
  EXPECT_EQ(parsed, Balance::kLeastConnections);
  EXPECT_TRUE(parse_balance("consistent_hash", &parsed));
  EXPECT_EQ(parsed, Balance::kConsistentHash);

  // 每一条都必须**拒绝**，而不是折叠、截断或回落到缺省值。
  parsed = Balance::kConsistentHash;
  EXPECT_FALSE(parse_balance("round-robin", &parsed));   // 连字符版
  EXPECT_FALSE(parse_balance("Round_Robin", &parsed));   // 大小写折叠版
  EXPECT_FALSE(parse_balance("least_connections", &parsed));  // 复数版
  EXPECT_FALSE(parse_balance("weighted", &parsed));      // 已裁不做的加权
  EXPECT_FALSE(parse_balance("", &parsed));
  EXPECT_FALSE(parse_balance("consistent_hash ", &parsed));  // 尾空格
  EXPECT_EQ(parsed, Balance::kConsistentHash) << "拒绝的一支不许把出参写了半个值";
}

// 组名的两条拒绝（空名、重名）+ 成员的两条拒绝（重复成员、哨兵目标）+ 未知组。
// **重复成员是迁移率用例的第一个陷阱**：同一个 `地址:端口` 在一个组里出现两次，
// "这个组有几个后端"就有两个答案，而我那条 N→N+1 的界是按"几个后端"算的。
TEST(S3_02_GroupTable, RejectsDuplicateNamesDuplicateMembersAndSentinels) {
  GroupTable table;
  std::uint16_t first = kNoUpstreamGroup;
  std::uint16_t second = kNoUpstreamGroup;
  ASSERT_TRUE(table.add_group("api", Balance::kRoundRobin, &first));
  EXPECT_FALSE(table.add_group("api", Balance::kLeastConnections, &second)) << "组名重名被接受了";
  EXPECT_FALSE(table.add_group("", Balance::kRoundRobin, &second)) << "空组名被接受了";
  EXPECT_EQ(table.size(), 1u) << "失败的那两次 add_group 不许留下半条组";

  ASSERT_TRUE(table.add_member(first, 0));
  EXPECT_FALSE(table.add_member(first, 0)) << "同一个后端在一个组里登记了两次";
  EXPECT_FALSE(table.add_member(first, kNoUpstreamTarget)) << "哨兵不是合法的目标下标";
  EXPECT_FALSE(table.add_member(9, 1)) << "不存在的组不该收下成员";

  const std::vector<std::uint16_t>* members = table.members_of(first);
  ASSERT_NE(members, nullptr);
  EXPECT_EQ(members->size(), 1u) << "被拒绝的 add_member 留下了成员";
}

// 组名 → 下标是配置文件里唯一允许写的身份（ADR-19「为什么」第二条：用下标写配置的话，
// `upstreams` 数组一增删排序，所有路由静默改道）。查不到必须给哨兵，不是给 0。
TEST(S3_02_GroupTable, IndexByNameIsTheOnlyIdentityAndUnknownIsSentinel) {
  GroupTable table;
  std::uint16_t group = kNoUpstreamGroup;
  ASSERT_TRUE(table.add_group("zeta", Balance::kRoundRobin, &group));
  ASSERT_TRUE(table.add_group("alpha", Balance::kRoundRobin, &group));
  EXPECT_EQ(table.group_index_by_name("zeta"), 0u);
  EXPECT_EQ(table.group_index_by_name("alpha"), 1u);
  EXPECT_EQ(table.group_index_by_name("nope"), kNoUpstreamGroup)
      << "查不到返回 0 的话，一条写错组名的路由会静默改道到第一个组";
  EXPECT_EQ(table.group_by_name("nope"), nullptr);
  EXPECT_EQ(table.group_at(2), nullptr) << "越界读要空指针，不是最后一个组";
}

// --- 二、FNV-1a：常量锁死的收据 ---------------------------------------------------

// ADR-19 第五条把两个常数写成了契约（换成 `std::hash` 就等于把"报告里的数"绑在
// 这台机器的这一版标准库上）。这里是**发布出来的测试向量**，不是我自己的实现回读自己：
// 谁把 offset basis 或 prime 改了、或者改成 32 位宽度，这一格当场红，而所有
// key→后端 的映射也随之作废——正是这条用例该抓的事。
TEST(S3_02_Fnv1a64, MatchesPublishedTestVectors) {
  EXPECT_EQ(fnv1a64(""), 0xcbf29ce484222325ULL);        // offset basis：空串就是初值
  EXPECT_EQ(fnv1a64("a"), 0xaf63dc4c8601ec8cULL);
  EXPECT_EQ(fnv1a64("foobar"), 0x85944171f73967e8ULL);
  // 长度不同的输入不该撞在一起（环的点键形如 `组名#序号#端点`，靠的就是这个）。
  EXPECT_NE(fnv1a64("api#0#127.0.0.1:18081"), fnv1a64("api#1#127.0.0.1:18081"));
}

// **这一格是"为什么环上不能直接用裸 FNV"的收据**（实测迁移率 48.10% 那条病的根因）。
// 同一个组的两个成员，点键只差末位端口号：裸 FNV-1a 对这种输入只差约一个 prime 的量级
// （1.1e12，相对 2^64 ≈ 6e-8），于是每个成员的 160 个点各自挤成一簇；加了终值混合之后
// 同一条相邻输入落到整圈的尺度上。写成 2^64 的比例而不是浮点，免得日后换了编译器又是一种答案。
TEST(S3_02_RingHash, SpreadsInputsThatOnlyDifferInTheLastByte) {
  auto gap = [](std::uint64_t a, std::uint64_t b) {
    return a > b ? a - b : b - a;
  };
  constexpr std::uint64_t kFull = 0xffffffffffffffffULL;
  const std::string left = "svc#0#127.0.0.1:18081";
  const std::string right = "svc#0#127.0.0.1:18082";

  // 裸 FNV：差值不到整圈的百万分之一（这正是"点成簇"的度量）。
  EXPECT_LT(gap(fnv1a64(left), fnv1a64(right)), kFull / 1000000u)
      << "末位差异被放大得不够的话，环上的点就是一簇一簇的";
  // ring_hash：同一条相邻输入落到千分之一圈以外（真正取到多小由哈希决定，但绝不会再是 6e-8 量级）。
  EXPECT_GT(gap(ring_hash(left), ring_hash(right)), kFull / 1000u)
      << "终值混合没生效：迁移率会退回实测那个 48%";
  // key 与点同一个函数（只混点不混 key 等于拿没刻度的尺量拉直过的尺）。
  EXPECT_EQ(ring_hash("/orders/1"), ring_hash("/orders/1"));
  EXPECT_NE(ring_hash("/orders/1"), fnv1a64("/orders/1"));
}

// --- 三、哈希环：建它的四条拒绝与它的圆 -------------------------------------------

// 同一份输入两次建环必须给出逐点相同的环（可重复喂入那条纪律在这一层的形态），而且点数
// = 成员数 × vnodes、点按哈希值升序。**点数这一格守着 vnodes 的语义**：如果"每个成员
// 160 个点"写成了"整张表 160 个点"，N→N+1 时每个既有点都会被挪位置，我的迁移率界就不成立。
TEST(S3_02_HashRing, IsDeterministicSortedAndSized) {
  const std::vector<std::string> keys = l7gate::testing::endpoint_keys_for(3);
  GroupTable a;
  GroupTable b;
  const std::uint16_t ga = make_group(&a, "svc", Balance::kConsistentHash, {0, 1, 2}, keys);
  const std::uint16_t gb = make_group(&b, "svc", Balance::kConsistentHash, {0, 1, 2}, keys);
  const UpstreamGroup* left = group_of(a, ga);
  const UpstreamGroup* right = group_of(b, gb);
  ASSERT_NE(left, nullptr);
  ASSERT_NE(right, nullptr);

  EXPECT_EQ(left->ring.size(), 3u * kDefaultVnodes) << "点数不等于成员数×vnodes：vnodes 的语义被改了";
  ASSERT_EQ(left->ring.size(), right->ring.size());
  EXPECT_TRUE(left->ring == right->ring) << "同一份配置两次建出两张不同的环";
  for (std::size_t i = 1; i < left->ring.size(); ++i) {
    ASSERT_LE(left->ring[i - 1].first, left->ring[i].first) << "环没排序，lower_bound 就是错的";
  }
}

// 成员取不到端点键 → **整体失败且不留半张环**。半张环的迁移率数字会"按我少算的那个数"
// 成立，而那条断言是本任务唯一能证伪"我实现了哈希环"的东西（ADR-19 第五条）。
TEST(S3_02_HashRing, MissingEndpointKeyLeavesNoHalfRing) {
  GroupTable table;
  std::uint16_t group = kNoUpstreamGroup;
  ASSERT_TRUE(table.add_group("svc", Balance::kConsistentHash, &group));
  ASSERT_TRUE(table.add_member(group, 0));
  ASSERT_TRUE(table.add_member(group, 7));  // 端点键表只有 3 条，这个成员取不到键
  UpstreamGroup* built = const_cast<UpstreamGroup*>(table.group_at(group));
  ASSERT_NE(built, nullptr);
  built->ring.emplace_back(1u, static_cast<std::uint16_t>(0));  // 先放一个点，验证失败路径把它清掉了

  const std::vector<std::string> keys = l7gate::testing::endpoint_keys_for(3);
  EXPECT_FALSE(build_hash_ring(built, keys));
  EXPECT_TRUE(built->ring.empty()) << "建环失败却留下了半张环：迁移率会按少算的那个数成立";
}

// 空组与 vnodes=0 都在建环这一步被挡：前者是"这条路由后面一个后端都没有"，
// 后者会建出一张空环，于是"选一个"永远无解——而运行期那一格回的是 502（见下面的 filter 用例）。
TEST(S3_02_HashRing, EmptyGroupAndZeroVnodesAreRejected) {
  const std::vector<std::string> keys = l7gate::testing::endpoint_keys_for(3);
  GroupTable empty_group;
  std::string error;
  std::uint16_t group = kNoUpstreamGroup;
  EXPECT_FALSE(l7gate::testing::add_balanced_group(&empty_group, "svc", Balance::kConsistentHash,
                                                   {}, keys, &group, &error));
  EXPECT_NE(error.find("build_hash_ring"), std::string::npos) << error;

  GroupTable zero_vnodes;
  EXPECT_FALSE(l7gate::testing::add_balanced_group(&zero_vnodes, "svc", Balance::kConsistentHash,
                                                   {0, 1}, keys, &group, &error, /*vnodes=*/0));
}

// 非 consistent_hash 的组**不建环**，而且返回成功。这条拒绝的是"顺手给所有组都建一张环"：
// 那张没人读的点表会占住每个目标 160 个 pair，而"环"对轮询和最少连接根本不是数据结构。
TEST(S3_02_HashRing, NonHashStrategyBuildsNoRingButSucceeds) {
  const std::vector<std::string> keys = l7gate::testing::endpoint_keys_for(3);
  GroupTable table;
  const std::uint16_t group = make_group(&table, "rr", Balance::kRoundRobin, {0, 1, 2}, keys);
  const UpstreamGroup* built = group_of(table, group);
  ASSERT_NE(built, nullptr);
  EXPECT_TRUE(built->ring.empty()) << "给轮询组建了一张没人读的环";
  EXPECT_EQ(built->targets.size(), 3u) << "成员还是得登记的，只是不建环";
}

// 空环返回 nullopt，**而不是"没选中所以给第一个"**。调用方（filter）把它当配置坏了回 502。
TEST(S3_02_HashRing, EmptyRingSelectsNothing) {
  UpstreamGroup group;
  group.name = "svc";
  group.balance = Balance::kConsistentHash;
  EXPECT_EQ(select_from_ring(group, "/anything"), std::nullopt);
}

// 环是圆的。这一格找的是"哈希值落在最后一个点之上"的输入——不找的话，回绕那段代码
// （`it == end → begin`）就是没人走过的分支，而它是环正确性的一半：没有它，
// 那一段 key 空间上的请求会全部失败而不是落到第一个后端。
TEST(S3_02_HashRing, KeysAboveTheLastPointWrapToTheFirst) {
  GroupTable table;
  // vnodes=1、两个成员 → 环上只有两个点，最后一个点的期望位置在 2/3 处，
  // 于是"超过它"的输入在几百个候选里必然找得到（这条是确定性的，没有随机种子）。
  const std::uint16_t group = make_group(&table, "svc", Balance::kConsistentHash, {0, 1},
                                         l7gate::testing::endpoint_keys_for(2), /*vnodes=*/1);
  const UpstreamGroup* built = group_of(table, group);
  ASSERT_NE(built, nullptr);
  ASSERT_EQ(built->ring.size(), 2u);

  std::string wrap_key;
  for (int i = 0; i < 100000; ++i) {
    std::string candidate = "wrap-" + std::to_string(i);
    if (ring_hash(candidate) > built->ring.back().first) {
      wrap_key = candidate;
      break;
    }
  }
  ASSERT_FALSE(wrap_key.empty()) << "没找到超过最后一个点的输入，这一格就没测到回绕";
  const std::optional<std::uint16_t> picked = select_from_ring(*built, wrap_key);
  ASSERT_TRUE(picked.has_value());
  EXPECT_EQ(*picked, built->ring.front().second) << "超过末尾的点没有回到环首";
  std::printf("[S3-02 ring] 回绕 key=%s hash=%llu → 选中目标 %u（环首是 %u）\n", wrap_key.c_str(),
              static_cast<unsigned long long>(ring_hash(wrap_key)),
              static_cast<unsigned int>(*picked),
              static_cast<unsigned int>(built->ring.front().second));
}

// 验收句的"同一 key 恒定同一后端"那一半（纯函数那一半）：重复喂 100 次。
// 端到端那一半在 reactor_test.cc 的 `S3_02_LoadBalance`，那里断的是"真连接上也确实是同一个后端"。
TEST(S3_02_HashRing, SameKeyAlwaysSelectsTheSameMember) {
  GroupTable table;
  const std::uint16_t group =
      make_group(&table, "svc", Balance::kConsistentHash, {0, 1, 2}, three_keys());
  const UpstreamGroup* built = group_of(table, group);
  ASSERT_NE(built, nullptr);

  for (std::string_view key : {std::string_view("/orders/1"), std::string_view("/health"),
                               std::string_view("alice"), std::string_view("")}) {
    const std::optional<std::uint16_t> first = select_from_ring(*built, key);
    ASSERT_TRUE(first.has_value()) << key;
    for (int i = 0; i < 100; ++i) {
      ASSERT_EQ(select_from_ring(*built, key), first) << "第 " << i << " 次喂同一个 key 换了后端";
    }
  }
}

// --- 四、迁移率：**这是量出来的，不是我引用的性质** --------------------------------

// 判据来自 ADR-19 第五条：N→N+1 时映射变了的 key 占比 ≤ 1/(N+1)。
// 形状 = 固定 10,000 个确定性 key（`key-%05d`，不用随机种子碰运气），N 个后端建一张环、
// N+1 个后端再建一张（**共用那 N 个后端原本的点键**，所以既有的点一个都没挪位置——
// 挪了的话这条量的就不是"加一个后端"而是"重排整张表"），统计差集比例。
// 比较用整数乘法，不引入浮点判等。跑出来的实测比例打印进日志，收尾时进 BENCHMARK。
TEST(S3_02_MigrationRate, AddingOneBackendMovesAtMostOneInNewTotal) {
  const std::vector<std::string> keys = l7gate::testing::endpoint_keys_for(8);
  constexpr int kKeys = 10000;

  for (const std::size_t n : {std::size_t{3}, std::size_t{7}}) {
    std::vector<std::uint16_t> members_before;
    std::vector<std::uint16_t> members_after;
    for (std::size_t t = 0; t < n + 1; ++t) {
      members_after.push_back(static_cast<std::uint16_t>(t));
      if (t < n) members_before.push_back(static_cast<std::uint16_t>(t));
    }

    GroupTable before_table;
    GroupTable after_table;
    const std::uint16_t before_group =
        make_group(&before_table, "svc", Balance::kConsistentHash, members_before, keys);
    const std::uint16_t after_group =
        make_group(&after_table, "svc", Balance::kConsistentHash, members_after, keys);
    const UpstreamGroup* left = group_of(before_table, before_group);
    const UpstreamGroup* right = group_of(after_table, after_group);
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);

    int migrated = 0;
    for (int i = 0; i < kKeys; ++i) {
      const std::string key = "key-" + std::to_string(i);
      const std::optional<std::uint16_t> a = select_from_ring(*left, key);
      const std::optional<std::uint16_t> b = select_from_ring(*right, key);
      ASSERT_TRUE(a.has_value() && b.has_value()) << key;
      if (*a != *b) ++migrated;
    }
    const double measured = static_cast<double>(migrated) / static_cast<double>(kKeys);
    std::printf("[S3-02 migration] N=%zu → N+1=%zu vnodes=%u keys=%d 迁移 %d 条 = %.2f%%（界 1/%zu = %.2f%%）\n",
                n, n + 1, kDefaultVnodes, kKeys, migrated, measured * 100.0, n + 1,
                100.0 / static_cast<double>(n + 1));
    // 整数形式：migrated × (N+1) ≤ kKeys  ⇔  migrated/kKeys ≤ 1/(N+1)
    EXPECT_LE(static_cast<std::int64_t>(migrated) * static_cast<std::int64_t>(n + 1),
              static_cast<std::int64_t>(kKeys))
        << "实测迁移率越过了 1/(N+1)：按 ADR-19，改 vnodes 或改哈希函数，不改判据";
  }
}

// --- 五、轮询游标与配对计数 ---------------------------------------------------------

// 轮询的形状就是验收句里那句 ⌊M/N⌋…⌈M/N⌉ 的最小形式，而且**游标是按组的**：两条规则
// 指向同一个组时必须共享同一个游标（ADR-19 第三条"组是策略的属性、不是规则的属性"），
// 两个不同的组各走各的。写成"周期回到 0"而不是"跑满 4 亿次不越界"，因为后者跑不完。
TEST(S3_02_RoundRobinCursor, CyclesPerGroupAndEachGroupHasItsOwn) {
  LbState state(/*target_count=*/3, /*group_count=*/2);
  std::vector<int> hits(3, 0);
  for (int i = 0; i < 9; ++i) {
    const std::size_t offset = state.advance_rr(0, 3);
    ASSERT_LT(offset, 3u);
    EXPECT_EQ(offset, static_cast<std::size_t>(i % 3)) << "轮询没走成 0,1,2 的周期";
    ++hits[offset];
  }
  for (const int hit : hits) EXPECT_EQ(hit, 3) << "9 次 ÷ 3 个成员，每个应当 3 次";

  // 第二个组的游标独立：它从**自己的** 0 开始，而第一个组的游标继续走它自己的周期。
  EXPECT_EQ(state.advance_rr(0, 3), 0u) << "走完 9 次应当回到周期起点";
  EXPECT_EQ(state.advance_rr(1, 3), 0u) << "第二个组不该接着第一个组的游标";
  EXPECT_EQ(state.advance_rr(1, 3), 1u);
  EXPECT_EQ(state.advance_rr(0, 3), 1u) << "第一个组的游标被别的组推进了：共享游标写错了键";
}

// 两个防御面：组下标越界与成员数为 0。两者都返回 0 而不是索引空 vector——
// 越界只可能是装配漏项，而这里猜一个后端等于把配置错误读成"策略有效"。
TEST(S3_02_RoundRobinCursor, UnknownGroupOrNoMembersYieldsZero) {
  LbState state(/*target_count=*/2, /*group_count=*/1);
  EXPECT_EQ(state.advance_rr(5, 2), 0u) << "不存在的组不该有游标，也不该崩";
  EXPECT_EQ(state.advance_rr(0, 0), 0u) << "成员数为 0 时取模会除零";
}

// **这条是交接段第 ⑦ 条坑的收据**：账必须加得也减得。少一次 `note_unpaired`，
// `least_conn` 的失败形态不是崩而是"永远选第一个成员"，看起来完全在正常工作。
// 下溢那一格同样重要：`uint32` 减成 42 亿之后那条后端永远"最忙"，等于被静默摘除。
TEST(S3_02_PairingCounts, CountsBothWaysClampsAtZeroAndIgnoresOutOfRange) {
  LbState state(/*target_count=*/2, /*group_count=*/1);
  EXPECT_EQ(state.pairings(0), 0u);
  state.note_paired(0);
  state.note_paired(0);
  EXPECT_EQ(state.pairings(0), 2u);
  state.note_unpaired(0);
  EXPECT_EQ(state.pairings(0), 1u) << "归还没减账：least_conn 会永远选第一个成员";
  state.note_unpaired(0);
  EXPECT_EQ(state.pairings(0), 0u);
  state.note_unpaired(0);
  EXPECT_EQ(state.pairings(0), 0u) << "多减一次会下溢成 42 亿，那条后端从此永远最忙";
  EXPECT_EQ(state.pairings(1), 0u) << "别的目标的账被动了";

  state.note_paired(99);  // 越界：装配漏项，不该扩表也不该猜
  EXPECT_EQ(state.pairings(99), 0u);
  EXPECT_EQ(state.target_count(), 2u) << "越界写入悄悄把表加长了";
}

// --- 六、LoadBalanceFilter：这一环只写目标，坏配置要表态 -----------------------------

// 没人决定过组 → kContinue，**且不写目标**。这一格是"不传 `--config` 时行为逐字节等于
// S3-01/S2"的根据（ADR-19 第一节那段"没有隐式路由这回事"）：链是空的、或者只有路由那一环
// 没命中，走的都是 reactor 自己的默认目标，而不是"均衡给挑了一个"。
TEST(S3_02_LoadBalanceFilter, UndecidedGroupPassesThroughWithoutWritingTarget) {
  GroupTable table;
  const std::uint16_t group =
      make_group(&table, "svc", Balance::kRoundRobin, {0, 1, 2}, three_keys());
  auto state = std::make_shared<LbState>(3, table.size());
  LoadBalanceFilter filter(table, state);

  RequestInfo info = plain_request("/anything");
  ASSERT_EQ(info.upstream_group, kNoUpstreamGroup);
  ASSERT_EQ(filter.on_request(info), Decision::kContinue);
  EXPECT_EQ(info.upstream_target, kNoUpstreamTarget) << "没人决定过组时这一环不许挑后端";
  EXPECT_EQ(info.reject_status, 0u);
  EXPECT_EQ(state->advance_rr(group, 3), 0u) << "没表态的这一环把轮询游标也推进了：白吃一格";
}

// 组不存在 / 组里没有成员 → 502 + kStop。**不是 404**：404 说"你要的东西我没有路由"，
// 这一格说的是"有路由、路由指的那个组里选不出后端"——那是我的配置坏了。
TEST(S3_02_LoadBalanceFilter, UnknownGroupOrEmptyMembersRejectsFiveOhTwo) {
  GroupTable table;
  std::uint16_t group = kNoUpstreamGroup;
  ASSERT_TRUE(table.add_group("empty", Balance::kRoundRobin, &group));  // 一个成员都没加
  auto state = std::make_shared<LbState>(3, table.size());
  LoadBalanceFilter filter(table, state);

  RequestInfo unknown = plain_request("/x");
  unknown.upstream_group = 9;  // 表里只有组 0
  ASSERT_EQ(filter.on_request(unknown), Decision::kStop);
  EXPECT_EQ(unknown.reject_status, 502u);
  EXPECT_EQ(unknown.upstream_target, kNoUpstreamTarget) << "表白了就不许同时留一个去处";

  RequestInfo empty_members = plain_request("/x");
  empty_members.upstream_group = group;
  ASSERT_EQ(filter.on_request(empty_members), Decision::kStop);
  EXPECT_EQ(empty_members.reject_status, 502u);

  // 502 必须是自产响应表里有货的那个码，否则 `fail_pair_with_reply` 会不发也不计数。
  EXPECT_FALSE(l7gate::http::gateway_error_reply(502).empty());
}

// 一致哈希的组**装配时漏建环**：这一格走的是 `select_from_ring` 返回 nullopt 那条路，
// 它必须回 502 而不是"没选中所以发默认后端"。装配具里 `add_balanced_group` 一定会建环，
// 所以这里手工建一条没环的组——**用例的形状要能表达"漏了这一步"这件事**。
TEST(S3_02_LoadBalanceFilter, HashGroupWithoutRingRejectsFiveOhTwo) {
  GroupTable table;
  std::uint16_t group = kNoUpstreamGroup;
  ASSERT_TRUE(table.add_group("svc", Balance::kConsistentHash, &group));
  ASSERT_TRUE(table.add_member(group, 0));  // 环没建
  auto state = std::make_shared<LbState>(3, table.size());
  LoadBalanceFilter filter(table, state);

  RequestInfo info = plain_request("/orders/1");
  info.upstream_group = group;
  ASSERT_EQ(filter.on_request(info), Decision::kStop);
  EXPECT_EQ(info.reject_status, 502u) << "漏建环退化成默认后端，观测上和'哈希生效'长得一模一样";
}

// 组内顺序就是轮询顺序：这一格断 filter 写出的是**成员下标**（全局目标下标），
// 不是组内偏移——写成偏移的话，第二个组就会从自己的 0 号目标开始，那是另一条静默改道。
TEST(S3_02_LoadBalanceFilter, RoundRobinWritesMemberTargetsInOrder) {
  GroupTable table;
  const std::vector<std::string> keys = l7gate::testing::endpoint_keys_for(4);
  const std::uint16_t group =
      make_group(&table, "svc", Balance::kRoundRobin, {2, 0, 3}, keys);  // 故意不按顺序登记
  auto state = std::make_shared<LbState>(4, table.size());
  LoadBalanceFilter filter(table, state);

  const std::vector<std::uint16_t> expected{2, 0, 3, 2, 0, 3};
  for (std::size_t i = 0; i < expected.size(); ++i) {
    RequestInfo info = plain_request("/x");
    info.upstream_group = group;
    ASSERT_EQ(filter.on_request(info), Decision::kContinue) << i;
    EXPECT_EQ(info.upstream_target, expected[i]) << "第 " << i << " 次写出的不是成员表里那一个目标";
  }
}

// `hash_key: header:<Name>` 的两条语义：大小写不敏感取第一个命中；**头缺失或值为空时
// 退回整条 target**（不是退回空串——空串会把所有没带那个头的请求钉在同一个后端上，
// 那是一次谁也不会去查的哈希倾斜）。
TEST(S3_02_LoadBalanceFilter, HeaderKeyIsCaseInsensitiveAndFallsBackToTarget) {
  HashKey header_key;
  header_key.source = HashKey::Source::kHeaderName;
  header_key.header_name = "x-user-id";
  GroupTable table;
  const std::uint16_t group = make_group(&table, "svc", Balance::kConsistentHash, {0, 1, 2},
                                         three_keys(), kDefaultVnodes, header_key);
  const UpstreamGroup* built = group_of(table, group);
  ASSERT_NE(built, nullptr);
  auto state = std::make_shared<LbState>(3, table.size());
  LoadBalanceFilter filter(table, state);

  const std::vector<l7gate::http::HeaderEntry> headers{
      l7gate::http::HeaderEntry{"X-User-Id", std::string_view("alice")},
  };
  RequestInfo info = plain_request("/whatever");
  info.upstream_group = group;
  info.headers = std::span<const l7gate::http::HeaderEntry>(headers);
  ASSERT_EQ(filter.on_request(info), Decision::kContinue);
  EXPECT_EQ(info.upstream_target, *select_from_ring(*built, "alice"))
      << "没按头的值取 key（或大小写敏感）：同一个用户会跟着 URI 搬家";

  // 头缺失 → 用整条 target 当 key。
  RequestInfo fallback = plain_request("/orders/9");
  fallback.upstream_group = group;
  ASSERT_EQ(filter.on_request(fallback), Decision::kContinue);
  EXPECT_EQ(fallback.upstream_target, *select_from_ring(*built, "/orders/9"))
      << "退回空串会把所有没带头的请求钉在同一个后端上";

  // 值为空也算缺失（同上一条理由）。
  const std::vector<l7gate::http::HeaderEntry> empty_value{
      l7gate::http::HeaderEntry{"x-user-id", std::string_view("")},
  };
  RequestInfo blank = plain_request("/orders/9");
  blank.upstream_group = group;
  blank.headers = std::span<const l7gate::http::HeaderEntry>(empty_value);
  ASSERT_EQ(filter.on_request(blank), Decision::kContinue);
  EXPECT_EQ(blank.upstream_target, fallback.upstream_target) << "空值没有退回 target";
}
