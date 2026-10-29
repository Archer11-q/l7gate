#pragma once

#include <string_view>

#include "filter/chain.h"
#include "route/router.h"

namespace l7gate::filter {

// 路由 filter（S3-01，ADR-18）——链上的**第一个业务 filter**。
//
// 它只做一件事：把这条请求的 target 串交给 `route::RouteTable` 那个纯函数，命中就把
// 目标下标写回 `info.upstream_target`，没命中就写 `info.reject_status = 404` 并 kStop。
// **它不回源、不碰 fd、不做后端之间的负载均衡**（三策略归 S3-02，健康检查归 S3-03）。
//
// 为什么路由要待在链上而不是主路径的一段 if：ADR-11 那条"新增 filter 不改主路径任何
// 一行"是整个链架构唯一可执行的验收，S4 的限流/熔断就靠它插进来。代价我也照记在
// ADR-18：本任务**确实**改了主路径的管路（`forward_request`/`ensure_upstream` 各多一个
// 参数、链调用点读回两个字段）——那是"路由这一环落地这一次"的改动，不是给后来者开的
// 口子；第二个业务 filter 加进来仍然主路径一行不改，这条由 S1-08 那条
// `PassThroughFiltersChangeNothingInTheForwardingPath` 用例继续盯着。
//
// 生命期与所有权：表是**引用**，由装配方（`main.cc` / 测试）持有并保证比 reactor 活得久。
// 不在这里复制表：`RouteTable` 里是已编译的正则自动机，复制一份既浪费又不回答任何问题——
// "同一份东西有两个抄本"正是这个项目反复栽的那类坑（原型见 `max_header_count` 那次）。
//
// 这里**没有**"目标表长度"这个字段。我第一版放了它、打算让 filter 顺手挡越界，随即撞上
// S1-03 那条老雷：**装了不读就是空头证明**——真正会拿这个下标去索引 `vector` 的是
// `Reactor::targets_`，越界检查必须在索引它的那一处做才叫检查。所以装配方（`main.cc`）在
// 启动前校验规则目标、reactor 在索引前再兜一次，filter 只管"命中/没命中"。
class RouteFilter : public RequestFilter {
 public:
  explicit RouteFilter(const route::RouteTable& table) noexcept;

  std::string_view name() const noexcept override { return "route"; }

  // 命中 → 写下标、返回 kContinue；无命中 → 写 404、返回 kStop。
  Decision on_request(RequestInfo& info) override;

 private:
  const route::RouteTable& table_;
};

}  // namespace l7gate::filter
