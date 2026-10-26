// SLO #1（转发尾延迟）的压测上游。**只服务压测**，不是网关的一部分，也不进 ctest。
//
// 为什么不能拿 tests/test_upstream.h 那个来压测：它是 poll 单线程、kMaxClients=32，
// 而 SLO 的测量条件写死了「1000 并发连接」。32 的上限会让第 33 条连接直接被摘掉，
// 压测跑出来的是"上游在拒绝连接"的数字，不是任何延迟数字。改那个类去支持 1000 条
// 也不对——它的价值恰恰是"dumb 且可控"（能回半条、能不回话、能切隧道），
// 那些分支在压测热路径上全是噪声。
//
// 所以这里只有一个能力：**按请求回一段固定字节**。它越薄，"网关引入的增量"里
// 混进去的上游开销就越小、也越好解释。注意口径：直连与经网关两组打的是**同一个**
// 上游，两边都含它的开销，所以差值归因得到网关头上——但"含多少"我得能说清，
// 于是这个文件里没有解析器、没有按需分配、没有第二个线程。
//
// 与网关同一条 ET 契约：读到一个 EAGAIN 才算这次可读事件处理完；写撞 EAGAIN 才挂
// EPOLLOUT，刷完立刻摘掉。摘不干净的话 1000 条空闲连接会让这个进程自旋，
// 而我把"上游 CPU 可忽略"当成压测结论的前提用着——前提必须由代码保证，不能由希望。
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "net/epoll.h"
#include "net/fd.h"
#include "net/socket.h"

// tests/ 里的对端住在 l7gate::testing 下，`net::` 顺着外层 l7gate 就能解析；
// 这个文件住在匿名命名空间里，没有那层外壳，所以显式起一个别名而不是满屏 l7gate::。
namespace net = l7gate::net;

namespace {

std::atomic<bool> g_stop_requested{false};
void on_signal(int) { g_stop_requested.store(true, std::memory_order_relaxed); }

constexpr int kPollTimeoutMs = 50;
// 请求终止串的滚动匹配：模式是 CRLF CRLF，只有它自己 + 前缀 "CR" 两种 border，
// 所以一个 4 状态（0..3）的小自动机就够，不需要真的 KMP 表。跨 read 边界连续，
// 因为状态存在连接上而不是存在这一批字节里。
// （这里原先写了一个 `constexpr kMatchStates = 4` 想给状态数留个名字：clang 在
// sanitizer preset 里报 `-Wunused-const-variable`——一个没人引用的常量是一句
// 关于结构的谎，删掉它比给它找个假用途诚实。状态数由 switch 的 0..3 直接读出。）

struct Client {
  net::UniqueFd fd;
  // 排了几条还没写完的响应，以及当前这条写到哪。wrk/hey 是"发一条读一条"，
  // 稳态下这两个数是 0/1，但代码不能假设它——假设了，pipeline 场景下
  // 这个数字就是错的，而我看不出来。
  std::size_t queued_responses = 0;
  std::size_t offset_in_response = 0;
  std::uint8_t match_state = 0;
  bool write_armed = false;
  // 一批事件里同一个 fd 可能出现两次（EPOLLIN|EPOLLOUT 合成一条，也可能被拆成
  // 两条）。中途 erase 会让后面那条拿着已析构的 Client* ——这是真 UAF，
  // 不是理论问题。所以这里只置 dead，退出事件批之后再统一 erase。
  bool dead = false;

  std::size_t count_requests(const char* data, std::size_t len) {
    std::size_t found = 0;
    for (std::size_t i = 0; i < len; ++i) {
      const char c = data[i];
      switch (match_state) {
        case 0: match_state = (c == '\r') ? 1 : 0; break;
        case 1: match_state = (c == '\n') ? 2 : (c == '\r' ? 1 : 0); break;
        case 2: match_state = (c == '\r') ? 3 : 0; break;
        default:  // 3：已经吃掉 CRLF CR
          if (c == '\n') {
            ++found;
            match_state = 0;
          } else {
            match_state = (c == '\r') ? 1 : 0;
          }
          break;
      }
    }
    return found;
  }
};

struct Options {
  std::string address = "127.0.0.1";
  std::uint16_t port = 0;  // 0 = 内核选；脚本读打印出来的实际端口，不自己挑
  std::size_t body_bytes = 1024;
  int max_clients = 4096;
  int backlog = 4096;
};

bool parse_addr_port(std::string_view spec, std::string* addr, std::uint16_t* port) {
  const std::size_t sep = spec.rfind(':');
  if (sep == std::string_view::npos) return false;
  *addr = std::string(spec.substr(0, sep));
  const std::string digits(spec.substr(sep + 1));
  errno = 0;
  char* end = nullptr;
  const long value = std::strtol(digits.c_str(), &end, 10);
  if (errno != 0 || end == digits.c_str() || *end != '\0' || value < 0 || value > 65535) {
    return false;
  }
  *port = static_cast<std::uint16_t>(value);
  return !addr->empty();
}

bool parse_bounded(const char* text, long min, long max, long* out) {
  errno = 0;
  char* end = nullptr;
  const long value = std::strtol(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || value < min || value > max) return false;
  *out = value;
  return true;
}

class BenchUpstream {
 public:
  explicit BenchUpstream(Options options) : options_(std::move(options)) {}

  bool start(std::string* why) {
    net::SocketResult listener =
        net::make_listen_socket(options_.address, options_.port, options_.backlog);
    if (!listener.ok()) {
      *why = "make_listen_socket errno=" + std::to_string(listener.error);
      return false;
    }
    const int bound = net::get_socket_port(listener.fd.get());
    if (bound < 0) {
      *why = "get_socket_port failed";
      return false;
    }
    port_ = static_cast<std::uint16_t>(bound);
    listener_ = std::move(listener.fd);

    // 响应体是一段重复的 'x'：压测不需要内容有意义，需要的是**每次一模一样**，
    // 否则"直连 vs 经网关"两组在比两个不同的东西。
    response_ = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(options_.body_bytes) +
                "\r\nConnection: keep-alive\r\n\r\n";
    response_.append(options_.body_bytes, 'x');

    int error = 0;
    if (!epoll_.add(listener_.get(), EPOLLIN | EPOLLET, &listener_tag_, &error)) {
      *why = "epoll add listener errno=" + std::to_string(error);
      return false;
    }
    return true;
  }

  int port() const noexcept { return port_; }

  void run() {
    std::vector<net::Epoll::Event> events;
    // Epoll 的 error 出参**每次调用都写**（wait 成功时也写 0），所以它不可为 nullptr。
    // 我第一版在这里传了 nullptr，进程在第一次 epoll_wait 就段错误——那不是网关的 bug，
    // 是我把一个必填出参当成了可选参数。同一条纪律也写在 epoll.h 的注释里。
    int wait_error = 0;
    while (!g_stop_requested.load(std::memory_order_relaxed)) {
      const int ready = epoll_.wait(kPollTimeoutMs, &events, &wait_error);
      if (ready < 0) {
        std::fprintf(stderr, "[bench-upstream] epoll_wait 失败 errno=%d，退出\n", wait_error);
        break;
      }
      for (int i = 0; i < ready; ++i) {
        const net::Epoll::Event& ev = events[static_cast<std::size_t>(i)];
        if (ev.user_data == &listener_tag_) {
          accept_more();
          continue;
        }
        auto* client = static_cast<Client*>(ev.user_data);
        if (client->dead) continue;  // 这批里已经关掉的那条：编号可能已被复用
        on_client_event(client, ev.events);
      }
      // 退出事件批才真正销毁对象。这一步和网关的 pending-destroy 是同一个道理，
      // 只是这里没有 shared_ptr，用一个 dead 标记 + 批后清理表达同一条不变量。
      for (const int fd : pending_erase_) clients_.erase(fd);
      pending_erase_.clear();
    }
    clients_.clear();  // 停止请求到达：剩下的连接随 map 一起关掉，不必逐个标记
  }

  void print_summary() const {
    std::printf("[bench-upstream] accepted=%llu refused_over_capacity=%llu refused_addfail=%llu "
                "peak_clients=%d max_clients=%d requests=%llu responses_written=%llu bytes_written=%llu\n",
                static_cast<unsigned long long>(accepted_),
                static_cast<unsigned long long>(refused_full_),
                static_cast<unsigned long long>(refused_addfail_),
                peak_clients_, options_.max_clients,
                static_cast<unsigned long long>(requests_),
                static_cast<unsigned long long>(responses_written_),
                static_cast<unsigned long long>(bytes_written_));
  }

 private:
  struct ListenerTag {};  // 监听 fd 的 user_data 哨兵：事件回来时分得清是谁。

  void accept_more() {
    while (true) {
      net::AcceptOutcome accepted = net::accept_connection(listener_.get());
      if (!accepted.ok()) return;  // EAGAIN 或真错误：两种都收工（ET 契约）
      if (static_cast<int>(clients_.size()) >= options_.max_clients) {
        // 满了：**接了再立刻关**，而不是"不接、让它在 backlog 里排队"。原注释说
        // 的是后者，代码做的是前者，而前者才是这里唯一诚实的行为——边缘触发下
        // "不 accept 就 return" 等于把监听 fd 的事件永远丢掉（没有新连接就没有新
        // 事件），压测上游会因为自己的事件模型而静默丢负载。接了关至少让客户端
        // 看见一条"连上就断"的失败，wrk 会把它记成 socket error，脚本据此废掉
        // 这一轮——工具的问题必须在报告里响，不能被工具的沉默盖住。
        ++refused_full_;
        return;
      }
      int error = 0;
      net::set_tcp_nodelay(accepted.fd.get(), &error);
      auto slot = std::make_unique<Client>();
      slot->fd = std::move(accepted.fd);
      Client* raw = slot.get();
      const int fd = raw->fd.get();
      clients_.emplace(fd, std::move(slot));
      if (static_cast<int>(clients_.size()) > peak_clients_) peak_clients_ = static_cast<int>(clients_.size());
      if (!epoll_.add(fd, EPOLLIN | EPOLLET, raw, &error)) {
        // 注册不上就关掉：留一个没人管事件的 fd，等于给这次压测埋一个泄漏源。
        ++refused_addfail_;
        clients_.erase(fd);
        continue;
      }
      ++accepted_;
    }
  }

  void on_client_event(Client* client, std::uint32_t events) {
    if ((events & EPOLLIN) != 0) {
      char buf[8192];
      while (true) {
        const ssize_t n = ::recv(client->fd.get(), buf, sizeof(buf), MSG_DONTWAIT);
        if (n > 0) {
          const std::size_t got = static_cast<std::size_t>(n);
          const std::size_t found = client->count_requests(buf, got);
          client->queued_responses += found;
          requests_ += found;
          continue;  // ET：读到 EAGAIN 才算这次可读事件处理完
        }
        if (n == 0) {
          close_client(client);
          return;
        }
        const int e = errno;
        if (e == EINTR) continue;
        if (e == EAGAIN || e == EWOULDBLOCK) break;
        close_client(client);
        return;
      }
    }
    if ((events & EPOLLOUT) != 0 || client->queued_responses > 0) {
      flush(client);
    }
  }

  void flush(Client* client) {
    if (client->dead) return;
    const int fd = client->fd.get();
    while (client->queued_responses > 0) {
      const std::size_t remain = response_.size() - client->offset_in_response;
      const ssize_t n = ::send(fd, response_.data() + client->offset_in_response, remain,
                               MSG_NOSIGNAL | MSG_DONTWAIT);
      if (n > 0) {
        const std::size_t wrote = static_cast<std::size_t>(n);
        client->offset_in_response += wrote;
        bytes_written_ += wrote;
        if (client->offset_in_response == response_.size()) {
          client->offset_in_response = 0;
          --client->queued_responses;
          ++responses_written_;
        }
        continue;
      }
      const int e = errno;
      if (e == EINTR) continue;
      if (e == EAGAIN || e == EWOULDBLOCK) {
        if (!client->write_armed) {
          int error = 0;
          epoll_.modify(fd, EPOLLIN | EPOLLOUT | EPOLLET, client, &error);
          client->write_armed = true;
        }
        return;  // 下一轮可写事件接着刷
      }
      close_client(client);
      return;
    }
    if (client->write_armed) {
      // 刷完就摘 EPOLLOUT。这条和网关侧是同一条纪律：可写在水平意义下永远就绪，
      // 留着它这个进程就在自旋，而 1000 条连接会把它自旋成一台 CPU 加热器。
      int error = 0;
      epoll_.modify(fd, EPOLLIN | EPOLLET, client, &error);
      client->write_armed = false;
    }
  }

  // 只标记 + 摘 epoll；对象由 run() 在事件批处理完之后统一销毁。
  void close_client(Client* client) {
    if (client->dead) return;
    const int fd = client->fd.get();
    int error = 0;
    epoll_.remove(fd, &error);
    client->dead = true;
    pending_erase_.push_back(fd);
  }

  Options options_;
  std::string response_;
  net::UniqueFd listener_;
  net::Epoll epoll_;
  std::unordered_map<int, std::unique_ptr<Client>> clients_;
  std::vector<int> pending_erase_;
  ListenerTag listener_tag_;
  std::uint16_t port_ = 0;
  std::uint64_t accepted_ = 0;
  // 两个"没接住"必须分开：一个是**容量**（说明这次压测的并发超过了工具的上限，
  // 数字要作废重跑），一个是 **epoll 注册失败**（说明工具自己坏了）。把它们
  // 记成同一个数，就等于把"我该调参"和"该修 bug"混成一条日志。
  std::uint64_t refused_full_ = 0;
  std::uint64_t refused_addfail_ = 0;
  int peak_clients_ = 0;
  std::uint64_t requests_ = 0;
  std::uint64_t responses_written_ = 0;
  std::uint64_t bytes_written_ = 0;
};

void usage(std::FILE* out) {
  std::fprintf(out,
      "l7gate_bench_upstream —— SLO #1 的压测上游（固定 1 KiB 响应、单线程 epoll ET）\n"
      "用法: l7gate_bench_upstream [--listen <ipv4:port>] [--body-bytes N]\n"
      "               [--max-clients N] [--backlog N]\n"
      "它只有一个能力：按请求回一段**每次一样**的字节。没有解析、没有按需分配、没有线程。\n"
      "不要拿它当功能测试的对端——故障形态（断在半路/坏框架/永不齐的头）在 tests/ 那个里。\n");
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string_view flag = argv[i];
    const char* value = (i + 1 < argc) ? argv[i + 1] : nullptr;
    if (flag == "--listen" && value != nullptr) {
      if (!parse_addr_port(value, &options.address, &options.port)) {
        std::fprintf(stderr, "参数错误: --listen 要的是 <ipv4:port>，收到 %s\n", value);
        return 2;
      }
      ++i;
    } else if (flag == "--body-bytes" && value != nullptr) {
      long n = 0;
      if (!parse_bounded(value, 1, 1 << 20, &n)) {
        std::fprintf(stderr, "参数错误: --body-bytes 要的是 1..1048576，收到 %s\n", value);
        return 2;
      }
      options.body_bytes = static_cast<std::size_t>(n);
      ++i;
    } else if (flag == "--max-clients" && value != nullptr) {
      long n = 0;
      if (!parse_bounded(value, 1, 100000, &n)) {
        std::fprintf(stderr, "参数错误: --max-clients 要的是 1..100000，收到 %s\n", value);
        return 2;
      }
      options.max_clients = static_cast<int>(n);
      ++i;
    } else if (flag == "--backlog" && value != nullptr) {
      long n = 0;
      if (!parse_bounded(value, 1, 65536, &n)) {
        std::fprintf(stderr, "参数错误: --backlog 要的是 1..65536，收到 %s\n", value);
        return 2;
      }
      options.backlog = static_cast<int>(n);
      ++i;
    } else if (flag == "--help" || flag == "-h") {
      usage(stdout);
      return 0;
    } else {
      std::fprintf(stderr, "参数错误: 无法识别的选项 %s\n", std::string(flag).c_str());
      usage(stderr);
      return 2;
    }
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  // SIGPIPE 必须忽略：压测客户端随时会消失，往已关闭的连接上写默认动作是**杀进程**。
  // 上游被一个提前退出的客户端杀掉，会表现成"压测数字莫名其妙少一半"，最难查。
  std::signal(SIGPIPE, SIG_IGN);

  BenchUpstream upstream(options);
  std::string why;
  if (!upstream.start(&why)) {
    std::fprintf(stderr, "压测上游启动失败: %s\n", why.c_str());
    return 1;
  }
  std::printf("[bench-upstream] 监听 %s:%u body=%zu\n", options.address.c_str(),
              static_cast<unsigned int>(upstream.port()), options.body_bytes);
  std::fflush(stdout);
  upstream.run();
  upstream.print_summary();
  std::fflush(stdout);
  return 0;
}
