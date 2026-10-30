#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "config/json.h"
#include "route/balance.h"
#include "route/router.h"

// 配置文件到网关内部结构的装配（S3-02，ADR-19）。
//
// 这份文件**只描述策略**：上游组、组里的后端、组用哪种策略、路由到哪一组。
// 监听地址 / 端口 / worker 数 / fd 预算 / 停读线 / 排水窗口全部留在命令行——
// 那条切分的理由与代价写在 ADR-19 第一节，这里只留一句：**把两种东西混进一份文件，
// 就要写一段"谁赢"的合并规则，而合并规则是迟早被人改错的地方**。
//
// 严格性：字段名、类型、取值域任何一项不对都返回 false 并留下**具体是哪一项**的话。
// 未知字段一律拒绝（不是忽略）。这条不是洁癖：`banlace` 拼错如果被忽略，
// 网关会安静地按 `round_robin` 跑起来，而报告里写的是"我配了最少连接"。
namespace l7gate::config {

struct Endpoint {
  std::string address;
  std::uint16_t port = 0;
};

class GatewayConfig {
 public:
  // 去重后的目标表。**下标就是 `RequestInfo::upstream_target` 的值域**，也和
  // `Reactor::Config::upstream_targets` 一一对应（main.cc 只做一次字段搬运）。
  // 去重是必需的：同一个 `地址:端口` 如果被两个组各自登记一次，池就会为同一台后端
  // 开两个桶，那既让"这台后端有多少条热连接"有两个答案，也白白多花一份单元组容量。
  const std::vector<Endpoint>& endpoints() const noexcept { return endpoints_; }

  const route::GroupTable& groups() const noexcept { return groups_; }
  const route::RouteTable& routes() const noexcept { return routes_; }

  // 文件里有没有 `routes` 段。**false 时 main.cc 一个 filter 都不装**——
  // 这条决定"只配了上游组、没配路由"时行为等于 S2（链是空的，走默认目标），
  // 而不是"隐式地有一条 catch-all 路由"。
  bool has_routes = false;

 private:
  // 两个装配入口都要写私有成员，所以两个都得是 friend。只 friend 一个的话，
  // 另一个（用例直接喂文本的那条）会编不过——而这两条**必须走同一段校验代码**，
  // 分成"用例验一份、线上跑另一份"就白验了。
  friend bool load_gateway_config(std::string_view path, GatewayConfig* out, std::string* error);
  friend bool build_gateway_config(const JsonValue& document, GatewayConfig* out, std::string* error);
  std::vector<Endpoint> endpoints_;
  route::GroupTable groups_;
  route::RouteTable routes_;
};

// 读文件 + 解析 + 校验 + 建环，一次做完。任何一步失败都返回 false。
// **不"退回默认配置继续跑"**：`--config` 给了就要么成立要么退出码 2。
bool load_gateway_config(std::string_view path, GatewayConfig* out, std::string* error);

// 从已经拿到手的文档文本装配（用例与 `--config` 共用同一条校验路径——
// 分成两条的话，用例验的是没上线的那一条）。
bool build_gateway_config(const JsonValue& document, GatewayConfig* out, std::string* error);

}  // namespace l7gate::config
