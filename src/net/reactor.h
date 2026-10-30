#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "filter/chain.h"
#include "net/connection.h"
#include "net/epoll.h"
#include "net/fd.h"
#include "net/stats.h"
#include "route/balance.h"

namespace l7gate::net {

// 单线程 epoll 边缘触发事件循环。ADR-02 的落地点，S2 要在这之上加线程，
// 所以它的并发前提写得比功能更细。
//
// 它承担三件事，每件都不是风格问题：
//
// **一、延后析构。** 一次 `epoll_wait` 可以在同一批里返回多个事件。如果我在批内
// 就把连接对象析构掉，批里后面那条事件带着的 `Connection*` 就是悬垂指针，而它
// 接下来会被解引用去 close 一个**已经被新连接复用的 fd 编号**。这不是理论风险：
// 10000 请求 × keep-alive 的场景里 fd 编号会被反复回收，命中的概率随负载上升，
// 表现是偶发地把别人的连接关掉——ASan 能抓到 UAF，但抓不到"合法但错的人"。
// 所以关闭被拆成六步（0 出向没刷完就先只挂 EPOLLOUT、不碰 fd → 1 摘 epoll →
// 2 解除对端的反向引用 → 3 出注册表 → 4 close fd → 5 批处理完再析构），
// 再加一道注册表反查作为第二层防护。两层都要，一层防悬垂、一层防张冠李戴。
//
// **二、accept 也要读到 EAGAIN。** 边缘触发不只对读数据成立，对监听 socket 一样：
// 一次 EPOLLIN 可能对应多个已完成的连接，只 accept 一个就把事件用掉了，剩下的
// 要等到下一次事件——而下一次可能永远不来。`Buffer::read_until_blocked` 是同一
// 条语义的数据版本，这里是它的连接版本。
//
// **三、EPOLLET 由封装层强制。** 见 `Epoll::kForcedEvents`。reactor 这里不再关心
// 触发模式，它只决定"关心哪些事件"。
class Reactor {
 public:
  struct Config {
    std::string listen_address = "127.0.0.1";
    // 0 表示由内核选端口；测试全靠它避免端口撞车，所以这个 0 是功能不是便利。
    std::uint16_t listen_port = 0;

    // --- 目标上游表（S3-01，ADR-18）---------------------------------------------
    // 取代 S2 那两个字段（`upstream_address` / `upstream_port`）。**数组下标就是
    // `filter::RequestInfo::upstream_target` 的值域**，所以路由的返回空间和回源的目的
    // 地是同一个索引体系，不需要第二份映射（那又是一份会漂移的抄本）。
    struct Target {
      std::string address = "127.0.0.1";
      std::uint16_t port = 0;
    };
    // 空表 = 没配上游（与 S2 时代 `upstream_port == 0` 同一口径，`ensure_upstream`
    // 那条短路一致）。**没有 filter 表态时一律用 `default_target`**，而 ReactorGroup
    // 给 worker i 的默认目标就是 S2-06a 那个第 `i mod E` 个端点——这就是"没装路由 filter
    // 时行为逐字节等于 S2"的落点，容量公式 `min(N,E)×28,232` 与八份 canonic 档因此不作废。
    std::vector<Target> upstream_targets;
    std::uint16_t default_target = 0;

    // S3-02（ADR-19）：**本 worker 的均衡状态**（轮询游标 + 每后端配对数）。
    // nullptr = 这个 reactor 不跑负载均衡（今天所有既有用例与压测都是这一格）。
    // `shared_ptr` 不表示跨线程共享：持有者是 reactor 的记账路径与 `LoadBalanceFilter`，
    // 两者都在同一个 worker 的循环线程里跑（ADR-14 的红线；理由见 ADR-19「为什么」第一条）。
    std::shared_ptr<l7gate::route::LbState> lb_state;

    int listen_backlog = 1024;
    // fd 预算。超过就直接把新连接关掉而不是排队：一个网关在 fd 耗尽时的正确
    // 行为是拒绝，不是"再多接一个然后 crash"。这个数字也是 S2 的 100k 场景里
    // 唯一需要提前想清楚的闸门（rlimit 侧的 NoFile 检查归 S2）。
    std::size_t max_connections = 4096;

    // S2-02（ADR-15）：本 worker 的上游连接池里最多留多少条**空闲**热上游。
    // 归还时池已满就直接关掉而不是放回——这是每连接内存（SLO #2）的硬上界。
    // **S3-01（ADR-15 追加节）改了这条上界的分母**：池按目标分桶之后，它是
    // **每 worker 每目标**的上界，所以上界从 `pool_max_idle` 抬到
    // `目标数 E × pool_max_idle`（缺省 64、E=4 → 每 worker 最多 256 条空闲上游）。
    // 这一格要进 SLO #2 的账，不许只写在配置注释里。100k 隧道那一档不受影响
    // （隧道态 1:1 专用、不入池，B-011 的"池摊销 = 0"仍然成立）。
    // 它和 max_connections 的关系：在飞配对的上游 + 池里空闲的上游 + 下游都算进
    // connections_，所以 `E × pool_max_idle` 要显著小于 max_connections，
    // 否则池会把 fd 预算吃光、导致下游在 ensure_upstream 那里被拒。
    std::size_t pool_max_idle = 64;

    Connection::Config connection;

    // > 0 时：解析满这么多个请求就自动 stop()。压测和验收需要"跑固定轮数后
    // 自己结束"，而不是靠外部信号——外部 SIGTERM 的送达时机本身不是可控量。
    std::size_t stop_after_requests = 0;

    // epoll_wait 的超时。它决定 stop() 之外还能靠什么结束循环；200ms 是
    // "测试里最坏等 200ms、生产里每 200ms 醒一次检查停止条件"的折中。
    // 将来挂起的 timer/空闲回收会改这个值（S4 的超时归 S4）。
    int wait_timeout_ms = 200;
  };

  explicit Reactor(Config config);
  // worker 模式构造（S2-01，ADR-14）：多个 Reactor 共享**同一份** ReactorStats。
  // 它全原子，所以并发 fetch_add 安全、聚合计数自动成立（stats.h 从 S1 起就是
  // 为这一刻写的）。用这个构造的 Reactor 不拥有 stats，生命周期由调用方
  // （ReactorGroup）保证长于所有 worker。
  Reactor(Config config, ReactorStats& shared_stats);
  ~Reactor();

  Reactor(const Reactor&) = delete;
  Reactor& operator=(const Reactor&) = delete;

  // 绑定监听 socket 并注册进 epoll。失败返回 false，errno 由 *error 带出。
  // 单 Reactor 模式（S1）用它：这个 Reactor 自己 accept、自己拥有连接。
  bool start_listen(int* error) noexcept;

  // worker 模式（S2-01，ADR-14）：**不**绑定监听 socket，只注册自唤醒 eventfd。
  // 连接由 acceptor 线程经 handoff() 送进来，owner worker 在自己的循环里构造。
  // 失败返回 false，errno 由 *error 带出。
  bool start_worker(int* error) noexcept;

  // acceptor 线程调用（跨线程）：把一个刚 accept 到的 fd 交给本 worker。
  // 只做两件跨线程的事——塞进 mutex 保护的队列、写 eventfd 叫醒循环——
  // 和 stop() 同一个纪律：不碰任何循环内部状态。fd 用 UniqueFd 承载，
  // 没被取走就在本对象析构时关掉，不漏。连接对象由 owner worker 构造，
  // acceptor 从不碰 Connection（ADR-14 第 1 条）。
  void handoff(UniqueFd fd) noexcept;

  // 阻塞直到 stop() 被调用（或 stop_after_requests 达成）。
  // 返回本次循环处理的事件批次数；正常退出路径之外没有别的返回值语义。
  //
  // **先调用 stop() 再进 run() 也停得下来**：停止请求记在 stop_requested_ 上，
  // 而不是靠"进循环之前把 running_ 置 true、退出位置 false"那一对动作。
  // 后者会把已经送达的停止请求直接抹掉，表现是循环线程永远不停。
  std::uint64_t run();

  // 可以在别的线程调用（测试就是这么停的），所以它只做一件跨线程的事：
  // 往 eventfd 写一个数。不碰任何循环内部状态 —— 那才是真正会在 S2 出问题的地方。
  //
  // **硬停**：置标志 + 敲门，循环下一轮检查就退出，在飞连接不被收尾（留给 `~Reactor`）。
  // S1/S2-04 的全部生命周期断言走的就是这条路，S2-05 没有改它的语义。
  // 排水请用 request_drain()；在排水期间收到 stop() 就是运维意义上的"别再等了"。
  void stop() noexcept;

  // S2-05（ARCHITECTURE 1.20）：**优雅停止**。与 stop() 的区别是循环不立刻退出——
  // 它进入排水相位：摘监听 fd（新连接从此拿 RST）、不再接受新请求、把已在飞的请求
  // 逐条响应完 + 刷完出向再关、空闲连接立即关、隧道硬关，直到没有值得等的东西
  // 或 timeout_ms 到点（兜底强关）才自己置上停止标志。
  // 跨线程纪律与 stop() 同一条：只做原子 store + 写 eventfd，不碰循环内部状态。
  void request_drain(std::int64_t timeout_ms) noexcept;

  bool running() const noexcept { return running_.load(std::memory_order_relaxed); }
  bool stop_requested() const noexcept { return stop_requested_.load(std::memory_order_relaxed); }
  // 排水是否已请求。**循环线程自己**在排水完成时置 stop_requested_，所以
  // ReactorGroup 用「所有 worker 的 stop_requested() 都为真」判断"全部排干了没有"，
  // 不需要往回投递任何完成事件（也就多一个跨线程接触面都没有）。
  bool drain_requested() const noexcept { return draining_.load(std::memory_order_relaxed); }

  // 实际监听端口（bind 到 0 时由内核决定，start_listen 之后才可读）。
  int listen_port() const noexcept { return listen_port_; }

  ReactorStats& stats() noexcept { return stats_; }
  const ReactorStats& stats() const noexcept { return stats_; }

  // epoll 计数器直接来自 Epoll，不是我自己数的第二份。
  // 这条很重要：验收要断言「注册/注销次数与连接建立/关闭次数逐项相等」，
  // 如果两边都由我手工 ++，那相等就只是我自己写对称的错觉，什么都证明不了。
  std::uint64_t epoll_registrations() const noexcept { return epoll_.registrations(); }
  std::uint64_t epoll_unregistrations() const noexcept { return epoll_.unregistrations(); }
  std::uint64_t epoll_modifications() const noexcept { return epoll_.modifications(); }

  // --- filter 链（S1-08）--------------------------------------------------
  // 只能在 run() 之前添加。链在循环线程里被逐条读，没有任何加锁，运行中改它就是一场
  // data race；S5 要热更新的话，唯一安全的形状是"整条链换一根指针"，不是往 vector 里 push。
  //
  // 转发主路径里只有**一处**链调用（`forward_pending_requests_body` 取到完整请求之后、
  // `forward_request` 之前），所以"新增一个 filter 要改主路径几行代码"这条验收的答案是
  // 0 行。这个 0 不是巧合，是 S1-04 起把策略留在 reactor、把状态留在 Connection 的分工
  // 兑的现（见 net/connection.h 顶部那段"它不管什么"）。
  void add_request_filter(std::unique_ptr<filter::RequestFilter> filter) noexcept;

  std::size_t request_filter_count() const noexcept { return request_filters_.size(); }

  std::size_t live_connection_count() const noexcept { return connections_.size(); }

  // S2-02（ADR-15）：本 worker 的上游连接池。**S3-01（ADR-15 追加节）改了口径**：
  // 从"本 worker 的空闲热上游数"变成"**按目标分桶后各桶之和**"。单目标配置下两者数值
  // 相同（只有一个桶在用），所以历史读数不作废；但任何"池有多满"的比较要说清是哪几个桶。
  // 仍然与 live_connection_count() 同一类：**非原子观测**，只能在循环线程停下之后读
  // （stats.h 第 25 行那条纪律）。
  std::size_t pool_idle_count() const noexcept;

  // 目标表长度。装配方（ReactorGroup / 测试）拿它校验路由规则的下标，
  // 因为越界的 upstream_target 若一路走到这里索引 `upstream_targets`，
  // 那不是"回错一个后端"而是越界读——查在索引处才叫查。
  std::size_t target_count() const noexcept { return config_.upstream_targets.size(); }
  std::uint16_t default_target() const noexcept { return config_.default_target; }

  // 第 t 个目标的地址与端口（越界返回空地址 + 0 端口，仅日志/断言用）。
  Config::Target target_at(std::size_t t) const noexcept;

  // --- S1-07 的「epoll 侧可断言」------------------------------------------
  // 验收句要的是"能观测到网关不再消费下游可读事件"，那话的主语是**注册表里的兴趣
  // 掩码**，不是任何计数器。这两个 getter 给的就是那份掩码的回执（来自
  // Connection::armed_events()，只在 epoll_ctl 成功时写），所以它们和
  // epoll_registrations() 同一类：非原子、只在循环线程停下之后读。
  // 不在注册表里的 fd 返回 0。
  std::uint32_t armed_events(int fd) const noexcept;
  // 当前"kOpen 且 EPOLLIN 已被摘掉"的连接数。背压触发时它 > 0，恢复之后回 0。
  std::size_t connections_with_read_disarmed() const noexcept;

 private:
  void handle_events(const std::vector<Epoll::Event>& events);
  void accept_all();
  void wake_up();
  // worker 模式：把 handoff 队列里的 fd 取出来，逐个构造 Connection 并注册进
  // **自己的** epoll，然后补一次探测读（ADR-14 第 5 条：ET 不为注册前已缓冲的
  // 字节负责）。只在循环线程调用（wake 分支里），所以碰 connections_ 无需加锁；
  // 与 acceptor 线程的唯一接触面是 handoff_fds_ 那个 swap，在 mutex 下完成。
  void drain_handoff();

  // 注册一个连接对象的 fd。events 由调用方给（EPOLLIN / EPOLLOUT 的组合）。
  // 成功时把这份掩码记在连接上（`Connection::set_armed_events`）——它是 S1-07
  // 去重与跳变计数的唯一依据，所以**只在 epoll_ctl 成功的那条分支里写**。
  bool register_connection(Connection& conn, std::uint32_t events) noexcept;
  bool modify_events(Connection& conn, std::uint32_t events) noexcept;

  // --- 背压（S1-07）：谁现在该被读，以及"没人来读"时谁来补一次读 ----------
  // 这里刻意分成两条互不相干的轨道，合在一条上迟早会漏掉另一种形状：
  //   **内存轨道 = 电平**：要不要摘 EPOLLIN 只看这条连接自己的入向可读区相对停读线
  //     的位置（带滞回）。对端出向有多满**不是**判据，理由见 ARCHITECTURE 1.15——
  //     每个方向的可读区都有硬上限，"内存不继续增长"已经由停读线本身担保。
  //   **ET 轨道 = 事件**：这一轮的读有没有停在停读线上（`Connection::take_read_stopped_at_line`）。
  //     停在**线上**就是没读到 EAGAIN，内核里可能还有字节；这时候只要掩码里仍挂着
  //     EPOLLIN（泵把缓冲腾空了 → 水位回落 → 没摘），就必须自己补一次读，因为边沿触发
  //     只为新跳变负责，而"对端写完就等回音"恰恰不会再有跳变。这条轨道不摘任何订阅，
  //     只多花一次 read syscall——它是 S1-02 那条 ET 契约在水位存在之后的收尾。
  bool wants_epollin(const Connection& conn) const noexcept;
  // 唯一的事件掩码策略函数：读兴趣（wants_epollin）+ 写兴趣（有货待发或正在 connect）。
  // 它是"应该订阅什么"，armed_events 是"实际订阅着什么"，两者的差就是该不该打 epoll_ctl。
  std::uint32_t desired_events(const Connection& conn) const noexcept;
  // 把 desired 交给 epoll：相等就不打 syscall（这是每请求 `epoll_ctl` 次数的支出闸门）。
  // EPOLLIN 0→1 时计 resume 并补一次读；1→0 且原因是停读线时计 pause。
  // 两条分支之外还管 ET 轨道：掩码不变或只动 EPOLLOUT 时，欠着的那一次读同样在这里入队。
  void apply_events(const std::shared_ptr<Connection>& conn);
  // 欠读的连接进队列并立刻排空。恢复（挂回 EPOLLIN）和停在停读线上（掩码没变）
  // 都走这一个入口，就是为了"补读"这件事只有一个触发点。
  void schedule_probe(const std::shared_ptr<Connection>& conn);
  // 恢复之后必须**主动读一次**：边沿触发只在电平跳变时报事件，挂回 EPOLLIN 的那一刻
  // 客户端的字节可能早就躺在内核接收缓冲里了，从此不再有跳变，这条连接就永久睡死。
  // 探测用嵌套保护排成队列（probing_），不在事件处理里递归下栈。
  void drain_probes();
  void probe_read(const std::shared_ptr<Connection>& conn);
  // 出向腾出空间之后重试被压住的那条搬运（刷空/刷出一部分都要试，见 flush_outbound）。
  void retry_peer_after_room(const std::shared_ptr<Connection>& conn);

  void on_downstream_readable(const std::shared_ptr<Connection>& conn, std::uint32_t events);
  void on_upstream_event(const std::shared_ptr<Connection>& conn, std::uint32_t events);
  void flush_outbound(const std::shared_ptr<Connection>& conn);

  // 转发策略（S1-04 的部分）：请求头+body 一旦完整，就把它的**原始字节**排进上游
  // 的出向缓冲，然后消费掉下游的这些字节。不改写、不重新分帧——逐字节透传的前提
  // 是"我碰过的字节和我没碰过的字节长得一样"，这条归 S1-06 的验收负责证明。
  // 返回 false 表示这一条没转出去（上游还没连上 / 上游出向满了 / **目标要换但手上还有
  // 在飞的请求**——第三种是 S3-01 新增的，见 `ensure_upstream` 那段"干净边界"）。
  // upgrade / close_after 是 S1-05 的两条连接级意图：从**这条请求自己的**已解析
  // view 判出（调用方必须在 finish_request 之前取用 view，这里是唯一的取用点），
  // 由本函数决定落到哪条连接、何时置 kClosing。判定不碰字节：转发出去的仍是原始请求。
  // `upstream_target` 是链调用点读回来的那个下标（S3-01，ADR-18）：filter 没表态时
  // 它已经被换成 `config_.default_target`，所以本函数以下只认"这就是要去的桶"。
  bool forward_request(const std::shared_ptr<Connection>& downstream,
                       std::size_t request_bytes, bool upgrade, bool close_after,
                       std::uint16_t upstream_target);
  // 响应语义泵（S1-06）：上游入向缓冲里的字节按"当前响应的阶段"过一遍框架判定
  // （http/response_framing 的纯函数），然后透传——头整块排（半个头排出去就是把
  // 畸形喂给客户端），body 按框架边界拆散排（截断的权力在框架知识，不在这条泵）。
  // 它取代了 S1-04/05 的 pump_upstream_to_downstream（整块透传，无框架判定）。
  // 隧道路径不经过这里（ADR-09）。
  void pump_response(const std::shared_ptr<Connection>& upstream);
  // 泵本体。和"重算这条连接的读兴趣"分开写：S1-07 之后每个消费上游入向字节的出口
  // 都要过一次 apply_events，而本体里有七八个 return 点——在每个点上补一句
  // 就是第七份"迟早漏一处"的抄本，所以收在外壳里做一次。
  void pump_response_body(const std::shared_ptr<Connection>& upstream);

  // reject → 4xx（S1-06）：把 gateway_error_reply(status) 排进下游出向，然后
  // **不对称**关闭配对——上游 kDiscardOutbound（它接到的可能是半条请求，没有
  // 值得发的货），下游 kFlushThenClose（4xx 还躺在它的出向里）。先手工把两侧
  // 的 peer_fd_ 解开，否则 close_connection 的级联会把同一个 why 传染给对端，
  // 下游的 4xx 就跟着被丢掉了。
  void fail_pair_with_reply(const std::shared_ptr<Connection>& downstream,
                            std::uint16_t status);

  // 隧道泵（S1-05）：不解析、不改写、不重新分帧，把 src 入向缓冲里的全部可读
  // 字节整块搬进 dst 的出向缓冲。装不下（出向水位）就一个字节都不搬——和请求侧
  // "整块不转"同一条背压方向。S1-06 之后响应侧有了框架判定，这条路依然原样保留：
  // 隧道里的字节永远不该被任何 HTTP 语义过一遍（ADR-09）。
  void pump_tunnel(const std::shared_ptr<Connection>& src,
                   const std::shared_ptr<Connection>& dst);
  // 本体 + 外壳的同一形状（见 pump_response_body 那条注释）：外壳负责在搬完之后
  // 重算 src 的读兴趣——搬成功要算（可能回落了），搬不动也要算（这一轮的读
  // 可能刚好把可读区顶到停读线）。
  void pump_tunnel_body(const std::shared_ptr<Connection>& src,
                        const std::shared_ptr<Connection>& dst);

  // 上游可读之后调用：若下游配对连接带着 upgrade 意图、且入向开头是一个完整的
  // 101 响应头，则把这**恰好 len 字节**的响应头原样排进下游（它是握手的一部分，
  // 之后才轮到帧），两侧进入隧道态，释放下游的 ParseScratch（release_scratch()
  // 自 S1-04 落地以来的第一个调用点），并计 upgrades_101。返回是否进入隧道。
  // 非 101 / 头未完整：什么都不改，正常响应路径继续。
  bool maybe_enter_tunnel(const std::shared_ptr<Connection>& upstream);

  // 把这条下游连接上"已经完整、但还没转出去"的请求一路转干净。
  enum class ForwardOutcome : std::uint8_t {
    kDrained,   // 现在没有可转发的完整请求了（要么全转走，要么剩的是半包）
    kBlocked,   // 有完整请求但暂时转不出去：连接不能关，等有可写事件再来
    kFailed,    // 这一轮里判定要关闭（解析错误或 EOF 前的残包），连接已被本函数关掉
  };
  ForwardOutcome forward_pending_requests(const std::shared_ptr<Connection>& downstream);
  // 本体（真正找请求、排字节、消费的那圈循环）。和"重算这条连接的读兴趣"分开写，
  // 理由同 pump_response_body：本体有七个 return 点，逐个补一句等于第七份抄本。
  ForwardOutcome forward_pending_requests_body(const std::shared_ptr<Connection>& downstream);

  // 确保这条下游连接已经有配对的上游，而且它配的是 `upstream_target` 那一个目标。
  // 没有就**先从该目标的桶里借**一条热上游（ADR-15 + 它的 S3-01 追加节），桶空才发起
  // 异步 connect 新建并配对。借出的上游在这条下游的整个存活期与它 1:1 配对（S1 的
  // keep-alive 复用不变）；下游关闭时上游按它自己那个目标归还进桶，而不是级联销毁。
  //
  // **目标变了而手上还有在飞的请求时不换**——返回那条旧目标的上游，由 `forward_request`
  // 认出"这不是我要的桶"、把这条请求压回下游缓冲（ADR-18 第五节：换了就把 ADR-16 的
  // 响应 FIFO 打乱，那不是我给路由买的单）。被压住的请求由 `pump_response` 的
  // `kComplete` 分支在在飞归零那一刻重新驱动；**少那一次重驱动就是一条挂死的环**。
  // 返回 nullptr 表示暂时还没有（正在 connect）或已达 fd 预算上限。
  std::shared_ptr<Connection> ensure_upstream(const std::shared_ptr<Connection>& downstream,
                                              std::uint16_t upstream_target);

  // --- 上游连接池（S2-02，ADR-15；分桶见 ADR-15 追加节）：借 / 还 / 淘汰 ---------
  // 池是**每 worker 一份**、只被本循环线程碰的按目标分桶的空闲热上游表，所以全程无锁
  // （ADR-14 的单线程拥有原样成立，这也是 ADR-15 选 per-worker 而不是 shared 的决定性理由）。
  // 三条边界必须分清，混一条就是 bug：
  //   借出（take_pooled_upstream）——从**指定那个桶**移除、配对给某条下游；
  //   归还（recycle_upstream）——下游关闭时（或目标切换时），干净的上游解配、复位、
  //      放回它自己那条目标对应的桶；
  //   淘汰（evict_pooled + close）——空闲上游被对端 EOF、或池满、或不干净时关闭。
  // 从 `target` 那个桶里取一条仍可用的空闲热上游（跳过陈旧的）；桶空返回 nullptr。计 pool_borrows。
  std::shared_ptr<Connection> take_pooled_upstream(std::uint16_t target);
  // 这条上游现在能不能安全复用：kOpen、非隧道、非一次性配对、出向空、没有在飞请求、
  // 响应跟踪停在干净边界（上一条响应已完整完结并复位）。任一不满足就淘汰而不是归还。
  // **S3-01 起它是两个问题的同一个判据**：能不能进池，和**能不能换目标**（ADR-18 第五节）。
  // 我刻意不另写第二个条件——"能进池却换不了目标"那种状态没人能解释。
  bool upstream_reusable(const Connection& upstream) const noexcept;
  // 下游关闭时（或干净边界上换目标时）调用：可复用且**那个目标的桶**未满 → 解配、复位、
  // 重新只挂 EPOLLIN（监控对端 EOF）、放回它自己那条目标对应的桶，计 pool_returns；
  // 桶满 → 关闭并计 pool_evictions_full（`pool_max_idle` 现在是**每 worker 每目标**的上界）。
  void recycle_upstream(const std::shared_ptr<Connection>& upstream);
  // 把这条上游从它那条目标的空闲桶里摘掉（不在桶里是 no-op），返回它是否确实在桶里。
  bool evict_pooled(const std::shared_ptr<Connection>& upstream);
  // 循环退出时排空所有桶：停掉的 worker 不该留着热连接。关闭全部空闲上游并清表。
  void drain_upstream_pool();

  // 某个目标对应的空闲桶；**越界返回 nullptr**。所有"拿 filter 写回来的下标去索引数组"
  // 的动作都必须走这一道，因为那件事的失败形态不是"回错一个后端"而是越界读。
  // 走这一道的第二个理由是口径：目标合法性只在两个地方判——装配方（规则下标 vs 表长度）
  // 和这里（索引前），而不是散在借、还、淘汰各写一遍。
  std::vector<std::shared_ptr<Connection>>* pool_bucket(std::uint16_t target) noexcept;
  const std::vector<std::shared_ptr<Connection>>* pool_bucket(std::uint16_t target) const noexcept;

  // --- 负载均衡的配对账（S3-02，ADR-19 第四条）--------------------------------------
  // 尺子是"当前被配给下游的上游连接条数"，不是"这个后端有多少条连接"——池里空闲的那条
  // 不算负载（它闲着的，这正是 least_conn 想看见的事）。
  // **只有两个 dissolution 入口会调 unpair**（`close_connection` 的两条互斥分支、
  // `ensure_upstream` 的目标切换），加一个 pair 入口（`ensure_upstream` 借出/新建）。
  // 记账点少是刻意的：这类计数一旦有三个写入点，就有一个"少减一次"的分支在等着。
  // `config_.lb_state` 为空时这两个函数直接返回——所有既有单 Reactor 路径与压测都不为账付费。
  void note_upstream_paired(std::uint16_t target) noexcept;
  void note_upstream_unpaired(std::uint16_t target) noexcept;

  // --- 优雅停止：排水（S2-05，ARCHITECTURE 1.20）---------------------------
  // 两条都只在**循环线程**上跑，碰的全是循环内状态（注册表 / epoll / 出向缓冲），
  // 所以它们与 ADR-14 的单线程拥有零冲突；唯一的跨线程接触面还是那两个原子量 +
  // 一次 eventfd 写。每轮排水 tick 的顺序固定为：扫描 → 判完成。
  // 「不再接受新连接」不在这里，它在 accept_all()：排水期间 accept 到就立刻把那条 fd
  // 关掉，而**不关监听 fd**——理由与代价都写在 accept_all 的注释里（一句话版本：
  // `fd_start == fd_end` 是全项目生命周期证据的地基，而 fd_start 在 start_listen 之后量）。
  // 逐条分流。快照一份 shared_ptr 再处理——注册表会在处理过程中被 erase
  // （六步关闭第 3 步），拿着迭代器关连接是给自己埋"边遍历边删"那一类雷。
  void drain_sweep();
  // 还有没有值得等的东西：任何一条连接出向没刷完、任何一条下游还 kOpen、
  // 任何一条配对中的上游还有在飞请求 → 还没排完。池里的空闲上游**不算**
  // （它由循环出口的 drain_upstream_pool() 收，与 S1/S2-02 的既有收尾路径同一条）。
  bool drain_pending_empty() const noexcept;

  // 关闭的**前置判断**：出向缓冲里还有没刷出去的字节时不能直接 close(fd)。
  // 那不是"少发一个响应"的语义问题，是我自己把已经接受的字节丢掉了：
  // 上游回完就断（客户端提前断开、S1-06 的截断判定）都会走到这条路径，
  // 而响应此刻正躺在下游的出向缓冲里。所以关连接分成两种意图：
  //   kFlushThenClose  —— 先挂上 EPOLLOUT 把货发完，由 flush_outbound 收尾（默认）
  //   kDiscardOutbound —— 写已经失败了（EPIPE/真 errno），留着也不会出去，立刻关
  // 只有第二种情况会看到 outbound_pending()>0 还继续关，所以"决定要关却没有
  // EPOLLOUT 可等"的状态不可能出现——那条漏掉的 EPOLLOUT 就是永久挂住的 fd。
  enum class CloseWhy : std::uint8_t { kFlushThenClose, kDiscardOutbound };

  // 六步里的第 0 到第 4 步在这里；第 5 步（析构）由 retiring_ 在批处理结束时兑现。
  void close_connection(const std::shared_ptr<Connection>& conn,
                        CloseWhy why = CloseWhy::kFlushThenClose) noexcept;
  void retire_closed() noexcept;

  // 有字节要发就得挂上 EPOLLOUT，包括已经决定关闭（kClosing）的连接 ——
  // 那里恰恰是最容易漏的一处：状态不是 kOpen 就跳过挂事件，等于把
  // "关之前先刷完"这条路自己堵死，货发不出去、fd 也关不掉。
  void arm_write(const std::shared_ptr<Connection>& conn) noexcept;

  Config config_;
  // worker 模式（S2-01，ADR-14）下多个 Reactor 共享一份 ReactorStats；单 Reactor
  // 模式自己拥有一份。owned_stats_ 非空即"自持"，stats_ 恒绑定到生效的那一份
  // （*owned_stats_ 或外部共享的）。**声明顺序要紧**：owned_stats_ 必须在 stats_
  // 之前，因为 stats_(*owned_stats_) 要解引用它，成员按声明序初始化。
  std::unique_ptr<ReactorStats> owned_stats_;
  ReactorStats& stats_;
  // 请求方向的 filter 链（S1-08）。按值持有而不是 unique_ptr 成员：空链必须是
  // "没有 filter"这个常态，主路径上不该为了它多一次判空分支。
  filter::FilterChain request_filters_;
  Epoll epoll_;
  UniqueFd listener_;
  UniqueFd wake_fd_;
  int listen_port_ = -1;

  std::unordered_map<int, std::shared_ptr<Connection>> connections_;
  std::vector<std::shared_ptr<Connection>> retiring_;

  // 上游连接池（S2-02，ADR-15；S3-01 起按目标分桶，见 ADR-15 追加节）：本 worker 的
  // 空闲热上游，**外层下标 = 目标下标**。它们**同时**还在 connections_ 里（仍注册在本
  // worker 的 epoll 上、监控对端 EOF），每个桶只是"这条目标的哪些上游是空闲可借"的子集。
  // 桶数组在构造时按目标表长度定长（运行期不 resize——变长是 S5 热重载的话题，那时按
  // ADR-10 换成"整张表换一根指针"）。每 worker 一份、只被循环线程碰，无锁。
  std::vector<std::vector<std::shared_ptr<Connection>>> upstream_pool_;

  // 待探测读的连接（S1-07 的恢复路径）。持 shared_ptr 和 retiring_ 同一个理由：
  // 排队期间这条连接可能正好被关掉，出队时必须靠注册表反查认人，不能信手里的裸指针。
  // probing_ 是嵌套保护：探测读本身会搬运、搬运会消费字节、消费会再次触发恢复判定，
  // 每次都直接递归下去的话栈深就由"缓冲腾出多少空间"决定——那是个我控制不了的量。
  // 排空循环把它压成一条队列，栈深恒为 1。
  std::vector<std::shared_ptr<Connection>> pending_probes_;
  bool probing_ = false;

  // 出向缓冲刷完之前，这条 fd 一直带着 EPOLLOUT。放在 map 外面单独记没有意义，
  // 状态就在连接对象自己身上（state + outbound 是否为空），这里不重复。
  std::atomic<bool> running_{false};
  // 停止请求独立于"循环在不在跑"。两个状态合成一个的原因我踩过一次：
  // 用 running_ 同时表达"我在循环里"和"我还没被要求停"，那么 run() 开头的
  // store(true) 就会把在那之前送达的 stop() 抹掉，循环再也不停。
  std::atomic<bool> stop_requested_{false};
  // --- S2-05 排水的两个跨线程原子量 -----------------------------------------
  // `draining_` 由 request_drain() 置位（信号 handler / ReactorGroup 的 accept 线程），
  // 循环线程每轮读一次；`drain_deadline_ms_` 是**单调时钟**上的毫秒数（0 = 未设），
  // 置位和读取都走原子，所以它不构成新的 data race（TSan 在这一层是门禁）。
  // 为什么 deadline 用"绝对时刻"而不是"剩余毫秒"：剩余量得有人每轮递减，而递减的人
  // 是循环线程 —— 那等于把兜底计时器做成"扫描频率的函数"，wait_timeout 一改它就变形。
  std::atomic<bool> draining_{false};
  std::atomic<std::int64_t> drain_deadline_ms_{0};
  // 第一次扫描要取一份"停止那一刻手上有什么"的快照（drain_in_flight_at_entry）。
  // 只被循环线程碰，所以是普通 bool，不进原子面。
  bool drain_first_sweep_done_ = false;
  std::vector<Epoll::Event> events_;

  // --- worker 模式的 handoff 队列（S2-01，ADR-14）--------------------------
  // 这是本类**唯一**的跨线程接触面：acceptor 线程 handoff() 里 lock+push+写 eventfd，
  // 循环线程 drain_handoff() 里 lock+swap 取走。除此之外 connections_/retiring_/
  // pending_probes_/epoll_ 全部只被 owner worker 的循环线程碰，天然无竞争。
  // 用 UniqueFd 承载：没被取走的 fd（例如 stop 之后残留的）在析构时自动关掉，不漏。
  std::mutex handoff_mutex_;
  std::vector<UniqueFd> handoff_fds_;
};

}  // namespace l7gate::net
