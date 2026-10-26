#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace l7gate::net {

// 连续字节缓冲。它是 ADR-02 选 ET 之后无法回避的那个组件：
// 边缘触发要求一次把可读的数据读到 EAGAIN 为止，否则事件不会再投递、数据永久卡住。
// 这条约束把"缓冲区"从可选优化变成必需结构 —— 上下游速度不匹配时数据落在哪里、
// 最多能落在哪里，全在这里定义（S1-07 的背压就是它的直接消费者）。
//
// 设计取舍，我先把理由写死在头文件里，因为它决定了后面所有指标的口径：
//
// 1. **连续内存，不做链式/chunk list。** HTTP 解析要求头部区段连续才能安全地
//    扫 CRLF 与判定 chunk-size，链式缓冲区会把"是否读到一个完整请求头"变成
//    一个需要跨节点判断的问题。代价是大 body 时 memmove 与扩容成本，换来的是
//    解析层可以写成纯函数（S1-03 的前提）。
// 2. **每个连接一个 Buffer 实例，容量按需几何增长。** 每连接内存 = 连接对象 +
//    buffer 实际分配，这是 SLO #2「每连接 ≤ 20 KB」的直接来源。
//    所以我必须能把 peak capacity 量出来，而不是等压测时抓 RSS 反推。
// 3. **两条线，不是一个。** `high_water_mark` 是停读线（S1-07 的钩子），
//    `max_capacity` 是容量增长的绝对上限。撞线是一个**可观测事件**（`limit_reached`），
//    不是错误 —— 把它做成错误的话，调用方就没法区分"对端太快"和"我坏了"。
class Buffer {
 public:
  struct Config {
    std::size_t initial_capacity = 4096;
    std::size_t max_capacity = 1u << 20;        // 1 MiB：容量增长的绝对上限
    std::size_t high_water_mark = 256u << 10;   // 256 KiB：**停读线**，早于绝对上限
  };

  // 一次"读到 EAGAIN"的结果。把 syscall 次数与是否真的撞过 EAGAIN 带出来，
  // 是为了让 ET 契约可以被**断言**而不是靠我声称遵守了。
  struct ReadResult {
    std::size_t bytes_read = 0;
    std::size_t read_syscalls = 0;
    bool would_block = false;    // 收到 EAGAIN —— ET 语义要求本轮必须看到它
    bool peer_closed = false;    // 收到 0
    bool limit_reached = false;  // 撞到 high_water_mark（停读线），本轮在**我们自己这边**提前停止
                                 // —— 这是 S1-07 的钩子，撞线是可观测事件而不是错误
    int errno_value = 0;         // 真错误时的 errno；0 表示没有错误
  };

  // 默认 Config 与自定义 Config 分成两个构造函数写。
  // 我第一版写成 `explicit Buffer(Config config = {})`，GCC 15 在类内默认实参
  // 上对嵌套聚合的 brace-init 报了 "could not convert <brace-enclosed initializer list>"
  // —— 与其跟编译器赌，不如把"不传参数就是默认配置"这件事写成显式重载，语义更清楚。
  Buffer();
  explicit Buffer(const Config& config);
  ~Buffer();

  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;
  Buffer(Buffer&&) noexcept;
  Buffer& operator=(Buffer&&) noexcept;

  // --- 容量与观测 -------------------------------------------------------
  std::size_t capacity() const noexcept { return storage_.size(); }
  std::size_t readable_bytes() const noexcept { return write_pos_ - read_pos_; }
  std::size_t writable_bytes() const noexcept { return storage_.size() - write_pos_; }
  std::size_t peak_capacity() const noexcept { return peak_capacity_; }
  std::size_t max_capacity() const noexcept { return config_.max_capacity; }
  bool above_high_water_mark() const noexcept;

  // --- 写入侧 -----------------------------------------------------------
  // 返回一块可以直接 recv()/read() 进去的可写区域。
  // **hint 是"本轮至多想要这么多"，不是预留**：实际给多少由当时的剩余空间决定，
  // 所以返回的 size 可以小于 hint，调用方必须按返回值办（buffer_test 的
  // append_all 就是这个用法的示例）。扩容只在**真满**（writable_bytes()==0）时
  // 发生，且一次翻倍不超过 max_capacity。撞上限时返回空区域，由调用方决定停读。
  std::pair<std::uint8_t*, std::size_t> writable_region(std::size_t hint);
  void commit_written(std::size_t n);

  // 从 fd 一直读到 EAGAIN。ET 语义的唯一入口，故意不提供"读一次"的版本：
  // 留一个单次读的口子里，迟早有人在事件循环里偷懒用它。
  //
  // 返回时保证三条，它们是这个类对外的契约：
  //   1. readable_bytes() <= high_water_mark（单次请求量被夹在距水位剩余之内，
  //      所以水位是硬不变量，不是"大概会停在这附近"）
  //   2. would_block / peer_closed / limit_reached 三者至多一个为真，
  //      且必有一个为真 —— 除非中途遇到真 errno
  //   3. limit_reached 为真时本轮**没有**读到 EAGAIN：停读发生在我们自己这边，
  //      而不是内核那边。这个区别就是 S1-07 能否正确恢复读取的前提。
  ReadResult read_until_blocked(int fd);

  // --- 读取侧 -----------------------------------------------------------
  const std::uint8_t* readable_data() const noexcept { return storage_.data() + read_pos_; }
  // 消费 n 字节（不拷出）。n 超过可读长度时返回 false 且不改动状态。
  bool consume(std::size_t n);
  // 把可读数据的前 n 字节拷进 out 并消费。返回实际消费数。
  std::size_t retrieve(void* out, std::size_t n);
  // 保留数据、只把读位置前移 n 字节（解析层"我先看看但不吃"的用法）。
  bool peek_consume(std::size_t n);

  void clear() noexcept;

 private:
  void record_peak() noexcept;

  Config config_;
  std::vector<std::uint8_t> storage_;
  std::size_t read_pos_ = 0;
  std::size_t write_pos_ = 0;
  std::size_t peak_capacity_ = 0;
  // 单次 ::read 的申请量，自适应：只在"上一次把申请量读满了"时才翻倍，上限 64 KiB。
  // 这个数就是每连接内存的**下限**，所以我在这里记全踩过的两版：
  //   1. 固定按 64 KiB 申请 → 1 KiB keep-alive 稳态把容量顶到 128 KiB；
  //   2. 申请量自适应、但 writable_region 仍按 hint 预留空间 → 同样场景顶到 8 KiB。
  // 两版都远超 SLO #2 的每连接 ≤ 20 KB 预算，而它们都是在测试之前看不出来的。
  // 现在默认配置实测 peak_capacity = initial_capacity = 4096（buffer_test 的
  // PeakCapacityUnderOneKiBKeepAliveWorkload 就是钉这个数的用例）。
  std::size_t read_hint_ = 0;
};

}  // namespace l7gate::net
