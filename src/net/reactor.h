#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

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

  std::size_t live_connection_count() const noexcept { return connections_.size(); }

 private:
  void handle_events(const std::vector<Epoll::Event>& events);
  void accept_all();
  void wake_up();

  // 注册一个连接对象的 fd。events 由调用方给（EPOLLIN / EPOLLOUT 的组合）。
  bool register_connection(Connection& conn, std::uint32_t events) noexcept;
  bool modify_events(Connection& conn, std::uint32_t events) noexcept;

  void on_downstream_readable(const std::shared_ptr<Connection>& conn, std::uint32_t events);
  void on_upstream_event(const std::shared_ptr<Connection>& conn, std::uint32_t events);
  void flush_outbound(const std::shared_ptr<Connection>& conn);

  // 转发策略（S1-04 的部分）：请求头+body 一旦完整，就把它的**原始字节**排进上游
  // 的出向缓冲，然后消费掉下游的这些字节。不改写、不重新分帧——逐字节透传的前提
  // 是"我碰过的字节和我没碰过的字节长得一样"，这条归 S1-06 的验收负责证明。
  // 返回 false 表示这一条没转出去（上游还没连上 / 上游出向满了），字节留在原地。
  bool forward_request(const std::shared_ptr<Connection>& downstream, std::size_t request_bytes);
  void pump_upstream_to_downstream(const std::shared_ptr<Connection>& upstream);

  // 把这条下游连接上"已经完整、但还没转出去"的请求一路转干净。
  enum class ForwardOutcome : std::uint8_t {
    kDrained,   // 现在没有可转发的完整请求了（要么全转走，要么剩的是半包）
    kBlocked,   // 有完整请求但暂时转不出去：连接不能关，等有可写事件再来
    kFailed,    // 这一轮里判定要关闭（解析错误或 EOF 前的残包），连接已被本函数关掉
  };
  ForwardOutcome forward_pending_requests(const std::shared_ptr<Connection>& downstream);

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
  Epoll epoll_;
  UniqueFd listener_;
  UniqueFd wake_fd_;
  int listen_port_ = -1;

  std::unordered_map<int, std::shared_ptr<Connection>> connections_;
  std::vector<std::shared_ptr<Connection>> retiring_;

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
