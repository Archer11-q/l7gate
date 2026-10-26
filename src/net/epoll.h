#pragma once

#include <cstdint>
#include <sys/epoll.h>
#include <vector>

#include "net/fd.h"

namespace l7gate::net {

// epoll 的薄封装。它只做三件事：创建、增改删、等待。
// 事件循环的策略（谁可读、谁该被摘掉读事件）不在这里，在 reactor。
//
// 有一件事它**必须**做：把 `EPOLLET` 在 add/modify 里无条件或进去。
// ADR-02 选的就是边缘触发，而"某个地方漏了一个 EPOLLET"的表现不是报错，
// 是行为悄悄变成水平触发——读写照样工作，只是 Buffer 的"读到 EAGAIN 为止"
// 从必需变成冗余，等到发现时已经是压测里一个解释不清的差异。
// 我不给自己留那个漏的口子里。
//
// 计数器是给验收用的，不是给日志用的：S1-04 要断言「epoll 注册/注销次数与
// 连接建立/关闭次数逐项相等」，那就得有一个与调用方自述无关的来源。
class Epoll {
 public:
  static constexpr std::uint32_t kForcedEvents = EPOLLET;

  // 只有 events 与 user_data 两个字段：epoll 带回来的就是这个，
  // fd 得由 reactor 从 user_data 反查（它持有 fd → 连接对象的注册表），
  // 而那次反查正是 fd 重用防护需要的同一步。刻意不设 `int fd` 字段——
  // 一个永远填不出真实值的字段比没有字段更危险。
  struct Event {
    std::uint32_t events = 0;
    void* user_data = nullptr;
  };

  // 构造期失败抛 std::system_error：epoll_create1 失败没有"退回单线程轮询"这种
  // 有意义的降级路径，继续跑只会让后面的每个数字都不可信。
  Epoll();
  ~Epoll();

  Epoll(const Epoll&) = delete;
  Epoll& operator=(const Epoll&) = delete;

  int fd() const noexcept { return fd_.get(); }

  // 注册/注销/修改。EPOLL_CTL_ADD 之后同一 fd 重复 ADD 会返回 EEXIST（原样带出来，
  // 不藏）——那是调用方的逻辑错误，我要它在返回值里可见。
  bool add(int fd, std::uint32_t events, void* user_data, int* error) noexcept;
  bool modify(int fd, std::uint32_t events, void* user_data, int* error) noexcept;
  bool remove(int fd, int* error) noexcept;

  // 等待事件。返回拿到的事件数（0 表示超时），负数表示失败；失败时 *error 有值。
  // 注意 EINTR：epoll_wait 被信号打断是正常事件，这里内部重试而不是报失败——
  // 把它冒出来会让调用方以为"这次等待没发生"，而事实是它发生了、还消费了时间片。
  int wait(int timeout_ms, std::vector<Event>* out, int* error) noexcept;

  // --- 观测面 ---------------------------------------------------------
  std::uint64_t registrations() const noexcept { return registrations_; }
  std::uint64_t unregistrations() const noexcept { return unregistrations_; }
  std::uint64_t modifications() const noexcept { return modifications_; }
  std::uint64_t wait_calls() const noexcept { return wait_calls_; }
  std::uint64_t events_delivered() const noexcept { return events_delivered_; }

 private:
  UniqueFd fd_;

  // 只统计**成功**的 ctl 调用。失败的那次不该进账，否则「注册数 == 连接数」
  // 这条断言会被一次 EEXIST 抵消掉，而那次 EEXIST 正是我要发现的 bug。
  std::uint64_t registrations_ = 0;
  std::uint64_t unregistrations_ = 0;
  std::uint64_t modifications_ = 0;
  std::uint64_t wait_calls_ = 0;
  std::uint64_t events_delivered_ = 0;
};

}  // namespace l7gate::net
