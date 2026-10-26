#pragma once

#include <utility>

namespace l7gate::net {

inline constexpr int kInvalidFd = -1;

// 文件描述符的 RAII 包装。
//
// 它存在的理由不是"整洁"，是 S1-04 的验收本身：「fd 数不随请求数单调增长」。
// 一个靠每条 return 路径手写 close() 的连接层，这个断言迟早会红——而红了之后
// 我根本不知道是哪条路径漏的。把所有权收进类型之后，"漏 fd" 等价于 "对象没析构"，
// 后者 ASan/LSan 能直接告诉我。
//
// 刻意不提供 `int get() const` 之外的隐式转换：fd 一旦能隐式当 int 用，
// 它就会流进 `if (fd)` 这种看起来对、实际把 kInvalidFd(-1) 当 true 的写法。
class UniqueFd {
 public:
  UniqueFd() = default;
  explicit UniqueFd(int fd) noexcept : fd_(fd) {}
  ~UniqueFd();

  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  UniqueFd(UniqueFd&& other) noexcept : fd_(other.fd_) { other.fd_ = kInvalidFd; }
  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
      close_now();
      fd_ = other.fd_;
      other.fd_ = kInvalidFd;
    }
    return *this;
  }

  int get() const noexcept { return fd_; }
  bool valid() const noexcept { return fd_ != kInvalidFd; }
  explicit operator bool() const noexcept { return valid(); }

  // 放弃所有权（调用方接手关闭责任）。
  int release() noexcept {
    const int fd = fd_;
    fd_ = kInvalidFd;
    return fd;
  }

  // 关旧纳新。传 kInvalidFd 等于只关旧的。
  void reset(int fd) {
    if (fd_ != fd) {
      close_now();
      fd_ = fd;
    }
  }

  // 显式关闭。重复调用安全（第二次是 no-op），所以它可以在状态机里
  // 被"关闭"这个动作无条件调用，不需要调用方先判断有没有关过。
  void close_now() noexcept;

 private:
  int fd_ = kInvalidFd;
};

}  // namespace l7gate::net
