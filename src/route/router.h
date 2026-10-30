#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// 路由决策（S3-01，ADR-18）。
//
// 为什么它是纯函数、而且必须是纯函数：这条继承的是 S1-03 给请求解析器立的规矩——
// 判定逻辑一旦和 socket 状态混在一起，我就只能用真连接去测它，测出来的失败分不清
// "路由错了"还是"时序没对上"。所以这里入参只有 (规则表, target 串)，不发 syscall、
// 不持有跨调用状态；重复喂同一条 target 必须得到完全相同的结果。这条性质让下面那批
// 规则语义用例（最长匹配优先、锚定、catch-all 空串）能在一个不建 socket 的文件里跑完。
//
// 匹配对象是解析器给的**原始 origin-form target**（`GET /a/b?c=1` 里那截 `/a/b?c=1`）：
// 含 query、不 URL 解码、不做路径规范化。这是安全选择而不是省事——`%2F`、`..`、大小写
// 折叠任何一项归一化都可能让"我路由掉的那个路径"和"上游自己解析到的路径"不是同一个，
// 那是请求走私的家族。归一化要做的话，得连"上游侧会看到什么"一起重新裁决。
//
// 这里只有"这条请求属于哪条路由"，**没有**负载均衡：一条路由固定对应一个目标下标，
// 一条路由对应多个后端、按请求在轮询/最少连接/一致性哈希里选，是 S3-02（ADR-19）。
namespace l7gate::route {

// "没有 filter 决定过目标"的哨兵。调用方（reactor）看到它就用自己的默认目标——
// 这条是 S2-06a 那个 `worker i → 端点 i mod E` 静态指派在新形状下的落点：**没有路由
// filter 时行为逐字节等于 S2**，容量公式与八份 canonic 档因此都不作废（ADR-18 后果段）。
inline constexpr std::uint16_t kNoUpstreamTarget = 0xffff;

// 0xffff 是哨兵，所以能用的下标空间少一个；这里把它写成断言而不是注释，
// 免得日后有人把 0xffff 当"第 65535 个后端"用。
inline constexpr std::uint16_t kMaxUpstreamTarget =
    static_cast<std::uint16_t>(kNoUpstreamTarget - 1);

// 上游组的下标空间，与 `upstream_target` 同一套哨兵写法。**两个哨兵值不同**是故意的：
// "没人决定过组"和"决定了组但组里没选出后端"要修的代码不是同一处，值一样就分不开。
// 声明放在这里而不是 `balance.h`，因为路由表和均衡层共用这两个下标空间，
// 而依赖方向是 balance → router（组存的成员就是 target 下标）。
inline constexpr std::uint16_t kNoUpstreamGroup = 0xfffe;
inline constexpr std::uint16_t kMaxUpstreamGroup =
    static_cast<std::uint16_t>(kNoUpstreamGroup - 1);

enum class Kind : std::uint8_t {
  kPrefix,  // 前缀命中，表内取**最长**的那个
  kRegex,   // 全串锚定命中（等价 std::regex_match，不是 search），表内取**配置顺序**的第一条
};

// 命名口径，写死在这里：`request_target` 永远是**请求行里那个 URI**（`/a/b?c=1`），
// `upstream_group` 永远是**上游组的下标**，`upstream_target` 永远是**单个后端端点的下标**。
// 三个词在中文里都可能被叫成"目标"，混着用迟早写出 `if (target == target)` 那种行。
//
// S3-02（ADR-19）把路由的返回从 target 改成 group：一条路由指向**一组**后端，
// 组里选哪一个由 `LoadBalanceFilter` 决定。这不是为了好看——把两者合成一步会让
// "路由"这个纯函数层染上有状态的选择，1.23 那条"规则语义用例不建 socket"就作废了。

// 规则表。顺序 = 添加顺序，而且链和解析器同一个口径：**不加 priority 字段**，
// 需要优先级的时候那就是装配顺序。
//
// 两类之间的固定优先序是 **regex 先于 prefix**。理由不是审美：`/` 这种 catch-all 前缀
// 是路由表里最常见的东西，如果 prefix 先赢，任何 regex 规则都会被一条 `/` 遮蔽掉。
// 这与 nginx 的 `location` 语义同形（最长前缀记住、再按顺序试正则、正则命中即胜），
// 只是我没实现它的 `=`（精确匹配）与 `^~`（前缀命中后不再试正则）两个修饰符。
class RouteTable {
 public:
  RouteTable() = default;
  // 不可复制：规则里的已编译模式是重对象，而这张表天生是"装配一次、被 filter 引用着读"
  // 的东西。真要两份就重新装配一份，别让一次拷贝悄悄复制了整张编译好的正则自动机。
  RouteTable(const RouteTable&) = delete;
  RouteTable& operator=(const RouteTable&) = delete;

  // 加一条前缀规则。空前缀是合法的 catch-all（命中任何 request_target）。
  // upstream_group 只要求落在组下标空间内；**这个组里到底有没有成员、成员对不对得上端点表，
  // 由装配方校验**（`config/gateway_config`），因为这张表不该知道端点表长什么样——否则
  // "路由"就同时知道目的地和目的地的目的地，两份真相迟早各说各话。
  bool add_prefix(std::string_view prefix, std::uint16_t upstream_group) noexcept;

  // 加一条正则规则。模式编译不过（语法非法）就返回 false 且**不留半条规则**：
  // 这是装配期错误，调用方必须像 `--upstream` 那样整体失败退出，
  // 不能让它退化成"这条规则没生效、请求安静地落到别的后端"。
  bool add_regex(std::string_view pattern, std::uint16_t upstream_group);

  // 命中 → upstream_group 下标；没有任何规则命中 → nullopt（调用方据此表态，见 ADR-18
  // 第二节：无匹配是请求侧的确定性结果，回 404，不是网关故障）。
  //
  // **noexcept 是有前提的**：add_regex 已经在编译期挡掉了非法模式，所以匹配阶段
  // 没有"表达式有错"这条抛出路径。我不在这里补 try/catch——链的 `run()` 本来就是
  // noexcept（ADR-11："要拒绝请返回 Decision::kStop"），在这里接住一个接不住的异常
  // 只会把装配期错误伪装成一次 nullopt，也就是"没有路由"，那比红着退出更糟。
  std::optional<std::uint16_t> match(std::string_view request_target) const noexcept;

  std::size_t size() const noexcept { return rules_.size(); }
  bool empty() const noexcept { return rules_.empty(); }

  // 仅日志与用例用：第 index 条规则的 kind 与它的原始模式串（越界返回空 kind_view）。
  // 它存在的理由和 `FilterChain::name_at()` 一样——断"顺序 = 添加顺序"要看内容本身，
  // 不看内部指针。
  struct KindView {
    Kind kind = Kind::kPrefix;
    std::string_view pattern;
    std::uint16_t upstream_group = kNoUpstreamGroup;
    bool valid = false;  // 越界时为 false
  };
  KindView rule_at(std::size_t index) const noexcept;

 private:
  struct Rule {
    Kind kind = Kind::kPrefix;
    std::string pattern;                                // 原始模式串（日志/用例回读用）
    std::uint16_t upstream_group = kNoUpstreamGroup;
    std::shared_ptr<const void> compiled_regex;         // Kind::kRegex 才有；<regex> 关在 .cc 里
  };

  std::vector<Rule> rules_;
};

}  // namespace l7gate::route
