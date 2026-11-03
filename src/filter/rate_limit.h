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
  // `limiter_capped`（S5-01，ADR-24 第三节）：可选的 limiter 级聚合槽，只在"夹子咬到"
  // 那一格跟着 +1。**为什么需要它**：`/metrics` 是 pull 的，而原来这一键只有
  // `capped_rejects_total()` 那条持桶表锁的全表走查能取到——面板上每次抓取都要在
  // 每条请求都要拿的那把锁里走 16384 格，那是仪器在被测方身上踩成本。
  // 传 nullptr 就是不要聚合（单测直接构造裸桶走这条路）。
  TokenBucket(std::uint64_t rate_per_sec, std::uint64_t burst,
              std::atomic<std::uint64_t>* limiter_capped = nullptr) noexcept;

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
  // S5-01：指向上面那只 limiter 级聚合槽（可为空）。裸指针不拥有任何东西，
  // 生命周期由 `RateLimiter` 的成员声明顺序保证（聚合槽声明在桶表之前，因此晚于它们析构）。
  std::atomic<std::uint64_t>* limiter_capped_ = nullptr;
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
  // 逐桶求和（只在退出报告与用例里调，它要拿桶表的锁）。
  // **这里曾经是"故意不加 limiter 级原子"**（S4-04 的原话：快路径拿不到"被夹住"这个
  // 原因，加一个没人读的枚举返回值等于发空头证明）。S5-01 把这句话改成现在这样：
  // pull 模型下"只有退出才有"等于面板上永远没有这一格，而全表走查要在**每条请求都要拿**
  // 的那把锁里走 16384 格。原理由仍然成立，所以**没有**改 `try_acquire` 的返回类型——
  // 聚合是在桶内部那个已经知道原因的分支里 +1 的（`rate_limit.cc` 的 `usable < 1000`
  // 且 `tokens >= 1000` 那一格），快路径一个字没多。代价与取证见 ADR-24 第三节。
  std::uint64_t capped_rejects_total() const;
  // S5-01（ADR-24 第三节）：同一格的**免锁**读法，给 `/metrics` 用。两条路必须给同一个
  // 数——`S5_01_RateLimitAccounts` 那条用例就是钉这件事的，走查与聚合一旦漂移它就红。
  std::uint64_t capped_rejects_now() const noexcept {
    return capped_rejects_aggregate_.load(std::memory_order_relaxed);
  }

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
  // S5-01：limiter 级聚合。**声明必须在 buckets_ / overflow_ 之前**——桶持它的裸指针，
  // 成员按逆序析构，声明在前才等于析构在后。
  std::atomic<std::uint64_t> capped_rejects_aggregate_{0};
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
