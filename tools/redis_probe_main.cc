// S4-04 首次连通取证（ADR-05 追加节第一节与第八节，契约 9.1）：hiredis 1.2.0 客户端 ↔
// `redis:7.2.5` 服务端真实跑通的最小可复跑工具。它不参与任何 SLO 测量。
//
// 与 mysql 探针（ADR-20）刻意不同的一点：**这个链 `l7gate_core`**。因为要取证的不只是
// "库能连上"，还有"我那段 Lua 与那套键口径在这台服务端上真的按我算的方式工作"——
// 脚本与键名是产品资产，探针复用它们才有意义；自己抄一份脚本去取证，证的是另一份东西。
//
// 取证即断言：connect → PING → SCRIPT LOAD → EVALSHA（四步确定性读数，逐条与本地
// `TokenBucket` 的数学对得上）→ HGETALL 读回 → DEL 清场。任何一步不符都退出码 1 并打出
// 是哪一步、期望值与实际值——"能连上"和"能用"是两个命题，"算得跟我一样"是第三个。
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include <hiredis/hiredis.h>

#include "filter/rate_limit.h"
#include "filter/redis_rate_limit.h"

namespace {

int fail(std::string_view step, std::string_view why) {
  std::fprintf(stderr, "[probe] ✗ %s: %s\n", std::string(step).c_str(),
               std::string(why).c_str());
  return 1;
}

const char* reply_type_name(int type) {
  switch (type) {
    case REDIS_REPLY_STATUS: return "status";
    case REDIS_REPLY_ERROR: return "error";
    case REDIS_REPLY_INTEGER: return "integer";
    case REDIS_REPLY_STRING: return "string";
    case REDIS_REPLY_ARRAY: return "array";
    case REDIS_REPLY_NIL: return "nil";
    default: return "other";
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string host = "127.0.0.1";
  int port = 16379;
  for (int i = 1; i < argc; ++i) {
    const std::string_view flag = argv[i];
    const char* value = (i + 1 < argc) ? argv[i + 1] : nullptr;
    if (flag == "--host" && value != nullptr) {
      host = value;
      ++i;
    } else if (flag == "--port" && value != nullptr) {
      port = std::atoi(value);
      ++i;
    } else {
      std::fprintf(stderr, "参数错误: %s\n", std::string(flag).c_str());
      return 2;
    }
  }

  std::printf("[probe] client=hiredis %d.%d.%d\n", HIREDIS_MAJOR, HIREDIS_MINOR, HIREDIS_PATCH);

  timeval connect_tv{2, 0};
  timeval command_tv{2, 0};
  redisOptions options{};
  options.type = REDIS_CONN_TCP;
  options.connect_timeout = &connect_tv;
  options.command_timeout = &command_tv;
  options.endpoint.tcp.ip = host.c_str();
  options.endpoint.tcp.port = port;
  redisContext* ctx = redisConnectWithOptions(&options);
  if (ctx == nullptr) return fail("connect", "redisConnectWithOptions 返回空");
  if (ctx->err != 0) {
    std::fprintf(stderr, "[probe] ✗ connect: %s\n", ctx->errstr);
    redisFree(ctx);
    return 1;
  }

  // 步骤 1：PING。
  auto* ping = static_cast<redisReply*>(redisCommand(ctx, "PING"));
  if (ping == nullptr) return fail("ping", "无响应（连接已坏）");
  if (ping->type != REDIS_REPLY_STATUS || std::string_view(ping->str) != "PONG") {
    const std::string got = ping->str != nullptr ? ping->str : "(null)";
    freeReplyObject(ping);
    return fail("ping", "期望 PONG，实际 " + got);
  }
  freeReplyObject(ping);
  std::printf("[probe] ping=ok\n");

  // 步骤 2：服务端版本（INFO），契约锁 7.2.5。
  auto* info = static_cast<redisReply*>(redisCommand(ctx, "INFO server"));
  if (info == nullptr || info->type != REDIS_REPLY_STRING || info->str == nullptr) {
    if (info != nullptr) freeReplyObject(info);
    return fail("info", "读不到 INFO server");
  }
  std::string server_version = "?";
  {
    const std::string_view text(info->str, info->len);
    const std::size_t at = text.find("redis_version:");
    if (at != std::string_view::npos) {
      const std::size_t begin = at + std::string_view("redis_version:").size();
      const std::size_t end = text.find('\n', begin);
      server_version = std::string(text.substr(begin, end - begin));
      while (!server_version.empty() && (server_version.back() == '\r' || server_version.back() == '\n')) {
        server_version.pop_back();
      }
    }
  }
  freeReplyObject(info);
  std::printf("[probe] server=redis %s\n", server_version.c_str());

  // 步骤 3：SCRIPT LOAD（产品那份脚本，一字不改）。
  auto* load = static_cast<redisReply*>(redisCommand(ctx, "SCRIPT LOAD %b",
                                                     l7gate::filter::kRateLimitBucketLua.data(),
                                                     l7gate::filter::kRateLimitBucketLua.size()));
  if (load == nullptr || load->type != REDIS_REPLY_STRING || load->str == nullptr) {
    if (load != nullptr) freeReplyObject(load);
    return fail("script_load", "没有返回 sha（服务端可能禁脚本）");
  }
  const std::string sha(load->str, load->len);
  freeReplyObject(load);
  std::printf("[probe] script_sha=%s script_bytes=%zu\n", sha.c_str(),
              l7gate::filter::kRateLimitBucketLua.size());

  // 步骤 4：键口径 + 四步确定性读数。用**固定**的 now（不取墙上时钟），这样期望值是算出来的。
  const std::string key = l7gate::filter::redis_bucket_key("path", "/s404-probe");
  std::printf("[probe] bucket_key=%s\n", key.c_str());
  auto* del0 = static_cast<redisReply*>(redisCommand(ctx, "DEL %s", key.c_str()));
  if (del0 == nullptr || del0->type != REDIS_REPLY_INTEGER) {
    if (del0 != nullptr) freeReplyObject(del0);
    return fail("del_prelude", "清场失败");
  }
  freeReplyObject(del0);

  // rate=1/s、burst=3（毫 token 制：满桶 = 3000）。
  struct Case {
    long long now;
    long long used;
    long long want;
    const char* what;
  } cases[] = {
      {1000000, 0, 3000, "fresh_full"},        // 新键直接满桶
      {1001000, 3, 0, "after_burst"},          // 满桶放掉 3 条 → 0
      {1002000, 0, 1000, "refill_1s"},         // 一秒补 rate 个 → 1000 毫
      {1062000, 0, 3000, "clamp_burst"},       // 补过头要夹回 burst
  };
  for (const Case& c : cases) {
    auto* r = static_cast<redisReply*>(
        redisCommand(ctx, "EVALSHA %s 1 %s %llu %llu %lld %lld", sha.c_str(), key.c_str(),
                     1ULL, 3ULL, c.now, c.used));
    if (r == nullptr || r->type != REDIS_REPLY_INTEGER) {
      const int type = r != nullptr ? r->type : -1;
      if (r != nullptr) freeReplyObject(r);
      return fail(c.what, std::string("EVALSHA 没返回整数（type=") + reply_type_name(type) + "）");
    }
    const long long got = r->integer;
    freeReplyObject(r);
    std::printf("[probe] case=%s now=%lld used=%lld want=%lld got=%lld %s\n", c.what, c.now, c.used,
                c.want, got, got == c.want ? "✓" : "✗");
    if (got != c.want) return fail(c.what, "期望 " + std::to_string(c.want) + "，实际 " + std::to_string(got));
  }

  // 步骤 5：HGETALL 读回状态（运维能看到的就是这两格）。
  auto* hash = static_cast<redisReply*>(redisCommand(ctx, "HGETALL %s", key.c_str()));
  if (hash == nullptr || hash->type != REDIS_REPLY_ARRAY || hash->elements != 4) {
    if (hash != nullptr) freeReplyObject(hash);
    return fail("hgetall", "桶状态不是 tokens/last 两对");
  }
  std::printf("[probe] hgetall=%s=%s %s=%s\n", hash->element[0]->str, hash->element[1]->str,
              hash->element[2]->str, hash->element[3]->str);
  freeReplyObject(hash);

  // 步骤 6：DEL 清场。
  auto* del = static_cast<redisReply*>(redisCommand(ctx, "DEL %s", key.c_str()));
  if (del == nullptr || del->type != REDIS_REPLY_INTEGER || del->integer != 1) {
    if (del != nullptr) freeReplyObject(del);
    return fail("del", "没删掉那一格");
  }
  freeReplyObject(del);

  // 步骤 7：与本地桶对账（同一个数学的两端）：满桶连取 4 次，第 4 次必须被拒。
  std::int64_t t = 1000000;
  l7gate::filter::TokenBucket local(1, 3);
  int allowed = 0;
  for (int i = 0; i < 4; ++i) {
    if (local.try_acquire(t)) ++allowed;
  }
  std::printf("[probe] local_bucket_allowed=%d (期望 3，与 case=fresh_full 的 3000 毫同一件事)\n",
              allowed);
  if (allowed != 3) return fail("local_bucket", "本地桶连 3 条都放不下，探针的前提不成立");

  redisFree(ctx);
  std::printf("[probe] 结论=七步全通 client=hiredis-%d.%d.%d server=%s\n", HIREDIS_MAJOR,
              HIREDIS_MINOR, HIREDIS_PATCH, server_version.c_str());
  return 0;
}
