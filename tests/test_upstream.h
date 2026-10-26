#pragma once

#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <poll.h>
#include <sched.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "net/fd.h"
#include "net/socket.h"

namespace l7gate::testing {

// 仓库内的极简测试上游。**只服务测试**，不是网关的一部分，不参与任何 SLO 测量。
//
// 它存在的理由记在 TASKS.md 的 S1-04 行：验收要真实的上游对端，本机没装 nginx
// （那是 S3 基线的锁定项，提前装只会让 S1-04 的上游行为跟着一个还没到决策点的
// 版本走），而 `python3 -m http.server` 不可控、也进不了 sanitizer 的覆盖范围。
//
// 它 dumb 是有意的：固定模板响应、不做 HTTP 语义校验、不解析 chunked。
// 上游唯一需要的能力是"按请求回一段已知字节"，外加两个造故障的旋钮（延迟、
// 回完就断）。上游一旦开始"聪明"，网关的失败就分不清是我坏还是它坏——
// 那正是测试对端最不该有的性质。
//
// 单线程 + poll，所以 `set_delay_before_response_ms` 会同时拖住它的所有客户端。
// 对"上游比下游慢"这个场景（S1-07）来说，这恰好是要的行为。
class TestUpstream {
 public:
  static constexpr std::size_t kMaxClients = 32;

  TestUpstream() = default;
  ~TestUpstream() { stop_and_join(); }

  TestUpstream(const TestUpstream&) = delete;
  TestUpstream& operator=(const TestUpstream&) = delete;

  // 绑到指定地址/端口并起接受循环线程。requested_port 给 0 就是让内核选——单元测试全靠它
  // 避免端口撞车，冒烟脚本也用它：脚本从打印出来的真实端口继续，比我挑一个
  // "看起来没人用"的端口可靠得多。
  bool start(std::string* failure, std::string_view address = "127.0.0.1",
             std::uint16_t requested_port = 0) {
    net::SocketResult listener = net::make_listen_socket(address, requested_port, 128);
    if (!listener.ok()) {
      *failure = "make_listen_socket failed: errno=" + std::to_string(listener.error);
      return false;
    }
    const int bound_port = net::get_socket_port(listener.fd.get());
    if (bound_port < 0) {
      *failure = "get_socket_port failed";
      return false;
    }
    listener_ = std::move(listener.fd);
    port_ = static_cast<std::uint16_t>(bound_port);
    stop_.store(false, std::memory_order_relaxed);
    worker_ = std::thread([this] { loop(); });
    return true;
  }

  void stop_and_join() {
    stop_.store(true, std::memory_order_relaxed);
    if (worker_.joinable()) {
      worker_.join();  // loop 的 poll 超时是 50ms，join 最坏等这么久
    }
    listener_.close_now();
  }

  std::uint16_t port() const noexcept { return port_; }
  std::uint64_t requests_served() const noexcept {
    return requests_served_.load(std::memory_order_relaxed);
  }

  // 三个测试侧旋钮。语义都限定在"对端行为"，不触碰网关内部状态。
  // 响应体的**长度**由 response_body_bytes_ 决定，内容形如 "seq=42....."，
  // 带序号是为了让客户端能断言响应没有乱序或串号。
  void set_response_body_bytes(std::size_t n) { response_body_bytes_ = n; }
  void set_delay_before_response_ms(int ms) { delay_ms_ = ms; }
  void set_close_after_first_response(bool on) noexcept { close_after_response_ = on; }

 private:
  struct Client {
    net::UniqueFd fd;
    std::string pending;  // 还没凑成一个完整请求的字节
    bool served_any = false;
  };

  void loop() {
    std::vector<Client> clients;
    while (!stop_.load(std::memory_order_relaxed)) {
      pollfd pfds[kMaxClients + 1];
      pfds[0] = pollfd{listener_.get(), POLLIN, 0};
      // 先固定这一批要轮询的客户端数量：accept 会往 clients 里追加，
      // 而 pfds 的下标是按追加前的数量建的。混用会导致越界读 revents，
      // 也会让新连接在它的第一个事件到来前被当成"上一个连接"处理。
      const std::size_t client_count = clients.size();
      for (std::size_t i = 0; i < client_count; ++i) {
        pfds[i + 1] = pollfd{clients[i].fd.get(), POLLIN, 0};
      }
      const nfds_t count = static_cast<nfds_t>(client_count + 1);

      const int ready = ::poll(pfds, count, 50 /* ms */);
      if (ready < 0) {
        if (errno == EINTR) continue;
        break;  // 监听侧坏了，继续循环没有意义
      }

      // 逆序遍历，erase 才不会让后面元素的下标错位。
      for (std::size_t i = client_count; i > 0; --i) {
        const short revents = pfds[i].revents;
        if (revents == 0) continue;
        Client& client = clients[i - 1];
        const bool alive = read_into(client, revents);
        if (alive) serve_complete_requests(client);
        const bool should_drop =
            !alive || (close_after_response_ && client.served_any);
        if (should_drop) {
          client.fd.close_now();
          clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(i - 1));
        }
      }

      if ((pfds[0].revents & POLLIN) != 0) {
        accept_more(clients);
      }
    }
    for (auto& client : clients) client.fd.close_now();
  }

  void accept_more(std::vector<Client>& clients) {
    while (clients.size() < kMaxClients) {
      net::AcceptOutcome accepted = net::accept_connection(listener_.get());
      if (!accepted.ok()) return;  // EAGAIN 或真错误，两种都收工
      Client client;
      client.fd = std::move(accepted.fd);
      clients.push_back(std::move(client));
    }
  }

  // 返回 false 表示这个 client 该被摘掉（对端关闭或读错）。
  bool read_into(Client& client, short revents) {
    if ((revents & (POLLERR | POLLNVAL)) != 0) return false;
    char buf[4096];
    while (true) {
      const ssize_t n = ::read(client.fd.get(), buf, sizeof(buf));
      if (n > 0) {
        client.pending.append(buf, static_cast<std::size_t>(n));
        continue;  // 非阻塞：读到 EAGAIN 为止，和网关侧的 ET 契约同一条形状
      }
      if (n == 0) return false;
      const int e = errno;
      if (e == EINTR) continue;
      if (e == EAGAIN || e == EWOULDBLOCK) return (revents & POLLHUP) == 0;
      return false;
    }
  }

  void serve_complete_requests(Client& client) {
    while (true) {
      const std::size_t header_end = client.pending.find("\r\n\r\n");
      if (header_end == std::string::npos) return;
      const std::size_t block_end = header_end + 4;

      std::size_t body_len = 0;
      bool has_cl = false;
      if (!content_length_of(client.pending.substr(0, block_end), &body_len, &has_cl)) {
        // 读不懂声明的长度就回 400：测试对端可以 dumb，但不能装看不见。
        respond(client, "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n", block_end);
        return;
      }
      (void)has_cl;
      if (client.pending.size() < block_end + body_len) return;  // body 还没到齐
      respond(client, build_response(), block_end + body_len);
    }
  }

  static bool content_length_of(const std::string& headers, std::size_t* out, bool* found) {
    // 只认单值、大小写不敏感的 Content-Length —— 覆盖测试会发出的形态就够了。
    // 真实请求的合法性判定是网关解析器的职责，在这里重抄一份只会让两处漂移。
    const std::string needle = "content-length:";
    std::string lower = headers;
    for (auto& c : lower) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    const std::size_t pos = lower.find(needle);
    if (pos == std::string::npos) {
      *out = 0;
      *found = false;
      return true;
    }
    *found = true;
    std::size_t i = pos + needle.size();
    while (i < lower.size() && (lower[i] == ' ' || lower[i] == '\t')) ++i;
    std::size_t value = 0;
    bool any_digit = false;
    while (i < lower.size() && lower[i] >= '0' && lower[i] <= '9') {
      value = value * 10u + static_cast<std::size_t>(lower[i] - '0');
      ++i;
      any_digit = true;
    }
    if (!any_digit) return false;
    *out = value;
    return true;
  }

  std::string build_response() {
    const std::uint64_t seq = requests_served_.fetch_add(1, std::memory_order_relaxed) + 1;
    std::string body = "seq=" + std::to_string(seq);
    while (body.size() < response_body_bytes_) body += '.';
    body.resize(response_body_bytes_);
    if (delay_ms_ > 0) {
      ::usleep(static_cast<useconds_t>(delay_ms_) * 1000u);
    }
    return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\nConnection: keep-alive\r\n\r\n" + body;
  }

  void respond(Client& client, const std::string& payload, std::size_t consumed) {
    const std::size_t total = payload.size();
    std::size_t off = 0;
    // 有上限的重试。没有上限的话，一个不读响应的客户端会让这个线程永远卡在
    // 自旋里，测试的失败表现就从"断言不通过"变成"TIMEOUT 挂 30 秒"——
    // 后者更难查，而且会连带拖住同一个上游线程上的其它客户端。
    int spins = 0;
    constexpr int kMaxSpins = 100000;
    while (off < total && spins < kMaxSpins) {
      const ssize_t n = ::send(client.fd.get(), payload.data() + off, total - off, MSG_NOSIGNAL);
      if (n <= 0) {
        const int e = errno;
        if (e == EINTR) continue;
        if (e == EAGAIN || e == EWOULDBLOCK) {
          ++spins;
          ::sched_yield();
          continue;
        }
        break;  // EPIPE 之类：对端已经不读了
      }
      off += static_cast<std::size_t>(n);
      spins = 0;
    }
    client.pending.erase(0, consumed);
    client.served_any = true;
  }

  net::UniqueFd listener_;
  std::thread worker_;
  std::atomic<bool> stop_{false};
  std::atomic<std::uint64_t> requests_served_{0};
  std::uint16_t port_ = 0;
  std::size_t response_body_bytes_ = 64;
  int delay_ms_ = 0;
  bool close_after_response_ = false;
};

}  // namespace l7gate::testing
