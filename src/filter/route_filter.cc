#include "filter/route_filter.h"

#include <cstdint>
#include <optional>

namespace l7gate::filter {
namespace {

// 无匹配的表态码。**404 而不是 502/503**：路由表里没有这条前缀/正则，是"客户端要的
// 东西我没有路由"——请求侧的确定性结果，重试、换后端、抬上游都没用。回 503 会把一次
// 配置遗漏读成"网关过载/后端不可用"，运维会去查一条根本没坏的 upstream（这正是
// ADR-12 那条"判据失准会变成雪崩放大器"的小同号版本）。
// 它必须是 `http::gateway_error_reply()` 表里有货的那个数字，否则下游什么也收不到。
constexpr std::uint16_t kNoRouteStatus = 404;

}  // namespace

RouteFilter::RouteFilter(const route::RouteTable& table) noexcept : table_(table) {}

filter::Decision RouteFilter::on_request(RequestInfo& info) {
  // 匹配的是解析器给的**原始 origin-form target**（含 query，不解码、不规范化）。
  // 这里不复制、不改写它：info.target 是借来的 view，本函数只读它一次。
  const std::optional<std::uint16_t> hit = table_.match(info.target);
  if (!hit.has_value()) {
    // kStop + reject_status：交给主路径上那条"有表态的停止"（S1-06 的 fail_pair_with_reply）。
    // 注意**不写 upstream_group 之外的任何字段**：upstream_group 保持哨兵、upstream_target 也
    // 保持哨兵，读它们的代码在 kStop 分支里，
    // 不会拿这个未决定的值去索引目标表。
    info.reject_status = kNoRouteStatus;
    return Decision::kStop;
  }
  info.upstream_group = *hit;
  return Decision::kContinue;
}

}  // namespace l7gate::filter
