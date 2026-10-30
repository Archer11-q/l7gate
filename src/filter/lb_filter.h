#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "filter/chain.h"
#include "route/balance.h"
#include "route/health.h"
#include "route/nodes.h"

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
//
// S3-03 追加：`health` 表被**同组全部 worker 共享**（全原子，先例 ReactorStats），
// 本环每请求只读一个问句——"这个成员现在被摘除了吗"。共享表不违反 ADR-14：
// 它不承载 fd，也不承载连接对象。
class LoadBalanceFilter : public RequestFilter {
 public:
  // `health` 可空：空 = 健康检查整套未装配（S3-02 形状，行为逐字节不变）。
  // `nodes` 可空：空 = 本网关不跑节点表 reload（组表用构造给的那份，S3-02 形状）。
  // 非 null 时**每请求 load 一根快照指针**（一次原子读）：reload 发布的新成员表
  // 从下一条请求开始生效——这是"reload 生效耗时"的请求侧实现位置。
  LoadBalanceFilter(const route::GroupTable& table, std::shared_ptr<route::LbState> state,
                    std::shared_ptr<const route::HealthTable> health = nullptr,
                    std::shared_ptr<const route::NodesPublisher> nodes = nullptr) noexcept;

  std::string_view name() const noexcept override { return "load-balance"; }

  // 组没被决定过 → kContinue（这一环对这条请求无事可做，交给 reactor 的默认目标）。
  // 组不存在 / 组里没有成员 / 环没建 → kStop + 502：那是我的配置坏了，不是客户端要的东西没有。
  Decision on_request(RequestInfo& info) override;

 private:
  // 一致性哈希的 key。`header:<Name>` 缺失时**退回整条 target**，不是退回空串——
  // 空串会让所有没带那个头的请求钉在同一个后端上，那是一次谁也不会去查的哈希倾斜。
  std::string_view hash_key_of(const RequestInfo& info, const route::HashKey& key) const noexcept;

  // 本请求的候选集（被摘除的成员不在其中；全员被摘时回退全员——ADR-12 的立场是
  // "故意保留一段错误"，而请求无去处（不自产错误响应）是 S4 的空格子）。
  // 复用同一个成员缓冲：本 filter 每 worker 一份、只在 owner 循环线程跑，
  // 所以热路径上不付每次请求一次堆分配的钱。
  const std::vector<std::uint16_t>& candidates_for(const route::UpstreamGroup& group);

  const route::GroupTable& table_;
  std::shared_ptr<route::LbState> state_;
  std::shared_ptr<const route::HealthTable> health_;
  std::shared_ptr<const route::NodesPublisher> nodes_;
  std::vector<std::uint16_t> candidates_scratch_;
};

}  // namespace l7gate::filter
