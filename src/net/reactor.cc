#include "net/reactor.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <string_view>
#include <system_error>
#include <utility>

#include "net/socket.h"

namespace l7gate::net {

namespace {

// Role / State 是 Connection 的嵌套类型；本文件里它们出现二十多次，
// 每个都写 Connection::Role 会让真正的逻辑淹没在限定名里。
using Role = Connection::Role;
using State = Connection::State;

// epoll 的 user_data 只能放一个 void*，而我要区分三类持有者：监听 socket、
// 自唤醒 eventfd、连接对象。用两个**静态对象自己的地址**当标签，比
// `reinterpret_cast<void*>(1)` / `(2)` 这种魔数好：地址天然唯一、不可能和一个
// 真实的 Connection 对象重合，而且拼错标签名会直接编译不过。
struct Tag {};
Tag g_listener_tag;
Tag g_wake_tag;

void* listener_tag() noexcept { return &g_listener_tag; }
void* wake_tag() noexcept { return &g_wake_tag; }

constexpr std::uint32_t kReadEvents = EPOLLIN;
constexpr std::uint32_t kReadWriteEvents = EPOLLIN | EPOLLOUT;

// S1-05 的请求侧意图判定。口径逐条写死，避免"看起来支持、实际看心情"：
//   upgrade    —— `Connection` 值里含逗号分隔的 `upgrade` 记号（不区分大小写、
//                 忽略记号两侧 OWS），**且**存在 `Upgrade` 头。两个条件缺一不立：
//                 只看 Upgrade 头会把"头里出现 websocket 字样的普通请求"当握手，
//                 而真实的握手在 RFC 9113 §4.1 里两个头都在。
//   close      —— `Connection` 值里含 `close` 记号。
// 判定只读已解析的 view，不碰原始字节——转出去的仍是逐字一致的请求。
bool connection_has_token(const http::ParsedRequest& req, std::string_view token) {
  const auto conn = req.find_header("connection");
  if (!conn.has_value()) return false;
  std::string_view rest = *conn;
  while (true) {
    const std::size_t comma = rest.find(',');
    std::string_view item = (comma == std::string_view::npos) ? rest : rest.substr(0, comma);
    // 剥 OWS（SP / HTAB）。
    while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) item.remove_prefix(1);
    while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) item.remove_suffix(1);
    if (item.size() == token.size()) {
      bool same = true;
      for (std::size_t i = 0; i < item.size(); ++i) {
        const char a = static_cast<char>(::tolower(static_cast<unsigned char>(item[i])));
        const char b = static_cast<char>(::tolower(static_cast<unsigned char>(token[i])));
        if (a != b) {
          same = false;
          break;
        }
      }
      if (same) return true;
    }
    if (comma == std::string_view::npos) break;
    rest = rest.substr(comma + 1);
  }
  return false;
}

bool is_upgrade_request(const http::ParsedRequest& req) {
  return connection_has_token(req, "upgrade") && req.find_header("upgrade").has_value();
}

bool is_close_request(const http::ParsedRequest& req) {
  return connection_has_token(req, "close");
}

}  // namespace

Reactor::Reactor(Config config) : config_(std::move(config)) {
  // 自唤醒通道。stop() 从别的线程调用时只做一件事：往这个 fd 写 8 字节。
  // 不用 atomic 标志 + 依赖超时轮询的原因：那样最坏要等一个 wait_timeout_ms
  // 才停得下来，而"停不下来"在压测里表现为脚本挂住，在测试里表现为 TIMEOUT 失败。
  int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (fd < 0) {
    throw std::system_error(errno, std::system_category(), "eventfd");
  }
  wake_fd_ = UniqueFd{fd};
}

Reactor::~Reactor() {
  // 拆注册表里的连接。顺序和运行期关闭一致：先摘 epoll，再 close fd。
  // 这里不依赖"epoll fd 自己关闭时内核会连带清掉interest list"——那确实够用，
  // 但注销计数器会因此对不上，而计数器正是这个任务要断言的东西之一。
  for (auto& [fd, conn] : connections_) {
    int error = 0;
    (void)epoll_.remove(fd, &error);
    conn->close_fd();
  }
  connections_.clear();
  retiring_.clear();

  if (listener_.valid()) {
    int error = 0;
    (void)epoll_.remove(listener_.get(), &error);
    listener_.close_now();
  }
  if (wake_fd_.valid()) {
    int error = 0;
    (void)epoll_.remove(wake_fd_.get(), &error);
  }
}

bool Reactor::start_listen(int* error) noexcept {
  SocketResult listener = make_listen_socket(config_.listen_address, config_.listen_port,
                                             config_.listen_backlog);
  if (!listener.ok()) {
    *error = listener.error;
    return false;
  }
  const int port = get_socket_port(listener.fd.get());
  if (port < 0) {
    *error = errno;
    return false;
  }

  listen_port_ = port;
  listener_ = std::move(listener.fd);

  if (!epoll_.add(listener_.get(), kReadEvents, listener_tag(), error)) {
    listener_.close_now();
    listen_port_ = -1;
    return false;
  }

  if (!epoll_.add(wake_fd_.get(), kReadEvents, wake_tag(), error)) {
    int ignore = 0;
    (void)epoll_.remove(listener_.get(), &ignore);
    listener_.close_now();
    listen_port_ = -1;
    return false;
  }
  *error = 0;
  return true;
}

std::uint64_t Reactor::run() {
  std::uint64_t batches = 0;
  running_.store(true, std::memory_order_relaxed);

  // 条件读的是 stop_requested_ 而不是 running_：见头文件里那两个原子量的分工。
  // stop() 可能在 run() 被调度之前就被调用（测试里是主线程 spawn 完立刻停，
  // 生产里是启动的一瞬间收到 SIGTERM），那种情况下循环必须一次都不进。
  while (!stop_requested_.load(std::memory_order_acquire)) {
    int error = 0;
    const int n = epoll_.wait(config_.wait_timeout_ms, &events_, &error);
    if (n < 0) {
      // Epoll::wait 已经把 EINTR 内部重试掉了，走到这里就是 EBADF/EINVAL 这一类
      // "epoll 本身坏了"的状态。继续循环只会让后面的所有数字都不可信，所以停。
      break;
    }
    if (!events_.empty()) {
      handle_events(events_);
      ++batches;
    }

    if (config_.stop_after_requests > 0 &&
        stats_.requests_parsed.load(std::memory_order_relaxed) >= config_.stop_after_requests) {
      break;
    }
  }

  running_.store(false, std::memory_order_relaxed);
  return batches;
}

void Reactor::stop() noexcept {
  // 顺序要紧：先落下"要停"这个决定，再敲门。反过来的话存在一个窗口 ——
  // 门敲了，循环醒来一看标志还是"不停"，接着回去等；而那次等待可能再也没有
  // 事件把它叫醒（自唤醒的 eventfd 已经被这次消费掉了）。
  // acquire/release 配对让"循环看到标志为真"时，也一定看到它之前写进 eventfd 的数。
  stop_requested_.store(true, std::memory_order_release);
  const std::uint64_t one = 1;
  // 写 eventfd 是原子的（8 字节一次），且失败只有 EAGAIN——计数已经大到写不进，
  // 那种情况下循环一定会被唤醒，所以忽略返回值在这里是安全的，不是省事。
  ssize_t rc = ::write(wake_fd_.get(), &one, sizeof(one));
  (void)rc;
}

void Reactor::wake_up() {
  std::uint64_t counter = 0;
  while (true) {
    const ssize_t rc = ::read(wake_fd_.get(), &counter, sizeof(counter));
    if (rc <= 0) break;  // EAGAIN 或真错误都到此为止：本轮不需要更多
  }
}

void Reactor::handle_events(const std::vector<Epoll::Event>& events) {
  for (const auto& ev : events) {
    if (ev.user_data == wake_tag()) {
      wake_up();
      continue;
    }
    if (ev.user_data == listener_tag()) {
      accept_all();
      continue;
    }

    auto* raw = static_cast<Connection*>(ev.user_data);
    // 第二层防护（第一层是 retiring_）：fd 编号可能已经属于另一个连接对象了。
    // 注册表反查一次，对不上就当这条事件是陈旧的丢掉——不猜、不"看起来像就处理"。
    auto it = connections_.find(raw->fd());
    if (it == connections_.end() || it->second.get() != raw) {
      continue;
    }
    const std::shared_ptr<Connection> conn = it->second;  // 批内持有一份引用

    const std::uint32_t revents = ev.events;
    if (conn->role() == Role::kDownstream) {
      if ((revents & (EPOLLIN | EPOLLHUP)) != 0) {
        on_downstream_readable(conn, revents);
      }
      // 关闭可能被上一步触发了；再看一眼注册表，别对已关闭的连接继续操作。
      auto still = connections_.find(conn->fd());
      if (still == connections_.end() || still->second.get() != conn.get()) {
        continue;
      }
      if ((revents & EPOLLOUT) != 0) {
        flush_outbound(conn);
      }
      if ((revents & (EPOLLERR | EPOLLHUP)) != 0) {
        auto again = connections_.find(conn->fd());
        if (again != connections_.end() && again->second.get() == conn.get()) {
          // 不再在这里自己判"出向有没有刷完"：那个判断已经收进 close_connection，
          // 两处各判一次迟早会判出不一致来。留一份真相来源。
          close_connection(conn);
        }
      }
    } else {
      on_upstream_event(conn, revents);
    }
  }

  // 批处理结束才真正释放引用。这是那六步里的第 5 步，也是唯一安全的一步。
  retire_closed();
}

void Reactor::accept_all() {
  while (true) {
    AcceptOutcome accepted = accept_connection(listener_.get());
    if (!accepted.ok()) {
      if (accepted.would_block()) {
        return;  // ET 的正常终点：一次事件可能带来多个连接，读到没为止才算处理完
      }
      // EMFILE/ENFILE：进程级 fd 耗尽，accept 不会自己变好，继续循环就是忙等。
      // 只能退出本轮等下一个监听事件。经典解法是预留一个 spare fd 撑住 accept，
      // 但那是"在没有证据时加机制"——max_connections 才是这道闸门，真被打到的时候
      // accept_errors 这个数会替我说话。
      stats_.accept_errors.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (connections_.size() >= config_.max_connections) {
      // 注意是 **continue 而不是 return**：accepted 出作用域就把这条新连接关掉了，
      // 但必须继续 accept 把队列排空。带着没排空的队列退出，ET 下这个监听事件
      // 不会再投递第二次，之后的新连接全滞留在 backlog 里 —— 那是最典型的
      // 边缘触发卡死，表现是"过一会儿不接新连接了"，不是报错。
      stats_.downstream_rejected_over_capacity.fetch_add(1, std::memory_order_relaxed);
      continue;
    }

    auto conn = std::make_shared<Connection>(std::move(accepted.fd), Role::kDownstream,
                                             config_.connection, stats_);
    stats_.downstream_accepted.fetch_add(1, std::memory_order_relaxed);
    int error = 0;
    (void)set_tcp_nodelay(conn->fd(), &error);
    if (!register_connection(*conn, kReadEvents)) {
      close_connection(conn);
      return;
    }
    connections_.emplace(conn->fd(), conn);
  }
}

bool Reactor::register_connection(Connection& conn, std::uint32_t events) noexcept {
  int error = 0;
  return epoll_.add(conn.fd(), events, &conn, &error);
}

bool Reactor::modify_events(Connection& conn, std::uint32_t events) noexcept {
  int error = 0;
  return epoll_.modify(conn.fd(), events, &conn, &error);
}

std::shared_ptr<Connection> Reactor::ensure_upstream(
    const std::shared_ptr<Connection>& downstream) {
  const int existing = downstream->peer_fd();
  if (existing != kInvalidFd) {
    auto it = connections_.find(existing);
    if (it != connections_.end()) return it->second;
  }
  if (config_.upstream_port == 0) {
    return nullptr;  // 没配上游：S1-04 的 serve 模式一定配了，这里只是不把空端口当合法值
  }
  if (connections_.size() >= config_.max_connections) {
    stats_.downstream_rejected_over_capacity.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }

  ConnectOutcome outcome = start_connect(config_.upstream_address, config_.upstream_port);
  if (outcome.status == ConnectStatus::kFailed) {
    stats_.upstream_connect_failed.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }

  auto upstream = std::make_shared<Connection>(std::move(outcome.fd), Role::kUpstream,
                                               config_.connection, stats_);
  stats_.upstream_opened.fetch_add(1, std::memory_order_relaxed);
  upstream->set_state(outcome.status == ConnectStatus::kConnected ? State::kOpen
                                                                  : State::kConnecting);
  int error = 0;
  (void)set_tcp_nodelay(upstream->fd(), &error);

  upstream->set_peer_fd(downstream->fd());
  downstream->set_peer_fd(upstream->fd());

  // 未完成握手时必须带 EPOLLOUT：可写事件是 connect 结果的唯一通知方式。
  // 但可写不等于连上了，所以处理它之前先问 SO_ERROR（见 on_upstream_event）。
  if (!register_connection(*upstream, kReadWriteEvents)) {
    downstream->set_peer_fd(kInvalidFd);
    close_connection(upstream);
    return nullptr;
  }
  connections_.emplace(upstream->fd(), upstream);
  return upstream;
}

void Reactor::on_downstream_readable(const std::shared_ptr<Connection>& conn,
                                     std::uint32_t revents) {
  if (conn->state() != State::kOpen) {
    // 已经决定关闭（kClosing）的连接不再接新活：此刻它唯一该做的事是把出向缓冲里
    // 剩下的字节发完，那是 flush_outbound 的工作。在这里继续解析转发，等于往一条
    // 正在收尾的连接上追加它永远发不出去的东西。
    return;
  }
  if (conn->tunneling()) {
    // 隧道：读完就搬，不解析、不改写（ADR-09）。Connection::on_readable 在这里
    // 退化成 read_raw 的同义路径，所以直接走观测面更窄的那一个入口。
    const Buffer::ReadResult read = conn->read_raw();
    if (read.errno_value != 0) {
      close_connection(conn);
      return;
    }
    const bool eof = read.peer_closed || (revents & EPOLLHUP) != 0;
    auto it = connections_.find(conn->peer_fd());
    if (it != connections_.end()) {
      pump_tunnel(conn, it->second);
    }
    if (eof) close_connection(conn);
    return;
  }

  Connection::ReadRound round = conn->on_readable();

  if (round.read.errno_value != 0) {
    close_connection(conn);
    return;
  }
  if (round.reject) {
    // S1-04 只关连接，不回 4xx —— 把 ParseError 映射成状态码是 S1-06 的活。
    // 现在先把口子留在这里：这一行就是那条映射将来插入的位置。
    close_connection(conn);
    return;
  }

  const bool eof = round.read.peer_closed || (revents & EPOLLHUP) != 0;

  // 这一轮解析出的请求此时已经挂在连接上（`conn->request()` 有值），
  // forward_pending_requests 会从它开始往下走 —— 一次读带进多条请求（粘包、
  // 客户端连发）的场景就是在这个循环里被走完的。
  //
  // 为什么必须在事件里走完：边缘触发只说"自上次读以来有新字节"，不会再说第二次。
  // 如果我只处理第一条就返回，第二条请求会一直躺在入向缓冲里，直到客户端**再写
  // 一次**才被看见 —— 而 keep-alive 客户端正在等它的响应，永远不会再写。
  // 表现是偶发挂住，不是报错。
  const ForwardOutcome outcome = forward_pending_requests(conn);
  if (outcome == ForwardOutcome::kFailed) return;

  if (eof) {
    // 对端收尾了。S1-04 不做到达响应的收尾语义（半包残留在 S1-06 里判定为截断），
    // 这里只保证一件事：EOF 之前读到的完整请求已经尽力转发过，没转出去的留在原地
    // 而不是被我消费掉。
    close_connection(conn);
  }
}

Reactor::ForwardOutcome Reactor::forward_pending_requests(
    const std::shared_ptr<Connection>& downstream) {
  if (downstream->state() != State::kOpen) {
    // 正在收尾的连接不再往外转：新建的上游连接没人用，转出去的请求也没地方回响应。
    return ForwardOutcome::kBlocked;
  }
  if (downstream->tunneling()) {
    // 隧道里没有请求可转发：这条路径上的字节全部走 pump_tunnel（见
    // on_downstream_readable / flush_outbound 的两个隧道路）。留在缓冲里的
    // 尾部字节是帧的一部分，不是"还没解析完的请求"。
    // 握手指意在这里取走（而不是只读）：maybe_enter_tunnel 没消费到的残留意图
    // ——比如 101 之后客户端又发了一条带 Upgrade 头的请求——不该漂向更远的将来。
    (void)downstream->take_upgrade_pending();
    return ForwardOutcome::kDrained;
  }
  if (downstream->close_after_forward()) {
    // close 是一次性配对：不再往这条连接对转发第二条请求。
    // 留在缓冲里的字节不归本函数处置——它们要么随连接收尾一起结束，要么
    // 在 S1-06 的语义面里被判定（close 之后还发请求的客户端就是畸形流量）。
    return ForwardOutcome::kBlocked;
  }
  for (;;) {
    const http::ParsedRequest* pending = downstream->request();
    if (pending != nullptr) {
      // 意图必须在看板还有效的这一刻取（view 指向入向缓冲，finish_request 一消费
      // 就作废）——这也正是"判定"与"取用"分开写的原因：字节照原样转，意图另存。
      const bool upgrade = is_upgrade_request(*pending);
      const bool close_after = is_close_request(*pending);
      if (!forward_request(downstream, pending->consumed, upgrade, close_after)) {
        return ForwardOutcome::kBlocked;  // 上游没连上或出向满了，字节一条都没消费
      }
      continue;
    }

    if (downstream->inbound().readable_bytes() == 0) return ForwardOutcome::kDrained;

    // 手上没有已解析的请求、缓冲里却还有字节 —— 继续解析下一段。
    // 这里重新走 on_readable 是安全的：游标是相对可读区起点的，压缩/扩容都不动它，
    // 而 view 作废的情况由 Connection 自己负责重解析（见 request_views_valid）。
    const Connection::ReadRound round = downstream->on_readable();
    if (round.read.errno_value != 0 || round.reject) {
      close_connection(downstream);  // 管线里后一条畸形：整条连接结束，S1-06 在这里回 4xx
      return ForwardOutcome::kFailed;
    }
    if (!round.request_ready) {
      if (round.read.peer_closed) {
        close_connection(downstream);  // 半包 + EOF：残留在缓冲里的字节不可能是合法请求
        return ForwardOutcome::kFailed;
      }
      return ForwardOutcome::kDrained;  // 半包，等下一次可读事件
    }
  }
}

bool Reactor::forward_request(const std::shared_ptr<Connection>& downstream,
                              std::size_t request_bytes, bool upgrade, bool close_after) {
  std::shared_ptr<Connection> upstream = ensure_upstream(downstream);
  if (upstream == nullptr) {
    return false;  // 正在 connect 或超出 fd 预算；请求还留在下游缓冲里，没丢
  }
  if (request_bytes > downstream->inbound().readable_bytes()) {
    return false;  // 调用方给的长度不可信，不动状态
  }

  // 排进上游出向缓冲的是**原始字节**，长度取 parsed.consumed（含框架，不解码）。
  // 上游出向满了就整块不转、下游也不消费：背压的正确方向是"让对端慢下来"，
  // 而不是"把我没地方放的数据扔掉"。
  if (!upstream->queue_bytes(downstream->inbound().readable_data(), request_bytes)) {
    return false;
  }

  // 意图落在连接上，必须赶在 finish_request 作废 view 之前（此刻 pending 还有效，
  // 而上面一次字节都没动过状态——这个顺序是刻意的，转发失败时意图也不会残留）。
  if (upgrade) downstream->mark_upgrade_request();
  if (close_after) {
    downstream->mark_close_after_forward();
    // 传播到配对上游：这条连接对是一次性的。不在这里关任何 fd——响应还在路上，
    // 网关在没有框架知识（S1-06）的情况下猜"响应到没到齐"然后提前关闭，
    // 丢的是已经接受的字节。收尾走 EOF 级联（on_upstream_event 的 peer_closed）。
    upstream->mark_close_after_response();
    stats_.close_headers_honored.fetch_add(1, std::memory_order_relaxed);
  }

  downstream->finish_request(request_bytes);
  stats_.bytes_down_to_upstream.fetch_add(static_cast<std::uint64_t>(request_bytes),
                                          std::memory_order_relaxed);

  arm_write(upstream);
  return true;
}

void Reactor::pump_upstream_to_downstream(const std::shared_ptr<Connection>& upstream) {
  if (upstream->tunneling()) {
    // 隧道路径在调用方（on_upstream_event / pump_tunnel 的对称重试）已经分走，
    // 这里只保留一道断言式的守卫：状态串了宁可原地停住，也不把帧当响应搬。
    return;
  }
  auto it = connections_.find(upstream->peer_fd());
  if (it == connections_.end()) return;  // 对端已关（或编号已被复用）：无去处
  const std::shared_ptr<Connection> downstream = it->second;

  const std::size_t available = upstream->inbound().readable_bytes();
  if (available == 0) return;

  if (!downstream->queue_bytes(upstream->inbound().readable_data(), available)) {
    return;  // 下游出向水位满：数据留在上游缓冲，等下游刷完再重试
  }
  upstream->inbound().consume(available);
  stats_.bytes_up_to_downstream.fetch_add(static_cast<std::uint64_t>(available),
                                          std::memory_order_relaxed);

  // 刚排进字节的这一侧要挂上 EPOLLOUT，否则数据只会在缓冲里等着 ——
  // 而挂事件这个动作在 kClosing 状态下也不能跳，所以判断统一收在 arm_write 里。
  arm_write(downstream);
}

void Reactor::pump_tunnel(const std::shared_ptr<Connection>& src,
                          const std::shared_ptr<Connection>& dst) {
  auto it = connections_.find(src->peer_fd());
  if (it == connections_.end()) return;  // 对端已关（或编号已被复用）：无去处
  const std::size_t available = src->inbound().readable_bytes();
  if (available == 0) return;
  // 整块搬运：装不下就一个字节都不动（与请求侧"整块不转"同一条背压方向）。
  // 半截帧比半截请求更糟——透传的意义就是两端看到的字节流完全一致。
  if (!dst->queue_bytes(src->inbound().readable_data(), available)) return;
  src->inbound().consume(available);
  if (src->role() == Role::kDownstream) {
    stats_.bytes_tunnel_down.fetch_add(static_cast<std::uint64_t>(available),
                                       std::memory_order_relaxed);
  } else {
    stats_.bytes_tunnel_up.fetch_add(static_cast<std::uint64_t>(available),
                                     std::memory_order_relaxed);
  }
  arm_write(dst);
}

bool Reactor::maybe_enter_tunnel(const std::shared_ptr<Connection>& upstream) {
  auto it = connections_.find(upstream->peer_fd());
  if (it == connections_.end()) return false;
  const std::shared_ptr<Connection> downstream = it->second;
  if (!downstream->pending_upgrade()) return false;  // 没握手意图：一次判定都不做

  const std::size_t head_len = upstream->detect_response_head_101();
  if (head_len == std::string_view::npos) {
    // 到这里只可能是两种"还不成立"：完整响应头没到齐，或到了但不是 101。
    // 都不改变隧道语义判定的材料（字节没被消费过），等下一轮可读再来一次即可。
    return false;
  }
  // 101 的响应头是握手的一部分，必须**先原样交给客户端**再谈隧道：此刻上游
  // 入向可读区的开头就是整个头（配对连接在握手期间不会有第二条响应进来）。
  if (!downstream->queue_bytes(upstream->inbound().readable_data(), head_len)) {
    return false;  // 下游出向满了：头留在原地，等腾出空间后的下一轮再进隧道
  }
  upstream->inbound().consume(head_len);
  stats_.bytes_tunnel_up.fetch_add(static_cast<std::uint64_t>(head_len),
                                   std::memory_order_relaxed);
  downstream->take_upgrade_pending();  // 意图已被消费：这条配对连接的握手到此完成
  upstream->enter_tunnel();
  downstream->enter_tunnel();
  // release_scratch() 自 S1-04 落地以来的**唯一**调用点。触发点就是这里
  // （101 之后），不是"请求结束"——交接段点名的那条禁止路径在结构上不成立：
  // Connection 里没有任何自动调用它的地方。
  downstream->release_scratch();
  stats_.upgrades_101.fetch_add(1, std::memory_order_relaxed);
  arm_write(downstream);
  return true;
}

void Reactor::on_upstream_event(const std::shared_ptr<Connection>& conn, std::uint32_t revents) {
  if (conn->state() == State::kConnecting) {
    if ((revents & EPOLLOUT) == 0) return;
    int so_error = 0;
    if (!get_so_error(conn->fd(), &so_error) || so_error != 0) {
      stats_.upstream_connect_failed.fetch_add(1, std::memory_order_relaxed);
      // 握手都没成的上游，它出向缓冲里排着的请求永远发不出去：丢弃，别等刷完。
      // 对端（下游）用默认的 flush 意图关闭 —— 那边可能正有一个响应要发给客户端。
      close_connection(conn, CloseWhy::kDiscardOutbound);
      return;
    }
    conn->set_state(State::kOpen);
    if (conn->outbound_pending() == 0) {
      modify_events(*conn, kReadEvents);  // 没东西要写就别再挂 EPOLLOUT
    }
  }

  if (conn->state() == State::kClosing) {
    // 一次性配对（S1-05 的 close 语义）把上游置成 kClosing 后，它唯一该做的
    // 是把出向的货刷完。这里不再读新字节：留着读就等于让"已决定关闭"的上游
    // 还能往下游灌第二条响应，而那条响应没人负责判定边界（边界判定归 S1-06）。
    if ((revents & EPOLLOUT) != 0) flush_outbound(conn);
    return;
  }

  if ((revents & (EPOLLIN | EPOLLHUP)) != 0) {
    if (conn->tunneling()) {
      // 隧道（ADR-09）：双向都是不透明字节，方向只由角色决定。
      const Buffer::ReadResult read = conn->read_raw();
      if (read.errno_value != 0) {
        close_connection(conn);
        return;
      }
      const bool eof = read.peer_closed || (revents & EPOLLHUP) != 0;
      auto it = connections_.find(conn->peer_fd());
      if (it != connections_.end()) {
        pump_tunnel(conn, it->second);
      }
      if (eof) close_connection(conn);
      return;
    }
    const Connection::ReadRound round = conn->on_readable();
    if (round.read.errno_value != 0) {
      close_connection(conn);
      return;
    }
    if (maybe_enter_tunnel(conn)) {
      // 握手完成：101 已排进下游出向缓冲，帧字节留在上游缓冲里，
      // 由后续可读事件/可写重试按隧道路径搬运。本条事件到此为止。
      return;
    }
    pump_upstream_to_downstream(conn);
    if (round.read.peer_closed) {
      // 上游结束。S1-04 的策略是"跟着关下游"：本任务没有连接池（池是 S2 的），
      // 也不做响应截断判定（那是 S1-06 的 5xx）。S2 建池之后这条要改成
      // "只关上游、下游继续复用"，到时候改的就在这一个地方。
      close_connection(conn);
      return;
    }
  }

  if ((revents & EPOLLOUT) != 0) {
    flush_outbound(conn);
  }
}

void Reactor::flush_outbound(const std::shared_ptr<Connection>& conn) {
  Connection::WriteRound round = conn->on_writable();

  if (round.write.peer_closed || round.write.errno_value != 0) {
    // 写已经坏了：留在缓冲里的字节再等也出不去，这里必须**丢弃**而不是继续
    // "先刷完再关"。留着一个刷不出去的缓冲，就等于留着一条永远关不掉的 fd。
    close_connection(conn, CloseWhy::kDiscardOutbound);
    return;
  }

  if (!round.drained) return;  // 满了没刷完：EPOLLOUT 挂着，下一轮继续

  if (conn->state() == State::kClosing) {
    // 之前因为"货没发完"被挡下的关闭，现在货发完了，把它走完。
    // 这一句就是那条延后关闭的出口；少了它，kClosing 是一条只进不出的状态。
    close_connection(conn);
    return;
  }

  if (conn->state() != State::kOpen) return;

  // 刷空了就把 EPOLLOUT 摘掉。这一步不是优化：EPOLLOUT 在发送缓冲有空位时
  // 永远为真，挂着不摘就等于让事件循环对着一个"随时可写"的 fd 自旋，
  // SLO #2 的「空闲 CPU < 5%」第一条就是被它破坏的。
  modify_events(*conn, kReadEvents);

  // 我这边腾出空间了，把对端压着发不出去的数据接过来继续走。
  auto it = connections_.find(conn->peer_fd());
  if (it == connections_.end()) return;
  const std::shared_ptr<Connection> peer = it->second;
  if (conn->role() == Role::kDownstream) {
    if (peer->tunneling()) {
      pump_tunnel(peer, conn);  // 对端（上游）在隧道里：重试帧字节的搬运
    } else {
      pump_upstream_to_downstream(peer);
    }
  } else {
    // 我刚腾出空间的那个 fd 是**上游**，所以压着不动的是对端（下游）那条还没转出去的
    // 请求。走 forward_pending_requests 而不是只试一次：出向缓冲刚空出来，
    // 排队的请求可能有好几条，只放一条过去就等于把同一个 bug 换成另一个触发条件。
    if (peer->tunneling()) {
      pump_tunnel(peer, conn);  // 下游在隧道里：重试帧字节往上游的搬运
    } else {
      (void)forward_pending_requests(peer);
    }
  }
}

void Reactor::arm_write(const std::shared_ptr<Connection>& conn) noexcept {
  if (conn->fd() == kInvalidFd) return;
  // kClosed 才是"fd 已经没了"，那种连接不需要再挂任何事件。
  // **kClosing 也要挂**：它正是"还剩货没发出去"的那个状态，跳过它就是
  // 让这条连接既发不完也关不掉。这个判断只写一次，三个调用点共用。
  if (conn->state() == State::kClosed) return;
  if (conn->outbound_pending() == 0) return;
  modify_events(*conn, kReadWriteEvents);
}

void Reactor::close_connection(const std::shared_ptr<Connection>& conn, CloseWhy why) noexcept {
  const int fd = conn->fd();
  if (fd == kInvalidFd) return;  // 幂等：重复关闭不重复计数

  if (why == CloseWhy::kFlushThenClose && conn->outbound_pending() > 0) {
    // 第 0 步：货还没发完。这里关掉 fd 就等于把已经接受的字节丢在原地，
    // 而调用我的场景（上游回完就断、客户端提前断开）恰恰是有响应在排队的场景。
    conn->set_state(State::kClosing);
    arm_write(conn);
    return;
  }

  int error = 0;
  (void)epoll_.remove(fd, &error);  // 第 1 步

  std::shared_ptr<Connection> peer_to_close;
  auto it = connections_.find(fd);
  if (it != connections_.end() && it->second.get() == conn.get()) {
    const int peer_fd = conn->peer_fd();
    if (peer_fd != kInvalidFd) {
      auto pit = connections_.find(peer_fd);
      if (pit != connections_.end()) {
        // 先解除对端的反向引用，再递归关它。顺序反过来的话，对端会在我已经
        // 出表之后又顺着这个编号找到一个复用了我编号的新连接，把它当成"我的对端"关掉。
        pit->second->set_peer_fd(kInvalidFd);
        peer_to_close = pit->second;
      }
    }
    conn->set_peer_fd(kInvalidFd);
    connections_.erase(it);  // 第 3 步
  }

  conn->set_state(State::kClosed);
  conn->close_fd();  // 第 4 步：fd 编号从这一行起才可被复用
  retiring_.push_back(conn);
  stats_.connections_closed.fetch_add(1, std::memory_order_relaxed);

  if (peer_to_close) {
    close_connection(peer_to_close, why);
  }
}

void Reactor::retire_closed() noexcept {
  // 第 5 步：批处理结束，这批事件里再没有指向它们的指针了，引用才归零。
  retiring_.clear();
}

}  // namespace l7gate::net
