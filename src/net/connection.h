#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "http/request_parser.h"
#include "net/buffer.h"
#include "net/fd.h"
#include "net/stats.h"

namespace l7gate::net {

// 一条 TCP 连接的状态。它是 S1-04 的核心，也是 S1-03 结尾欠下的那两笔债的
// 还款地点：**已扫描游标**和 **view 生命周期**都必须由"有地方存状态"的对象来承担，
// 而解析器按设计不能存（否则 S1-03 的"可重复喂入"可测性就没了）。
//
// 边界，说清楚它**不管**什么：它不做路由、不做转发决策、不碰 epoll。
// 那些是 reactor 的策略。这个类只回答两个问题——"这个 fd 上到了什么字节"和
// "这些字节里有没有一个完整的、合法的请求"。把它们分开不是审美：S1-08 的
// filter 链要插在策略侧，如果策略埋在连接对象里，"新增 filter 不改主路径"
// 那条验收就只能靠重构实现。
//
// 所有权：reactor 的注册表持有 `shared_ptr<Connection>`。这里**故意不**互相持有
// 对端的 shared_ptr——上下游成对、又都放进同一个注册表，两个 shared_ptr 会构成
// 循环引用，那 20 KB 就永久留着了，而 LSan 只会在测试里告诉我"泄漏"却不告诉我
// 是两个对象抱着不死。所以对端用 `peer_fd_`（一个 int）表示，需要时由 reactor
// 查注册表拿；查不到就是对端已经关了，这正是 pump 该停的信号。
class Connection {
 public:
  enum class Role : std::uint8_t { kDownstream, kUpstream };

  enum class State : std::uint8_t {
    kConnecting,  // 仅上游：connect 已发起，等可写事件 + SO_ERROR 才算数
    kOpen,
    kClosing,  // 已决定关闭；出向缓冲刷完就真关
    kClosed,
  };

  struct Config {
    Buffer::Config inbound;
    // 出向 Buffer 的配置在**第一次真的需要写**的时候才用到（见 outbound_or_create()）。
    // 空闲长连接不为"我可能要说点什么"预支内存 —— 这是卡点四那笔账的来源。
    Buffer::Config outbound;
    http::ParserConfig parser;
    std::size_t max_header_count = http::kDefaultMaxHeaderCount;
  };

  // 一轮可读的产出。reactor 只看这些字段做决策，不自己碰 buffer。
  struct ReadRound {
    Buffer::ReadResult read;
    bool request_ready = false;   // 解析到 kOk：request() 可用，request_bytes 是要转发的字节数
    bool reject = false;          // 解析错误或头部超限：连接该结束，S1-06 在这里映射 4xx
    http::ParseError error = http::ParseError::kNone;
    std::size_t request_bytes = 0;
  };

  struct WriteRound {
    Buffer::WriteResult write;
    bool drained = false;  // 出向缓冲空了 —— reactor 该摘掉 EPOLLOUT，别让它自旋
  };

  Connection(UniqueFd fd, Role role, const Config& config, ReactorStats& stats);
  ~Connection();

  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;
  Connection(Connection&&) = delete;
  Connection& operator=(Connection&&) = delete;

  int fd() const noexcept { return fd_.get(); }
  Role role() const noexcept { return role_; }
  State state() const noexcept { return state_; }
  void set_state(State state) noexcept { state_ = state; }

  int peer_fd() const noexcept { return peer_fd_; }
  void set_peer_fd(int fd) noexcept { peer_fd_ = fd; }

  // 关掉 fd，但**不**析构对象。reactor 的延后析构需要这两步分开：
  // 顺序是 `Reactor::close_connection` 那六步里的第 1–4 步（摘 epoll → 解除对端
  // 引用 → 出注册表 → 本函数 close fd），第 5 步（析构）排在事件批处理结束时。
  // close 之后 fd 编号立刻可能被新连接复用，所以批内剩下的旧事件只能靠
  // 注册表反查来识别（见 reactor 的 dispatch），这一步是那个防护的前提。
  void close_fd() noexcept;

  // --- 缓冲 -------------------------------------------------------------
  Buffer& inbound() noexcept { return inbound_; }
  const Buffer& inbound() const noexcept { return inbound_; }

  // 观测用，**永不分配**。我第一版把这两个 getter 写成了惰性创建的版本
  // （`Buffer* outbound()` / `http::ParseScratch* scratch()`），结果是测试里
  // 一句 `EXPECT_EQ(conn.scratch(), nullptr)` 自己把 scratch 建了出来、
  // 断言随即失败——"检查有没有"这个动作本身改变了答案。
  // 这类 API 的名字在说谎，所以修的是接口而不是那行测试。
  bool has_outbound() const noexcept { return outbound_ != nullptr; }
  const Buffer* outbound() const noexcept { return outbound_.get(); }
  bool has_scratch() const noexcept { return scratch_ != nullptr; }
  const http::ParseScratch* scratch() const noexcept { return scratch_.get(); }

  std::size_t outbound_pending() const noexcept {
    return outbound_ ? outbound_->readable_bytes() : 0;
  }

  // 把 data[0..len) 排进出向缓冲。返回 false 表示装不下（出向水位到了），
  // 调用方必须据此停止从对端继续读 —— 这就是 S1-07 背压的入口。
  bool queue_bytes(const void* data, std::size_t len) noexcept;

  // --- 事件入口 ---------------------------------------------------------
  ReadRound on_readable();
  WriteRound on_writable();

  // --- 解析结果的生命周期 -----------------------------------------------
  // 返回 nullptr 表示 view 已作废（buffer 动过 / scratch 已释放）。
  // 这不是"防御性判空"：`ParsedRequest` 里全是 `string_view`，指向的就是
  // inbound_ 那块内存，而 reactor 每次读都可能触发 memmove 或扩容。
  // 用过期 view 的后果不是崩溃，是把上一个请求的 Host 或 Content-Length
  // 安到下一个请求上——那是走私，只不过是我自己造的。
  const http::ParsedRequest* request() const noexcept;
  bool request_views_valid() const noexcept;

  // 一个请求处理完了：消费 request_bytes、复位游标、丢掉解析结果。
  // **调用方必须已经把它需要的字段取用或拷贝完毕**（顺序见 request() 的注释）。
  void finish_request(std::size_t request_bytes) noexcept;

  // 隧道化之后调用（S1-05 的 Upgrade 路径）：握手完成后 header 不再需要，
  // 而 100k 空闲长连接每人挂着 2048 字节就是 200 MB，正好把 SLO #2 的预算吃满。
  // 触发点只有一个：Reactor 把完整的 101 响应头排进下游出向缓冲**之后**
  // （见 reactor.cc 的 maybe_enter_tunnel）。这里刻意**没有**任何"请求结束就
  // 释放"的自动路径——那样 keep-alive 的下一条请求会重新分配这 2048 字节，
  // 等于白省（交接段点名不许写的那条，落成结构：想触发只能显式调用本函数）。
  void release_scratch() noexcept;

  // --- 隧道（S1-05）------------------------------------------------------
  // 语义来自 ADR-09：带 `Connection: upgrade` + `Upgrade` 的请求在上游回出完整
  // 的 101 响应头之后，这条连接对**不再存在"请求"这个单位**——两个方向都是
  // 不透明字节流，不解析、不改写、不重新分帧。

  // 转发一条请求前调用（此时 view 还有效）：命中 upgrade 形态就把意图记在
  // 连接上，请求被 finish_request 消费掉之后 view 作废了也丢不掉。
  void mark_upgrade_request() noexcept { pending_upgrade_ = true; }
  // 命中 `Connection: close`：这条配对连接是一次性的。本函数只落意图；
  // reactor 在转发成功的那一刻把它传播给配对的上游（mark_close_after_response），
  // 真正的收尾走 EOF 级联。"立刻关"不是 close 的语义——那会把还在路上的响应
  // 字节丢掉，所以这条捷径我从结构上就没开（响应边界判定归 S1-06）。
  void mark_close_after_forward() noexcept { close_after_forward_ = true; }

  bool pending_upgrade() const noexcept { return pending_upgrade_; }
  // 取走一次性意图。用"取"而不是"读"：转发的语义是**下一条** 101 才成隧道，
  // 同一条连接上更晚的普通响应不该被更早一次没成形的 upgrade 意图影响。
  bool take_upgrade_pending() noexcept {
    const bool v = pending_upgrade_;
    pending_upgrade_ = false;
    return v;
  }
  bool close_after_forward() const noexcept { return close_after_forward_; }
  bool tunneling() const noexcept { return tunneling_; }

  // 上游侧的一次性标记（S1-05 的 close 语义）：由 reactor 在转发 close 请求时
  // 从下游传播过来。语义口径写死在这里，免得被误读成"立刻关"：
  //   - 这条配对连接是**一次性**的，不承诺复用；
  //   - "响应边界"的精确判定（几条字节算一条响应完结）归 S1-06，本任务不做
  //     预支——网关不在没有框架知识的情况下猜边界；
  //   - 实际收尾走既有的 EOF/断开级联路径（上游 EOF → 六步关闭 → 级联下游），
  //     与 S1-06 里"上游中途断开"的处理复用同一条机制。这里只固化意图与观测。
  void mark_close_after_response() noexcept { close_after_response_ = true; }
  bool close_after_response() const noexcept { return close_after_response_; }

  // 进入隧道态。前置条件是 pending_upgrade_ 已经被 take 掉（调用方负责时序），
  // 这里只落状态。幂等：重复调用无副作用。
  void enter_tunnel() noexcept { tunneling_ = true; }

  // 仅上游角色有意义（调用方是 reactor 的上游可读路径，它保证这一点）：
  // 判断"入向可读区的开头恰好是一个完整的 101 响应头"。成立返回该头的字节数，
  // 调用方据此转入隧道；不成立返回 npos，**不做任何状态改变**。
  // 扫描带游标（head_scan_pos_）：每轮只看新到的字节 + 一个常数回退窗口，
  // 不重扫整个头部区——和下游侧 scan_pos_ 还的是同一笔债（游标是相对可读区
  // 起点的；可读区起点被 consume 挪动后游标自动作废，所以每次返回 npos 都把
  // 游标夹到当前末尾，下一次从"新字节 - 3"处续扫）。
  std::size_t detect_response_head_101() noexcept;

  // 隧道模式的字节入口：只把 fd 上的字节读进入向缓冲（读到 EAGAIN / 撞水位
  // 为止，ET 契约不变），**不解析、不产生任何 HTTP 语义**。
  Buffer::ReadResult read_raw() noexcept;

  std::size_t header_scan_bytes() const noexcept { return header_scan_bytes_; }
  std::size_t parse_calls() const noexcept { return parse_calls_; }

 private:
  // 惰性创建只允许发生在"真的要用"的路径上：queue_bytes / on_readable / on_writable。
  // 它们私有，外部就只能拿到不会分配的版本，检查不再改变被检查的东西。
  Buffer* outbound_or_create() noexcept;
  http::ParseScratch* scratch_or_create() noexcept;

  // 游标推进：只回答"头部区结束没有 / 有没有超限"，**不裁决合法性**。
  // 这条分工是刻意的：一旦这里也判 CR/LF 是否成对，判定就有了两个来源，
  // 而 S1-03 刚因"配置写了、代码没读"翻过一次车。合法性只由 parse_request 说。
  bool advance_header_scan() noexcept;

  UniqueFd fd_;
  Role role_;
  State state_ = State::kOpen;
  int peer_fd_ = kInvalidFd;
  Config config_;
  ReactorStats* stats_;

  Buffer inbound_;
  std::unique_ptr<Buffer> outbound_;
  std::unique_ptr<http::ParseScratch> scratch_;

  // 头部区扫描游标：相对于 inbound_ 可读区起点的已扫描字节数。
  // 用"相对可读区"而不是绝对偏移，是因为 memmove 压缩会把内容整体搬到起点、
  // 相对位置不变——绝对偏移一压缩就作废，那就等于把游标变成一个需要到处
  // 记得同步的隐藏状态。
  std::size_t scan_pos_ = 0;
  bool headers_complete_ = false;
  // 上游侧 101 检测的扫描游标（相对可读区起点；上游不消费，起点恒为 0）。
  std::size_t head_scan_pos_ = 0;
  // 隧道三态（S1-05）：见上面隧道接口的注释。pending/ close 是"这条请求带来的
  // 连接级意图"，tunneling_ 是"配对连接已切换为不透明字节流"。
  bool pending_upgrade_ = false;
  bool close_after_forward_ = false;
  bool close_after_response_ = false;
  bool tunneling_ = false;
  // 已解析出一个完整请求、但调用方还没 finish_request。挡住重复解析，
  // 否则 requests_parsed 会随下一批字节到达而虚高。
  bool request_ready_ = false;
  // 这个请求是否已经计入 requests_parsed。view 作废后的重解析（见 on_readable 里
  // 那段注释）是同一条请求的第二次解析动作：parse_calls 要记两次，
  // requests_parsed 只能记一次。少这个标志，要么计数虚高，要么重复解析死循环。
  bool request_parsed_counted_ = false;
  std::size_t header_scan_bytes_ = 0;
  std::size_t parse_calls_ = 0;

  std::optional<http::ParsedRequest> request_;
  std::uint64_t request_inbound_generation_ = 0;
  std::uint64_t request_scratch_epoch_ = 0;
  std::uint64_t scratch_epoch_ = 0;
};

}  // namespace l7gate::net
