#include "net/epoll.h"

#include <sys/epoll.h>

#include <cerrno>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>

namespace l7gate::net {

namespace {
constexpr int kMaxEventsPerWait = 256;
}

Epoll::Epoll() {
  const int fd = ::epoll_create1(EPOLL_CLOEXEC);
  if (fd < 0) {
    throw std::system_error(errno, std::system_category(), "epoll_create1");
  }
  fd_ = UniqueFd{fd};
}

Epoll::~Epoll() = default;

bool Epoll::add(int fd, std::uint32_t events, void* user_data, int* error) noexcept {
  epoll_event ev = {};
  ev.events = events | kForcedEvents;
  ev.data.ptr = user_data;
  if (::epoll_ctl(fd_.get(), EPOLL_CTL_ADD, fd, &ev) != 0) {
    *error = errno;
    return false;
  }
  ++registrations_;
  *error = 0;
  return true;
}

bool Epoll::modify(int fd, std::uint32_t events, void* user_data, int* error) noexcept {
  epoll_event ev = {};
  ev.events = events | kForcedEvents;
  ev.data.ptr = user_data;
  if (::epoll_ctl(fd_.get(), EPOLL_CTL_MOD, fd, &ev) != 0) {
    *error = errno;
    return false;
  }
  ++modifications_;
  *error = 0;
  return true;
}

bool Epoll::remove(int fd, int* error) noexcept {
  // epoll_ctl 的 event 参数在 DEL 时内核不读，但 glibc 的签名要指针，
  // 且某些旧内核传 nullptr 会 EINVAL——传一个哑结构体最省事。
  epoll_event ev = {};
  if (::epoll_ctl(fd_.get(), EPOLL_CTL_DEL, fd, &ev) != 0) {
    *error = errno;
    return false;
  }
  ++unregistrations_;
  *error = 0;
  return true;
}

int Epoll::wait(int timeout_ms, std::vector<Event>* out, int* error) noexcept {
  out->clear();
  for (;;) {
    // 内核写的是 epoll_event（data 是 union），这里直接写在 raw_events_ 上再转，
    // 而不是把 Event 硬塞给内核：Event 里多了 fd 字段，布局不同，
    // 拿 reinterpret_cast 赌布局一致是这类代码最不该犯的错。
    epoll_event ready[kMaxEventsPerWait] = {};
    const int n = ::epoll_wait(fd_.get(), ready, kMaxEventsPerWait, timeout_ms);
    ++wait_calls_;
    if (n < 0) {
      if (errno == EINTR) {
        continue;  // 被打断不是失败，见头文件
      }
      *error = errno;
      return -1;
    }
    *error = 0;
    if (n == 0) {
      return 0;
    }
    out->reserve(out->size() + static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
      Event e;
      e.events = ready[static_cast<std::size_t>(i)].events;
      e.user_data = ready[static_cast<std::size_t>(i)].data.ptr;
      out->push_back(e);
    }
    events_delivered_ += static_cast<std::size_t>(n);
    return n;
  }
}

}  // namespace l7gate::net
