#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "filter/rate_limit.h"

// S4-04（ADR-05 落地追加节）：Redis 分布式计数 = **给本地桶挂一只授权夹子**。
//
// 分工一句话：**Redis 不重新定义限流数学，它只回答"这一拍这个进程最多还能放多少"。**
// `TokenBucket`（S4-03）继续是执行器，`rate`/`burst`/键/溢出全不动；这个文件里的
// `RedisSettler` 每 `settle_ms` 一拍，把各桶自上次结算以来的放行数上报给 Redis、
// 取回"全局剩余毫 token"，写进那只桶的 cap。夹子是透镜不是额度（见 rate_limit.h）：
// 它只会让放行变少，永远不会多——这条方向性是整个降级设计安全的前提。
//
// 为什么不每请求问一次 Redis：Lua EVAL 是阻塞往返，而本仓库在 S3-04 已把"阻塞 IO
// 不进 worker 事件循环"写成明文先例（ADR-06/ADR-20 的 reload 线程）。所以这里同样是
// 一条独立线程，跨线程流动的只有每桶两个原子（cap 写、used 读）。ADR-14 一字不动。
namespace l7gate::filter {

// Redis 侧那只桶的脚本。**它与 `TokenBucket::try_acquire` 必须逐位同一套数学**，
// 否则"单实例下数字一致"这条验收就成了比对两个不同的东西。毫 token 单位、
// 补货夹 burst、扣上报的消耗、再夹一次。
//
// 三条实现约束写在这里而不是代码注释里，因为它们都会咬人：
//   一 **不调 `redis.call("TIME")`**：脚本里的非确定命令会让"之后写"在复制形态下直接
//      报错；时钟由调用方提供（`now_ms` 用网关的单调毫秒），单机语境下两端同一个钟，
//      这是加强不是妥协——代价是跨主机部署时"同一个钟"不再成立，归部署形态。
//   二 **补货量先夹一次再加**（`add = min(rate*elapsed, burst)`）：Lua 的 number 是
//      double，`rate ≤ 1e6` 乘上一个以 epoch 计的 `elapsed`（≈1.7e12）会到 1.7e18，
//      超过 2^53 之后整数不再精确。反正结果马上要夹到 burst，先夹等价且安全。
//   三 **返回的是毫 token 的剩余**，与 `TokenBucket::set_cap_milli` 同一个单位。
inline constexpr std::string_view kRateLimitBucketLua = R"LUA(
local rate = tonumber(ARGV[1])
local burst = tonumber(ARGV[2]) * 1000
local now = tonumber(ARGV[3])
local used = tonumber(ARGV[4])
local tokens = redis.call('HGET', KEYS[1], 'tokens')
local last = redis.call('HGET', KEYS[1], 'last')
if (not tokens) or (not last) then
  tokens = burst
  last = now
end
tokens = tonumber(tokens)
last = tonumber(last)
local elapsed = now - last
if elapsed < 0 then elapsed = 0 end
local add = rate * elapsed
if add > burst then add = burst end
tokens = tokens + add
if tokens > burst then tokens = burst end
tokens = tokens - used * 1000
if tokens < 0 then tokens = 0 end
redis.call('HSET', KEYS[1], 'tokens', tokens, 'last', now)
return tokens
)LUA";

// 键口径（ADR-05 追加节第三节）：`l7gate:rl:v1:<kind>:<fnv1a64(本地键) 的 16 位十六进制>`。
// path 键含原始 query、`header:` 键的字节形态由客户端控制——原样拼进键名会让 Redis 侧
// 键长不可算、`redis-cli` 里看到任意字节。哈希掉换来的更硬的东西是：键与 S4-03 的本地键
// **同源**（同一个 `RateLimiter` 算出来的那个字符串），不存在第二套键定义。
// 碰撞方向是"两个键共享一只全局桶"= 更严不是更松；16384 键下概率 ≈ 16384²/2⁶⁵ ≈ 7.3e-12。
std::string redis_bucket_key(std::string_view kind_label, std::string_view local_key);

// 溢出键在 Redis 侧也共用一格（与本地那只溢出桶同一条退化，不是第二份账）。
inline constexpr std::string_view kRateLimitOverflowKey = "l7gate:rl:v1:ovf";

// 键口径里那一段 `<kind>`：`path`，或 `hdr:<小写头名>`。
// 小写是因为头名大小写不敏感（`find_header_value` 就是按这个语义比的）——`ETag` 与 `etag`
// 必须落到同一只全局桶，否则同一个值在 Redis 侧会被算成两份额度。
// 它是纯函数、第一个读者是 `main.cc` 的装配处 + ctest（1.16 那条"字段与读者同批"的纪律）。
std::string redis_kind_label(bool by_header, std::string_view header_name);

struct RedisSettlerConfig {
  std::string host = "127.0.0.1";
  std::uint16_t port = 6379;
  std::int64_t settle_ms = 200;    // 结算周期（缺省与 ADR-21 的超时扫描同一粒度）
  std::int64_t timeout_ms = 1000;  // 单次连接/命令超时：挂死的 redis 不能拖死这条线程
  std::string kind_label = "path";  // "path" | "hdr:<小写头名>"
};

// 一条后台线程 + 一条同步 hiredis 连接（只属于这条线程）。
// 观测面四个数，每一个都有第一个读者（ADR-05 追加节第八节的验收句）：
//   settles_ok / settles_failed —— 降级三段的时间线与"每秒都在有账"
//   degraded —— 当前是不是兜底态（横幅与退出账都打）
//   settle_commands —— **累计**真正发出去的 EVALSHA 条数。它是"结算只遍历有需求的桶"
//     这条的唯一证据：一拍都没流量的话，7 拍 × 1 个活跃键也只能读到 1，而不是 7。
//     （原来读成"最后一拍的条数"，安静收尾那一拍会把它报成 0——一个没有读者的形状。）
class RedisSettler {
 public:
  RedisSettler(std::shared_ptr<RateLimiter> limiter, RedisSettlerConfig config);
  RedisSettler(const RedisSettler&) = delete;
  RedisSettler& operator=(const RedisSettler&) = delete;
  ~RedisSettler();

  void start();
  // 幂等：没起过 / 已经停过都直接返回。main.cc 在打退出账**之前**调它，
  // 这样 `settles_ok` 的最后一拍与业务的 allowed/rejected 不会互相插队。
  void stop();

  std::uint64_t settles_ok() const noexcept { return settles_ok_.load(std::memory_order_relaxed); }
  std::uint64_t settles_failed() const noexcept {
    return settles_failed_.load(std::memory_order_relaxed);
  }
  bool degraded() const noexcept { return degraded_.load(std::memory_order_relaxed) != 0; }
  std::uint64_t settle_commands() const noexcept {
    return settle_commands_.load(std::memory_order_relaxed);
  }

 private:
  void run();
  // 一拍：PING 探活 + 逐桶按需 EVALSHA 结算。返回 false = 这一拍失败（调用方兜底并重连）。
  bool settle_tick();
  bool ensure_connected();
  bool load_script();
  void reset_connection() noexcept;
  // 取回授权并写进桶的 cap；返回是否这一条命令成功。
  bool settle_bucket(const RateLimiter::BucketView& view, const std::string& key,
                     std::uint64_t delta_used, std::int64_t now_ms);

  std::shared_ptr<RateLimiter> limiter_;
  RedisSettlerConfig config_;

  std::thread worker_;
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> running_{false};

  // 连接句柄只被结算线程碰（`void*` 以免这个头把 hiredis 传染给所有包含者）。
  void* ctx_ = nullptr;
  std::string script_sha_;
  // 每键上次上报给 Redis 的 `used_total` 与上次看到的 `capped_rejects`。
  // 只在结算线程里读写，所以不需要锁。
  std::unordered_map<std::string, std::uint64_t> reported_used_;
  std::unordered_map<std::string, std::uint64_t> seen_capped_;

  std::atomic<std::uint64_t> settles_ok_{0};
  std::atomic<std::uint64_t> settles_failed_{0};
  std::atomic<std::uint64_t> settle_commands_{0};
  std::atomic<std::uint64_t> degraded_{0};
};

}  // namespace l7gate::filter
