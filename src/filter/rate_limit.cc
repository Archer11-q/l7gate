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

TokenBucket::TokenBucket(std::uint64_t rate_per_sec, std::uint64_t burst,
                         std::atomic<std::uint64_t>* limiter_capped) noexcept
    : burst_milli_(burst * kOneTokenMilli),
      // 1 token/秒 = 1000 毫 token / 1000 毫秒 = `rate` 毫 token 每毫秒。
      // rate 是整数（配置层只认整数），所以这里没有取整误差。
      rate_milli_per_ms_(rate_per_sec),
      limiter_capped_(limiter_capped) {
  // 初始满桶：last_ms = 0、tokens = burst。第一次调用时 `elapsed` 会把 tokens
  // 重新夹到 burst——"刚启动就按 burst 放行"是令牌桶的定义，不是补课。
  state_.store(static_cast<std::uint64_t>(burst_milli_), std::memory_order_relaxed);
  // S4-04：缺省不夹（= burst），也就是 S4-03 的纯本地语义。结算线程成功一拍才收紧它。
  cap_milli_.store(burst_milli_, std::memory_order_relaxed);
}

void TokenBucket::set_cap_milli(std::uint64_t cap_milli) noexcept {
  // 授权永不使本地多放：cap 高于 burst 没有意义（本地自己就夹在 burst），取小。
  cap_milli_.store(cap_milli < burst_milli_ ? cap_milli : burst_milli_, std::memory_order_relaxed);
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
    // **夹子是透镜不是额度**：只在这次判定里把可见的 tokens 压低，写回的仍是本地轨迹的
    // `tokens - 1000`。所以 Redis 授权变高时本地立刻跟得上、变低时只会少放。
    const std::uint64_t cap = cap_milli_.load(std::memory_order_relaxed);
    const std::uint64_t usable = tokens < cap ? tokens : cap;
    if (usable < kOneTokenMilli) {
      // 拒绝不写回：懒补货，下次按同一个 last 重算。
      if (tokens >= kOneTokenMilli) {
        capped_rejects_.fetch_add(1, std::memory_order_relaxed);
        // S5-01（ADR-24 第三节）：同拍给 limiter 级聚合 +1。写这一格的条件与上面完全
        // 一致（只有"本地有 token 但被夹子压掉"才算），所以两条读法天然同源；
        // 它在快路径上的成本是**零**——不夹、放行、本地耗尽三种情况都走不到这里。
        if (limiter_capped_ != nullptr) {
          limiter_capped_->fetch_add(1, std::memory_order_relaxed);
        }
      }
      return false;
    }
    const std::uint64_t next =
        (static_cast<std::uint64_t>(now) << 32) | (tokens - kOneTokenMilli);
    if (state_.compare_exchange_weak(old, next, std::memory_order_relaxed)) {
      used_total_.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
  }
}

RateLimiter::RateLimiter(std::uint64_t rate_per_sec, std::uint64_t burst)
    : rate_per_sec_(rate_per_sec),
      burst_(burst),
      overflow_(std::make_unique<TokenBucket>(rate_per_sec, burst, &capped_rejects_aggregate_)) {}

TokenBucket* RateLimiter::bucket_for(std::string_view key) noexcept {
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = buckets_.find(key);  // 异构查找：不为查表分配
  if (it != buckets_.end()) return it->second.get();
  if (buckets_.size() < kMaxKeys) {
    auto bucket = std::make_unique<TokenBucket>(rate_per_sec_, burst_,
                                                &capped_rejects_aggregate_);
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

std::vector<RateLimiter::BucketView> RateLimiter::snapshot_buckets() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<BucketView> views;
  views.reserve(buckets_.size() + 1);
  for (const auto& entry : buckets_) {
    views.push_back(BucketView{entry.first, entry.second.get(),
                               entry.second->used_total(), false});
  }
  views.push_back(BucketView{std::string{}, overflow_.get(), overflow_->used_total(), true});
  return views;
}

void RateLimiter::release_all_caps() noexcept {
  std::lock_guard<std::mutex> lock(mu_);
  for (auto& entry : buckets_) entry.second->release_cap();
  overflow_->release_cap();
}

std::uint64_t RateLimiter::capped_rejects_total() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::uint64_t sum = overflow_->capped_rejects();
  for (const auto& entry : buckets_) sum += entry.second->capped_rejects();
  return sum;
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
