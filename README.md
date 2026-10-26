# l7gate

一个单机部署的 L7 流量治理网关：解析 HTTP/1.1 请求，按策略路由、限流、熔断后转发给上游，并对整个过程给出**可复现**的性能与故障边界数据。

不是"又一个高性能 Web 服务器"，不是框架，也不是功能清单。对外形态是一个可以 `docker compose up` 起来、替换 Nginx 位置的网关。

## 现在的状态

**S1 的八步全部完成（构建骨架、动态缓冲、HTTP/1.1 解析器、epoll ET 事件循环 + 双向转发、keep-alive 复用与 Upgrade 透传、回源转发 + 响应回写闭环、主动背压、filter 链骨架）：网关现在真的能转发请求、也认得响应了——它知道一条响应在哪里结束，也知道什么时候该停下不读。** 我不在这段含糊其辞：`l7gate --serve --listen ... --upstream ...` 起一个单线程 epoll 边缘触发循环，把下游解析出的完整请求按**原始字节**搬到上游、把上游的响应原样搬回来，keep-alive 复用同一条回源连接，`Connection: close` 兑现成一次性配对；带 upgrade 记号的请求在上游回出完整 101 头后整条连接转成**纯字节隧道**——帧不解析、不改写（ADR-09），101 之后那 2048 字节的解析 scratch 被显式释放。S1-06 补上的是响应侧的认识：七种框架结论（CL / chunked / 无 body / 1xx / close-delimited / 头未齐 / 头坏）、chunked 的边界游标只数边界不重组 body、`ParseError → 400/413/501` 的映射表（switch 没有 default，加错误码不认账就编译不过），以及**截断的两条口径**——最终头没排给下游时网关自产 502，头已经出去了就只能以连接终止表态、绝不追加第二条响应。真 curl 打 10000 个请求跑完之后，存活连接归零、fd 回到基线、构造数等于析构数，ASan/LSan 与 TSan 都是 0 报告。**S1 的八步已全部完成**：S1-07 补上了主动背压的"摘事件"动作（撞停读线摘 `EPOLLIN`、水位回落带一次探测读挂回来），S1-08 立起了 `filter/chain` 的顺序骨架（今天没有业务 filter）。还没有的是路由、负载均衡、限流、熔断、连接池、多线程——下面每一行都分开写清楚，没有的一律标"未测"。**SLO #1 至今每一份取样都未达标**：增量 p99 四份读数是 57.540 / 36.420 / 37.890 / 29.720 ms（目标 ≤ 1.0 ms），收尾按 **30–38 ms 量级**陈述、不挑最好看的那一次，归因与取数规程的约束写在 [docs/BENCHMARK.md](docs/BENCHMARK.md) B-001…B-004；S1 的门禁结论与留给 S2 的九条债在 [docs/ROADMAP.md](docs/ROADMAP.md)。

| 项目 | 状态 |
|---|---|
| 动态缓冲 `net/Buffer` | 已实现（S1-02 + S1-04 写出侧）：连续、带高水位、`read()`/`send()` 到 `EAGAIN` 才算一轮结束；默认配置下 1 KiB keep-alive × 1000 轮实测 `peak_capacity=4096` |
| HTTP/1.1 解析器 `http/request_parser` | 已实现（S1-03）：纯函数、不碰 fd，CL 与 chunked 两种 body，22 个错误码；`release -O3` 下 595 MB/s / 5.5M req/s（单线程、无 syscall，**归因用，不是 SLO**） |
| `net/Connection` + `net/Reactor` + 双向透传 | 已实现（S1-04）：六步关闭（含"先刷完出向"那一步）、accept 读到 EAGAIN、一次事件走完管线里的所有请求；真 curl 10000 请求 → `requests_parsed=10000`、`requests_rejected=0`、`downstream_accepted=1`、`upstream_opened=1`、`fd 9→9`、`live_connections=0`、constructed=destructed=2 |
| S1-03 欠的两笔债（已扫描游标 / view 生命周期） | 已还（S1-04）：10000 请求下 `header_scan_bytes = 838,894` 与 `bytes_down_to_upstream` **完全相等**，一次重扫都没有；无游标的 dribble 场景原本是 7,474,911 字节（1956×） |
| HTTP 响应语义（状态码映射 / chunked / 截断 / 逐字节一致验收） | 已实现（S1-06）：`http/response_framing` 七个框架结论 + chunked 边界游标（一字节不重组）+ `ParseError → 400/413/501` 映射表；截断两条口径分别落在 `upstream_truncated_before_head`（→ 自产 502）与 `upstream_truncated_in_body`（→ 只以 EOF 表态，零追加）。验收证据两条路：ctest 26 条（19 纯函数 + 7 reactor 闭环）+ `scripts/smoke-s1-06.sh` 八阶段真 curl（1200 B 固定体逐字节比对、`--raw` 下的 chunked、`/dies`、`/dies-mid-body`、`/bad-chunk`、`/endless-head`、`/dev/tcp` 手写的 400/501/TRACE）。阶段八那条口径核对是 `upstream_opened == requests_parsed`：**被拒的请求一条都不该回源** |
| keep-alive 复用与 Upgrade 透传 | 已实现（S1-05）：3 个顺序请求共用一条下游与一条回源连接（`downstream_accepted=1`、`upstream_opened=1`、fd 回基线，有断言）；`Connection: close` 走意图传播 + EOF 级联兑现成一次性配对（不 eager close，响应边界精确判定归 S1-06）；双条件（`Connection` 含 `upgrade` 记号 **且** 有 `Upgrade` 头）命中才记握手意图，101 之后两个方向的字节逐字节透传、`parse_calls` 不再增长——二进制帧回显逐字节比对在 reactor 用例里锁住；`release_scratch()` 全仓唯一触发点在 101 之后 |
| 背压（高水位触发后摘掉读事件） | 已实现（S1-07）：判据只看这条连接的入向可读区相对它自己的停读线在哪，滞回是"到线停、回落到半线以下才恢复"，恢复自带一次主动探测读（边沿触发不为已经在里的字节负责）。四个方向各一条用例：请求方向（慢上游 + 384 条 POST，平台期 2,922,610 B / 载荷 3,170,580 B）、响应方向（1 MiB + 客户端一字节不读）、隧道方向（2,742,440 B 帧字节，平台期 1,898,661 B，恢复后一个字节不少）、稳态无压强（1000 请求 `epoll_modifications=3999`＝**4.00 次/请求**，与 S1-04 的 40,000/10,000 同一口径，背压没有多打一枪）。进程侧另有 `scripts/smoke-s1-07.sh` 四阶段：真 curl `--limit-rate` 慢客户端、`/dev/tcp` 的 400 条 pipelined POST 打在 stall 中的上游、`--high-water-mark 4096` 下一条 100 KiB 请求**停在上线上**（`read_disarmed_connections=1`、curl 零字节、没有 5xx）、`/dies-mid-head`。口径与那笔被删掉的钉窗实验在 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) 1.15 |
| 尾延迟 p99 | **已测，三次都未达标**：SLO #1 标准口径（1 KiB / 1000 并发 / 60 s 稳态 / release / 同机回环）下的增量 p99 取样 = **57.540 ms**（B-001，S1-06）→ **36.420 / 37.890 ms**（B-003 与其同 binary 复跑，S1-07）→ **29.720 ms**（B-004，S1-08 收尾）。按 **30–38 ms 量级、超目标 30–38 倍**陈述，不挑最好看的那一次。能确定的是每请求 `epoll_ctl` **5.00 → 4.00**（三次读数 4.005 / 3.999 / 4.003，极差 0.15%）；不能确定的是 p99 那 20–28 ms 的变化来源——同 binary 复跑让**直连基线自己漂了 11%**，取样带宽（27%）大于待测改动的可信区间，所以 S2 之后判断一次改动必须同机交替、每改动至少两轮。归因三层仍成立：闭环排队主导（`延迟 ≈ 连接数/QPS`，B-004 贴合 1.4%）、每请求服务成本 ≈36 µs（50 并发对照 1.360/1.370 ms 两遍同值）、来源是单 Reactor。**p999 无法判定**——wrk 4.1.0 与 hey 0.1.4 的分位表都只到 99%。全表在 [docs/BENCHMARK.md](docs/BENCHMARK.md) B-001…B-004 |
| QPS vs Nginx | **未测**（且 Docker 尚未接入，基线起不来）；S1 交的是**自测 QPS**，三份标准口径：经网关 18,229.47 / 直连 59,860.19（B-001），经网关 19,640.38 / 直连 59,624.37（B-003），经网关 **21,582.93** / 直连 57,170.55（B-004，S1 收尾那份）——按卡点三，这个数字在 S1 不设判定基线 |
| 100k 长连接每连接内存 | **未测**（S2 之后）；当前有的是 128 条空闲连接摊出的 **5.81 KiB/连接**（debug 本轮读数，三次同值；历史读数 4.47 / 4.62 / 5.31 / 5.66 / 5.72，带子本身是 glibc malloc 的取整噪声，按区间记、**不外推**）。`sizeof(Connection)` 现 **520**（S1-06 因响应跟踪 400→512，S1-07 加 `armed_events_`+`read_stopped_at_line_` 5 B 被对齐吃成 +8）；出向 Buffer 与 scratch 在纯空闲场景仍为 0。**S1-07 本轮每连接摊出比 S1-06 那轮多 0.50 KiB，而这 8 字节的对象解释不了它——差额我没有归因，照记在 ARCHITECTURE 1.11，留给 S2 的 100k 稳态实测裁决** |
| filter 链骨架 `filter/chain` | 已实现（S1-08）：**今天没有业务 filter**，交的是顺序——`run()` 正序、第一个 `kStop` 之后不再调用后面的、filter 的状态跨请求存活（unique_ptr 持有，不复制）。链里**没有** `priority` 字段：需要优先级就是配置数组的顺序。主路径上只有**一处**链调用（取完 upgrade/close 意图之后、排字节之前），那句「新增 filter 不修改转发主路径的任何一行代码」在测试里的形式是：空链与两条放行 filter 的链，同一套流量的响应内容与 `requests_parsed`/`bytes_down_to_upstream`/`responses_completed`/`upstream_opened`/`epoll_registrations`/`epoll_modifications`/`high_water_hits` **逐项相等**。停下 = "不转发、连接不关、不发响应"，今天就是这个形态（谁替客户端回 429 归 S4 限流）。新增 8 条（6 纯链 + 2 真循环），`sizeof(Connection)` 不变 |
| 限流 / 熔断 / 路由 / 热重载 | 未实现（S3–S5）；接口上留的返工点写在 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) 1.16 末尾——路由要一个"目标上游"字段、响应方向要一个钩子，两者今天都没有读者，所以我不预先造字段 |

## 三个数字

项目的全部价值锚点是这三个数，测量条件写死在 [docs/SLO.md](docs/SLO.md)：

1. **转发尾延迟**——网关自身引入的增量 p99 ≤ 1.0 ms，p999 ≤ 5.0 ms（同机回环、1 KB 响应体、keep-alive、1000 并发、稳态 60 s）→ **每一份取样都未达标**：57.540（B-001）/ 36.420（B-003）/ 37.890（B-003 同 binary 复跑）/ 29.720 ms（B-004，S1 收尾），按 **30–38 ms 量级**陈述；p999 因工具只到 99% 而无法判定
2. **长连接资源占用**——100k WebSocket 长连接稳态，每连接 ≤ 20 KB，空闲 CPU < 5% → **未测**（要 S2 的连接模型 + 先解决 `ulimit -n`=10240）
3. **吞吐**——QPS ≥ **Nginx 同环境实测值的 60%**（16 vCPU、1 KB keep-alive 回显）→ **未测**（Docker 未接入，基线起不来）；S1 只交自测 QPS：经网关 18,229 / 直连 59,860（B-001），经网关 19,640 / 直连 59,624（B-003），经网关 21,583 / 直连 57,171（B-004，收尾那份）

第三个数还有附加产出：与 Nginx 的差值必须定位到 HTTP 解析、内存分配、连接管理、日志写盘、系统调用次数中的哪几项。「差多少」谁都能测，**「差在哪」才是这个项目**。

关于性能的一句实话：我选 epoll **边缘触发**，但 ET 并不比 LT "更先进"，两者性能差别极小。选它的真实理由是它逼出正确的 Buffer 设计——一次必须读到 `EAGAIN`（[ADR-02](docs/adr/ADR-02-epoll-et-io-uring-deferred.md)）。

这句"逼出"现在有实物支撑了：`net/Buffer` 的扩容策略我改了两版，两版都是被内存用例打回来的——固定 64 KiB 申请把每连接容量下限抬到 128 KiB，改成按 hint 预留仍然抬到 8 KiB，而 SLO #2 的预算是 20 KB。细节与最终口径在 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) 第 1.3 节。

S1-04 又添了一条，这条更能说明 ET 的代价在哪：**边缘触发的坑不报错，它挂住。** 超容量时我写的是 `return`，正确的是关掉这条、继续 `accept` 直到 `EAGAIN`——带着没排空的队列退出，内核不会再投第二次监听事件，之后的新连接全滞留在 backlog 里。表现是"过一会儿不接新连接了"，退出码 0、日志一行没有。同一个形状还有两处：一次事件里读进多条请求时必须全走完（keep-alive 客户端正在等响应，它不会再写），EPOLLOUT 刷空后必须摘掉（不摘就是对着"随时可写"的 fd 自旋，SLO #2 的「空闲 CPU < 5%」第一条就断在这儿）。三处现在各有一条用例盯着。

## 与 Nginx 的功能差异（不是性能差异）

在流量治理这个维度上，本项目功能不比 Nginx 开源版少：

- Nginx OSS 只有被动的 `max_fails` / `fail_timeout`；**主动健康检查是 Nginx Plus 才有的功能**
- Nginx **没有内置熔断器**
- 熔断带**最多摘除比例安全阀**（默认 10%，对齐 Envoy 的 `max_ejection_percent`）——判据失准时最坏只损失 10% 容量，防止熔断本身变成雪崩放大器（[ADR-12](docs/adr/ADR-12-circuit-breaker-ejection-cap.md)）

这两件事不能混着说：**我比 Nginx 多的不是速度，是治理策略**；吞吐基线仍然是 Nginx，而且目标是它的 60%，不是超过它。

## 架构

多线程 Reactor（one loop per thread）+ epoll ET + 配置驱动的 filter 链——**当前是单个 Reactor 线程**，多线程是 S2 的事，但连接对象的所有权与销毁路径从 S1-04 就按"可能被别的线程持有"设计（六步关闭 + 批末延后析构 + 全原子的观测面），S2 的 TSan 基线因此不是从零开始猜。现状模块与实际实现程度见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)——那份文档严格区分"已实现"和"已定但未实现"。

数据流（括号里是当前真实程度，S1-04 之后这条链第一次被事件循环串起来了）：

```
client ──epoll ET──▶ Reactor ──▶ Buffer ──▶ HTTP/1.1 parser
                       │              │
                       │              └─ 原始字节 ──▶ 上游连接（S1-04 已通：一条下游配一条回源，keep-alive 复用；
                       │                              S1-05 已通：双条件 upgrade 命中且上游回出完整 101 头后，
                       │                              这条配对整体转纯字节隧道——不解析、不改写、不重新分帧）
                       │                              上游响应 ──▶ 下游（S1-06 已通：反向同一条形状，但网关现在知道
                       │                              一条响应在哪里结束——七种框架结论 + chunked 边界游标，
                       │                              头没出去时自产 502，头已出去时只以连接终止表态）
                       │                              背压（S1-07 已通：入向可读区顶到停读线就摘掉这条 fd 的
                       │                              EPOLLIN，回落到半线以下才挂回来、并主动探测读一次——
                       │                              摘与挂都是跳变，边沿触发不为已经在里的字节负责）
                       │
               filter chain（配置决定顺序）           ← S1-08，还没有
                       │
        路由 ──▶ 负载均衡 ──▶ 上游连接池                ← S2/S3，还没有
                       │
               熔断 / 限流（共享状态）                  ← S4，还没有
```

现在这条链已经能双向搬字节、响应侧的语义齐了（S1-06：状态码映射、chunked 透传、逐字节一致、两种截断的可定义表态都有断言），**主动背压也落了地**（S1-07：撞停读线就摘这条 fd 的 EPOLLIN，水位回落带一次探测读挂回来；四个方向各一条用例，进程侧四阶段冒烟）。多线程、连接池、策略层一个都还没有；`sizeof(Connection)` 现在是 520（S1-06 响应跟踪 +112、S1-07 的 epoll 掩码回执与"欠一次读"标记 +8），这条口径同步在 ARCHITECTURE 1.11。

网络层全部在本仓库内自行实现，不引入 asio / muduo / brpc（[ADR-04](docs/adr/ADR-04-no-third-party-network-lib.md)）——"网络层怎么写的"就是这个项目要交付的东西。

## 快速开始

环境要求见 [docs/ENVIRONMENT.md](docs/ENVIRONMENT.md)。**构建与压测必须在 Linux/WSL2 原生文件系统内进行**，放在 Windows 挂载盘（`/mnt/d` 之类）上会让 9P 的 IO 抖动直接进 p99，测出来的数既不属于网关也不属于 Nginx。

```bash
cmake --preset debug          # GCC 15.2，日常开发
cmake --build --preset debug
ctest --preset debug          # 当前 112/112（6 工具链 + 9 buffer + 21 解析 + 3 buffer 写出侧 + 11 连接 + 4 连接层隧道 + 10 事件循环 + 5 握手闭环 + 5 101 判定 + 19 响应框架 + 7 转发闭环 + 4 背压闭环 + 6 纯链 + 2 filter 循环）

./build/debug/l7gate --self-check
# version=0.1.0 compiler=gcc 15.2.0 __cplusplus=202002 gtest=1.14.0 \
#   sanitizers=none build_type=Debug malloc=glibc-malloc

# S1-04 的透传路径：起一个上游，把网关架在中间，用真 curl 打一发
./build/debug/tests/l7gate_test_upstream --listen 127.0.0.1:0 --body-bytes 64 &
./build/debug/l7gate --serve --listen 127.0.0.1:18080 --upstream 127.0.0.1:<上面自报的端口>
curl -sS -i http://127.0.0.1:18080/hello
kill -TERM <网关 pid>      # 停止时才打印 [serve] key=value 统计，包括 fd 与 RSS 的首末值
```

`--serve` 打的每一行统计都是给脚本读的，不是给人的漂亮表格；读不到的一律是 `-1` 而不是 `0`——**"没测到"和"没有泄漏"是两件事**。完整的端到端验收在 `bash scripts/smoke-s1-04.sh`（两阶段：单请求逐字节可见 + 真 curl 10000 请求后 fd/存活连接归零），它自己会打印每一项断言的通过与否，退出码就是结论。

三个构建形态与各自的用途：

| preset | 编译器 | 用途 |
|---|---|---|
| `debug` | GCC | 日常开发 |
| `release` | GCC | **压测唯一允许**——sanitizer 构建测出来的是检查开销的延迟 |
| `asan-ubsan` | Clang | S1 门禁（`-O0` 是刻意的，见下） |
| `tsan` | Clang | S2 门禁：连接对象跨线程传递与异步销毁的唯一自动化验证手段 |

> `debug` / `release` 之外的一切数字都不进 `BENCHMARK.md`；sanitizer preset 一律用 `-O0`，因为在 `-O1` 起，"可被证明无副作用的坏写入"会被优化掉，ASan 什么都不报——那种"干净"是假的。

任务收尾时用 `bash scripts/verify-all-presets.sh` 一次性拿到三个 preset 的构建退出码、警告计数与 `ctest` 结果——「ASan 干净」这句话不该靠我手敲，而该有一条命令能复现。

## 明确不做的东西

每条都有 ADR 写明理由，不装作它们不存在：HTTP/2 与 gRPC 与 QUIC、TLS 握手、第三方网络库、etcd/xDS/K8s/Service Mesh、动态服务发现、**响应缓存**（缓存是另一种网关形态，做了它 p99 就不再度量流量治理）、WebSocket 帧解析（只做 Upgrade 后透传）、多进程 `SO_REUSEPORT`、Web 管理后台、自研压测客户端（除 C100K 长连接场景）。

还有一个反向承诺：**io_uring、连接对象池、日志异步化全部挂起**。只有当压测 + perf 火焰图拿出证据证明那是真实瓶颈时才立项，且先写 ADR。在没有证据的情况下优化，是空壳项目的典型形态。

## 文档

| 文档 | 内容 |
|---|---|
| [CLAUDE.md](CLAUDE.md) | 唯一契约：身份约定、边界、SLO、决策、门禁 |
| [TASKS.md](TASKS.md) | 任务表（唯一真相源）：ID / 可断言验收标准 / 状态 / 实测数字 |
| [docs/SLO.md](docs/SLO.md) | 三个数字的目标与写死的测量条件 |
| [docs/ENVIRONMENT.md](docs/ENVIRONMENT.md) | 环境自检实测版本、版本偏离的理由、sanitizer 阳性验证 |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | 已实现的现状 |
| [docs/BENCHMARK.md](docs/BENCHMARK.md) | 压测记录（原始数字 + 结论 + 环境）：B-001 是第一份 p99，B-004 是 S1 收尾那份，**四份都未达标照记** |
| [docs/FAULT-INJECTION.md](docs/FAULT-INJECTION.md) | 故障注入的预期 vs 实测 + sanitizer 结论（每条"干净"都带反证） |
| [docs/ROADMAP.md](docs/ROADMAP.md) | 五阶段与每阶段**门禁结果**：S1 四件套逐条对账、一键复现、实测暴露出来留给 S2 的九条债（每条指着具体数字）、以及五项**没有答复就不动**的事项 |
| [docs/adr/](docs/adr/) | 13 条决策记录 |

原来那句"`docs/ROADMAP.md` 等阶段门禁那份再建——空的证据文件比没有更糟"已经兑现：它是在 S1 门禁**有结果可记**的那一刻建的，内容全部指向已有数字，没有一句新证据。

## 许可

MIT。见 [LICENSE](LICENSE)。
