#pragma once

#include <atomic>
#include <cstdint>

namespace l7gate::net {

// O(n) 那一格（遍历注册表数"有多少条连接被摘了读"）的采样下限。它不能跟循环的拍走：
// 探活开着时一拍只有 20ms（`reactor.cc` 把 `epoll_wait` 上限夹到探活间隔），按拍遍历
// 等于每秒 50 次 O(连接数) 的走查，那就是仪器在被测方身上踩出成本。
inline constexpr std::int64_t kMetricsSampleEveryMs = 1000;

// S5-01（ADR-24 第四节）：每 worker 一份的**运行期观测槽**。
//
// 这里放的只有 `ReactorStats` 装不下的那一族：它们在退出账里今天是由
// `ReactorGroup` 在全部 worker join 之后遍历求和得到的（`reactor_group.cc:305-309`），
// 底层是每 worker 各一份的**非原子**量（`Epoll::modifications_` 等，`epoll.h:68-72`），
// 而 `stats.h:25` 的纪律明写着这类观测「只能在循环线程 join 之后读」。
// /metrics 是 pull 模型，读的时候循环一定在转，所以必须新造一条合法的路：
//
//   **属主线程把值抄进原子槽，采集线程只读原子。**
//
// 抄的动作发生在唯一有权读那张注册表与那个 Epoll 实例的线程上，纪律没有被绕过，
// 被改变的只是"谁读"——采集线程读的是这个槽，不是 worker 的私有内存。
//
// 写入节拍是循环**已有的那一拍**（`wait_timeout_ms`，缺省 200ms），不是每事件：
// 每事件写等于为了观测去改被 B-018 当作结构性尺的那条指令。唯一的 O(n) 量
// （`read_disarmed_connections` 要遍历注册表）降到每秒至多一次，见 ADR-24 第四节。
//
// `alignas(64)`：16 条 worker 各占一根 cache line。共用一根线的话，采集线程每次
// 渲染都要把 16 条热循环的 line 逐个拉过来，那才是真的给被测方加成本。
struct alignas(64) LoopObservation {
  std::atomic<std::uint64_t> epoll_registrations{0};
  std::atomic<std::uint64_t> epoll_unregistrations{0};
  std::atomic<std::uint64_t> epoll_modifications{0};
  std::atomic<std::uint64_t> read_disarmed_connections{0};
};

// 求和后的形状。它是**普通值**而不是原子：一次求和就是一次切面，把它做成原子表
// 反而暗示"这个数在某一瞬间同时成立"，而四条 relaxed load 之间循环还在走。
// 退出账那边（`reactor_group.cc:305-309`）同样是逐 worker 相加，口径一致。
struct LoopObservationSum {
  std::uint64_t epoll_registrations = 0;
  std::uint64_t epoll_unregistrations = 0;
  std::uint64_t epoll_modifications = 0;
  std::uint64_t read_disarmed_connections = 0;
};

// n = 0（没开指标面）时返回全零，不解引用空指针。
inline LoopObservationSum sum_loop_observations(const LoopObservation* slots,
                                                std::size_t n) noexcept {
  LoopObservationSum out;
  for (std::size_t i = 0; i < n; ++i) {
    out.epoll_registrations += slots[i].epoll_registrations.load(std::memory_order_relaxed);
    out.epoll_unregistrations += slots[i].epoll_unregistrations.load(std::memory_order_relaxed);
    out.epoll_modifications += slots[i].epoll_modifications.load(std::memory_order_relaxed);
    out.read_disarmed_connections += slots[i].read_disarmed_connections.load(std::memory_order_relaxed);
  }
  return out;
}

}  // namespace l7gate::net
