// S4-04（ADR-05 落地追加节）：Redis 分布式计数 = 给本地桶挂一只授权夹子。
//
// 三层判据，各由不同的件负责：
//   夹子的数学（S4_04_BucketCap）——**冻结时钟**下"被夹住"与"本地轨迹没被吃掉"必须同时成
//       立，这是"透镜不是额度"那句话的可执行形式；
//   键口径（S4_04_RedisKey）——kind 与小写头名、定长哈希、以及一个**实测钉住的键名**
//       （换哈希函数就得改这条，正是 ADR-19 给环定的那个形状）；
//   结算线程的状态机（S4_04_Settler）——用 RESP 夹具跑，**不依赖 Docker**：没流量不发命令、
//       授权为 0 时真的少放、失败放开夹子并进降级态、恢复后重新夹上、半途失败的那笔消耗
//       下一拍原样重报、NOSCRIPT 重 LOAD 再问一次。
//
// 真 Redis 上"我的 Lua 算得对不对"不在这个文件里，那是 `tools/l7gate_redis_probe` 对着
// 7.2.5 逐格钉的（ADR-05 追加节第八节）。夹具与探针各管一段，不许互相顶替。
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "filter/rate_limit.h"
#include "filter/redis_rate_limit.h"
#include "test_redis_server.h"

namespace l7gate::filter {
namespace {

std::int64_t steady_ms() noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

template <typename Pred>
bool wait_until(Pred pred, int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return pred();
}

// 读某条键的授权上限。用快照而不是"打一枪看它让不让过"——观测不该改变被测状态
// （S1-04 那次 `EXPECT_EQ(conn.scratch(), nullptr)` 自己把 scratch 建出来是同一类错）。
std::int64_t cap_of(const std::shared_ptr<RateLimiter>& limiter, std::string_view key) {
  for (const RateLimiter::BucketView& view : limiter->snapshot_buckets()) {
    if (!view.overflow && view.key == key) {
      return static_cast<std::int64_t>(view.bucket->cap_milli());
    }
  }
  return -1;
}

RedisSettlerConfig settler_config(const test::TestRedisServer& server) {
  RedisSettlerConfig config;
  config.host = "127.0.0.1";
  config.port = server.port();
  config.settle_ms = 100;
  config.timeout_ms = 1000;
  config.kind_label = "path";
  return config;
}

// ---- 一、夹子的数学 ---------------------------------------------------------------

TEST(S4_04_BucketCap, CapLensRejectsButDoesNotSpendTheBucket) {
  TokenBucket bucket(1, 3);
  bucket.set_cap_milli(500);  // 半个 token：本地满桶也放不出去
  const std::int64_t t0 = 1000000;
  EXPECT_FALSE(bucket.try_acquire(t0));
  EXPECT_EQ(bucket.capped_rejects(), 1u);
  EXPECT_EQ(bucket.used_total(), 0u);
  // **夹子是透镜**：被夹住的那次没扣本地令牌，所以放开之后同一个 instant 仍能放满 burst。
  bucket.release_cap();
  int allowed = 0;
  for (int i = 0; i < 3; ++i) {
    if (bucket.try_acquire(t0)) ++allowed;
  }
  EXPECT_EQ(allowed, 3);
  EXPECT_EQ(bucket.used_total(), 3u);
  // 第四条是真没令牌了：这不算"被夹住"，capped_rejects 不该替它背账。
  EXPECT_FALSE(bucket.try_acquire(t0));
  EXPECT_EQ(bucket.capped_rejects(), 1u);
}

TEST(S4_04_BucketCap, GrantAboveBurstCannotAdmitMore) {
  TokenBucket bucket(1, 3);
  bucket.set_cap_milli(1000000);  // 一个荒谬大的授权
  EXPECT_EQ(bucket.cap_milli(), 3000u);  // 夹到 burst：Redis 说多少都不抬高本地上限
  int allowed = 0;
  const std::int64_t t0 = 1000000;
  for (int i = 0; i < 4; ++i) {
    if (bucket.try_acquire(t0)) ++allowed;
  }
  EXPECT_EQ(allowed, 3);
}

TEST(S4_04_BucketCap, OverflowBucketAndReleaseAreProcessWide) {
  RateLimiter limiter(1, 3);
  const std::int64_t t0 = 1000000;
  EXPECT_TRUE(limiter.try_acquire("/a", t0));
  EXPECT_TRUE(limiter.try_acquire("/b", t0));
  std::vector<RateLimiter::BucketView> views = limiter.snapshot_buckets();
  // 两个键 + 那只溢出桶：快照必须把溢出桶也带上，否则它在 Redis 侧就没有账本。
  ASSERT_EQ(views.size(), 3u);
  EXPECT_TRUE(views.back().overflow);
  for (RateLimiter::BucketView& view : views) view.bucket->set_cap_milli(0);
  EXPECT_EQ(limiter.capped_rejects_total(), 0u);  // 前面两条是放行，不是被夹
  EXPECT_FALSE(limiter.try_acquire("/a", t0));
  EXPECT_EQ(limiter.capped_rejects_total(), 1u);
  limiter.release_all_caps();  // 降级兜底：所有桶（含溢出桶）回到本地语义
  EXPECT_TRUE(limiter.try_acquire("/a", t0));
}

// ---- 二、键口径 -------------------------------------------------------------------

TEST(S4_04_RedisKey, KeyShapeIsPrefixedFixedLengthAndPinned) {
  // 这个后缀是 2026-11-02 在真 Redis 上读到的那一格（探针与冒烟脚本都扫到过它）：
  // 换 fnv1a64 的实现就等于换键空间，必须是一次刻意的改动，不是顺手的重构。
  EXPECT_EQ(redis_bucket_key("path", "/same"), "l7gate:rl:v1:path:28fbd8b1b3664968");
  EXPECT_EQ(redis_bucket_key("hdr:etag", "/same"), "l7gate:rl:v1:hdr:etag:28fbd8b1b3664968");
  // 同一个值在不同 kind 下是两只桶；不同的值必须是两个哈希。
  EXPECT_NE(redis_bucket_key("path", "/same"), redis_bucket_key("hdr:etag", "/same"));
  EXPECT_NE(redis_bucket_key("path", "/same"), redis_bucket_key("path", "/same?x=1"));
  EXPECT_EQ(redis_bucket_key("path", "/a"), redis_bucket_key("path", "/a"));
  EXPECT_EQ(redis_bucket_key("path", ""), redis_bucket_key("path", ""));
}

TEST(S4_04_RedisKey, KindLabelLowercasesHeaderNameBecauseHeaderNamesAreInsensitive) {
  EXPECT_EQ(redis_kind_label(false, "ignored"), "path");
  EXPECT_EQ(redis_kind_label(true, "ETag"), "hdr:etag");
  // `ETag` 与 `etag` 是同一个头（`find_header_value` 就是这么比的），必须落同一只全局桶。
  EXPECT_EQ(redis_bucket_key(redis_kind_label(true, "ETag"), "v7"),
            redis_bucket_key(redis_kind_label(true, "etag"), "v7"));
  EXPECT_EQ(std::string(kRateLimitOverflowKey), "l7gate:rl:v1:ovf");
}

// ---- 三、结算线程的状态机（夹具，不碰 Docker）------------------------------------

TEST(S4_04_Settler, IdleBucketsCostNoCommands) {
  test::TestRedisServer server;
  std::string why;
  ASSERT_TRUE(server.start(0, &why)) << why;
  auto limiter = std::make_shared<RateLimiter>(100, 5);
  RedisSettler settler(limiter, settler_config(server));
  settler.start();
  ASSERT_TRUE(wait_until([&] { return settler.settles_ok() >= 3u; }, 5000));
  EXPECT_FALSE(settler.degraded());
  // 一条流量都没有 → 一拍 EVALSHA 都不该发。这条断言守的是"16384 个冷键 ≠ 16384 次命令"。
  EXPECT_EQ(server.count_cmd("evalsha"), 0u);
  EXPECT_EQ(settler.settle_commands(), 0u);
  EXPECT_EQ(settler.settles_failed(), 0u);
  settler.stop();
  server.stop();
}

TEST(S4_04_Settler, DemandedBucketIsSettledWithItsUsageAndTheGrantBecomesTheCap) {
  test::TestRedisServer server;
  std::string why;
  ASSERT_TRUE(server.start(0, &why)) << why;
  auto limiter = std::make_shared<RateLimiter>(1, 3);
  RedisSettler settler(limiter, settler_config(server));
  settler.start();
  EXPECT_TRUE(limiter->try_acquire("/a", steady_ms()));
  EXPECT_TRUE(limiter->try_acquire("/a", steady_ms()));
  ASSERT_TRUE(wait_until([&] { return settler.settle_commands() >= 1u; }, 5000));
  EXPECT_EQ(server.last_eval_used(), "2");
  // 默认授权"很宽"（夹具给的数远大于 burst）→ 夹子被夹到 burst，等价于不夹。
  EXPECT_EQ(cap_of(limiter, "/a"), 3000);
  const std::size_t evals = server.count_cmd("evalsha");
  ASSERT_EQ(evals, 1u);
  // **冷桶不花钱**：结掉的那条键之后没有任何新流量，接下来三拍不该再发一条 EVALSHA。
  // （16384 个键的表如果每拍全表扫着问，这一条就会红。）
  std::this_thread::sleep_for(std::chrono::milliseconds(350));
  EXPECT_EQ(server.count_cmd("evalsha"), evals);
  // 第二个键出现时，才多出一条命令，而且命令里带的就是那一格的键名（键口径真的上了线）。
  EXPECT_TRUE(limiter->try_acquire("/b", steady_ms()));
  ASSERT_TRUE(wait_until([&] { return server.count_cmd("evalsha") >= evals + 1; }, 5000));
  bool saw_b_key = false;
  for (const test::TestRedisServer::Seen& seen : server.commands()) {
    if (seen.cmd == "evalsha" && seen.args.size() >= 7 &&
        seen.args[2] == redis_bucket_key("path", "/b")) {
      saw_b_key = true;
    }
  }
  EXPECT_TRUE(saw_b_key) << "EVALSHA 的 KEYS[1] 不是 /b 那格的键名";
  settler.stop();
  server.stop();
}

TEST(S4_04_Settler, GrantOfZeroBitesAndTheRejectionItCausesRefreshesIt) {
  test::TestRedisServer server;
  std::string why;
  ASSERT_TRUE(server.start(0, &why)) << why;
  auto limiter = std::make_shared<RateLimiter>(1, 3);
  RedisSettler settler(limiter, settler_config(server));
  settler.start();
  // 先制造一次结算（有一条放行），再把授权改成 0。
  EXPECT_TRUE(limiter->try_acquire("/a", steady_ms()));
  ASSERT_TRUE(wait_until([&] { return settler.settle_commands() >= 1u; }, 5000));
  server.set_grant(0);
  EXPECT_TRUE(limiter->try_acquire("/a", steady_ms()));  // 这一条放行 = 又一次需求
  ASSERT_TRUE(wait_until([&] { return cap_of(limiter, "/a") == 0; }, 5000));
  // 夹子咬下去：本地桶还有余粮（burst=3 只用了 2、还在按 rate 补），也必须被拒。
  EXPECT_FALSE(limiter->try_acquire("/a", steady_ms()));
  EXPECT_EQ(limiter->capped_rejects_total(), 1u);
  // **被夹住本身就是需求信号**：下一拍会重结算。把授权放开，它必须自己活过来——
  // 这条断言挡住的是"陈旧的低授权把键永久饿死"那一类失效。
  server.set_grant(1000000);
  ASSERT_TRUE(wait_until([&] { return limiter->try_acquire("/a", steady_ms()); }, 5000));
  settler.stop();
  server.stop();
}

TEST(S4_04_Settler, FailureOpensTheCapAndRecoversReclampingIt) {
  test::TestRedisServer server;
  std::string why;
  ASSERT_TRUE(server.start(0, &why)) << why;
  auto limiter = std::make_shared<RateLimiter>(1, 3);
  RedisSettler settler(limiter, settler_config(server));
  settler.start();
  // 第一段：授权 0 → 夹子收紧（这一格先钉住"cap 确实被谁动过"，后面"放开"才有含义）。
  server.set_grant(0);
  EXPECT_TRUE(limiter->try_acquire("/a", steady_ms()));
  ASSERT_TRUE(wait_until([&] { return cap_of(limiter, "/a") == 0; }, 5000));
  EXPECT_FALSE(settler.degraded());
  // 第二段：Redis 持续不可达 → 降级 + **夹子全放开**，兜底就是 S4-03 的本地语义。
  server.set_refuse_evalsha(true);
  EXPECT_FALSE(limiter->try_acquire("/a", steady_ms()));  // 被夹住的那一条 = 需求信号
  ASSERT_TRUE(wait_until([&] { return settler.settles_failed() >= 1u; }, 5000));
  EXPECT_TRUE(settler.degraded());  // 持续故障下这个读数是稳态，不靠采样时机
  ASSERT_TRUE(wait_until([&] { return cap_of(limiter, "/a") == 3000; }, 5000));
  EXPECT_TRUE(limiter->try_acquire("/a", steady_ms()));  // 拒绝能力按配置阈值继续
  // 第三段：Redis 回来 → 降级态自己摘掉、夹子按同一份授权重新咬住。
  server.set_refuse_evalsha(false);
  ASSERT_TRUE(wait_until(
      [&] { return !settler.degraded() && cap_of(limiter, "/a") == 0; }, 5000));
  EXPECT_FALSE(limiter->try_acquire("/a", steady_ms()));
  settler.stop();
  server.stop();
}

TEST(S4_04_Settler, FailedReportIsRetriedWithTheSameUsageNextTick) {
  test::TestRedisServer server;
  std::string why;
  ASSERT_TRUE(server.start(0, &why)) << why;
  auto limiter = std::make_shared<RateLimiter>(1, 5);
  RedisSettler settler(limiter, settler_config(server));
  settler.start();
  server.set_refuse_evalsha(true);
  for (int i = 0; i < 3; ++i) ASSERT_TRUE(limiter->try_acquire("/a", steady_ms()));
  ASSERT_TRUE(wait_until([&] { return settler.settles_failed() >= 1u; }, 5000));
  EXPECT_EQ(server.last_eval_used(), "3");  // 被打断的那次带着 3
  server.set_refuse_evalsha(false);
  ASSERT_TRUE(wait_until([&] { return settler.settles_ok() >= 1u; }, 5000));
  // **先记账后失败**会把这笔消耗丢掉，重报的就是 0（Redis 于是多授权——方向不安全）。
  EXPECT_EQ(server.last_eval_used(), "3");
  // 成功之后这笔就结掉了：后面几拍不许再报第二遍（重复扣减的另一种失效）。
  const std::size_t evals = server.count_cmd("evalsha");
  EXPECT_GE(evals, 2u);
  std::this_thread::sleep_for(std::chrono::milliseconds(350));
  EXPECT_EQ(server.count_cmd("evalsha"), evals);
  settler.stop();
  server.stop();
}

TEST(S4_04_Settler, NoscriptReloadsTheScriptAndAsksOnceMore) {
  test::TestRedisServer server;
  std::string why;
  ASSERT_TRUE(server.start(0, &why)) << why;
  auto limiter = std::make_shared<RateLimiter>(1, 3);
  RedisSettler settler(limiter, settler_config(server));
  settler.start();
  EXPECT_TRUE(limiter->try_acquire("/a", steady_ms()));  // 制造需求
  ASSERT_TRUE(wait_until([&] { return settler.settles_ok() >= 1u; }, 5000));
  server.set_noscript();
  EXPECT_TRUE(limiter->try_acquire("/a", steady_ms()));  // 又一次需求
  // 等的是"重 LOAD 真的发生了"这个条件，不是读一次计数：NOSCRIPT、重 LOAD、重试三条命令
  // 落在同一拍的几微秒里，先读到哪个数全看调度——那是"绿了也不说明什么"的用例。
  ASSERT_TRUE(wait_until([&] { return server.count_cmd("script") >= 2u; }, 5000));
  // NOSCRIPT 是**这一拍内部**处理掉的：不该把整拍判失败，也不该进降级计数。
  EXPECT_EQ(settler.settles_failed(), 0u);
  EXPECT_FALSE(settler.degraded());
  EXPECT_GE(server.count_cmd("evalsha"), 2u);  // 被挡那条 + 重试那条
  EXPECT_EQ(cap_of(limiter, "/a"), 3000);
  settler.stop();
  server.stop();
}

}  // namespace
}  // namespace l7gate::filter
