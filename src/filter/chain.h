#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "http/request_parser.h"
#include "route/router.h"

// S1-08：filter 链的**骨架**。限流、熔断归 S4（`TASKS.md`）；S3-01 在这里起了**第一个
// 业务 filter**（路由）。骨架要交付的是三件事，一件比一件容易被后来的改动磨掉：
//   一、顺序 = 配置顺序，而且这件事能被单测断言（不是"我记得是正序"）；
//   二、转发主路径里只有**一处**链调用，新增 filter 不改主路径的任何一行——这条验收
//        能成立的前提是策略本来就在 reactor 而不在 Connection（S1-04 起就这么分的，
//        见 net/connection.h 顶部那段"它不管什么"）；
//   三、接口能装下 S3/S4 要放的东西。
// S3-01（ADR-18）做了一次接口返工：`RequestInfo` 多了两个可写字段、`on_request` 的参数
// 从 const 引用变成可写引用。**这条返工有前提**——字段必须和它的第一个读者同批进来，
// 那正是 1.16 末尾欠着的那笔债，读者是 `Reactor::forward_request` / `ensure_upstream`。
namespace l7gate::filter {

// 一条请求过链之后，链只可能给出两种答复。
// **仍然没有 kRejected(code)**：拒掉之后回什么字节是那个 filter 的责任，所以状态码挂在
// `RequestInfo::reject_status` 上、不挂在返回值上。S1-08 当年拒绝给返回值加 status 字段，
// 理由是"那是给一条没人走的分支发空头证明"；S3-01 是第一个走这条分支的人（路由无匹配
// 回 404），字段现在才进来——同一条纪律的两半，不是自相矛盾（见 ADR-18 第一节）。
enum class Decision : std::uint8_t {
  kContinue,  // 我看了，不拦
  kStop,      // 这条请求不该往上游走
};

// 递给 filter 的可见面 + 写回通道。
// **读的那四个字段全是借用**：`method`/`target`/`headers` 指向下游入向缓冲里的 view，
// `Connection::finish_request` 一消费就作废（S1-03 定下的 view 生命周期纪律，S1-04 在
// 连接层把它落成"下一次读之前必须取用完毕"）。所以规矩不变：**不许把 RequestInfo 或它
// 里面的任何 view 存进 filter 的成员**。要在请求之间记住东西，记连接身份（`downstream_fd`）
// 和 filter 自己的状态。
//
// 参数为什么不再是 const：链需要一个**写通道**——路由要改目的地，S4 的限流要带状态码。
// 我选择去掉 const 而不是另造一个出参结构体，因为那样链就有两个通信通道（返回值 + 出参），
// 而这两个通道在 S4 会开始互相说不同的话（`kStop` 了却还带着一个 target）。写面就是下面
// 那两个 `uint16_t`，读面那四个 view 照旧只读；这个对象每次调用在栈上现造，没有生命周期风险。
struct RequestInfo {
  std::string_view method;
  std::string_view target;
  std::span<const http::HeaderEntry> headers;
  std::size_t request_bytes = 0;  // 整条请求的原始字节数（含 body 区）
  int downstream_fd = -1;         // 这条请求来自哪条下游连接（S1 的身份就是 fd）

  // --- S3-01（ADR-18）：两个写回字段，读者都在 reactor -------------------------------
  // 目标上游的下标。`route::kNoUpstreamTarget` 读作"这个 filter 没决定"，reactor 于是用
  // 它自己的默认目标。**这条就是"没装路由 filter 时行为逐字节等于 S2"的实现位置**：
  // 缺了它，空链那一臂就变成"每条请求都必须有 filter 表态"。
  std::uint16_t upstream_target = route::kNoUpstreamTarget;
  // 拒这条请求时回什么状态码。0 = 不表态，保持 S1-08 的行为（字节留在原地、连接不关、
  // 一条响应都不发）；非 0 走 S1-06 已有的 `fail_pair_with_reply()` 那条路。
  // 取值必须是 `http::gateway_error_reply()` 那张表里有货的数字。写一个表里没有的数会
  // 得到"空 view → 不发也不计数"（`reactor.cc` 那条 `reply.empty()` 早退），
  // 那是设计上的拒绝，不是漏发。
  std::uint16_t reject_status = 0;
};

class RequestFilter {
 public:
  RequestFilter() = default;
  // 不可复制：filter 可以有状态（计数器、限流窗口、熔断健康度、路由表引用），复制一份
  // 状态不是本意，而链持有的是 unique_ptr——没有复制需求，就把这条路堵死而不是留着一个
  // 不会用的构造。
  RequestFilter(const RequestFilter&) = delete;
  RequestFilter& operator=(const RequestFilter&) = delete;
  virtual ~RequestFilter() = default;

  // 名字给日志和用例用。断"执行顺序 == 配置顺序"要看名字，不看指针值——指针相等
  // 只证明"跑的是那两个"，不证明"按那个次序跑"。
  virtual std::string_view name() const noexcept = 0;

  // **不许抛异常**：链的 run 是 noexcept 的，抛出来就是 std::terminate。
  // 单线程事件循环里没有"捕获 filter 的异常继续跑"这条路——那条路会把一条请求
  // 停在半处理状态，比当场停下更难查。要拒绝请返回 Decision::kStop。
  // `info` 可写，但**只许写 `upstream_target` / `reject_status` 那两个数字**（读面是借的）。
  virtual Decision on_request(RequestInfo& info) = 0;
};

// 顺序由**添加顺序**决定的链。没有 priority 字段、没有按名字插队：需要优先级的时候
// 就是配置里数组的顺序，再加一套排序键只会让"谁先跑"有两个互相冲突的答案。
class FilterChain {
 public:
  FilterChain() = default;
  FilterChain(const FilterChain&) = delete;
  FilterChain& operator=(const FilterChain&) = delete;

  void add(std::unique_ptr<RequestFilter> filter) noexcept;

  std::size_t size() const noexcept { return filters_.size(); }
  bool empty() const noexcept { return filters_.empty(); }
  std::string_view name_at(std::size_t index) const noexcept;  // 越界返回空串，仅测试/日志用

  // 正序跑完或提前停：第一个 `kStop` 就地返回，**后面的 filter 一次都不被调用**——
  // 所以后面那些 filter 可能写回的字段也不该被读：调用方看返回值，不看"链跑到了第几环"。
  // 空链返回 kContinue——"没有 filter"和"filter 都放行"在主路径上必须是同一个行为，
  // 否则 S1-08 这个骨架就成了转发路径上的一个分支条件。
  Decision run(RequestInfo& info) noexcept;

 private:
  std::vector<std::unique_ptr<RequestFilter>> filters_;
};

}  // namespace l7gate::filter
