// S4-03（ADR-21 第二/四节）：本地令牌桶限流。
//
// 这一批用例分两层，判据不同：
//   桶的数学（S4_03_TokenBucket）——**冻结时钟**下放行/拒绝的序列必须逐条对，
//       补货速率、burst 上界、拒绝不消耗、键隔离、键数上溢都各有一条；并发那一条
//       用 16 线程 × 200 次断言"共享桶的总放行恰好是 burst，不是 16×burst"。
//   filter 的接线（S4_03_RateLimitFilter）——键怎么取、拒绝时写什么状态码；
//       "429 不回源 / 计数怎么记"在 reactor_test.cc 的端到端那两条里。
//
// 时钟是显式参数而不是 filter 自己读表：桶的所有断言都必须能在同一毫秒里冻结，
// 否则"补了 999ms 还不够一个令牌"这种边界会被调度抖动吃掉。
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "filter/rate_limit.h"
#include "http/request_parser.h"

namespace l7gate::filter {
namespace {

using http::HeaderEntry;

RequestInfo plain_request(std::string_view target,
                          std::span<const HeaderEntry> headers = {}) {
  RequestInfo info;
  info.method = "GET";
  info.target = target;
  info.headers = headers;
  info.request_bytes = 40;
  info.downstream_fd = 7;
  return info;
}

}  // namespace

// 一、burst 是上界、补货按 rate 走。rate=1/s、burst=3：
//    同一毫秒 3 放 1 拒；999ms 后仍拒（差 1 毫 token）；再 1ms 放 1；闲置 10s 也只补到 3。
TEST(S4_03_TokenBucket, BurstIsTheCeilingAndRefillFollowsTheRate) {
  RateLimiter limiter(1, 3);
  const std::int64_t t0 = 100000;
  EXPECT_TRUE(limiter.try_acquire("k", t0));
  EXPECT_TRUE(limiter.try_acquire("k", t0));
  EXPECT_TRUE(limiter.try_acquire("k", t0));
  EXPECT_FALSE(limiter.try_acquire("k", t0)) << "burst 之后还放行：桶没有上界";
  EXPECT_FALSE(limiter.try_acquire("k", t0 + 999))
      << "999ms 就补出了一个令牌：补货速率不是 rate/s";
  EXPECT_TRUE(limiter.try_acquire("k", t0 + 1000)) << "1 秒补不出 1 个令牌";
  EXPECT_TRUE(limiter.try_acquire("k", t0 + 11000));
  EXPECT_TRUE(limiter.try_acquire("k", t0 + 11000));
  EXPECT_TRUE(limiter.try_acquire("k", t0 + 11000));
  EXPECT_FALSE(limiter.try_acquire("k", t0 + 11000)) << "闲置再久也补不出第 4 个";
  EXPECT_EQ(limiter.allowed_total(), 7u);
  EXPECT_EQ(limiter.rejected_total(), 3u);
}

// 二、键之间互不拖累：/a 的额度用完不影响 /b。
TEST(S4_03_TokenBucket, KeysHaveIndependentBuckets) {
  RateLimiter limiter(1, 2);
  EXPECT_TRUE(limiter.try_acquire("/a", 5));
  EXPECT_TRUE(limiter.try_acquire("/a", 5));
  EXPECT_FALSE(limiter.try_acquire("/a", 5));
  EXPECT_TRUE(limiter.try_acquire("/b", 5)) << "另一个键被 /a 的额度拖累";
  EXPECT_TRUE(limiter.try_acquire("/b", 5));
  EXPECT_FALSE(limiter.try_acquire("/b", 5));
  EXPECT_EQ(limiter.key_count(), 2u);
}

// 三、**共享桶在并发下不放大**：16 线程 × 200 次、时钟冻结（rate=1/s，进程内几毫秒
//    补不出一个令牌），总放行必须恰好 = burst。per-worker 桶或"两个原子拼状态"的过补
//    都会在这里露馅（前者 16×1000，后者随机多放）。
TEST(S4_03_TokenBucket, ConcurrentAcquireIsExactAcrossThreads) {
  RateLimiter limiter(1, 1000);
  constexpr int kThreads = 16;
  constexpr int kPerThread = 200;
  std::atomic<int> allowed{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&] {
      for (int j = 0; j < kPerThread; ++j) {
        if (limiter.try_acquire("hot", 1000)) allowed.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (std::thread& th : threads) th.join();
  EXPECT_EQ(allowed.load(std::memory_order_relaxed), 1000)
      << "共享桶被并发放大或缩水：CAS 不是全局准确的";
  EXPECT_EQ(limiter.allowed_total(), 1000u);
  EXPECT_EQ(limiter.rejected_total(),
            static_cast<std::uint64_t>(kThreads * kPerThread - 1000));
}

// 四、键数硬上界（ADR-21 第四节）：第 kMaxKeys+1 个键起共用溢出桶，表不再增长。
TEST(S4_03_TokenBucket, KeysBeyondTheCapShareOneOverflowBucket) {
  RateLimiter limiter(1, 1);
  for (std::size_t i = 0; i < RateLimiter::kMaxKeys; ++i) {
    ASSERT_TRUE(limiter.try_acquire("k" + std::to_string(i), 0)) << i;
  }
  EXPECT_EQ(limiter.key_count(), RateLimiter::kMaxKeys);
  EXPECT_TRUE(limiter.try_acquire("overflow-a", 0)) << "溢出桶的初始满桶应当放行一次";
  EXPECT_FALSE(limiter.try_acquire("overflow-b", 0)) << "溢出键共享额度：第二个必须被拒";
  EXPECT_EQ(limiter.overflow_keys(), 2u);
  EXPECT_EQ(limiter.key_count(), RateLimiter::kMaxKeys) << "溢出键不该再往表里长";
}

// 五、filter 的接线：path 是缺省键，拒绝写 429 + kStop。
TEST(S4_03_RateLimitFilter, PathKeyIsTheDefault) {
  auto limiter = std::make_shared<RateLimiter>(1, 1);
  RateLimitFilter filter(limiter, RateLimitKeySpec{});
  RequestInfo first = plain_request("/x");
  EXPECT_EQ(filter.on_request(first), Decision::kContinue);
  EXPECT_EQ(first.reject_status, 0u);
  RequestInfo second = plain_request("/x");
  EXPECT_EQ(filter.on_request(second), Decision::kStop);
  EXPECT_EQ(second.reject_status, 429u);
  RequestInfo other = plain_request("/y");
  EXPECT_EQ(filter.on_request(other), Decision::kContinue) << "不同路径不许共享 path 键的额度";
}

// 六、header 键：大小写不敏感取第一个值；头缺失/为空退回 target（与 hash_key 同口径）。
TEST(S4_03_RateLimitFilter, HeaderKeyUsesFirstValueAndFallsBackToTarget) {
  auto limiter = std::make_shared<RateLimiter>(1, 1);
  RateLimitKeySpec spec;
  spec.source = RateLimitKeySpec::Source::kHeader;
  spec.header_name = "X-Key";
  RateLimitFilter filter(limiter, spec);

  const std::vector<HeaderEntry> alpha{{"X-Key", "alpha"}};
  RequestInfo a1 = plain_request("/p", alpha);
  EXPECT_EQ(filter.on_request(a1), Decision::kContinue);
  RequestInfo a2 = plain_request("/p", alpha);
  EXPECT_EQ(filter.on_request(a2), Decision::kStop);

  const std::vector<HeaderEntry> beta{{"x-key", "beta"}};  // 头名大小写不敏感
  RequestInfo b1 = plain_request("/p", beta);
  EXPECT_EQ(filter.on_request(b1), Decision::kContinue) << "beta 的额度被 alpha 吃掉了";

  RequestInfo q1 = plain_request("/q");  // 没有 X-Key → 键 = target
  EXPECT_EQ(filter.on_request(q1), Decision::kContinue);
  RequestInfo q2 = plain_request("/q");
  EXPECT_EQ(filter.on_request(q2), Decision::kStop) << "无头请求没有退回 target 键";
}

// 七、没装限流（limiter=nullptr）＝放行且不写任何字段——与空链同形。
TEST(S4_03_RateLimitFilter, NullLimiterIsPassThrough) {
  RateLimitFilter filter(nullptr, RateLimitKeySpec{});
  RequestInfo info = plain_request("/x");
  EXPECT_EQ(filter.on_request(info), Decision::kContinue);
  EXPECT_EQ(info.reject_status, 0u);
  EXPECT_EQ(info.upstream_target, route::kNoUpstreamTarget);
}

}  // namespace l7gate::filter
