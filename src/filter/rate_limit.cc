#include "filter/rate_limit.h"

#include <chrono>
#include <utility>

namespace l7gate::filter {
namespace {

std::int64_t steady_now_ms() noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

TokenBucket::TokenBucket(std::uint64_t rate_per_sec, std::uint64_t burst) noexcept
    : burst_milli_(burst * kOneTokenMilli),
      // 1 token/秒 = 1000 毫 token / 1000 毫秒 = `rate` 毫 token 每毫秒。
      // rate 是整数（配置层只认整数），所以这里没有取整误差。
      rate_milli_per_ms_(rate_per_sec) {
  // 初始满桶：last_ms = 0、tokens = burst。第一次调用时 `elapsed` 会把 tokens
  // 重新夹到 burst——"刚启动就按 burst 放行"是令牌桶的定义，不是补课。
  state_.store(static_cast<std::uint64_t>(burst_milli_), std::memory_order_relaxed);
}

bool TokenBucket::try_acquire(std::int64_t now_ms) noexcept {
  const std::uint32_t now = static_cast<std::uint32_t>(now_ms);
  for (;;) {
    std::uint64_t old = state_.load(std::memory_order_relaxed);
    const std::uint32_t last = static_cast<std::uint32_t>(old >> 32);
    std::uint64_t tokens = old & 0xffffffffULL;
    // 无符号相减对 32 位毫秒回绕是安全的（49.7 天一圈）；真的一次 idle 超过半圈，
    // elapsed 会是个巨大的数 → 补到 burst，语义正好是"这么久了桶当然是满的"。
    const std::uint64_t elapsed = static_cast<std::uint32_t>(now - last);
    if (elapsed != 0) {
      tokens += rate_milli_per_ms_ * elapsed;
      if (tokens > burst_milli_) tokens = burst_milli_;
    }
    if (tokens < kOneTokenMilli) return false;  // 拒绝不写回：懒补货，下次按同一个 last 重算
    const std::uint64_t next =
        (static_cast<std::uint64_t>(now) << 32) | (tokens - kOneTokenMilli);
    if (state_.compare_exchange_weak(old, next, std::memory_order_relaxed)) return true;
  }
}

RateLimiter::RateLimiter(std::uint64_t rate_per_sec, std::uint64_t burst)
    : rate_per_sec_(rate_per_sec),
      burst_(burst),
      overflow_(std::make_unique<TokenBucket>(rate_per_sec, burst)) {}

TokenBucket* RateLimiter::bucket_for(std::string_view key) noexcept {
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = buckets_.find(key);  // 异构查找：不为查表分配
  if (it != buckets_.end()) return it->second.get();
  if (buckets_.size() < kMaxKeys) {
    auto bucket = std::make_unique<TokenBucket>(rate_per_sec_, burst_);
    TokenBucket* raw = bucket.get();
    buckets_.emplace(std::string(key), std::move(bucket));
    return raw;
  }
  overflow_keys_.fetch_add(1, std::memory_order_relaxed);
  return overflow_.get();
}

bool RateLimiter::try_acquire(std::string_view key, std::int64_t now_ms) noexcept {
  // 锁只护查表；准入（CAS）在锁外。桶的生命周期 = RateLimiter 的生命周期（不回收），
  // 所以锁外拿着裸指针是安全的——这条和"键数上界不回收"是同一个决定的正面与代价。
  TokenBucket* bucket = bucket_for(key);
  if (bucket->try_acquire(now_ms)) {
    allowed_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  rejected_.fetch_add(1, std::memory_order_relaxed);
  return false;
}

std::size_t RateLimiter::key_count() const noexcept {
  std::lock_guard<std::mutex> lock(mu_);
  return buckets_.size();
}

RateLimitFilter::RateLimitFilter(std::shared_ptr<RateLimiter> limiter,
                                 RateLimitKeySpec spec) noexcept
    : limiter_(std::move(limiter)), spec_(std::move(spec)) {}

Decision RateLimitFilter::on_request(RequestInfo& info) {
  if (limiter_ == nullptr) return Decision::kContinue;  // 没装限流：与空链同形
  if (limiter_->try_acquire(key_of(info), steady_now_ms())) return Decision::kContinue;
  info.reject_status = 429;
  return Decision::kStop;
}

std::string_view RateLimitFilter::key_of(const RequestInfo& info) const noexcept {
  if (spec_.source == RateLimitKeySpec::Source::kHeader && !spec_.header_name.empty()) {
    const std::optional<std::string_view> value =
        find_header_value(info.headers, spec_.header_name);
    // 头缺失或为空 → 退回整条 target（与 hash_key 的既有口径同一条）：
    // 空串当键会把所有没带头的请求钉进同一只桶，那是"限了一个不该限的集合"。
    if (value.has_value() && !value->empty()) return *value;
  }
  return info.target;
}

}  // namespace l7gate::filter
