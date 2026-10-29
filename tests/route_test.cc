// S3-01（ADR-18）：路由决策的用例。分两层写在一个文件里，因为这两层都不需要 socket：
//   一、`route::RouteTable::match` —— 纯函数本体，规则语义全部在这里断；
//   二、`filter::RouteFilter` —— 它怎么把命中结果交回链（写哪两个字段、什么时候 kStop）。
// "路由改过的目标真的被 `ensure_upstream` 读到"那半条验收**不在这里**，在 reactor_test.cc
// 的 `S3_01_Routing`：需要真连接与事件循环的断言不放进纯函数文件（1.16 那条分工，
// S1-06/07/08 一直是这么切的）。
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "filter/chain.h"
#include "filter/route_filter.h"
#include "http/request_parser.h"
#include "http/response_framing.h"
#include "route/router.h"

namespace {

using l7gate::filter::Decision;
using l7gate::filter::FilterChain;
using l7gate::filter::RequestInfo;
using l7gate::filter::RouteFilter;
using l7gate::route::kNoUpstreamTarget;
using l7gate::route::Kind;
using l7gate::route::RouteTable;

// 一台"命中即返回其下标"的表，逐条加规则。返回值一律走 optional，所以"没命中"
// 与"命中了下标 0"是可区分的——这条性质本身也要断（见 NoRulesMatchNothing）。
std::optional<std::uint16_t> hit(const RouteTable& table, std::string_view target) {
  return table.match(target);
}

}  // namespace

// --- 一、前缀：最长优先 ----------------------------------------------------------

// 这条是 ADR-18 里"字典序命中会当场红"那句的可执行形式。
// 样本集是照着那个失败形状挑的：三条规则按字典序排是 `/ap` < `/api` < `/api/v2`，
// 所以"排序后取第一条命中"的实现会把 `/api/v2/users` 送到 `/ap` 的后端（2）。
// 最长匹配给的是 1。**添加顺序也故意不是长度顺序**，否则"根本没排序、只是恰好
// 长的在前面"也能过。
TEST(S3_01_RoutePrefix, LongestMatchWinsNotLexicographicOrder) {
  RouteTable table;
  ASSERT_TRUE(table.add_prefix("/ap", 2));
  ASSERT_TRUE(table.add_prefix("/api/v2", 1));
  ASSERT_TRUE(table.add_prefix("/api", 0));
  ASSERT_EQ(table.size(), 3u);

  EXPECT_EQ(hit(table, "/api/v2/users"), 1u)
      << "不是最长优先：字典序 + 第一条命中的实现会把这条送到 /ap（下标 2）";
  EXPECT_EQ(hit(table, "/api/orders"), 0u);
  EXPECT_EQ(hit(table, "/apple"), 2u);
}

// "第一条命中就返回"是另一个失败形状：把最长的那条放在**后面**加，
// 顺序扫描就会先撞上一条短的。
TEST(S3_01_RoutePrefix, LongerRuleAddedLaterStillWins) {
  RouteTable table;
  ASSERT_TRUE(table.add_prefix("/api", 0));
  ASSERT_TRUE(table.add_prefix("/api/v2", 1));
  EXPECT_EQ(hit(table, "/api/v2/users"), 1u)
      << "第一条命中就返回的实现会在这里给出 0（短规则先被扫到）";
  EXPECT_EQ(hit(table, "/api/v3/users"), 0u);
}

// 空前缀 = catch-all。这一格是**实现最容易写错的一条**：只比"谁更长"的话，
// 长度 0 永远赢不了自己初值 0，于是表里明明有一条兜底规则、请求却得到"无匹配"→404。
// reactor.cc 里那个 `!found ||` 就是为了这一条存在的。
TEST(S3_01_RoutePrefix, EmptyPrefixIsCatchAll) {
  RouteTable table;
  ASSERT_TRUE(table.add_prefix("", 7));
  EXPECT_EQ(hit(table, "/anything"), 7u);
  EXPECT_EQ(hit(table, "/"), 7u);

  // 有别的规则时它仍然兜住那些没人命中的：/b 谁都不挨着，只剩 catch-all。
  ASSERT_TRUE(table.add_prefix("/a", 3));
  EXPECT_EQ(hit(table, "/a/x"), 3u);
  EXPECT_EQ(hit(table, "/b/x"), 7u);

  // 而 `/`（长度 1）比空串更"具体"，同一条请求上它该赢——长度顺序在这里是真的。
  ASSERT_TRUE(table.add_prefix("/", 9));
  EXPECT_EQ(hit(table, "/b/x"), 9u) << "catch-all 之间取最长，不是取第一条";
}

// 等长的两条规则：按添加顺序取第一条。这条断言的意义不在"等长"多常见，
// 在于它把"顺序 = 添加顺序"这条链口径一直守到路由表这一层（S1-08 给链定的规矩）。
TEST(S3_01_RoutePrefix, EqualLengthTakesFirstAdded) {
  RouteTable table;
  ASSERT_TRUE(table.add_prefix("/ab", 1));
  ASSERT_TRUE(table.add_prefix("/aB", 2));
  ASSERT_TRUE(table.add_prefix("/ab", 3));  // 与第一条完全同键，后来者不该顶掉它
  EXPECT_EQ(hit(table, "/ab/c"), 1u);
}

// --- 二、正则：全串锚定 ----------------------------------------------------------

// `regex_search` 与 `regex_match` 的差别就钉在这里：模式 `/ap` 在 search 语义下
// 命中 `/api/x`，在 match（全串锚定）语义下不命中。选后者是 ADR-18 第三节定死的口径。
TEST(S3_01_RouteRegex, IsAnchoredNotSubstring) {
  RouteTable table;
  ASSERT_TRUE(table.add_regex("/ap", 1));
  EXPECT_FALSE(hit(table, "/api/x").has_value())
      << "这是 search 语义：模式出现在串里就算命中，而锚定语义要求整条 target 匹配完";

  ASSERT_TRUE(table.add_regex("/health", 2));
  EXPECT_EQ(hit(table, "/health"), 2u);
  EXPECT_FALSE(hit(table, "/healthz").has_value());
}

// query 也参与匹配（匹配的是**原始** origin-form，含 query、不切 `?`、不解码）。
// 这条断言的价值是把"到底匹配什么"钉成一个可断言的东西，而不是留给读代码的人猜。
TEST(S3_01_RouteRegex, QueryParticipatesBecauseTargetIsRaw) {
  RouteTable table;
  ASSERT_TRUE(table.add_regex(R"(/search\?q=api)", 1));
  EXPECT_EQ(hit(table, "/search?q=api"), 1u);
  EXPECT_FALSE(hit(table, "/search").has_value());
  // 前缀同理：query 是串的一部分。
  RouteTable prefix_table;
  ASSERT_TRUE(prefix_table.add_prefix("/search?q=", 2));
  EXPECT_EQ(prefix_table.match("/search?q=api"), 2u);
}

// 正则**优先于**前缀（哪怕前缀更长）。这一条把 ADR-18 里那个取舍钉死：
// 反过来定的话，路由表里一条最常见的 `/` 兜底就会把所有正则规则全部遮蔽。
TEST(S3_01_RouteRegex, RegexBeatsAnyPrefixMatch) {
  RouteTable table;
  ASSERT_TRUE(table.add_prefix("/", 0));
  ASSERT_TRUE(table.add_prefix("/health/probe", 5));
  ASSERT_TRUE(table.add_regex("/health", 1));
  EXPECT_EQ(hit(table, "/health"), 1u);
  // 前缀命中的更长时才轮到自己：/health/probe 没有正则覆盖它。
  EXPECT_EQ(hit(table, "/health/probe"), 5u);
  EXPECT_EQ(hit(table, "/other"), 0u);
}

// 两条正则都能命中同一条 target 时，取**配置顺序**的第一条（不是"更具体的那条"——
// 我没有定义什么叫更具体，定义了就得再解释一次解释）。
TEST(S3_01_RouteRegex, FirstAddedRegexWins) {
  RouteTable table;
  ASSERT_TRUE(table.add_regex(R"(/v[0-9]+/users)", 1));
  ASSERT_TRUE(table.add_regex(R"(/v1/users)", 2));
  EXPECT_EQ(hit(table, "/v1/users"), 1u);
}

// --- 三、装配期的拒绝面 ----------------------------------------------------------

// 非法模式必须在**加进来这一刻**被挡住，且不留半条规则。理由写在 router.h：
// 一条编译不过的规则退化成"这条没生效"，观测上和"没有路由"长得一模一样。
TEST(S3_01_RouteAssembly, InvalidRegexLeavesNoRule) {
  RouteTable table;
  EXPECT_FALSE(table.add_regex("/(", 1));      // 未闭合的分组
  EXPECT_FALSE(table.add_regex("/a)", 2));     // 多出来的闭括号
  EXPECT_EQ(table.size(), 0u) << "失败的 add_regex 不许留下半条规则";
  EXPECT_FALSE(hit(table, "/(").has_value());

  ASSERT_TRUE(table.add_regex("/ok", 3));
  EXPECT_EQ(table.size(), 1u);
  EXPECT_EQ(table.rule_at(0).upstream_target, 3u);
  EXPECT_EQ(table.rule_at(0).kind, Kind::kRegex);
}

// 哨兵不是合法下标。main.cc 的 `--route /a=65535` 因此在这一步就被挡掉，
// 而不是变成一个"看起来配了、实际指向哨兵"的规则。
TEST(S3_01_RouteAssembly, SentinelTargetIsRejected) {
  RouteTable table;
  EXPECT_FALSE(table.add_prefix("/a", kNoUpstreamTarget));
  EXPECT_FALSE(table.add_regex("/a", kNoUpstreamTarget));
  EXPECT_EQ(table.size(), 0u);
}

// rule_at 是 main.cc 那条启动前校验（规则下标 vs 端点数）唯一的读法。
// 它必须把 kind/pattern/target 三项都如实还原——校验的是 target，而报错时要能说清
// 是哪一条规则，所以 pattern 也得在。
TEST(S3_01_RouteAssembly, RuleAtRoundTripsForTheValidator) {
  RouteTable table;
  ASSERT_TRUE(table.add_prefix("/api/v2", 1));
  ASSERT_TRUE(table.add_regex(R"(/health[0-9]*)", 4));

  const auto first = table.rule_at(0);
  ASSERT_TRUE(first.valid);
  EXPECT_EQ(first.kind, Kind::kPrefix);
  EXPECT_EQ(first.pattern, "/api/v2");
  EXPECT_EQ(first.upstream_target, 1u);

  const auto second = table.rule_at(1);
  ASSERT_TRUE(second.valid);
  EXPECT_EQ(second.kind, Kind::kRegex);
  EXPECT_EQ(second.pattern, R"(/health[0-9]*)");
  EXPECT_EQ(second.upstream_target, 4u);

  EXPECT_FALSE(table.rule_at(2).valid) << "越界不越读";
}

// 纯函数那条前提在这里兑现两次：空表要能明确区分"没命中"和"命中了下标 0"，
// 而同一个输入重复喂必须给同一个输出（S1-03 给解析器立的是同一条规矩）。
TEST(S3_01_RoutePurity, NoMatchIsNotIndexZeroAndIsRepeatable) {
  RouteTable empty;
  EXPECT_FALSE(hit(empty, "/anything").has_value()) << "空表返回的是 nullopt，不是 0";

  RouteTable table;
  ASSERT_TRUE(table.add_prefix("/x", 0));  // 命中 0 —— 与"没命中"必须是两个不同的答案
  EXPECT_TRUE(hit(table, "/x/y").has_value());
  EXPECT_EQ(hit(table, "/x/y").value(), 0u);
  EXPECT_FALSE(hit(table, "/z").has_value());

  for (int i = 0; i < 100; ++i) {
    ASSERT_EQ(hit(table, "/x/y"), 0u) << "第 " << i << " 次喂同一个输入得到了不同答案";
  }
}

// --- 四、RouteFilter：它往链的写回通道里放什么 ----------------------------------

namespace {

RequestInfo info_for(std::string_view target) {
  static const std::vector<l7gate::http::HeaderEntry> headers{
      l7gate::http::HeaderEntry{"Host", std::string_view("gateway.test")},
  };
  RequestInfo info;
  info.method = "GET";
  info.target = target;  // 生命周期：用例里的 target 串都是字面量或活到断言之后的局部串
  info.headers = std::span<const l7gate::http::HeaderEntry>(headers);
  info.request_bytes = 42;
  info.downstream_fd = 17;
  return info;
}

}  // namespace

// 命中 → 写下标、放行；**不写 reject_status**。反过来说：一个放行的 filter 若顺手
// 写了状态码，主路径也不会理它（主路径只在 kStop 那一支读它）——但这条断言存在的
// 意义是"路由命中时的写入面只有一个字段"，别多写。
TEST(S3_01_RouteFilter, HitWritesTargetAndContinues) {
  RouteTable table;
  ASSERT_TRUE(table.add_prefix("/api", 2));
  RouteFilter filter(table);
  FilterChain chain;
  chain.add(std::make_unique<RouteFilter>(table));

  RequestInfo info = info_for("/api/v1/orders");
  ASSERT_EQ(chain.run(info), Decision::kContinue);
  EXPECT_EQ(info.upstream_target, 2u);
  EXPECT_EQ(info.reject_status, 0u);

  // 直接用 filter 也必须是同一个答案（链在这里只是转发决定，不该改变语义）。
  RequestInfo single = info_for("/api");
  ASSERT_EQ(filter.on_request(single), Decision::kContinue);
  EXPECT_EQ(single.upstream_target, 2u);
}

// 无匹配 → 404 + kStop，**且不写 upstream_target**。
// 后半句不是多余的：主路径在 kStop 那一支读的是 reject_status，但如果 filter 同时也写了
// 一个下标，那"这条请求被拦下了"和"这条请求有个去处"就会同时成立——S3-02 的负载均衡
// 一旦把两者分开读，这就是一个把请求发给后端的静默泄漏。
TEST(S3_01_RouteFilter, NoMatchRejectsFourOhFourWithoutTarget) {
  RouteTable table;
  ASSERT_TRUE(table.add_prefix("/api", 2));
  ASSERT_TRUE(table.add_regex("/health", 3));
  RouteFilter filter(table);

  RequestInfo info = info_for("/somewhere/else");
  ASSERT_EQ(filter.on_request(info), Decision::kStop);
  EXPECT_EQ(info.reject_status, 404u);
  EXPECT_EQ(info.upstream_target, kNoUpstreamTarget)
      << "拦下的那条请求不许留下目标下标";
}

// 404 这个数不是"filter 想回什么就回什么"：它必须是 `gateway_error_reply()` 表里有货的
// 那个码，否则 `fail_pair_with_reply` 拿到空 view 会**不发也不计数**（那条 `reply.empty()`
// 早退是设计上的拒绝）。这一格把两层的约定钉在一起，免得日后有人把这里改成 503。
TEST(S3_01_RouteFilter, RejectStatusMustBeASelfProducedReplyWeActuallyHave) {
  RouteTable table;
  ASSERT_TRUE(table.add_prefix("/api", 0));
  RouteFilter filter(table);
  RequestInfo info = info_for("/nope");
  ASSERT_EQ(filter.on_request(info), Decision::kStop);
  EXPECT_FALSE(l7gate::http::gateway_error_reply(info.reject_status).empty())
      << "写了表里没有的状态码 = 客户端什么也收不到，而网关计数说它发了";
}

// 隧道路径不经过链（ADR-09 / 1.16）。这一格在 reactor_test.cc 的
// `S3_01_Routing.TunneledConnectionNeverAsksTheRouteFilter` 里断，因为它需要真握手；
// 这里不放假版本：一个不调用 socket 的用例断不出"链一次都没被调用"。
