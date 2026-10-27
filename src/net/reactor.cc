#include "net/reactor.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <string_view>
#include <system_error>
#include <utility>

#include "http/response_framing.h"
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

// S1-04 时这里是两个常量（`kReadEvents` / `kReadWriteEvents`），六个使用点只在
// "读"与"读+写"之间切换，所以"既不读也不写"那个注册集在整个仓库里不存在。
// S1-07 把它变成必须的：撞停读线就是要摘掉 EPOLLIN。连接侧的掩码从此由
// `Reactor::desired_events()` 一处算出来（读兴趣 + 写兴趣），不再由调用方挑常量——
// 挑常量的版本等于让"哪里该停读"在六个使用点里各决定一次。
// 常驻的那两个 fd（监听、自唤醒）不在这条策略里：它们永远只读，写死 EPOLLIN。
// 顺带一笔：EPOLLIN 与 EPOLLOUT 都摘掉（events=0）是 epoll 允许的注册状态，
// 而 EPOLLERR/EPOLLHUP 无论如何都会上报，所以"完全没兴趣"不等于"这条连接聋了"。
constexpr std::uint32_t kPersistentReadEvents = EPOLLIN;

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

Reactor::Reactor(Config config)
    : config_(std::move(config)),
      owned_stats_(std::make_unique<ReactorStats>()),
      stats_(*owned_stats_) {
  // 自唤醒通道。stop() 从别的线程调用时只做一件事：往这个 fd 写 8 字节。
  // 不用 atomic 标志 + 依赖超时轮询的原因：那样最坏要等一个 wait_timeout_ms
  // 才停得下来，而"停不下来"在压测里表现为脚本挂住，在测试里表现为 TIMEOUT 失败。
  // worker 模式下这个 fd 还兼作 handoff 的唤醒信号（见 handoff()）。
  int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (fd < 0) {
    throw std::system_error(errno, std::system_category(), "eventfd");
  }
  wake_fd_ = UniqueFd{fd};
}

Reactor::Reactor(Config config, ReactorStats& shared_stats)
    : config_(std::move(config)),
      owned_stats_(nullptr),
      stats_(shared_stats) {
  // 与上面同一个自唤醒通道；区别只是 stats_ 绑定到外部共享的那一份（ADR-14 第 4 条）。
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

  if (!epoll_.add(listener_.get(), kPersistentReadEvents, listener_tag(), error)) {
    listener_.close_now();
    listen_port_ = -1;
    return false;
  }

  if (!epoll_.add(wake_fd_.get(), kPersistentReadEvents, wake_tag(), error)) {
    int ignore = 0;
    (void)epoll_.remove(listener_.get(), &ignore);
    listener_.close_now();
    listen_port_ = -1;
    return false;
  }
  *error = 0;
  return true;
}

bool Reactor::start_worker(int* error) noexcept {
  // worker 模式（ADR-14）：不绑监听 socket，只把自唤醒 eventfd 注册进 epoll。
  // 连接由 acceptor 线程经 handoff() 送进来，所以这里没有 listener_tag 那一档。
  // listen_port_ 保持 -1：worker 不监听，任何读它端口的代码都是错的。
  if (!epoll_.add(wake_fd_.get(), kPersistentReadEvents, wake_tag(), error)) {
    return false;
  }
  *error = 0;
  return true;
}

void Reactor::handoff(UniqueFd fd) noexcept {
  if (!fd.valid()) return;
  {
    std::lock_guard<std::mutex> lock(handoff_mutex_);
    handoff_fds_.push_back(std::move(fd));
  }
  // 叫醒 owner worker 的循环。和 stop() 同一个机制、同一条纪律：跨线程只写 eventfd，
  // 不碰循环内部状态。写失败只有 EAGAIN（计数大到写不进），那种情况循环必然已被
  // 唤醒，所以忽略返回值是安全的。
  const std::uint64_t one = 1;
  ssize_t rc = ::write(wake_fd_.get(), &one, sizeof(one));
  (void)rc;
}

void Reactor::drain_handoff() {
  std::vector<UniqueFd> fds;
  {
    std::lock_guard<std::mutex> lock(handoff_mutex_);
    fds.swap(handoff_fds_);
  }
  for (UniqueFd& fd : fds) {
    if (!fd.valid()) continue;
    if (connections_.size() >= config_.max_connections) {
      // 与 accept_all 同一口径：超预算就拒，fd 随 UniqueFd 析构关掉。
      // 注意这是**每 worker** 的预算，总容量 = N × max_connections（ADR-14 后果段）。
      stats_.downstream_rejected_over_capacity.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    auto conn = std::make_shared<Connection>(std::move(fd), Role::kDownstream,
                                             config_.connection, stats_);
    stats_.downstream_accepted.fetch_add(1, std::memory_order_relaxed);
    int error = 0;
    (void)set_tcp_nodelay(conn->fd(), &error);
    if (!register_connection(*conn, desired_events(*conn))) {
      close_connection(conn);
      continue;
    }
    connections_.emplace(conn->fd(), conn);
    // ADR-14 第 5 条：ET 不为"注册之前已经缓冲的字节"负责。fd 从 acceptor 线程
    // accept、排队、到这里注册，窗口比单 Reactor 的 accept_all 大得多，客户端很可能
    // 已经把请求写进了内核接收缓冲而注册那一刻没有新跳变。补一次探测读把已缓冲的
    // 字节读干净——复用 S1-07 背压恢复那套机器，不另写路径。
    schedule_probe(conn);
  }
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

void Reactor::add_request_filter(std::unique_ptr<filter::RequestFilter> filter) noexcept {
  request_filters_.add(std::move(filter));
}

std::uint32_t Reactor::armed_events(int fd) const noexcept {
  auto it = connections_.find(fd);
  if (it == connections_.end()) return 0;
  return it->second->armed_events();
}

std::size_t Reactor::connections_with_read_disarmed() const noexcept {
  // "网关不再消费下游可读事件"这句话在代码里的直接对应物就是这个数：
  // 注册表里有多少条连接的 interest list 上已经没有 EPOLLIN。
  // 它不是计数器（不需要循环在跑的时候也准），所以按非原子观测的规矩来：
  // 只在循环线程停下之后读（stats.h 第 25 行那条纪律）。
  std::size_t disarmed = 0;
  for (const auto& [fd, conn] : connections_) {
    if (conn->state() != Connection::State::kOpen) continue;  // 收尾中的连接本来就不该读
    if ((conn->armed_events() & EPOLLIN) == 0) ++disarmed;
  }
  return disarmed;
}

void Reactor::handle_events(const std::vector<Epoll::Event>& events) {
  for (const auto& ev : events) {
    if (ev.user_data == wake_tag()) {
      wake_up();
      // worker 模式：自唤醒 eventfd 兼作 handoff 信号（ADR-14）。单 Reactor 模式下
      // handoff_fds_ 恒空，这一句是 no-op，所以无条件调用是安全的。
      drain_handoff();
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
    if (!register_connection(*conn, desired_events(*conn))) {
      close_connection(conn);
      return;
    }
    connections_.emplace(conn->fd(), conn);
  }
}

bool Reactor::register_connection(Connection& conn, std::uint32_t events) noexcept {
  int error = 0;
  if (!epoll_.add(conn.fd(), events, &conn, &error)) return false;
  // 只在成功分支写：这份回执的意义就是"内核里现在确实是这个掩码"，
  // 失败也写就等于给后面的去重喂了一个假账本。
  conn.set_armed_events(events);
  return true;
}

bool Reactor::modify_events(Connection& conn, std::uint32_t events) noexcept {
  int error = 0;
  if (!epoll_.modify(conn.fd(), events, &conn, &error)) return false;
  conn.set_armed_events(events);
  return true;
}

// --- 背压的判定与动作（S1-07）--------------------------------------------
//
// 判据只有一条，而且刻意只有一条：**这条连接自己的入向可读区相对停读线在哪**。
// "对端的出向缓冲有多满"不是判据——不是没想到，是它会把同一件事记两次账：
// 上下游任何一侧的水位都是硬上限（Buffer 的不变量），所以"内存不继续增长"这条
// 在只按可读区判时就已经成立；把对端状态再叠进来只会让恢复条件多一个来源，
// 而多一个来源就多一处"迟早对不上"的地方。口径与滞回的理由记在 ARCHITECTURE 1.15。
//
// 这条判据只管**摘不摘订阅**，不管**要不要再读一次**，后者是边沿触发的旧账：
// 一轮读停在停读线上就是不等于停在 EAGAIN 上，内核里剩下的字节不会再有跳变来
// 交代。所以 apply_events 除了算掩码，还负责把"欠着一次读"的连接排进探测队列。
// 两条分开写才不会互相遮蔽：只看水位会睡死（响应方向第一次跑红的就是它），
// 只看跳变会在水位线上多打 epoll_ctl。
bool Reactor::wants_epollin(const Connection& conn) const noexcept {
  // 收尾中的连接不再接新活（原来靠 on_downstream_readable 开头的状态判断兜住，
  // 现在直接从订阅面上摘掉：不订阅就不会被叫醒，也就没有"醒了才发现不该干活"的分支）。
  if (conn.state() == State::kClosing || conn.state() == State::kClosed) return false;

  const std::size_t hwm = conn.inbound().high_water_mark();
  const std::size_t readable = conn.inbound().readable_bytes();
  const bool read_armed = (conn.armed_events() & EPOLLIN) != 0;
  // 滞回：到线才停，回落到半线以下才恢复。写成同一个条件的版本（`readable < hwm`
  // 就恢复）在水位线上反复横跳——每搬掉一条刚够 1 KiB 的请求就挂/摘一次 EPOLLIN，
  // 两次 epoll_ctl。背压本该是省下 syscall 的那条路，不该自己变成一场风暴。
  if (read_armed) return readable < hwm;
  return readable <= hwm / 2;
}

std::uint32_t Reactor::desired_events(const Connection& conn) const noexcept {
  std::uint32_t events = 0;
  if (wants_epollin(conn)) events |= EPOLLIN;
  // kConnecting 的上游必须带 EPOLLOUT：那是 connect 结果的唯一通知方式。
  // 出向有货也带（包括 kClosing —— 那里恰恰是最容易漏挂的一处，S1-04 记过）。
  if (conn.state() == State::kConnecting || conn.outbound_pending() > 0) events |= EPOLLOUT;
  return events;
}

void Reactor::apply_events(const std::shared_ptr<Connection>& conn) {
  if (conn->fd() == kInvalidFd) return;  // 第 4 步之后编号已经可以被复用，别再碰
  if (conn->state() == State::kClosed) return;

  const std::uint32_t want = desired_events(*conn);
  const std::uint32_t have = conn->armed_events();
  // 标记在这里一次性消费（take_）：它问的是"上一次重算之后这条连接有没有欠一次读"，
  // 而这次重算就是它的答复。留着不用等于把一次欠账记成永久欠账。
  const bool stopped_at_line = conn->take_read_stopped_at_line();

  if (want == have) {
    // 去重：这一个 return 就是每请求 epoll_ctl 的支出闸门。但"掩码不用改"不等于
    // "这条连接不用再读"——停在停读线上的那一轮**没有读到 EAGAIN**，内核接收缓冲
    // 里可能还压着字节，而边沿触发只为新跳变负责。对端如果已经把所有字节交出去
    // （写完就等回音），那就再没有跳变了：字节躺在内核里，连接挂在 EPOLLIN 上，
    // 谁都不动。响应方向第一次跑就是停在这个形状上（读到 96 KiB 之后彻底安静）。
    if (stopped_at_line && (want & EPOLLIN) != 0) schedule_probe(conn);
    return;
  }

  const bool had_read = (have & EPOLLIN) != 0;
  const bool wants_read = (want & EPOLLIN) != 0;
  // 计 pause 的条件比"EPOLLIN 从有到无"窄：kClosing 那条跳变也是摘掉 EPOLLIN，
  // 但那是收尾不是背压。混进来的话这个数就不再等于"停读线触发了几次"，
  // 而验收要的正是后者。
  const bool by_water_line =
      conn->state() == State::kOpen &&
      conn->inbound().readable_bytes() >= conn->inbound().high_water_mark();

  if (!modify_events(*conn, want)) return;  // epoll_ctl 失败：回执不变，下轮重算再试

  if (had_read && !wants_read) {
    if (by_water_line) {
      stats_.backpressure_pause_count.fetch_add(1, std::memory_order_relaxed);
    }
    return;
  }
  if (!had_read && wants_read) {
    stats_.backpressure_resume_count.fetch_add(1, std::memory_order_relaxed);
    // 挂回 EPOLLIN 只是把"以后的跳变"要回来了；已经躺在内核接收缓冲里的那批字节
    // 不会再产生跳变。所以恢复的语义必须包含一次主动读，否则这条连接永久睡死。
    schedule_probe(conn);
    return;
  }
  // 只动 EPOLLOUT 的跳变（读的一侧从头挂到尾）：这一轮若停在过线上，同样欠一次读。
  if (stopped_at_line && wants_read) schedule_probe(conn);
}

void Reactor::schedule_probe(const std::shared_ptr<Connection>& conn) {
  pending_probes_.push_back(conn);
  drain_probes();
}

void Reactor::probe_read(const std::shared_ptr<Connection>& conn) {
  // 复用事件处理的那两条路径，而不是另写一条"探测版"：读干净 + 路由字节 +
  // 撞线再摘，语义和真事件一模一样。revents 给 0 / 只给 EPOLLIN，意思就是
  // "这次没有任何电平信号，是我主动去看的"——EOF 因此只能由 read() 返回 0 得出。
  if (conn->role() == Role::kDownstream) {
    on_downstream_readable(conn, 0);
  } else {
    on_upstream_event(conn, EPOLLIN);
  }
}

void Reactor::drain_probes() {
  if (probing_) return;  // 已经在排空：嵌套的恢复只入队，由下面这个循环接手
  probing_ = true;
  while (!pending_probes_.empty()) {
    const std::shared_ptr<Connection> conn = pending_probes_.back();
    pending_probes_.pop_back();
    // 认人方式和批内反查同一条：fd 可能已经关掉了（探测队列里躺着的就是上一条
    // 事件里被级联关闭的连接），也可能编号已经被新连接复用。
    auto it = connections_.find(conn->fd());
    if (it == connections_.end() || it->second.get() != conn.get()) continue;
    probe_read(conn);
  }
  probing_ = false;
}

void Reactor::retry_peer_after_room(const std::shared_ptr<Connection>& conn) {
  auto it = connections_.find(conn->peer_fd());
  if (it == connections_.end()) return;
  const std::shared_ptr<Connection> peer = it->second;
  if (conn->role() == Role::kDownstream) {
    if (peer->tunneling()) {
      pump_tunnel(peer, conn);  // 对端（上游）在隧道里：重试帧字节的搬运
    } else {
      pump_response(peer);
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
  // 掩码交给 desired_events 算而不是写死那两个常量：kConnecting 由状态机保证带上
  // EPOLLOUT；回环上 connect 立刻完成的那条分支此刻出向还是空的，于是只订阅读——
  // 请求是下一步才排进来的，届时 arm_write 会补上 EPOLLOUT。
  if (!register_connection(*upstream, desired_events(*upstream))) {
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
      pump_tunnel(conn, it->second);  // 搬成功时在末尾重算 src 的兴趣（S1-07）
    } else {
      apply_events(conn);  // 没配对可搬：这一轮的读仍然可能把可读区顶到停读线
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
    // S1-06 的映射点：ParseError → 可定义的 4xx/5xx，回完再关。
    // kNone 不可能是 reject（reject 只在 error != kNone 时置位），status 给 0
    // 只是把"理论上没有分类的错误"留给 fail_pair 的兜底路径处理。
    fail_pair_with_reply(conn, http::reply_status_for_parse_error(round.error));
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

// 外壳只干一件事：这一轮无论走到哪个出口，下游入向的**可读区高度**都可能已经变了
// （转走了一条请求，或者一条都没转走），而 S1-07 的停读/恢复只看这个量。
// 写成外壳而不是在每个 return 前补一句，理由和 pump_response 那条一样：
// 本体有七个出口，"每个出口都记得补"是我迟早会漏的那种纪律，不是已经有的纪律。
Reactor::ForwardOutcome Reactor::forward_pending_requests(
    const std::shared_ptr<Connection>& downstream) {
  const ForwardOutcome outcome = forward_pending_requests_body(downstream);
  apply_events(downstream);
  return outcome;
}

Reactor::ForwardOutcome Reactor::forward_pending_requests_body(
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
      // S1-08 的链调用点，主路径上只有这一处。放在"取完意图之后、排字节之前"是有讲究的：
      // 意图已经定下来了（filter 拦不住一条已经被判定要隧道化的连接，那是它自己的语义），
      // 而一个字节都还没进上游的出向缓冲（拦下来就真的没回源，不是"回了源再假装没回"）。
      // RequestInfo 里的 view 是借来的，链跑完就作废——所以这里把它做成栈上临时对象，
      // 且在 finish_request 之前：filter 想留下任何字段，只能自己复制出去。
      const filter::RequestInfo info{
          .method = pending->method,
          .target = pending->target,
          .headers = pending->headers,
          .request_bytes = pending->consumed,
          .downstream_fd = downstream->fd(),
      };
      if (request_filters_.run(info) == filter::Decision::kStop) {
        // 停下不等于表态。今天没有任何业务 filter 会返回 kStop，所以这条出口的实际含义
        // 只有"不转发、字节留在原地、连接不关"；谁替客户端回那条 429/503，是第一个会停
        // 请求的 filter（S4 限流）的责任，那时这条出口才有自己的名字。
        // 复用 kBlocked 而不是新加第四个枚举值：那个值的语义是"暂时转不出去，以后会再来试"，
        // 而被链停下的请求**永远**不会轮到它 —— 两者现在行为一致（字节不动、连接活着），
        // 等 S4 给它配了响应再说，那时区别才有内容。
        return ForwardOutcome::kBlocked;
      }
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
    if (round.read.errno_value != 0) {
      close_connection(downstream);
      return ForwardOutcome::kFailed;
    }
    if (round.reject) {
      // 管线里后一条畸形。这里分两种语义，取决于配对上游有没有"在途"的请求：
      //  在途（上游出向还没刷完 / 上游已开始回响应）——第 i 条请求已转出去，
      //  客户端期望的第 i 条响应还在路上。此刻替"第 i+1 条"回 4xx 会把响应序列
      //  错一位（客户端会把它当成对第 i 条的答复），而且按 S1-05 定死的口径
      //  "不承诺 close 之后的请求"，4xx 的归属本身就说不清。诚实的做法是
      //  断链级联：客户端以连接终止感知，绝不错位表态。
      //  不在途——这条畸形就是第一个、也是唯一一个被网关"看见"的请求，响应
      //  序列没有错位问题，走正常的 reject→4xx 映射。
      const int ufd = downstream->peer_fd();
      std::shared_ptr<Connection> upstream;
      if (ufd != kInvalidFd) {
        auto uit = connections_.find(ufd);
        if (uit != connections_.end()) upstream = uit->second;
      }
      const bool in_flight =
          upstream != nullptr &&
          (upstream->outbound_pending() > 0 ||
           upstream->response_tracking().phase != Connection::ResponsePhase::kHead);
      if (in_flight) {
        close_connection(downstream);
      } else {
        fail_pair_with_reply(downstream, http::reply_status_for_parse_error(round.error));
      }
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

// 外壳干的同一件事：泵每消费掉一段上游入向的字节，都可能让可读区回落到恢复线以下。
// S1-07 之前这里"下次可读事件再说"是成立的（反正一直订阅着 EPOLLIN），
// 现在不成立——摘掉之后没有事件会来，恢复只能在这里判。
void Reactor::pump_response(const std::shared_ptr<Connection>& upstream) {
  pump_response_body(upstream);
  apply_events(upstream);
}

void Reactor::pump_response_body(const std::shared_ptr<Connection>& upstream) {
  if (upstream->tunneling()) {
    // 隧道路径在调用方（on_upstream_event / flush_outbound 的对称重试）已经分走，
    // 这里只保留一道断言式的守卫：状态串了宁可原地停住，也不把帧当响应搬（ADR-09）。
    return;
  }
  auto it = connections_.find(upstream->peer_fd());
  if (it == connections_.end()) return;  // 对端已关（或编号已被复用）：无去处
  const std::shared_ptr<Connection> downstream = it->second;

  using Phase = Connection::ResponsePhase;
  using Framing = http::ResponseFraming;

  for (;;) {
    Connection::ResponseTracking& r = upstream->response_tracking();
    const std::size_t available = upstream->inbound().readable_bytes();

    if (r.phase == Phase::kHead) {
      if (available == 0) return;
      const std::size_t head_len = upstream->detect_response_head();
      if (head_len == std::string_view::npos) {
        // 头未齐。唯一要当场判死的是"未齐且已经不可能是齐了"：可读区已经超出
        // 响应头预算还找不到终止串——那不是慢上游，是坏上游（响应头没有配置上的
        // 上限就等于给上游开了一个免费的内存放大口，方向反过来的 413）。
        if (available > config_.connection.parser.max_headers_bytes) {
          // 超长且仍未见终止串：不是慢，是坏。fail_pair 的入参是下游——
          // 表态要发的那一方；上游由它顺着 peer_fd 反查后按丢弃关闭。
          // 计数口径见 stats.h：这一支和"上游在头之前 EOF"是同一类表态。
          stats_.upstream_truncated_before_head.fetch_add(1, std::memory_order_relaxed);
          fail_pair_with_reply(downstream, 0);
          return;
        }
        return;  // 半包不是错误，也不是截断：等下一轮可读
      }
      const http::ResponseHeadInfo info =
          http::analyze_response_head(upstream->inbound().readable_data(), head_len);
      if (info.framing == Framing::kHeadInvalid) {
        // 头齐了但框架声明不可接受（坏状态行 / CL+TE / 不一致的多 CL / 非法 TE）。
        // 此刻**最终头还没排给下游**（无效头从不整块排），网关还有自产 502 的
        // 资格；把这段畸形字节转出去才是失职——客户端会看到一个下游没见过的
        // 上游在说话。
        // 计数与"头之前 EOF"同一口径：网关还没把最终头交出去，表态资格还在。
        stats_.upstream_truncated_before_head.fetch_add(1, std::memory_order_relaxed);
        fail_pair_with_reply(downstream, 0);
        return;
      }
      // 头必须**整块**排：半个响应头排出去就是把语法不完整的字节喂给客户端，
      // 而且头是框架结论的唯一来源——拆散发等于让"判定"和"发出"看到不同的字节。
      if (!downstream->queue_bytes(upstream->inbound().readable_data(), head_len)) {
        return;  // 下游出向水位满：头留在原地，刷完由 flush_outbound 重试
      }
      upstream->inbound().consume(head_len);
      stats_.bytes_up_to_downstream.fetch_add(static_cast<std::uint64_t>(head_len),
                                              std::memory_order_relaxed);
      arm_write(downstream);

      if (info.framing == Framing::kProvisional) {
        // 1xx（含"没配对成功的 101"——上游对无 upgrade 的请求回 101 是畸形流量，
        // 按 1xx 处理就是等它的最终响应）：本条无 body，后面跟的是**另一个头**。
        // 不置 final_head_forwarded（1xx 不剥夺 502 资格）、不完结。
        continue;
      }
      r.final_head_forwarded = true;  // 从这一行起，网关对这条响应只能以 EOF 表态
      switch (info.framing) {
        case Framing::kNoBody:
          r.framing = info.framing;
          r.phase = Phase::kComplete;
          break;
        case Framing::kContentLength:
          r.framing = info.framing;
          r.declared_body_bytes = info.declared_body_bytes;
          r.body_remaining = info.declared_body_bytes;
          r.phase = (info.declared_body_bytes == 0) ? Phase::kComplete : Phase::kBody;
          break;
        case Framing::kChunked:
          r.framing = info.framing;
          r.chunked = {};
          r.phase = Phase::kBody;
          break;
        case Framing::kCloseDelimited:
          r.framing = info.framing;
          r.phase = Phase::kBody;  // 完结点是 EOF，不是字节数（见 on_upstream_event）
          break;
        default:
          break;  // kHeadIncomplete/kHeadInvalid 在上面已经分走
      }
      if (r.phase == Phase::kComplete) continue;  // body 一条字节都没有：直接进完结处理
      continue;  // 头后字节（第一块 body / 下一段帧）本轮就在缓冲里，接着泵
    }

    if (r.phase == Phase::kBody) {
      if (available == 0) return;
      std::size_t forward = 0;
      bool reached_complete = false;
      bool broken_framing = false;

      if (r.framing == Framing::kChunked) {
        // 先推进游标再排字节：advance 会停在边界（最后一条 chunk 的终止空行之后），
        // 边界之后的字节属于下一条响应，本不该过框架判定，也不该在这一轮排出去。
        const std::size_t pos =
            http::chunked_advance(r.chunked, upstream->inbound().readable_data(), available);
        if (r.chunked.phase == http::ChunkedProgress::Phase::kInvalid) {
          // 框架坏了：advance 之前的字节已经透传，之后没有"边界"可言。
          // 最终头早发出去了，网关只能以 EOF 表态——级联关闭，客户端看到断流。
          broken_framing = true;
          forward = pos;
        } else {
          forward = pos;
          reached_complete = (r.chunked.phase == http::ChunkedProgress::Phase::kComplete);
        }
      } else if (r.framing == Framing::kContentLength) {
        forward = std::min(available, r.body_remaining);
        reached_complete = (forward == r.body_remaining);
      } else {
        forward = available;  // kCloseDelimited：全透传，EOF 才是完结
      }

      if (forward == 0) {
        if (broken_framing) {
          stats_.upstream_truncated_in_body.fetch_add(1, std::memory_order_relaxed);
          close_connection(upstream);  // 级联下游：EOF 语义就是截断的表态
          return;
        }
        return;
      }

      // body 可以拆散排：框架边界由游标管着，任意切分都不改变客户端看到的字节序。
      // "截断的权力在框架知识，不在这条泵"——拆排不拆判定。
      const std::size_t queued =
          downstream->queue_bytes_partial(upstream->inbound().readable_data(), forward);
      if (queued == 0) return;  // 出向满：数据留在上游缓冲，等下游腾出空间
      upstream->inbound().consume(queued);
      stats_.bytes_up_to_downstream.fetch_add(static_cast<std::uint64_t>(queued),
                                              std::memory_order_relaxed);
      arm_write(downstream);
      if (r.framing == Framing::kContentLength) r.body_remaining -= queued;

      if (broken_framing) {
        stats_.upstream_truncated_in_body.fetch_add(1, std::memory_order_relaxed);
        close_connection(upstream);
        return;
      }
      if (reached_complete && queued == forward) {
        r.phase = Phase::kComplete;
        continue;
      }
      if (queued < forward) return;  // 排了一部分（撞水位）：游标状态留在原地，下轮续
      continue;
    }

    // kComplete：这条响应对下游已经（或即将，由 flush 负责）完整交付。
    stats_.responses_completed.fetch_add(1, std::memory_order_relaxed);
    if (upstream->close_after_response()) {
      // S1-05 留的口子在这里兑现：一次性配对的响应**完结**了（不是猜边界，
      // 是框架判定给出的完结点），现在关上游是安全的——它出向没有货，入向的
      // 残字节属于"这条连接不再复用"之后的世界。级联把下游也带向收尾，
      // 下游出向里的响应字节由 kFlushThenClose 保证发完。
      close_connection(upstream);
      return;
    }
    // 重置必须**无条件**做在这里。我第一版写成"缓冲空就先 return，下次再来再重置"，
    // 那等于把 kComplete 留在连接上过夜：下一条响应到达时会先撞上这个尾巴，
    // responses_completed 凭空多 1，然后才从头判——一个"看起来对称"的漏记。
    upstream->reset_response_tracking();
    if (upstream->inbound().readable_bytes() == 0) return;  // 等下一条响应的字节
  }
}

void Reactor::fail_pair_with_reply(const std::shared_ptr<Connection>& downstream,
                                   std::uint16_t status) {
  // 对端已经不在了：没有谁接收这条表态。不排队、**不计数**——
  // gateway_replies_sent 的口径是"进了下游出向缓冲"，写在这个口径上的计数
  // 才是验收能引用的数。入向字节随下面这条 close 一起消失，没有谁欠它响应：
  // 配对已经断了，对端早就是 EOF 表态的世界。
  if (downstream == nullptr) return;

  // status==0 的调用点是"没有更细分类可用"的简写（坏/超长的上游头）。
  if (status == 0) status = 502;
  const std::string_view reply = http::gateway_error_reply(status);
  if (reply.empty()) return;  // 不该发生；发生了也不发一条空响应出去

  // 先解链再分别关。不这么做的话 close_connection 的级联会把**同一个 why**
  // 传染给对端：下游出向里刚排进去的 4xx/5xx 会被 kDiscardOutbound 顺手丢掉，
  // 客户端收到的表态从"一条完整错误响应"变成"连接断了"——那正是本函数要
  // 避免的事。解链后两侧各自按自己的意图关闭，互不传染。
  const int peer_fd = downstream->peer_fd();
  std::shared_ptr<Connection> upstream;
  if (peer_fd != kInvalidFd) {
    auto it = connections_.find(peer_fd);
    if (it != connections_.end()) {
      upstream = it->second;
    }
  }
  downstream->set_peer_fd(kInvalidFd);
  if (upstream) upstream->set_peer_fd(kInvalidFd);

  // 排队在解链之前还是之后都一样——queue 不看对端，看的是这块 buffer；
  // 但必须在下游还没被关掉之前：kClosed 之后 fd 已回收，发出去也没人收。
  if (!downstream->queue_bytes(reply.data(), reply.size())) {
    // 下游出向连一条几十字节的小响应都装不下——那不是水位，是这条连接已经
    // 病到没救（或它本来就在 kClosing 收尾、缓冲被大响应占满）。此刻发不出
    // 表态，就不能计"已发出"：gateway_replies_sent 只记真的进了出向缓冲的。
    if (upstream) close_connection(upstream, CloseWhy::kDiscardOutbound);
    close_connection(downstream, CloseWhy::kDiscardOutbound);
    return;
  }
  stats_.gateway_replies_sent.fetch_add(1, std::memory_order_relaxed);

  if (upstream) close_connection(upstream, CloseWhy::kDiscardOutbound);
  close_connection(downstream, CloseWhy::kFlushThenClose);
}

void Reactor::pump_tunnel(const std::shared_ptr<Connection>& src,
                          const std::shared_ptr<Connection>& dst) {
  pump_tunnel_body(src, dst);
  // 第三条同一形状的壳：搬成功要重算（可读区回落，可能要恢复），搬不动也要重算
  // （这一轮的读可能刚好把可读区顶到停读线，漏掉就是"该摘没摘"）。
  // 写在本体的每个 return 前面是五个抄本，写在壳里是一次。
  apply_events(src);
}

void Reactor::pump_tunnel_body(const std::shared_ptr<Connection>& src,
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
    // 原来这里写的是"没货待发就 modify 成 kReadEvents"。换成 apply_events 之后
    // 同一件事多担保了一层：出向还有货就继续挂 EPOLLOUT，一条都不漏。
    apply_events(conn);
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
      if (it == connections_.end()) {
        apply_events(conn);  // 没配对可搬：这一轮的读仍然可能顶到停读线
      } else {
        pump_tunnel(conn, it->second);  // 末尾会重算 src 的兴趣
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
      // 但"到此为止"不能带着一次没算过的兴趣判定：上面那次 read_raw 之前，
      // 头 + 帧字节可能已经把可读区顶到了停读线。
      apply_events(conn);
      return;
    }
    pump_response(conn);
    if (round.read.peer_closed) {
      // 上游 EOF。S1-06 的截断分类在这一个地方落定，口径只有两条：
      // **最终头还没排给下游** → 网关还有自产表态的资格 → 502 + 不对称关闭；
      // **头已经出去** → 再回 5xx 就是给同一条请求发两条响应，只能以连接
      // 终止表态（客户端拿框架知识自己判定截断——这正是这套框架泵存在的理由）。
      if (!conn->tunneling()) {
        Connection::ResponseTracking& r = conn->response_tracking();
        const bool complete_by_eof =
            r.phase == Connection::ResponsePhase::kComplete ||
            (r.phase == Connection::ResponsePhase::kBody &&
             r.framing == http::ResponseFraming::kCloseDelimited);
        if (!r.final_head_forwarded && !complete_by_eof) {
          // 含"一个字节都没回"和"只回了 1xx"两种：都还没有最终头，502 合法。
          stats_.upstream_truncated_before_head.fetch_add(1, std::memory_order_relaxed);
          auto pit = connections_.find(conn->peer_fd());
          if (pit == connections_.end()) {
            // 下游已经不在了：没人需要这条表态，直接把上游按丢弃关掉。
            close_connection(conn, CloseWhy::kDiscardOutbound);
            return;
          }
          fail_pair_with_reply(pit->second, 502);
          return;
        }
        if (r.phase == Connection::ResponsePhase::kBody &&
            r.framing != http::ResponseFraming::kCloseDelimited) {
          // CL/chunked 的 body 没走完就断了：框架意义上的截断，客户端会看到
          // 断流；网关不追加任何字节（头已经发出，追加就是伪造）。
          stats_.upstream_truncated_in_body.fetch_add(1, std::memory_order_relaxed);
        }
      }
      // 其余（完结、close-delimited 的 EOF=完结、隧道）跟着关：S2 建池之前
      // 配对连接对就是"一条响应序列"的生命周期，EOF 级联是唯一的收尾路径。
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

  if (!round.drained) {
    // 出向没刷空。原来这里直接 return——那是 S1-07 之前唯一的一处"背压已经解开、
    // 但没人去搬"的死角：内核发送缓冲被腾出一大块时确实会再报一次可写，可腾出的
    // 是**小块**时（对端慢读、窗口一点点打开）这一轮写掉了几 KB 却没写空，
    // 于是被压住的那一侧一直等到下一次可写事件才被想起来，而它等的正是我摘掉的
    // 那个事件。写了字节就立刻重试一次搬运；一个字节都没写出去才真的等下一轮。
    if (round.write.bytes_written == 0) return;
    apply_events(conn);
    retry_peer_after_room(conn);
    return;
  }

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
  // 原来是写死 `modify_events(*conn, kReadEvents)`；换成 apply_events 之后同一处
  // 还多担了一件事：这条连接如果正撞在停读线上，摘 EPOLLOUT 时不会顺手把 EPOLLIN 加回来。
  apply_events(conn);

  // 我这边腾出空间了，把对端压着发不出去的数据接过来继续走。
  retry_peer_after_room(conn);
}

void Reactor::arm_write(const std::shared_ptr<Connection>& conn) noexcept {
  if (conn->fd() == kInvalidFd) return;
  // kClosed 才是"fd 已经没了"，那种连接不需要再挂任何事件。
  // **kClosing 也要挂**：它正是"还剩货没发出去"的那个状态，跳过它就是
  // 让这条连接既发不完也关不掉。这个判断只写一次，三个调用点共用。
  if (conn->state() == State::kClosed) return;
  if (conn->outbound_pending() == 0) return;
  // 原来这里无条件 `modify_events(*conn, kReadWriteEvents)`，两个问题各属一个任务：
  //   一、无条件 = 每个请求至少多打一发 epoll_ctl（B-001 里 5.0 次/请求那笔支出的一部分）；
  //   二、总是带 EPOLLIN = 把刚因为撞停读线摘掉的读兴趣又打开，背压当场失效。
  // apply_events 一次解决两条：掩码由 desired_events 现算，相等就不 syscall。
  apply_events(conn);
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
  // 注销成功了就把回执清零。写在这里而不是等 ~Connection：这条 fd 编号从第 4 步
  // 起就可能被新连接复用，旧对象手里那份"我还订阅着什么"的账本留着没有任何读者，
  // 只有一个读者是错的（connections_with_read_disarmed() 遍历的是注册表，
  // 已经出表的对象不在里面，所以清零只为"关掉之后读数不骗人"这一条）。
  conn->set_armed_events(0);

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
