#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "metrics/metrics_render.h"
#include "net/epoll.h"
#include "net/fd.h"
#include "net/socket.h"
#include "route/config_snapshot.h"
#include "route/nodes.h"

namespace l7gate::filter {
class RateLimiter;
class RedisSettler;
}  // namespace l7gate::filter

namespace l7gate::metrics {

// S5-01（ADR-24 第六节）：`/metrics` 的暴露面——**一条独立线程 + 一个独立端口**。
//
// 它为什么必须是自己的一条线程而不是复用某条 worker 的循环（ADR-24 第三节）：
//   * 抓取连接因此**结构上**碰不到 `Connection`，那个唯一的 `requests_parsed`
//     自增点（`connection.cc:184-187`）对它不可达——验收 ② 是"没有代码路径"，
//     不是"写了个 if"；
//   * 它的 `epoll_ctl` 打在自己的 `Epoll` 实例上，一次都不进 worker 那几份，
//     所以 B-018 那把"每请求 `epoll_modifications`"的尺不被 scrape 节拍污染；
//   * 读 `/proc` 的那些 syscall 落在采集线程上，"不给 worker 循环加 syscall"
//     这条约束在物理上就是靠这个线程边界成立的。
//
// 生命周期：`Dependencies` 里全是**借来的裸指针**，本对象不拥有任何东西。装配方必须
// 让它们比自己活得久——`main.cc` 里的声明顺序因此不是风格问题（与 `ReactorGroup`
// 那条 `stats_` 必须在 `workers_` 之前的规矩同一条）。
struct Dependencies {
  // 静态格与那张原子表、观测槽数组（`render()` 直接吃它，见 metrics_render.h）。
  Sources base;
  // 这三格每次抓取现读，所以给的是所有者指针而不是 `*Accounts` 值。
  const l7gate::filter::RateLimiter* rate_limiter = nullptr;
  const l7gate::filter::RedisSettler* redis_settler = nullptr;
  const l7gate::route::NodesPublisher* nodes = nullptr;
  // S5-02：配置世代的账（nullptr = 没 arm，那一整族键一个不发）。
  const l7gate::route::ConfigPublisher* config = nullptr;
};

// 暴露面本体。三拍状态机（读请求 → 写响应 → 关），没有会话状态、没有 keep-alive：
// **观测面的代码量本身就是风险面**（ADR-24 第六节）。
class MetricsServer {
 public:
  static constexpr std::size_t kMaxConcurrentScrapes = 8;
  // 请求头上限。超过它就是 431，不跟一个不肯发完头部的客户端耗着。
  static constexpr std::size_t kMaxRequestBytes = 4096;

  struct Config {
    std::string bind_address = "127.0.0.1";
    // 0 = 由内核选端口（测试全靠它避免端口撞车，与 `Reactor::Config::listen_port`
    // 同一条约定；`--metrics-port 0` 走的就是这一格）。
    std::uint16_t port = 0;
    int backlog = 16;
    // 一条抓取连接的读写期限。超时就关掉并计入 `metrics_scrapes_failed`。
    std::int64_t scrape_timeout_ms = 2000;
    // 采集线程 epoll_wait 的上限。它决定 stop() 之外还能靠什么结束这条线程，
    // 与 worker 的那个 200ms 是两把独立的旋钮（不共享：共享等于让观测面寄生在被测方上）。
    int wait_timeout_ms = 200;
  };

  explicit MetricsServer(Config config, Dependencies deps) noexcept;
  ~MetricsServer();

  MetricsServer(const MetricsServer&) = delete;
  MetricsServer& operator=(const MetricsServer&) = delete;

  // 绑定 + 建自己的 epoll + 建自唤醒 eventfd。**不 spawn 线程**——先拿到端口再决定
  // 何时起跑，与 `ReactorGroup::start_listen` 同一个顺序（`reactor_group.h:132-133`）。
  // **必须在 `count_open_fds()` 取 `fd_start` 之前调用**（ADR-24 第四节那条硬前置），
  // 否则这个常驻监听 fd 只出现在 `fd_end` 一侧，`fd_start == fd_end` 那条贯穿九条
  // 冒烟的生命周期判据当场断。
  bool start_listen(int* error) noexcept;

  // 起线程。`start_listen` 没成功时返回 false（不给自己发一张"起来了"的证明）。
  bool start(int* error) noexcept;

  // 置停止标志 + 写 eventfd 叫醒采集线程，然后 join。幂等。
  // **必须在打退出账之前调用**（`main.cc:947-949` 那条"结算线程先停再打账"的同一根
  // 理由：否则最后一拍的渲染会与业务账互相插队，读者看到的是一笔对不上的中间态）。
  void stop() noexcept;

  int port() const noexcept { return port_; }
  bool running() const noexcept { return running_.load(std::memory_order_relaxed); }

  // 启动基线两格。`fd_start` / `rss_start_kib` 是**启动那一刻**的采样，而本对象必须
  // 比它更早存在（监听 fd 要进 `fd_start` 那一侧，见 `ADR-24` 第四节的硬前置），
  // 所以这两格是"起了之后补进去"的：只能在 `start()` 之前或循环还没被抓住的时候调。
  void set_start_baseline(std::int64_t fd_start, std::int64_t rss_start_kib) noexcept {
    deps_.base.fd_start = fd_start;
    deps_.base.rss_start_kib = rss_start_kib;
  }

  // 仪器自己的三格（只由采集线程写，读侧是主线程打退出账时——那时线程已 join）。
  std::uint64_t scrapes_ok() const noexcept { return scrapes_ok_.load(std::memory_order_relaxed); }
  std::uint64_t scrapes_failed() const noexcept {
    return scrapes_failed_.load(std::memory_order_relaxed);
  }
  std::uint64_t render_bytes() const noexcept { return render_bytes_.load(std::memory_order_relaxed); }

  // 给测试用的纯入口：一次渲染（不碰 socket）。也是 `S5_01_MetricsCatalog`
  // 拿键名清单的地方——渲染层与传输层分开，格式才有独立的断言点。
  std::string render_once(std::int64_t now_ms);

 private:
  void thread_main() noexcept;
  void accept_all() noexcept;
  void close_conn(std::size_t slot) noexcept;
  // 返回这次响应的状态码；200 才算一次成功抓取。
  int handle_request(std::size_t slot) noexcept;

  Config config_;
  Dependencies deps_;

  l7gate::net::UniqueFd listener_;
  l7gate::net::UniqueFd wake_fd_;
  l7gate::net::Epoll epoll_;
  int port_ = -1;

  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> running_{false};
  std::thread thread_;

  // 抓取连接的小槽位表。按索引而非指针登记进 epoll（低位标签），照探活那一族的先例
  // （`reactor.h:468-497`）：槽位表不搬家，所以索引永远是稳的。
  struct Conn {
    l7gate::net::UniqueFd fd;
    std::string request;   // 累积到的请求字节（上限 kMaxRequestBytes）
    std::string reply;     // 已渲染好的完整响应（含状态行与头）
    std::size_t sent = 0;
    std::int64_t deadline_ms = 0;
    bool in_use = false;
    // 这次响应是不是失败档（400/404/405/431/超时）。判完就记下，等**字节真的发完**
    // 再进账——现在就关连接的话客户端拿到的是 RST，那连"我回过它"都证明不了。
    bool failed = false;
    // exposition 自己的字节数（不含状态行与响应头），进 `metrics_render_bytes`。
    std::size_t body_bytes = 0;
  };
  Conn conns_[kMaxConcurrentScrapes];

  std::atomic<std::uint64_t> scrapes_ok_{0};
  std::atomic<std::uint64_t> scrapes_failed_{0};
  std::atomic<std::uint64_t> render_bytes_{0};

  std::vector<l7gate::net::Epoll::Event> events_;
};

}  // namespace l7gate::metrics
