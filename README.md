# l7gate

单机部署的 L7 流量治理网关：解析 HTTP/1.1，按策略路由、限流、熔断后转发上游，并对整个过程给出**可复现**的性能与故障边界数据。

不是"又一个高性能 Web 服务器"，不是框架，也不是功能清单。对外形态是一个 `docker compose up` 起来、顶替 Nginx 位置的网关。

**S1 完成 · S2 完成（S2-01…S2-07 全 DONE，门禁四件套齐，已 push `47bca67..dcccca5`）** ｜ C++20 ｜ Linux / WSL2 ｜ 单测 137/137（三 preset；asan/tsan 各 2 条 Skipped＝不判） ｜ MIT

---

## 三个数字

项目的全部价值锚点。测量条件写死在 [docs/SLO.md](docs/SLO.md)，未达标照记。

| # | 数字 | 目标 | 当前读数 | 判定 |
|---|---|---|---|---|
| 1 | 转发增量 p99（1 KiB / 1000 并发 / 60 s / 同机回环） | ≤ 1.0 ms | **S2-07 做了第一次线程模型 A/B**（同机交替各 3 轮、只动 `--workers`）：**workers=16 → 4.160 / 4.330 / 6.290 ms（组内极差 2.130）**；**workers=1 → 32.990 / 33.050 / 35.210 ms（极差 2.220）**；两条系列**不重叠**（间隔 26.700 ms）→ **多 Reactor 确实把增量从 30–38 ms 带打到 4–6 ms 带**。此前那八份 canonic 档全部跑在单 Reactor 缺省路径上（脚本从没传过 `--workers`），所以这一格到今天才被测过（B-008） | ❌ 未达标（超目标 4.2–6.3 倍；剩下的是每请求 ≈8.7 µs 的净加工成本 × 1000 并发） |
| 1b | 转发增量 p999 | ≤ 5.0 ms | **SLO 写死的 1000 并发那一档：取不到**（B-009）。手段已到位（`s1-p999.sh` + `l7gate_c100k --requests-per-conn` 的 HDR 分位）并第一次算出增量（32.192 ms），但客户端自报 **0.99 / 1.00 核**（两臂都贴着单核上限），而同一份分布的 p99 增量与 wrk 那条线差 **6 倍** → 仪器项主导。非标准的 100 并发归因实验出数：增量 p999 = 5.420 ms（w1）/ 3.764 ms（w16），只作方向 | ⚪ **明确记"取不到"**，既不判达标也不判未达标（解锁需多线程观察者 / S4 绑核方案 / perf 三选一，已记债） |
| 2 | 100k 长连接每**连接对**内存 + 空闲 CPU | ≤ 20 KB / < 5% | **100,000 连接对稳态实测：17.47 KiB/对、空闲 CPU 占单核 0.22%（占整机 16 核 0.01%）、稳态窗口散布 0 KiB**（`established=100000/100000`、`upstream_connect_failed=0`、每端点实测 25,000 条、`constructed==destructed==200000`、`fd 40→40`）。**S2-07 补上构成表**（B-011）：入向存储 8,192 + 出向存储 8,192 + `Connection`×2 1,056 + 出向 `Buffer` 对象×2 176 = **可点名 17,616 B/对**，控制块/注册表节点/取整 **245 B**，进程内堆实测 **17,861 B/对** 与生产 RSS 商 **17,889 B/对 差 0.16%**；scratch 隧道化后 0、池摊销 0；**S1-07 那 0.50 KiB 未归因差额结掉**（N=128 处 VmRSS 的仪器粒度：同码 N=128 与 N=1024 的每连接堆字节差 61 B，而那段 VmRSS +0 KiB） | ✅ 达标（B-007 + B-011） |
| 3 | QPS ≥ Nginx 同环境实测值的 60% | ≥ 60% | **206.11% / 203.74%**（两轮复现，差 1.2%；第二轮 nginx 原始态 9,184.79 / l7gate 18,713.55） | ⏳ 有读数，**终判推到 S5**（已裁） |

三格非绿图标（⚪ 与 ⏳）的含义各不相同，我不想被读成同一个意思：#1b 是**工具取不到那个分位**——已裁决在自研 C100K 客户端里做 HDR 分位，不引第三方工具；**#2 已经从"测量条件不齐"变成 ✅**——原先写的"`ulimit -n`=10240 是硬前置"于 2026-10-27 实测推翻（soft 可自抬到 hard=1048576），真前置是回源那一跳的单元组容量只有 28,232，解开它的是 S2-06a（端点列表 + per-worker 静态映射，容量 = `min(N,E) × 28,232`），B-007 因此有了第一份达标的 100k 读数，见 `ENVIRONMENT.md` 第 7 节；#3 是**数拿到了，但按裁决把终判推到 S5**——nginx 在容器里只能走端口发布，分母被压到同一上游天花板（56,332.52 QPS）的 16%，比值两轮复现是 206.11% 与 203.74%（差 1.2%，见 B-005 附），契约要求的「同一分配器记录」也没满足；更要紧的是 S1 的单 Reactor 对 16 worker 的 nginx，**赢的是对照组而不是实现**。**拿一个被削弱的分母给自己发达标证明，正是 SLO 里那句"最容易造假的点在于和谁比"。**

第三个数还有附加产出：与 Nginx 的差值必须定位到 HTTP 解析、内存分配、连接管理、日志写盘、系统调用次数中的哪几项。「差多少」谁都能测，**「差在哪」才是这个项目**。

> 证据：[docs/BENCHMARK.md](docs/BENCHMARK.md) B-001…B-006（含每轮环境快照、原始 wrk/hey 输出、#3 第二轮复现的 B-005 附、SLO #2 第一份取数与它的失真归因 B-006、夹具改动后的同参复跑 B-006 附、口径变更节）。

---

## 目录

- [现在能做什么](#现在能做什么)
- [阶段进度](#阶段进度)
- [架构与数据流](#架构与数据流)
- [快速开始](#快速开始)
- [明确不做的东西](#明确不做的东西)
- [可观测面与调试界面（规划中，未实现）](#可观测面与调试界面规划中未实现)
- [文档索引](#文档索引)

---

## 现在能做什么

`l7gate --serve --listen <ip:port> (--upstream <ip:port>[,<ip:port>...] | --config <path>) [--workers N] [--serve-requests N] [--max-connections N] [--high-water-mark BYTES] [--drain-timeout-ms MS]` 起事件循环（`--workers 1` 是缺省的单线程 epoll ET；`--workers N≥2` 起 N 条 worker 循环线程，主线程 accept 后 round-robin 派发，连接对象单线程拥有，ADR-03/ADR-14）。**`--upstream` 与 `--config` 是二选一**（ADR-19 第一节：后端表有两个来源时，"哪一份是真相"本身就是一份会漂移的配置），两个都给、或两个都不给，都是退出码 2。已经能：

- 把下游解析出的完整请求按**原始字节**搬到上游，响应原样搬回，keep-alive 复用同一条回源连接
- 认得一条响应在哪里结束：七种框架结论 + chunked 边界游标（只数边界、不重组 body）
- 在头没出去时自产 502，头已出去时只以连接终止表态——**绝不给同一条请求发两条响应**
- 双条件命中（`Connection` 含 `upgrade` 记号 **且** 有 `Upgrade` 头）后整条连接转纯字节隧道：帧不解析、不改写（ADR-09）
- 撞停读线就摘掉这条 fd 的 `EPOLLIN`，水位回落时带一次主动探测读挂回来（边沿触发不为已经在里的字节负责）
- 按配置顺序过 filter 链；**S3-01 起链上有第一个业务 filter：路由**（前缀取最长命中、正则全串锚定且优先于前缀，无匹配回 404 且**不回源**）。决策是纯函数（`src/route/`，不碰 fd），目的地通过 `RequestInfo` 的写回通道交给回源路径
- **S3-02 起链上是两环**：`RouteFilter` 决定"这条请求属于**哪一组**后端"，`LoadBalanceFilter` 决定"组里这次给**哪一个**"（轮询 / 最少连接 / 一致性哈希）。分成两环不是为了好看：路由是纯函数、均衡天生有状态，合成一环就再也无法在不建 socket 的文件里测规则语义，而 S3-03 的健康检查也没有地方插（ADR-19 第三条）。**均衡状态是每 worker 一份**（游标与配对计数都是普通整数，只被该 worker 的循环线程碰），所以 ADR-14 的跨线程接触面一个字没扩大，`tsan` 零 data race 就是它的可证伪形式
- 后端表与路由表从**一份 JSON 子集文件**读进来（`--config`）。解析器在本仓库里手写、不引第三方库，为的是"读不出就整体失败"这件事有地方表达：未知字段、重复键、尾逗号、注释、浮点、`balance` 拼错、空 `targets`、vnodes 越界、正则编不过、路由指向不存在的组——**每一条都是拒绝**，退出码 2 且话里点名是哪一项，绝不"退回默认配置继续跑"
- **S3-03 起组里的成员是"健康"的**：被动记账（连不上 / 头坏 / 5xx 记失败、`<500` 完整头记活着）+ **主动探活**（每目标可配 `health_path`，2xx/3xx/4xx 活、5xx/连接失败/超时死，探活连接归 owner worker 的事件循环、**不进业务计数**）。连续 `max_fails` 次失败摘除 `fail_timeout`，探活成功立即恢复；**最多摘除比例安全阀**（`max_ejection_percent`，默认 10%）保证判据失准时最坏只损失 10% 容量，被阀挡下的每一次都有计数
- **S3-04 起成员表可以来自 MySQL**：组声明 `"source": "mysql"`（与 `targets` 互斥），成员从 `l7gate_nodes` 表按 `--nodes-reload-ms` 周期 reload——**策略在文件、成员在数据库**，增删一台后端不重启不热重载；MySQL 挂了按已加载的配置继续服务，恢复后自动接上

还没有的：限流、熔断、指标暴露、配置热重载。（多 Reactor、上游连接池（**S3-01 起按目标分桶，仍 per-worker**）、配对模型与 pipeline/`close` 语义、优雅停止/排水、路由与三策略负载均衡、**被动+主动健康检查与摘除安全阀、节点表的 MySQL 来源与 reload** 都已落地——逐条证据在下面那张表。**`--route` / `--route-regex` 这对 S3-01 的临时开关已在 S3-02 被配置文件取代、不留兼容层**（传了就是"无法识别的选项"）：留兼容层等于同一件事有两种配法，而成本对照与故障注入只会各测一种。**加权不做**（已裁：要做先进 ADR-19，并连带写清它与健康检查摘除的相互作用）。**回源容量公式分岔**：没装路由 filter 时仍是 `min(worker 数, 端点数) × 单元组容量`（本机实测 28,232，所以 B-007 那档要求 `--workers ≥ 4`）；装了路由之后每个 (worker, 目标) 元组各自有 28,232 的余量，`--workers 1` 也能按请求连多个端点。）

<details>
<summary><b>逐条验收证据（点开）</b>——每个数字都来自本轮重跑，不是记忆</summary>

| 模块 | 实测 |
|---|---|
| `net/Buffer`（S1-02） | 默认配置下 1 KiB keep-alive × 1000 轮 `peak_capacity=4096`；`read()` 必须读到 `EAGAIN` 才算一轮结束（ET 契约） |
| `http/request_parser`（S1-03） | 纯函数不碰 fd；CL 与 chunked 两种 body；22 个错误码、37 类畸形输入（契约要求 ≥12）；release `-O3` 595 MB/s / 5.5M req/s（**归因用，不是 SLO**） |
| `net/Reactor`（S1-04） | 真 curl 10000 请求：`requests_parsed=10000`、`downstream_accepted=1`、`upstream_opened=1`、`live_connections=0`、`constructed==destructed`、fd 回基线；六步关闭 + 批末延后析构 |
| S1-03 两笔债（S1-04 还） | 10000 请求下 `header_scan_bytes == bytes_down_to_upstream`，一次重扫都没有；无游标的 dribble 场景原本是 7,474,911 字节（1956×） |
| Upgrade / keep-alive（S1-05） | 3 条顺序请求共用一条下游与一条回源连接；`Connection: close` 走意图传播 + EOF 级联；101 后二进制帧逐字节回显比对；`release_scratch()` 全仓唯一触发点在 101 之后 |
| 响应语义（S1-06） | ctest 26 条 + `smoke-s1-06.sh` 八阶段真 curl：1200 B 固定体逐字节一致、`--raw` 下的 chunked、`/dies`→502（直连同上游是 curl 退出码 52 零字节）、`/dies-mid-body`→恰好 600 B 且零追加、`/bad-chunk` 不伪造内容、`/endless-head` 超预算判死、`/dev/tcp` 手写的 400/501/TRACE；口径核对 `upstream_opened == requests_parsed`（被拒的请求一条都不该回源） |
| 背压（S1-07） | 四个方向各一条用例：请求方向 384 条 POST 平台期 2,922,610 B（载荷 3,170,580）；响应方向 1 MiB + 客户端一字节不读；隧道方向 2,742,440 B 帧字节平台期 1,898,661 B 且恢复后一字节不少；稳态 1000 请求 `epoll_modifications=3999`＝**4.00 次/请求**（与 S1-04 同口径，背压没多打一枪）。进程侧 `smoke-s1-07.sh` 四阶段，含 `--high-water-mark 4096` 下一条 100 KiB 请求**停在停读线上**（curl 零字节、无 5xx；S2-05 起这一步的"停在没有 EPOLLIN"由 `drain_read_disarmed_at_entry=1` 报出，因为排水最终会把这条卡住的连接收尾，停下之后再读必然是 0）。口径与被删掉的钉窗实验见 ARCHITECTURE 1.15 |
| filter 链（S1-08） | 顺序＝添加顺序（用例故意按 c、a、b 添加）、第一个 `kStop` 之后不再调用后面的、filter 状态跨请求存活；主路径上只有一处链调用；「不改主路径」的可执行形式＝空链与两条放行 filter 下响应内容与七项计数**逐项相等**。TSan 这轮抓到的是我自己写的第一版（循环还在跑时读非原子计数器） |
| 多 Reactor（S2-01） | 一个 acceptor + N 个 worker、round-robin 派发、**连接单线程拥有、跨线程只传裸 fd**（ADR-14）。`DispatchIsRoundRobinBalanced`：8 连接派发到 4 worker 每个恰好 2 条；`TenThousandRequests...`：10000 请求跨 4 worker，`constructed==destructed==16`、`fd 5→5`、聚合 `epoll 注册 20（=4 wake_fd+16 连接）/注销 16`；`ConcurrentLoad...`：16 线程 × 50 请求并发穿 4 worker，**tsan preset 零 data race**（S2 门禁硬条件）。生产路径 `--serve --workers 4` + 真 curl 20/20 200、`fd 15→15` |
| 上游连接池（S2-02） | **per-worker**（ADR-15，项目所有人裁决）：下游关闭时干净的上游**归还进池**、被下一条下游借走复用，而不是随之销毁（上游活得比下游久）。`ShortLivedDownstreamsReuseOnePooledUpstream`：20 条短命下游 → `upstream_opened=1`、`pool_borrows=19`；生产路径 4-worker + 20 条短命真 curl → **`upstream_opened=4`（每 worker 1 条）而非 20**、省 16 次握手。ADR-15 点名的边界由 `IdlePooledUpstreamClosedByPeerIsEvictedNotTruncation` 钉死：空闲上游被对端关是**正常回收**（`pool_evictions_eof`），绝不误计成截断、不发 502。**诚实边界**：wrk 闭环基准下池不减少上游连接数，S2-07 不声称它有收益 |
| 配对模型（S2-04） | 支持 HTTP/1.1 pipeline：一条下游的多个在飞请求按 FIFO 回送；`Connection: close` 在**它自己那条**请求的响应后兑现（不是在第一条）——修掉 S1-07 的"400 条只回 1 条"债。靠 `Connection` 的在飞计数（转发 +1 / 响应完结 −1，close 归零才关）。`S2_04_Pairing` 三条 + 旧条件阳性验证会红（ADR-16）。`sizeof(Connection)` 520→528 |
| 优雅停止／排水（S2-05） | SIGTERM/SIGINT 从"停止接受 + 退出循环"改成**按请求排水**：排水线 = `in_flight_requests_ > 0`（已转发给配对上游的才算已接受），空闲（在飞归零）立即关、在飞的逐条答完 + 刷完出向再关、半包交给 `--drain-timeout-ms`（默认 5000，**兜底不是承诺**）、**隧道只能硬关**（ADR-09 不解析帧 → 发不出 WS close 握手）。多 worker 协调：accept 循环把排水下发给每条 worker、**非阻塞地**读到"每条 worker 都自行报了停止"才 join。实测（`scripts/smoke-s2-05.sh` 五阶段）：阶段一 elapsed 里在飞那条**答完才退**（`drain_in_flight_at_entry=1`、`drain_forced_timeout=0`、76 字节=头 63+体 13 完整）；阶段二 3 条空闲连接 **0.11s** 退干净（兜底写着 60000ms，没用上）；阶段三半包 **0.81s** 走兜底（deadline 800ms，早于它关就算红）；阶段四隧道 **2189 字节 = 101 头 141 + 帧 2048 恰好相等**（不多一个伪造字节、不少一个缓冲字节）；阶段五 3 worker 各排各的（`drain_requested=3`、`in_flight_at_entry=3`、新连接被拒 1 条且 `requests_parsed` 停在 3）。五阶段生命周期三项全部归零（`live=0`、`constructed==destructed`、`fd_start==fd_end`）。一处不那么好看的取舍也写在这里而不是藏起来：**监听 fd 留到析构才关**，所以排水期间的新连接是"连上就被关"而不是 ECONNREFUSED——因为 `fd_start == fd_end` 是全项目生命周期证据的地基，而 `fd_start` 是在 `start_listen()` 之后量的（完整理由与代价见 ARCHITECTURE 1.20） |
| 测量手段／SLO #2 取数（S2-06） | 自研长连接客户端 `tools/c100k_main.cc`（契约 1.3 唯一许可自研的场景，手段变更记录在 [ADR-17](docs/adr/ADR-17-c100k-hdr-measurement.md)）+ `scripts/bench/c100k.sh`。**两种形态**：默认「握手后空闲」测 #2（每连接对内存、稳态散布、空闲 CPU 两栏分母），`--requests-per-conn` 测**每请求**延迟——#1 的 p999 只有后者能给。分位数是 HDR 风格两级直方图（1 µs 桶到 1 ms，其后每倍频程 16 桶）**带误差棒**，不报单点；100k 条连接靠 **4 个目标地址**（实测单元组上限 28,232，目标地址数与每地址连接数一律写进报告）。**工具自己要有等式**：客户端条数 == 网关 `requests_parsed` == `responses_completed` == 上游 `requests`，外加两条字节等式与一条量纲口算——就是这套东西抓出了我自己的三个错（限速其实没限速、观测打印压死事件循环、`EPOLLOUT` 没摘把延迟读成 4 µs），逐条在 [FAULT-INJECTION 第 7 节](docs/FAULT-INJECTION.md) |
| 回源多端点（S2-06a，ADR-17 候选 A） | `--upstream` 接受端点列表，**worker i 只用第 `i mod E` 个**（静态、按请求不选、无健康检查——那三者仍归 S3）。回源容量因此 = `min(worker, E) × 28,232`（实测单元组上限）。`S2_06a_MultiEndpoint` 三条用**不同体长的两个/三个 `TestUpstream`** 区分端点，断"各 worker 打各的端点""4 worker×2 端点按 `i mod 2` 各 2 条""**端点数 > worker 数时多出来的端点服务数 == 0**"；阳性验证：把 `i mod E` 改成常量 `0` → **三条同时红**，改回后三 preset 各 **135/135**、tsan 零 race。真进程侧由夹具自己打 `accepted_by_addr=`：2 worker×4 端点 400 条 → `127.0.0.1:200,127.0.0.2:200`（第 3、4 个地址 0 条）；25k 档 → 四地址各 **6,250**。顺带修掉一个取数定义：`c100k.sh` 的稳态窗口右界现在钉在客户端存活期，否则客户端退出那一刻的 FIN 风暴会被算进"空闲 CPU"（实测把 0.21% 抬成 1.57%）。详见 ARCHITECTURE 1.21 |
| S2-07 · 三个数字按取样规程重测 | ① **#1 的线程模型 A/B**：`s1-p99.sh` 加 `WORKERS`（缺省 1＝与历史八份逐字同形），同机交替各 3 轮 → workers=1 的 32.990–35.210 ms 打到 workers=16 的 **4.160–6.290 ms**，组内极差 2.1–2.2 ms、两系列不重叠（B-008）。脚本现在按**横幅签名**当场判路径，防止"A/B 两臂其实跑的是同一个形状"。② **p999**：新脚本 `scripts/bench/s1-p999.sh`（两臂各自 ≥60 s 窗口门 + 每臂自报观察者 CPU + 等式 `direct+proxy == upstream`），1000 并发那一档**如实记"取不到"**——两臂都贴着单核上限，同一份分布的 p99 增量与 wrk 线差 6 倍（B-009）。③ **构成表**：2 条 `S2_07_MemoryComposition` 复刻生产的分配形状逐项量出来，堆实测 17,861 B/对 vs 生产 17,889 B/对（0.16%），并把 S1-07 那 0.50 KiB 空项归到"N=128 处 VmRSS 没有分辨率"（B-011）。④ **负载下的排水**：`c100k.sh` 加 `DRAIN_UNDER_LOAD=1`，客户端仍持有 100k 条连接时发 SIGTERM → **0.947 s** 收干净、`hard_closed == established`、兜底一次没用上，默认 5000 ms 站得住（B-010，S4 的"请求超时 ≤ drain"对齐由此有依据） |
| 路由作为链上的一环（S3-01，ADR-18） | **链上第一个业务 filter**。决策是纯函数（`src/route/router.{h,cc}`：前缀**最长命中优先**、正则**全串锚定**且**优先于前缀**、匹配原始 origin-form 含 query 不解码不规范化）；无匹配 → **404** 走 S1-06 那张自产响应表的同一形状，且实测 `upstream_opened=0`（**拦下来就真的没回源**）。目标通过 `RequestInfo` 新加的 `upstream_target`/`reject_status` 交回主路径（`Decision` 没加第三个值、`ForwardOutcome` 没加第四个值）。**池按目标分桶但仍 per-worker 不 shared**（ADR-15 追加节）：`upstream_pool_` 是 `vector<vector<…>>`，`pool_max_idle` 变成**每 worker 每目标**，`pool_idle_count()` 改成各桶之和，`sizeof(Connection)` 仍 **528**（`static_assert` 钉住，因为 B-011 那张构成表按它算账）。**目标只在干净边界换**（`in_flight==0` + `outbound_pending==0` + `upstream_reusable`），换不动就压住这条请求，代价是 `pump_response` 的 `kComplete` 补一次重驱动——**不补是一条会挂死的环**（摘掉它：102 ms → 5,486 ms 红在读超时）。实测：三 preset 各 **164/164**、非 gtest 警告各 0 条、**tsan 零 data race**；`/b` 那条请求 `upstream_opened=1` 且另一个后端自报 `connections_accepted=0`；成本对照见 **B-012**（空链 vs 路由链，每请求 `epoll_ctl` 4.0000–4.0005 → **无可分辨成本**） |
| 负载均衡两环与配置文件（S3-02，ADR-19） | **链上是两环**：`RouteFilter` 写"哪一组"、`LoadBalanceFilter` 写"组里哪一个"（合成一环就把有状态的选择塞进纯函数层，1.23 那条"规则语义不建 socket"当场作废，S3-03 的健康检查也没地方插）。**均衡状态每 worker 一份、全非原子**，环在装配期建好运行期只读，所以新增的不是并发面——`tsan` 各 preset **219/219** 日志里 `ThreadSanitizer` 0 次是它的证明方式。三策略各自的可断言形状：轮询按**后端**计数（同一条 keep-alive 上 12 条请求、3 台**各 4 条**、`upstream_opened==3`）；最少连接断**方向**（两台被占时新请求去第三台）+ **释放的容量必须被重新算进去**（关掉中间那条下游，下一条要回到那一台——这条是交接段第 ⑦ 条装配坑的收据）；一致哈希**同一 key 换连接也不换后端** + **N→N+1 迁移率是量出来的**：10,000 个确定性 key 实测 **24.41%（N=3）/ 11.11%（N=7）**，界 25% / 12.5%。**这一格实测逼出一次哈希函数变更**：点键只差末位端口号时裸 FNV-1a 只差约 `Δ × prime`，点成簇 → 迁移率 48.10% / 40.10%，且 **vnodes 抬到 10,000 一点都不改**；按 ADR-19 给的第二个旋钮加了 splitmix 终值（FNV 两个常数一字未动，仍由发布向量钉着）。同一轮发现**判据的形状问题**：把环查找改成"取环上第一个点"时 N=7 读到 0.00%、反而满足"≤ 1/(N+1)"——**只看上界挡不住退化实现**，所以读成"上界 + 散布"一对（ADR-19 追加第二条）。配置文件：JSON 子集手写不引库，**读不出就整体失败**（30 条用例逐条断"报错的话里点到那个字段名"），`--config` 与 `--upstream` 互斥、S3-01 那两个临时开关**删掉不留兼容层**；`deploy/gateway.json` 既被 ctest 读、也被 `scripts/smoke-s3-02.sh` 五阶段真跑通（后端计数由上游自报 **2/1/2**）。阳性验证六处全部真打红（FAULT-INJECTION §9）。**成本 B-013 分两半**：多两环只要不换后端 → 与空链的计数器**逐字相同**（每请求 `epoll_ctl` 4.0000、`upstream_opened=100`、`pool_borrows=0`），判"在本档分辨率的边缘，判不出是否非零"；**每请求换后端 → p50 +1,664 µs（1.57 倍）、QPS −36%**，切换占 16.3% 请求。两个副作用原样记：对照臂 `least_conn` 有**平手吸附**（98.3% 吸到成员 0，两条单测抓不到）；B-012 那四轮的落盘自带 7 条 ✗（每连接配额被当总配额用，`done == planned` 永远合不上）——两者都登记为待裁，不回头改已记的数 |
| 健康检查与摘除安全阀（S3-03，ADR-12 落地 + ADR-19 追加三/四） | **被动**四个记账点（连不上 / 头坏 / 头之前断 / 5xx 最终头）+ **主动探活**（判据钉死：2xx/3xx/4xx 活、5xx/连接失败/超时死——固定回 404 的后端样本就是这条判据的钉子；探活连接是 owner 循环私有的状态机，`requests_parsed == responses_completed == 0` 在探活世界照成立）。**安全阀**：10 台全死、上限 10% → 恰好摘 1 台、540 次被挡有账。**门禁两数**：摘除耗时 **202 ms / 202 ms**（kill → 网关自报摘除时刻，同一根墙钟轴）、误摘率 **0/3480 与 0/3471**（探活轮数 × 健康后端数，判据成对：上界 + "摘除确实发生过"下界）。**least_conn 平手改平手集合内轮询**（B-013 实测 98.3% 吸附成员 0 的修复，ADR-19 追加三）。三 preset 各 246/246、tsan 0 race；阳性验证五处真红（FAULT-INJECTION §10）。成本 **B-014** 见下 |
| 节点表的 MySQL 来源（S3-04，ADR-20） | **首次连通取证**：`l7gate_mysql_probe` 五步全通（**8.4.11 客户端 ↔ 8.0.36 服务端**，契约 9.1 挂了四天的版本偏离就此实测成立）。成员表 `l7gate_nodes`、组级 `"source": "mysql"` 与 `targets` 互斥、**reload = 专用线程查询 + `atomic<shared_ptr>` 整表换指针**（ever-seen 目标下标只增不减，跨 reload 存活成员下标稳定）、**降级零中断**。reload 生效 **406/536 ms**（粒度 = `--nodes-reload-ms` 500ms 周期）。真容器六阶段冒烟 `smoke-s3-04.sh` 全绿；三 preset 各 257/257、tsan 0 race。成本 B-015/B-016 见下 |
| 故障注入矩阵第一轮（S3-05，ADR-13） | 五类故障全过（FAULT-INJECTION §12，真进程 + 逐请求 trace）：connect refused **摘 273ms/恢 89ms**；5xx **324/129ms**；半开 **5280ms**（探活侧 ≤1s，读数被在途请求 5s 排空主导——归因在 §12）；慢过超时摘 2108ms、**慢在超时内不摘**（分界线 = probe_timeout）；探活失败业务正常 **359/90ms**（摘除后直连业务路径仍 200——探活是成员资格的唯一真源）。恢复语义全表一致 **89–129ms**。ctest 补半开/慢线内两格 |
| sanitizer | `debug` / `asan-ubsan` / `tsan` 各 **259/259**，非 gtest 警告 **0 条**（其中 `asan-ubsan` 与 `tsan` 各有 **2 条 Skipped**：构成表的数值判定——sanitizer 换了分配器后 `mallinfo2()` 一律报 0，所以**跳过＝不判，不是通过**，结构那一半照判）；每条"干净"都带反证（[docs/FAULT-INJECTION.md](docs/FAULT-INJECTION.md) 第 3 节）。多线程的 TSan 反证做过三次：S2-01 摘掉 handoff 锁 → 报 3 处 race，S2-03 的 stop-under-load churn → 报 **14 处**，S2-05 把排水的跨线程标志做成普通 `bool` → 6 条排水用例里 **5 处 data race**，每次都还原后全绿——"TSan 干净"是能红的绿。排水机制另有三次逻辑阳性验证（摘守卫→3 条红、摘隧道分支→1 条红、关 accept 拒绝→1 条红），以及**一次"拆了也不红"的诚实记录**：把隧道收尾从 `kFlushThenClose` 换成 `kDiscardOutbound` 全绿，因为 4096 字节的回显早被内核套接字缓冲吸收了——那条判据因此归 S1-04 的既有用例，不归这条 |
| 跨线程销毁（S2-03） | per-worker 池让"连接被非 owner 线程持有"的 harder 竞态**没出现**（所有 `shared_ptr` 持有者都在同一 worker 线程内，析构必在 owner 线程，跨线程只流动裸 fd）。红队三条 `S2_03_CrossThreadDestruction`：负载活跃时 stop、handoff 的 fd 注册前被对端关、连接风暴——tsan 零 race、asan/LSan 零 UAF 零泄漏。销毁协议定稿在 ADR-14 |

</details>

---

## 阶段进度

| 阶段 | 目标 | 状态 | 门禁 |
|---|---|---|---|
| **S1** | 单连接转发 + 第一份 p99 | ✅ 八个任务全 DONE | 四件套：#2/#3/#4 齐，#1 的 #3 半边按裁决**推到 S5 终判**（理由见上表） |
| **S2** | 多 Reactor + 上游连接池 + 跨线程销毁 | ✅ **八个任务全 DONE（S2-01…S2-07 + S2-06a）**：三 preset 各 **137/137**、非 gtest 警告各 0 条、TSan **零 data race**；#2 达标 + 构成表 0.16% 吻合、#1 未达标但第一次测到多 Reactor 有效（33 ms → 4–6 ms）、p999 那一档记"取不到"、100k 隧道负载下排水 0.947 s。**四件套齐，已按门禁收尾 push（`47bca67..dcccca5`，2026-10-29）**（逐条对账见 [docs/ROADMAP.md](docs/ROADMAP.md) 的「## S2」节） | TSan 干净 |
| **S3** | 路由 + 负载均衡 + 健康检查 + 摘除安全阀 | ✅ **六个任务全 DONE（S3-01…S3-06）**：三 preset 各 **259/259**、非 gtest 警告各 0 条、TSan **零 data race**；摘除耗时 202 ms、误摘率 0/3480（带分母）、安全阀 10 台全死停 1/10（B-014）；首次连通取证 8.4.11↔8.0.36（契约 9.1 翻案）、reload 生效 406/536 ms、降级零中断（B-015）；五类故障矩阵全过（§12）；每请求成本 B-016 见下。**四件套齐，push 待门禁收尾授权**（逐条对账见 [docs/ROADMAP.md](docs/ROADMAP.md) 的「## S3」节） | 摘除耗时、误摘率、故障注入实测 |
| **S4** | 限流 + 熔断 + 超时重试防放大 | 未开始 | 429 生效数据、熔断时间线、Redis 降级 |
| **S5** | 热重载 + 指标 + 面板 + vs Nginx 归因 | 未开始 | 三个数字终稿、归因报告、故障注入全表 |

S1 的门禁结论、一键复现命令、以及**实测暴露出来留给 S2 的九条债**（每条都指着具体数字）在 [docs/ROADMAP.md](docs/ROADMAP.md)。S2–S5 只有一句话目标——契约第 4 节要求每个阶段门禁通过时才拆下一阶段，提前拆会写成幻想中的任务表。

---

## 架构与数据流

多线程 Reactor（one loop per thread）+ epoll ET + 配置驱动的 filter 链。**S2-01 起多 Reactor 骨架已落地**：一个 acceptor 线程 accept、round-robin 派发给 N 个 worker（`--workers N`），**连接对象单线程拥有、跨线程只传裸 fd**（ADR-03/ADR-14）——于是"两个线程碰同一条连接"在结构上不成立，TSan 门禁（117/117 零 race）证的就是这件事。缺省仍是单 Reactor（`--workers 1`），S1 的精确计数与冒烟证据不作废。**S2-02 起上游连接池已落地**（per-worker，下游关闭时上游归还进池被复用，ADR-15）；跨线程异步销毁的 harder 情形归 S2-03。

```
client ──epoll ET──▶ acceptor ──round-robin──▶ N×worker Reactor ──▶ Buffer ──▶ HTTP/1.1 parser
                       │                        （✅ S2-01：连接单线程拥有，跨线程只传 fd）
                       │              │
                       │              ├─ 原始字节 ──▶ 上游连接        ✅ S1-04/05（回源复用 + 101 后纯字节隧道）
                       │              ├─ 上游响应 ──▶ 下游            ✅ S1-06（框架判定 + 截断两条口径）
                       │              └─ 停读线 / 探测读恢复          ✅ S1-07（滞回 + 边沿触发的欠账）
                       │
               filter chain（配置决定顺序）   ✅ S1-08 骨架 + S3-01 路由 + S3-02 负载均衡（业务环两枚）
                       │
        上游连接池（per-worker，按目标分桶）    ✅ S2-02（ADR-15）+ S3-01 分桶（仍 per-worker，不 shared）
                       │
   路由→组 ──▶ 组内选目标 ──▶ 池按目标借还      ✅ S3-01/S3-02（策略与后端表来自 --config）
                       │
        熔断 / 限流（共享状态）                ⏳ S4
                       │
        指标 → Grafana 面板 / WS 调试页        ⏳ S5（见「可观测面与调试界面」）
```

一句关于性能的实话：我选边缘触发，但 ET 不比 LT "更先进"，两者性能差别极小。选它的真实理由是**它逼出正确的 Buffer 设计**——一次必须读到 `EAGAIN`（[ADR-02](docs/adr/ADR-02-epoll-et-io-uring-deferred.md)）。这句"逼出"有实物支撑：`net/Buffer` 的扩容策略我改了两版，两版都被内存用例打回来（固定 64 KiB 申请把每连接容量下限抬到 128 KiB；按 hint 预留仍抬到 8 KiB，而 SLO #2 的预算是 20 KB），细节在 ARCHITECTURE 1.3。

而 ET 真正的代价是另一件事：**它的坑不报错，它挂住。** 超容量时我第一版写的是 `return`，正确的是关掉这条、继续 `accept` 直到 `EAGAIN`——带着没排空的队列退出，内核不会再投第二次监听事件，表现是"过一会儿不接新连接了"，退出码 0、日志一行没有。同一个形状还有两处（一次事件里读进多条请求必须全走完；`EPOLLOUT` 刷空后必须摘掉，否则对着"随时可写"的 fd 自旋，SLO #2 的空闲 CPU 第一条就断在这儿）。三处现在各有一条用例盯着。

网络层全部在本仓库内自行实现，不引入 asio / muduo / brpc（[ADR-04](docs/adr/ADR-04-no-third-party-network-lib.md)）——"网络层怎么写的"就是这个项目要交付的东西。

### 与 Nginx 的功能差异（不是性能差异）

在流量治理这个维度上，本项目功能不比 Nginx 开源版少：

- Nginx OSS 只有被动的 `max_fails` / `fail_timeout`；**主动健康检查是 Nginx Plus 才有的功能**
- Nginx **没有内置熔断器**
- 熔断带**最多摘除比例安全阀**（默认 10%，对齐 Envoy 的 `max_ejection_percent`）——判据失准时最坏只损失 10% 容量，防止熔断本身变成雪崩放大器（[ADR-12](docs/adr/ADR-12-circuit-breaker-ejection-cap.md)）

这两件事不能混着说：**我比 Nginx 多的不是速度，是治理策略**；吞吐基线仍然是 Nginx，目标是它的 60%，不是超过它。

---

## 快速开始

环境要求与实测版本见 [docs/ENVIRONMENT.md](docs/ENVIRONMENT.md)。**构建与压测必须在 Linux/WSL2 原生文件系统内**——放在 `/mnt/d` 这类挂载盘上，9P 的 IO 抖动会直接进 p99，测出来的数既不属于网关也不属于 Nginx。

```bash
cmake --preset debug && cmake --build --preset debug
ctest --preset debug                      # 219/219（2026-10-30，S3-02；构成见 TASKS.md 各任务行。
                                          # asan-ubsan / tsan 两个 preset 里各有 2 条 Skipped：
                                          # sanitizer 换分配器后 `mallinfo2()` 一律报 0，跳过＝不判、不是通过）
./build/debug/l7gate --self-check         # version / compiler / gtest / sanitizers / build_type / malloc

# 手动跑一遍透传：起上游 → 把网关架在中间 → 用真 curl 打一发
./build/debug/tests/l7gate_test_upstream --listen 127.0.0.1:0 --body-bytes 64 &
./build/debug/l7gate --serve --listen 127.0.0.1:18080 --upstream 127.0.0.1:<上面自报的端口>
curl -sS -i http://127.0.0.1:18080/hello
kill -TERM <网关 pid>                     # 停止时才打印 [serve] key=value 统计（含 fd 与 RSS 首末值）

# 换成策略配置（S3-02）：后端表与路由表进文件，--upstream 就不再给（两者互斥）
./build/debug/tests/l7gate_test_upstream --listen 127.0.0.1:18081 --body-bytes 64 &
./build/debug/tests/l7gate_test_upstream --listen 127.0.0.1:18082 --body-bytes 128 &
./build/debug/tests/l7gate_test_upstream --listen 127.0.0.1:18083 --body-bytes 192 &
./build/debug/l7gate --serve --listen 127.0.0.1:18080 --config deploy/gateway.json
curl -sS http://127.0.0.1:18080/api/users | wc -c   # 轮询：这一发 64
curl -sS http://127.0.0.1:18080/api/users | wc -c   # …下一发 128（同一条组里换了一个成员）
curl -sS http://127.0.0.1:18080/v2/orders | wc -c   # 一致哈希：同一个 key 恒定同一个后端
kill -TERM <网关 pid>                               # 横幅里的 policy=on 才是"配置真生效"的签名
```

`deploy/gateway.json` 是**会被 ctest 读的那一份**（`S3_02_ConfigFile.TheShippedExampleConfigAssembles`），
不是文档里的示意图；它跑不跑得通用 `PRESET=debug bash scripts/smoke-s3-02.sh` 验，五阶段全绿才作数。

`--serve` 打的每行统计是给脚本读的，不是给人的漂亮表格；读不到的一律是 `-1` 而不是 `0`——**"没测到"和"没有泄漏"是两件事**。

| preset | 编译器 | 用途 |
|---|---|---|
| `debug` | GCC | 日常开发 |
| `release` | GCC | **压测唯一允许**——sanitizer 构建测出来的是检查开销的延迟 |
| `asan-ubsan` | Clang | S1 门禁（强制 `-O0`：`-O1` 起会把"可被证明无副作用的坏写入"优化掉，那种"干净"是假的） |
| `tsan` | Clang | S2 门禁：跨线程传递与异步销毁的唯一自动化验证手段 |

```bash
bash scripts/verify-all-presets.sh        # 三 preset 构建 + ctest + 我方警告计数，一次拿全
PRESET=debug bash scripts/smoke-s1-04.sh  # 真 curl 10000：fd / 存活连接 / 字节数全绿
PRESET=debug bash scripts/smoke-s1-06.sh  # 八阶段：逐字节一致 / chunked / 两类截断 / 坏框架 / 映射表
PRESET=debug bash scripts/smoke-s1-07.sh  # 四阶段：慢客户端 / 慢上游 stall / 停读线之下 / 半条响应头
PRESET=debug bash scripts/smoke-s2-05.sh  # 五阶段优雅停止：在飞答完才退 / 空闲立即关 / 半包走兜底 / 隧道硬关不伪造 / 3 worker 各排各的
PRESET=debug bash scripts/smoke-s3-02.sh  # 五阶段配置面：示例配置真跑通（轮询/哈希/catch-all 各一格）/ 404 不回源 / 五条拒绝全退出码 2 / 排水归零 / 后端自报计数
scripts/bench/s1-p99.sh                   # SLO #1 标准口径（canonic 由参数自己判，非标那轮不进文档）
scripts/bench/s2-qps-vs-nginx.sh          # SLO #3 五档：nginx 三档 + 网关 + 直连天花板
scripts/bench/c100k.sh                    # SLO #2：多目标地址长连接稳态 + 每连接对内存 + HDR 分位
                                          # 非标准口径跑法：CONNECTIONS=25000 TARGETS=4 bash scripts/bench/c100k.sh
                                          # 排水档（客户端仍持连时发 SIGTERM）：
                                          #   DRAIN_UNDER_LOAD=1 CONNECTIONS=100000 TARGETS=4 UPSTREAM_ENDPOINTS=4 \
                                          #   WORKERS=16 RAMP_MS=300000 HANDSHAKE_TIMEOUT_MS=120000 bash scripts/bench/c100k.sh
scripts/bench/s1-p999.sh                  # SLO #1 的 p999 半边：1000 并发闭环，直连 vs 经网关的增量
                                          # 每一臂都自报观察者 CPU；1000 并发那一档目前判"取不到"（B-009）
                                          # 成本 A/B 的旋钮（缺省值下 argv 与历史逐字相同，B-008/B-009/B-012 不作废）：
                                          #   GATEWAY_EXTRA=…           只给经网关那一臂追加网关参数
                                          #   UPSTREAM_COUNT=2          多起一台压测上游（两臂进程数对齐）
                                          #   GATEWAY_CONFIG_TEMPLATE=… 经网关那一臂改用现场渲染端口的 --config
                                          #     （--config 与 --upstream 互斥，所以是**替换**不是追加；
                                          #      渲染后残留占位符或横幅没有 policy=on 都会当场 fatal）
```

「ASan 干净」这句话不该靠我手敲，所以有一条命令能复现它；同理每个数字都能一键重跑，这是契约第 7 节对这几份文档的完成判据。

---

## 明确不做的东西

每条都有 ADR 写明理由，不装作它们不存在：HTTP/2 / gRPC / QUIC、TLS 握手、第三方网络库、etcd / xDS / K8s / Service Mesh、动态服务发现、**响应缓存**（缓存是另一种网关形态，做了它 p99 就不再度量流量治理）、WebSocket 帧解析（只做 Upgrade 后透传）、多进程 `SO_REUSEPORT`、Web 管理后台、自研压测客户端（除 C100K 长连接场景）。

还有一个反向承诺：**io_uring、连接对象池、日志异步化全部挂起**。只有当压测 + perf 火焰图拿出证据证明那是真实瓶颈时才立项，且先写 ADR。在没有证据的情况下优化，是空壳项目的典型形态。

---

## 可观测面与调试界面（规划中，未实现）

三个数字最终要能被**看见**，不是只存在于压测报告里。这一节是 S5 的交付物形状，现在**一行代码都没有**，接口也未定（[docs/SLO.md](docs/SLO.md) 的 #2/#3 测量条件就是面板上的格子）。

**① Grafana 面板「网关总览」——六个格子，就是三个数字的落地形态**

| 格子 | 来源 | 说明 |
|---|---|---|
| 吞吐（QPS） | Prometheus counter | 对应 SLO #3 |
| 转发延迟 p99 / p999 | 直方图分位 | 对应 SLO #1；**p999 那一格的取数方案已裁决**：由自研 C100K 客户端的 HDR 分位统计供数（当前 wrk/hey 只到 99%） |
| 活跃连接 | gauge | 配 `每连接内存` 才能看 SLO #2 是否守住 20 KB |
| 熔断状态 | 三态机当前态 + 触发次数 | S4 的熔断是否真的在开合 |
| 上游健康度 | 每后端：健康 / 已摘除 | 配合摘除比例安全阀（ADR-12）看"误摘" |
| 状态码分布 | 200 / 429 / 503 占比 | **429 那一栏是限流真的在生效的直接证据**——没有它，"限流已实现"只是一句自述 |

**② WebSocket 调试页——只有三块：地址栏、消息流、输入框**

连上 `ws://<host>:<port>/ws` 后能看到握手状态、RTT、已收条数，以及逐条带方向的帧；被限流时那一行直接显示 `429 rate limited`。它存在的理由不是方便我调试，而是**把"透传不裁决"（ADR-09）这件事变成肉眼可验的**：帧内容原样进出，网关只在连接层面做握手与转发。

**③ 故障注入的现场**：配合 `tc netem` 注入延迟与丢包，同一页面上能亲眼看到限流返回 429、熔断把后端摘掉。`netem` 在本机 6.18 WSL2 内核上**尚未验证可用**（记在 ENVIRONMENT 第 7 节），所以这一条的落地前提是它可用，否则按契约 8.2 先写 ADR 再换手段。

> 已作为待拆任务记入 [TASKS.md](TASKS.md) 的 S5 行与 [docs/ROADMAP.md](docs/ROADMAP.md) 的 S5 门禁条件。面板与页面都属 S5，不在 S1–S4 的授权范围内，我不会提前动手。

---

## 文档索引

| 文档 | 内容 |
|---|---|
| [CLAUDE.md](CLAUDE.md) | 唯一契约：身份约定、边界、SLO、决策、门禁 |
| [TASKS.md](TASKS.md) | 任务表（唯一真相源）：ID / 可断言验收标准 / 状态 / 实测数字 |
| [docs/SLO.md](docs/SLO.md) | 三个数字的目标与写死的测量条件 |
| [docs/ENVIRONMENT.md](docs/ENVIRONMENT.md) | 环境自检实测版本、版本偏离的理由、sanitizer 阳性验证、Docker 与镜像源现状 |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | 已实现的现状（严格区分"已实现"与"已定但未实现"）；**第 5 节是踩坑索引**——一行一条、只指路不复制，格式固定「现象 / 根因 / 防复发的断言」，写不出第三条的记录在索引里明写"缺" |
| [docs/BENCHMARK.md](docs/BENCHMARK.md) | 压测记录：B-001…B-005 + 口径变更节，**未达标照记** |
| [docs/FAULT-INJECTION.md](docs/FAULT-INJECTION.md) | 故障注入的预期 vs 实测 + sanitizer 结论（每条"干净"都带反证） |
| [docs/ROADMAP.md](docs/ROADMAP.md) | 五阶段与每阶段**门禁结果**：S1 四件套对账、一键复现、留给 S2 的九条债、待答复事项 |
| [docs/adr/](docs/adr/) | 16 条决策记录（ADR-14 连接所有权、ADR-15 per-worker 上游连接池、ADR-16 请求-响应配对模型随 S2 新增） |

---

## 许可

MIT。见 [LICENSE](LICENSE)。
