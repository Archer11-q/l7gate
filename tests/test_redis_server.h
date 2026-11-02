#pragma once

// S4-04 的 RESP 服务端夹具（不是产品的一部分，只被 ctest 用）。
//
// 为什么要有它而不是直接连容器：这套件的判据是**结算线程的状态机**——"有需求才发命令"、
// "失败就兜底并把夹子放开"、"恢复后重新夹上"、"半途失败的那笔消耗下一拍重报"、
// "NOSCRIPT 会重 LOAD 再问一次"。这些都不需要真的 Redis，需要的是"一个会按我要求回答、
// 也会按我要求耍赖的 RESP 对端"。写成夹具之后，同一批断言能在 asan/tsan 两个 preset 下跑
// ——连着 Docker 就永远做不到这一点。
//
// 真 Redis 的语义由 `tools/l7gate_redis_probe` 与 `scripts/smoke-s4-04.sh` 负责：
// **这个夹具不证明我的 Lua 在 Redis 上算得对**，那条是探针对着 7.2.5 逐格钉的
// （ADR-05 追加节第八节第一句）。两边各管一段，不许互相顶替。
//
// 生命周期沿用 `test_upstream.h` 的形状：stop 先置标志、join（poll 超时 50ms）、**再**关
// fd——先关 fd 会让那个编号被别的线程复用，接受循环就可能接到一条不属于它的连接。
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace l7gate::test {

class TestRedisServer {
 public:
  // 一条被看到的命令：小写命令名 + 其余参数（EVALSHA 的参数第 6 个就是我上报的 used）。
  struct Seen {
    std::string cmd;
    std::vector<std::string> args;
  };

  TestRedisServer() = default;
  TestRedisServer(const TestRedisServer&) = delete;
  TestRedisServer& operator=(const TestRedisServer&) = delete;
  ~TestRedisServer() { stop(); }

  // port=0 → 让内核选，随后 port() 读回。失败时 *why 带 errno 说明。
  bool start(std::uint16_t port, std::string* why) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
      *why = std::strerror(errno);
      return false;
    }
    int on = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      *why = std::strerror(errno);
      ::close(listen_fd_);
      listen_fd_ = -1;
      return false;
    }
    socklen_t len = sizeof(addr);
    ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    if (::listen(listen_fd_, 8) != 0) {
      *why = std::strerror(errno);
      ::close(listen_fd_);
      listen_fd_ = -1;
      return false;
    }
    running_.store(true, std::memory_order_relaxed);
    worker_ = std::thread([this] { loop(); });
    return true;
  }

  void stop() {
    running_.store(false, std::memory_order_relaxed);
    if (worker_.joinable()) worker_.join();
    if (listen_fd_ >= 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
    }
  }

  std::uint16_t port() const { return port_; }

  // 之后的 EVALSHA 都回这个数（毫 token）。默认给一个足够大的值 = 不夹。
  void set_grant(std::int64_t tokens_milli) {
    std::lock_guard<std::mutex> lock(mu_);
    grant_milli_ = tokens_milli;
  }
  // 持续故障开关：on 之后每条 EVALSHA 都直接把连接关掉（= Redis 挂了/网络断）。
  // 为什么是"持续"而不是"下一条"：降级态的断言要能在**稳态**下读——只耍赖一次的话，
  // 下一拍就恢复正常，`degraded` 与 cap 的读数落在我这次采样之前还是之后全看调度，
  // 那种用例绿了也不说明什么。（一次性故障仍然要注入，那是 NOSCRIPT 那条走法。）
  void set_refuse_evalsha(bool on) {
    std::lock_guard<std::mutex> lock(mu_);
    refuse_evalsha_ = on;
  }
  // 下一次 EVALSHA 回 `-ERR NOSCRIPT`（模拟服务端脚本没了），回过一次自动解除。
  void set_noscript() {
    std::lock_guard<std::mutex> lock(mu_);
    noscript_ = true;
  }

  std::vector<Seen> commands() {
    std::lock_guard<std::mutex> lock(mu_);
    return seen_;
  }
  std::size_t count_cmd(std::string_view cmd) {
    std::lock_guard<std::mutex> lock(mu_);
    std::size_t n = 0;
    for (const Seen& s : seen_) {
      if (s.cmd == cmd) ++n;
    }
    return n;
  }
  // 最后一条 EVALSHA 上报的 used。参数是 `EVALSHA <sha> <numkeys> <key> <rate> <burst> <now> <used>`
  // 去掉命令名之后共 7 个，used 是第 7 个（下标 6）——数错一格读到的是 now。
  std::string last_eval_used() {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto it = seen_.rbegin(); it != seen_.rend(); ++it) {
      if (it->cmd == "evalsha" && it->args.size() >= 7) return it->args[6];
    }
    return {};
  }

 private:
  void loop() {
    while (running_.load(std::memory_order_relaxed)) {
      pollfd pfd{listen_fd_, POLLIN, 0};
      const int ready = ::poll(&pfd, 1, 50);
      if (ready <= 0) continue;
      const int fd = ::accept(listen_fd_, nullptr, nullptr);
      if (fd < 0) continue;
      serve(fd);
      ::close(fd);
    }
  }

  static void write_all(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
      const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
      if (n <= 0) return;
      sent += static_cast<std::size_t>(n);
    }
  }

  bool readable(int fd) {
    pollfd pfd{fd, POLLIN, 0};
    while (::poll(&pfd, 1, 50) == 0) {
      if (!running_.load(std::memory_order_relaxed)) return false;
    }
    return true;
  }

  bool read_line(int fd, std::string* out) {
    out->clear();
    char c = 0;
    for (;;) {
      if (!readable(fd)) return false;
      const ssize_t n = ::recv(fd, &c, 1, 0);
      if (n != 1) return false;
      if (c == '\n') {
        if (!out->empty() && out->back() == '\r') out->pop_back();
        return true;
      }
      out->push_back(c);
    }
  }

  bool read_command(int fd, std::vector<std::string>* argv) {
    std::string line;
    if (!read_line(fd, &line)) return false;
    if (line.empty() || line[0] != '*') {
      // 内联命令（产品不会发，但夹具不该因为一个没预期的形态就卡死）。
      std::size_t begin = 0;
      while (begin < line.size()) {
        std::size_t end = line.find(' ', begin);
        if (end == std::string::npos) end = line.size();
        if (end > begin) argv->push_back(line.substr(begin, end - begin));
        begin = end + 1;
      }
      return !argv->empty();
    }
    const int count = std::atoi(line.c_str() + 1);
    for (int i = 0; i < count; ++i) {
      if (!read_line(fd, &line)) return false;
      if (line.empty() || line[0] != '$') return false;
      const int len = std::atoi(line.c_str() + 1);
      std::string blob;
      while (static_cast<int>(blob.size()) < len) {
        if (!readable(fd)) return false;
        char buf[256];
        const int want = len - static_cast<int>(blob.size());
        const ssize_t got =
            ::recv(fd, buf, want < 256 ? static_cast<std::size_t>(want) : 256, 0);
        if (got <= 0) return false;
        blob.append(buf, static_cast<std::size_t>(got));
      }
      char crlf[2];
      if (::recv(fd, crlf, 2, 0) != 2) return false;
      argv->push_back(blob);
    }
    return !argv->empty();
  }

  void serve(int fd) {
    for (;;) {
      std::vector<std::string> argv;
      if (!read_command(fd, &argv)) return;
      std::string cmd = argv[0];
      for (char& c : cmd) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
      // Redis 的命令名大小写不敏感，产品发的是 `SCRIPT LOAD`——子命令也得按小写比，
      // 不然夹具会把一条它"没预期到"的命令回成 -ERR，被测方看到的是服务端拒绝而不是我的解析错。
      std::string second = argv.size() >= 2 ? argv[1] : std::string{};
      for (char& c : second) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
      std::int64_t grant = 0;
      bool refuse = false;
      bool noscript = false;
      {
        std::lock_guard<std::mutex> lock(mu_);
        refuse = refuse_evalsha_ && cmd == "evalsha";
        // 只对 EVALSHA 生效：一拍里有 LOAD 与 PING，"谁先来谁消化"会让这个故障永远打不到
        // 它要测的那一格（实测就是这样把 NOSCRIPT 喂给了 PING）。
        noscript = noscript_ && cmd == "evalsha";
        if (noscript) noscript_ = false;  // 一次性，否则会永远回 NOSCRIPT
        grant = grant_milli_;
        seen_.push_back(Seen{cmd, std::vector<std::string>(argv.begin() + 1, argv.end())});
      }
      if (refuse) return;  // 连上报这条路都走不通 = 对端不在了
      if (cmd == "ping") {
        write_all(fd, "+PONG\r\n");
      } else if (cmd == "script" && second == "load") {
        write_all(fd, "$40\r\n0000000000000000000000000000000000000001\r\n");
      } else if (cmd == "evalsha") {
        if (noscript) {
          write_all(fd, "-NOSCRIPT No matching script. Please use EVAL.\r\n");
        } else {
          write_all(fd, ":" + std::to_string(grant) + "\r\n");
        }
      } else {
        write_all(fd, "-ERR unknown command\r\n");
      }
    }
  }

  int listen_fd_ = -1;
  std::uint16_t port_ = 0;
  std::atomic<bool> running_{false};
  std::thread worker_;
  std::mutex mu_;
  std::int64_t grant_milli_ = 1000000;  // 默认"很宽"：不夹，等价 S4-03 的本地桶
  bool refuse_evalsha_ = false;
  bool noscript_ = false;
  std::vector<Seen> seen_;
};

}  // namespace l7gate::test
