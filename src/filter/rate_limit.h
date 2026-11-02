#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "filter/chain.h"

// 本地令牌桶限流（S4-03，ADR-21 第二/四节）。
//
// 两层的分工写死在这里：
//   `TokenBucket` —— **状态**：一个桶的令牌余额与上次补货时刻，打包在**一个 64 位原子**里
//                  （高 32 位 = 单调毫秒的低 32 位，低 32 位 = 毫 token），CAS 补货 + 扣减。
//                  两个原子（余额一个、时刻一个）做 CAS 有一个已知的过补窗口：赢家在两次
//                  写之间被输家读到"新余额 + 旧时刻"，输家按旧时刻多补一截。打包成一个字
//                  之后 CAS 是单点，没有这个窗口。
//   `RateLimiter` —— **键 → 桶**：per-key 的桶表 + 一只溢出桶。表用 mutex 保护（临界区
//                  只有一次哈希查找），桶的准入在锁外做——"共享原子"约束的是桶状态，
//                  不是这张表。**per-worker 桶会把限额放大 N 倍，那是装饰不是限流**，
//                  所以这张表在 main.cc 建一次、每条 worker 的 filter 拿同一份。
//
// 键集（ADR-21 第四节）：`path`（缺省，原始 origin-form 含 query）或 `header:<名字>`。
// `ip` 不在本阶段：`RequestInfo` 没有对端地址，加它要动 `sizeof(Connection)` 的 528。
namespace l7gate::filter {

class TokenBucket {
 public:
  TokenBucket(std::uint64_t rate_per_sec, std::uint64_t burst) noexcept;

  // 有令牌就扣一个并返回 true；没有就返回 false。**拒绝不写回状态**——补货是懒的，
  // 下一次调用按同一个 last 重算，结果一样，少一次 CAS。
  bool try_acquire(std::int64_t now_ms) noexcept;

  // S4-04（ADR-05 追加节第二节）：**授权上限是一只透镜，不是第二份额度**。
  // 结算线程把 Redis 返回的"全局剩余毫 token"写进来，准入时在 CAS 循环内
  // `min(tokens, cap)` 夹一道；本地补货的轨迹不因夹取而改变（写回的仍是扣减后的
  // `tokens`），所以 cap 只会让放行**变少**、不会变多。缺省 = `burst` = 不夹，
  // 也就是 S4-03 的纯本地语义——这正是降级兜底的样子：Redis 挂了就没人再写它。
  void set_cap_milli(std::uint64_t cap_milli) noexcept;
  void release_cap() noexcept { cap_milli_.store(burst_milli_, std::memory_order_relaxed); }
  std::uint64_t cap_milli() const noexcept { return cap_milli_.load(std::memory_order_relaxed); }

  // 自进程启动以来这条桶**放行**的条数（单调、不清零）。结算线程按"上次上报值之后的
  // 差量"报给 Redis，所以清零会把两次结算之间的消耗丢掉——那是过授，方向不安全。
  std::uint64_t used_total() const noexcept { return used_total_.load(std::memory_order_relaxed); }
  // "本地补货够、但被夹子按下去"的那一格拒绝——夹子真的咬过人的证据（S4-04 验收）。
  std::uint64_t capped_rejects() const noexcept {
    return capped_rejects_.load(std::memory_order_relaxed);
  }

 private:
  static constexpr std::uint64_t kOneTokenMilli = 1000;
  // 打包字：高 32 = last_ms（低 32 位）、低 32 = 毫 token。burst ≤ 1e6 → 1e9 毫单位，
  // 装得下；rate ≤ 1e6/s 时每毫秒补 rate 个毫 token，`rate * elapsed` 在 uint64 里不会溢出。
  std::atomic<std::uint64_t> state_{0};
  std::uint64_t burst_milli_ = 0;
  std::uint64_t rate_milli_per_ms_ = 0;
  std::atomic<std::uint64_t> cap_milli_{0};
  std::atomic<std::uint64_t> used_total_{0};
  std::atomic<std::uint64_t> capped_rejects_{0};
};

class RateLimiter {
 public:
  // 键数硬上界（ADR-21 第四节）：`header:` 键的基数由客户端控制，没有上界就是内存 DoS。
  // 满了之后新键共用 `overflow_` 这一只桶——限流仍然生效，粒度退化为"溢出键共享一份额度"。
  static constexpr std::size_t kMaxKeys = 16384;

  RateLimiter(std::uint64_t rate_per_sec, std::uint64_t burst);

  bool try_acquire(std::string_view key, std::int64_t now_ms) noexcept;

  // 观测面（测试、退出报告）：三格与桶状态同源，不由外部再数一份。
  std::uint64_t allowed_total() const noexcept { return allowed_.load(std::memory_order_relaxed); }
  std::uint64_t rejected_total() const noexcept {
    return rejected_.load(std::memory_order_relaxed);
  }
  std::uint64_t overflow_keys() const noexcept {
    return overflow_keys_.load(std::memory_order_relaxed);
  }
  std::size_t key_count() const noexcept;
  std::uint64_t rate_per_sec() const noexcept { return rate_per_sec_; }
  std::uint64_t burst() const noexcept { return burst_; }

  // ---- S4-04（ADR-05 追加节）：结算线程用的三面 ------------------------------------
  // 一条桶的可见快照。`bucket` 的指针稳定性来自"桶不回收"（ADR-21 第四节第 4 条），
  // 溢出桶也在同一份里（`overflow=true`、`key` 为空串）——它的额度在 Redis 侧也共用一格。
  struct BucketView {
    std::string key;
    TokenBucket* bucket = nullptr;
    std::uint64_t used_total = 0;
    bool overflow = false;
  };
  // 取快照 = 一次 mutex 内的浅拷贝；**Redis 的往返必须在快照之后、锁之外做**——
  // 拿着桶表的锁去做阻塞 IO，就把限流的热点路径交给了一个外部依赖。
  std::vector<BucketView> snapshot_buckets() const;
  // 降级兜底：把所有桶（含溢出桶）的 cap 放回 `burst`，语义退回 S4-03 的纯本地桶。
  void release_all_caps() noexcept;
  // 逐桶求和（只在退出报告与用例里调，它要拿桶表的锁）——不在快路径上加一个 limiter 级
  // 原子，是因为 `try_acquire` 返回 bool、快路径拿不到"被夹住"这个原因，而加一个没人读的
  // 枚举返回值等于发空头证明。
  std::uint64_t capped_rejects_total() const;

 private:
  // C++20 的异构查找：请求路径上不为"查表"分配一个 std::string。
  struct KeyHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view text) const noexcept {
      return std::hash<std::string_view>{}(text);
    }
  };
  struct KeyEqual {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept { return a == b; }
  };

  TokenBucket* bucket_for(std::string_view key) noexcept;

  const std::uint64_t rate_per_sec_;
  const std::uint64_t burst_;
  // mutable：`key_count()` 是 const 观测接口，但它要读表。
  mutable std::mutex mu_;
  std::unordered_map<std::string, std::unique_ptr<TokenBucket>, KeyHash, KeyEqual> buckets_;
  std::unique_ptr<TokenBucket> overflow_;
  std::atomic<std::uint64_t> allowed_{0};
  std::atomic<std::uint64_t> rejected_{0};
  std::atomic<std::uint64_t> overflow_keys_{0};
};

// 键的来源。配置层有自己的 `RateLimitSection`（config 不认识 filter），
// 装配方做一次字段搬运——同 `--config` 与 `--upstream` 的切分一个道理。
struct RateLimitKeySpec {
  enum class Source : std::uint8_t { kPath, kHeader };
  Source source = Source::kPath;
  std::string header_name;  // 仅 kHeader 有值
};

class RateLimitFilter : public RequestFilter {
 public:
  RateLimitFilter(std::shared_ptr<RateLimiter> limiter, RateLimitKeySpec spec) noexcept;

  std::string_view name() const noexcept override { return "rate_limit"; }

  // 超速率 → `kStop + reject_status=429`（ADR-21 第二节第 3 条）：
  // 走 S3-01 的写回通道与 reactor 的 `fail_pair_with_reply`，不回源。
  Decision on_request(RequestInfo& info) override;

 private:
  std::string_view key_of(const RequestInfo& info) const noexcept;

  std::shared_ptr<RateLimiter> limiter_;
  RateLimitKeySpec spec_;
};

}  // namespace l7gate::filter
