#pragma once

#include <atomic>
#include <cstdint>

namespace l7gate::net {

// 事件循环的观测面。
//
// 为什么全是 `std::atomic<std::uint64_t>` 而不是普通整数：S1-04 的测试是
// 「另起一个线程跑 reactor，主线程读计数」。这在单线程语义下不是必须的
// （只有循环线程写），但它恰好是 S2 门禁要验的第一件事——连接对象跨线程
// 传递时的数据竞争。现在写成原子，TSan 在 S1 就给我一个干净的基线；
// 等 S2 真加线程时，"这条线有没有竞争"才有前后对照，而不是从零开始猜。
//
// 所有读取走 `relaxed`：这些数字是**观测**，不是同步手段。谁要是拿它们当
// "等到 requests_parsed == 10000 就往下走"的同步条件，那是把观测面当锁用，
// 那类 bug 的表现是偶发挂住而不是报错。
//
// 这条禁令要区分生产代码和测试，否则它会把测试逼成别的东西。生产逻辑里读计数
// 来决定下一步（"等 live_connections 归零再复用这块内存"）就是被禁止的那种用法；
// 测试里可以等计数，但必须**先有内核级观测**（read() 返回 0、响应已收全、连接已
// 建立），等的只是同一批事件处理里紧跟其后的那几步收尾。前者的病因是"事情有没有
// 发生"，后者只是"我这侧的观察有没有传到位"——用错哪一种都会偶发挂住，但只有
// 前者是设计缺陷。非原子的观测（epoll 计数、live_connection_count()）不进这条
// 规则，它只能在循环线程 join 之后读。
struct ReactorStats {
  ReactorStats() = default;
  ReactorStats(const ReactorStats&) = delete;
  ReactorStats& operator=(const ReactorStats&) = delete;

  std::uint64_t load(std::atomic<std::uint64_t>& counter) const noexcept {
    return counter.load(std::memory_order_relaxed);
  }
  // 上面那个 helper 只是把 memory_order_relaxed 这件事集中写一次，
  // 免得每个测试各写一遍、其中某处顺手写成 seq_cst 还以为自己改了什么语义。


  // --- 对象生命周期：验收句「引用计数归零并析构」靠这一对数相等来断言 -------
  std::atomic<std::uint64_t> connections_constructed{0};
  std::atomic<std::uint64_t> connections_destructed{0};
  // **已构造减去已析构**，不是"注册表里有几个"。两者在延后析构下会短暂不一致：
  // 六步关闭的第 5 步（析构）排在事件批处理结束时，所以 fd 已经关了、对象还在
  // retiring_ 里。写成"注册表数"会让这个字段在每个批次的尾巴上都和名字不符，
  // 而测试正是要靠它判断"有没有对象抱着不放"。注册表的实时大小由
  // Reactor::live_connection_count() 给出，那个只能在循环线程停下之后读。
  std::atomic<std::uint64_t> live_connections{0};

  // --- 连接建立/关闭：与 epoll 注册/注销逐项对照用 ------------------------
  std::atomic<std::uint64_t> downstream_accepted{0};
  std::atomic<std::uint64_t> upstream_opened{0};
  std::atomic<std::uint64_t> upstream_connect_failed{0};
  std::atomic<std::uint64_t> connections_closed{0};

  // 两类"我主动不接"的计数。它们和 connections_closed 必须分开：混在一起的话
  // 「fd 数不增长」这条断言就失去了意义——被预算挡掉的连接也是连接。
  std::atomic<std::uint64_t> downstream_rejected_over_capacity{0};
  std::atomic<std::uint64_t> accept_errors{0};  // EMFILE/ENFILE 一类

  // --- 请求与转发 -------------------------------------------------------
  std::atomic<std::uint64_t> requests_parsed{0};
  std::atomic<std::uint64_t> requests_rejected{0};
  std::atomic<std::uint64_t> bytes_down_to_upstream{0};
  std::atomic<std::uint64_t> bytes_up_to_downstream{0};

  // --- S1-03 那两笔债的偿还证据 -----------------------------------------
  // 游标累计扫描的字节数。没有游标时这个数字应该等于"每轮都把整个头部区重扫一遍"
  // 的量（S1-03 实测 7,474,911），有游标之后它应该只随**新到达的字节**增长。
  std::atomic<std::uint64_t> header_scan_bytes{0};
  std::atomic<std::uint64_t> parse_calls{0};

  // --- S1-07 的钩子：水位撞了几次（本任务只统计，不摘事件） -------------
  std::atomic<std::uint64_t> high_water_hits{0};

  // 出向 Buffer 的实际分配情况，卡点四那笔账的数据来源：
  // 惰性分配到底省下了多少，只有把"创建了几个"和"每个多大"分开记才算得出来。
  std::atomic<std::uint64_t> outbound_buffers_created{0};
  std::atomic<std::uint64_t> scratches_created{0};
};

}  // namespace l7gate::net
