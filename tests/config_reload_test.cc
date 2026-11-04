// S5-02（ADR-10 落地追加节）：配置热重载这一层的用例。
//
// 这批用例存在的第一理由不是"覆盖字段"，是把追加节里那三条**机制**句子变成能红的断言：
//   一、换代只有一次原子 store，"半套生效"在结构上不可能——用例不满足于证明它没发生，
//        还要把发布点挪到校验之前（退化 H1）证明它会发生（名单在 FAULT-INJECTION 的 S5-02 那节）。
//   二、旧世代由引用计数延命，"最后一个引用释放前不回收"是**测**出来的：钉住一根、发布新的、
//        看析构数不动；放开、看它动。写一句"shared_ptr 应该会延命"不算验收。
//   三、拒载必须点名，点名的对象是**字段**而不是"配置错了"这一类。所以每条拒绝用例都同时断
//        两件事：进了哪一格计数 + 那句话里有没有那个字段名（`config_test.cc` 替 ADR-19 后果段
//        立的同一条立场）。
//
// 真进程那一侧（启动时那两条拒绝、生效耗时的毫秒数、在飞请求零截断）在
// `scripts/smoke-s5-02.sh` 与 `reactor_test.cc` 的 `S5_02_ReloadE2E`——两边加起来才是这条任务。
#include <sys/stat.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "config/config_reloader.h"
#include "config/gateway_config.h"
#include "config/json.h"
#include "route/balance.h"
#include "route/config_snapshot.h"
#include "route/health.h"

namespace {

using l7gate::config::GatewayConfig;
using l7gate::config::JsonValue;
using l7gate::route::ConfigPublisher;
using l7gate::route::ConfigSnapshot;
using l7gate::route::HealthPolicy;
using l7gate::route::HealthTable;
using l7gate::route::NodeTarget;
using l7gate::route::TargetRegistry;

std::string read_whole(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

bool build_from(std::string_view text, GatewayConfig* out, std::string* error) {
  JsonValue document;
  if (!l7gate::config::parse_json(text, &document, error)) return false;
  return l7gate::config::build_gateway_config(document, out, error);
}

// ---------------------------------------------------------------------------
// 配置文本
// ---------------------------------------------------------------------------

// 一份两组的配置：`api` 轮询两台、`shard` 一致性哈希两台。端口是编的——这些用例验的是
// "哪一代说什么"，不是"谁接了请求"；发消息那部分在 E2E 与冒烟里。
std::string v1_text() {
  return R"({
    "upstreams": [
      { "name": "api", "balance": "round_robin", "targets": [
        { "address": "127.0.0.1", "port": 18001 },
        { "address": "127.0.0.1", "port": 18002 } ] },
      { "name": "shard", "balance": "consistent_hash", "vnodes": 8, "targets": [
        { "address": "127.0.0.1", "port": 18002 },
        { "address": "127.0.0.1", "port": 18003 } ] }
    ],
    "routes": [
      { "match": "regex", "pattern": "/v[0-9]+/orders", "upstream": "shard" },
      { "match": "prefix", "pattern": "/api", "upstream": "api" },
      { "match": "prefix", "pattern": "", "upstream": "api" }
    ],
    "health": { "max_fails": 3, "probe_interval_ms": 0 },
    "circuit": { "window_ms": 10000, "minimum_requests": 20, "error_rate_percent": 50,
                 "half_open_requests": 3, "half_open_successes": 2, "half_open_timeout_ms": 3000 },
    "rate_limit": { "rate": 100, "burst": 100, "key": "path" }
  })";
}

// v2 只改**定义域内**的东西：`/api` 从 `api` 组改指 `shard`、`max_fails` 3→7、
// `shard` 组多一台 18004。
std::string v2_text() {
  return R"({
    "upstreams": [
      { "name": "api", "balance": "round_robin", "targets": [
        { "address": "127.0.0.1", "port": 18001 },
        { "address": "127.0.0.1", "port": 18002 } ] },
      { "name": "shard", "balance": "consistent_hash", "vnodes": 8, "targets": [
        { "address": "127.0.0.1", "port": 18002 },
        { "address": "127.0.0.1", "port": 18003 },
        { "address": "127.0.0.1", "port": 18004 } ] }
    ],
    "routes": [
      { "match": "regex", "pattern": "/v[0-9]+/orders", "upstream": "shard" },
      { "match": "prefix", "pattern": "/api", "upstream": "shard" },
      { "match": "prefix", "pattern": "", "upstream": "api" }
    ],
    "health": { "max_fails": 7, "probe_interval_ms": 0 },
    "circuit": { "window_ms": 10000, "minimum_requests": 20, "error_rate_percent": 50,
                 "half_open_requests": 3, "half_open_successes": 2, "half_open_timeout_ms": 3000 },
    "rate_limit": { "rate": 100, "burst": 100, "key": "path" }
  })";
}

// 把 v1 里某一处替换掉，其余逐字不动——"只改了 X"这件事必须由构造方式保证，
// 不能靠我手写两份然后自称只差一处。
std::string mutated(std::string from, std::string to) {
  std::string text = v1_text();
  const std::size_t at = text.find(from);
  EXPECT_NE(at, std::string::npos) << "改不动：" << from;
  text.replace(at, from.size(), to);
  return text;
}

// v1 去掉整个 `rate_limit` 段（段消失 = 链上少一环 = 装配形状变化）。
// 用"删掉这一整段"而不是"改一个字段"来造它，拒绝理由才落在段的那一格上。
std::string without_rate_limit_text() {
  std::string text = v1_text();
  const std::string needle =
      R"(,
    "rate_limit": { "rate": 100, "burst": 100, "key": "path" })";
  const std::size_t at = text.find(needle);
  EXPECT_NE(at, std::string::npos);
  text.erase(at, needle.size());
  return text;
}

// `shard` 组的成员来源换成 MySQL（合法文件，但两条发布路径不叠加，见追加节第一节第 7 条）。
std::string mysql_group_text() {
  return R"({
    "upstreams": [
      { "name": "api", "balance": "round_robin", "targets": [
        { "address": "127.0.0.1", "port": 18001 } ] },
      { "name": "shard", "balance": "round_robin", "source": "mysql" }
    ],
    "health": { "max_fails": 3, "probe_interval_ms": 0 },
    "rate_limit": { "rate": 100, "burst": 100, "key": "path" }
  })";
}

// ---------------------------------------------------------------------------
// 夹具：临时文件 + 与 `main.cc` 首装那一代完全相同的装配顺序
// ---------------------------------------------------------------------------

// 临时文件写在 **ctest 的工作目录**（= 构建目录，已被 .gitignore 挡住）而不是 `/tmp`：
// 一个跑完会在磁盘上留东西的用例，留的位置必须是别人一眼能认出是谁写的地方。
class ReloadFixture {
 public:
  ReloadFixture(std::string_view name, const std::string& initial)
      : path_("test-tmp/s5-02-" + std::string(name) + ".json") {
    std::filesystem::create_directories("test-tmp");
    write(initial);
    std::string why;
    if (!ConfigPublisher_probe_hash(why)) {
      error_ = why;
      return;
    }
    if (!build_from(read_whole(path_), &initial_config_, &error_)) return;
    // 先按文件端点表 intern：静态端点的下标必须与文件表一一对应（0..E-1），
    // ever-seen 的追加从这之后开始。顺序反过来就是"文件说 3 号是 A、注册表说 3 号是 B"。
    for (const l7gate::config::Endpoint& endpoint : initial_config_.endpoints()) {
      registry_->intern(NodeTarget{endpoint.address, endpoint.port, endpoint.health_path});
    }
    std::shared_ptr<const ConfigSnapshot> snapshot = l7gate::config::build_config_snapshot(
        initial_config_, initial_hash_, 1u, *registry_, &error_);
    if (snapshot == nullptr) return;
    const HealthPolicy policy = initial_config_.health().policy;
    if (initial_config_.health().present) {
      health_ = std::make_shared<HealthTable>(initial_config_.endpoints().size(), policy,
                                              /*capacity = */ 0, /*policy_reloadable = */ true);
      health_->set_policy(snapshot->health);
    }
    publisher_->publish(snapshot);
    reloader_ = std::make_unique<l7gate::config::ConfigReloader>(path_, /*interval_ms = */ 1,
                                                                 publisher_, health_, registry_,
                                                                 initial_config_);
    reloader_->note_published(initial_hash_);
    ok_ = true;
  }

  ~ReloadFixture() { std::remove(path_.c_str()); }

  bool ok() const { return ok_; }
  const std::string& error() const { return error_; }
  ConfigPublisher& publisher() { return *publisher_; }
  std::shared_ptr<const ConfigSnapshot> current() { return publisher_->load(); }
  HealthTable& health() { return *health_; }
  TargetRegistry& registry() { return *registry_; }
  const std::string& last_reject_reason() const { return reloader_->last_reject_reason(); }
  std::size_t registry_size() const { return registry_->size(); }

  void write(const std::string& text) {
    std::ofstream file(path_, std::ios::binary | std::ios::trunc);
    file << text;
  }

  // 写一份新内容并推一拍（时间戳单调往前走，节拍 1 ms，所以每一拍都真的到点）。
  void write_and_tick(const std::string& text) {
    write(text);
    tick();
  }

  void tick() {
    now_ms_ += 1000;
    reloader_->reload_tick(now_ms_);
  }

 private:
  bool ConfigPublisher_probe_hash(std::string& why) {
    return l7gate::config::ConfigReloader::file_content_hash(path_, &initial_hash_, &why);
  }

  const std::string path_;
  std::shared_ptr<ConfigPublisher> publisher_ = std::make_shared<ConfigPublisher>();
  std::shared_ptr<TargetRegistry> registry_ = std::make_shared<TargetRegistry>();
  std::shared_ptr<HealthTable> health_;
  std::unique_ptr<l7gate::config::ConfigReloader> reloader_;
  GatewayConfig initial_config_;
  std::uint64_t initial_hash_ = 0;
  // 从一个很大的基数起算：`note_published` 把下一次检查排在**真实**单调钟之后，
  // 用例要的只是"每一拍都过线"，所以直接跳到它后面去（不是绕过节拍，是同一格算术）。
  std::int64_t now_ms_ = 1'000'000'000'000ll;
  std::string error_;
  bool ok_ = false;
};

// 只在装配成功后跑：夹具的构造失败要有**一句**带原因的话，而不是十四格连环断言。
#define ASSERT_FIXTURE(fx)                                              \
  ASSERT_TRUE((fx).ok()) << "首代装配失败：" << (fx).error()

// ===========================================================================
// 一、首代快照：下标、环、路由重放
// ===========================================================================

TEST(S5_02_Snapshot, IndexSpaceMatchesTheFileEndpointTable) {
  ReloadFixture fx("idx", v1_text());
  ASSERT_FIXTURE(fx);
  const std::shared_ptr<const ConfigSnapshot> gen = fx.current();
  ASSERT_NE(gen, nullptr);
  EXPECT_EQ(gen->generation, 1u);
  // 去重后的文件端点表：18001/18002/18003 三台（18002 在两个组里各出现一次，但只登记一次）。
  ASSERT_EQ(gen->nodes->targets.size(), 3u);
  EXPECT_EQ(gen->nodes->targets[0].port, 18001);
  EXPECT_EQ(gen->nodes->targets[1].port, 18002);
  EXPECT_EQ(gen->nodes->targets[2].port, 18003);

  const l7gate::route::GroupTable& groups = gen->nodes->groups;
  ASSERT_EQ(groups.size(), 2u);
  const auto* api = groups.group_by_name("api");
  const auto* shard = groups.group_by_name("shard");
  ASSERT_NE(api, nullptr);
  ASSERT_NE(shard, nullptr);
  // 组成员存的是 ever-seen 下标；这里断的是"下标解出来的那台机器"而不是下标本身——
  // 读侧真正关心的只有端点串。
  auto port_of = [&](const l7gate::route::UpstreamGroup& group, std::size_t position) {
    return gen->nodes->targets[group.targets[position]].port;
  };
  ASSERT_EQ(api->targets.size(), 2u);
  EXPECT_EQ(port_of(*api, 0), 18001);
  EXPECT_EQ(port_of(*api, 1), 18002);
  ASSERT_EQ(shard->targets.size(), 2u);
  EXPECT_EQ(port_of(*shard, 0), 18002);
  EXPECT_EQ(port_of(*shard, 1), 18003);

  // 环在发布前就建好（装配期建环的纪律在快照上原样成立，ADR-20 第四节第二条）。
  EXPECT_TRUE(api->ring.empty());       // 轮询组没有环可建
  EXPECT_EQ(shard->ring.size(), 8u * 2u)  // vnodes 8 × 成员 2
      << "一致性哈希的环没在建好的状态下发出去";
}

TEST(S5_02_Snapshot, RouteReplayKeepsOrderPatternsAndGroupIndexes) {
  ReloadFixture fx("route", v1_text());
  ASSERT_FIXTURE(fx);
  const auto gen = fx.current();
  ASSERT_EQ(gen->routes.size(), 3u);
  // 顺序 = 文件顺序（链上"第一条命中"的正则那一支完全依赖它）。
  EXPECT_EQ(gen->routes.rule_at(0).kind, l7gate::route::Kind::kRegex);
  EXPECT_EQ(gen->routes.rule_at(0).pattern, "/v[0-9]+/orders");
  EXPECT_EQ(gen->routes.rule_at(1).kind, l7gate::route::Kind::kPrefix);
  EXPECT_EQ(gen->routes.rule_at(2).pattern, "");
  // 命中面。`shard` 是第 1 组、`api` 第 0 组。
  EXPECT_EQ(gen->routes.match("/v2/orders").value_or(999), 1);
  EXPECT_EQ(gen->routes.match("/api/x").value_or(999), 0);
  EXPECT_EQ(gen->routes.match("/whatever").value_or(999), 0);  // catch-all
  EXPECT_EQ(gen->routes.match("/vX/orders").value_or(999), 0);  // 正则不过 → 落到 catch-all
}

// ===========================================================================
// 二、引用计数：旧世代在最后一个引用放手之前不回收（验收句②）
// ===========================================================================

TEST(S5_02_Publisher, OldGenerationIsNotFreedUntilTheLastReferenceGivesItUp) {
  ReloadFixture fx("refcount", v1_text());
  ASSERT_FIXTURE(fx);

  const std::uint64_t built_before = ConfigSnapshot::constructed();
  const std::uint64_t died_before = ConfigSnapshot::destructed();

  std::shared_ptr<const ConfigSnapshot> pinned = fx.current();  // 在飞请求的那一根针
  ASSERT_EQ(pinned->generation, 1u);

  // 造并发布第二代（走的是与 tick 同一条装配路径，只是这里由用例自己发）。
  GatewayConfig next;
  std::string why;
  ASSERT_TRUE(build_from(v2_text(), &next, &why)) << why;
  std::shared_ptr<const ConfigSnapshot> gen2 = l7gate::config::build_config_snapshot(
      next, /*content_hash = */ 0x1234u, 2u, fx.registry(), &why);
  ASSERT_NE(gen2, nullptr) << why;
  EXPECT_EQ(ConfigSnapshot::constructed() - built_before, 1u);
  fx.publisher().publish(gen2);

  // 换代已经发生了，而第一代还活着：**这就是那句"最后一个引用释放前不回收"**。
  EXPECT_EQ(ConfigSnapshot::destructed(), died_before)
      << "刚 store 完旧世代就析构了——那根针没有钉住任何东西";
  EXPECT_EQ(ConfigSnapshot::live(), 2u) << "在册世代数不等于构造减析构";
  // 钉着的那一根读到的仍然是旧答案（新表里 `/api` 已经改指 shard 组了）。
  EXPECT_EQ(pinned->routes.match("/api/x").value_or(999), 0);
  EXPECT_EQ(fx.current()->routes.match("/api/x").value_or(999), 1);

  pinned.reset();
  EXPECT_EQ(ConfigSnapshot::destructed() - died_before, 1u)
      << "最后一根针放开了，旧世代却没有死——引用计数在别处被漏出来了";
  EXPECT_EQ(ConfigSnapshot::live(), 1u);
}

// ===========================================================================
// 三、成功的那一轮：路由 / 策略 / 成员表同时换代
// ===========================================================================

TEST(S5_02_Reloader, GoodReloadSwapsRoutesHealthPolicyAndMemberTableTogether) {
  ReloadFixture fx("good", v1_text());
  ASSERT_FIXTURE(fx);
  fx.write_and_tick(v2_text());

  EXPECT_EQ(fx.publisher().checks(), 1u);
  EXPECT_EQ(fx.publisher().publishes(), 1u);
  EXPECT_EQ(fx.publisher().rejects(), 0u);
  EXPECT_EQ(fx.publisher().generation(), 2u);

  const auto gen = fx.current();
  EXPECT_EQ(gen->routes.match("/api/x").value_or(999), 1) << "路由没换代";
  EXPECT_EQ(gen->nodes->targets.size(), 4u) << "新成员没进 ever-seen 表";
  const auto* shard = gen->nodes->groups.group_by_name("shard");
  ASSERT_NE(shard, nullptr);
  EXPECT_EQ(shard->targets.size(), 3u);

  // 策略换代：`max_fails` 3→7。
  EXPECT_EQ(fx.health().policy_view()->max_fails, 7u) << "circuit/health 参数没换代（验收句①）";
  // **同一根针，不是抄本**：健康表读的就是这份快照里的那个对象。
  EXPECT_EQ(fx.health().policy_view().operator->(), gen->health.get())
      << "策略有第二份真相了：表里那份与快照里那份不是同一个对象";
  EXPECT_GT(fx.publisher().last_publish_epoch_ms(), 0) << "换代时刻没有自报（生效延迟那一格要用它）";
  EXPECT_GE(fx.publisher().last_publish_duration_ms(), 0);
}

// ===========================================================================
// 四、坏配置：拒绝 + 点名 + 旧世代继续服务（验收句④）+ 命令行来源不参与（验收句⑤）
// ===========================================================================

// 每条拒绝用例都以"随后一次好配置确实发布了"收尾：上界必配下界，
// 否则"没生效"也可能只是因为这套东西根本发不出去。
TEST(S5_02_Reloader, RateLimitParametersAreOutOfScopeAndRefusedByName) {
  ReloadFixture fx("ratelimit", v1_text());
  ASSERT_FIXTURE(fx);
  const auto before = fx.current();

  fx.write_and_tick(mutated(R"("rate": 100)", R"("rate": 200)"));
  EXPECT_EQ(fx.publisher().rejects_scope(), 1u);
  EXPECT_EQ(fx.publisher().publishes(), 0u) << "限流参数变了却换发了世代——那是半套";
  EXPECT_EQ(fx.publisher().generation(), 1u);
  EXPECT_EQ(fx.current(), before) << "拒载之后发布点上的世代指针动了";
  EXPECT_NE(fx.last_reject_reason().find("rate_limit"), std::string::npos)
      << "拒绝了，但话里没点到那一段：" << fx.last_reject_reason();

  fx.write_and_tick(v2_text());  // 下界
  EXPECT_EQ(fx.publisher().publishes(), 1u);
}

// ADR-28（S5-09）：重载路径上的同一个校验器。地址写成服务名 → 那一轮拒载、`config_publishes`
// 一动不动、旧世代仍在服务；话里点到实际读到的值。S5-05 的退化 C1 记的是这件事的**后果**
// （整份 `/metrics` 抓失败），这一条钉的是它的前因（那种配置根本进不来）。
TEST(S5_09_Reloader, HostnameAddressIsRefusedByNameAndKeepsTheOldGeneration) {
  ReloadFixture fx("byname", v1_text());
  ASSERT_FIXTURE(fx);
  const auto before = fx.current();

  fx.write_and_tick(mutated(R"("address": "127.0.0.1")", R"("address": "upstream-a")"));
  EXPECT_EQ(fx.publisher().rejects_scope(), 1u);
  EXPECT_EQ(fx.publisher().publishes(), 0u) << "地址非法却换发了世代——那是半套";
  EXPECT_EQ(fx.publisher().generation(), 1u);
  EXPECT_EQ(fx.current(), before) << "拒载之后发布点上的世代指针动了";
  EXPECT_NE(fx.last_reject_reason().find("不是 IPv4 字面量"), std::string::npos)
      << "拒绝了，但话里没点到那条理由：" << fx.last_reject_reason();
  EXPECT_NE(fx.last_reject_reason().find("upstream-a"), std::string::npos)
      << "话里没点到实际读到的值：" << fx.last_reject_reason();

  fx.write_and_tick(v2_text());  // 下界：合法配置照样发得出去
  EXPECT_EQ(fx.publisher().publishes(), 1u);
}

TEST(S5_02_Reloader, RateLimitKeyChangeIsRefusedByName) {
  ReloadFixture fx("ratekey", v1_text());
  ASSERT_FIXTURE(fx);
  fx.write_and_tick(mutated(R"("key": "path")", R"("key": "header:x-y")"));
  EXPECT_EQ(fx.publisher().rejects_scope(), 1u);
  EXPECT_EQ(fx.publisher().publishes(), 0u);
  EXPECT_NE(fx.last_reject_reason().find("key"), std::string::npos) << fx.last_reject_reason();
  fx.write_and_tick(v2_text());
  EXPECT_EQ(fx.publisher().publishes(), 1u);
}

TEST(S5_02_Reloader, SectionAppearingOrVanishingIsRefusedBecauseItIsChainShape) {
  ReloadFixture fx("shape", v1_text());
  ASSERT_FIXTURE(fx);
  // 段的**消失**：链的形状变了（装不装那一环），不在重载定义域。
  fx.write_and_tick(without_rate_limit_text());
  EXPECT_EQ(fx.publisher().rejects_scope(), 1u);
  EXPECT_NE(fx.last_reject_reason().find("rate_limit"), std::string::npos)
      << fx.last_reject_reason();
  // 段的**出现**：首装没有 `routes` 的那一台，重载也不许凭空挂一个 filter 进链。
  ReloadFixture bare("shape-bare", R"({"upstreams": [
      { "name": "api", "balance": "round_robin", "targets": [
        { "address": "127.0.0.1", "port": 18001 } ] }]})");
  ASSERT_FIXTURE(bare);
  bare.write_and_tick(R"({"upstreams": [
      { "name": "api", "balance": "round_robin", "targets": [
        { "address": "127.0.0.1", "port": 18001 } ] }],
    "routes": [ { "match": "prefix", "pattern": "/", "upstream": "api" } ]})");
  EXPECT_EQ(bare.publisher().rejects_scope(), 1u);
  EXPECT_NE(bare.last_reject_reason().find("routes"), std::string::npos)
      << bare.last_reject_reason();
  EXPECT_EQ(bare.publisher().publishes(), 0u);
}

TEST(S5_02_Reloader, CommandLineSourcedKeyInFileIsRefusedAndNamed) {
  ReloadFixture fx("two-source", v1_text());
  ASSERT_FIXTURE(fx);
  // 已裁② 的切分不许被重载绕过：监听/worker 这类键属于命令行，出现在文件里
  // = 顶层未知字段 = 拒载并点名。这一格同时就是验收句⑤ 那句"违反 = 拒绝并点名"。
  fx.write_and_tick(mutated(R"("health": {)", R"("listen": "0.0.0.0", "workers": 4, "health": {)"));
  EXPECT_EQ(fx.publisher().rejects_scope(), 1u);
  EXPECT_EQ(fx.publisher().publishes(), 0u);
  EXPECT_NE(fx.last_reject_reason().find("listen"), std::string::npos)
      << fx.last_reject_reason();
  fx.write_and_tick(v2_text());
  EXPECT_EQ(fx.publisher().publishes(), 1u);
}

TEST(S5_02_Reloader, MysqlGroupIsRefusedBecauseTwoPublishersAreOneTruthTooMany) {
  ReloadFixture fx("mysql", v1_text());
  ASSERT_FIXTURE(fx);
  fx.write_and_tick(mysql_group_text());
  EXPECT_EQ(fx.publisher().rejects_scope(), 1u);
  EXPECT_EQ(fx.publisher().publishes(), 0u);
  EXPECT_NE(fx.last_reject_reason().find("mysql"), std::string::npos) << fx.last_reject_reason();
  fx.write_and_tick(v2_text());
  EXPECT_EQ(fx.publisher().publishes(), 1u);
}

TEST(S5_02_Reloader, SyntaxErrorKeepsTheOldGenerationServing) {
  ReloadFixture fx("syntax", v1_text());
  ASSERT_FIXTURE(fx);
  const auto before = fx.current();
  fx.write_and_tick(R"({ "upstreams": [ oops })");
  EXPECT_EQ(fx.publisher().rejects_syntax(), 1u);
  EXPECT_EQ(fx.publisher().rejects_scope(), 0u) << "语法错被记成了越界那一格——两类的账要分得开";
  EXPECT_EQ(fx.publisher().publishes(), 0u);
  EXPECT_EQ(fx.current(), before);
  // 旧世代仍在服务：路由答的还是旧的那一组。
  EXPECT_EQ(fx.current()->routes.match("/api/x").value_or(999), 0);
  fx.write_and_tick(v2_text());
  EXPECT_EQ(fx.publisher().publishes(), 1u);
}

TEST(S5_02_Reloader, FieldOutOfDomainIsRefusedAndNamesTheField) {
  ReloadFixture fx("field", v1_text());
  ASSERT_FIXTURE(fx);
  fx.write_and_tick(mutated(R"("balance": "round_robin")", R"("balance": "round_robn")"));
  EXPECT_EQ(fx.publisher().rejects_scope(), 1u);
  EXPECT_EQ(fx.publisher().publishes(), 0u);
  EXPECT_NE(fx.last_reject_reason().find("round_robn"), std::string::npos)
      << fx.last_reject_reason();
  fx.write_and_tick(v2_text());
  EXPECT_EQ(fx.publisher().publishes(), 1u);
}

TEST(S5_02_Reloader, OversizedFileIsRefusedBeforeItIsParsed) {
  ReloadFixture fx("big", v1_text());
  ASSERT_FIXTURE(fx);
  std::string huge = v2_text();
  huge.insert(huge.size() - 1, 2u << 20, ' ');  // 尾部补白：语法仍然对，但大小先该挡下来
  fx.write_and_tick(huge);
  EXPECT_EQ(fx.publisher().rejects_scope(), 1u);
  EXPECT_EQ(fx.publisher().publishes(), 0u);
  EXPECT_NE(fx.last_reject_reason().find("1 MiB"), std::string::npos) << fx.last_reject_reason();
  fx.write_and_tick(v2_text());
  EXPECT_EQ(fx.publisher().publishes(), 1u);
}

TEST(S5_02_Reloader, TouchWithoutContentChangeIsACheckNotAPublish) {
  ReloadFixture fx("touch", v1_text());
  ASSERT_FIXTURE(fx);
  // 内容一模一样的重写：mtime 与 size 都可能变（这里 size 没变、mtime 变了），
  // 但**发布必须看内容哈希**。"查了一遍不等于发布了一次"就是这一格。
  fx.write_and_tick(v1_text());
  EXPECT_EQ(fx.publisher().checks(), 1u);
  EXPECT_EQ(fx.publisher().publishes(), 0u) << "touch 被读成了一次换代";
  EXPECT_EQ(fx.publisher().generation(), 1u);
  fx.write_and_tick(v2_text());
  EXPECT_EQ(fx.publisher().publishes(), 1u) << "同样的节拍里真变更却没发布——上一格就成了假绿";
}

TEST(S5_02_Reloader, RejectLedgerIsConserved) {
  ReloadFixture fx("conserve", v1_text());
  ASSERT_FIXTURE(fx);
  fx.write_and_tick(R"({ bad json )");                            // 语法
  fx.write_and_tick(mutated(R"("rate": 100)", R"("rate": 200)"));  // 越界
  fx.write_and_tick(mutated(R"("max_fails": 3)", R"("max_fails": 99999999)"));  // 越界
  // 守恒类不变式：总账 = 两格分类账之和（这类才允许硬 ✗，口径第 5 条）。
  EXPECT_EQ(fx.publisher().rejects(),
            fx.publisher().rejects_syntax() + fx.publisher().rejects_scope());
  EXPECT_EQ(fx.publisher().rejects(), 3u);
  EXPECT_EQ(fx.publisher().publishes(), 0u);
}

// ===========================================================================
// 五、策略槽本身：没 arm 的表不许被动过
// ===========================================================================

TEST(S5_02_PolicySlot, TableBuiltWithoutReloadabilityIgnoresSetPolicy) {
  HealthPolicy base{};
  base.max_fails = 3;
  HealthTable off(2, base, /*capacity = */ 0, /*policy_reloadable = */ false);
  HealthTable on(2, base, /*capacity = */ 0, /*policy_reloadable = */ true);

  auto next = std::make_shared<HealthPolicy>(base);
  next->max_fails = 9;
  off.set_policy(next);
  on.set_policy(next);
  EXPECT_EQ(off.policy_view()->max_fails, 3u)
      << "没 arm 的表被换掉了策略——那等于关着的开关也能改行为";
  EXPECT_EQ(on.policy_view()->max_fails, 9u) << "arm 的表没换——那两条臂就没差别了";

  // 旧策略对象在最后一个持有者放手之前不析构：针是共享的，不是拷贝出来的副本。
  const HealthPolicy* raw = on.policy_view().operator->();
  EXPECT_EQ(raw, next.get());
  next.reset();
  HealthPolicy* mutable_raw = nullptr;
  {
    auto view = on.policy_view();
    EXPECT_EQ(view.operator->(), raw) << "针放开了就不认识同一个对象了";
    EXPECT_EQ(view->max_fails, 9u);  // 对象仍活着（由表里那根 shared_ptr 持有）
    mutable_raw = const_cast<HealthPolicy*>(view.operator->());
  }
  auto again = std::make_shared<HealthPolicy>(base);
  on.set_policy(again);  // 上一代被表松开，`raw` 此刻可以合法死亡
  EXPECT_NE(on.policy_view().operator->(), mutable_raw);
}

// ===========================================================================
// 六、组数扩容：轮询游标的容器（S5-02 才第一次让"组数"成为可变的东西）
// ===========================================================================

TEST(S5_02_LbState, GroupCapacityGrowsWithoutLosingCursors) {
  l7gate::route::LbState state(/*target_count = */ 2, /*group_count = */ 1);
  EXPECT_EQ(state.advance_rr(0, 2), 0u);
  EXPECT_EQ(state.advance_rr(0, 2), 1u);  // 游标推到 1
  // 新世代多两个组：不扩容就是 `rr_cursor_[2]` 越界写（这条是 S5-02 新造的边界）。
  state.ensure_group_capacity(4);
  EXPECT_EQ(state.advance_rr(3, 2), 0u) << "新组该从 0 开始轮";
  EXPECT_EQ(state.advance_rr(0, 2), 0u) << "老组的游标被扩容抹掉了——轮询分布会当场跳一格";
  // 组数变少不缩：这里只是断它不出错，理由写在 balance.h 那条声明旁边。
  state.ensure_group_capacity(1);
  EXPECT_EQ(state.advance_rr(0, 2), 1u);
}

}  // namespace
