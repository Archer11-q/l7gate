#include "net/reactor_group.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>

#include <algorithm>
#include <cerrno>
#include <system_error>
#include <utility>

#include "filter/lb_filter.h"  // config_.groups 非空时给每个 worker 挂的均衡那一环
#include "filter/route_filter.h"  // config_.route_table 非空时给每个 worker 挂的那一环
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
    // S3-01（ADR-18）：整张目标表交给每一个 worker，而 `i mod E` 降级成**这个 worker 的
    // 默认目标**。所以：
    //   没装路由 filter 时——路由不表态 → 用默认目标 → worker i 仍然一辈子只连第 i mod E 个
    //     端点，S2-06a 的静态指派逐字节保留（容量公式 `min(N,E)×28,232`、B-007 的 100k、
    //     八份 canonic 档因此都不作废，`s1-p99.sh` 靠横幅签名判路径那两条也没被动到）；
    //   装了路由 filter 时——按请求选目标，池因此按目标分桶（ADR-15 追加节）。
    // 跨线程接触面仍然只有裸 fd：目标表是**构造期拷进去的只读数据**，运行期只在 owner
    // 线程读它，没有新增任何共享可变状态（TSan 是这条的裁判，不是我）。
    wc.upstream_targets = config_.upstream_endpoints;
    if (!config_.upstream_endpoints.empty()) {
      wc.default_target = static_cast<std::uint16_t>(
          static_cast<std::size_t>(i) % config_.upstream_endpoints.size());
    }
    wc.max_connections = config_.max_connections_per_worker;
    wc.connection = config_.connection;
    wc.wait_timeout_ms = config_.wait_timeout_ms;
    wc.stop_after_requests = 0;
    // S3-03（ADR-12 落地）：健康表与探活分派同样必须在构造前进 wc（与上面 lb_state
    // 同一条教训：Reactor 存 config 的副本，构造之后写等于没写）。探活分派按
    // `target % N == i` 切——探活流量不随 worker 数放大。
    wc.health = config_.health;
    wc.nodes = config_.nodes;
    wc.worker_index = static_cast<std::size_t>(i);
    wc.worker_group_size = static_cast<std::size_t>(n);
    // 均衡状态必须**在 Reactor 构造之前**就进 wc：Reactor 存的是 config 的一份副本，
    // 构造之后再往 wc 上写就等于没写——那份账会只加不减（本任务实测把这一格写错过一次，
    // 表现是 least_conn 永远选第一个成员，而它看起来完全"在正常工作"）。
    std::shared_ptr<l7gate::route::LbState> lb_state;
    if (config_.groups != nullptr && !config_.groups->empty()) {
      lb_state = std::make_shared<l7gate::route::LbState>(
          config_.upstream_endpoints.size(), config_.groups->size());
      wc.lb_state = lb_state;
    }
    auto worker = std::make_unique<Reactor>(wc, *stats_);
    // 两个 filter 都只能在 run() 之前挂（链在循环线程里被逐条读、无锁，S1-08 那条时序纪律）。
    // **顺序 = 路由在前、均衡在后**：均衡读的那个组下标就是路由这一环写下的，
    // 反过来的话链短路时没人决定过组，均衡只能退回默认目标。
    if (config_.route_table != nullptr && !config_.route_table->empty()) {
      worker->add_request_filter(std::make_unique<filter::RouteFilter>(*config_.route_table));
    }
    if (lb_state != nullptr) {
      worker->add_request_filter(std::make_unique<filter::LoadBalanceFilter>(
          *config_.groups, std::move(lb_state), config_.health, config_.nodes));
    }
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
  bool drain_dispatched = false;
  // accept 循环跑在调用线程上（生产里是主线程，ADR-03「主线程 accept」）。
  while (!stop_requested_.load(std::memory_order_acquire)) {
    int error = 0;
    // 排水期间把等待上限收紧到 50ms：worker 全部排干之后 accept 循环要能立刻发现，
    // 而不是等满 config_.wait_timeout_ms（生产里 200ms）——下线时间是被这条兜住的。
    const int wait_ms = drain_requested_.load(std::memory_order_relaxed)
                            ? std::min(config_.wait_timeout_ms, 50)
                            : config_.wait_timeout_ms;
    const int n = accept_epoll_.wait(wait_ms, &events_, &error);
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

    // S2-05：优雅停止。**先下发、后等齐**，两步都在这一条循环里做。
    if (drain_requested_.load(std::memory_order_acquire)) {
      if (!drain_dispatched) {
        // 「不再接受新连接」由 accept_and_dispatch() 自己按 drain_requested_ 判：
        // accept 到就关掉那条 fd、不再 handoff。**不关监听 fd**——理由写在
        // Reactor::accept_all 的注释里（fd_start==fd_end 是全项目的生命周期地基）。
        const std::int64_t timeout_ms = drain_timeout_ms_.load(std::memory_order_relaxed);
        for (auto& worker : workers_) {
          if (worker) worker->request_drain(timeout_ms);
        }
        drain_dispatched = true;
        continue;  // 本轮只负责下发，下一条判断从下一轮开始
      }
      // 每个 worker 排干之后会**自己**置上 stop_requested_（那是它循环线程的最后一步），
      // 所以这里只需要读那个原子量，不需要任何完成投递。用阻塞 join 等它们会挡住
      // accept 线程本身——而 worker 的进度需要它自己的线程在跑。
      if (all_workers_stop_requested()) break;
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

void ReactorGroup::request_drain(std::int64_t timeout_ms) noexcept {
  if (stop_requested_.load(std::memory_order_relaxed)) return;  // 已经在退出路上
  const bool first = !drain_requested_.exchange(true, std::memory_order_relaxed);
  if (first) {
    drain_timeout_ms_.store(timeout_ms, std::memory_order_relaxed);
  }
  // 只做 async-signal-safe 的那一件事：敲 accept 循环的门。真正做事的是 run() 里
  // 那条循环——它关掉监听 fd、把排水下发给 worker、然后等它们各自报"排干了"。
  const std::uint64_t one = 1;
  ssize_t rc = ::write(accept_wake_fd_.get(), &one, sizeof(one));
  (void)rc;
}

bool ReactorGroup::all_workers_stop_requested() const noexcept {
  // 只读每个 worker 的**原子**停止标志，所以 accept 线程在循环里查它是合法的。
  // worker 自己排干之后由它自己的循环线程置上这个标志（Reactor::run 的排水分支），
  // 因此这里不需要任何"排水完成"的投递队列——跨线程接触面没有变大。
  for (const auto& worker : workers_) {
    if (worker && !worker->stop_requested()) return false;
  }
  return true;
}

void ReactorGroup::accept_and_dispatch() {
  // 排水期间：照旧把 accept 队列排空（ET 的监听事件只报一次，不排空就是滞留），
  // 但每一条新连接**不构造 Connection、不 handoff**，随 accepted 出作用域就被关掉。
  // 与 Reactor::accept_all 同一条动作、同一个理由（那边注释记着为什么不关监听 fd）。
  const bool draining = drain_requested_.load(std::memory_order_relaxed);
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
    if (draining) {
      stats_->drain_rejected_new_connections.fetch_add(1, std::memory_order_relaxed);
      continue;
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
