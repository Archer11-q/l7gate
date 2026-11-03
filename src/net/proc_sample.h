#pragma once

#include <cstdint>

namespace l7gate::net {

// --- /proc 自报（S5-01 从 `main.cc` 原样搬过来，语义一字未改）--------------
// 冒烟脚本要的是「10000 请求之后 fd 回到基线、每连接内存是多少」，而这两个数
// 只有进程自己看 /proc 才拿得准：外部工具在别的时刻看，看到的可能是别的连接。
// 读不到就报 -1，绝不拿 0 冒充"没有泄漏"。
//
// S5-01（ADR-24 第四节）把它们搬进 net 层，是因为运行期的 `/metrics` 也要读同一对
// 数——搬而不复制，是为了不让"fd 现在几个"这件事有两份会各自漂移的实现。
// 采样动作发生在**调用方所在线程**：指标面读它的是采集线程，不是 worker 循环，
// 所以"汇聚不给 worker 循环加 syscall"这条约束与这两个函数无关。
std::int64_t count_open_fds();
std::int64_t current_rss_kib();

}  // namespace l7gate::net
