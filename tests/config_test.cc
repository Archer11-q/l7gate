// S3-02（ADR-19 第二条与后果段）：配置文件这一层的用例——JSON 子集的拒绝面 + 装配的拒绝面 +
// 装配成功后内部结构长什么样。
//
// **这批用例存在的第一理由不是"覆盖字段"，是"每一条坏输入都必须整体失败"**。ADR-19 立的立场是
// 读不出就失败、不"退回默认配置继续跑"：一个能安静退化成"少配一条路由"的网关，它的观测面看起来
// 只是负载不对，不是配置错了——那种失效没人会去查配置文件。所以每条拒绝用例都同时断两件事：
// 返回 false，**且**报错的话里点到了那个字段名或那一条数组下标（"退出码 2 + 指出哪一个字段"是
// ADR-19 后果段写死的形状，只断 false 的话，"整个文件读不懂"和"这一条字段错了"就分不出来）。
//
// 为什么不放在一次性脚本里跑真进程（上一会话就是这么验的 16 条）：那 16 条不在仓库里，
// 谁也复跑不了，而契约 8.1 要的是"验收标准能被断言"。进程侧的 `--config` 装配面（与 `--upstream`
// 互斥、旧开关已删、示例配置真能跑通）留在 `scripts/smoke-s3-02.sh`，两边加起来才是这条任务。
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "config/gateway_config.h"
#include "config/json.h"
#include "metrics/metrics_render.h"  // S5-09：sanitize_metric_name——键名合法性这件事的裁判
#include "net/socket.h"              // S5-09（ADR-28）：is_ipv4_literal 与 bind 那一侧的对照

#ifndef L7GATE_SOURCE_DIR
#define L7GATE_SOURCE_DIR "."
#endif

namespace {

using l7gate::config::GatewayConfig;
using l7gate::config::JsonValue;

// 解析 + 装配，一次做完（与 `--config` 走的是同一条路径，不是"用例验一份、线上跑另一份"）。
bool build_from(std::string_view text, GatewayConfig* out, std::string* error) {
  JsonValue document;
  if (!l7gate::config::parse_json(text, &document, error)) return false;
  return l7gate::config::build_gateway_config(document, out, error);
}

// 一条该被拒绝的配置。**同时断"话里点到了哪个字段"**——这是 ADR-19 后果段那条
// 「退出码 2 + 一条指出哪一个字段的 stderr 消息」在单测里的形状。
void expect_rejected(std::string_view text, std::string_view needle) {
  GatewayConfig config;
  std::string error;
  const bool ok = build_from(text, &config, &error);
  EXPECT_FALSE(ok) << "这条该被拒绝，却装配成功了：" << text;
  EXPECT_NE(error.find(needle), std::string::npos)
      << "拒绝了，但话里没点到「" << needle << "」：" << error;
}

// 一份只有 upstreams 段的骨架（组级坏输入用它，**故意不带 routes**：带上的话，
// "组名被写坏"这一类用例会先在 routes 的组名校验上炸，一条用例就有两个失败原因）。
std::string groups_only(std::string_view groups_body) {
  return std::string(R"({"upstreams":[)") + std::string(groups_body) + "]}";
}

std::string with_routes(std::string_view groups_body, std::string_view routes_body) {
  return std::string(R"({"upstreams":[)") + std::string(groups_body) + R"(],"routes":[)" +
         std::string(routes_body) + "]}";
}

constexpr std::string_view kApi =
    R"({"name":"api","balance":"round_robin","targets":[{"address":"127.0.0.1","port":18081}]})";

}  // namespace

// --- 一、JSON 子集：写死在 ADR 里的那些"不接受" ------------------------------------

// 重复键。多数库的语义是"后者覆盖前者"，而一份配置不该有那种语义：写了两次 `port` 的人
// 要么没注意、要么在试"最后一行生效"，两种都该被挡在启动前。
TEST(S3_02_ConfigJson, RejectsDuplicateKeyInOneObject) {
  expect_rejected(R"({"port":18081,"port":18082})", "重复的键");
}

// 尾逗号是最常见的手改残留（删掉数组最后一项就剩一个逗号）。
TEST(S3_02_ConfigJson, RejectsTrailingComma) {
  expect_rejected(R"({"a":[1,2,]})", "尾逗号");
  expect_rejected(R"({"a":1,})", "尾逗号");
}

// 注释：这套子集里没有注释。允许 `//` 就等于允许"这一行是给人看的、不是给网关看的"，
// 而配置文件里**只有**被读到的东西算数这件事一旦含糊，注释掉一条路由就不再生效。
TEST(S3_02_ConfigJson, RejectsComments) {
  // 注释不是被"跳过"的，所以 `//` 走到的是"这里期望一个字符串键"那条拒绝——
  // 我要的是它**不静默**，具体报哪一句是解析器的事，这一格钉住的是"过不了"。
  expect_rejected("{ // 一个组\n\"upstreams\":[]}", "期望字符串");
  expect_rejected("{ /* 一个组 */ \"upstreams\":[]}", "期望字符串");
}

// 非整数的一切写法：浮点、科学计数、加号、前导零。端口和 vnodes 都是整数语义，
// `18081.0` 被读成 18081 是"看起来配了、类型其实不对"的典型。
TEST(S3_02_ConfigJson, RejectsAnythingThatIsNotADecimalInteger) {
  expect_rejected(R"({"a":1.0})", "十进制整数");
  expect_rejected(R"({"a":1e3})", "十进制整数");
  // `+1` 落的是"无法识别的值"那一句：解析器在起手就不认加号，还没走到数字那一段。
  expect_rejected(R"({"a":+1})", "无法识别");
  expect_rejected(R"({"a":01})", "前导零");
  expect_rejected(R"({"a":NaN})", "无法识别");
  expect_rejected(R"({"a":Infinity})", "无法识别");
}

// 顶层必须是对象或数组。顶层标量意味着"这份文件根本不是一个配置文档"。
TEST(S3_02_ConfigJson, RejectsTopLevelScalar) {
  expect_rejected(R"("just a string")", "顶层");
  expect_rejected("42", "顶层");
  expect_rejected("null", "顶层");
}

// BOM 是 Windows 编辑器写进去的，肉眼看不见。留着它的表现是"文件明明有内容，解析却说非法字符"，
// 所以要给一句专门的话，而不是混进一般的语法错误里。
TEST(S3_02_ConfigJson, RejectsBomWithItsOwnMessage) {
  expect_rejected(std::string("\xEF\xBB\xBF") + R"({"upstreams":[]})", "BOM");
}

// 转义白名单之外的东西一律拒绝；`\uXXXX` 落在代理对区间也拒绝（子集只做 BMP 内的 UTF-8 编码，
// 允许半个代理对等于允许一份"看起来配了名字、名字其实是乱码"的配置）。
TEST(S3_02_ConfigJson, RejectsEscapesOutsideTheWhitelist) {
  expect_rejected(R"({"a":"\q"})", "转义");
  expect_rejected(R"({"a":"\uD800"})", "代理");
  // 白名单里的必须过：`\u0041` 是 'A'。
  GatewayConfig config;
  std::string error;
  JsonValue document;
  ASSERT_TRUE(l7gate::config::parse_json(R"({"a":"\u0041\/x\\y\t"})", &document, &error)) << error;
  const JsonValue* value = document.find("a");
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(value->text, "A/x\\y\t");
}

// 单引号不是 JSON。
TEST(S3_02_ConfigJson, RejectsSingleQuotes) {
  expect_rejected("{'a':1}", "期望字符串");
}

// 深度上限 32。这一条不是为了刁难人：配置文件是**磁盘上来的输入**，
// 嵌套深度不能由它决定我的栈用量（解析器是递归下降的）。
TEST(S3_02_ConfigJson, RejectsMoreThanThirtyTwoLevels) {
  std::string deep_ok;
  for (int i = 0; i < 32; ++i) deep_ok += "[";
  deep_ok += "1";
  for (int i = 0; i < 32; ++i) deep_ok += "]";
  JsonValue document;
  std::string error;
  EXPECT_TRUE(l7gate::config::parse_json(deep_ok, &document, &error)) << "32 层应当过：" << error;

  std::string deeper = "[" + deep_ok + "]";
  EXPECT_FALSE(l7gate::config::parse_json(deeper, &document, &error));
  EXPECT_NE(error.find("32"), std::string::npos) << error;
}

// --- 二、装配：组与成员 ------------------------------------------------------------

TEST(S3_02_ConfigAssembly, AcceptsAMinimalWorkingConfig) {
  GatewayConfig config;
  std::string error;
  ASSERT_TRUE(build_from(with_routes(kApi, R"({"match":"prefix","pattern":"/a","upstream":"api"})"),
                         &config, &error))
      << error;
  EXPECT_EQ(config.endpoints().size(), 1u);
  EXPECT_EQ(config.groups().size(), 1u);
  EXPECT_EQ(config.routes().size(), 1u);
  EXPECT_TRUE(config.has_routes);
  EXPECT_EQ(config.groups().group_at(0)->balance, l7gate::route::Balance::kRoundRobin);
}

TEST(S3_02_ConfigAssembly, RejectsUnknownFieldInAGroup) {
  // `weight` 是已裁不做的加权。今天它必须是不认识的字段，而不是"读了但没实现"。
  expect_rejected(groups_only(
                      R"({"name":"api","balance":"round_robin","weight":3,"targets":[{"address":"127.0.0.1","port":18081}]})"),
                  "weight");
}

TEST(S3_02_ConfigAssembly, RejectsMisspelledBalance) {
  // 值拼错（`round-robin` 少一个下划线）如果被读成"用缺省策略"，跑起来也是均衡的，
  // 只有语义不是要的那一种——这一族最难查。
  expect_rejected(groups_only(
                      R"({"name":"api","balance":"round-robin","targets":[{"address":"127.0.0.1","port":18081}]})"),
                  "balance");
  // 键名拼错（`banlace`）走的是两条拒绝：不认识的键 + 缺 `balance`。
  expect_rejected(groups_only(
                      R"({"name":"api","banlace":"round_robin","targets":[{"address":"127.0.0.1","port":18081}]})"),
                  "banlace");
}

TEST(S3_02_ConfigAssembly, RejectsEmptyTargets) {
  expect_rejected(groups_only(R"({"name":"api","balance":"round_robin","targets":[]})"),
                  "targets");
}

TEST(S3_02_ConfigAssembly, RejectsEmptyAndDuplicateGroupNames) {
  expect_rejected(groups_only(
                      R"({"name":"","balance":"round_robin","targets":[{"address":"127.0.0.1","port":18081}]})"),
                  "name");
  expect_rejected(groups_only(std::string(kApi) + "," + std::string(kApi)), "api");
}

TEST(S3_02_ConfigAssembly, RejectsTheSameBackendTwiceInOneGroup) {
  // 同组重复成员会让"这个组有几个后端"有两个答案，而 N→N+1 的迁移率判据按"几个后端"算界。
  expect_rejected(groups_only(
                      R"({"name":"api","balance":"round_robin","targets":[{"address":"127.0.0.1","port":18081},{"address":"127.0.0.1","port":18081}]})"),
                  "同一个后端");
}

TEST(S3_02_ConfigAssembly, RejectsBadPortAndEmptyAddress) {
  expect_rejected(groups_only(
                      R"({"name":"api","balance":"round_robin","targets":[{"address":"127.0.0.1","port":0}]})"),
                  "port");
  expect_rejected(groups_only(
                      R"({"name":"api","balance":"round_robin","targets":[{"address":"127.0.0.1","port":70000}]})"),
                  "port");
  // 字符串形式的端口（`"18081"`）是最常见的手抄错误，它必须在类型这一层就被挡。
  expect_rejected(groups_only(
                      R"({"name":"api","balance":"round_robin","targets":[{"address":"127.0.0.1","port":"18081"}]})"),
                  "port");
  expect_rejected(groups_only(
                      R"({"name":"api","balance":"round_robin","targets":[{"address":"","port":18081}]})"),
                  "address");
}

TEST(S3_02_ConfigAssembly, RejectsVnodesOutsideOneToASmallUpperBound) {
  expect_rejected(groups_only(
                      R"({"name":"api","balance":"consistent_hash","vnodes":0,"targets":[{"address":"127.0.0.1","port":18081}]})"),
                  "vnodes");
  expect_rejected(groups_only(
                      R"({"name":"api","balance":"consistent_hash","vnodes":-1,"targets":[{"address":"127.0.0.1","port":18081}]})"),
                  "vnodes");
  expect_rejected(groups_only(
                      R"({"name":"api","balance":"consistent_hash","vnodes":1000001,"targets":[{"address":"127.0.0.1","port":18081}]})"),
                  "vnodes");
  expect_rejected(groups_only(
                      R"({"name":"api","balance":"consistent_hash","vnodes":"160","targets":[{"address":"127.0.0.1","port":18081}]})"),
                  "vnodes");
}

TEST(S3_02_ConfigAssembly, RejectsHashKeyThatIsNeitherTargetNorHeaderName) {
  expect_rejected(groups_only(
                      R"({"name":"api","balance":"consistent_hash","hash_key":"cookie","targets":[{"address":"127.0.0.1","port":18081}]})"),
                  "hash_key");
  // `header:` 后面没有名字：key 变成空串，而空串会把所有请求钉在同一个后端上。
  expect_rejected(groups_only(
                      R"({"name":"api","balance":"consistent_hash","hash_key":"header:","targets":[{"address":"127.0.0.1","port":18081}]})"),
                  "hash_key");
}

TEST(S3_02_ConfigAssembly, DeduplicatesBackendsSharedByTwoGroups) {
  // 两个组共用一个后端（18082）：目标表必须**去重成 3 条**（18081/18082/18083），
  // 否则池会为同一台后端开两个桶，"这台后端有多少条热连接"就有两个答案，
  // 而回源容量公式 `min(N,E) × 单元组容量` 里的 E 也被悄悄抬大了。
  const std::string two_groups =
      R"({"name":"a","balance":"round_robin","targets":[{"address":"127.0.0.1","port":18081},{"address":"127.0.0.1","port":18082}]},{"name":"b","balance":"consistent_hash","targets":[{"address":"127.0.0.1","port":18082},{"address":"127.0.0.1","port":18083}]})";
  GatewayConfig config;
  std::string error;
  ASSERT_TRUE(build_from(groups_only(two_groups), &config, &error)) << error;
  EXPECT_EQ(config.endpoints().size(), 3u) << "共享的那个后端被登记了两次";
  const std::vector<std::uint16_t>* a = config.groups().members_of(0);
  const std::vector<std::uint16_t>* b = config.groups().members_of(1);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  ASSERT_EQ(a->size(), 2u);
  ASSERT_EQ(b->size(), 2u);
  EXPECT_EQ((*a)[1], (*b)[0]) << "两个组里那个共同后端应当指向同一个下标";
}

TEST(S3_02_ConfigAssembly, BuildsTheRingOnlyForTheHashGroup) {
  const std::string two_groups =
      R"({"name":"rr","balance":"round_robin","targets":[{"address":"127.0.0.1","port":18081}],"vnodes":7},{"name":"h","balance":"consistent_hash","targets":[{"address":"127.0.0.1","port":18081},{"address":"127.0.0.1","port":18082}],"vnodes":8})";
  GatewayConfig config;
  std::string error;
  ASSERT_TRUE(build_from(groups_only(two_groups), &config, &error)) << error;
  // 轮询组**不建环**（vnodes 写了也不建）：给没人读的东西建一张 160×成员数 的点表，
  // 是"配置写了、代码没读"那一族坑的又一形态。
  EXPECT_TRUE(config.groups().group_at(0)->ring.empty());
  EXPECT_EQ(config.groups().group_at(1)->ring.size(), 2u * 8u) << "环的点数不是成员数×vnodes";
}

TEST(S3_02_ConfigAssembly, ReadsHashKeyFormsAndKeepsTargetAsDefault) {
  const std::string header_group =
      R"({"name":"h","balance":"consistent_hash","hash_key":"header:X-User-Id","targets":[{"address":"127.0.0.1","port":18081}]})";
  GatewayConfig config;
  std::string error;
  ASSERT_TRUE(build_from(groups_only(header_group), &config, &error)) << error;
  const l7gate::route::HashKey& key = config.groups().group_at(0)->hash_key;
  EXPECT_EQ(key.source, l7gate::route::HashKey::Source::kHeaderName);
  EXPECT_EQ(key.header_name, "X-User-Id");  // 名字原样留着：查找时才做大小写不敏感

  // **第二次装配要换一个干净的 `GatewayConfig`**：`build_gateway_config` 是往 `out` 里
  // **追加**组的（它假设拿到的是新建对象，`main.cc` 就是这么用的），复用同一个对象会在
  // "组名重复"上失败，而那句话读起来像配置的错、其实是用例的错。这一格我自己撞过一次，
  // 记在这里是为了让下一次不用重新发现。
  GatewayConfig target_key_config;
  ASSERT_TRUE(build_from(groups_only(R"({"name":"h","balance":"consistent_hash","hash_key":"target","targets":[{"address":"127.0.0.1","port":18081}]})"),
                         &target_key_config, &error))
      << error;
  EXPECT_EQ(target_key_config.groups().group_at(0)->hash_key.source,
            l7gate::route::HashKey::Source::kTarget);
}

// --- 三、routes 段 ----------------------------------------------------------------

TEST(S3_02_ConfigRoutes, RejectsARouteToPointingAtAnUnknownGroup) {
  expect_rejected(with_routes(kApi, R"({"match":"prefix","pattern":"/a","upstream":"nope"})"),
                  "nope");
}

TEST(S3_02_ConfigRoutes, RejectsAnUncompilableRegexPattern) {
  // 正则在装配期就编，编不过整体失败。留一条"没生效"的规则，请求会安静地落到别的后端。
  expect_rejected(with_routes(kApi, R"({"match":"regex","pattern":"/(","upstream":"api"})"),
                  "pattern");
}

TEST(S3_02_ConfigRoutes, RejectsAnUnknownMatchKind) {
  expect_rejected(with_routes(kApi, R"({"match":"exact","pattern":"/a","upstream":"api"})"),
                  "match");
}

TEST(S3_02_ConfigRoutes, KeepsConfigOrderAndResolvesGroupNamesToIndices) {
  GatewayConfig config;
  std::string error;
  const std::string routes =
      R"({"match":"regex","pattern":"/v[0-9]+/orders","upstream":"shard"},{"match":"prefix","pattern":"/api","upstream":"api"},{"match":"prefix","pattern":"","upstream":"api"})";
  ASSERT_TRUE(build_from(with_routes(std::string(kApi) + "," +
                                     R"({"name":"shard","balance":"consistent_hash","targets":[{"address":"127.0.0.1","port":18082}]})",
                                     routes),
                         &config, &error))
      << error;
  ASSERT_EQ(config.routes().size(), 3u);
  // 顺序 = 数组顺序（链和路由表同一个口径，不加 priority 字段）。
  EXPECT_EQ(config.routes().rule_at(0).kind, l7gate::route::Kind::kRegex);
  EXPECT_EQ(config.routes().rule_at(0).pattern, "/v[0-9]+/orders");
  EXPECT_EQ(config.routes().rule_at(0).upstream_group, 1u) << "shard 是第二个组";
  EXPECT_EQ(config.routes().rule_at(1).upstream_group, 0u);
  EXPECT_EQ(config.routes().rule_at(2).upstream_group, 0u);
  EXPECT_EQ(config.routes().match("/v2/orders"), 1u);
  EXPECT_EQ(config.routes().match("/api/x"), 0u);
}

// 空前缀是合法的 catch-all（`match` 用例已经在 route_test.cc 断过语义，这里断配置面收得下它）。
TEST(S3_02_ConfigRoutes, AcceptsAnEmptyPrefixAsCatchAll) {
  GatewayConfig config;
  std::string error;
  ASSERT_TRUE(build_from(with_routes(kApi, R"({"match":"prefix","pattern":"","upstream":"api"})"),
                         &config, &error))
      << error;
  EXPECT_EQ(config.routes().match("/anything/at/all"), 0u);
}

// `has_routes` 断的是**段在不在**，不是空不空。这个区分是有意的：main.cc 用它决定装不装
// `RouteFilter`，而"装了路由环但一条规则都没有"与"根本没装"在网关里是两个状态
// （前者每条请求 404，后者走默认目标）。今天还没有对"`routes: []` 是否该直接拒绝"的裁决，
// 所以这里只钉字段含义本身。
TEST(S3_02_ConfigRoutes, HasRoutesReportsSectionPresenceNotEmptiness) {
  GatewayConfig absent;
  std::string error;
  ASSERT_TRUE(build_from(groups_only(kApi), &absent, &error)) << error;
  EXPECT_FALSE(absent.has_routes) << "没有 routes 段却被当成「配了路由」，链会装上一条空表";
  EXPECT_TRUE(absent.routes().empty());

  GatewayConfig empty_array;
  ASSERT_TRUE(build_from(with_routes(kApi, ""), &empty_array, &error)) << error;
  EXPECT_TRUE(empty_array.has_routes);
  EXPECT_EQ(empty_array.routes().size(), 0u);

  GatewayConfig present;
  ASSERT_TRUE(build_from(with_routes(kApi, R"({"match":"prefix","pattern":"/a","upstream":"api"})"),
                         &present, &error))
      << error;
  EXPECT_TRUE(present.has_routes);
  EXPECT_EQ(present.routes().size(), 1u);
}

// 缺字段的话里要把**这一层已有的键**打出来（`need()` 那一条）。理由不是好看：配置文件出错时
// 最贵的是"我到底写错了哪一个键"，把兄弟键列出来才能一眼看出是漏写还是拼错。
TEST(S3_02_ConfigRoutes, MissingRequiredFieldListsTheSiblingKeys) {
  expect_rejected(groups_only(R"({"name":"api","targets":[{"address":"127.0.0.1","port":18081}]})"),
                  "balance");
  GatewayConfig config;
  std::string error;
  EXPECT_FALSE(build_from(groups_only(R"({"name":"api","targets":[{"address":"127.0.0.1","port":18081}]})"),
                          &config, &error));
  EXPECT_NE(error.find("name"), std::string::npos) << error;
  EXPECT_NE(error.find("targets"), std::string::npos) << error;
}

// --- 四、文件层 ---------------------------------------------------------------------

// 文件读不到要在话里带上路径——运维拿到退出码 2 之后第一件事就是"它读的到底是哪个文件"。
TEST(S3_02_ConfigFile, MissingFileFailsAndNamesThePath) {
  GatewayConfig config;
  std::string error;
  EXPECT_FALSE(l7gate::config::load_gateway_config(
      "/home/archer/这一份配置文件不存在.json", &config, &error));
  EXPECT_NE(error.find("这一份配置文件不存在.json"), std::string::npos) << error;
}

// **契约第 7 节对"配置文件字段变化"的完成判据是"示例配置能直接跑通"**。这一条是它的静态那一半：
// `deploy/gateway.json` 必须装配得过，而且形状与文档承诺一致（两个组共用一个后端 → 去重成 3 条、
// 一个轮询组 + 一个一致哈希组、三条路由含一条 catch-all）。跑通进程的那一半在
// `scripts/smoke-s3-02.sh`，两边都在才算这一格不是我说说的。
TEST(S3_02_ConfigFile, TheShippedExampleConfigAssembles) {
  const std::string path = std::string(L7GATE_SOURCE_DIR) + "/deploy/gateway.json";
  GatewayConfig config;
  std::string error;
  ASSERT_TRUE(l7gate::config::load_gateway_config(path, &config, &error)) << error;
  EXPECT_EQ(config.endpoints().size(), 3u) << "示例配置里两个组共用一个后端，去重后应当是 3 条";
  ASSERT_EQ(config.groups().size(), 2u);
  EXPECT_EQ(config.groups().group_at(0)->balance, l7gate::route::Balance::kRoundRobin);
  EXPECT_EQ(config.groups().group_at(1)->balance, l7gate::route::Balance::kConsistentHash);
  EXPECT_FALSE(config.groups().group_at(1)->ring.empty()) << "哈希组没有环：示例配置一跑就 502";
  EXPECT_EQ(config.routes().size(), 3u);
  EXPECT_TRUE(config.has_routes);
  // S4-03：示例配置里那一格也要被 ctest 真的装配一遍（字段变了而示例没变，
  // "示例能直接跑通"这句就没有人验）。
  EXPECT_TRUE(config.rate_limit().present);
  EXPECT_EQ(config.rate_limit().rate_per_sec, 1000u);
  EXPECT_EQ(config.rate_limit().burst, 1000u);
  EXPECT_FALSE(config.rate_limit().by_header);
}

// --- S3-03（ADR-19 追加第四条）：health 段与 health_path -----------------------------
// 严格性原样适用：段可选、字段可选，但**认的字段每一个都要类型与取值域对**，
// 不认的字段一个都不进。每条拒绝用例仍然同时断「话里点到了那个字段」。
namespace {

std::string with_health(std::string_view health_body) {
  return std::string(R"({"upstreams":[{"name":"api","balance":"round_robin",)"
                     R"("targets":[{"address":"127.0.0.1","port":18081}]}],"health":)") +
         std::string(health_body) + "}";
}

// 两个组共用一台后端（health_path 一致/冲突的对就建在这上面）。
std::string two_groups_on(std::string_view path_a, std::string_view path_b) {
  auto group = [](std::string_view name, std::string_view path) {
    std::string g = std::string("{\"name\":\"") + std::string(name) +
                    R"(","balance":"round_robin","targets":[{"address":"127.0.0.1","port":18081)";
    if (!path.empty()) g += std::string(",\"health_path\":\"") + std::string(path) + "\"";
    g += "}]}";
    return g;
  };
  return "{\"upstreams\":[" + group("a", path_a) + "," + group("b", path_b) + "]}";
}

}  // namespace

TEST(S3_03_ConfigHealth, SectionAbsentMeansHealthDisabled) {
  GatewayConfig config;
  std::string error;
  ASSERT_TRUE(build_from(groups_only(kApi), &config, &error)) << error;
  EXPECT_FALSE(config.health().present) << "没有 health 段却不许凭空装配健康检查";
}

TEST(S3_03_ConfigHealth, EmptySectionAssemblesWithPolicyDefaults) {
  GatewayConfig config;
  std::string error;
  ASSERT_TRUE(build_from(with_health("{}"), &config, &error)) << error;
  ASSERT_TRUE(config.health().present);
  const l7gate::route::HealthPolicy& policy = config.health().policy;
  EXPECT_EQ(policy.max_fails, 3u);
  EXPECT_EQ(policy.fail_timeout_ms, 10000);
  EXPECT_EQ(policy.probe_interval_ms, 1000);
  EXPECT_EQ(policy.probe_timeout_ms, 500);
  EXPECT_EQ(policy.max_ejection_percent, 10u);
}

TEST(S3_03_ConfigHealth, EveryFieldOverridesAndIntervalZeroDisablesProbes) {
  GatewayConfig config;
  std::string error;
  ASSERT_TRUE(build_from(with_health(
                R"({"max_fails":5,"fail_timeout_ms":2000,"probe_interval_ms":0,)"
                R"("probe_timeout_ms":100,"max_ejection_percent":25})"),
                &config, &error))
      << error;
  const l7gate::route::HealthPolicy& policy = config.health().policy;
  EXPECT_EQ(policy.max_fails, 5u);
  EXPECT_EQ(policy.fail_timeout_ms, 2000);
  EXPECT_EQ(policy.probe_interval_ms, 0) << "0 = 不主动探活，这一格是格式不是缺省";
  EXPECT_EQ(policy.probe_timeout_ms, 100);
  EXPECT_EQ(policy.max_ejection_percent, 25u);
}

TEST(S3_03_ConfigHealth, RejectsUnknownFieldAndOutOfRangeValues) {
  expect_rejected(with_health(R"({"probe_jitter_ms":1})"), "probe_jitter_ms");
  expect_rejected(with_health(R"({"max_fails":0})"), "max_fails");
  expect_rejected(with_health(R"({"max_fails":-1})"), "max_fails");
  expect_rejected(with_health(R"({"fail_timeout_ms":0})"), "fail_timeout_ms");
  expect_rejected(with_health(R"({"probe_interval_ms":-5})"), "probe_interval_ms");
  expect_rejected(with_health(R"({"probe_timeout_ms":0})"), "probe_timeout_ms");
  expect_rejected(with_health(R"({"max_ejection_percent":101})"), "max_ejection_percent");
  expect_rejected(with_health(R"({"max_ejection_percent":"10"})"), "max_ejection_percent");
  expect_rejected(with_health("[]"), "health");
}

TEST(S3_03_ConfigHealth, HealthPathReadBackDefaultsAndRejectsBadShape) {
  GatewayConfig config;
  std::string error;
  ASSERT_TRUE(build_from(groups_only(kApi), &config, &error)) << error;
  ASSERT_EQ(config.endpoints().size(), 1u);
  EXPECT_EQ(config.endpoints()[0].health_path, "/") << "缺省路径是 /（已裁第 3 条①）";

  GatewayConfig with_path;
  ASSERT_TRUE(build_from(
      groups_only(R"({"name":"api","balance":"round_robin","targets":[{"address":"127.0.0.1",)"
                  R"("port":18081,"health_path":"/healthz"}]})"),
      &with_path, &error))
      << error;
  EXPECT_EQ(with_path.endpoints()[0].health_path, "/healthz");

  expect_rejected(
      groups_only(R"({"name":"api","balance":"round_robin","targets":[{"address":"127.0.0.1",)"
                  R"("port":18081,"health_path":""}]})"),
      "health_path");
  expect_rejected(
      groups_only(R"({"name":"api","balance":"round_robin","targets":[{"address":"127.0.0.1",)"
                  R"("port":18081,"health_path":"healthz"}]})"),
      "health_path");
  expect_rejected(
      groups_only(R"({"name":"api","balance":"round_robin","targets":[{"address":"127.0.0.1",)"
                  R"("port":18081,"health_path":3}]})"),
      "health_path");
}

TEST(S3_03_ConfigHealth, SameBackendDifferentHealthPathIsRejectedSamePathDedups) {
  // 同一台机器两个"活着"的定义 → 拒绝。
  expect_rejected(two_groups_on("/healthz", "/livez"), "health_path");
  // 一致的路径照常去重成一条目标。
  GatewayConfig config;
  std::string error;
  ASSERT_TRUE(build_from(two_groups_on("/healthz", "/healthz"), &config, &error)) << error;
  EXPECT_EQ(config.endpoints().size(), 1u);
  EXPECT_EQ(config.endpoints()[0].health_path, "/healthz");
  // 双方都不写（都用缺省 /）也照常去重。
  GatewayConfig both_default;
  ASSERT_TRUE(build_from(two_groups_on("", ""), &both_default, &error)) << error;
  EXPECT_EQ(both_default.endpoints().size(), 1u);
}

// --- S4-05（ADR-12 追加二第三节）：顶层 `circuit` 段 ----------------------------------
// 段不存在 = 熔断只有 S3-03 那一条判据（今天的行为逐字节不变）；段存在则六字段全可选、
// 且**要求 health 段同在**（摘除与安全阀都长在健康表上，已裁第一条）。

namespace {

std::string with_circuit(std::string_view health_body, std::string_view circuit_body) {
  return std::string(R"({"upstreams":[{"name":"api","balance":"round_robin",)"
                     R"("targets":[{"address":"127.0.0.1","port":18081}]}],"health":)") +
         std::string(health_body) + R"(,"circuit":)" + std::string(circuit_body) + "}";
}

}  // namespace

TEST(S4_05_CircuitConfig, SectionAbsentMeansCircuitDisabled) {
  GatewayConfig config;
  std::string error;
  ASSERT_TRUE(build_from(with_health("{}"), &config, &error)) << error;
  ASSERT_TRUE(config.health().present);
  EXPECT_FALSE(config.health().policy.circuit.enabled)
      << "没有 circuit 段凭空装配熔断 = 悄悄改掉到期恢复的语义";
}

TEST(S4_05_CircuitConfig, EmptySectionAssemblesWithPolicyDefaults) {
  GatewayConfig config;
  std::string error;
  ASSERT_TRUE(build_from(with_circuit("{}", "{}"), &config, &error)) << error;
  const l7gate::route::CircuitPolicy& circuit = config.health().policy.circuit;
  ASSERT_TRUE(circuit.enabled);
  EXPECT_EQ(circuit.window_ms, 10000);
  EXPECT_EQ(circuit.minimum_requests, 20u);
  EXPECT_EQ(circuit.error_rate_percent, 50u);
  EXPECT_EQ(circuit.half_open_requests, 3u);
  EXPECT_EQ(circuit.half_open_successes, 2u);
  EXPECT_EQ(circuit.half_open_timeout_ms, 3000);
  // health 段那五个缺省没被 circuit 段挤掉。
  EXPECT_EQ(config.health().policy.max_fails, 3u);
  EXPECT_EQ(config.health().policy.fail_timeout_ms, 10000);
}

TEST(S4_05_CircuitConfig, EveryFieldOverrides) {
  GatewayConfig config;
  std::string error;
  ASSERT_TRUE(build_from(with_circuit(R"({"max_fails":1})",
                                      R"({"window_ms":500,"minimum_requests":7,)"
                                      R"("error_rate_percent":30,"half_open_requests":5,)"
                                      R"("half_open_successes":5,"half_open_timeout_ms":150})"),
                          &config, &error))
      << error;
  const l7gate::route::CircuitPolicy& circuit = config.health().policy.circuit;
  EXPECT_EQ(circuit.window_ms, 500);
  EXPECT_EQ(circuit.minimum_requests, 7u);
  EXPECT_EQ(circuit.error_rate_percent, 30u);
  EXPECT_EQ(circuit.half_open_requests, 5u);
  EXPECT_EQ(circuit.half_open_successes, 5u) << "K 等于 B 是合法边界（每条探测都必须成功）";
  EXPECT_EQ(circuit.half_open_timeout_ms, 150);
}

TEST(S4_05_CircuitConfig, RejectsUnknownFieldTypesAndOutOfRangeNamingTheField) {
  expect_rejected(with_circuit("{}", R"({"bucket_count":8})"), "bucket_count");
  expect_rejected(with_circuit("{}", R"({"window_ms":50})"), "window_ms");
  expect_rejected(with_circuit("{}", R"({"window_ms":0})"), "window_ms");
  expect_rejected(with_circuit("{}", R"({"window_ms":"1000"})"), "window_ms");
  expect_rejected(with_circuit("{}", R"({"minimum_requests":0})"), "minimum_requests");
  // 阈值下限是 1 不是 0：0 = "窗口里有过一条失败就摘"，那句话说的是 max_fails=1。
  expect_rejected(with_circuit("{}", R"({"error_rate_percent":0})"), "error_rate_percent");
  expect_rejected(with_circuit("{}", R"({"error_rate_percent":101})"), "error_rate_percent");
  expect_rejected(with_circuit("{}", R"({"half_open_requests":0})"), "half_open_requests");
  expect_rejected(with_circuit("{}", R"({"half_open_successes":0})"), "half_open_successes");
  expect_rejected(with_circuit("{}", R"({"half_open_timeout_ms":99})"), "half_open_timeout_ms");
  expect_rejected(with_circuit("{}", "[]"), "circuit");
}

// K > B 是把"恢复"配成不可达：一个半开周期最多放 B 条请求，却要求攒到 B 条以上的成功。
// 拒绝的话里必须**同时**点到这两个名字，只点一个的话配置的人得自己猜是哪一条越了界
// （S4-04 那条"同时点名两个名字"的口径）。
TEST(S4_05_CircuitConfig, RejectsSuccessesAboveRequestsNamingBothFields) {
  expect_rejected(with_circuit("{}", R"({"half_open_requests":2,"half_open_successes":3})"),
                  "half_open_successes");
  expect_rejected(with_circuit("{}", R"({"half_open_requests":2,"half_open_successes":3})"),
                  "half_open_requests");
  // 缺省值之间不冲突（B=3、K=2），所以只写一个超界的 successes 也照样拒。
  expect_rejected(with_circuit("{}", R"({"half_open_successes":4})"), "half_open_successes");
}

// `circuit` 没有自己的执行机构：没有 health 段就没有健康表，也就没有摘除、没有阀、
// 没有那条 CAS 状态位可挂。这条同样要同时点名两个段。
TEST(S4_05_CircuitConfig, CircuitSectionWithoutHealthIsRejectedNamingBoth) {
  const std::string text =
      R"({"upstreams":[{"name":"api","balance":"round_robin",)"
      R"("targets":[{"address":"127.0.0.1","port":18081}]}],"circuit":{}})";
  expect_rejected(text, "circuit");
  expect_rejected(text, "health");
}

// --- S3-04（ADR-20 第二节）：组级 source 字段 -----------------------------------------
// 与 targets 互斥、只认字面量 "mysql"；mysql 组在成员到岗前就是空组。

TEST(S3_04_ConfigNodes, SourceAndTargetsAreMutuallyExclusive) {
  expect_rejected(
      R"({"upstreams":[{"name":"api","balance":"round_robin","source":"mysql",)"
      R"("targets":[{"address":"127.0.0.1","port":18081}]}]})",
      "source");
}

TEST(S3_04_ConfigNodes, SourceOnlyAcceptsTheMysqlLiteral) {
  expect_rejected(
      R"({"upstreams":[{"name":"api","balance":"round_robin","source":"file"}]})",
      "source");
  expect_rejected(
      R"({"upstreams":[{"name":"api","balance":"round_robin","source":3}]})",
      "source");
}

TEST(S3_04_ConfigNodes, MysqlGroupAssemblesEmptyUntilMembersArrive) {
  GatewayConfig config;
  std::string error;
  ASSERT_TRUE(build_from(
      R"({"upstreams":[{"name":"api","balance":"consistent_hash","source":"mysql",)"
      R"("vnodes":160}]})",
      &config, &error))
      << error;
  ASSERT_EQ(config.groups().size(), 1u);
  const auto* group = config.groups().group_at(0);
  ASSERT_NE(group, nullptr);
  EXPECT_TRUE(group->source_mysql);
  EXPECT_TRUE(group->targets.empty()) << "成员没到岗就是空组，502 是它的表态";
  EXPECT_TRUE(group->ring.empty()) << "环随第一份有成员的快照发布，装配期不建";
  // 哈希组缺环的 502 语义由 S3_02_LoadBalanceFilter.HashGroupWithoutRingRejectsFiveOhTwo 钉着，
  // mysql 组到岗前的行为与它同一条路。
}

// --- S4-03（ADR-21 第二/四节）：rate_limit 段 ---------------------------------------
// 段可选；段存在时 rate/burst **必填**（不给限流编缺省值）；key 可选、缺省 path。
// `ip` 是**具名推迟**（ADR-21 第四节第 1 条），不是未知字段——话里必须点到它，
// 免得读的人以为"配了 ip 却没生效"。

namespace {

std::string with_rate_limit(std::string_view limit_body) {
  return std::string(R"({"upstreams":[{"name":"api","balance":"round_robin",)"
                     R"("targets":[{"address":"127.0.0.1","port":18081}]}],"rate_limit":)") +
         std::string(limit_body) + "}";
}

}  // namespace

TEST(S4_03_ConfigRateLimit, ParsesRateBurstAndKey) {
  {
    GatewayConfig config;
    std::string error;
    ASSERT_TRUE(build_from(with_rate_limit(R"({"rate":10,"burst":20})"), &config, &error))
        << error;
    EXPECT_TRUE(config.rate_limit().present);
    EXPECT_EQ(config.rate_limit().rate_per_sec, 10u);
    EXPECT_EQ(config.rate_limit().burst, 20u);
    EXPECT_FALSE(config.rate_limit().by_header) << "key 缺省是 path";
  }
  {
    GatewayConfig config;  // 每次装一份新的：组表在对象里累积，复用会把 name 判成重复
    std::string error;
    ASSERT_TRUE(build_from(with_rate_limit(R"({"rate":5,"burst":7,"key":"header:X-Api-Key"})"),
                           &config, &error))
        << error;
    EXPECT_TRUE(config.rate_limit().by_header);
    EXPECT_EQ(config.rate_limit().header_name, "X-Api-Key");
  }
  {
    // 段不存在 = 不装限流（与 health 同形）。
    GatewayConfig config;
    std::string error;
    ASSERT_TRUE(build_from(groups_only(R"({"name":"api","balance":"round_robin",)"
                                       R"("targets":[{"address":"127.0.0.1","port":18081}]})"),
                           &config, &error))
        << error;
    EXPECT_FALSE(config.rate_limit().present);
  }
}

TEST(S4_03_ConfigRateLimit, RejectsBadSectionsAndParameters) {
  expect_rejected(with_rate_limit(R"({"burst":20})"), "rate");
  expect_rejected(with_rate_limit(R"({"rate":10})"), "burst");
  expect_rejected(with_rate_limit(R"({"rate":0,"burst":20})"), "rate");
  expect_rejected(with_rate_limit(R"({"rate":10,"burst":0})"), "burst");
  expect_rejected(with_rate_limit(R"({"rate":1000001,"burst":1})"), "rate");
  expect_rejected(with_rate_limit(R"({"rate":1,"burst":1000001})"), "burst");
  expect_rejected(with_rate_limit(R"({"rate":"10","burst":1})"), "rate");
  expect_rejected(with_rate_limit(R"({"rate":10,"burst":1,"keys":"path"})"), "keys");
  expect_rejected(with_rate_limit("[]"), "rate_limit");
  // `ip` 具名推迟：话里点到 ip（并指向 ADR-21 第四节），不是当未知字段糊掉。
  expect_rejected(with_rate_limit(R"({"rate":10,"burst":1,"key":"ip"})"), "ip");
  expect_rejected(with_rate_limit(R"({"rate":10,"burst":1,"key":"target"})"), "key");
  expect_rejected(with_rate_limit(R"({"rate":10,"burst":1,"key":"header:"})"), "key");
  expect_rejected(with_rate_limit(R"({"rate":10,"burst":1,"key":"header:a b"})"), "key");
}

// ===========================================================================
// S5-09（ADR-28）：`address` 只收 IPv4 字面量。
//
// 这条检查存在的原因不是"想限制配置"，而是那个值会被**原样拼进指标名**
// （`health_t<i>_<address>_<port>_<metric>`），连字符在 Prometheus 名字法
// `[a-zA-Z_:][a-zA-Z0-9_:]*` 里非法，后果是整份 `/metrics` 抓失败——
// S5-05 的退化 C1 实测（FAULT-INJECTION §24.1）：那台网关 101 个键一起丢。
// ===========================================================================

namespace {

// Prometheus 的名字法，手写检查（不引 <regex>：这里只判字符集合，正则反而把简单事弄复杂）。
bool is_legal_metric_name(std::string_view name) {
  if (name.empty()) return false;
  const auto lead_ok = [](char c) {
    return c == ':' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
  };
  const auto tail_ok = [&lead_ok](char c) {
    return lead_ok(c) || (c >= '0' && c <= '9');
  };
  if (!lead_ok(name[0])) return false;
  for (char c : name.substr(1)) {
    if (!tail_ok(c)) return false;
  }
  return true;
}

// 一份只有一个 target 的最小配置，地址是唯一变量。
// 参数收的是**带引号的 JSON 值令牌**（拒绝用例要能把原样输入写出来，包括那对引号）。
std::string one_target(std::string_view address_token) {
  return groups_only(R"({"name":"api","balance":"round_robin","targets":[{"address":)" +
                     std::string(address_token) + R"(,"port":18081}]})");
}

// 给裸地址套上 JSON 的那对引号（上面那个 one_target 收的是**带引号**的形状，
// 因为拒绝用例要直接把原样输入写出来；这里手头是 std::string，别在两处各写一遍转义）。
std::string quote(std::string_view bare) { return std::string("\"") + std::string(bare) + "\""; }

}  // namespace

// 上界：非字面量一律拒，而且话里要点到**实际读到的值**（不是"整个文件读不懂"）。
TEST(S5_09_AddressLiteral, RejectsAnythingTheSocketLayerCannotParse) {
  for (const char* bad : {R"("localhost")", R"("upstream-a")", R"("127.0.0.1:8081")",
                          R"("::1")", R"("999.1.1.1")", R"("1.2.3")", R"("172.31.250.21 ")",
                          R"("1.2.3.4.5")"}) {
    // needle 用的是消息里的固定那半句；值本身在另一半。
    expect_rejected(one_target(bad), "不是 IPv4 字面量");
  }
}

// 下界：合法字面量必须照过——否则"恒拒载"这种退化会一路绿到线上。
TEST(S5_09_AddressLiteral, AcceptsEveryLiteralShape) {
  for (const char* good : {R"("127.0.0.1")", R"("0.0.0.0")", R"("172.31.250.21")",
                            R"("255.255.255.255")"}) {
    GatewayConfig config;
    std::string error;
    EXPECT_TRUE(build_from(one_target(good), &config, &error))
        << "这条该被接受，却被拒了：" << good << " → " << error;
  }
}

// 混合：一个合法一个非法 → **整份**拒，而且点名的必须是那一条非法的下标（不许半套）。
TEST(S5_09_AddressLiteral, RejectsWholeDocumentWhenAnyTargetIsBad) {
  expect_rejected(groups_only(R"({"name":"api","balance":"round_robin","targets":[)"
                              R"({"address":"127.0.0.1","port":18081},)"
                              R"({"address":"upstream-b","port":18082}]})"),
                  "targets[1]");
  // 反过来排（非法在前）也要红在同一件事上，而不是先撞别的检查。
  expect_rejected(groups_only(R"({"name":"api","balance":"round_robin","targets":[)"
                              R"({"address":"upstream-b","port":18081},)"
                              R"({"address":"127.0.0.1","port":18082}]})"),
                  "targets[0]");
}

// 谓词与消费者不许分叉：这里拿**监听那一侧**当对照（bind 端口 0，不碰网络），
// 断的是"校验说合法的集合"与"socket 层真能用的集合"在这一批样本上一致。
// bind 用得了的地址只有本机真有的那两个——`127.0.0.1` 与 `0.0.0.0`；
// `172.31.250.21` 是编排子网里的地址，宿主上没有，把它放进这一臂会红在 bind 上，
// 而红的仍是我的取数方式，不是被测方（上一发就是这么写的，照记）。
TEST(S5_09_AddressLiteral, PredicateAgreesWithTheSocketLayerItGuards) {
  for (const char* good : {"127.0.0.1", "0.0.0.0"}) {
    EXPECT_TRUE(l7gate::net::is_ipv4_literal(good)) << good;
    auto socket = l7gate::net::make_listen_socket(good, 0, 4);
    EXPECT_TRUE(socket.ok()) << good << " 字面量过不了 bind，谓词就是在说谎";
  }
  // 谓词说合法、而 bind 用不了的那一类（本机没这个地址）不在这一臂里——它不是"不合法"。
  EXPECT_TRUE(l7gate::net::is_ipv4_literal("172.31.250.21"));
  for (const char* bad : {"localhost", "upstream-a", "127.0.0.1:8081", "::1", "999.1.1.1"}) {
    EXPECT_FALSE(l7gate::net::is_ipv4_literal(bad)) << bad;
  }
}

// C1 那件事的正身：**校验放过的地址，拼出来的键名必须合法**；而每一个被它拒掉的
// "sanitize 管不到的字符"型地址，若真放过去，键名就会非法。两组同时成立才说明
// **挡住它的是这道校验，不是渲染层那点运气**——摘掉 gateway_config.cc 那条检查，组 B 第一条就红。
TEST(S5_09_AddressLiteral, WhateverPassesTheValidatorYieldsLegalPrometheusNames) {
  for (const std::string address : {"127.0.0.1", "0.0.0.0", "172.31.250.21", "255.255.255.255"}) {
    GatewayConfig config;
    std::string error;
    ASSERT_TRUE(build_from(one_target(quote(address)), &config, &error)) << error;
    const std::string key =
        "health_t0_" + l7gate::metrics::sanitize_metric_name(address + "_18081") + "_ejections";
    EXPECT_TRUE(is_legal_metric_name(key)) << "校验放过了它，键名却是非法的：" << key;
  }
  // 组 B 只收**机械变换管不到的**字符：`.` 与 `:` 是 ADR-24 第五节已经换掉的两类，
  // 把它们写进来会得到一条假红（我上一发拿 `localhost` 断这件事就是假红——它字母组成，
  // 拼出来的键名本来就合法，这条用例当时断错了对象）。
  for (const std::string address : {"upstream-a", "my host", "a%b"}) {
    GatewayConfig config;
    std::string error;
    ASSERT_FALSE(build_from(one_target(quote(address)), &config, &error))
        << "这个地址本该被拒（放过去会让整份 /metrics 非法）：" << address;
    const std::string key =
        "health_t0_" + l7gate::metrics::sanitize_metric_name(address + "_18081") + "_ejections";
    EXPECT_FALSE(is_legal_metric_name(key))
        << "它键名居然是合法的——那这一组样本选错了，挡住它的不是校验：" << key;
  }
}

