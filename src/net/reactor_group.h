#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "net/connection.h"
#include "net/epoll.h"
#include "net/fd.h"
#include "net/reactor.h"
#include "net/stats.h"

namespace l7gate::net {

// 多 Reactor（S2-01）。ADR-03 的落地点 + ADR-14 的所有权模型：
//
//   **一个监听 fd，主线程 accept，round-robin 派发给 N 个 worker。** 每个 worker 是
//   一个独立的 `Reactor`（自己的 epoll、自己的循环线程、自己的连接注册表）。连接对象
//   **单线程拥有**：acceptor 只把裸 fd 经 worker 的 handoff 队列送过去，`Connection`
//   由 owner worker 自己构造、自己读写、自己关闭、自己析构。跨线程边界上流动的只有
//   一个 `int`（和一次 eventfd 写），所以"两个线程同时碰一条连接"在结构上不成立——
//   这正是让 TSan 门禁（契约 4.1，S2 唯一核心门禁）成为可证伪命题而不是自述的关键。
//
//   **`ReactorStats` 全 worker 共享一份**（全原子，并发 fetch_add 安全、聚合自动成立）。
//   `Epoll` 计数器与注册表观测是每 worker 各一份的非原子量，按 stats.h 的纪律只能在
//   对应循环停下之后读——本类的聚合 getter 因此都标注"run() 返回之后才可调用"。
//
// 线程账：run() 在**调用它的线程**上跑 accept 循环（生产里是主线程），另外 spawn
// N 条 worker 线程。所以总线程数 = 1 + N，与 ADR-03「主线程 accept + N 个 Reactor
// 线程」逐字对上。
//
// 停止有两条路（S2-05，ARCHITECTURE 1.20）：
//   **stop()** = 硬停，S1 语义原样保留——accept 循环退出，run() 随后 stop + join
//   所有 worker，在飞连接不被收尾（交给 ~Reactor）。既有测试与 `--serve-requests`
//   走的都是这条路，所以 S1/S2-04 的生命周期断言一条没改。
//   **request_drain(timeout_ms)** = 优雅停止：把排水下发给每个 worker，然后**继续在
//   accept 循环里空转**，直到每个 worker 都自行置上 stop_requested_（排水完成或兜底
//   到点）才 join。"不再接受新连接"落在 accept_and_dispatch()：排水期间 accept 到就把
//   那条 fd 关掉，而**监听 fd 留到析构**（理由与代价写在 Reactor::accept_all 的注释里）。
//   这里刻意不用现成的 stop_workers_and_join()：它是阻塞 join，会把 joiner 变成
//   排水的依赖——而 worker 的排水进度需要它自己的线程在跑。
//   排水期间再收到一次 stop() 就是运维意义上的"别再等了"：accept 循环退出，
//   run() 对每个 worker 走硬停。
class ReactorGroup {
 public:
  struct Config {
    std::string listen_address = "127.0.0.1";
    // 0 表示由内核选端口；测试全靠它避免端口撞车。
    std::uint16_t listen_port = 0;

    // S2-06a（ADR-17 已裁候选 A）：回源端点列表。**第 i 条 worker 只用第 i mod E 个端点**
    // （E = 列表长度），所以一条 worker 从头到尾只面对一个 (目的地址, 目的端口) 元组。
    // 这条形状是选它而不是选"共享池 + 按请求选目标"的实际原因：ADR-15 的 per-worker 池
    // 因此**仍然是单目标池**（`upstream_pool_` 不带目标键），不加锁、不扩大 ADR-14 的
    // 跨线程接触面。它**不是**路由也不是负载均衡：那两者要按请求决定目标，归 S3。
    // 容量账：这一跳能开出的连接数 = min(worker 数, E) × 单元组容量（本机实测 28,232），
    // 所以 `--workers 1` 的单 Reactor 路径永远只用第 0 个端点（`main.cc` 里就是这么取的）。
    struct UpstreamEndpoint {
      std::string address = "127.0.0.1";
      std::uint16_t port = 0;
    };
    // 空列表 = 没配上游（与 `Reactor::Config` 里 `upstream_port == 0` 同一口径，
    // `ensure_upstream` 那条短路一致）。
    std::vector<UpstreamEndpoint> upstream_endpoints;

    int listen_backlog = 1024;
    // worker 线程数 N。契约 SLO #3 的前提是本机 16 vCPU；默认取一个保守值，
    // 由 main.cc 的 --workers 覆盖（缺省仍走单 Reactor，见 ADR-14 后果段）。
    int worker_count = 1;
    // **每 worker** 的 fd 预算，总容量 = N × 这个值（ADR-14 后果段）。
    std::size_t max_connections_per_worker = 4096;

    Connection::Config connection;

    // > 0 时：共享 stats 的 requests_parsed 聚合达到这个数就让 accept 循环退出、
    // 随后停所有 worker。压测/验收需要"跑固定轮数后自己结束"。
    std::size_t stop_after_requests = 0;

    // accept 循环与每个 worker 循环的 epoll_wait 超时。
    int wait_timeout_ms = 200;
  };

  explicit ReactorGroup(Config config);
  ~ReactorGroup();

  ReactorGroup(const ReactorGroup&) = delete;
  ReactorGroup& operator=(const ReactorGroup&) = delete;

  // 绑定监听 socket、建 accept 侧 epoll、构造并启动 N 个 worker（各 start_worker）。
  // 失败返回 false，errno 由 *error 带出。**不 spawn 线程**——那是 run() 的事，
  // 这样"先 start_listen 拿端口、再决定何时 run"的测试时序成立。
  bool start_listen(int* error) noexcept;

  // spawn N 条 worker 线程，然后在**调用线程**上跑 accept 循环，直到 stop()。
  // 退出前 stop 所有 worker 并 join。返回 accept 循环处理的事件批次数。
  std::uint64_t run();

  // 跨线程调用（信号 handler / 测试主线程）：只做两件 async-signal-safe 的事——
  // 置 stop_requested_、写 accept 侧 eventfd。worker 的停止与 join 在 run() 里做。
  // **硬停**（S1 语义）：不排水。排水用 request_drain()。
  void stop() noexcept;

  // S2-05（ARCHITECTURE 1.20）：**优雅停止**。跨线程纪律与 stop() 同一条——只做
  // 原子 store + 写 accept 侧 eventfd，不碰任何 worker 的循环内状态。
  // 被叫醒的 accept 循环负责：把 request_drain() 下发给每个 worker，然后在自己的
  // 循环里等到**每个 worker 都自行置上 stop_requested_**（各自的排水各自完成）才退出
  // 并 join。"不再接受新连接"落在 accept_and_dispatch()：accept 到就关掉那条 fd，
  // 而监听 fd 留到析构（理由写在 Reactor::accept_all 的注释里）。
  void request_drain(std::int64_t timeout_ms) noexcept;

  int listen_port() const noexcept { return listen_port_; }
  int worker_count() const noexcept { return static_cast<int>(workers_.size()); }
  bool running() const noexcept { return running_.load(std::memory_order_relaxed); }
  bool stop_requested() const noexcept { return stop_requested_.load(std::memory_order_relaxed); }
  bool drain_requested() const noexcept { return drain_requested_.load(std::memory_order_relaxed); }

  ReactorStats& stats() noexcept { return *stats_; }
  const ReactorStats& stats() const noexcept { return *stats_; }

  // --- 聚合观测：每 worker 各一份的非原子量求和。**只在 run() 返回之后调用** ------
  // （worker 循环线程已 join，此时读它们的 Epoll 计数器与注册表才合法，见 stats.h
  // 第 25 行那条纪律。在循环还转着的时候读这些是 S1-08 抓过的那类 race。）
  std::uint64_t epoll_registrations() const noexcept;
  std::uint64_t epoll_unregistrations() const noexcept;
  std::uint64_t epoll_modifications() const noexcept;
  std::size_t live_connection_count() const noexcept;
  // 背压观测的聚合（每 worker 的 connections_with_read_disarmed 求和）。
  std::size_t connections_with_read_disarmed() const noexcept;
  // 派发均衡断言用：第 i 个 worker 注册表里当前的连接数。同样只在循环停下后读。
  std::size_t worker_live_count(int i) const noexcept;

 private:
  void accept_and_dispatch();
  void drain_accept_wake();
  // 每个 worker 都自己置上了 stop_requested_（排水完成或兜底到点）。
  // 只读原子量，所以 accept 线程在循环里查它是合法的——这也是本组不需要
  // 任何"排水完成"投递队列的原因。
  bool all_workers_stop_requested() const noexcept;
  // stop 所有 worker + join 所有线程。幂等：run() 末尾调一次，~ReactorGroup 再调
  // 一次兜底（run() 没被调用、或中途抛异常时线程不会带着未 join 的状态被析构）。
  void stop_workers_and_join();

  Config config_;
  // 声明顺序 = 销毁顺序的反面，这里有两处硬约束：
  //   ① stats_ 必须在 workers_ 之前声明 —— worker 持 ReactorStats&，不能比它先死；
  //   ② threads_ 必须在 workers_ 之前销毁前 join —— 析构函数体先 join，再让成员
  //      按逆序（threads_ → workers_ → stats_）unwind，所以 joinable 的线程不会
  //      在 Reactor 还活着时被拆掉。
  std::unique_ptr<ReactorStats> stats_;
  UniqueFd listener_;
  Epoll accept_epoll_;
  UniqueFd accept_wake_fd_;
  int listen_port_ = -1;

  std::vector<std::unique_ptr<Reactor>> workers_;
  std::vector<std::thread> threads_;

  std::atomic<std::size_t> rr_counter_{0};
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};
  // 排水请求与它的兜底窗口。两个都是原子：request_drain() 可能来自信号 handler 或
  // 测试线程，而读它们的是跑 accept 循环的调用线程。窗口存的是**毫秒数**（不是绝对
  // 时刻），因为下发给每个 worker 的那一刻才由 worker 各自换算成自己的 deadline——
  // 换算点越靠近实际干活的那条线程，多个 worker 之间的"同时到点"就越不是假话。
  std::atomic<bool> drain_requested_{false};
  std::atomic<std::int64_t> drain_timeout_ms_{0};
  std::vector<Epoll::Event> events_;
};

}  // namespace l7gate::net
