#include "net/reactor_group.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>

#include <cerrno>
#include <system_error>
#include <utility>

#include "net/socket.h"

namespace l7gate::net {

namespace {

// accept 侧 epoll 的 user_data 标签：区分监听 socket 与自唤醒 eventfd。
// 和 reactor.cc 里那两个 Tag 同一个理由——用静态对象自己的地址当标签，天然唯一、
// 不可能与真实对象重合、拼错名字编译不过。worker 侧的标签在 reactor.cc 里，两边
// 各管各的 epoll，互不共享 user_data 空间。
struct AcceptTag {};
AcceptTag g_accept_listener_tag;
AcceptTag g_accept_wake_tag;

void* accept_listener_tag() noexcept { return &g_accept_listener_tag; }
void* accept_wake_tag() noexcept { return &g_accept_wake_tag; }

constexpr std::uint32_t kAcceptReadEvents = EPOLLIN;

}  // namespace

ReactorGroup::ReactorGroup(Config config)
    : config_(std::move(config)), stats_(std::make_unique<ReactorStats>()) {}

ReactorGroup::~ReactorGroup() {
  // 析构函数体先跑：join 掉所有 worker 线程，之后成员才按逆序 unwind
  // （threads_ → workers_ → stats_）。少了这一步，joinable 的 std::thread 被析构
  // 会 std::terminate，而 worker 还可能在 stats_ 被拆之后访问它。
  stop_workers_and_join();
}

bool ReactorGroup::start_listen(int* error) noexcept {
  SocketResult listener =
      make_listen_socket(config_.listen_address, config_.listen_port, config_.listen_backlog);
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

  const int afd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (afd < 0) {
    *error = errno;
    listener_.close_now();
    listen_port_ = -1;
    return false;
  }
  accept_wake_fd_ = UniqueFd{afd};

  if (!accept_epoll_.add(listener_.get(), kAcceptReadEvents, accept_listener_tag(), error)) {
    accept_wake_fd_.close_now();
    listener_.close_now();
    listen_port_ = -1;
    return false;
  }
  if (!accept_epoll_.add(accept_wake_fd_.get(), kAcceptReadEvents, accept_wake_tag(), error)) {
    int ignore = 0;
    (void)accept_epoll_.remove(listener_.get(), &ignore);
    accept_wake_fd_.close_now();
    listener_.close_now();
    listen_port_ = -1;
    return false;
  }

  // 构造 N 个 worker，全部共享 *stats_（ADR-14 第 4 条）。worker 不监听，所以
  // 它们的 Config 里 listen_* 留空、stop_after_requests 归零（停止由本 group 决定）。
  int n = config_.worker_count;
  if (n < 1) n = 1;
  for (int i = 0; i < n; ++i) {
    Reactor::Config wc;
    wc.upstream_address = config_.upstream_address;
    wc.upstream_port = config_.upstream_port;
    wc.max_connections = config_.max_connections_per_worker;
    wc.connection = config_.connection;
    wc.wait_timeout_ms = config_.wait_timeout_ms;
    wc.stop_after_requests = 0;
    auto worker = std::make_unique<Reactor>(wc, *stats_);
    if (!worker->start_worker(error)) {
      // 已经建好的 worker 由本对象析构清理（它们的 start_worker 若成功则 wake_fd
      // 已注册，~Reactor 会注销）。listener_/accept_epoll_ 同样由析构清理。
      return false;
    }
    workers_.push_back(std::move(worker));
  }

  *error = 0;
  return true;
}

std::uint64_t ReactorGroup::run() {
  running_.store(true, std::memory_order_relaxed);

  // 先 spawn worker 线程：它们各自 start_worker 已在 start_listen 里完成，
  // 这里只负责跑循环。worker 一上来就可能收到 handoff（accept 循环随后才开始）。
  for (std::size_t i = 0; i < workers_.size(); ++i) {
    threads_.emplace_back([this, i] { workers_[i]->run(); });
  }

  std::uint64_t batches = 0;
  // accept 循环跑在调用线程上（生产里是主线程，ADR-03「主线程 accept」）。
  while (!stop_requested_.load(std::memory_order_acquire)) {
    int error = 0;
    const int n = accept_epoll_.wait(config_.wait_timeout_ms, &events_, &error);
    if (n < 0) break;  // accept_epoll_ 坏了：继续只会让后面的数字不可信
    if (!events_.empty()) {
      for (const auto& ev : events_) {
        if (ev.user_data == accept_wake_tag()) {
          drain_accept_wake();
          continue;
        }
        if (ev.user_data == accept_listener_tag()) {
          accept_and_dispatch();
        }
      }
      ++batches;
    }
    if (config_.stop_after_requests > 0 &&
        stats_->requests_parsed.load(std::memory_order_relaxed) >= config_.stop_after_requests) {
      break;
    }
  }

  stop_workers_and_join();
  running_.store(false, std::memory_order_relaxed);
  return batches;
}

void ReactorGroup::stop() noexcept {
  // 顺序与 Reactor::stop() 同一条理由：先落"要停"这个决定，再敲门。
  stop_requested_.store(true, std::memory_order_release);
  const std::uint64_t one = 1;
  ssize_t rc = ::write(accept_wake_fd_.get(), &one, sizeof(one));
  (void)rc;
}

void ReactorGroup::accept_and_dispatch() {
  while (true) {
    AcceptOutcome accepted = accept_connection(listener_.get());
    if (!accepted.ok()) {
      if (accepted.would_block()) {
        return;  // ET 的正常终点：一次事件可能带来多个连接，accept 到没为止
      }
      // EMFILE/ENFILE 一类：进程级 fd 耗尽，继续循环是忙等。accept_errors 替我说话。
      stats_->accept_errors.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (workers_.empty()) return;  // 不该发生（start_listen 至少建 1 个）
    // round-robin：第 k 条 accept 交给 worker[k mod N]。确定性、可断言均衡（ADR-14 第 3 条）。
    const std::size_t idx = rr_counter_.fetch_add(1, std::memory_order_relaxed) % workers_.size();
    workers_[idx]->handoff(std::move(accepted.fd));
  }
}

void ReactorGroup::drain_accept_wake() {
  std::uint64_t counter = 0;
  while (true) {
    const ssize_t rc = ::read(accept_wake_fd_.get(), &counter, sizeof(counter));
    if (rc <= 0) break;  // EAGAIN 或真错误都到此为止
  }
}

void ReactorGroup::stop_workers_and_join() {
  for (auto& worker : workers_) {
    if (worker) worker->stop();
  }
  for (auto& t : threads_) {
    if (t.joinable()) t.join();
  }
}

std::uint64_t ReactorGroup::epoll_registrations() const noexcept {
  std::uint64_t sum = 0;
  for (const auto& worker : workers_) sum += worker->epoll_registrations();
  return sum;
}

std::uint64_t ReactorGroup::epoll_unregistrations() const noexcept {
  std::uint64_t sum = 0;
  for (const auto& worker : workers_) sum += worker->epoll_unregistrations();
  return sum;
}

std::uint64_t ReactorGroup::epoll_modifications() const noexcept {
  std::uint64_t sum = 0;
  for (const auto& worker : workers_) sum += worker->epoll_modifications();
  return sum;
}

std::size_t ReactorGroup::live_connection_count() const noexcept {
  std::size_t sum = 0;
  for (const auto& worker : workers_) sum += worker->live_connection_count();
  return sum;
}

std::size_t ReactorGroup::connections_with_read_disarmed() const noexcept {
  std::size_t sum = 0;
  for (const auto& worker : workers_) sum += worker->connections_with_read_disarmed();
  return sum;
}

std::size_t ReactorGroup::worker_live_count(int i) const noexcept {
  if (i < 0 || static_cast<std::size_t>(i) >= workers_.size()) return 0;
  return workers_[static_cast<std::size_t>(i)]->live_connection_count();
}

}  // namespace l7gate::net
