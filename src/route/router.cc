#include "route/router.h"

#include <regex>
#include <string>
#include <utility>

namespace l7gate::route {
namespace {

// 模式语法写死成 ECMAScript（`std::regex` 的默认值，显式写出来是因为"默认"是库的
// 属性、不是我的口径），加 `optimize`：这张表是**装配期编译一次、每条请求都用它**。
// B-008 那格每请求 ≈8.7 µs 的净赤字里，我不给路由再加一条"每条请求现编正则"的路。
// 类型取 `std::regex_constants::syntax_option_type`——libstdc++ 的 `basic_regex` 没有
// 把那个 typedef 挂成自己的成员（实测 GCC 15.2 报 "does not name a type"）。
constexpr std::regex_constants::syntax_option_type kSyntax =
    std::regex_constants::ECMAScript | std::regex_constants::optimize;

}  // namespace

bool RouteTable::add_prefix(std::string_view prefix, std::uint16_t upstream_target) noexcept {
  if (upstream_target == kNoUpstreamTarget) return false;  // 哨兵不是可用的后端下标
  rules_.push_back(Rule{
      .kind = Kind::kPrefix,
      .pattern = std::string(prefix),
      .upstream_target = upstream_target,
      .compiled_regex = nullptr,  // 前缀规则不编译模式；显式写出来，-Wmissing-field-initializers
                                  // 才有地方闭嘴，而它闭嘴的意义是"这条真的是没填而不是漏填"
  });
  return true;
}

bool RouteTable::add_regex(std::string_view pattern, std::uint16_t upstream_target) {
  if (upstream_target == kNoUpstreamTarget) return false;
  std::shared_ptr<const std::regex> compiled;
  try {
    compiled = std::make_shared<std::regex>(std::string(pattern), kSyntax);
  } catch (const std::regex_error&) {
    return false;  // 不留半条规则：装配方必须整体失败（理由见头文件）
  }
  rules_.push_back(Rule{
      .kind = Kind::kRegex,
      .pattern = std::string(pattern),
      .upstream_target = upstream_target,
      .compiled_regex = std::move(compiled),
  });
  return true;
}

std::optional<std::uint16_t> RouteTable::match(std::string_view request_target) const noexcept {
  // **一、正则先赢**，按配置顺序取第一条命中（头文件里那条 catch-all 遮蔽的理由）。
  for (const Rule& rule : rules_) {
    if (rule.kind != Kind::kRegex) continue;
    const std::shared_ptr<const void>& holder = rule.compiled_regex;
    if (holder == nullptr) continue;  // 装配期漏填：这条规则当不存在，不猜一个模式
    const auto* re = static_cast<const std::regex*>(holder.get());
    // 用迭代器版而不是 `std::string` 版：后者每条请求复制一次 URI，而这一格正落在
    // 每请求成本的分母上。
    //
    // 选 `regex_match` 而不是 `regex_search` 不是风格：match 的语义就是**全串锚定**，
    // 而 search 的"模式出现在任何位置都算命中"会让 `/ap` 命中 `/api/x`。
    // RouteTest.RegexIsAnchoredNotSubstring 钉的就是这条差别。
    if (std::regex_match(request_target.begin(), request_target.end(), *re)) {
      return rule.upstream_target;
    }
  }

  // **二、前缀取最长命中，不在这里排序、也不"第一条命中就返回"。**
  // 这条函数最容易被"顺手优化"成的形状就是按字典序排一遍再取第一条——那会让
  // `/api/v2/users` 落到 `/api` 的后端（规则 `/api` 在字典序里排在 `/api/v2` 前面）。
  // 用例 LongestPrefixWinsOverLexicographicOrder 用一对"字典序会答错"的样本钉住它。
  bool found = false;
  std::size_t best_len = 0;
  std::uint16_t best_target = kNoUpstreamTarget;
  for (const Rule& rule : rules_) {
    if (rule.kind != Kind::kPrefix) continue;
    // view 指向 rule.pattern 的内部缓冲。表在 run() 之前装配完、运行期不再改（和
    // filter 链不许运行期改同一条时序纪律），所以这里不会悬垂。
    const std::string_view prefix(rule.pattern);
    if (!request_target.starts_with(prefix)) continue;
    // `!found` 那一半是给**空前缀（catch-all）**留的：它的长度是 0，`prefix.size() >
    // best_len` 永远不成立，只比长度就永远选不上它——而"一条 `/` 兜住其余全部请求"
    // 正是路由表最常见的形状。等长时取配置顺序的第一条（同一个"顺序=添加顺序"口径）。
    if (!found || prefix.size() > best_len) {
      found = true;
      best_len = prefix.size();
      best_target = rule.upstream_target;
    }
  }
  if (found) return best_target;
  return std::nullopt;  // 没有任何规则命中：调用方按 ADR-18 第二节回 404
}

RouteTable::KindView RouteTable::rule_at(std::size_t index) const noexcept {
  if (index >= rules_.size()) return {};
  const Rule& rule = rules_[index];
  return KindView{
      .kind = rule.kind,
      .pattern = std::string_view(rule.pattern),
      .upstream_target = rule.upstream_target,
      .valid = true,
  };
}

}  // namespace l7gate::route
