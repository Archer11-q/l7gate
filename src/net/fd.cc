#include "net/fd.h"

#include <cerrno>
#include <unistd.h>

namespace l7gate::net {

void UniqueFd::close_now() noexcept {
  if (fd_ != kInvalidFd) {
    // EBADF 在这里被吞掉：它意味着这个编号早已被关过，而我能确定的是
    // "我这次没有再欠一个没关的 fd"。真正要防的泄漏由 LSan 与 /proc/self/fd
    // 计数承担（见 tests/reactor_test.cc 的 TenThousandSequentialRequestsLeaveNothingAlive），
    // 不靠 close() 的返回值当哨兵。
    const int saved_errno = errno;
    (void)::close(fd_);
    errno = saved_errno;
    fd_ = kInvalidFd;
  }
}

UniqueFd::~UniqueFd() { close_now(); }

}  // namespace l7gate::net
