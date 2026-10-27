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

  // --- S1-02/S1-07：停读线（高水位）的观测面 -----------------------------
  // 口径从 S1-02 起就没动过，也不许动：`limit_reached` 这个**事件**发生几次就计几次
  // （一轮 `read_until_blocked` 最多一次，因为它撞线就 break）。S1-07 加的是"撞线之后
  // 摘掉 EPOLLIN"这个动作，不是新的计数理由——所以这条现在仍然只回答"我停了几次自己"，
  // 下面那两条才回答"我停没停对端"。改了这条口径，S1-02 那张反证表就作废了。
  std::atomic<std::uint64_t> high_water_hits{0};
  // S1-07 的背压动作：EPOLLIN 的一次 1→0 / 0→1 跳变各计一次，且只在**真的成功交给
  // epoll_ctl** 时计（去重靠 Connection::armed_events() 那份回执）。
  // 口径写死三条，免得日后被读成别的意思：
  //   pause  —— 只计"因入向可读区到达停读线"而摘掉 EPOLLIN 的那次跳变；
  //             kClosing/kClosed 那种"收尾连接不再读"不是背压，不进这里。
  //   resume —— 只计把 EPOLLIN 挂回去的跳变，每一次都配一次主动探测读（边沿触发下
  //             挂回之后内核不会再报已经躺在接收缓冲里的那些字节，少一次探测就是
  //             永久睡死）。所以 resume 与探测读的次数**逐条相等**，这是可断言的。
  // 两者都是"停读线的滞回跳变"，不是水位线本身：滞回带取 [0, hwm/2] 恢复、
  // [hwm, ∞) 暂停，中间的区间保持原状（理由见 ARCHITECTURE 1.15）。
  std::atomic<std::uint64_t> backpressure_pause_count{0};
  std::atomic<std::uint64_t> backpressure_resume_count{0};
  // 两个方向各自累计**从 fd 读进缓冲**的字节数。它们存在的唯一理由是"平台期"这件事
  // 需要一个可断言的量：停读生效时这条连接不再有新字节进入网关，于是它停止增长；
  // 恢复后没有新的客户端写也能继续增长（那正是探测读在起作用）。
  // 注意它们是累计值不是速率，也不参与任何策略判定（同上那条禁令）。
  std::atomic<std::uint64_t> bytes_read_downstream{0};
  std::atomic<std::uint64_t> bytes_read_upstream{0};

  // --- S1-05：Upgrade 透传与 Connection: close 的观测面 ------------------
  // 101 透传路径的进入次数。"每条隧道恰好一次"就是它的全部语义：帧的收发
  // 不计数（透传不感知内容，感知了就违反 ADR-09），字节的量由下面两个数覆盖。
  std::atomic<std::uint64_t> upgrades_101{0};
  std::atomic<std::uint64_t> bytes_tunnel_down{0};  // 下游 → 上游（隧道方向之一）
  std::atomic<std::uint64_t> bytes_tunnel_up{0};    // 上游 → 下游（含完整 101 响应头）
  // 请求侧 `Connection: close` 被兑现的次数（配对连接置为一次性）。
  std::atomic<std::uint64_t> close_headers_honored{0};

  // --- S1-06：响应语义面的观测 -------------------------------------------
  // 网关自产并排进下游出向的错误响应条数（4xx 对畸形请求 / 502 对坏上游）。
  // "回 400 而不是只关连接"这条验收要有可断言的来源，而不能靠 curl 的退出码猜。
  std::atomic<std::uint64_t> gateway_replies_sent{0};
  // 上游在**最终响应头之前**断开/坏头 → 网关回 502 的次数。与下一条互斥可断言：
  // 头已经出去之后只能以 EOF 表态，那记在下面那条，不会重复计入这一条。
  std::atomic<std::uint64_t> upstream_truncated_before_head{0};
  // 最终头已透传、body 未走完就断/框架坏 → 以 EOF 告知截断的次数。
  std::atomic<std::uint64_t> upstream_truncated_in_body{0};
  // 按框架判定"完结"的响应条数（kComplete 边界的到达次数）。close 语义的
  // "响应边界"兑现点计数，也是 keep-alive 配对接力的放行信号。
  std::atomic<std::uint64_t> responses_completed{0};

  // 出向 Buffer 的实际分配情况，卡点四那笔账的数据来源：
  // 惰性分配到底省下了多少，只有把"创建了几个"和"每个多大"分开记才算得出来。
  std::atomic<std::uint64_t> outbound_buffers_created{0};
  std::atomic<std::uint64_t> scratches_created{0};

  // --- S2-02：per-worker 上游连接池（ADR-15）的观测面 ----------------------
  // 池把上游连接数与下游**生命周期**解耦：一条上游在它的下游关闭后归还进池、
  // 被下一条下游借走复用，而不是随下游一起销毁。下面四个数就是这条回收链的证据，
  // 缺一个就说不清"到底复用了没有"：
  //   pool_borrows        —— 从池里借出热上游的次数（复用发生了）。
  //   pool_returns        —— 下游关闭时上游被归还进池的次数（回收发生了）。
  //   pool_evictions_eof  —— 空闲池内上游被对端关闭而淘汰的次数（正常回收，
  //                          **绝不许**误计成 upstream_truncated_before_head，见 ADR-15）。
  //   pool_evictions_full —— 池满时归还改成直接关闭的次数（pool_max_idle 的硬上界生效）。
  // 新建上游仍计在既有的 upstream_opened 上（池空才新建），所以
  // 「N 条短命下游只开 M 条上游」= upstream_opened(=M) ≪ downstream_accepted(=N)，
  // 差额由 pool_borrows 补上。当前空闲数是**非原子**观测（Reactor::pool_idle_count()），
  // 只能在循环停下后读，和 live_connection_count() 同一类。
  std::atomic<std::uint64_t> pool_borrows{0};
  std::atomic<std::uint64_t> pool_returns{0};
  std::atomic<std::uint64_t> pool_evictions_eof{0};
  std::atomic<std::uint64_t> pool_evictions_full{0};
};

}  // namespace l7gate::net
