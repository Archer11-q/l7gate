# ADR-14 · 连接所有权与跨线程传递/异步销毁模型

- 状态：已采纳（S2-01 落地「单向 handoff + 单线程拥有」这一半；跨线程异步销毁的 harder 情形随 S2-02 连接池引入、S2-03 定稿）
- 日期：2026-10-27
- 相关：ADR-03（多线程 Reactor 而非多进程）、ADR-02（epoll ET）、契约 3.1/4.1、`docs/ARCHITECTURE.md` 1.7（六步关闭 + `retiring_`）、`src/net/stats.h`（全原子观测面）

## 我选择什么

**一条连接对象在它整个生命周期里只属于一个 worker 线程（single-threaded ownership）。** 具体形状：

1. **跨线程只传裸 fd，不传连接对象。** 主线程（acceptor）`accept()` 得到一个 fd，把它塞进目标 worker 的 handoff 队列（`std::mutex` 保护），再写该 worker 的 eventfd 把它叫醒。**`Connection` 对象由 owner worker 自己在循环线程里构造**——acceptor 线程从不碰任何 `Connection`，跨线程边界上流动的只有一个 `int`（用 `UniqueFd` 承载，保证没被取走的 fd 在析构时也会关掉）。

2. **owner worker 独占这条连接的所有操作。** 构造、注册进自己的 epoll、读、写、状态迁移、六步关闭、`retiring_` 批末析构——全部在 owner 线程。`connections_` / `retiring_` / `pending_probes_` / `epoll_` 这些**非原子的循环内状态因此天然无竞争**：它们每个 worker 各一份，只被该 worker 的循环线程碰。

3. **派发策略是 round-robin。** acceptor 维护一个原子计数器，第 k 条 accept 到的连接交给 `worker[k mod N]`。选它的理由和 filter 链不排 priority 同源：它确定性、可断言（M 条连接派发到 N worker，每 worker 接到数落在 ⌊M/N⌋…⌈M/N⌉），而"最少连接"需要跨 worker 读实时连接数（那是共享状态 + S3 负载均衡的活）。

4. **`ReactorStats` 全 worker 共享一份。** 它全是 `std::atomic<uint64_t>`（`stats.h` 从 S1 起就这么写，注释明说是"给 S2 跨线程共享留的基线"），多个 worker 线程并发 `fetch_add` 安全、聚合计数自动成立。而 `Epoll` 的三个计数器与注册表观测（`live_connection_count()` / `armed_events()` / `connections_with_read_disarmed()`）是**非原子的、每 worker 各一份**，按 `stats.h` 第 25 行那条纪律只能在对应 worker 的循环线程 join 之后读。

5. **handoff 之后补一次探测读（probe）。** ET 只为"注册之后发生的电平跳变"负责：fd 在 acceptor 线程 accept、排队、worker 取出、注册进 epoll 这段窗口里，客户端可能已经把请求写进了内核接收缓冲，而注册那一刻没有新跳变，epoll 不投递事件，这条连接就会睡死到客户端再写一次为止。所以 worker 注册完一条 handoff 连接后立刻 `schedule_probe()`——复用的正是 S1-07 背压恢复那套「挂回 EPOLLIN 后主动读一次」的机器（`schedule_probe`/`drain_probes`/`probe_read`），不另写一条路径。单 Reactor 的 `accept_all()` 不补这次探测：它 accept 与注册在同一线程紧邻发生，窗口小到 S1 全套测试从未撞到；handoff 把窗口拉大到跨线程排队，探测因此是必需的而不是可选的。

## 为什么

**决定性理由是它把 TSan 门禁（契约 4.1，S2 唯一核心门禁）变成一个可证伪的命题，而不是一句自述。** ADR-03 选了多线程 Reactor，代价是并发正确性，而对账手段就是 TSan。要让"TSan 干净"有意义，就得让"哪些状态是共享的"这件事**在结构上最小化**：

- 如果连接对象在 acceptor 线程构造、再交给 worker，那么构造（写 `stats_`、分配 buffer）与后续操作分属两个线程，需要 mutex/eventfd 建立 happens-before 才不算 race——能做对，但每一处都多一个"忘了同步"的失败面。只传 fd 把这个面收到零：**对象从生到死只被一个线程碰过**，TSan 对连接对象内部状态根本无从报起，因为它压根没有第二个访问者。
- 循环内那几坨非原子状态（注册表、retiring、探测队列）是 S1 里最容易出"合法但错的人"那类 bug 的地方（见 ARCHITECTURE 1.7 的 fd 复用防护）。让每个 worker 各持一份、互不共享，就把"跨线程访问注册表"这一整类 race 从设计里删掉了，而不是靠加锁去堵。
- 共享 `ReactorStats` 而不是每 worker 各一份再求和：求和要在 30+ 个计数器上逐项写、迟早漏一项或写错口径（这正是 S1「同一口径有两个抄本就会漂移」那条教训的形态）。全原子共享一份，聚合是免费的、且没有第二份真相。

**round-robin 而不是最少连接**：见上，最少连接要跨 worker 读实时连接数，那是共享状态，违背本 ADR 把共享面收到最小的初衷；负载均衡是 S3 的显式任务，届时再按 ADR 引入。

## 我放弃什么

- **放弃了「连接对象可以在 worker 之间迁移」的灵活性。** 一旦一条连接归了 worker i，它终身不迁到 worker j。代价是：如果派发不均衡（某 worker 的连接都是长连接、另一个都是短连接），负载会倾斜。我接受它，因为 (a) round-robin 在连接数上均衡，(b) 真正的倾斜治理是 S3 负载均衡 + 可能的连接迁移，那是另一个 ADR，(c) 迁移连接对象 = 把一个非原子的循环内状态跨线程搬，正是本 ADR 要消灭的那类 race。
- **放弃了 acceptor 与 worker 合并成一个线程的可能。** 可以让 worker 0 兼做 accept，但那样 worker 0 的循环里就混进了"派发给别人"的跨线程写，且它自己还要处理连接，职责不清。ADR-03 写的就是"主线程 accept + N 个 worker"，我照做。
- **accept 单点可能是瓶颈**（ADR-03 已列为延迟观察项）：所有连接都过主线程一个 accept 循环。我**不在 S2-01 提前优化**（不做 SO_REUSEPORT 多 acceptor、不做 accept 线程池）——契约 3.2 的延迟决策纪律同样适用于此：等压测（S2-07）拿出"主线程 accept 排队"的证据再立项，届时先写 ADR。

## 后果与约束

- **异步销毁的 harder 情形留给 S2-02/S2-03。** 本 ADR 在 S2-01 兑现的是"连接只被 owner 线程持有"这一半——此时 `shared_ptr<Connection>` 的所有持有者（注册表、`retiring_`、`pending_probes_`）都在同一个 worker 线程内，六步关闭 + 批末析构与 S1 完全一致，析构必然发生在 owner 线程。**S2-02 的上游连接池会打破这个前提**：池是共享状态，一条上游连接可能被非 owner 线程持有 `shared_ptr`（借出/归还/淘汰），那时"最后一个引用在哪个线程释放"就不再确定。S2-03 要正面处理这批竞态并定稿销毁协议（候选形状：跨线程 retire 投递回 owner，保证析构永远在 owner 线程），本 ADR 届时追加那一章。
- **`l7gate_core` 从此链 `Threads::Threads`。** ARCHITECTURE §2 当初刻意不让库本体链线程（"提前链上等于给'我已经线程安全了'发一张空头证明"）——S2-01 就是那个"到期"的时刻：`ReactorGroup` 真的持有 `std::thread`。这不是新依赖（pthread 是平台线程原语，不是第三方库，不触发禁令三），是那条注释预告过的 S2 动作，随本任务同步更新 §2 的措辞。
- **`--serve` 默认仍是单 Reactor（`--workers 1`）。** S1 的三个冒烟脚本断言的是单 Reactor 的精确 epoll 计数（`epoll_registrations=4` = 监听 + eventfd + 1 下游 + 1 上游）。多 Reactor 下 acceptor 与每个 worker 各有自己的 epoll，这些计数会变。为了不作废 S1 的门禁证据，多 Reactor 走显式的 `--workers N`（N≥2）开关，默认路径逐字节不变；S2-07 重测 SLO 时再决定是否翻默认值。
- **可证伪性。** 本模型的失败形态是"某个非 owner 线程碰了连接对象或某 worker 的循环内状态"，而它的检测器就是 TSan：S2-01 的验收要求含多 worker 的套件在 `tsan` preset 下零 data race。如果哪天有人把连接对象塞进跨线程的容器、或让两个 worker 共享一个注册表，TSan 会红——这条 ADR 的正确性由门禁持续复查，不靠我记得。
