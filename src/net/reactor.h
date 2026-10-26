#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "filter/chain.h"
#include "net/connection.h"
#include "net/epoll.h"
#include "net/fd.h"
#include "net/stats.h"

namespace l7gate::net {

// 单线程 epoll 边缘触发事件循环。ADR-02 的落地点，S2 要在这之上加线程，
// 所以它的并发前提写得比功能更细。
//
// 它承担三件事，每件都不是风格问题：
//
// **一、延后析构。** 一次 `epoll_wait` 可以在同一批里返回多个事件。如果我在批内
// 就把连接对象析构掉，批里后面那条事件带着的 `Connection*` 就是悬垂指针，而它
// 接下来会被解引用去 close 一个**已经被新连接复用的 fd 编号**。这不是理论风险：
// 10000 请求 × keep-alive 的场景里 fd 编号会被反复回收，命中的概率随负载上升，
// 表现是偶发地把别人的连接关掉——ASan 能抓到 UAF，但抓不到"合法但错的人"。
// 所以关闭被拆成六步（0 出向没刷完就先只挂 EPOLLOUT、不碰 fd → 1 摘 epoll →
// 2 解除对端的反向引用 → 3 出注册表 → 4 close fd → 5 批处理完再析构），
// 再加一道注册表反查作为第二层防护。两层都要，一层防悬垂、一层防张冠李戴。
//
// **二、accept 也要读到 EAGAIN。** 边缘触发不只对读数据成立，对监听 socket 一样：
// 一次 EPOLLIN 可能对应多个已完成的连接，只 accept 一个就把事件用掉了，剩下的
// 要等到下一次事件——而下一次可能永远不来。`Buffer::read_until_blocked` 是同一
// 条语义的数据版本，这里是它的连接版本。
//
// **三、EPOLLET 由封装层强制。** 见 `Epoll::kForcedEvents`。reactor 这里不再关心
// 触发模式，它只决定"关心哪些事件"。
class Reactor {
 public:
  struct Config {
    std::string listen_address = "127.0.0.1";
    // 0 表示由内核选端口；测试全靠它避免端口撞车，所以这个 0 是功能不是便利。
    std::uint16_t listen_port = 0;
    std::string upstream_address = "127.0.0.1";
    std::uint16_t upstream_port = 0;

    int listen_backlog = 1024;
    // fd 预算。超过就直接把新连接关掉而不是排队：一个网关在 fd 耗尽时的正确
    // 行为是拒绝，不是"再多接一个然后 crash"。这个数字也是 S2 的 100k 场景里
    // 唯一需要提前想清楚的闸门（rlimit 侧的 NoFile 检查归 S2）。
    std::size_t max_connections = 4096;

    Connection::Config connection;

    // > 0 时：解析满这么多个请求就自动 stop()。压测和验收需要"跑固定轮数后
    // 自己结束"，而不是靠外部信号——外部 SIGTERM 的送达时机本身不是可控量。
    std::size_t stop_after_requests = 0;

    // epoll_wait 的超时。它决定 stop() 之外还能靠什么结束循环；200ms 是
    // "测试里最坏等 200ms、生产里每 200ms 醒一次检查停止条件"的折中。
    // 将来挂起的 timer/空闲回收会改这个值（S4 的超时归 S4）。
    int wait_timeout_ms = 200;
  };

  explicit Reactor(Config config);
  ~Reactor();

  Reactor(const Reactor&) = delete;
  Reactor& operator=(const Reactor&) = delete;

  // 绑定监听 socket 并注册进 epoll。失败返回 false，errno 由 *error 带出。
  bool start_listen(int* error) noexcept;

  // 阻塞直到 stop() 被调用（或 stop_after_requests 达成）。
  // 返回本次循环处理的事件批次数；正常退出路径之外没有别的返回值语义。
  //
  // **先调用 stop() 再进 run() 也停得下来**：停止请求记在 stop_requested_ 上，
  // 而不是靠"进循环之前把 running_ 置 true、退出位置 false"那一对动作。
  // 后者会把已经送达的停止请求直接抹掉，表现是循环线程永远不停。
  std::uint64_t run();

  // 可以在别的线程调用（测试就是这么停的），所以它只做一件跨线程的事：
  // 往 eventfd 写一个数。不碰任何循环内部状态 —— 那才是真正会在 S2 出问题的地方。
  void stop() noexcept;

  bool running() const noexcept { return running_.load(std::memory_order_relaxed); }
  bool stop_requested() const noexcept { return stop_requested_.load(std::memory_order_relaxed); }

  // 实际监听端口（bind 到 0 时由内核决定，start_listen 之后才可读）。
  int listen_port() const noexcept { return listen_port_; }

  ReactorStats& stats() noexcept { return stats_; }
  const ReactorStats& stats() const noexcept { return stats_; }

  // epoll 计数器直接来自 Epoll，不是我自己数的第二份。
  // 这条很重要：验收要断言「注册/注销次数与连接建立/关闭次数逐项相等」，
  // 如果两边都由我手工 ++，那相等就只是我自己写对称的错觉，什么都证明不了。
  std::uint64_t epoll_registrations() const noexcept { return epoll_.registrations(); }
  std::uint64_t epoll_unregistrations() const noexcept { return epoll_.unregistrations(); }
  std::uint64_t epoll_modifications() const noexcept { return epoll_.modifications(); }

  // --- filter 链（S1-08）--------------------------------------------------
  // 只能在 run() 之前添加。链在循环线程里被逐条读，没有任何加锁，运行中改它就是一场
  // data race；S5 要热更新的话，唯一安全的形状是"整条链换一根指针"，不是往 vector 里 push。
  //
  // 转发主路径里只有**一处**链调用（`forward_pending_requests_body` 取到完整请求之后、
  // `forward_request` 之前），所以"新增一个 filter 要改主路径几行代码"这条验收的答案是
  // 0 行。这个 0 不是巧合，是 S1-04 起把策略留在 reactor、把状态留在 Connection 的分工
  // 兑的现（见 net/connection.h 顶部那段"它不管什么"）。
  void add_request_filter(std::unique_ptr<filter::RequestFilter> filter) noexcept;

  std::size_t request_filter_count() const noexcept { return request_filters_.size(); }

  std::size_t live_connection_count() const noexcept { return connections_.size(); }

  // --- S1-07 的「epoll 侧可断言」------------------------------------------
  // 验收句要的是"能观测到网关不再消费下游可读事件"，那话的主语是**注册表里的兴趣
  // 掩码**，不是任何计数器。这两个 getter 给的就是那份掩码的回执（来自
  // Connection::armed_events()，只在 epoll_ctl 成功时写），所以它们和
  // epoll_registrations() 同一类：非原子、只在循环线程停下之后读。
  // 不在注册表里的 fd 返回 0。
  std::uint32_t armed_events(int fd) const noexcept;
  // 当前"kOpen 且 EPOLLIN 已被摘掉"的连接数。背压触发时它 > 0，恢复之后回 0。
  std::size_t connections_with_read_disarmed() const noexcept;

 private:
  void handle_events(const std::vector<Epoll::Event>& events);
  void accept_all();
  void wake_up();

  // 注册一个连接对象的 fd。events 由调用方给（EPOLLIN / EPOLLOUT 的组合）。
  // 成功时把这份掩码记在连接上（`Connection::set_armed_events`）——它是 S1-07
  // 去重与跳变计数的唯一依据，所以**只在 epoll_ctl 成功的那条分支里写**。
  bool register_connection(Connection& conn, std::uint32_t events) noexcept;
  bool modify_events(Connection& conn, std::uint32_t events) noexcept;

  // --- 背压（S1-07）：谁现在该被读，以及"没人来读"时谁来补一次读 ----------
  // 这里刻意分成两条互不相干的轨道，合在一条上迟早会漏掉另一种形状：
  //   **内存轨道 = 电平**：要不要摘 EPOLLIN 只看这条连接自己的入向可读区相对停读线
  //     的位置（带滞回）。对端出向有多满**不是**判据，理由见 ARCHITECTURE 1.15——
  //     每个方向的可读区都有硬上限，"内存不继续增长"已经由停读线本身担保。
  //   **ET 轨道 = 事件**：这一轮的读有没有停在停读线上（`Connection::take_read_stopped_at_line`）。
  //     停在**线上**就是没读到 EAGAIN，内核里可能还有字节；这时候只要掩码里仍挂着
  //     EPOLLIN（泵把缓冲腾空了 → 水位回落 → 没摘），就必须自己补一次读，因为边沿触发
  //     只为新跳变负责，而"对端写完就等回音"恰恰不会再有跳变。这条轨道不摘任何订阅，
  //     只多花一次 read syscall——它是 S1-02 那条 ET 契约在水位存在之后的收尾。
  bool wants_epollin(const Connection& conn) const noexcept;
  // 唯一的事件掩码策略函数：读兴趣（wants_epollin）+ 写兴趣（有货待发或正在 connect）。
  // 它是"应该订阅什么"，armed_events 是"实际订阅着什么"，两者的差就是该不该打 epoll_ctl。
  std::uint32_t desired_events(const Connection& conn) const noexcept;
  // 把 desired 交给 epoll：相等就不打 syscall（这是每请求 `epoll_ctl` 次数的支出闸门）。
  // EPOLLIN 0→1 时计 resume 并补一次读；1→0 且原因是停读线时计 pause。
  // 两条分支之外还管 ET 轨道：掩码不变或只动 EPOLLOUT 时，欠着的那一次读同样在这里入队。
  void apply_events(const std::shared_ptr<Connection>& conn);
  // 欠读的连接进队列并立刻排空。恢复（挂回 EPOLLIN）和停在停读线上（掩码没变）
  // 都走这一个入口，就是为了"补读"这件事只有一个触发点。
  void schedule_probe(const std::shared_ptr<Connection>& conn);
  // 恢复之后必须**主动读一次**：边沿触发只在电平跳变时报事件，挂回 EPOLLIN 的那一刻
  // 客户端的字节可能早就躺在内核接收缓冲里了，从此不再有跳变，这条连接就永久睡死。
  // 探测用嵌套保护排成队列（probing_），不在事件处理里递归下栈。
  void drain_probes();
  void probe_read(const std::shared_ptr<Connection>& conn);
  // 出向腾出空间之后重试被压住的那条搬运（刷空/刷出一部分都要试，见 flush_outbound）。
  void retry_peer_after_room(const std::shared_ptr<Connection>& conn);

  void on_downstream_readable(const std::shared_ptr<Connection>& conn, std::uint32_t events);
  void on_upstream_event(const std::shared_ptr<Connection>& conn, std::uint32_t events);
  void flush_outbound(const std::shared_ptr<Connection>& conn);

  // 转发策略（S1-04 的部分）：请求头+body 一旦完整，就把它的**原始字节**排进上游
  // 的出向缓冲，然后消费掉下游的这些字节。不改写、不重新分帧——逐字节透传的前提
  // 是"我碰过的字节和我没碰过的字节长得一样"，这条归 S1-06 的验收负责证明。
  // 返回 false 表示这一条没转出去（上游还没连上 / 上游出向满了），字节留在原地。
  // upgrade / close_after 是 S1-05 的两条连接级意图：从**这条请求自己的**已解析
  // view 判出（调用方必须在 finish_request 之前取用 view，这里是唯一的取用点），
  // 由本函数决定落到哪条连接、何时置 kClosing。判定不碰字节：转发出去的仍是原始请求。
  bool forward_request(const std::shared_ptr<Connection>& downstream,
                       std::size_t request_bytes, bool upgrade, bool close_after);
  // 响应语义泵（S1-06）：上游入向缓冲里的字节按"当前响应的阶段"过一遍框架判定
  // （http/response_framing 的纯函数），然后透传——头整块排（半个头排出去就是把
  // 畸形喂给客户端），body 按框架边界拆散排（截断的权力在框架知识，不在这条泵）。
  // 它取代了 S1-04/05 的 pump_upstream_to_downstream（整块透传，无框架判定）。
  // 隧道路径不经过这里（ADR-09）。
  void pump_response(const std::shared_ptr<Connection>& upstream);
  // 泵本体。和"重算这条连接的读兴趣"分开写：S1-07 之后每个消费上游入向字节的出口
  // 都要过一次 apply_events，而本体里有七八个 return 点——在每个点上补一句
  // 就是第七份"迟早漏一处"的抄本，所以收在外壳里做一次。
  void pump_response_body(const std::shared_ptr<Connection>& upstream);

  // reject → 4xx（S1-06）：把 gateway_error_reply(status) 排进下游出向，然后
  // **不对称**关闭配对——上游 kDiscardOutbound（它接到的可能是半条请求，没有
  // 值得发的货），下游 kFlushThenClose（4xx 还躺在它的出向里）。先手工把两侧
  // 的 peer_fd_ 解开，否则 close_connection 的级联会把同一个 why 传染给对端，
  // 下游的 4xx 就跟着被丢掉了。
  void fail_pair_with_reply(const std::shared_ptr<Connection>& downstream,
                            std::uint16_t status);

  // 隧道泵（S1-05）：不解析、不改写、不重新分帧，把 src 入向缓冲里的全部可读
  // 字节整块搬进 dst 的出向缓冲。装不下（出向水位）就一个字节都不搬——和请求侧
  // "整块不转"同一条背压方向。S1-06 之后响应侧有了框架判定，这条路依然原样保留：
  // 隧道里的字节永远不该被任何 HTTP 语义过一遍（ADR-09）。
  void pump_tunnel(const std::shared_ptr<Connection>& src,
                   const std::shared_ptr<Connection>& dst);
  // 本体 + 外壳的同一形状（见 pump_response_body 那条注释）：外壳负责在搬完之后
  // 重算 src 的读兴趣——搬成功要算（可能回落了），搬不动也要算（这一轮的读
  // 可能刚好把可读区顶到停读线）。
  void pump_tunnel_body(const std::shared_ptr<Connection>& src,
                        const std::shared_ptr<Connection>& dst);

  // 上游可读之后调用：若下游配对连接带着 upgrade 意图、且入向开头是一个完整的
  // 101 响应头，则把这**恰好 len 字节**的响应头原样排进下游（它是握手的一部分，
  // 之后才轮到帧），两侧进入隧道态，释放下游的 ParseScratch（release_scratch()
  // 自 S1-04 落地以来的第一个调用点），并计 upgrades_101。返回是否进入隧道。
  // 非 101 / 头未完整：什么都不改，正常响应路径继续。
  bool maybe_enter_tunnel(const std::shared_ptr<Connection>& upstream);

  // 把这条下游连接上"已经完整、但还没转出去"的请求一路转干净。
  enum class ForwardOutcome : std::uint8_t {
    kDrained,   // 现在没有可转发的完整请求了（要么全转走，要么剩的是半包）
    kBlocked,   // 有完整请求但暂时转不出去：连接不能关，等有可写事件再来
    kFailed,    // 这一轮里判定要关闭（解析错误或 EOF 前的残包），连接已被本函数关掉
  };
  ForwardOutcome forward_pending_requests(const std::shared_ptr<Connection>& downstream);
  // 本体（真正找请求、排字节、消费的那圈循环）。和"重算这条连接的读兴趣"分开写，
  // 理由同 pump_response_body：本体有七个 return 点，逐个补一句等于第七份抄本。
  ForwardOutcome forward_pending_requests_body(const std::shared_ptr<Connection>& downstream);

  // 确保这条下游连接已经有配对的上游；没有就发起异步 connect 并配对。
  // 返回 nullptr 表示暂时还没有（正在 connect）或已达 fd 预算上限。
  std::shared_ptr<Connection> ensure_upstream(const std::shared_ptr<Connection>& downstream);

  // 关闭的**前置判断**：出向缓冲里还有没刷出去的字节时不能直接 close(fd)。
  // 那不是"少发一个响应"的语义问题，是我自己把已经接受的字节丢掉了：
  // 上游回完就断（客户端提前断开、S1-06 的截断判定）都会走到这条路径，
  // 而响应此刻正躺在下游的出向缓冲里。所以关连接分成两种意图：
  //   kFlushThenClose  —— 先挂上 EPOLLOUT 把货发完，由 flush_outbound 收尾（默认）
  //   kDiscardOutbound —— 写已经失败了（EPIPE/真 errno），留着也不会出去，立刻关
  // 只有第二种情况会看到 outbound_pending()>0 还继续关，所以"决定要关却没有
  // EPOLLOUT 可等"的状态不可能出现——那条漏掉的 EPOLLOUT 就是永久挂住的 fd。
  enum class CloseWhy : std::uint8_t { kFlushThenClose, kDiscardOutbound };

  // 六步里的第 0 到第 4 步在这里；第 5 步（析构）由 retiring_ 在批处理结束时兑现。
  void close_connection(const std::shared_ptr<Connection>& conn,
                        CloseWhy why = CloseWhy::kFlushThenClose) noexcept;
  void retire_closed() noexcept;

  // 有字节要发就得挂上 EPOLLOUT，包括已经决定关闭（kClosing）的连接 ——
  // 那里恰恰是最容易漏的一处：状态不是 kOpen 就跳过挂事件，等于把
  // "关之前先刷完"这条路自己堵死，货发不出去、fd 也关不掉。
  void arm_write(const std::shared_ptr<Connection>& conn) noexcept;

  Config config_;
  ReactorStats stats_;
  // 请求方向的 filter 链（S1-08）。按值持有而不是 unique_ptr 成员：空链必须是
  // "没有 filter"这个常态，主路径上不该为了它多一次判空分支。
  filter::FilterChain request_filters_;
  Epoll epoll_;
  UniqueFd listener_;
  UniqueFd wake_fd_;
  int listen_port_ = -1;

  std::unordered_map<int, std::shared_ptr<Connection>> connections_;
  std::vector<std::shared_ptr<Connection>> retiring_;

  // 待探测读的连接（S1-07 的恢复路径）。持 shared_ptr 和 retiring_ 同一个理由：
  // 排队期间这条连接可能正好被关掉，出队时必须靠注册表反查认人，不能信手里的裸指针。
  // probing_ 是嵌套保护：探测读本身会搬运、搬运会消费字节、消费会再次触发恢复判定，
  // 每次都直接递归下去的话栈深就由"缓冲腾出多少空间"决定——那是个我控制不了的量。
  // 排空循环把它压成一条队列，栈深恒为 1。
  std::vector<std::shared_ptr<Connection>> pending_probes_;
  bool probing_ = false;

  // 出向缓冲刷完之前，这条 fd 一直带着 EPOLLOUT。放在 map 外面单独记没有意义，
  // 状态就在连接对象自己身上（state + outbound 是否为空），这里不重复。
  std::atomic<bool> running_{false};
  // 停止请求独立于"循环在不在跑"。两个状态合成一个的原因我踩过一次：
  // 用 running_ 同时表达"我在循环里"和"我还没被要求停"，那么 run() 开头的
  // store(true) 就会把在那之前送达的 stop() 抹掉，循环再也不停。
  std::atomic<bool> stop_requested_{false};
  std::vector<Epoll::Event> events_;
};

}  // namespace l7gate::net
