# l7gate

**一个单机部署的 C++20 L7 流量治理网关**：自研 epoll ET + 多线程 Reactor 网络核，解析 HTTP/1.1，按配置好的 filter 链做路由、负载均衡、健康检查与摘除、限流、熔断、超时与重试——然后把整个过程的性能与故障边界**用可复跑的脚本量出来，包括没达标的部分**。

它不是"又一个高性能 Web 服务器"，不是框架，也不是功能清单。对外形态是一个能顶替 Nginx 位置的网关。

[![C++20 · 无第三方网络库](https://img.shields.io/badge/C%2B%2B20-no%20third--party%20net%20lib-blue)](docs/adr/ADR-04-no-third-party-network-lib.md)
[![单测 383/383 · 三 preset](https://img.shields.io/badge/unit%20tests-383%2F383%20%C3%97%203%20presets-brightgreen)](docs/ROADMAP.md)
[![TSan 0 data race](https://img.shields.io/badge/ThreadSanitizer-0%20races-green)](docs/FAULT-INJECTION.md)
[![SLO #2 达标](https://img.shields.io/badge/SLO%20%232%20100k%20%E5%86%85%E5%AD%98-17.47%20KiB%2F%E5%AF%B9%20%E2%9C%85-green)](docs/BENCHMARK.md)
[![SLO #1 未达标，照记](https://img.shields.io/badge/SLO%20%231%20p99-4.2%E2%80%936.3%20ms%20%E2%9D%8C-red)](docs/SLO.md)
[![MIT](https://img.shields.io/badge/license-MIT-lightgrey)](LICENSE)

## 为什么值得往下看

1. **零第三方库**——网络层、HTTP 解析、JSON 子集解析、令牌桶、一致性哈希、SHA-1/base64 全部本仓库自研，一个第三方库都没引。
2. **治理功能在"流量治理"维度不比 Nginx 开源版少**——Nginx OSS 没有主动健康检查（那是 Plus 的功能），也没有内置熔断器；这两样这里都有，还带摘除比例安全阀与半开恢复。
3. **每个数字都写死测量条件、一键可复跑，未达标就标红留在首屏**——`#1` 到现在仍未达标，`#1b` 那一档明确记"取不到"。

## 核心数据

项目的全部价值锚点。目标值写死在 [docs/SLO.md](docs/SLO.md)，**任何情况下不许为了迁就实测而改目标**；未达标照记并归因。完整读数与归因在 [docs/BENCHMARK.md](docs/BENCHMARK.md)（B-001…B-022）。

| # | 指标（测量条件写死） | 目标 | 当前读数 | 判定 |
|---|---|---|---|---|
| **1** | 转发增量 p99 · 1 KiB 体 / 1000 并发 / 60 s 稳态 / 同机回环 | ≤ 1.0 ms | HEAD 终稿（S5-07，`canonic=1`，按批次分开报）：workers=16 全带 **−0.740 ~ 4.480 ms**；workers=1 跨批带 **31.910 ~ 54.440 ms**——同一台机器同一份 binary 一天之内能散布 20 ms | ❌ **未达标照记**（最好一发 3.140 ms，超目标 3.1 倍；目标值与五条测量条件一个字没动） |
| **1b** | 转发增量 p999 · 同一档 | ≤ 5.0 ms | 两臂稳态窗口已抬过 60 s 门、两发 `rc=0`，但观察者仍占单核 0.98 / 0.99——只剩仪器自己饱和 | ⚪ **明确记"取不到"，且这是永久口径**：多线程观察者已于 2026-11-05 裁"不立项"（[已裁 ⑫](TASKS.md)），这一格是 1000 并发闭环这一档的真实边界，不拿别的工具去顶它 |
| **2** | 100k 连接对稳态内存 + 空闲 CPU | ≤ 20 KB / < 5% | **17.47 KiB/对**（两轮同一数）、稳态 15 样本散布 **0 KiB**、空闲 CPU 占单核 **0.26%** | ✅ **达标** |
| **3** | QPS ≥ Nginx 同环境实测值的 60%（终判档＝两侧都进容器） | ≥ 60% | `--workers 16` 五发全带 **104.41% ~ 111.34%**；`--workers 1` → **78.58% ~ 80.27%** | ✅ **达标**（对称档远离阈值） |

<details>
<summary><b>四个判定各自的含义与完整读数</b>（点开）</summary>

**三个图标各有各的含义，不想被读成同一个意思**：`#1` 是**测到了、判未达标**（六轮 `canonic=1`，跨零点那一轮照记）；`#1b` 是**工具取不到那个分位**（已裁决在自研客户端里做 HDR，不引第三方工具；S5-07 把两臂稳态窗口都抬过 60 s 之后仍然取不到，卡点只剩观察者自己贴核）；`#3` 是**测到了、且在对齐过的档位上判达标**。而 `#2` 那一格曾经写着"`ulimit -n`=10240 是硬前置"——2026-10-27 实测推翻（soft 可自抬到 hard=1048576），真墙是回源那一跳的单元组容量只有 **28,232**，解开它的是 S2-06a（端点列表 + per-worker 静态映射，容量 = `min(N,E) × 28,232`）。

**首屏那枚红徽章是故意的。** `#3` 在终判档（两侧都进容器、`--workers 16` 与 nginx 的 `worker_processes auto` 对齐）上是 **110.32% ~ 111.34%**，过了 60% 那条线；而当年那句批评原样留着——**赢的是对照组，不是实现**——因为它说的是**旧档**：裸进程网关打容器 nginx，分母被端口发布与容器出向压到同一上游天花板的 16%，比值 206%、176%、241% 都好看，可那些数里有一大半是对面被削出来的。S5-07 做的事就是把那一维对齐，然后重新量一次（[B-022](docs/BENCHMARK.md)）。

**#1 的完整读数**（HEAD 终稿 S5-07，`canonic=1`，按批次分开报、不合并）：workers=16 → 下午三发 **3.140 / 4.090 / −0.740 ms**、晚上两发 **4.480 / 2.730 ms**（**含负数**：直连 29.850 反高于经网关 29.110，这一档增量的分辨率已被仪器带宽吃穿）；workers=1 → 下午三发 **43.330 / 50.840 / 51.280 ms**、晚上三发 **31.910 / 33.640 / 33.900 ms**。**同一台机器同一份 binary 一天之内能散布 20 毫秒，所以"退化了多少"这句不写**（一度写下"单 Reactor 退化 16~18 ms"，被晚上那批复核掉，现场更正见 [B-021](docs/BENCHMARK.md) 判定归因第 (3) 条）。对照 B-008（S2 时代）：w16 当年 4.160–6.290、w1 当年 32.990–35.210。

**#1b 的完整读数**：两臂稳态窗口都抬过 60 s 门（直连 70,017 / 71,057 ms、经网关 62,052 / 63,951 ms，等式与生命周期全闭合、两发 `rc=0`），两臂观察者仍占单核 0.98 / 0.99，增量读 16.128 / 26.368 ms——同一份分布的 p99 增量与 wrk 那条线差 **4–6 倍**。B-009 当年"窗口不够"那个解释到此被排掉，只剩仪器自己饱和。

**#2 的完整读数**（HEAD 两轮 S5-07 逐项复现 B-007）：**17.47 KiB/对**、稳态 15 样本散布 **0 KiB**、空闲 CPU 占单核 **0.26%** / 占整机 **0.02%**、`established=100000/100000`、`constructed==destructed==200000`、`fd 40→40`、每端点自报 25,000 条。**唯一动的一格是空进程基线 RSS：4,444 → 7,624 KiB**（S3~S5 的代码本体涨了 3.2 MiB，与每连接持有量无关，判据本来就把它减掉）。

**#3 的完整读数**：`--workers 16` → **110.90% / 110.92% / 111.34%**，两发反序（网关先跑）**110.32% 与 104.41%**；`--workers 1`（shipped 缺省）→ **78.58% / 79.45% / 80.27%**。对照**旧档**是 176.25% / 241.15%，而它的分母自己漂 **45.8%**、且网络路径不对称——那一档只作对照。仍不齐的一条是契约写的「同一分配器记录」：nginx 用它镜像里的 libc，网关是宿主 glibc 2.43，照记。

**#3 的附加产出**：与 Nginx 的差值必须定位到 **HTTP 解析 / 内存分配 / 连接管理 / 日志写盘 / 系统调用次数**中的哪几项。**"差多少"谁都能测，"差在哪"才是这个项目**（终判与归因在 [B-022](docs/BENCHMARK.md)；火焰图按 [ENVIRONMENT 4.1](docs/ENVIRONMENT.md) 的已验证形态取——perf 直接拉起被测进程、只有用户态栈、`perf_event_paranoid` 保持 2 不抬。**nginx 那一臂"同法"本轮实测取不到**，三条理由与替代口径在 [ENVIRONMENT 4.2](docs/ENVIRONMENT.md)——差就差在"要给锁定的基线镜像加一层"，那属禁令三，**2026-11-05 已裁：不加**（[已裁 ⑪](TASKS.md)——裁的是代价不是能力：那一层装得上、图出得来，但对照分母一换身份，B-005/B-022 的比值从此不同轴）。所以 #3 的归因是**定形的**：网关侧有用户态 self 榜，nginx 侧只有结构对照。

</details>

## 特性一览

全部已落地，每条都有可断言的验收句与落盘证据。逐条读数在 [TASKS.md](TASKS.md) 各行末列，故障实测在 [docs/FAULT-INJECTION.md](docs/FAULT-INJECTION.md)，成本对照在 [docs/BENCHMARK.md](docs/BENCHMARK.md)。

### 网络核（全部自研，零第三方库）

| 能力 | 关键读数 |
|---|---|
| **epoll ET + 多线程 Reactor**：一个 acceptor + N worker round-robin，连接对象单线程拥有、跨线程只传裸 fd | `tsan` 零 data race（摘掉 handoff 锁就报 3 处）· [ADR-14](docs/adr/ADR-14-connection-ownership-handoff.md) |
| **HTTP/1.1 解析**：半包/粘包、`Content-Length` 与 chunked、chunked 只数边界不重组 body | 22 个错误码、37 类畸形输入；release `-O3` 595 MB/s |
| **keep-alive 复用**：pipeline 按 FIFO 回送、`Connection: close` 在它自己那条响应后兑现 | [ADR-16](docs/adr/ADR-16-request-response-pairing.md) |
| **WebSocket**：Upgrade 后整条连接转纯字节隧道，帧不解析、不改写 | [ADR-09](docs/adr/ADR-09-websocket-passthrough.md) |
| **背压**：撞停读线摘 `EPOLLIN`、水位回落带一次主动探测读挂回 | 稳态 1000 请求 `epoll_modifications=3999`＝4.00 次/请求 |
| **优雅停止 / 排水**：空闲立即关、在飞逐条答完再关、半包走超时兜底、隧道只能硬关 | 100k 负载下排水实测 **0.947 s**（B-010） |

### 流量治理

| 能力 | 关键读数 |
|---|---|
| **filter 链**：顺序由配置决定，第一个 `kStop` 之后不再调用后面的 | [ADR-11](docs/adr/ADR-11-filter-chain.md) |
| **路由**：前缀最长命中优先、正则全串锚定且优先于前缀、无匹配回 404 且不回源 | 实测 `upstream_opened=0` · [ADR-18](docs/adr/ADR-18-routing-filter-and-target-bucketed-pool.md) |
| **负载均衡三策略**：轮询 / 最少连接 / 一致性哈希（每 worker 一份状态、全非原子） | 12 条请求 3 台各 4 条；N→N+1 迁移率 **24.41% / 11.11%**（界 25% / 12.5%）· [ADR-19](docs/adr/ADR-19-load-balance-and-config-file.md) |
| **主动健康检查 + 摘除安全阀**：被动记账 + 主动探活（2xx/3xx/4xx 活、5xx 死）+ 最多摘除比例默认 10% | 摘除耗时 **202 ms**、误摘 **0/3480**、10 台全死停在 **1/10** · [ADR-12](docs/adr/ADR-12-circuit-breaker-ejection-cap.md) |
| **熔断两个输入 + 半开限流恢复**：滑窗错误率 + 连续失败；到期只放 `half_open_requests` 条且钉给该成员、半开继续占阀位 | [ADR-12 追加二](docs/adr/ADR-12-circuit-breaker-ejection-cap.md) |
| **限流 → 429**：本地令牌桶按键分桶、全 worker 共用同一份桶表；**Redis 分布式计数**，挂掉后拒绝能力不消失 | 4 worker × burst=20 实测总放行恰好 20；Redis 挂了六连击仍 `3×200 + 3×429` · [ADR-05](docs/adr/ADR-05-redis-ratelimit-only.md) |
| **三格请求级超时 + 自产 502/503/504**：绝不同一条请求发两条响应 | [ADR-21](docs/adr/ADR-21-request-timeouts-and-ratelimit.md) / [ADR-22](docs/adr/ADR-22-self-produced-error-replies.md) |
| **connect 失败换成员重试**：预算 1 且结构性防级联 | 放大倍数实测 **1.50 / 顶格 2.00**，一条请求都没被复制 · [ADR-23](docs/adr/ADR-23-connect-failure-retry.md) |
| **配置热重载**（不是热重启）：不可变世代 + 一次 `atomic<shared_ptr>` 发布；一条连接都不关、一个 fd 都不迁 | 生效 **133–205 ms** 实测 · [ADR-10](docs/adr/ADR-10-hot-reload-not-hot-restart.md) |
| **成员表可来自 MySQL**：策略在文件、成员在数据库；MySQL 挂了按已加载配置继续服务 | 8.4.11 ↔ 8.0.36 连通、降级零中断 · [ADR-20](docs/adr/ADR-20-mysql-node-table-and-libmysqlclient.md) |
| **上游连接池**：per-worker、按目标分桶，上游活得比下游久 | 20 条短命下游 → `upstream_opened=1`、`pool_borrows=19` · [ADR-15](docs/adr/ADR-15-perworker-upstream-pool.md) |

### 可观测面

| 能力 | 关键读数 |
|---|---|
| **`/metrics` 指标面**：Prometheus text 0.0.4 纯自实现、独立端口、默认关闭；键名与退出账逐字相同 | 抓取一条业务请求都不多计 · [ADR-24](docs/adr/ADR-24-metrics-exposition.md) |
| **Prometheus 抓取 + Grafana「网关总览」六格** + 只读落盘摘要的旁路 percentile exporter | 面板 20 条表达式是唯一真相，冒烟 grep 同一串 · [ADR-25](docs/adr/ADR-25-bench-percentile-exporter.md) |
| **WebSocket 调试页**：地址栏 + 消息流 + 输入框，限流时显示 `429 rate limited` | 取数证据是 DOM 断言不是人眼 · [ADR-26](docs/adr/ADR-26-ws-echo-upstream-and-headless-dom.md) |

### 交付形态

| 能力 | 关键读数 |
|---|---|
| **`docker compose` 一键起全套**：网关 + 两台上游 + exporter + Redis + MySQL + Prometheus + Grafana，八件一条 `up -d` | 十四条冒烟 **66 项判定**读 `verdict.txt` · [ADR-27](docs/adr/ADR-27-compose-one-command-stack.md) |
| **`tc netem` 网络层故障注入六臂**（非特权 user+netns 的回环） | 慢与死的分界线在 `probe_timeout/4`——S5-12 把它扫到 5 ms：**阶跃在「一次探活过不过期」那一层，界 ∈ (70, 75] ms**；「摘不摘」是它的二次函数，75/80 属带内翻动区（**§29**）；实测**安全阀在挡摘除** · [FAULT-INJECTION §25](docs/FAULT-INJECTION.md) |
| **十四个冒烟脚本 + 全部压测一键复跑**：判定读落盘摘要，不信退出码 | 见[复现与验证](#复现与验证) |

<details>
<summary><b>原始特性大表（含每条的一手证据与阶段归属）</b>（点开）</summary>

| 能力 | 落地 | 一手证据 |
|---|---|---|
| HTTP/1.1 解析：半包/粘包、`Content-Length` 与 chunked、七种框架结论、chunked 只数边界不重组 body | S1-03/06 | 22 个错误码、37 类畸形输入（契约要求 ≥12）；`smoke-s1-06.sh` 八阶段逐字节比对；release `-O3` 595 MB/s（**归因用，不是 SLO**） |
| keep-alive 复用、**pipeline 按 FIFO 回送**、`Connection: close` 在**它自己那条**响应后兑现 | S1-05 / S2-04 | [ADR-16](docs/adr/ADR-16-request-response-pairing.md)；修掉 S1-07 那笔"400 条只回 1 条"的债 |
| **Upgrade 后整条连接转纯字节隧道**：帧不解析、不改写（`Connection` 含 upgrade 记号**且**有 `Upgrade` 头，双条件才转） | S1-05 | [ADR-09](docs/adr/ADR-09-websocket-passthrough.md)；S2-05 阶段四断"收到的恰好 = 101 头 + 发出的 2048 帧字节" |
| 背压：撞停读线摘 `EPOLLIN`、水位回落带一次主动探测读挂回（边沿触发不为已在里的字节负责） | S1-07 | ARCHITECTURE 1.15；四方向各一条用例，稳态 1000 请求 `epoll_modifications=3999`＝**4.00 次/请求**（背压没多打一枪） |
| **filter 链**：顺序由配置决定，第一个 `kStop` 之后不再调用后面的 | S1-08 | [ADR-11](docs/adr/ADR-11-filter-chain.md)；"不改主路径"的可执行形式＝空链与两环链下响应内容与七项计数**逐项相等** |
| 多线程 Reactor：一个 acceptor + N worker round-robin，**连接对象单线程拥有、跨线程只传裸 fd** | S2-01 | [ADR-14](docs/adr/ADR-14-connection-ownership-handoff.md)；`tsan` 零 data race 是这条的**可证伪形式**（摘掉 handoff 锁就报 3 处） |
| 上游连接池：**per-worker、按目标分桶**（下游关闭时干净的上游归还复用，而不是随之销毁——上游活得比下游久） | S2-02 / S3-01 | [ADR-15](docs/adr/ADR-15-perworker-upstream-pool.md)；20 条短命下游 → `upstream_opened=1`、`pool_borrows=19`；空闲上游被对端关是**正常回收**、不误计截断 |
| 优雅停止 / 排水：空闲立即关、在飞逐条答完再关、半包走超时兜底、**隧道只能硬关** | S2-05 | ARCHITECTURE 1.20；实测 0.11 s / 0.81 s / 100k 负载下 **0.947 s**（B-010） |
| 路由：前缀**最长命中**优先、正则**全串锚定且优先于前缀**、无匹配回 404 **且不回源** | S3-01 | [ADR-18](docs/adr/ADR-18-routing-filter-and-target-bucketed-pool.md)；实测 `upstream_opened=0`、另一台自报 `connections_accepted=0` |
| 负载均衡三策略：轮询 / 最少连接 / 一致性哈希（**每 worker 一份状态、全非原子**） | S3-02 | [ADR-19](docs/adr/ADR-19-load-balance-and-config-file.md)；12 条请求 3 台**各 4 条**；N→N+1 迁移率实测 **24.41% / 11.11%**（界 25% / 12.5%） |
| 被动记账 + **主动探活**（2xx/3xx/4xx 活、5xx 死）+ **最多摘除比例安全阀**（默认 10%） | S3-03 | [ADR-12](docs/adr/ADR-12-circuit-breaker-ejection-cap.md)；摘除耗时 **202 ms**、误摘 **0/3480**（带分母）、10 台全死停在 **1/10**（B-014） |
| 成员表可来自 **MySQL**：`"source":"mysql"` + 周期 reload，**策略在文件、成员在数据库**；MySQL 挂了按已加载配置继续服务 | S3-04 | [ADR-20](docs/adr/ADR-20-mysql-node-table-and-libmysqlclient.md)；**8.4.11 ↔ 8.0.36** 首次连通取证五步全通、reload 生效 **406/536 ms**、降级零中断（B-015） |
| 三格请求级超时（连接 / 响应头 / body 空闲）与回源失败的**自产 502 / 503 / 504**，绝不同一条请求发两条响应 | S4-01/02 | [ADR-21](docs/adr/ADR-21-request-timeouts-and-ratelimit.md) / [ADR-22](docs/adr/ADR-22-self-produced-error-replies.md)；结掉 S2-06 那 **71,769** 条客户端超时旧债 |
| **本地令牌桶限流 → 429**：按键分桶、**全 worker 共用同一份桶表**、键数硬上界 16384 | S4-03 | 4 worker × burst=20 实测总放行**恰好 20**（不是 80）；同 path 六连击 `200 200 200 429 429 429`；每 worker 一只桶那个退化 → 用例红 |
| **Redis 分布式计数**：Redis 授予额度、本地桶执行（CAS 里 `min(tokens, cap)` 的"上限夹子"）；挂掉后**拒绝能力不消失**，恢复后自动退出降级 | S4-04 | [ADR-05 追加节](docs/adr/ADR-05-redis-ratelimit-only.md)、B-017（cpuset 只绑 Redis，实测生效）；`hiredis 1.2.0 ↔ redis 7.2.5` 七步探针 |
| **熔断两个输入 + 半开限流恢复**：滑窗错误率管"连续计数被成功清零"那种形状；到期只放 `half_open_requests` 条且**被放行那条被钉给那个成员**；**半开继续占安全阀的位** | S4-05 | [ADR-12 追加二](docs/adr/ADR-12-circuit-breaker-ejection-cap.md)；`自报 6 == 摘除前 4 + 门放行 2` 逐条对上；八处阳性验证全红（其中一处第一版 0 红，是**用例的失效不是实现的清白**） |
| **connect 失败换成员重试，预算 1 且结构性防级联**（重放那条的备选字段被盖成"已用掉"的记号） | S4-06 | [ADR-23](docs/adr/ADR-23-connect-failure-retry.md)；放大倍数实测 **1.50 / 顶格 2.00**，且**后端自报实收 == 客户端请求数**（一条都没复制）；半开那一臂 `after_retry=0`（探测请求没被换台救活） |
| 配对账的"谁解链谁减账"（`least_conn` 那把尺曾经**只加不减**、永不自愈） | S4-06a | [ADR-19 追加五](docs/adr/ADR-19-load-balance-and-config-file.md)；**这条改动修前修后全量都是绿的**，所以"改完还是绿的"在这里不提供任何信息，验收主体是两处退化 |
| **配置热重载**（不是热重启）：不可变世代 + 一次 `atomic<shared_ptr>` 发布 + 引用计数延命；**一条连接都不关、一个 fd 都不迁** | S5-02 | [ADR-10 追加节](docs/adr/ADR-10-hot-reload-not-hot-restart.md)；生效 **133–205 ms** 实测（节拍 200 ms，粒度就是节拍——**没写成"秒级"**）；三条在飞请求全部走完旧表 |
| 运行期指标面 `/metrics`：Prometheus text 0.0.4 **纯自实现**、独立 `--metrics-port`、默认关闭；键名与退出账**逐字相同** | S5-01 | [ADR-24](docs/adr/ADR-24-metrics-exposition.md)、B-019；主档四臂 `7,199,900 / 1,800,000 = 3.999944` **同一个整数** |
| **Prometheus 抓取 + Grafana「网关总览」六格** + 一个只读落盘摘要的旁路 percentile exporter | S5-03 | [ADR-25](docs/adr/ADR-25-bench-percentile-exporter.md)；面板那 20 条表达式是唯一真相，冒烟 grep 同一串；`429` 静置臂 **34==34**、扰动臂 **49==49** |
| **WebSocket 调试页**：地址栏 + 消息流 + 输入框，限流时显示 `429 rate limited`，取数证据是 **DOM 断言**不是人眼 | S5-04 | [ADR-26](docs/adr/ADR-26-ws-echo-upstream-and-headless-dom.md)；`文案 7 == 哨兵 7 == Δrate_limit_rejected 7`；`closed code=1005` 两侧配对归零 |
| **`docker compose` 一键起全套**（八个服务一条 `up -d`；壳镜像**不 COPY 产物**、被网关引用的地址走显式静态 IPAM） | S5-05 | [ADR-27](docs/adr/ADR-27-compose-one-command-stack.md)；十四条冒烟第 14 条十一个阶段 **66 项判定**读 `verdict.txt`；容器之间按服务名解析通，**但网关不做名字解析**（`inet_pton`，服务名那一版实测 502 + `upstream_connect_failed`） |
| **`tc netem` 现场六臂**（执行场所＝非特权 user+netns 的回环；宿主回环开工前后各核一次，全程 `noqueue`） | S5-06 | [FAULT-INJECTION §25](docs/FAULT-INJECTION.md) 与 [ENVIRONMENT 5.4](docs/ENVIRONMENT.md)：**网络层一次探活 ≈4×delay**，慢与死的分界线在 `probe_timeout/4`（不是 300 ms）；三台一起超时造出应用层从来没有的一格——**安全阀在挡摘除**（`blocked_by_valve` 155→410 而 `ejected_now` 峰值只有 1） |
| **三个数字终稿 + vs Nginx 差值归因**（终判档＝两侧都进容器、同一种 `-p` 发布） | S5-07 | [B-021](docs/BENCHMARK.md) / [B-022](docs/BENCHMARK.md) 与上面「核心数据」那四行；脚本 `scripts/bench/s5-07-qps-symmetric.sh`、`s5-07-flamegraph.sh`。**归因两边不对称这件事明写**：网关侧有用户态 self 榜，nginx 侧同法火焰图在这台机器上取不到（三条实测在 [ENVIRONMENT 4.2](docs/ENVIRONMENT.md)），"nginx 慢在哪一层"那句今天没有证据 |
| **S5 门禁收尾**（四件套与另加三样逐项指路、§25.3 欠的两条阳性验证补打、三笔待决逐笔给状态） | S5-08 | [docs/ROADMAP.md](docs/ROADMAP.md)「## S5」节 + [FAULT-INJECTION §26](docs/FAULT-INJECTION.md)：D4 谎报注入红 **3** 条、D5 清场失灵红 **3** 条，还原后与备份 `cmp` 逐字节相同、复跑 `PASS checks=22 fails=0` |

**这一阶段八条全 DONE（2026-11-04 关门，门禁对账在 [docs/ROADMAP.md](docs/ROADMAP.md)「## S5」节）；关门之后追加一条 S5-09**——它结掉的正是 S5-05 抓到、后面三条任务都不许顺手改的那条缺陷：配置里的 `address` 现在**只收 IPv4 字面量**，不是就整份拒载 + 点名（[ADR-28](docs/adr/ADR-28-ipv4-literal-validation-at-config-parse.md)，修法由项目所有人 2026-11-04 选定；状态与门禁读数见 TASKS 的 S5-09 行）。剩下的都不是功能缺口，而是**排期与裁决**，原样列在 [TASKS.md](TASKS.md)「已知待决事项」与 ROADMAP 的 S5 债表里：nginx 基线镜像加 perf 层（禁令三／契约 9.1，归项目所有人；可行性已探完——Debian 12 里 `apt install linux-perf` 装得上、由 perf 直接拉起 nginx 出得来 3K 样本与 62 条 `ngx_` 符号）、`l7gate_c100k` 观察者多线程（`tools/` 的测量手段 + 一条 ADR，按 ADR-17 先例；按建议**暂不立项**），另有七格"写在纸上没测"的账与三条候补。**没有一笔被读成已闭合。**

</details>

## 快速开始

> 构建与压测**必须在 Linux/WSL2 原生 ext4 内**。放在 `/mnt/d` 这类挂载盘上，9P 的 IO 抖动会直接进 p99——测出来的数既不属于网关也不属于 Nginx（契约 9.4）。环境要求与实测版本见 [docs/ENVIRONMENT.md](docs/ENVIRONMENT.md)。

### 构建 + 全量单测

```bash
cmake --preset debug && cmake --build --preset debug
ctest --preset debug              # 383/383（asan-ubsan / tsan 里各有 2 条 Skipped：
                                  #   sanitizer 换分配器后 mallinfo2() 一律报 0 → 跳过＝不判、不是通过）
./build/debug/l7gate --self-check # version / compiler / gtest / sanitizers / build_type / malloc
```

### ① 最简转发

上游用仓库自带的测试夹具，端口给 `0` 让内核选，启动行会打印真实端口：

```bash
./build/debug/tests/l7gate_test_upstream --listen 127.0.0.1:0 --body-bytes 64 &
./build/debug/l7gate --serve --listen 127.0.0.1:18080 --upstream 127.0.0.1:<上面打印的端口>
curl -sS -i http://127.0.0.1:18080/hello
kill -TERM <网关 pid>             # 停止时才打 [serve] key=value 退出账（含 fd 与 RSS 首末值）
```

### ② 策略配置

`--upstream` 与 `--config` **二选一**，两个都给或都不给都是退出码 2（后端表有两个来源时，"哪一份是真相"本身就是一份会漂移的配置）：

```jsonc
// deploy/gateway.json —— 这一份会被 ctest 读，不是文档里的示意图
{
  "upstreams": [ { "name": "api",
                   "balance": "round_robin",        // round_robin | least_conn | hash
                   "hash_key": "x-user",            // 仅 balance=hash 需要
                   "targets": [ { "address": "127.0.0.1", "port": 18081 },
                                { "address": "127.0.0.1", "port": 18082 } ] } ],
  "routes": [ { "match": "prefix", "pattern": "/api", "upstream": "api" } ],
  "health":   { "max_fails": 3, "fail_timeout_ms": 5000, "probe_interval_ms": 1000,
                "max_ejection_percent": 10 },
  "circuit":  { "window_ms": 10000, "minimum_requests": 20, "error_rate_percent": 50,
                "half_open_requests": 2, "half_open_successes": 2 },   // 要求 health 段同在
  "rate_limit": { "rate": 100, "burst": 50, "key": "path" }             // 段不存在 = 不装限流
}
```

```bash
./build/debug/tests/l7gate_test_upstream --listen 127.0.0.1:18081 --body-bytes 64 &
./build/debug/tests/l7gate_test_upstream --listen 127.0.0.1:18082 --body-bytes 128 &
./build/debug/l7gate --serve --listen 127.0.0.1:18080 --config deploy/gateway.json
curl -sS http://127.0.0.1:18080/api/users | wc -c   # 轮询：这一发 64
curl -sS http://127.0.0.1:18080/api/users | wc -c   # …下一发 128（同一组里换了一个成员）
kill -TERM <网关 pid>                               # 横幅里的 policy=on 才是"配置真生效"的签名
```

JSON 子集解析器在本仓库里**手写、不引第三方库**，为的是"读不出就整体失败"这件事有地方表达：未知字段、重复键、尾逗号、注释、浮点、`balance` 拼错、空 `targets`、vnodes 越界、正则编不过、路由指向不存在的组——**每一条都是拒绝**，退出码 2 且话里点名是哪一项，绝不"退回默认配置继续跑"。

### ③ 一键起全套演示面

```bash
cmake --build --preset release -j 16        # **先构建**：产物是挂进容器的，不是打包进镜像的
docker compose -f deploy/compose/docker-compose.yml up -d --build
curl -s http://127.0.0.1:18080/hello        # 业务面
# Grafana http://127.0.0.1:13000（admin / l7gate-demo）、Prometheus http://127.0.0.1:19090
docker compose -f deploy/compose/docker-compose.yml down --remove-orphans
docker ps -a --filter name=l7gate-          # 停栈后这一条必须是空的
```

八件一起：网关 + 两台上游 + 旁路 exporter + Redis + MySQL + Prometheus + Grafana。**这一栈只服务演示面**——三个数字的判定仍走裸进程与 `scripts/bench/` 的落盘摘要（[ADR-27](docs/adr/ADR-27-compose-one-command-stack.md)）。

<details>
<summary><b>更多配方与命令行总览</b>（点开）</summary>

**命令行总览**（`--upstream` 与 `--config` 二选一；给 `--self-check` 只打印构建指纹）：

```
l7gate --serve --listen <ipv4:port>
        (--upstream <ip:port>[,<ip:port>...] | --config <path>)
        [--workers N] [--serve-requests N] [--max-connections N]
        [--high-water-mark BYTES] [--drain-timeout-ms MS]
        [--config-reload-ms MS] [--metrics-port PORT]
        [--upstream-connect-timeout-ms MS] [--upstream-head-timeout-ms MS]
        [--upstream-body-idle-timeout-ms MS]
        [--nodes-db <host:port> --nodes-user U --nodes-password P --nodes-schema DB
         --nodes-reload-ms MS --nodes-timeout-ms MS]
        [--redis-db <host:port> --redis-settle-ms MS --redis-timeout-ms MS]
```

划分只有一条：**部署事实留在命令行，流量策略留在 `--config`**。所以监听地址、`--workers`、Redis 与重载节拍这些都不进配置文件——配置文件里出现 `listen`/`workers` 会被当成"本配置不认识的字段"当场拒载并点名。`--metrics-port 0` 是"内核给端口"（测试靠它），**关闭的形态是这个标志缺席**。完整清单以 `l7gate` 不带参数打出的 usage 为准。

```bash
# 热重载（S5-02）：改文件、不重启、不关任何连接。缺省 0 = 关着，与不带这个开关逐字节相同
./build/debug/l7gate --serve --listen 127.0.0.1:18080 --config deploy/gateway.json --config-reload-ms 200
#   改一下 /api 那条路由指的组，再 curl 一次：体长换了，pid 没换
#   退出账那族键（一行一个键）：config_gen / config_publishes / config_checks /
#   config_rejects{,_syntax,_scope} / config_snapshots_live / config_last_publish_epoch_ms
#   写坏了：旧那一代继续服务、config_rejects_* +1、stderr 那句话点到具体是哪个字段
#   定义域外（会拒载并点名）：rate_limit 段的任何变化、段的出现/消失、文件里写 listen/workers、source:mysql 的组

# 熔断（S4-05）：坏的那台在 /boom 上回 503，好的那台回 200
./build/debug/tests/l7gate_test_upstream --listen 127.0.0.1:18081 --body-bytes 64 &
./build/debug/tests/l7gate_test_upstream --listen 127.0.0.1:18082 --body-bytes 64 --path-status "/boom:503" &
./build/debug/l7gate --serve --listen 127.0.0.1:18080 --config deploy/gateway-circuit.json
for i in $(seq 1 40); do curl -s -o /dev/null -w '%{http_code} ' http://127.0.0.1:18080/boom; done
#   攒够 minimum_requests 之后坏的那台被窗口判摘；此后它的体长不再是 64

# 限流交给 Redis 聚合（S4-04；不给 --redis-db 就还是纯本地桶）
docker run -d --name l7gate-redis --cpuset-cpus 15 -p 127.0.0.1:16379:6379 \
  redis:7.2.5 --save "" --appendonly no      # 契约锁 7.2.5：不是 latest，也不是本机那个 8.0
./build/debug/tools/l7gate_redis_probe --port 16379   # 七步连通取证（含产品那份 Lua）
./build/debug/l7gate --serve --listen 127.0.0.1:18080 --config <你的配置> \
  --redis-db 127.0.0.1:16379 --redis-settle-ms 200 --redis-timeout-ms 1000
curl -sS -o /dev/null -w '%{http_code}\n' http://127.0.0.1:18080/same    # 超速率仍是 429
docker stop l7gate-redis    # 六连击仍是 3×200 + 3×429：拒绝能力不消失
docker start l7gate-redis   # 不用重启网关，redis_degraded 自己回到 0

# 观测面 + 调试页（S5-01/03/04）
./build/debug/l7gate --serve --listen 0.0.0.0:18080 --config deploy/gateway.json --metrics-port 18522 &
curl -s http://127.0.0.1:18522/metrics | head      # 77 个固定键 + 每目标 10 键，与退出账同名同口径
./build/debug/tests/l7gate_test_upstream --listen 127.0.0.1:18941 --ws-echo --html demo/ws-echo.html &
./build/debug/l7gate --serve --listen 0.0.0.0:18940 --config build/smoke-s5-04/gw.json --metrics-port 18524 &
# 浏览器打开 http://127.0.0.1:18940/demo/ws-echo.html —— 页面必须经网关同源提供（见 ADR-26）
```

`--serve` 打的每行统计是给脚本读的，不是给人的漂亮表格；**读不到的一律是 `-1` 而不是 `0`——"没测到"和"没有泄漏"是两件事。**

</details>

## 架构与数据流

```
client ──epoll ET──▶ acceptor ──round-robin（跨线程只传裸 fd）──▶ N × worker Reactor
                                                                    │
                              filter chain（顺序由配置决定）        ├─ Buffer（一次必须读到 EAGAIN）
                              ├─ rate_limit     → 429，不回源        ├─ HTTP/1.1 parser（纯函数、不碰 fd）
                              ├─ route          → 哪一组，404 不回源 └─ 响应框架判定（CL / chunked / 截断两条口径）
                              └─ load_balance   → 组里哪一个
                                                                    │
                        HealthTable（被动记账 + 主动探活 + 滑窗错误率 + 半开 + 摘除安全阀）
                                                                    │
                        上游连接池 per-worker、按目标分桶 ──▶ 回源（三格超时 / 自产 502·503·504 / connect 失败换台重试）
                                                                    │
                        ConfigSnapshot（不可变世代，一次 atomic store） · /metrics 观测面（独立线程，不进业务计数）
```

网络层全部在本仓库内自行实现，不引入 asio / muduo / brpc（[ADR-04](docs/adr/ADR-04-no-third-party-network-lib.md)）——**"网络层怎么写的"就是这个项目要交付的东西**。

三条结构性决定，它们决定了别的东西能不能做：

1. **连接对象单线程拥有。** 池、限流游标、健康状态都可以是 per-worker 的非原子量，因为"两个线程碰同一条连接"在结构上不成立——跨线程流动的是裸 fd（[ADR-14](docs/adr/ADR-14-connection-ownership-handoff.md)）。`tsan` 零 data race 是这条的**可证伪形式**而不是装饰：S2-01 摘掉 handoff 锁 → 报 3 处 race，S2-03 的 churn → 报 14 处，S2-05 把排水标志做成普通 `bool` → 5 处。**能红的绿才算数。**
2. **决策与执行分开。** 路由是纯函数（不建 socket、不碰 fd），选择与摘除是每 worker 的活状态，阻塞 IO（MySQL reload、Redis 结算）各在专用线程——所以三格超时、探活、reload 谁都不进 worker 的事件循环。读数：每请求 `epoll_modifications` 稳定在 **4.0003–4.0004**、与空链基准**逐数相同**（B-018/B-019）——限流、熔断、Redis 三件都不进 syscall 面。
3. **仪器不进被测方。** 探活连接不计入业务计数、`/metrics` 那条抓取 socket 根本不是 `Connection`、分位由客户端自己算、exporter **刻意不链** `l7gate_core`（这一条在链接期就是事实，不是口号）。

<details>
<summary><b>关于 epoll ET 的实话，以及与 Nginx / Envoy 的功能差异</b>（点开）</summary>

**一句关于性能的实话**：选边缘触发，但 **ET 不比 LT"更先进"，两者性能差别极小**。选它的真实理由是它**逼出正确的 Buffer 设计**——一次必须读到 `EAGAIN`（[ADR-02](docs/adr/ADR-02-epoll-et-io-uring-deferred.md)）。这句"逼出"有实物支撑：`net/Buffer` 的扩容策略改了两版，两版都被内存用例打回来（固定 64 KiB 申请把每连接容量下限抬到 128 KiB；按 hint 预留仍抬到 8 KiB，而 SLO #2 的预算是 20 KB），细节在 ARCHITECTURE 1.3。

而 ET 真正的代价是另一件事：**它的坑不报错，它挂住。** 超容量时第一版写的是 `return`，正确的是关掉这条、继续 `accept` 直到 `EAGAIN`——带着没排空的队列退出，内核不会再投第二次监听事件，表现是"过一会儿不接新连接了"，退出码 0、日志一行没有。同一个形状还有两处（一次事件里读进多条请求必须全走完；`EPOLLOUT` 刷空后必须摘掉，否则对着"随时可写"的 fd 自旋，SLO #2 的空闲 CPU 第一条就断在这儿）。**三处现在各有一条用例盯着**，索引在 [ARCHITECTURE 第 5 节](docs/ARCHITECTURE.md)。

**与 Nginx 的差异：功能维度（不是性能维度）**

- Nginx OSS 只有被动的 `max_fails` / `fail_timeout`；**主动健康检查是 Nginx Plus 才有的功能**；
- Nginx **没有内置熔断器**；Envoy 的 outlier detection 有摘除比例但**没有半开门**，Hystrix 有半开但**没有"半开继续占阀位"**这件事——这台把两者合在一根 CAS 状态位上，`health_ejected_now` 在装配熔断时数的是"不接全量流量的成员数"；
- **配置热重载不换 worker、不换进程**：Nginx 的 `reload` 保留 master 但**让全部 worker 优雅退出、由新 worker 接管**；Envoy 的 **hot restart** 是进程级二进制热升级（传 listening fd + drain + 共享内存继承统计）。这台只换一根 `atomic<shared_ptr>` 指向的不可变策略世代，一条连接都不关、一个 fd 都不迁——**比 Nginx 的 reload 更轻，但可升级的东西也更少（只换配置不换代码）**。这三个词差着层级，**不许混称**（[ADR-10](docs/adr/ADR-10-hot-reload-not-hot-restart.md)）；
- 熔断带**最多摘除比例安全阀**（默认 10%，对齐 Envoy 的 `max_ejection_percent`）：判据失准时最坏只损失 10% 容量，防止熔断本身变成雪崩放大器（[ADR-12](docs/adr/ADR-12-circuit-breaker-ejection-cap.md)）。

这两件事不能混着说：**比 Nginx 多的不是速度，是治理策略**；吞吐基线仍然是 Nginx，目标是它的 60%，不是超过它。

</details>

<details>
<summary><b>可观测面的限定语与细节（Grafana 六格 · WS 调试页 · netem 现场）</b>（点开）</summary>

三个数字要能被**看见**。这一层落了六件：运行期指标面（S5-01）、配置热重载（S5-02）、Prometheus + Grafana 六格（S5-03）、WebSocket 调试页（S5-04）、`docker compose` 一键起全套（S5-05）、`tc netem` 现场六臂（S5-06）；再加上三个数字的终判（S5-07）——判定读的仍是 `scripts/bench/` 的落盘摘要，**不是面板**（ADR-25 第六节：那一格只把已归档的读数搬到眼睛前面）。

**运行期指标面 `--metrics-port PORT`**：拿到的是一段 Prometheus text format 0.0.4 的纯文本，**键名与退出账那套 `[serve] k=v` 逐字相同**（77 个固定键 + 每目标 10 键）。**默认关闭**：不给这个标志就不起线程、不建监听 fd、worker 循环里一个字节都不抄——冒烟拿**修前二进制**跑同一组参数、把两边的键名清单逐行 diff 判死。**抓取一条业务请求都不许多计**：`requests_parsed` 那个唯一自增点对抓取 socket 结构上不可达，用例里带一条阳性对照。三处显式例外写在 [ADR-24 第五节](docs/adr/ADR-24-metrics-exposition.md)。

**Grafana「网关总览」六格**：入库的是 [deploy/grafana/dashboards/gateway-overview.json](deploy/grafana/dashboards/gateway-overview.json)（provisioning 出来的），**面板里那 20 条表达式是唯一真相**——冒烟断言直接从 JSON 里 grep 同一串。面板三句限定语是断言不是记得（冒烟逐条 grep 面板文案）：① **`health_ejected_now=0` 不能读成"没有坏后端"**（只装 `health` 且"探活会答、业务恒 5xx"的成员摘不掉，管这一形状要装 `circuit`）；② **逐目标健康键只覆盖首装那批成员**（重载带进来的新成员要进 `health_t<i>_*` 键得重启）；③ **RSS 那一格写"含热重载的预留槽位容量"**（arm 热重载实测常驻 **+19.1 MiB**，B-020）。**面板是眼睛，不是账本**：三个数字的判定读的仍是 [docs/BENCHMARK.md](docs/BENCHMARK.md) 的落盘读数，Grafana 上的曲线不进入任何达标结论。

手摆起法（网关与 exporter 在 WSL 里跑，Prometheus/Grafana 走 Docker、契约锁的 tag）：

```bash
build/release/l7gate --serve --listen 0.0.0.0:18080 --config deploy/gateway-circuit.json --metrics-port 18522 &
build/release/tools/l7gate_bench_exporter --root build/bench --listen 0.0.0.0:18090 &
docker run -d --name l7gate-prometheus --add-host=host.docker.internal:host-gateway \
  -p 127.0.0.1:19090:9090 -v "$PWD/deploy/prometheus/prometheus.yml:/etc/prometheus/prometheus.yml:ro" \
  prom/prometheus:v2.51.2 --config.file=/etc/prometheus/prometheus.yml --storage.tsdb.path=/prometheus
docker run -d --name l7gate-grafana --add-host=host.docker.internal:host-gateway \
  -p 127.0.0.1:13000:3000 -e GF_SECURITY_ADMIN_PASSWORD=l7gate-demo \
  -v "$PWD/deploy/grafana/provisioning:/etc/grafana/provisioning:ro" \
  -v "$PWD/deploy/grafana/dashboards:/var/lib/grafana/dashboards:ro" grafana/grafana:10.4.2
```

`--add-host=host.docker.internal:host-gateway` **不是可选项**：本发行版下容器不共享 WSL 网络命名空间，只能经**宿主上被发布的端口**出去（实测这条路抓得到 `18522/metrics`，而 `172.17.0.1` 那一路是 `Connection refused`）。

**compose 栈的四件事实**（[ADR-27](docs/adr/ADR-27-compose-one-command-stack.md)）：`--ws-echo` 那台上游不进栈（它是仪器与假上游）；被网关引用的地址全是静态 IPv4（网关不做名字解析，`inet_pton`——S5-09 之后非 IPv4 字面量在装配期就被拒，[ADR-28](docs/adr/ADR-28-ipv4-literal-validation-at-config-parse.md)）；没有 `network_mode: host`、`image` 里没有 `latest`；Redis 按 B-017 只绑 Redis 自己的核。两句明写的"没做"：**MySQL 起而不用**（接 `--nodes-db` 会撞 ADR-10 追加节第七条的定义域收缩）、**这一栈不 arm 文件重载**。判定入口是 `scripts/smoke-s5-05.sh`（十阶段、`build/smoke-s5-05/verdict.txt` 落盘）；清理只按 `l7gate-` 前缀，同机别人的容器一条都不碰。

**WebSocket 调试页**（[demo/ws-echo.html](demo/ws-echo.html)）：地址栏 + 消息流 + 输入框，零依赖——**帧内容原样进出，网关只在连接层面做握手与转发**。三条协议与浏览器的边界（读数在 [ADR-26](docs/adr/ADR-26-ws-echo-upstream-and-headless-dom.md)）：`429` 文案只能来自页面自己发的一条普通 HTTP 请求（浏览器不暴露握手状态码，实测被限流的 7 条只有 `code=1006`）；页面必须经网关同源提供（跨源 fetch 会被 CORS 挡，而网关不做静态文件服务）；回显发生在上游不在网关（客户端帧一定带掩码，网关与 ADR-09 一字未动）。没做的两样照记：「RTT」与「逐条带方向的帧」不当作已实现。

**故障注入的现场**（S5-06）：`tc netem` 在非特权 user+netns 里能用——每一臂都在一个全新的 ns 里跑，注入的作用域是那一个 ns 的 `lo`，宿主回环从头到尾 `noqueue`。六臂对照与那条**被实测改写过的机制句**在 [FAULT-INJECTION §25](docs/FAULT-INJECTION.md)：网络层 delay 把建连也算进等效耗时，"慢与死的分界线"从 `probe_timeout` 移到 **`probe_timeout/4`**；loss 那一臂经 TCP 重传把**探活**推过超时（业务侧几乎全 200）——**探活这条路比业务更怕丢包**。

</details>

## 诚实的边界

做不到的事写在纸上，不靠读者自己踩。

- **热重载的定义域**（[ADR-10 追加节](docs/adr/ADR-10-hot-reload-not-hot-restart.md)）：限流参数改了不生效（拒载并点名，要改就重启）；`routes`/`health`/`rate_limit` 段的出现/消失改了不生效；`source: mysql` 的组与文件重载互斥（启动退出码 2）；生效延迟报的是毫秒（133–205 ms），**没有一处写成"秒级"**。
- **`address` 只收 IPv4 字面量**（S5-09，[ADR-28](docs/adr/ADR-28-ipv4-literal-validation-at-config-parse.md)）：文件里的 `targets[].address` 与 MySQL 节点表的 `address` 列都不是就**整份拒载 / 整批不生效**，话里点到"哪一组哪一行 + 实际读到的值"。这不是新加的限制——消费那个值的一直是 `inet_pton(AF_INET)`，主机名**本来就连不上**。
- **命令行与配置文件各管什么**：监听地址、`--workers`、Redis 参数、`--config-reload-ms` 这些**部署事实**留在命令行、不进配置文件，也不参与重载；路由/后端表/健康与熔断参数才是配置文件的东西。两个来源同时给＝拒绝。
- **限流**：桶键数上界 16384，溢出键共用一只桶并计数；Redis 余额上报有一个结算周期的滞后（B-017 第四节）。
- **WebSocket**：不做消息级限流与消息级熔断，只能连接级（[ADR-09 代价段](docs/adr/ADR-09-websocket-passthrough.md)）。
- **成本读数要带着分辨率读**：`p50/QPS 无可分辨`**不等于"免费"**——B-018 的分辨率是 ≈128 µs/请求；B-020 每请求那一格**读不出方向**，"arm 便宜"这句不说。**两句分开写，不拿后者的"读不出"去洗前者。**
- **"干净"都带反证**：ASan / LSan / UBSan / TSan 全部用故意写坏的样本验过**能红**；阳性验证必须真打红，第一版打不红的逐条弄清为什么（**不改用例凑红**）。每一轮"仪器侧自己错了哪几处"逐条列在 [FAULT-INJECTION](docs/FAULT-INJECTION.md) 各节。

<details>
<summary><b>完整理由与至今未闭合的账</b>（点开）</summary>

热重载定义域的完整理由：

- **限流参数改了不生效**（拒载并点名）。`rate`/`burst` 是 `const` 成员、桶不回收：换新桶表等于排水期两张表各带一份满预算 + 已积累令牌静默清零；改成可变参数等于老键永远用旧参数——那才是字面意义的半套生效。两条都比"不接受这次改动"更糟，所以选窄的那边：**要改限流参数就重启**。
- **段的出现/消失改了不生效**（同样拒载）。`routes`/`health`/`rate_limit` 段在不在决定装不装那一环 filter、建不建那张表——那是链的**形状**，不是链的内容。
- **`source: mysql` 的组与文件重载互斥**（启动就退出码 2）：节点表已经有一条发布路径，再叠一条就是两个节拍往同一个下标空间写两份真相。
- **一条请求的失败记账用的是"记账那一刻"的参数**，不是它当初做路由决定时那一代。替它的代价是每连接 +16 B 并撞 `sizeof(Connection)==528` 那条 `static_assert`——拿每连接内存换一条不破的不变式，**不换**。

**`address` 校验的完整理由**（S5-09）：挡在前面还因为主机名会被原样拼进 `health_t<i>_<addr>_<port>_<metric>`，连字符在 Prometheus 名字法里非法，后果是**整份 `/metrics` 抓失败**（S5-05 的 C1 实测）。`--upstream` 命令行那一支没改语义：它与 `--config` 互斥、也产不出那些键，给主机名仍是运行期 502。

**分辨率的两句分开写**：B-018 那一档的分辨率是一个 HDR 桶 ≈128 µs/请求，比 B-008 那把 8.7 µs 的尺大一个数量级以上，"无可分辨"只排除"掉进 µs 级的悬崖"、**不排除个位数 µs**。B-020 每请求那一格读不出方向（off 臂自己跨轮漂了 24%），而同一轮的常驻那一格 **+19.1 MiB** 无歧义（两臂组内极差 <0.3 MiB）。

**一份不体面的清单**：每一轮"仪器侧我自己错了哪几处"都在 [FAULT-INJECTION](docs/FAULT-INJECTION.md) 的 §20.1 / §21.2 / §22.2 / §23.2 / §24.2 / §25.2 与 [BENCHMARK B-022](docs/BENCHMARK.md) 末逐条列着，其中相当一部分**如果没被抓住，会被读成比事实更强的结论**。还有一次"拆了也不红"的诚实记录：把隧道收尾从 `kFlushThenClose` 换成 `kDiscardOutbound` 全绿，因为 4096 字节的回显早被内核套接字缓冲吸收了——那条判据因此归 S1-04 的既有用例，不归这条。

**一条至今没能归因的事实**：真脚本在全跑形状（真容器 / 真子进程 + 真浏览器）下会打 FAIL 而 `echo $?` 读成 0，而 11 组最小复现与 4 个截断版全是 rc=1——没找到具体那一句。处置不是解释而是绕开：**判定写进 `verdict.txt`，门禁读盘**。

**S5-08 关门时这几笔没闭合的账也照记**：① 对称容器档的 `sym3-w16-r2` 等三发报过 wrk `Socket errors: timeout`，条数与非 2xx 逐数相等——**重跑两发没复现**（0 条 timeout、0 条非 2xx，退出账三格超时与自产 5xx 全 0），而历史三发的退出账已被一次硬杀永久带走，所以这一格是**未证**，不是已修；更糟的是它连条数都被记错过一次（"16/48"其实是两发不三发，nginx 臂也报过）——见 [B-022 追记](docs/BENCHMARK.md)。② 干净发里网关有 `requests_parsed − responses_completed = 647 / 456`，而**上游夹具在同一批里留下同量级的 `requests − responses_written`（653 / 486）**：这只排除"网关单边丢账"一种读法，收尾瞬间两侧计数点各停在什么位置没取同轴的账，**不猜机制**。③ 顺带一条关于工具的：`wrk 4.1.0` 在"accept 后永不回字节"的假对端上**根本不产 `Socket errors: timeout` 计数**（四条档全 `0 requests`），所以那一格的触发条件用手边手段测不到（[ENVIRONMENT 2.3](docs/ENVIRONMENT.md)）。

</details>

## 明确不做的东西

每条都有 ADR 写明理由，不装作它们不存在：

> HTTP/2 / gRPC / QUIC · TLS 握手 · 第三方网络库（asio / muduo / brpc） · etcd / xDS / K8s / Service Mesh · 动态服务发现 · **响应缓存**（缓存是**另一种网关形态**——Apache Traffic Server 的全部价值就在缓存，做了它 p99 就不再度量流量治理） · WebSocket 帧解析（只做 Upgrade 后透传） · 多进程 `SO_REUSEPORT` · Web 管理后台 / 登录 / 配置可视化编辑器（那些是 CRUD，且会把评审注意力从 p99 拉到 UI） · OpenTelemetry 与分布式追踪 · 多副本下的 Redis 计数一致性实测 · 加权负载均衡（要做先进 ADR-19 并连带写清它与健康检查摘除安全阀分母的相互作用）

还有一个**反向承诺**：**io_uring 对比分支、连接对象池 / 自定义分配器、日志异步化全部挂起**。只有当压测 + perf 火焰图拿出证据证明那是真实瓶颈时才立项，且先写 ADR。**在没有证据的情况下优化，是空壳项目的典型形态。**

## 复现与验证

| preset | 编译器 | 用途 |
|---|---|---|
| `debug` | GCC 15.2 | 日常开发 |
| `release` | GCC | **压测唯一允许**——sanitizer 构建测出来的是检查开销的延迟 |
| `asan-ubsan` | Clang 21 | S1 门禁（强制 `-O0`：`-O1` 起会把"可被证明无副作用的坏写入"优化掉，那种"干净"是假的） |
| `tsan` | Clang 21 | S2 门禁：跨线程传递与异步销毁的**唯一**自动化验证手段 |

```bash
bash scripts/verify-all-presets.sh        # 三 preset 构建 + ctest + 我方警告计数，一次拿全
for s in smoke-s1-04 smoke-s1-06 smoke-s1-07 smoke-s2-05 smoke-s3-02 smoke-s3-04 \
         smoke-s4-04 smoke-s4-05 smoke-s4-06 smoke-s5-01 smoke-s5-02 smoke-s5-03 smoke-s5-04 smoke-s5-05; do
  PRESET=debug bash scripts/$s.sh         # 十四条冒烟。需要 Docker 的三条：s3-04（MySQL 8.0.36）、
                                          #   s4-04（redis:7.2.5）与 s5-05（整栈 compose）：容器不在位时
                                          #   脚本在阶段 0 自己拒跑，不换一个顶上去
                                          # s5-03 / s5-04 / s5-05 的判定读 build/smoke-*/verdict.txt，不信退出码
                                          # s5-04 还要 Windows 侧 chrome.exe 无头取 DOM，不在位同样拒跑
                                          # 跑冒烟之前必须先 `cmake --build build/debug -j 16`（**全目标**）
done

scripts/bench/s1-p99.sh                   # SLO #1 标准口径（canonic 由参数自己判，非标那轮不进文档）
scripts/bench/s1-p999.sh                  # SLO #1 的 p999 半边：每臂自报观察者 CPU；1000 并发那档目前判"取不到"
scripts/bench/c100k.sh                    # SLO #2：多目标地址长连接稳态 + 每连接对内存 + HDR 分位
scripts/bench/s5-07-qps-symmetric.sh      # **SLO #3 的终判档**：两侧都进容器、同一种 -p 发布、打同一个上游
scripts/bench/s2-qps-vs-nginx.sh          # 旧形状（裸进程网关 vs 容器 nginx）——自 S5-07 起**只作对照**，无宣判资格
scripts/bench/s5-07-flamegraph.sh         # 火焰图，按 ENVIRONMENT 4.1 那一种已验证形态取（perf 直接拉起被测进程）
scripts/fault/s3-05-matrix.sh             # 故障注入第一轮五类；第二轮叠加五类见 s4-07-matrix.sh
scripts/fault/s5-06-netem.sh              # 网络层六臂（非特权 user+netns 里的回环），与应用层读数对照并归因
```

非标准口径与排水档这类"怎么把旋钮拧对"的写法，写在上面每个脚本自己的 `--help` 与 [docs/SLO.md](docs/SLO.md) 的脚本索引里，README 不抄第二份。「ASan 干净」这句话不该靠手敲，所以有一条命令能复现它——**同理每个数字都能一键重跑**，这是契约第 7 节对这几份文档的完成判据；**长跑判定只认落盘输出**。

## 阶段门禁与文档地图

| 阶段 | 内容 | 状态 | 门禁读数 |
|---|---|---|---|
| **S1** | 单连接转发 + 第一份 p99 | ✅ 八条全 DONE，已 push | ASan/UBSan 干净（带反证）、真 curl 10000 请求全数对上、`fd` 与存活连接归零、第一份 p99 |
| **S2** | 多 Reactor + 连接池 + 跨线程销毁 | ✅ 八条全 DONE，已 push `47bca67..dcccca5` | **TSan 零 data race**；#2 达标、#1 未达标但第一次测到多 Reactor 有效（33 ms → 4–6 ms 带）、100k 负载下排水 **0.947 s** |
| **S3** | 路由 + 负载均衡 + 健康检查 + 安全阀 | ✅ 六条全 DONE，已 push `e54ae61..17b9234` | 摘除 **202 ms**、误摘 **0/3480**、安全阀 10 台全死停在 **1/10**、五类故障矩阵全过 |
| **S4** | 限流 + 熔断 + 超时与重试防放大 | ✅ 九条全 DONE，已 push `17b9234..0f54343` | 429 生效、熔断恢复时间线、Redis 三段降级、重试放大 **1.50 / 2.00** 实测、配对账两处退化 |
| **S5** | 热重载 + 指标 + 面板六格 + 调试页 + `docker compose` 一键栈 + `tc netem` 现场 + 三个数字终判 | ✅ **八条全 DONE（S5-01…S5-08，2026-11-04 关门，push 已执行 `0f54343..c324a9c`）；关门后追加的 S5-09 也已 DONE**（`address` 只收 IPv4 字面量，[ADR-28](docs/adr/ADR-28-ipv4-literal-validation-at-config-parse.md)） | 三 preset 各 **383/383**（S5-09 之前 376/376）、非 gtest 警告各 **0 条**、`tsan` 直跑日志 `ThreadSanitizer` **0 次**（381 PASSED + 2 SKIPPED）、**十四条冒烟**逐条 rc=0 且 ✗ 计数 **0**（第 12/13/14 条读 `verdict.txt`：40 / 29 / 69 项 `fails=0`）；三个数字 **#1 未达标照记 / #1 的 p999 取不到 / #2 达标 / #3 达标** |

**逐条验收的读数不在这里。** 每条任务的可断言验收句与实测数字在 [TASKS.md](TASKS.md) 表格末列；压测与成本对照在 [docs/BENCHMARK.md](docs/BENCHMARK.md)（B-001…B-022，含未达标记录与**口径变更节**）；故障注入的预期 vs 实测在 [docs/FAULT-INJECTION.md](docs/FAULT-INJECTION.md)（§1–§25，S5 的矩阵收口索引在 [docs/ROADMAP.md](docs/ROADMAP.md)「## S5」节）；阶段门禁对账在 [docs/ROADMAP.md](docs/ROADMAP.md)。**同一份证据抄第二份，迟早会变成两份互相分叉的**——所以这一节只留结论与指针。

<details>
<summary><b>文档地图与 28 条 ADR</b>（点开）</summary>

| 文档 | 内容 |
|---|---|
| [CLAUDE.md](CLAUDE.md) | **唯一契约**：身份约定、三个数字、三条禁令、门禁四件套、版本锁定表 |
| [TASKS.md](TASKS.md) | 唯一真相源：任务 ID / 可断言验收标准 / 状态 / 实测数字 / 已裁清单 |
| [docs/SLO.md](docs/SLO.md) | 三个数字的目标与**写死的**测量条件、脚本索引 |
| [docs/ENVIRONMENT.md](docs/ENVIRONMENT.md) | 环境自检实测版本、唯一实质偏离（libmysqlclient）、sanitizer 阳性验证、perf 能否采样、Docker 与抓取通路、无头浏览器仪器 |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | **已实现的现状**（严格区分"已实现"与"已定但未实现"）：模块 1.1…1.39 + 第 5 节踩坑索引（格式固定「现象 / 根因 / 防复发的断言」，写不出第三条的明写"缺"） |
| [docs/BENCHMARK.md](docs/BENCHMARK.md) | 压测方法 + vs Nginx 流程 + 实测（含未达标照记与口径变更节） |
| [docs/FAULT-INJECTION.md](docs/FAULT-INJECTION.md) | 故障矩阵预期 vs 实测 + 每轮阳性验证与仪器自错照记 |
| [docs/INTERVIEW.md](docs/INTERVIEW.md) | 60 秒故事，以及"没有它会怎样"的实测版 |
| [docs/ROADMAP.md](docs/ROADMAP.md) | 五阶段与每阶段**门禁结果**、一键复现、遗留账目 |

**每条 ADR 都写清"我放弃什么"**：01 HTTP/1.1 · 02 epoll ET + io_uring 挂起 · 03 多线程 Reactor · 04 网络层自研 · 05 Redis 只做分布式计数与降级 · 06 MySQL 配置与审计 · 07 不引 etcd/xDS/K8s · 08 不做响应缓存 · 09 WebSocket 只透传 · 10 热重载不是热重启 · 11 filter 链 · 12 摘除比例安全阀 · 13 应用层故障注入为主 · 14 连接所有权 · 15 per-worker 池 · 16 请求-响应配对 · 17 C100K 与 HDR 测量手段 · 18 路由 filter · 19 负载均衡与配置文件 · 20 MySQL 节点表 · 21 超时与限流 · 22 自产错误响应 · 23 connect 失败重试 · 24 指标暴露面 · 25 旁路 percentile exporter · 26 演示页的握手对端与取数仪器 · 27 compose 编排与运行时壳镜像 · 28 地址校验前移到配置层

</details>

## 许可

MIT — 见 [LICENSE](LICENSE)。
