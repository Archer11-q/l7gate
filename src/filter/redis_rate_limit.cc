#include "filter/redis_rate_limit.h"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>

#include <sys/time.h>

#include <hiredis/hiredis.h>

#include "route/balance.h"

namespace l7gate::filter {
namespace {

std::int64_t steady_now_ms() noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

constexpr char kHexDigits[] = "0123456789abcdef";

std::string hex16(std::uint64_t value) {
  std::string out(16, '\0');
  for (std::size_t i = 0; i < 16; ++i) {
    out[15 - i] = kHexDigits[(value >> (4 * i)) & 0xfULL];
  }
  return out;
}

// 睡眠切片：与 `main.cc` 里节点表 reload 线程同一个形状——停止的信号要能在几十毫秒内
// 被看到，而不是等完一个 settle 周期。
constexpr std::int64_t kStopSliceMs = 50;

}  // namespace

std::string redis_bucket_key(std::string_view kind_label, std::string_view local_key) {
  std::string out = "l7gate:rl:v1:";
  out.append(kind_label);
  out.push_back(':');
  out.append(hex16(route::fnv1a64(local_key)));
  return out;
}

std::string redis_kind_label(bool by_header, std::string_view header_name) {
  if (!by_header) return "path";
  std::string out = "hdr:";
  for (const char c : header_name) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

RedisSettler::RedisSettler(std::shared_ptr<RateLimiter> limiter, RedisSettlerConfig config)
    : limiter_(std::move(limiter)), config_(std::move(config)) {}

RedisSettler::~RedisSettler() { stop(); }

void RedisSettler::start() {
  if (running_.exchange(true, std::memory_order_acq_rel)) return;
  stop_requested_.store(false, std::memory_order_relaxed);
  worker_ = std::thread([this] { run(); });
}

void RedisSettler::stop() {
  stop_requested_.store(true, std::memory_order_relaxed);
  if (worker_.joinable()) worker_.join();
  running_.store(false, std::memory_order_relaxed);
  // 句柄只归结算线程所有；join 之后由调用方（析构）放手才是安全的。
  reset_connection();
}

void RedisSettler::run() {
  std::int64_t next = steady_now_ms() + config_.settle_ms;
  while (!stop_requested_.load(std::memory_order_relaxed)) {
    const std::int64_t now = steady_now_ms();
    if (now >= next) {
      if (settle_tick()) {
        settles_ok_.fetch_add(1, std::memory_order_relaxed);
        degraded_.store(0, std::memory_order_relaxed);
      } else {
        settles_failed_.fetch_add(1, std::memory_order_relaxed);
        degraded_.store(1, std::memory_order_relaxed);
        // 兜底不是"什么都不做"：把夹子全放开，语义立刻退回 S4-03 的纯本地桶——拒绝能力
        // 不消失，而在 Redis 恢复之前不会再有一条陈旧的低授权把某个键饿死。
        limiter_->release_all_caps();
        reset_connection();
      }
      next = steady_now_ms() + config_.settle_ms;
      continue;
    }
    const std::int64_t wait = next - now;
    const std::int64_t slice = wait < kStopSliceMs ? wait : kStopSliceMs;
    std::this_thread::sleep_for(std::chrono::milliseconds(slice));
  }
}

void RedisSettler::reset_connection() noexcept {
  if (ctx_ != nullptr) {
    redisFree(static_cast<redisContext*>(ctx_));
    ctx_ = nullptr;
  }
  script_sha_.clear();
}

bool RedisSettler::load_script() {
  redisContext* ctx = static_cast<redisContext*>(ctx_);
  if (ctx == nullptr) return false;
  void* reply = redisCommand(ctx, "SCRIPT LOAD %b", kRateLimitBucketLua.data(),
                             kRateLimitBucketLua.size());
  if (reply == nullptr) {
    reset_connection();
    return false;
  }
  auto* shape = static_cast<redisReply*>(reply);
  if (shape->type != REDIS_REPLY_STRING || shape->str == nullptr) {
    freeReplyObject(reply);
    reset_connection();
    return false;
  }
  script_sha_.assign(shape->str, shape->len);
  freeReplyObject(reply);
  return true;
}

bool RedisSettler::ensure_connected() {
  if (ctx_ != nullptr && !script_sha_.empty()) return true;
  if (ctx_ == nullptr) {
    timeval connect_tv{};
    connect_tv.tv_sec = config_.timeout_ms / 1000;
    connect_tv.tv_usec = (config_.timeout_ms % 1000) * 1000;
    timeval command_tv = connect_tv;
    redisOptions options{};
    options.type = REDIS_CONN_TCP;
    options.connect_timeout = &connect_tv;
    options.command_timeout = &command_tv;
    options.endpoint.tcp.ip = config_.host.c_str();
    options.endpoint.tcp.port = static_cast<int>(config_.port);
    redisContext* fresh = redisConnectWithOptions(&options);
    if (fresh == nullptr || fresh->err != 0) {
      if (fresh != nullptr) redisFree(fresh);
      return false;
    }
    ctx_ = fresh;
  }
  // 脚本只在"连着但没 sha"时 LOAD 一次；NOSCRIPT 的运行时回退在 settle_bucket 里。
  return load_script();
}

bool RedisSettler::settle_bucket(const RateLimiter::BucketView& view, const std::string& key,
                                 std::uint64_t delta_used, std::int64_t now_ms) {
  redisContext* ctx = static_cast<redisContext*>(ctx_);
  const std::string rate = std::to_string(limiter_->rate_per_sec());
  const std::string burst = std::to_string(limiter_->burst());
  const std::string used = std::to_string(delta_used);
  const std::string now = std::to_string(static_cast<long long>(now_ms));

  void* reply = redisCommand(ctx, "EVALSHA %s 1 %s %s %s %s %s", script_sha_.c_str(), key.c_str(),
                             rate.c_str(), burst.c_str(), now.c_str(), used.c_str());
  if (reply == nullptr) return false;
  auto* shape = static_cast<redisReply*>(reply);
  const bool noscript = shape->type == REDIS_REPLY_ERROR && shape->str != nullptr &&
                        std::string(shape->str).rfind("NOSCRIPT", 0) == 0;
  if (noscript) {
    // 服务端重启或 SCRIPT FLUSH 之后脚本不在了：重新 LOAD 一次、原样重试一次。这是整条
    // 链上唯一"换个说法再问一次"的地方——第二次重试就是把故障交给运气。
    freeReplyObject(reply);
    if (!load_script()) return false;
    reply = redisCommand(ctx, "EVALSHA %s 1 %s %s %s %s %s", script_sha_.c_str(), key.c_str(),
                         rate.c_str(), burst.c_str(), now.c_str(), used.c_str());
    if (reply == nullptr) return false;
    shape = static_cast<redisReply*>(reply);
  }
  if (shape->type != REDIS_REPLY_INTEGER) {
    freeReplyObject(reply);
    return false;
  }
  const long long remaining = shape->integer;
  freeReplyObject(reply);
  // 负数在脚本里已经夹成 0，这里再夹一次是防"跑在服务端的不是我写的脚本"这种运维事故。
  view.bucket->set_cap_milli(remaining < 0 ? 0 : static_cast<std::uint64_t>(remaining));
  return true;
}

bool RedisSettler::settle_tick() {
  if (!ensure_connected()) return false;
  redisContext* ctx = static_cast<redisContext*>(ctx_);
  void* ping = redisCommand(ctx, "PING");
  if (ping == nullptr) return false;
  auto* ping_shape = static_cast<redisReply*>(ping);
  const bool alive = ping_shape->type == REDIS_REPLY_STATUS && ping_shape->str != nullptr &&
                     std::string(ping_shape->str) == "PONG";
  freeReplyObject(ping);
  if (!alive) return false;

  const std::int64_t now_ms = steady_now_ms();
  const std::vector<RateLimiter::BucketView> views = limiter_->snapshot_buckets();
  std::uint64_t commands = 0;
  for (const RateLimiter::BucketView& view : views) {
    const std::string key = view.overflow ? std::string(kRateLimitOverflowKey)
                                          : redis_bucket_key(config_.kind_label, view.key);
    const auto reported = reported_used_.find(key);
    const auto seen = seen_capped_.find(key);
    const std::uint64_t prev_used = reported != reported_used_.end() ? reported->second : 0;
    const std::uint64_t prev_capped = seen != seen_capped_.end() ? seen->second : 0;
    // 单调计数：桶不回收（ADR-21 第四节第 4 条），`used_total` 只增不减，差量就是这一拍的
    // 消耗。真看到回退（那是我算错了）就当 0 报——少报会让 Redis 多授权，那才是不安全的方向。
    const std::uint64_t delta = view.used_total >= prev_used ? view.used_total - prev_used : 0;
    const std::uint64_t capped = view.bucket->capped_rejects();
    // **结算只遍历"有需求"的桶**：这一拍放过东西（要扣全局额度），或者被夹子按下去过
    // （要刷新授权，否则陈旧的低授权会把它饿死——而"陈旧的低授权一直在产生 capped_rejects"
    // 正是这条需求信号自我修正的地方）。16384 个冷键不该换来 16384 次 EVALSHA。
    if (delta == 0 && capped == prev_capped) continue;
    if (!settle_bucket(view, key, delta, now_ms)) {
      // 半途失败也要把这一拍已经发出去的条数记账：它是"结算只遍历有需求的桶"那条断言的
      // 分子，丢掉一段等于让证据比实际少（计数器不替失败的路径遮羞）。
      settle_commands_.fetch_add(commands, std::memory_order_relaxed);
      return false;
    }
    // **成功之后才记账**：命令失败时保留上一次的上报值，下一拍会把同一笔消耗重报。
    // 先记账再失败等于把这笔消耗丢掉，Redis 于是多授权——方向不安全。
    reported_used_[key] = view.used_total;
    seen_capped_[key] = capped;
    ++commands;
  }
  settle_commands_.fetch_add(commands, std::memory_order_relaxed);
  return true;
}

}  // namespace l7gate::filter
