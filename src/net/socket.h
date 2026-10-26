#pragma once

#include <cerrno>
#include <cstdint>
#include <string_view>

#include "net/fd.h"

namespace l7gate::net {

// socket 层的建立与属性设置。这里**不管**读写——读走 `Buffer::read_until_blocked`，
// 写走 `Buffer::write_to_fd`，两边都带 ET 语义。这个文件只负责"把一个 fd 弄成
// 事件循环可以信任的形状"：非阻塞、CLOEXEC、bind/listen 或 connect 已发起。
//
// 约定（和 buffer 一致，且我按这条约定处理所有错误）：
//   - 运行期失败 → 返回码 + errno 原样带出来，不抛异常、不打日志
//   - 构造期不可恢复失败（epoll_create1 之类的内部不变量）→ 抛 std::system_error
//   理由：连接层每秒钟可能撞几十次 EAGAIN，把它做成异常会让"正常"和"坏了"
//   在代码形状上长得一样；而 epoll 创建失败是进程根本不该继续的状态。

struct SocketResult {
  UniqueFd fd;
  int error = 0;  // 0 表示成功
  bool ok() const noexcept { return fd.valid() && error == 0; }
};

struct AcceptOutcome {
  UniqueFd fd;
  int error = 0;
  bool ok() const noexcept { return fd.valid() && error == 0; }
  // 区分"暂时没连接"和"accept 坏了"——ET 下 accept 必须循环到这两个值之一，
  // 和 read 必须循环到 EAGAIN 是同一条语义，只是发生在监听 socket 上。
  bool would_block() const noexcept { return error == EAGAIN || error == EWOULDBLOCK; }
};

enum class ConnectStatus : std::uint8_t {
  kConnected,    // 同机回环上确实可能立刻完成
  kInProgress,   // EINPROGRESS：结果要等可写事件 + SO_ERROR
  kFailed,       // 立刻失败，error 有值
};

struct ConnectOutcome {
  UniqueFd fd;
  ConnectStatus status = ConnectStatus::kFailed;
  int error = 0;
  bool usable() const noexcept { return fd.valid() && status != ConnectStatus::kFailed; }
};

bool set_nonblocking(int fd, int* error) noexcept;
bool set_reuse_address(int fd, int* error) noexcept;
bool set_tcp_nodelay(int fd, int* error) noexcept;

// 异步 connect 的结果要等到可写事件，可写本身不代表成功——必须再问 SO_ERROR。
// 这一层最容易写错的地方就在这：只看 EPOLLOUT 就认为连上了，等于把 RST 当握手。
bool get_so_error(int fd, int* error) noexcept;

// 监听 socket 实际绑到的端口（bind 端口 0 时由内核选，测试需要拿回来）。
// 返回 -1 表示失败。
int get_socket_port(int fd) noexcept;

// AF_INET + SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC + reuseaddr + bind + listen。
// backlog 由调用方给；这里不藏默认值，因为它决定 accept 队列溢出时客户端看到
// 的是 ECONNREFUSED 还是排队，S1-04 的 fd 泄漏测试要能显式控制它。
SocketResult make_listen_socket(std::string_view ipv4_addr, std::uint16_t port, int backlog) noexcept;

AcceptOutcome accept_connection(int listen_fd) noexcept;

// 非阻塞 connect。IPv4 字面量（inet_pton，不做 DNS——DNS 是 S3 路由层的事，
// 而且它会在事件循环线程里阻塞）。
ConnectOutcome start_connect(std::string_view ipv4_addr, std::uint16_t port) noexcept;

}  // namespace l7gate::net
