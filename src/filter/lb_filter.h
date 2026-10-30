#pragma once

#include <memory>
#include <string_view>

#include "filter/chain.h"
#include "route/balance.h"

namespace l7gate::filter {

// 负载均衡 filter（S3-02，ADR-19 第三条）——链上的**第二环**：读 `RequestInfo::upstream_group`，
// 写 `RequestInfo::upstream_target`。
//
// 为什么它与 `RouteFilter` 是两个 filter 而不是一个：路由是纯函数（给定表与 URI，答案唯一），
// 均衡**天生有状态**（轮询游标、每后端的配对数、一致性哈希环）。合成一环就等于把状态塞进
// 纯函数那一层，1.23 那条"规则语义用例能在不建 socket 的文件里跑完"当场作废，
// 而 S3-03 的健康检查也没有地方插——它要的是"这一组里现在谁可用"，不是"这条请求属于哪条路由"。
//
// 状态归属：`LbState` 是**每 worker 一份**（`ReactorGroup` 装配时给每条 worker 造一份，
// 连同这一环 filter 一起）。这里的 `shared_ptr` 不表示跨线程共享——持有它的两个对象
// （reactor 的记账路径与这一环 filter）都在同一个 worker 的循环线程里跑，
// 所以整条链仍然无锁、`tsan` 仍然零 data race（ADR-14 的红线，ADR-19「为什么」第一条）。
class LoadBalanceFilter : public RequestFilter {
 public:
  LoadBalanceFilter(const route::GroupTable& table, std::shared_ptr<route::LbState> state) noexcept;

  std::string_view name() const noexcept override { return "load-balance"; }

  // 组没被决定过 → kContinue（这一环对这条请求无事可做，交给 reactor 的默认目标）。
  // 组不存在 / 组里没有成员 / 环没建 → kStop + 502：那是我的配置坏了，不是客户端要的东西没有。
  Decision on_request(RequestInfo& info) override;

 private:
  // 一致性哈希的 key。`header:<Name>` 缺失时**退回整条 target**，不是退回空串——
  // 空串会让所有没带那个头的请求钉在同一个后端上，那是一次谁也不会去查的哈希倾斜。
  std::string_view hash_key_of(const RequestInfo& info, const route::HashKey& key) const noexcept;

  const route::GroupTable& table_;
  std::shared_ptr<route::LbState> state_;
};

}  // namespace l7gate::filter
