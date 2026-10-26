#pragma once

#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
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
// 回完就断），以及 S1-05 的两条握手配合：请求头里出现 `Upgrade:` 就回完整 101
// 并转入裸回显（它自己同样不解析帧——对端越"笨"，网关的透传越无处藏），
// 请求带 `Connection: close` 就按语义在这条响应之后收尾。
// 上游一旦开始"聪明"，网关的失败就分不清是我坏还是它坏——
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
  // S1-06 的逐字节一致验收需要"同一个字节序列被取两次"：一次直连、一次经网关。
  // seq 编号会让两次取到不同的体，所以这里把体钉成固定串——钉死之后"一致"
  // 才是可比命题。默认不开，S1-04/05 的用例继续用带 seq 的体（它们要的就是编号）。
  void set_fixed_response_body(std::string body) { fixed_body_ = std::move(body); }
  // 只发前 n 字节就断（n=0 → accept 后一个字节的响应都不发就关）。
  // 请求路径带 /dies，或全局开关打开时对**所有**请求生效。它是"坏上游"的
  // 可控版本：截断的位置由测试定，断的行为由内核定。
  void set_die_after_response_bytes(std::size_t n) noexcept { die_after_bytes_ = n; }

 private:
  struct Client {
    net::UniqueFd fd;
    std::string pending;  // 还没凑成一个完整请求的字节
    bool served_any = false;
    // S1-05：请求带 `Connection: close` 时，上游按语义在这条响应之后收尾。
    // 网关侧的 close 兑现走"配对一次性"路径，不依赖对端配合；这里配合一下，
    // 让"上游先关"与"网关按 close 收尾"两种时序都能被同一套用例打到。
    bool closed_by_close_header = false;
    // S1-05：回过 101 之后这个连接就是隧道——**不再按 HTTP 读**，收到的字节
    // 原样回显（一次 read 回一次，正好和"网关每事件搬一块"对齐）。测试对端
    // 在这里同样"不解析帧"：它证明的只是"另一端原样收到"，语义都在网关侧断言。
    bool tunneled = false;
    // S1-06：这一条由上游**主动断掉**（坏上游的两种形态：不回话 / 回半条）。
    // 断完之后本轮就把 client 摘掉，不让它带着 -1 的 fd 再进一次 poll 表。
    bool hang_up = false;
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
        if (client.tunneled) {
          // 隧道：不碰语义，pending 里是什么就回显什么（包括"看起来像畸形 HTTP"
          // 的帧字节）。回显掉即清空——这里没有"请求"可消费。
          if (alive && !client.pending.empty()) {
            echo_all(client, client.pending);
            client.pending.clear();
          }
        } else if (alive) {
          serve_complete_requests(client);
        }
        const bool should_drop =
            !alive || client.hang_up ||
            (close_after_response_ && client.served_any) ||
            (client.closed_by_close_header && client.served_any);
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

      const std::string headers = client.pending.substr(0, block_end);
      std::string lower = headers;
      for (auto& c : lower) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));

      // S1-05 的 upgrade 分支：请求头部里出现 `Upgrade:` 就回完整的 101 响应头，
      // 这个连接从此进隧道。判定刻意比网关侧宽松（不看 Connection 记号）——
      // "宽松的上游 + 严格的网关"组合起来，才能把"网关只认双条件"这件事测出来：
      // 只带 Upgrade 头的请求若被网关当握手，这里的 101 会原样漏给客户端并被
      // 隧道计数断言抓住（见 S1_05_Reactor 的 negative 用例）。
      if (lower.find("upgrade:") != std::string::npos) {
        respond(client, build_101(), block_end);
        client.tunneled = true;
        return;
      }

      std::size_t body_len = 0;
      bool has_cl = false;
      if (!content_length_of(headers, &body_len, &has_cl)) {
        // 读不懂声明的长度就回 400：测试对端可以 dumb，但不能装看不见。
        respond(client, "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n", block_end);
        return;
      }
      (void)has_cl;
      if (client.pending.size() < block_end + body_len) return;  // body 还没到齐
      const bool want_close = lower.find("connection: close") != std::string::npos;
      if (want_close) client.closed_by_close_header = true;

      // S1-06 的故障与框架模式。判定口径只看请求 target 里的路径前缀——
      // 上游不需要 HTTP 语义，只需要"看到不同路径回不同字节"，聪明留给网关。
      const std::string_view target = first_line_target(headers);
      // **顺序就是正确性**：/dies 是 /dies-mid-body 的前缀，先匹配短的那条，
      // 长的那条永远轮不到——我第一次跑冒烟时"回半条 body"这一步实际打的是
      // "一个字节都不回"的分支，而两边的断言恰好都能被"502 + 计数"糊过去。
      // 前缀匹配表要么按长度降序排，要么改成全等比较；这里选降序并把它写下来。
      if (target.rfind("/dies-mid-body", 0) == 0) {
        // 头是完整的、body 只回一半就关——框架意义上的"截断在 body 中途"。
        // 与 /dies 的区别就是这一刀落在最终头之后：网关此时已经剥夺了自产
        // 5xx 的资格，只能以连接终止表态。用例要靠这两条分别打到两个分支。
        const std::string payload = build_response(want_close);
        const std::size_t head_end = payload.find("\r\n\r\n") + 4;
        const std::size_t cut = head_end + (payload.size() - head_end) / 2;
        echo_all(client, payload.substr(0, cut));
        client.pending.erase(0, block_end + body_len);
        client.hang_up = true;
        return;
      }
      if (target.rfind("/dies", 0) == 0) {
        // 断在声明之外：一个字节响应都不发就关（截断在头之前）。
        client.hang_up = true;
        return;
      }
      if (target.rfind("/chunked", 0) == 0) {
        respond(client, build_chunked(), block_end + body_len);
        if (client.closed_by_close_header) return;
        continue;
      }
      if (target.rfind("/bad-chunk", 0) == 0) {
        // 合法的 chunked 头 + 非法的框架字节（非 hex 的 size 行）。
        respond(client, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                        "ZZ\r\n",
                block_end + body_len);
        if (client.closed_by_close_header) return;
        continue;
      }
      if (target.rfind("/endless-head", 0) == 0) {
        // 一个**永远不会齐**的响应头：超过网关的响应头预算仍没有终止串，而且
        // 不关连接。这是"慢上游"和"坏上游"的分界线——半包本身不是错误，
        // 只有当它大到不可能是合法头时才允许判死（否则正常的慢上游会被 502 错杀）。
        std::string huge = "HTTP/1.1 200 OK\r\nX-Pad: ";
        huge.append(70u * 1024u, 'p');  // 70 KiB > 默认 64 KiB 的响应头预算
        echo_all(client, huge);
        client.pending.erase(0, block_end + body_len);  // 请求要消费掉，否则这个循环永不退出
        requests_served_.fetch_add(1, std::memory_order_relaxed);
        return;  // 一条请求换来一段永不完结的头，本轮到此为止
      }

      std::string payload = build_response(want_close);
      if (die_after_bytes_ < payload.size()) {
        // 全局断链模式：只发前 die_after_bytes_ 字节就关（0 时一个字节的响应都不发）。
        // 截断的**位置**由测试定，断的**动作**由内核的 FIN 完成——这条分工让
        // "头之前断"和"body 中途断"两个分支能用同一个旋钮打出来。
        if (die_after_bytes_ > 0) echo_all(client, payload.substr(0, die_after_bytes_));
        client.hang_up = true;
        return;
      }
      respond(client, payload, block_end + body_len);
      if (client.closed_by_close_header) return;  // 收尾由外层 should_drop 兑现
    }
  }

  std::string build_101() {
    // 握手计入"已服务"（脚本侧的上游自报数要保持同一条口径），但 101 没有
    // seq 体——隧道里没有响应体这回事。
    requests_served_.fetch_add(1, std::memory_order_relaxed);
    return "HTTP/1.1 101 Switching Protocols\r\n"
           "Upgrade: websocket\r\n"
           "Connection: Upgrade\r\n"
           "Sec-WebSocket-Accept: dGhpcyBpcyBub3QgcmVhbCBjcnlwdG9ncmFwaHk=\r\n"
           "\r\n";
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

  std::string build_response(bool close_after) {
    // 计数在两种模式下都要落："服务过几条"是测试侧的独立证据，不该因为
    // 体被钉成固定串就消失（那样经网关/直连比对的两条请求就不在任何账上了）。
    const std::uint64_t seq = requests_served_.fetch_add(1, std::memory_order_relaxed) + 1;
    std::string body;
    if (!fixed_body_.empty()) {
      body = fixed_body_;
    } else {
      body = "seq=" + std::to_string(seq);
      while (body.size() < response_body_bytes_) body += '.';
      body.resize(response_body_bytes_);
    }
    if (delay_ms_ > 0) {
      ::usleep(static_cast<useconds_t>(delay_ms_) * 1000u);
    }
    return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\nConnection: " + (close_after ? "close" : "keep-alive") + "\r\n\r\n" + body;
  }

  // S1-06 的 chunked 模板：两段 + last-chunk + 空行。体由 response_body_bytes_
  // 决定并**按声明长度切分**，这样"chunk 之和 == 体长"这件事不需要用例再核对一遍。
  std::string build_chunked() {
    const std::uint64_t seq = requests_served_.fetch_add(1, std::memory_order_relaxed) + 1;
    std::string body;
    if (!fixed_body_.empty()) {
      body = fixed_body_;
    } else {
      body = "seq=" + std::to_string(seq);
      while (body.size() < response_body_bytes_) body += '.';
      body.resize(response_body_bytes_);
    }
    const std::size_t half = body.size() / 2;
    char size_buf[32];
    std::string out = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
                      "Connection: keep-alive\r\n\r\n";
    std::snprintf(size_buf, sizeof(size_buf), "%zX\r\n", half);
    out += size_buf;
    out += body.substr(0, half);
    out += "\r\n";
    std::snprintf(size_buf, sizeof(size_buf), "%zX\r\n", body.size() - half);
    out += size_buf;
    out += body.substr(half);
    out += "\r\n";
    out += "0\r\n\r\n";
    return out;
  }

  // 请求行的第二格就是 target。上游不做解析，只认前缀。
  static std::string_view first_line_target(const std::string& headers) {
    const std::size_t line_end = headers.find("\r\n");
    const std::string_view line(headers.data(),
                                line_end == std::string::npos ? headers.size() : line_end);
    const std::size_t sp1 = line.find(' ');
    if (sp1 == std::string_view::npos) return {};
    const std::size_t sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos) return {};
    return line.substr(sp1 + 1, sp2 - sp1 - 1);
  }

  // 隧道回显：和 respond 同一套"有上限自旋"的写法——一个不再读的对端不能把这个
  // 线程永远卡住，否则测试的表现从"断言不通过"退化成"TIMEOUT 挂 30 秒"。
  void echo_all(Client& client, const std::string& data) {
    std::size_t off = 0;
    int spins = 0;
    constexpr int kMaxSpins = 100000;
    while (off < data.size() && spins < kMaxSpins) {
      const ssize_t n = ::send(client.fd.get(), data.data() + off, data.size() - off,
                               MSG_NOSIGNAL);
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
  std::string fixed_body_;
  std::size_t die_after_bytes_ = std::numeric_limits<std::size_t>::max();
};

}  // namespace l7gate::testing
