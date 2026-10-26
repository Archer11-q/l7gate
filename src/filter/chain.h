#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "http/request_parser.h"

// S1-08：filter 链的**骨架**。这一层今天没有任何业务 filter——限流、熔断、路由分别
// 归 S4/S4/S3（`TASKS.md`），它们的形状要在真的那条需求到来时才定，提前写等于猜。
// 骨架要交付的是三件事，一件比一件容易被后来的改动磨掉：
//   一、顺序 = 配置顺序，而且这件事能被单测断言（不是"我记得是正序"）；
//   二、转发主路径里只有**一处**链调用，新增 filter 不改主路径的任何一行——这条验收
//        能成立的前提是策略本来就在 reactor 而不在 Connection（S1-04 起就这么分的，
//        见 net/connection.h 顶部那段"它不管什么"）；
//   三、接口能装下 S3/S4 要放的东西。装不下的那部分我不假装装得下：见下面 Decision
//        与 RequestInfo 各自末尾那条"这里没有 X，因为今天没有读者"。
namespace l7gate::filter {

// 一条请求过链之后，链只可能给出两种答复。
// **没有 kRejected(code)**：拒掉之后回什么字节，是第一个会停请求的 filter（S4 限流）
// 的责任，那时它自己就该带着状态码来。今天给它加一个 status 字段，就是给一条没人走的
// 分支发一张空头证明——和 S1-03 那次"配置写了、代码没读"同一类错。
enum class Decision : std::uint8_t {
  kContinue,  // 我看了，不拦
  kStop,      // 这条请求不该往上游走
};

// 递给 filter 的可见面。字段全是**借用**：`method`/`target`/`headers` 指向下游入向
// 缓冲里的 view，`Connection::finish_request` 一消费就作废（S1-03 定下的 view 生命周期
// 纪律，S1-04 在连接层把它落成"下一次读之前必须取用完毕"）。
// 所以规矩写在类型注释上、也写在用例里：**不许把 RequestInfo 或它里面的任何 view 存进
// filter 的成员**。要在请求之间记住东西，记连接身份（`downstream_fd`）和 filter 自己的状态。
// 这里**没有**"目标上游"这一格：路由要改目的地，而 S1 的回源地址是配置死的、
// `ensure_upstream` 只读配置、没有任何代码会读这个字段。它该在 S3 落地时和它的读者一起进来。
struct RequestInfo {
  std::string_view method;
  std::string_view target;
  std::span<const http::HeaderEntry> headers;
  std::size_t request_bytes = 0;  // 整条请求的原始字节数（含 body 区）
  int downstream_fd = -1;         // 这条请求来自哪条下游连接（S1 的身份就是 fd）
};

class RequestFilter {
 public:
  RequestFilter() = default;
  // 不可复制：filter 可以有状态（计数器、限流窗口、熔断健康度），复制一份状态不是本意，
  // 而链持有的是 unique_ptr——没有复制需求，就把这条路堵死而不是留着一个不会用的构造。
  RequestFilter(const RequestFilter&) = delete;
  RequestFilter& operator=(const RequestFilter&) = delete;
  virtual ~RequestFilter() = default;

  // 名字给日志和用例用。断"执行顺序 == 配置顺序"要看名字，不看指针值——指针相等
  // 只证明"跑的是那两个"，不证明"按那个次序跑"。
  virtual std::string_view name() const noexcept = 0;

  // **不许抛异常**：链的 run 是 noexcept 的，抛出来就是 std::terminate。
  // 单线程事件循环里没有"捕获 filter 的异常继续跑"这条路——那条路会把一条请求
  // 停在半处理状态，比当场停下更难查。要拒绝请返回 Decision::kStop。
  virtual Decision on_request(const RequestInfo& info) = 0;
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

  // 正序跑完或提前停：第一个 `kStop` 就地返回，**后面的 filter 一次都不被调用**。
  // 空链返回 kContinue——"没有 filter"和"filter 都放行"在主路径上必须是同一个行为，
  // 否则 S1-08 这个骨架就成了转发路径上的一个分支条件。
  Decision run(const RequestInfo& info) noexcept;

 private:
  std::vector<std::unique_ptr<RequestFilter>> filters_;
};

}  // namespace l7gate::filter
