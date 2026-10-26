# l7gate

单机部署的 L7 流量治理网关：解析 HTTP/1.1，按策略路由、限流、熔断后转发上游，并对整个过程给出**可复现**的性能与故障边界数据。

不是"又一个高性能 Web 服务器"，不是框架，也不是功能清单。对外形态是一个 `docker compose up` 起来、顶替 Nginx 位置的网关。

**S1 完成 · S2 未开始** ｜ C++20 ｜ Linux / WSL2 ｜ 单测 112/112 ｜ MIT

---

## 三个数字

项目的全部价值锚点。测量条件写死在 [docs/SLO.md](docs/SLO.md)，未达标照记。

| # | 数字 | 目标 | 当前读数 | 判定 |
|---|---|---|---|---|
| 1 | 转发增量 p99（1 KiB / 1000 并发 / 60 s / 同机回环） | ≤ 1.0 ms | **29.720 ms**（四份取样 57.540 / 36.420 / 37.890 / 29.720） | ❌ 未达标 |
| 1b | 转发增量 p999 | ≤ 5.0 ms | 取不到——wrk 4.1.0 / hey 0.1.4 分位表只到 99% | ⚪ 无法判定（方案已裁：自研 C100K 客户端做 HDR 分位） |
| 2 | 100k 长连接每**连接对**内存 | ≤ 20 KB | 空闲 5.81 KiB/对象、负载 ≈19.7 KiB/对；100k 未测 | ⚪ 未到测量条件 |
| 3 | QPS ≥ Nginx 同环境实测值的 60% | ≥ 60% | **206%**（nginx 原始态 9,147.69 / l7gate 18,854.32） | ⏳ 有读数，**终判推到 S5**（已裁） |

三格"⚪/"的含义各不相同，我不想被读成同一个意思：#1b 是**工具取不到那个分位**——已裁决在自研 C100K 客户端里做 HDR 分位，不引第三方工具；#2 是**测量条件还不存在**（100k 长连接要 S2 的连接模型，且 `ulimit -n`=10240 是硬前置）；#3 是**数拿到了，但按裁决把终判推到 S5**——nginx 在容器里只能走端口发布，分母被压到同一上游天花板（56,332.52 QPS）的 16%，比值两次取样 158%/206% 散布 30%，契约要求的「同一分配器记录」也没满足；更要紧的是 S1 的单 Reactor 对 16 worker 的 nginx，**赢的是对照组而不是实现**。**拿一个被削弱的分母给自己发达标证明，正是 SLO 里那句"最容易造假的点在于和谁比"。**

第三个数还有附加产出：与 Nginx 的差值必须定位到 HTTP 解析、内存分配、连接管理、日志写盘、系统调用次数中的哪几项。「差多少」谁都能测，**「差在哪」才是这个项目**。

> 证据：[docs/BENCHMARK.md](docs/BENCHMARK.md) B-001…B-005（含每轮环境快照、原始 wrk/hey 输出、口径变更节）。

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

`l7gate --serve --listen <ip:port> --upstream <ip:port>` 起一个**单线程 epoll 边缘触发**循环，已经能：

- 把下游解析出的完整请求按**原始字节**搬到上游，响应原样搬回，keep-alive 复用同一条回源连接
- 认得一条响应在哪里结束：七种框架结论 + chunked 边界游标（只数边界、不重组 body）
- 在头没出去时自产 502，头已出去时只以连接终止表态——**绝不给同一条请求发两条响应**
- 双条件命中（`Connection` 含 `upgrade` 记号 **且** 有 `Upgrade` 头）后整条连接转纯字节隧道：帧不解析、不改写（ADR-09）
- 撞停读线就摘掉这条 fd 的 `EPOLLIN`，水位回落时带一次主动探测读挂回来（边沿触发不为已经在里的字节负责）
- 按配置顺序过 filter 链——**链已立，今天没有业务 filter**

还没有的：路由、负载均衡、连接池、限流、熔断、多线程、指标暴露。

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
| 背压（S1-07） | 四个方向各一条用例：请求方向 384 条 POST 平台期 2,922,610 B（载荷 3,170,580）；响应方向 1 MiB + 客户端一字节不读；隧道方向 2,742,440 B 帧字节平台期 1,898,661 B 且恢复后一字节不少；稳态 1000 请求 `epoll_modifications=3999`＝**4.00 次/请求**（与 S1-04 同口径，背压没多打一枪）。进程侧 `smoke-s1-07.sh` 四阶段，含 `--high-water-mark 4096` 下一条 100 KiB 请求**停在停读线上**（`read_disarmed_connections=1`、curl 零字节、无 5xx）。口径与被删掉的钉窗实验见 ARCHITECTURE 1.15 |
| filter 链（S1-08） | 顺序＝添加顺序（用例故意按 c、a、b 添加）、第一个 `kStop` 之后不再调用后面的、filter 状态跨请求存活；主路径上只有一处链调用；「不改主路径」的可执行形式＝空链与两条放行 filter 下响应内容与七项计数**逐项相等**。TSan 这轮抓到的是我自己写的第一版（循环还在跑时读非原子计数器） |
| sanitizer | `debug` / `asan-ubsan` / `tsan` 各 **112/112**，非 gtest 警告 **0 条**；每条"干净"都带反证（[docs/FAULT-INJECTION.md](docs/FAULT-INJECTION.md) 第 3 节） |

</details>

---

## 阶段进度

| 阶段 | 目标 | 状态 | 门禁 |
|---|---|---|---|
| **S1** | 单连接转发 + 第一份 p99 | ✅ 八个任务全 DONE | 四件套：#2/#3/#4 齐，#1 的 #3 半边按裁决**推到 S5 终判**（理由见上表） |
| **S2** | 多 Reactor + 上游连接池 + 跨线程销毁 | 未开始 | TSan 干净 |
| **S3** | 路由 + 负载均衡 + 健康检查 + 摘除安全阀 | 未开始 | 摘除耗时、误摘率、故障注入实测 |
| **S4** | 限流 + 熔断 + 超时重试防放大 | 未开始 | 429 生效数据、熔断时间线、Redis 降级 |
| **S5** | 热重载 + 指标 + 面板 + vs Nginx 归因 | 未开始 | 三个数字终稿、归因报告、故障注入全表 |

S1 的门禁结论、一键复现命令、以及**实测暴露出来留给 S2 的九条债**（每条都指着具体数字）在 [docs/ROADMAP.md](docs/ROADMAP.md)。S2–S5 只有一句话目标——契约第 4 节要求每个阶段门禁通过时才拆下一阶段，提前拆会写成幻想中的任务表。

---

## 架构与数据流

多线程 Reactor（one loop per thread）+ epoll ET + 配置驱动的 filter 链。**当前是单个 Reactor 线程**，但连接对象从 S1-04 就按"可能被别的线程持有"设计（六步关闭 + 批末延后析构 + 全原子观测面），S2 的 TSan 基线不是从零开始猜。

```
client ──epoll ET──▶ Reactor ──▶ Buffer ──▶ HTTP/1.1 parser
                       │              │
                       │              ├─ 原始字节 ──▶ 上游连接        ✅ S1-04/05（回源复用 + 101 后纯字节隧道）
                       │              ├─ 上游响应 ──▶ 下游            ✅ S1-06（框架判定 + 截断两条口径）
                       │              └─ 停读线 / 探测读恢复          ✅ S1-07（滞回 + 边沿触发的欠账）
                       │
               filter chain（配置决定顺序）   ✅ S1-08 骨架已立，业务 filter 归 S3/S4
                       │
        路由 ──▶ 负载均衡 ──▶ 上游连接池       ⏳ S2/S3
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
ctest --preset debug                      # 112/112（14 个套件，构成见 TASKS.md 交接段）
./build/debug/l7gate --self-check         # version / compiler / gtest / sanitizers / build_type / malloc

# 手动跑一遍透传：起上游 → 把网关架在中间 → 用真 curl 打一发
./build/debug/tests/l7gate_test_upstream --listen 127.0.0.1:0 --body-bytes 64 &
./build/debug/l7gate --serve --listen 127.0.0.1:18080 --upstream 127.0.0.1:<上面自报的端口>
curl -sS -i http://127.0.0.1:18080/hello
kill -TERM <网关 pid>                     # 停止时才打印 [serve] key=value 统计（含 fd 与 RSS 首末值）
```

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
scripts/bench/s1-p99.sh                   # SLO #1 标准口径（canonic 由参数自己判，非标那轮不进文档）
scripts/bench/s2-qps-vs-nginx.sh          # SLO #3 五档：nginx 三档 + 网关 + 直连天花板
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
| [docs/adr/](docs/adr/) | 13 条决策记录 |

---

## 许可

MIT。见 [LICENSE](LICENSE)。
