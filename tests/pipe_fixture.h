#pragma once

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <thread>

#include <gtest/gtest.h>

#include "net/fd.h"

namespace l7gate::testing {

// 非阻塞管道夹具。用真 fd 而不是 mock，是因为 ET 契约的内容就是"读到 EAGAIN 才停"，
// 这件事只有内核能告诉我；mock 只会告诉我"我以为它是这样"。
// S1-02（buffer_test.cc）和 S1-03（request_parser_test.cc 的 dribble 用例）共用它，
// 所以从测试文件里抽出来放在头里 —— 复制两份的代价是其中一份会先腐烂。
class PipeFixture {
 public:
  PipeFixture() {
    int fds[2] = {-1, -1};
    if (::pipe2(fds, O_NONBLOCK) == 0) {
      rfd_ = fds[0];
      wfd_ = fds[1];
    }
  }
  ~PipeFixture() {
    if (rfd_ >= 0) ::close(rfd_);
    if (wfd_ >= 0) ::close(wfd_);
  }
  PipeFixture(const PipeFixture&) = delete;
  PipeFixture& operator=(const PipeFixture&) = delete;

  bool ok() const { return rfd_ >= 0 && wfd_ >= 0; }
  int rfd() const { return rfd_; }
  int wfd() const { return wfd_; }

  void release_write_end() {
    if (wfd_ >= 0) {
      ::close(wfd_);
      wfd_ = -1;
    }
  }

  // 一直写到全部落地。管道满（Linux 默认 64 KiB）时 EAGAIN 是正常状态，不是失败 ——
  // 我第一版在这里写了 ASSERT_GE(n, 0)，把正常的 EAGAIN 判成测试失败，
  // 只是当时的负载都没超过管道容量所以没暴露。
  void write_all(std::string_view data) {
    std::size_t off = 0;
    int eagain_spins = 0;
    while (off < data.size()) {
      const ssize_t n = ::write(wfd_, data.data() + off, data.size() - off);
      if (n > 0) {
        off += static_cast<std::size_t>(n);
        continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        ASSERT_LT(++eagain_spins, 1000000) << "管道始终写不进去";
        std::this_thread::yield();
        continue;
      }
      ADD_FAILURE() << "pipe write failed errno=" << errno;
      return;
    }
  }

 private:
  int rfd_ = -1;
  int wfd_ = -1;
};

// 管道夹具的"所有权转移"版本：读端交给 Connection 持有（UniqueFd），夹具只留写端。
// 和 PipeFixture 的分工：PipeFixture 服务"我拿裸 fd 喂 Buffer"（S1-02/S1-03），
// PipePair 服务"连接对象真的拥有一个 fd"（S1-04 起）。同样只留一份定义——
// 它原本长在 connection_test.cc 里，S1-05 的 upgrade_test.cc 也要用它，
// 而复制两份的代价写在上面那段注释里：其中一份会先腐烂。
class PipePair {
 public:
  PipePair() {
    int fds[2] = {-1, -1};
    if (::pipe2(fds, O_NONBLOCK) == 0) {
      rfd_ = fds[0];
      wfd_ = fds[1];
    }
  }
  ~PipePair() {
    if (wfd_ >= 0) ::close(wfd_);
  }
  PipePair(const PipePair&) = delete;
  PipePair& operator=(const PipePair&) = delete;

  bool ok() const { return rfd_ >= 0 && wfd_ >= 0; }
  // 读端交给 Connection 持有；这里只留写端，析构时写端关闭即可。
  net::UniqueFd take_read_end() { return net::UniqueFd{rfd_}; }

  void write_one_byte(char c) {
    while (true) {
      const ssize_t n = ::write(wfd_, &c, 1);
      if (n == 1) return;
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        std::this_thread::yield();
        continue;
      }
      ADD_FAILURE() << "pipe write failed errno=" << errno;
      return;
    }
  }

  void write_all(std::string_view data) {
    std::size_t off = 0;
    int spins = 0;
    while (off < data.size()) {
      const ssize_t n = ::write(wfd_, data.data() + off, data.size() - off);
      if (n > 0) {
        off += static_cast<std::size_t>(n);
        continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        ASSERT_LT(++spins, 1000000) << "管道始终写不进去";
        std::this_thread::yield();
        continue;
      }
      ADD_FAILURE() << "pipe write failed errno=" << errno;
      return;
    }
  }

 private:
  int rfd_ = -1;
  int wfd_ = -1;
};

}  // namespace l7gate::testing
