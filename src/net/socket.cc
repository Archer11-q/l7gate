#include "net/socket.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace l7gate::net {

namespace {

// ipv4 字面量 → sockaddr_in。返回 false 时 error 已被填成 EAFNOSUPPORT/EINVAL。
bool to_sockaddr(std::string_view ipv4_addr, std::uint16_t port, sockaddr_in* out, int* error) noexcept {
  std::memset(out, 0, sizeof(*out));
  out->sin_family = AF_INET;
  out->sin_port = htons(port);
  char buf[64] = {};
  // string_view 不保证 NUL 结尾，所以必须先拷进定长缓冲再交给 inet_pton；
  // 直接 .data() 传下去会在没有 NUL 时越界读——ASan 抓得到，但没必要留给它抓。
  if (ipv4_addr.size() >= sizeof(buf)) {
    *error = EINVAL;
    return false;
  }
  ipv4_addr.copy(buf, ipv4_addr.size());
  const int rc = ::inet_pton(AF_INET, buf, &out->sin_addr);
  if (rc != 1) {
    *error = (rc == 0) ? EINVAL : errno;
    return false;
  }
  return true;
}

}  // namespace

bool set_nonblocking(int fd, int* error) noexcept {
  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    *error = errno;
    return false;
  }
  if ((flags & O_NONBLOCK) != 0) {
    *error = 0;
    return true;
  }
  if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    *error = errno;
    return false;
  }
  *error = 0;
  return true;
}

bool set_reuse_address(int fd, int* error) noexcept {
  int on = 1;
  if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
    *error = errno;
    return false;
  }
  *error = 0;
  return true;
}

bool set_tcp_nodelay(int fd, int* error) noexcept {
  int on = 1;
  if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) < 0) {
    *error = errno;
    return false;
  }
  *error = 0;
  return true;
}

bool get_so_error(int fd, int* error) noexcept {
  int so_error = 0;
  socklen_t len = sizeof(so_error);
  if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0) {
    *error = errno;
    return false;
  }
  *error = so_error;
  return true;
}

int get_socket_port(int fd) noexcept {
  sockaddr_in addr = {};
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
    return -1;
  }
  return static_cast<int>(ntohs(addr.sin_port));
}

SocketResult make_listen_socket(std::string_view ipv4_addr, std::uint16_t port, int backlog) noexcept {
  SocketResult result;
  sockaddr_in addr = {};
  int error = 0;
  if (!to_sockaddr(ipv4_addr, port, &addr, &error)) {
    result.error = error;
    return result;
  }

  // 非阻塞在创建时就带上，不给"创建了但还没设非阻塞"这段窗口留事件循环的口子。
  // SOCK_CLOEXEC 同理：fork+exec 的进程不该继承监听 socket。
  result.fd = UniqueFd{::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
  if (!result.fd.valid()) {
    result.error = errno;
    return result;
  }

  if (!set_reuse_address(result.fd.get(), &error)) {
    result.error = error;
    return result;
  }
  if (::bind(result.fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    result.error = errno;
    return result;
  }
  if (::listen(result.fd.get(), backlog) < 0) {
    result.error = errno;
    return result;
  }
  return result;
}

AcceptOutcome accept_connection(int listen_fd) noexcept {
  AcceptOutcome outcome;
  // 同样是 accept4：拿到手的 fd 一定已经是非阻塞的。
  // 用 ::accept 的话，"忘了设非阻塞"的后果不是报错，是这条连接的 read()
  // 在 ET 下把整个事件循环线程挂住——那属于会伪装成性能问题的死锁。
  const int fd = ::accept4(listen_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
  if (fd < 0) {
    outcome.error = errno;
    return outcome;
  }
  outcome.fd = UniqueFd{fd};
  return outcome;
}

ConnectOutcome start_connect(std::string_view ipv4_addr, std::uint16_t port) noexcept {
  ConnectOutcome outcome;
  sockaddr_in addr = {};
  int error = 0;
  if (!to_sockaddr(ipv4_addr, port, &addr, &error)) {
    outcome.error = error;
    return outcome;
  }

  outcome.fd = UniqueFd{::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
  if (!outcome.fd.valid()) {
    outcome.error = errno;
    return outcome;
  }

  if (::connect(outcome.fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
    outcome.status = ConnectStatus::kConnected;
    outcome.error = 0;
    return outcome;
  }
  const int e = errno;
  if (e == EINPROGRESS) {
    // 不是错误。可写事件到来后仍要用 SO_ERROR 确认，见 get_so_error 的注释。
    outcome.status = ConnectStatus::kInProgress;
    outcome.error = 0;
    return outcome;
  }
  outcome.status = ConnectStatus::kFailed;
  outcome.error = e;
  return outcome;
}

}  // namespace l7gate::net
