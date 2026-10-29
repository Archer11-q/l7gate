#include "net/reactor.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <string_view>
#include <system_error>
#include <utility>

#include "http/response_framing.h"
#include "net/socket.h"
#include "route/router.h"  // kNoUpstreamTarget：链调用点判"filter 没表态"用的那个哨兵

namespace l7gate::net {

namespace {

// Role / State 是 Connection 的嵌套类型；本文件里它们出现二十多次，
// 每个都写 Connection::Role 会让真正的逻辑淹没在限定名里。
using Role = Connection::Role;
using State = Connection::State;

// 单调时钟上的毫秒。排水兜底的 deadline 存的是**绝对时刻**，所以置它的一方（信号
// handler / accept 线程）和查它的一方（循环线程）读的是同一根轴，不需要谁每轮去
// 递减剩余量——递减的版本等于把兜底计时器做成"扫描频率的函数"。
std::int64_t monotonic_ms() noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// 排水期间的事件等待上限。边沿触发不会替一条静止的连接再报一次事件，而"在飞归零"
// 和"兜底到点"这两件事都只能在扫描里发现，所以排水时循环必须自己按节拍醒一次。
// 50ms 是"最坏多等一拍"与"别把下线变成一场忙等"之间的取值。
constexpr int kDrainTickMs = 50;

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
  // 池的桶数组在构造时按目标表长度定长（S3-01）。运行期不 resize：目标表变长是 S5
  // 热重载的话题，那时按 ADR-10 换成"整张表换一根指针"，而不是往一个正在被循环读的
  // 容器里 push（那和"运行期改链"是同一条 race）。
  upstream_pool_.resize(config_.upstream_targets.size());
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
  upstream_pool_.resize(config_.upstream_targets.size());  // 同上：定长在构造时
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
  // 池里的空闲上游也在 connections_ 里（上面已 close_fd），这里释放池表持有的那份
  // shared_ptr，让引用计数归零、对象析构（constructed==destructed 的最终兑现点）。
  upstream_pool_.clear();

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
    // 排水期间把等待上限收紧到 kDrainTickMs：不收紧的话，"在飞响应到齐了"这件事
    // 最坏要等一个 config_.wait_timeout_ms（生产 200ms）才被下一轮扫描看见，
    // 而兜底 deadline 也是只有扫描时才检查的量。
    const bool draining = draining_.load(std::memory_order_relaxed);
    const int wait_ms = draining ? std::min(config_.wait_timeout_ms, kDrainTickMs)
                                 : config_.wait_timeout_ms;
    const int n = epoll_.wait(wait_ms, &events_, &error);
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
      // 这条 break **不排水**：它的语义是"解析满 N 条就停循环"，帮助文本一直明写着
      // 它可能抢在最后一条响应送达之前退出。排水挂在信号 / request_drain() 上。
      break;
    }

    if (draining) {
      // 顺序固定：分流 → 判完成（"不再接受新连接"那一半在 accept_all / accept_and_dispatch
      // 里，它不关监听 fd，理由写在那两处）。
      drain_sweep();
      if (drain_pending_empty()) {
        // 排干了。由**循环线程自己**置停止标志，所以 ReactorGroup 只靠读每个 worker 的
        // stop_requested() 就知道"全部 worker 都排完了"，不需要任何完成投递——
        // 跨线程接触面仍然是 ADR-14 那一处，没有变大。
        stop_requested_.store(true, std::memory_order_release);
      }
    }
  }

  // S2-02（ADR-15）：循环退出时排空上游池——停掉的 worker 不该留着热连接，
  // 也让"stop 之后 constructed==destructed"这条生命周期断言成立（不必等 ~Reactor）。
  drain_upstream_pool();

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

void Reactor::request_drain(std::int64_t timeout_ms) noexcept {
  if (stop_requested_.load(std::memory_order_relaxed)) return;  // 已经在退出路上，没有可排的水
  // exchange 而不是 store：第二个 SIGTERM 会再次走到这里，那一次**不**重新设 deadline、
  // 也不重复计 drain_requested——兜底窗口从第一次信号算起，否则"多敲几次铃"就能把
  // 一次下线无限期延长，那正是这条兜底要防的事。
  const bool first = !draining_.exchange(true, std::memory_order_relaxed);
  if (first) {
    stats_.drain_requested.fetch_add(1, std::memory_order_relaxed);
    drain_deadline_ms_.store(monotonic_ms() + timeout_ms, std::memory_order_relaxed);
  }
  const std::uint64_t one = 1;
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
  // 排水期间监听 fd **照旧订阅读**，但每一条被 accept 到的连接立刻关掉（accepted
  // 出作用域 → UniqueFd 关 fd），不构造 Connection、不进 handoff、不进注册表。
  //
  // 为什么不是"直接把监听 fd 关掉"（那样客户端拿到 ECONNREFUSED，看起来更"停止接受"）：
  // 一、全项目的生命周期不变量是 `fd_start == fd_end`，而 fd_start 是在 start_listen
  //     **之后**量的——排水途中关掉监听 fd 会让每个既有冒烟脚本的这条断言变成 −1，
  //     等于把 S1 的门禁证据改成"要按停止方式加分支的等式"。那种等式是迟早会算错的。
  // 二、和 S1-04 既有的"超容量就 accept 掉再关"是同一条动作（那条也是 continue 不是
  //     return，ET 下必须把队列排空），不是新机制。
  // 代价照记：排水期间新建连接看到的是"连上就被关"，不是 ECONNREFUSED；后者要等进程
  // 退出、监听 socket 真的没了才成立。
  const bool draining = draining_.load(std::memory_order_relaxed);
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
    if (draining) {
      // 排水期间不接新连接：这条 fd 随 accepted 出作用域被 UniqueFd 关掉。
      // 仍然 continue（不是 return）——ET 下监听事件只报一次，带着没排空的队列退出
      // 就是 S1-04 记过的那类"过一会儿不接新连接了"的卡死，只不过这次卡的是收尾。
      stats_.drain_rejected_new_connections.fetch_add(1, std::memory_order_relaxed);
      continue;
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
    const std::shared_ptr<Connection>& downstream, std::uint16_t upstream_target) {
  // 索引目标表之前先认这条下标。`upstream_target` 是 filter 写回来的值（见链调用点），
  // 而这里的失败形态不是"回错一个后端"而是越界读，所以检查必须在索引它的那一处。
  if (upstream_target >= config_.upstream_targets.size()) {
    return nullptr;  // 装配漏检：当作"这一轮拿不到上游"，请求留在下游缓冲里，
                     // 不猜一个目标把请求发出去（猜 = 把配置错误读成后端脾气）。
  }
  const int existing = downstream->peer_fd();
  if (existing != kInvalidFd) {
    auto it = connections_.find(existing);
    if (it != connections_.end()) {
      std::shared_ptr<Connection> paired = it->second;
      if (paired->upstream_target() == upstream_target) return paired;
      // **目标变了。** 只有在干净边界上才换：在飞归零 + 出向空 + `upstream_reusable()`。
      // 这三个条件就是 ADR-15 那条"能不能归还进池"的判据本身（我刻意不另写一套——
      // "能进池却换不了目标"那种状态没人能解释）。换不动就返回**旧的那条**，由
      // forward_request 认出桶不对、把这条请求压回下游缓冲：换了就会把 ADR-16 的
      // 响应 FIFO 打乱（上一条的响应还在 A 路上，下一条却发给 B）。
      if (paired->in_flight_requests() == 0 && paired->outbound_pending() == 0 &&
          upstream_reusable(*paired)) {
        // 先解链，再按**它自己那条目标**归还——顺序和 close_connection 里那条级联归还
        // 同一个形状：recycle 要求调用方已经解配，否则它会带着一个仍然指向下游的
        // peer_fd 进池，下一条借它的下游就成了幽灵配对。
        paired->set_peer_fd(kInvalidFd);
        downstream->set_peer_fd(kInvalidFd);
        recycle_upstream(paired);
      } else {
        return paired;  // 压住，不是失败
      }
    }
    // 查不到就是那个 fd 已经关了（注册表反查认人，与批内事件同一条纪律）：
    // 配对关系随它一起消失，落到下面借/新建那条路。
  }
  // S2-02（ADR-15）：先从**这个目标自己的桶**里借一条热上游。借出**不新增连接**（它本来
  // 就在 connections_ 里），所以不受 fd 预算约束；桶空才落到下面新建那条受预算约束的路径。
  if (auto pooled = take_pooled_upstream(upstream_target)) {
    pooled->set_peer_fd(downstream->fd());
    downstream->set_peer_fd(pooled->fd());
    // 借出时它已注册、只挂着 EPOLLIN（空闲监控态）。请求字节随后由 forward_request
    // 排进它的出向缓冲并 arm_write 补上 EPOLLOUT，所以这里不必再 apply_events。
    return pooled;
  }
  if (connections_.size() >= config_.max_connections) {
    stats_.downstream_rejected_over_capacity.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }

  const Config::Target& target = config_.upstream_targets[upstream_target];
  ConnectOutcome outcome = start_connect(target.address, target.port);
  if (outcome.status == ConnectStatus::kFailed) {
    stats_.upstream_connect_failed.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }

  auto upstream = std::make_shared<Connection>(std::move(outcome.fd), Role::kUpstream,
                                               config_.connection, stats_);
  stats_.upstream_opened.fetch_add(1, std::memory_order_relaxed);
  // 目标标签必须在这里就写：它决定这条上游将来归还进**哪一个**桶。写在 stats 之后、
  // 注册之前，是为了让任何一条"借出去又还回来"的路径都不可能碰到一条没标签的上游。
  upstream->set_upstream_target(upstream_target);
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

// --- 上游连接池（S2-02，ADR-15；S3-01 按目标分桶，见 ADR-15 追加节）--------------
//
// 池是每 worker 一份、只被本循环线程碰的**按目标分桶**的空闲热上游表，所以全程无锁
// （ADR-14 的单线程拥有原样成立，这也是 ADR-15 选 per-worker 而不是 shared 的决定性理由）。
// 三条边界必须分清，混一条就是 bug：
//   借出（take_pooled_upstream）——从**指定那个目标的桶**移除、配对给某条下游；
//   归还（recycle_upstream）——下游关闭时（或干净边界换目标时），干净的上游解配、复位、
//      放回**它自己那条目标**对应的桶；
//   淘汰（evict_pooled + close）——空闲上游被对端 EOF、或那个桶满了、或不干净时关闭。
// 归还只发生在「下游关闭」或「目标切换」这两个干净边界上，不按单个请求（理由见 ADR-15
// 「为什么」：按请求借还会和 pipelining 的响应 FIFO 冲突，那是 S2-04 的配对模型）。

std::vector<std::shared_ptr<Connection>>* Reactor::pool_bucket(std::uint16_t target) noexcept {
  if (target >= upstream_pool_.size()) return nullptr;
  return &upstream_pool_[target];
}

const std::vector<std::shared_ptr<Connection>>* Reactor::pool_bucket(
    std::uint16_t target) const noexcept {
  if (target >= upstream_pool_.size()) return nullptr;
  return &upstream_pool_[target];
}

std::size_t Reactor::pool_idle_count() const noexcept {
  // 各桶之和（ADR-15 追加节改的口径）。单目标配置下与 S2 的 `upstream_pool_.size()`
  // 逐数相同——这是"历史读数不作废"那句话的实现形式，不是我说说的。
  std::size_t total = 0;
  for (const std::vector<std::shared_ptr<Connection>>& bucket : upstream_pool_) {
    total += bucket.size();
  }
  return total;
}

Reactor::Config::Target Reactor::target_at(std::size_t t) const noexcept {
  if (t >= config_.upstream_targets.size()) return Config::Target{};
  return config_.upstream_targets[t];
}

std::shared_ptr<Connection> Reactor::take_pooled_upstream(std::uint16_t target) {
  std::vector<std::shared_ptr<Connection>>* bucket = pool_bucket(target);
  if (bucket == nullptr) return nullptr;  // 没有这个目标的桶：不猜，当作借不到
  while (!bucket->empty()) {
    std::shared_ptr<Connection> upstream = bucket->back();
    bucket->pop_back();
    // 防御性核验：桶里的上游应当一直 kOpen 且仍在本 worker 的注册表里。不满足就是
    // 陈旧条目（理论上不该发生，但"从池里拿出来的东西"必须验，和批内反查同一条纪律）。
    // 这里**不**再验一次 `upstream_target() == target`：桶的下标就是目标，而归还与淘汰
    // 两处都只按 `upstream_target()` 选桶，所以"从 3 号桶里拿出一条 5 号目标的上游"
    // 不是被漏掉的检查、是结构上不成立的事。真要查它，查的是那个不变量的来源（写入点），
    // 而写入点已经被 ensure_upstream 的那次边界检查管住了。
    if (upstream->fd() == kInvalidFd || upstream->state() != State::kOpen) continue;
    auto it = connections_.find(upstream->fd());
    if (it == connections_.end() || it->second.get() != upstream.get()) continue;
    stats_.pool_borrows.fetch_add(1, std::memory_order_relaxed);
    return upstream;
  }
  return nullptr;
}

bool Reactor::upstream_reusable(const Connection& upstream) const noexcept {
  // 全部满足才值得留：kOpen（没在收尾/没死）、非隧道（隧道是专用双向流，无法共享）、
  // 非一次性配对（close_after_response 的语义就是不复用）、出向空（没有在飞请求字节）、
  // **没有在飞请求**（S2-04：请求都转出去了但响应还没全回来的上游不能回收，否则响应会
  // 配错人）、响应跟踪停在 kHead（上一条响应已完整完结并复位，没有半条响应挂在游标上）。
  // S3-01 起这个判据还兼管第二个问题：**能不能换目标**（ADR-18 第五节）。两个问题用
  // 同一个判据是刻意的——分成两套迟早出现"能进池却换不了目标"那种没人能解释的状态。
  return upstream.role() == Role::kUpstream && upstream.state() == State::kOpen &&
         !upstream.tunneling() && !upstream.close_after_response() &&
         upstream.outbound_pending() == 0 && upstream.in_flight_requests() == 0 &&
         upstream.response_tracking().phase == Connection::ResponsePhase::kHead;
}

void Reactor::recycle_upstream(const std::shared_ptr<Connection>& upstream) {
  // 调用方（close_connection 的下游分支 / ensure_upstream 的目标切换）已经把这条上游
  // 解配（peer_fd 置 kInvalidFd）。
  std::vector<std::shared_ptr<Connection>>* bucket = pool_bucket(upstream->upstream_target());
  if (bucket == nullptr) {
    // 目标桶不存在 = 这条上游带着一个表里没有的下标，只能关掉。计数走 evictions_full
    // 不合适（那不是"满"），走 close 就好：它是"没能留下"这一族的兜底路径。
    close_connection(upstream);
    return;
  }
  if (bucket->size() >= config_.pool_max_idle) {
    // 那个桶满了：给每连接内存一个硬上界（SLO #2），多出来的直接关，不留。
    // **口径注意**：这条上界现在是"每 worker 每目标"的，所以上界总量是 E × pool_max_idle。
    stats_.pool_evictions_full.fetch_add(1, std::memory_order_relaxed);
    close_connection(upstream);
    return;
  }
  upstream->reset_response_tracking();  // 干净起点：下一条借它的下游从 kHead 开始
  // 重新只挂 EPOLLIN：空闲上游要能被对端的 EOF 叫醒（好淘汰它），但出向已空，
  // 绝不能留着 EPOLLOUT 对着"随时可写"的 fd 自旋（那会吃掉 SLO #2 的空闲 CPU）。
  apply_events(upstream);
  bucket->push_back(upstream);
  stats_.pool_returns.fetch_add(1, std::memory_order_relaxed);
}

bool Reactor::evict_pooled(const std::shared_ptr<Connection>& upstream) {
  // 只去它自己那条目标的桶找——跨桶扫一遍会掩盖"标签写错"这个 bug，
  // 而标签写错的真实后果正是"请求发给错后端"，那件事必须留得住痕迹。
  std::vector<std::shared_ptr<Connection>>* bucket = pool_bucket(upstream->upstream_target());
  if (bucket == nullptr) return false;
  for (auto it = bucket->begin(); it != bucket->end(); ++it) {
    if (*it == upstream) {
      bucket->erase(it);
      return true;
    }
  }
  return false;
}

void Reactor::drain_upstream_pool() {
  // 循环退出时排空**所有桶**：停掉的 worker 不该留着热连接（否则 constructed==destructed
  // 要等 ~Reactor 才成立，而生命周期断言读的是 stop 之后、析构之前那一刻）。
  // 逐条 close_connection 会把它们从 connections_ 摘掉并推进 retiring_，最后统一兑现析构。
  for (std::vector<std::shared_ptr<Connection>>& bucket : upstream_pool_) {
    std::vector<std::shared_ptr<Connection>> idle;
    idle.swap(bucket);
    for (const std::shared_ptr<Connection>& upstream : idle) {
      if (upstream->fd() != kInvalidFd) close_connection(upstream);
    }
  }
  retire_closed();
}

// --- 优雅停止：排水（S2-05，ARCHITECTURE 1.20）-----------------------------
//
// 三条规则先写死在这里，因为"排水"这两个字太容易各写一套：
//   一、**排水粒度 = 按请求**，而"已收到的请求"的判据是 `in_flight_requests_ > 0`
//      （就是 ADR-16 那个在飞计数：转发 +1 / 响应完结 −1）。停在下游入向缓冲里、
//      还没转出去的完整请求属于**尚未接受**——排水期间 `forward_pending_requests_body`
//      入口直接 kBlocked，一条都不再转出去。我不为它回 4xx：网关不替一个正在下线的
//      自己伪造状态码，那条表态能力是 S4 第一个会停请求的 filter 才有的。
//   二、**空闲连接立即关**（在飞归零且缓冲为空，或只剩一条没被接受的完整请求）。
//      半条请求 / 写完就发呆的客户端落不进任何一格，只能交给兜底——这正是兜底存在
//      的理由，不是这里没做完。
//   三、**隧道只能硬关**（ADR-09 不解析帧 → 发不出 WS close 握手）。做法是把已缓冲的
//      帧字节两个方向各搬一次，然后按 kFlushThenClose 收尾；客户端的表现是断线重连。
//
// 三条都只在循环线程上跑，碰的全是 owner worker 自己的状态（ADR-14 原样成立）。
// 排水完成的判据也刻意留在**本线程**：循环线程自己置 stop_requested_，
// ReactorGroup 只读那个原子量，于是"谁在推进排水"始终是同一条线程。
//
// 「不再接受新连接」这一半不在这里，它在 accept_all() 里。刻意**不关监听 fd**，
// 理由与代价都写在 accept_all 的注释里——一句话版本：`fd_start == fd_end` 这条
// 不变量是全项目生命周期证据的地基，而 fd_start 是在 start_listen 之后量的。

void Reactor::drain_sweep() {
  const std::int64_t deadline = drain_deadline_ms_.load(std::memory_order_relaxed);
  const bool expired = deadline > 0 && monotonic_ms() >= deadline;

  // 快照一份再处理：注册表会在处理过程中被 erase（六步关闭第 3 步），
  // 拿着迭代器关连接就是给自己埋"边遍历边删"那一类雷。
  std::vector<std::shared_ptr<Connection>> snapshot;
  snapshot.reserve(connections_.size());
  for (const auto& [fd, conn] : connections_) snapshot.push_back(conn);

  // 一次性快照："停止那一刻手上还有几条在飞请求、还有几条停在停读线上"。只取第一次
  // 扫描，所以它们是**时刻**的读数，不是每轮累加（累加版会把同一个等待数 N 遍，那个
  // 数就没有任何口径可言了）。
  // 后一个快照在这里补 S1-07 那条观测的账：排水会把卡住的连接也收尾，循环停下之后
  // `read_disarmed_connections` 因此回 0——不在这一刻取，进程侧那份"网关不再消费下游
  // 可读事件"的直接证据就会被 S2-05 自己抹掉。取它的是循环线程自己，注册表此刻归它
  // 独占，所以这不违反"非原子观测只能在循环停下后读"那条纪律（stats.h 第 25 行）。
  if (!drain_first_sweep_done_) {
    drain_first_sweep_done_ = true;
    std::uint64_t waiting = 0;
    for (const std::shared_ptr<Connection>& conn : snapshot) {
      if (conn->role() != Role::kDownstream || conn->tunneling()) continue;
      if (conn->peer_fd() == kInvalidFd) continue;
      auto pit = connections_.find(conn->peer_fd());
      if (pit != connections_.end() && pit->second->in_flight_requests() > 0) ++waiting;
    }
    // fetch_add 而不是 store：`ReactorStats` 是**全 worker 共享一份**（ADR-14 第 4 条），
    // store 会让最后一个跑完第一次扫描的 worker 把前面所有 worker 的快照覆盖掉——
    // 那种写法在单 Reactor 下看起来完全正确，多 Reactor 下就是一个少报的数。
    stats_.drain_in_flight_at_entry.fetch_add(waiting, std::memory_order_relaxed);
    stats_.drain_read_disarmed_at_entry.fetch_add(
        static_cast<std::uint64_t>(connections_with_read_disarmed()), std::memory_order_relaxed);
  }

  auto peer_of = [this](const Connection& conn) -> std::shared_ptr<Connection> {
    if (conn.peer_fd() == kInvalidFd) return nullptr;
    auto it = connections_.find(conn.peer_fd());
    if (it == connections_.end()) return nullptr;
    return it->second;
  };

  for (const std::shared_ptr<Connection>& conn : snapshot) {
    if (conn->fd() == kInvalidFd) continue;
    // 认人和批内反查同一条纪律：fd 可能已经关掉了，也可能编号已经被新连接复用。
    auto fresh = connections_.find(conn->fd());
    if (fresh == connections_.end() || fresh->second.get() != conn.get()) continue;
    if (conn->state() == State::kClosed) continue;

    if (expired) {
      // 兜底到点：一律强关。kDiscardOutbound 跳过六步关闭的第 0 步，所以连"出向
      // 永远刷不完"的收尾中连接也能走完这一步——不绕的话就是"决定强关、却因为刷
      // 不完而永远关不掉"那条死锁形态。丢的是网关出向里没发出去的字节，客户端看到
      // 截断；这是兜底的代价，1.20 里记着，不在这里粉饰。
      if (conn->state() != State::kClosing) {
        // 收尾中的连接不重复计：它是在此前某轮被按 kFlushThenClose 发起关闭的，
        // 那时已经落进 drain_closed_idle / drain_closed_buffered 之一。
        stats_.drain_forced_timeout.fetch_add(1, std::memory_order_relaxed);
      }
      close_connection(conn, CloseWhy::kDiscardOutbound);
      continue;
    }

    // 收尾中的连接不需要我再判一次：flush_outbound 把出向刷完就会自己走完关闭。
    if (conn->state() == State::kClosing) continue;

    if (conn->tunneling()) {
      // 隧道硬关（规则三）。**只从下游那一侧动手**：注册表按 fd 顺序扫，先扫到上游侧
      // 的话，直接关它会级联关掉下游——配对照样收尾，但 `drain_tunnels_hard_closed`
      // 就是 0，而那个数正是"隧道走的是硬关、不是优雅握手"这句话唯一的可断言形态。
      // 第一版就是这么红的（用例断 1、实到 0），所以这条分支留在代码里而不是脑子里。
      if (conn->role() == Role::kUpstream) {
        if (peer_of(*conn) != nullptr) continue;  // 下游还在，由它驱动这一对的收尾
        close_connection(conn);                   // 只剩上游侧（下游已走）：刷完它的出向就关
        continue;
      }
      const std::shared_ptr<Connection> peer = peer_of(*conn);
      if (peer) {
        pump_tunnel(conn, peer);  // 客户端 → 上游：已缓冲的帧字节
        pump_tunnel(peer, conn);  // 上游 → 客户端：已缓冲的帧字节
      }
      stats_.drain_tunnels_hard_closed.fetch_add(1, std::memory_order_relaxed);
      close_connection(conn);  // 级联带走配对上游，两侧都按 kFlushThenClose 刷完出向
      continue;
    }

    if (conn->role() == Role::kUpstream) {
      // 空闲在池里的上游（peer 无效）不在这里关——它由循环出口的 drain_upstream_pool()
      // 收，那是 S2-02 既有的收尾路径；两条路径各关一次就等于把同一个对象数两遍。
      if (conn->peer_fd() == kInvalidFd) continue;
      const std::shared_ptr<Connection> peer = peer_of(*conn);
      const bool downstream_finished = (peer == nullptr) || (peer->state() != State::kOpen);
      if (downstream_finished && conn->in_flight_requests() == 0) close_connection(conn);
      continue;
    }

    // 剩下的就是"下游、非隧道、还 kOpen"——这条连接现在能不能收尾，判据只有一条：
    // 它配对的上游还有没有在飞请求。有就等（响应到齐的那一轮扫描会来关它）。
    const std::shared_ptr<Connection> peer = peer_of(*conn);
    const std::size_t in_flight = (peer != nullptr) ? peer->in_flight_requests() : 0;
    if (in_flight > 0) continue;
    const std::size_t buffered = conn->inbound().readable_bytes();
    if (buffered == 0 || conn->request() != nullptr) {
      if (buffered == 0) {
        stats_.drain_closed_idle.fetch_add(1, std::memory_order_relaxed);
      } else {
        stats_.drain_closed_buffered.fetch_add(1, std::memory_order_relaxed);
      }
      close_connection(conn);
    }
  }
}

bool Reactor::drain_pending_empty() const noexcept {
  for (const auto& [fd, conn] : connections_) {
    if (conn->fd() == kInvalidFd || conn->state() == State::kClosed) continue;
    // 出向还有货 = 交付没完成（含"收尾中的 kClosing"和"响应已排队但内核发送缓冲
    // 还没腾出来"两种）。这两种都必须等，否则这条判据会在响应还没送达时报"排干了"，
    // 而验收句要断的恰恰是"退出前它的响应完整送达"。
    if (conn->outbound_pending() > 0) return false;
    if (conn->role() == Role::kDownstream) {
      if (conn->state() == State::kOpen) return false;
    } else if (conn->state() == State::kOpen && conn->peer_fd() != kInvalidFd &&
               conn->in_flight_requests() > 0) {
      return false;
    }
  }
  return true;
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
  if (draining_.load(std::memory_order_relaxed)) {
    // S2-05 规则一的后半句：**排水期间不再接受新请求**。拦在这里而不是拦在 accept，
    // 是因为"已 accept 的连接"和"已接受的请求"是两件事——连接要留着把它在飞的请求
    // 答完，而字节已经进网关、还没转出去的那条请求按口径属于尚未接受。
    // 返回 kBlocked 而不是 kFailed：连接还活着，字节留在原地，由排水扫描决定它
    // 什么时候收尾；也不回 4xx（网关不替正在下线的自己伪造状态码，见本节开头）。
    return ForwardOutcome::kBlocked;
  }
  for (;;) {
    // close 是一次性配对的边界（S2-04，ADR-16）：转发过一条带 `Connection: close` 的请求
    // 之后，这条连接对不再转发任何后续请求——即使它们和 close 请求在同一次可读里到达。
    // 检查必须放在循环体每次迭代的开头（原来只在函数入口查一次），否则一批 pipelined 里
    // close 之后的请求会被同一次调用一股脑转出去、然后又被下面 pump_response 的 close 兑现
    // 提前砍掉。那些字节按 RFC 9112 是客户端违规（发了 close 记号就不得再 pipeline），
    // 随配对关闭一起丢弃，不回源、不伪造响应。返回 kBlocked 而非 kFailed：连接还活着，
    // 要等它已转出的那些请求的响应逐条回完（close 请求是最后转出的那条，它的响应到了才关）。
    if (downstream->close_after_forward()) {
      return ForwardOutcome::kBlocked;
    }
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
      // S3-01：这个对象不再是 const——它是链唯一的写通道（upstream_target / reject_status），
      // 下面两行就是那两个字段各自的第一个读者。**字段和读者同批进来**这条纪律（1.16 的债）
      // 在这里兑现：删掉任何一个读者，另一个就得写不进去。
      filter::RequestInfo info{
          .method = pending->method,
          .target = pending->target,
          .headers = pending->headers,
          .request_bytes = pending->consumed,
          .downstream_fd = downstream->fd(),
      };
      if (request_filters_.run(info) == filter::Decision::kStop) {
        if (info.reject_status != 0) {
          // **有表态的停止**。S3-01 的第一个走路人是"路由无匹配 → 404"（ADR-18 第二节）。
          // 走 S1-06 已有的那条自产响应路径（解链 → 回 → 下游 kFlushThenClose），
          // 所以这里返回 kFailed：连接已被本函数关掉，与解析错误那两条同一个形状。
          fail_pair_with_reply(downstream, info.reject_status);
          return ForwardOutcome::kFailed;
        }
        // 没有表态的停止 = S1-08 的原样行为。今天链上没有任何 filter 走这一格（路由要么
        // 命中要么 404），但它必须留着：S4 的限流第一批实现就是"停下但暂不表态"的形状。
        // 复用 kBlocked 而不是新加第四个枚举值：那个值的语义是"暂时转不出去，以后会再来试"，
        // 而被链停下的请求**永远**不会轮到它——两者现在行为一致（字节不动、连接活着），
        // 等 S4 给它配了响应再说，那时区别才有内容。
        return ForwardOutcome::kBlocked;
      }
      // 目标下标的读者一：filter 没表态就用本 worker 的默认目标（S2-06a 的 i mod E 就落在
      // 这一行——没装路由 filter 时这条路径与 S2 逐字节同形，那是八份 canonic 档不作废的凭据）。
      std::uint16_t upstream_target = info.upstream_target;
      if (upstream_target == route::kNoUpstreamTarget) upstream_target = config_.default_target;
      if (upstream_target >= config_.upstream_targets.size()) {
        // 路由规则指向一个不存在的端点 = 我的装配漏检。这里**不能**返回 kBlocked：
        // 那等于把这条请求永久压在缓冲里等一个不会出现的桶，而"安静地不动"正是本项目
        // 最忌讳的失效形态（原型见 S2-06 那 71,769 条 handshake-timeout）。回 502 而不是
        // 404：404 说的是"你要的东西没有路由"，这条说的是"有路由、那个路由的目的地没配"。
        fail_pair_with_reply(downstream, 502);
        return ForwardOutcome::kFailed;
      }
      if (!forward_request(downstream, pending->consumed, upgrade, close_after,
                           upstream_target)) {
        return ForwardOutcome::kBlocked;  // 上游没连上、出向满了，或目标要换但手上还有在飞
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
                              std::size_t request_bytes, bool upgrade, bool close_after,
                              std::uint16_t upstream_target) {
  std::shared_ptr<Connection> upstream = ensure_upstream(downstream, upstream_target);
  if (upstream == nullptr) {
    return false;  // 正在 connect、超出 fd 预算，或目标下标越界；请求还留在下游缓冲里，没丢
  }
  if (upstream->upstream_target() != upstream_target) {
    // S3-01（ADR-18 第五节）：**目标要换，但手上还有在飞的请求（或这条配对不干净）**。
    // ensure_upstream 把这个情况表达成"给我的还是旧目标那条"，而不是新造一个返回值——
    // 新造一个就违反了"S1-08 之后 ForwardOutcome 只有三个值、Decision 只有两个值"那条。
    // 这里返回 false 的语义和"上游出向满了"完全一致：**一个字节都没排、状态没动、
    // 这条请求留在原地下一次再试**。真正让它不会永远睡死的，是 pump_response 在
    // 在飞归零那一刻补的那一次重驱动（少它这条就是挂死，不是慢）。
    return false;
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
  // S2-04（ADR-16）：字节成功排队 = 这条请求已转发、开始占用上游一个在飞位。
  // 与 pump_response 的 note_response_completed 配平；close 兑现时机靠它判"归零没"。
  upstream->note_request_forwarded();

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
  // S3-01（ADR-18 第五节）：**在飞归零那一刻，把配对下游压着的那条请求重新推一次**。
  // 这条是"目标要换但手上还有在飞"那个分支欠下的还款：请求被压在下游缓冲里之后，
  // 边沿触发不会为"上游的响应刚好回完了"这件事再报一次下游事件，而客户端在等响应、
  // 不会为了唤醒我再写一个字节——**没有这一次重驱动，那条请求就永久睡死**（不是慢，是挂）。
  // 判据只用 `request() != nullptr`（= 已解析出一个完整请求、还没 finish_request），
  // 所以没被压住过的连接在这里只是一次指针读；`forward_pending_requests` 只在真的
  // 压着东西时才干活。
  // 放在外壳而不是 `kComplete` 分支里，理由和 `pump_response_body` 有七个 return 点
  // 同一条：分支里补一句等于写第七份"迟早漏一处"的抄本，而收在这里做一次就覆盖全部出口。
  if (upstream->in_flight_requests() != 0) return;
  const int peer_fd = upstream->peer_fd();
  if (peer_fd == kInvalidFd) return;  // 已经没有配对的下游（级联关闭/回收进池）
  auto it = connections_.find(peer_fd);
  if (it == connections_.end()) return;  // fd 编号已被复用：认人，不认编号
  if (it->second->request() == nullptr) return;
  (void)forward_pending_requests(it->second);
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
    // S2-04（ADR-16）：这条响应完结 = 一个在飞请求走完了，先把数减下去。
    const std::size_t remaining_in_flight = upstream->note_response_completed();
    if (upstream->close_after_response() && remaining_in_flight == 0) {
      // S1-05 留的口子在这里兑现，S2-04 修正了它的时机：**不是第一条响应一完结就关**，
      // 而是"带 close 的那条请求"的响应完结才关。close 请求是最后转发的那条（它的后面
      // 不再转发，见 forward_pending_requests_body 的循环内检查），所以它的响应也就是最后
      // 到达的那个——在飞数归零正是它的完结点。到这里关上游是安全的：它出向没有货、手上
      // 每一条已转发请求的响应都已完整交给下游。级联把下游也带向收尾，下游出向里的响应
      // 字节由 kFlushThenClose 保证发完。
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
    // S2-02（ADR-15）：空闲池内上游（未配对，peer_fd 无效）被对端关闭、或来了字节。
    // 它没有配对下游、没有在飞请求，所以这是一次**正常的空闲连接回收**——绝不能走下面
    // 的截断表态（那会把它误计成 upstream_truncated_before_head，而它压根没有下游可表态），
    // 也不级联任何下游。on_upstream_event 里上游 peer_fd 无效只可能是"空闲在池里"这一种
    // 情形（新建即配对、fail_pair 拆对后立刻关且已摘 epoll），所以这个判据是可靠的。
    if (conn->peer_fd() == kInvalidFd) {
      if (evict_pooled(conn)) {
        stats_.pool_evictions_eof.fetch_add(1, std::memory_order_relaxed);
      }
      close_connection(conn);
      return;
    }
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
  // 在改动 conn 之前记下它的角色：下面第 4 步之后 conn 已 kClosed，而"这条被关的是
  // 下游还是上游"决定了它的对端该被回收（下游关 → 上游进池）还是级联关闭（上游死 →
  // 下游跟着关，S1-06）。role_ 本身不随关闭改变，但提前存一份读起来更清楚。
  const bool conn_is_downstream = (conn->role() == Role::kDownstream);

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
    // S2-02（ADR-15）：只有「下游干净关闭」才把配对上游归还进池——上游因此活得比
    // 它的下游久，被下一条下游借走复用。两个守卫缺一不可：
    //   conn_is_downstream —— 上游死掉时它的对端是下游，那条下游要按 S1-06 级联关闭
    //                          （502/截断表态），绝不能"回收"一条下游；
    //   kFlushThenClose + upstream_reusable —— 丢弃式关闭（写坏了）或上游不干净
    //                          （隧道/一次性/出向还有货/响应没走完）都淘汰而不是归还。
    if (conn_is_downstream && why == CloseWhy::kFlushThenClose &&
        upstream_reusable(*peer_to_close)) {
      recycle_upstream(peer_to_close);
    } else {
      close_connection(peer_to_close, why);
    }
  }
}

void Reactor::retire_closed() noexcept {
  // 第 5 步：批处理结束，这批事件里再没有指向它们的指针了，引用才归零。
  retiring_.clear();
}

}  // namespace l7gate::net
