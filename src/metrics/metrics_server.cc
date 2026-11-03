#include "metrics/metrics_server.h"

#include "route/config_snapshot.h"
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <utility>

#include "filter/rate_limit.h"
#include "filter/redis_rate_limit.h"
#include "net/proc_sample.h"

namespace l7gate::metrics {
namespace {

// 采集线程自己的单调毫秒。它与 `main.cc` 那个 `started`、以及 worker 循环里的
// `monotonic_ms()` 走同一根轴（`steady_clock`），否则 `elapsed_ms` 两侧读的就不是
// 同一段时长。
std::int64_t steady_ms() noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

constexpr std::size_t kNoSlot = static_cast<std::size_t>(-1);

// epoll 的 `user_data` 标签。这张 epoll 上只有本线程自己的 fd，没有 `Connection`，
// 所以标签体系是私有的：**低位为 1 = 保留标签**，其余是「(槽位 + 2) << 1」。
// 用索引而不是指针，照的是探活那一族的先例（`reactor.cc:438-442`）——槽位表是固定
// 数组，永远不会因为搬家而让一个在册 fd 指向旧地址。
// 不是 `constexpr`：`reinterpret_cast` 从整数到指针不算常量表达式，编译器就在这儿拦我。
void* const kListenerTag = reinterpret_cast<void*>(0x1);
void* const kWakeTag = reinterpret_cast<void*>(0x3);

void* conn_tag(std::size_t slot) noexcept { return reinterpret_cast<void*>((slot + 2) << 1); }

std::size_t tag_to_slot(void* tag) noexcept {
  const auto raw = reinterpret_cast<std::uintptr_t>(tag);
  if ((raw & 1u) != 0 || raw < 4) return kNoSlot;
  return (raw >> 1) - 2;
}

constexpr char kContentType[] = "text/plain; version=0.0.4; charset=utf-8";

std::string build_response(int status, const char* status_text, std::string body,
                           bool exposition_type) {
  std::string out = "HTTP/1.1 ";
  out += std::to_string(status);
  out += ' ';
  out += status_text;
  out += "\r\n";
  out += "Content-Type: ";
  out += exposition_type ? kContentType : "text/plain; charset=utf-8";
  out += "\r\n";
  out += "Content-Length: ";
  out += std::to_string(body.size());
  out += "\r\n";
  // 每次响应都关连接：不实现 keep-alive，就没有会话状态可写（ADR-24 第六节）。
  // 代价是每拍多一次 TCP 握手，换来的是仪器少一个能出 bug 的面。
  out += "Connection: close\r\n\r\n";
  out += body;
  return out;
}

// 请求行的三段。返回 false = 不是合法的 `METHOD SP TARGET SP HTTP/x.y`。
bool split_request_line(const std::string& request, std::string_view* method,
                        std::string_view* target) noexcept {
  const std::size_t line_end = request.find("\r\n");
  if (line_end == std::string::npos) return false;
  const std::string_view line(request.data(), line_end);
  const std::size_t first = line.find(' ');
  if (first == std::string_view::npos) return false;
  const std::size_t second = line.find(' ', first + 1);
  if (second == std::string_view::npos) return false;
  *method = line.substr(0, first);
  *target = line.substr(first + 1, second - first - 1);
  return line.substr(second + 1).rfind("HTTP/", 0) == 0;
}

}  // namespace

MetricsServer::MetricsServer(Config config, Dependencies deps) noexcept
    : config_(std::move(config)), deps_(std::move(deps)) {}

MetricsServer::~MetricsServer() {
  stop();
  // 常驻的两个 fd 在这里关：`stop()` 之后、`report_stats` 里采的 `fd_end` 仍然看得见它们，
  // 与 `fd_start` 同一视野（理由写在 `stop()` 那段注释里）。
  if (listener_.valid()) {
    int ignored = 0;
    epoll_.remove(listener_.get(), &ignored);
    listener_.close_now();
  }
  if (wake_fd_.valid()) {
    int ignored = 0;
    epoll_.remove(wake_fd_.get(), &ignored);
    wake_fd_.close_now();
  }
}

bool MetricsServer::start_listen(int* error) noexcept {
  l7gate::net::SocketResult listener =
      l7gate::net::make_listen_socket(config_.bind_address, config_.port, config_.backlog);
  if (!listener.ok()) {
    *error = listener.error;
    return false;
  }
  const int bound = l7gate::net::get_socket_port(listener.fd.get());
  if (bound < 0) {
    *error = errno;
    return false;
  }
  // 自唤醒通道：`stop()` 在别的线程只做"置标志 + 写一个数"，不碰本线程的任何状态
  // （与 `Reactor::stop()` / `ReactorGroup::stop()` 同一条跨线程纪律）。
  l7gate::net::UniqueFd wake{::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)};
  if (!wake.valid()) {
    *error = errno;
    return false;
  }
  if (!epoll_.add(listener.fd.get(), EPOLLIN, kListenerTag, error)) {
    return false;
  }
  if (!epoll_.add(wake.get(), EPOLLIN, kWakeTag, error)) {
    // 半个注册留在这儿不比全注册更好：把 listener 也摘掉，回到"没开始"的状态。
    int ignored = 0;
    epoll_.remove(listener.fd.get(), &ignored);
    return false;
  }
  listener_ = std::move(listener.fd);
  wake_fd_ = std::move(wake);
  port_ = bound;
  *error = 0;
  return true;
}

bool MetricsServer::start(int* error) noexcept {
  if (port_ < 0) {
    // 没 start_listen 过就没有端口。不给自己发一张"起来了"的证明。
    *error = EINVAL;
    return false;
  }
  stop_requested_.store(false, std::memory_order_relaxed);
  running_.store(true, std::memory_order_relaxed);
  thread_ = std::thread([this] { thread_main(); });
  *error = 0;
  return true;
}

void MetricsServer::stop() noexcept {
  stop_requested_.store(true, std::memory_order_release);
  if (wake_fd_.valid()) {
    const std::uint64_t one = 1;
    const ssize_t ignored = ::write(wake_fd_.get(), &one, sizeof(one));
    (void)ignored;
  }
  if (thread_.joinable()) thread_.join();
  // 线程已停，下面收尾的是它没来得及关的东西（stop 时仍在册的抓取连接）。
  // 这时改 `conns_` 没有竞争：那条线程已经不在了。
  //
  // **监听 fd 与 eventfd 故意留到析构**（与网关那条"监听 fd 留到析构"的口径同一件事）：
  // `report_stats` 在 `stop()` 之后才采 `fd_end`，这里要是提前关掉它们，`fd_start`
  // （含这两个常驻 fd）与 `fd_end` 量的就不是同一组常驻项——实测形态是
  // `fd 12→10`，冒烟脚本 `smoke-s5-01.sh` 阶段 2 那条 fd 断言当场红（本轮就是这么抓到的）。
  for (std::size_t i = 0; i < kMaxConcurrentScrapes; ++i) {
    if (conns_[i].in_use) close_conn(i);
  }
}

void MetricsServer::thread_main() noexcept {
  while (!stop_requested_.load(std::memory_order_acquire)) {
    int error = 0;
    const int n = epoll_.wait(config_.wait_timeout_ms, &events_, &error);
    if (n < 0) break;  // epoll 本身坏了：与 worker 同一条纪律，继续跑后面的数都不可信。
    for (const l7gate::net::Epoll::Event& ev : events_) {
      if (ev.user_data == kListenerTag) {
        accept_all();
        continue;
      }
      if (ev.user_data == kWakeTag) {
        std::uint64_t drained = 0;
        const ssize_t ignored = ::read(wake_fd_.get(), &drained, sizeof(drained));
        (void)ignored;
        continue;
      }
      const std::size_t slot = tag_to_slot(ev.user_data);
      if (slot >= kMaxConcurrentScrapes || !conns_[slot].in_use) continue;  // 陈旧标签
      Conn& c = conns_[slot];

      if ((ev.events & (EPOLLHUP | EPOLLERR)) != 0) {
        // 对端在响应发完之前就断了：这算一次失败抓取，但不是错误——curl 手滑打错
        // 路径就是这个形状。计数进账，不打印，不打断任何别的东西。
        scrapes_failed_.fetch_add(1, std::memory_order_relaxed);
        close_conn(slot);
        continue;
      }

      if ((ev.events & EPOLLOUT) != 0 && !c.reply.empty()) {
        const ssize_t wrote = ::send(c.fd.get(), c.reply.data() + c.sent, c.reply.size() - c.sent,
                                     MSG_NOSIGNAL);
        if (wrote > 0) {
          c.sent += static_cast<std::size_t>(wrote);
        } else if (wrote < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
          continue;  // ET：发不出去就等下一个可写事件，不忙等
        } else {
          scrapes_failed_.fetch_add(1, std::memory_order_relaxed);
          close_conn(slot);
          continue;
        }
        if (c.sent >= c.reply.size()) {
          // 判"成功"的时点是**字节真的发完**，不是"我决定了要回 200"。
          if (c.failed) {
            scrapes_failed_.fetch_add(1, std::memory_order_relaxed);
          } else {
            scrapes_ok_.fetch_add(1, std::memory_order_relaxed);
            render_bytes_.fetch_add(c.body_bytes, std::memory_order_relaxed);
          }
          close_conn(slot);
        }
        continue;
      }

      if ((ev.events & EPOLLIN) != 0) {
        // ET：读到 EAGAIN、读到分隔符齐、或超过上限，三种之一才停手。
        for (;;) {
          char buf[1024];
          const ssize_t got = ::recv(c.fd.get(), buf, sizeof(buf), 0);
          if (got > 0) {
            c.request.append(buf, static_cast<std::size_t>(got));
            if (c.request.size() > kMaxRequestBytes) break;
            if (c.request.find("\r\n\r\n") != std::string::npos) break;
            continue;
          }
          if (got == 0) break;  // 对端关了：手上这点字节就是它能发来的全部
          if (errno == EINTR) continue;
          break;               // EAGAIN 或其它
        }
        const bool complete = c.request.find("\r\n\r\n") != std::string::npos;
        const bool oversized = c.request.size() > kMaxRequestBytes;
        if (!complete && !oversized) continue;
        const int status = handle_request(slot);
        c.failed = (status != 200);
      }
    }

    // 期限只在"醒来"这一拍检查（与 worker 的 deadline 表同一形状）：一条卡住的抓取
    // 连接最坏多活一个 `wait_timeout_ms`。不为此起 timer fd——那才是往被测方借资源。
    if (!stop_requested_.load(std::memory_order_relaxed)) {
      const std::int64_t now = steady_ms();
      for (std::size_t i = 0; i < kMaxConcurrentScrapes; ++i) {
        if (conns_[i].in_use && conns_[i].deadline_ms != 0 && now >= conns_[i].deadline_ms) {
          scrapes_failed_.fetch_add(1, std::memory_order_relaxed);
          close_conn(i);
        }
      }
    }
  }
  running_.store(false, std::memory_order_relaxed);
}

void MetricsServer::accept_all() noexcept {
  for (;;) {
    l7gate::net::AcceptOutcome accepted = l7gate::net::accept_connection(listener_.get());
    if (!accepted.ok()) {
      // ET：到"暂时没有了"或"accept 坏了"之一才停手（与 `Reactor::accept_all` 同形）。
      if (!accepted.would_block()) scrapes_failed_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    std::size_t free_slot = kMaxConcurrentScrapes;
    for (std::size_t i = 0; i < kMaxConcurrentScrapes; ++i) {
      if (!conns_[i].in_use) {
        free_slot = i;
        break;
      }
    }
    if (free_slot >= kMaxConcurrentScrapes) {
      // 抓取是低频事，同时压进 8 条已经是异常现场。直接关掉比排队等它更便宜，而且
      // 这条路径上一个字节都不碰 worker。`accepted` 出作用域 → UniqueFd 把 fd 关掉。
      scrapes_failed_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    Conn& c = conns_[free_slot];
    int error = 0;
    // 只读注册：可写注册留到 `handle_request` 把响应备好那一刻（一直接 EPOLLOUT 的话
    // 这条线程会在"还没东西可写"上被反复叫醒——白烧观察者自己的 CPU）。
    if (!epoll_.add(accepted.fd.get(), EPOLLIN, conn_tag(free_slot), &error)) {
      scrapes_failed_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    c.fd = std::move(accepted.fd);
    c.request.clear();
    c.reply.clear();
    c.sent = 0;
    c.failed = false;
    c.body_bytes = 0;
    c.deadline_ms = steady_ms() + config_.scrape_timeout_ms;
    c.in_use = true;
  }
}

void MetricsServer::close_conn(std::size_t slot) noexcept {
  Conn& c = conns_[slot];
  if (!c.in_use) return;
  int ignored = 0;
  epoll_.remove(c.fd.get(), &ignored);  // 注销失败不影响关 fd：留下的只是一个陈旧标签
  c.fd.close_now();
  c.request.clear();
  c.reply.clear();
  c.sent = 0;
  c.failed = false;
  c.body_bytes = 0;
  c.deadline_ms = 0;
  c.in_use = false;
}

int MetricsServer::handle_request(std::size_t slot) noexcept {
  Conn& c = conns_[slot];
  std::string_view method;
  std::string_view target;
  std::string body;
  int status = 200;

  if (c.request.size() > kMaxRequestBytes) {
    status = 431;
    c.reply = build_response(status, "Request Header Fields Too Large", "", false);
  } else if (!split_request_line(c.request, &method, &target)) {
    status = 400;
    c.reply = build_response(status, "Bad Request", "", false);
  } else if (method != "GET") {
    status = 405;
    c.reply = build_response(status, "Method Not Allowed", "", false);
  } else if (target != "/metrics") {
    // 只有这一条路径。不做 /healthz、不做索引页（ADR-24 第八节：不在本任务里）。
    status = 404;
    c.reply = build_response(status, "Not Found", "", false);
  } else {
    body = render_once(steady_ms());
    c.body_bytes = body.size();
    status = 200;
    c.reply = build_response(status, "OK", std::move(body), true);
  }

  int error = 0;
  epoll_.modify(c.fd.get(), EPOLLOUT, conn_tag(slot), &error);
  return status;
}

std::string MetricsServer::render_once(std::int64_t now_ms) {
  Sources sources = deps_.base;
  // 仪器自己的三格：只由这条线程写，渲染时读的正是自己刚写的值。
  sources.scrapes_ok = scrapes_ok_.load(std::memory_order_relaxed);
  sources.scrapes_failed = scrapes_failed_.load(std::memory_order_relaxed);
  sources.render_bytes = render_bytes_.load(std::memory_order_relaxed);

  RateLimitAccounts rate;
  if (deps_.rate_limiter != nullptr) {
    rate.present = true;
    rate.allowed = deps_.rate_limiter->allowed_total();
    rate.rejected = deps_.rate_limiter->rejected_total();
    rate.overflow_keys = deps_.rate_limiter->overflow_keys();
    // 唯一一次进桶表锁：`buckets_.size()` 是 O(1)。全表走查（O(kMaxKeys)，
    // `kMaxKeys = 16384`）不在这里做——那会把每条请求都要拿的同一把锁占到几十微秒，
    // 就是 ADR-24 第三节第 4 条要挡的那件事。
    rate.keys = deps_.rate_limiter->key_count();
    rate.capped_rejects = deps_.rate_limiter->capped_rejects_now();
  }
  sources.rate_limit = rate;

  RedisAccounts redis;
  if (deps_.redis_settler != nullptr && deps_.rate_limiter != nullptr) {
    // 与 `report_redis` 同一条 if：settler 与 limiter 缺一都不发这五键。
    redis.present = true;
    redis.settles_ok = deps_.redis_settler->settles_ok();
    redis.settles_failed = deps_.redis_settler->settles_failed();
    redis.settle_commands = deps_.redis_settler->settle_commands();
    redis.degraded = deps_.redis_settler->degraded();
  }
  sources.redis = redis;

  NodeAccounts nodes;
  if (deps_.nodes != nullptr) {
    nodes.present = true;
    nodes.reloads_ok = deps_.nodes->reloads_ok();
    nodes.reloads_failed = deps_.nodes->reloads_failed();
    nodes.last_publish_epoch_ms = deps_.nodes->last_publish_epoch_ms();
  }
  sources.nodes = nodes;

  ConfigAccounts config;
  if (deps_.config != nullptr) {
    config.present = true;
    config.generation = deps_.config->generation();
    config.checks = deps_.config->checks();
    config.publishes = deps_.config->publishes();
    config.rejects = deps_.config->rejects();
    config.rejects_syntax = deps_.config->rejects_syntax();
    config.rejects_scope = deps_.config->rejects_scope();
    config.snapshots_constructed = l7gate::route::ConfigSnapshot::constructed();
    config.snapshots_destructed = l7gate::route::ConfigSnapshot::destructed();
    config.snapshots_live = l7gate::route::ConfigSnapshot::live();
    config.last_publish_epoch_ms = deps_.config->last_publish_epoch_ms();
    config.last_publish_duration_ms = deps_.config->last_publish_duration_ms();
  }
  sources.config = config;

  ProcSample proc;
  proc.fd_open_now = l7gate::net::count_open_fds();
  proc.rss_kib_now = l7gate::net::current_rss_kib();
  return render(sources, proc, now_ms);
}

}  // namespace l7gate::metrics
