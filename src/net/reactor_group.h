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
// 停止语义沿用 S1（停止接受 + 退出循环，**不排水在飞连接**）：stop() 让 accept 循环
// 退出，run() 随后 stop 所有 worker 并 join。真正的排水 + 超时兜底是 S2-05。
class ReactorGroup {
 public:
  struct Config {
    std::string listen_address = "127.0.0.1";
    // 0 表示由内核选端口；测试全靠它避免端口撞车。
    std::uint16_t listen_port = 0;
    std::string upstream_address = "127.0.0.1";
    std::uint16_t upstream_port = 0;

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
  void stop() noexcept;

  int listen_port() const noexcept { return listen_port_; }
  int worker_count() const noexcept { return static_cast<int>(workers_.size()); }
  bool running() const noexcept { return running_.load(std::memory_order_relaxed); }
  bool stop_requested() const noexcept { return stop_requested_.load(std::memory_order_relaxed); }

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
  std::vector<Epoll::Event> events_;
};

}  // namespace l7gate::net
