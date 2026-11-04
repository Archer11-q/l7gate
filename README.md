# l7gate

**一个单机部署的 C++20 L7 流量治理网关**：自己写 epoll ET + 多线程 Reactor，解析 HTTP/1.1，按配置好的 filter 链做路由、负载均衡、健康检查与摘除、限流、熔断、超时与重试，然后把整个过程的性能与故障边界**用可复跑的脚本量出来——包括没达标的部分**。

它不是"又一个高性能 Web 服务器"，不是框架，也不是功能清单。对外形态是一个能顶替 Nginx 位置的网关。

[![C++20 · 无第三方网络库](https://img.shields.io/badge/C%2B%2B20-no%20third--party%20net%20lib-blue)](docs/adr/ADR-04-no-third-party-network-lib.md)
[![单测 376/376 · 三 preset](https://img.shields.io/badge/unit%20tests-376%2F376%20%C3%97%203%20presets-brightgreen)](docs/ROADMAP.md)
[![TSan 0 data race](https://img.shields.io/badge/ThreadSanitizer-0%20races-green)](docs/FAULT-INJECTION.md)
[![SLO #2 达标](https://img.shields.io/badge/SLO%20%232%20100k%20%E5%86%85%E5%AD%98-17.47%20KiB%2F%E5%AF%B9%20%E2%9C%85-green)](docs/BENCHMARK.md)
[![SLO #1 未达标，照记](https://img.shields.io/badge/SLO%20%231%20p99-4.2%E2%80%936.3%20ms%20%E2%9D%8C-red)](docs/SLO.md)
[![MIT](https://img.shields.io/badge/license-MIT-lightgrey)](LICENSE)

> **为什么值得往下看**，三件事：
> ① 网络层、HTTP 解析、JSON 子集解析、令牌桶、一致性哈希、SHA-1/base64 全部本仓库自研，**一个第三方库都没引**；
> ② 治理功能在"流量治理"这个维度不比 Nginx 开源版少——Nginx OSS 没有主动健康检查（那是 Plus 的功能），也**没有内置熔断器**；
> ③ **每个数字都写死测量条件、都能一键复跑，未达标就标红留在首屏**：`#1` 到现在仍未达标，`#1b` 那一档我明确记"取不到"。

---

## 目录

- [三个数字](#三个数字) · [特性一览](#特性一览) · [快速开始](#快速开始) · [架构与数据流](#架构与数据流)
- [可观测面与调试界面](#可观测面与调试界面) · [诚实的边界](#诚实的边界) · [明确不做的东西](#明确不做的东西)
- [复现与验证](#复现与验证) · [阶段门禁与文档地图](#阶段门禁与文档地图)

---

## 三个数字

项目的全部价值锚点。目标值写死在 [docs/SLO.md](docs/SLO.md)，**任何情况下不许为了迁就实测而改目标**；未达标照记并归因。

| # | 数字（测量条件写死） | 目标 | 当前读数 | 判定 |
|---|---|---|---|---|
| **1** | 转发增量 p99 · 1 KiB 体 / 1000 并发 / 60 s 稳态 / 同机回环 | ≤ 1.0 ms | workers=16 → **4.160 / 4.330 / 6.290 ms**；workers=1 → 32.990–35.210 ms。两条系列不重叠（间隔 26.700 ms）：多 Reactor 确实把增量从 30–38 ms 带打到 4–6 ms 带。此前八份 canonic 档全跑在单 Reactor 缺省路径上（脚本从没传过 `--workers`），所以这一格到 S2-07 才被测过 | ❌ **未达标**（超 4.2–6.3 倍；剩下的是每请求 ≈8.7 µs 的净加工成本 × 1000 并发）<br>[B-008](docs/BENCHMARK.md) |
| **1b** | 转发增量 p999 · 同一档 | ≤ 5.0 ms | 手段已到位（自研 C100K 客户端 + HDR 分位）并第一次算出增量（32.192 ms），但**两臂观察者都贴着单核上限**（客户端自报 0.99 / 1.00 核），而同一份分布的 p99 与 wrk 那条线差 **6 倍** → 仪器项主导。非标准的 100 并发归因实验出数：5.420 ms（w1）/ 3.764 ms（w16），**只作方向** | ⚪ **明确记"取不到"**——既不判达标也不判未达标（解锁需多线程观察者 / 绑核方案 / perf 三选一，已记债）<br>[B-009](docs/BENCHMARK.md) |
| **2** | 100k **连接对**稳态内存 + 空闲 CPU | ≤ 20 KB / < 5% | **17.47 KiB/对**；空闲 CPU 占单核 **0.22%**（占整机 16 核 0.01%）；稳态窗口散布 0 KiB；`established=100000/100000`、`upstream_connect_failed=0`、每端点实测 25,000 条、`constructed==destructed==200000`、`fd 40→40`。构成表逐项实测：可点名 17,616 B/对 + 控制块/节点/取整 245 B，进程内堆 **17,861 B/对** 与生产 RSS 商 **17,889 B/对 差 0.16%** | ✅ **达标**<br>[B-007 + B-011](docs/BENCHMARK.md) |
| **3** | QPS ≥ Nginx **同环境实测值**的 60% | ≥ 60% | **206.11% / 203.74%**（两轮复现，差 1.2%；第二轮 nginx 原始态 9,184.79 / l7gate 18,713.55）——但 nginx 在容器里只能走端口发布，分母被压到同一上游天花板（56,332.52 QPS）的 **16%**，且 S1 的单 Reactor 对的是 16 worker 的 nginx | ⏳ 有读数，**终判推到 S5**（已裁）<br>[B-005 附](docs/BENCHMARK.md) |

**三个非绿图标含义不同，我不想被读成同一个意思**：`#1b` 是**工具取不到那个分位**（已裁决在自研客户端里做 HDR，不引第三方工具）；`#3` 是**数拿到了，但按裁决把终判推到 S5**。而 `#2` 那一格曾经写着"`ulimit -n`=10240 是硬前置"——2026-10-27 实测推翻（soft 可自抬到 hard=1048576），真墙是回源那一跳的单元组容量只有 **28,232**，解开它的是 S2-06a（端点列表 + per-worker 静态映射，容量 = `min(N,E) × 28,232`）。

**首屏那枚红徽章是故意的。** `#3` 那两轮比值很好看，但当年对它的那句批评我原样留着：**赢的是对照组，不是实现**——拿一个被削弱的分母给自己发达标证明，正是 SLO 里那句"最容易造假的点在于和谁比"。

第三个数还有附加产出：与 Nginx 的差值必须定位到 **HTTP 解析 / 内存分配 / 连接管理 / 日志写盘 / 系统调用次数**中的哪几项。**"差多少"谁都能测，"差在哪"才是这个项目**（终判在 S5-07；火焰图只能按 [ENVIRONMENT 4.1](docs/ENVIRONMENT.md) 那一种已验证形态取——perf 直接拉起被测进程、只有用户态栈、nginx 对照臂同法，`perf_event_paranoid` 保持 2 不抬）。

---

## 特性一览

全部已落地，每条都有可断言的验收句与落盘证据。**这张表只给结论与指针，不复制证据**——逐条读数在 [TASKS.md](TASKS.md) 各行末列，故障实测在 [docs/FAULT-INJECTION.md](docs/FAULT-INJECTION.md)，成本对照在 [docs/BENCHMARK.md](docs/BENCHMARK.md)。

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

**还没落地的**：三个数字终稿与 vs Nginx 归因报告（S5-07）、S5 门禁收尾（S5-08）。`docker compose` 一键起全套（S5-05）**与** `tc netem` 现场验证（S5-06）都已 DONE——前者的形状与读数见上面「可观测面与调试界面」里 S5-05 那一小节与 [ADR-27](docs/adr/ADR-27-compose-one-command-stack.md)，后者的六臂对照与被实测改写的那条机制句见 [docs/FAULT-INJECTION.md](docs/FAULT-INJECTION.md) §25 与 [docs/ENVIRONMENT.md](docs/ENVIRONMENT.md) 5.4。

---

## 快速开始

构建与压测**必须在 Linux/WSL2 原生 ext4 内**。放在 `/mnt/d` 这类挂载盘上，9P 的 IO 抖动会直接进 p99——测出来的数既不属于网关也不属于 Nginx（契约 9.4）。环境要求与实测版本见 [docs/ENVIRONMENT.md](docs/ENVIRONMENT.md)。

```bash
cmake --preset debug && cmake --build --preset debug
ctest --preset debug              # 376/376（asan-ubsan / tsan 里各有 2 条 Skipped：
                                  #   sanitizer 换分配器后 mallinfo2() 一律报 0 → 跳过＝不判、不是通过）
./build/debug/l7gate --self-check # version / compiler / gtest / sanitizers / build_type / malloc
```

想一把把演示面起起来（网关 + 两台上游 + 旁路 exporter + Redis + MySQL + Prometheus + Grafana，八件）：
`docker compose -f deploy/compose/docker-compose.yml up -d --build`——**先构建再上栈**，
而且**这一栈只服务演示面**：三个数字的判定仍走上面的裸进程与 `scripts/bench/` 的落盘摘要。
形状与它的四条实测通路写在[可观测面与调试界面](#可观测面与调试界面)那一节与 [ADR-27](docs/adr/ADR-27-compose-one-command-stack.md)。

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

划分只有一条：**部署事实留在命令行，流量策略留在 `--config`**。所以监听地址、`--workers`、Redis 与重载节拍这些都不进配置文件——配置文件里出现 `listen`/`workers` 会被当成"本配置不认识的字段"当场拒载并点名。`--metrics-port 0` 是"内核给端口"（测试靠它），**关闭的形态是这个标志缺席**。完整清单以 `l7gate` 不带参数打出的 usage 为准，我这里抄一份是为了少一次来回。

**① 最简转发**——上游用仓库自带的测试夹具，端口给 `0` 让内核选，启动行会打印真实端口：

```bash
./build/debug/tests/l7gate_test_upstream --listen 127.0.0.1:0 --body-bytes 64 &
./build/debug/l7gate --serve --listen 127.0.0.1:18080 --upstream 127.0.0.1:<上面打印的端口>
curl -sS -i http://127.0.0.1:18080/hello
kill -TERM <网关 pid>             # 停止时才打 [serve] key=value 退出账（含 fd 与 RSS 首末值）
```

**② 策略配置**——`--upstream` 与 `--config` **二选一**，两个都给或都不给都是退出码 2（后端表有两个来源时，"哪一份是真相"本身就是一份会漂移的配置）：

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

<details>
<summary><b>③ 另外四个配方：热重载 · 熔断 · Redis 降级 · 观测面与调试页</b>（点开）</summary>

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
# 浏览器打开 http://127.0.0.1:18940/demo/ws-echo.html —— 页面必须经网关同源提供，理由见可观测面那一节
```

</details>

`--serve` 打的每行统计是给脚本读的，不是给人的漂亮表格；**读不到的一律是 `-1` 而不是 `0`——"没测到"和"没有泄漏"是两件事。**

---

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

**一句关于性能的实话**：我选边缘触发，但 **ET 不比 LT"更先进"，两者性能差别极小**。选它的真实理由是它**逼出正确的 Buffer 设计**——一次必须读到 `EAGAIN`（[ADR-02](docs/adr/ADR-02-epoll-et-io-uring-deferred.md)）。这句"逼出"有实物支撑：`net/Buffer` 的扩容策略我改了两版，两版都被内存用例打回来（固定 64 KiB 申请把每连接容量下限抬到 128 KiB；按 hint 预留仍抬到 8 KiB，而 SLO #2 的预算是 20 KB），细节在 ARCHITECTURE 1.3。

而 ET 真正的代价是另一件事：**它的坑不报错，它挂住。** 超容量时我第一版写的是 `return`，正确的是关掉这条、继续 `accept` 直到 `EAGAIN`——带着没排空的队列退出，内核不会再投第二次监听事件，表现是"过一会儿不接新连接了"，退出码 0、日志一行没有。同一个形状还有两处（一次事件里读进多条请求必须全走完；`EPOLLOUT` 刷空后必须摘掉，否则对着"随时可写"的 fd 自旋，SLO #2 的空闲 CPU 第一条就断在这儿）。**三处现在各有一条用例盯着**，索引在 [ARCHITECTURE 第 5 节](docs/ARCHITECTURE.md)。

### 与 Nginx 的差异：功能维度（不是性能维度）

- Nginx OSS 只有被动的 `max_fails` / `fail_timeout`；**主动健康检查是 Nginx Plus 才有的功能**；
- Nginx **没有内置熔断器**；Envoy 的 outlier detection 有摘除比例但**没有半开门**，Hystrix 有半开但**没有"半开继续占阀位"**这件事——我这台把两者合在一根 CAS 状态位上，`health_ejected_now` 在装配熔断时数的是"不接全量流量的成员数"；
- **配置热重载不换 worker、不换进程**：Nginx 的 `reload` 保留 master 但**让全部 worker 优雅退出、由新 worker 接管**；Envoy 的 **hot restart** 是进程级二进制热升级（传 listening fd + drain + 共享内存继承统计）。我这台只换一根 `atomic<shared_ptr>` 指向的不可变策略世代，一条连接都不关、一个 fd 都不迁——**比 Nginx 的 reload 更轻，但可升级的东西也更少（只换配置不换代码）**。这三个词差着层级，**不许混称**（[ADR-10](docs/adr/ADR-10-hot-reload-not-hot-restart.md)）；
- 熔断带**最多摘除比例安全阀**（默认 10%，对齐 Envoy 的 `max_ejection_percent`）：判据失准时最坏只损失 10% 容量，防止熔断本身变成雪崩放大器（[ADR-12](docs/adr/ADR-12-circuit-breaker-ejection-cap.md)）。

这两件事不能混着说：**我比 Nginx 多的不是速度，是治理策略**；吞吐基线仍然是 Nginx，目标是它的 60%，不是超过它。

---

## 可观测面与调试界面

三个数字要能被**看见**。四件已落地：运行期指标面（S5-01）、配置热重载（S5-02）、Prometheus + Grafana（S5-03）、WebSocket 调试页（S5-04）。

### 运行期指标面 `--metrics-port PORT`

拿到的是一段 Prometheus text format 0.0.4 的纯文本，**键名与退出账那套 `[serve] k=v` 逐字相同**（77 个固定键 + 每目标 10 键；`# TYPE` 全表按量的性质分——`trips_rate` 名字带 rate 但它是**计数**，TYPE 诚实记 `counter` 并带一条 `# HELP` 说清）。**默认关闭**：不给这个标志就不起线程、不建监听 fd、worker 循环里一个字节都不抄——这句不是自述，冒烟拿**修前二进制**跑同一组参数、把两边的键名清单逐行 diff 判死。三处显式例外写在 [ADR-24 第五节](docs/adr/ADR-24-metrics-exposition.md)。**抓取一条业务请求都不许多计**：`requests_parsed` 那个唯一自增点对抓取 socket 结构上不可达，用例里带一条阳性对照——同一条字节发给业务端口时计数确实 +1，否则"一动不动"可能只是因为服务根本没起来。

### Grafana「网关总览」六格

入库的是 [deploy/grafana/dashboards/gateway-overview.json](deploy/grafana/dashboards/gateway-overview.json)（provisioning 出来的，不是我手工点的），**面板里那 20 条表达式是唯一真相**——冒烟断言直接从 JSON 里 grep 同一串，不另写一份查询（另写一份＝面板与断言分家）。

| 格 | 键名 | 一句话 |
|---|---|---|
| 1 吞吐 | `sum(rate(requests_parsed[1m]))` | `1m` 只是展示窗口，**SLO 判定读的仍是落盘摘要** |
| 2 p99 / p999 | `l7gate_bench_increment_microseconds{quantile=…}` + `l7gate_bench_observer_cores` | **这一格不是网关自报**：一台 armed 网关的 `/metrics` 实测 **101 个 `# TYPE` 全是整数计数、没有一格是时延**（ADR-24 裁"分位由客户端自己算"的必然后果）。数由旁路 exporter 从**已落盘压测摘要**搬来，是**批次级**读数；每条 series 自带 `canonic`/`conns`/`batch`，`canonic=0` 的批次不得当 SLO 判定读；观察者占单核那一列与分位同格发 |
| 3 活跃连接 | `live_connections`、`sum(increase(downstream_accepted[1m]))` | `live_connections` 数的是 `Connection` 对象；SLO #2 的分母已裁成「连接对」，读成连接对要**除以 2** |
| 4 熔断状态 | `health_ejected_now`、`circuit_half_open_now`、`health_ejections_blocked_by_valve`、`health_all_denied_fallback` | 装配熔断时 `health_ejected_now` = 摘除 **+ 半开**；`blocked_by_valve` 非零就是安全阀在挡摘除 |
| 5 上游健康度 | `{__name__=~"health_t.*_ejections"}` / `_probe_fail` / `_recoveries` / `_trips_rate` | 正则匹配的那一族键名逐个存在（曾经有一个我想象出来的键名被写进面板，那一格会永远空） |
| 6 状态码分布 | `responses_completed`、`rate_limit_rejected`、`gateway_replies_{connect_failed,all_ejected,over_capacity}` | **429 那一格画的就是 `rate_limit_rejected` 本身**，面板不另算一遍；互背书实测静置臂 **34==34**、扰动臂 **49==49**，两边取数通道不同（一边直抓 `:18522/metrics`，一边问 Prometheus 的 `/api/v1/query`） |

另有一格**附注**（不是第七格）：`rss_kib_now`。

**三句限定语是断言，不是我记得**（冒烟逐条 grep 面板文案，少一句红一条）：

1. **`health_ejected_now=0` 不能读成"没有坏后端"**——只装 `health` 且"探活会答、业务恒 5xx"的成员，在业务失败比探活稀疏的流量下**摘不掉**（实测 `ejections=0` 而那台自报接了 20/40 条 503 且永不停；ARCHITECTURE 1.31 边界⑥）。管这一形状要装 `circuit`。
2. **逐目标健康键只覆盖首装那批成员**——配置重载带进来的新成员会正常被路由/摘除/恢复，但要进 `health_t<i>_*` 那些键得重启。
3. **RSS 那一格写"含热重载的预留槽位容量"**——arm 了热重载就把健康表按 `kMaxUpstreamTarget + 1` = 65,535 槽开容量，实测常驻 **+19.1 MiB**（B-020），不许画成"网关自己吃内存"。**分位那一格固定带"本档 ≈128 µs/请求"的分辨率限定**，以及 B-009 那句"1000 并发那档 p999 判取不到、卡点是观察者单核饱和"。

起法（网关与 exporter 在 WSL 里跑，Prometheus/Grafana 走 Docker、契约锁的 tag）：

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

`--add-host=host.docker.internal:host-gateway` **不是可选项**：本发行版下容器不共享 WSL 网络命名空间，只能经**宿主上被发布的端口**出去（实测这条路抓得到 `18522/metrics`，而 `172.17.0.1` 那一路是 `Connection refused`）。所以 `--listen` 要给 `0.0.0.0`，观测面跟着 `--listen` 走、不另开旋钮。

### 一键起全套（`docker compose`，S5-05）

上面那一串是**手摆**的起法，它同时也是三个数字的判定通路（裸进程 + 落盘摘要）。想一把起全套就用编排那份：[deploy/compose/docker-compose.yml](deploy/compose/docker-compose.yml)——八件一起：网关 + 两台上游 + 旁路 exporter + Redis + MySQL + Prometheus + Grafana。

```bash
cmake --build --preset release -j 16        # **先构建**：产物是挂进容器的，不是打包进镜像的
docker compose -f deploy/compose/docker-compose.yml up -d --build
curl -s http://127.0.0.1:18080/hello        # 业务面
# Grafana http://127.0.0.1:13000（admin / l7gate-demo）、Prometheus http://127.0.0.1:19090
docker compose -f deploy/compose/docker-compose.yml down --remove-orphans
docker ps -a --filter name=l7gate-          # 停栈后这一条必须是空的
```

四件写在文件里的事实，不是假设（[ADR-27](docs/adr/ADR-27-compose-one-command-stack.md)）：

- **`--ws-echo` 那台上游不进这个栈**。S5-04 已经写明它是**仪器与假上游**，别把演示页的握手对端当生产后端；这一栈里的两台上游走默认档。
- **被网关引用的地址全是静态 IPv4**（`172.31.250.0/24` 由编排自己声明）。容器之间按服务名解析这一格本轮实测**通**（第一次尝试就 200），**但网关不做名字解析**——它的地址走 `inet_pton(AF_INET)`，实测把配置里的 `address` 写成 `localhost` 就是 `502` + `upstream_connect_failed`。冒烟把"服务名那一版"真跑了一次，它必须红（阶段 4）。反过来 Prometheus 与 Grafana 自己做名字解析，所以**入库那两份抓取配置的 target 值（`host.docker.internal:18522/18090/19090`）一个字母没动**——实测容器经 `host.docker.internal` 打得通别的容器发布出来的端口，因此不必改；那两份**顶上关于 `extra_hosts` 的说法按本轮实测更正过**（"不带上就变假目标"这句说重了，见 [ADR-27 第三节](docs/adr/ADR-27-compose-one-command-stack.md)），改的是注释、不是值。
- **没有 `network_mode: host`，`image` 里没有 `latest`**（前者在这台机器上不成立，后者是契约 9.1）。四个基础设施镜像走锁定 tag；我们自己产物那台是 `l7gate/runtime:s5-05`——一个**壳镜像**（`ubuntu:26.04` + 契约已经点名的 `libmysqlclient24` / `libhiredis1.1.0`），里面没有任何本项目产物。
- **Redis 按 B-017 只绑 Redis**（`cpuset: "15"` 只出现在 redis 那一段，网关不绑）。契约要的是"Redis 不许抢走网关正在用的核"，不是"各占一半机器"；网关仍可被调度到 15 号核，那条竞争没被消除，只是不再扩散。前提 `docker info` 的 `NCPU=16` 由冒烟阶段 0 先断住。

**这一栈只服务演示面**：网关进了容器，数据路径就多一次 DNAT/userland proxy——这正是 B-005 那句"赢的是对照组不是实现"的根据本身。所以三个数字的判定仍走上面那条裸进程通路 + `scripts/bench/` 的落盘摘要；要把网关与 nginx 放在同一拓扑里比（已裁第 ⑨ 条第 4 项那一臂）归 S5-07，且**两侧都进容器**。

两句明写的"没做"：**MySQL 起而不用**（它是契约锁的服务、健康且可达，但没接成路由来源——接 `--nodes-db` 会撞 ADR-10 追加节第七条"首装含 mysql 组的网关不许 arm 文件重载"，那是定义域的收缩，不归编排决定）；**这一栈不 arm 文件重载**，所以"限流参数改了不生效""逐目标健康键只覆盖首装那批成员"那两句照旧有效（面板 RSS 那一格仍然写"含热重载的预留槽位容量"，那是 B-020 的口径，不是这一栈 arm 了）。

判定入口是 `scripts/smoke-s5-05.sh`（十阶段、`build/smoke-s5-05/verdict.txt` 落盘），不是"我起起来了"。清理只按 `l7gate-` 前缀，脚本里没有任何全量 `rm`/`kill`，同机别人的容器一条都不碰（开工前后各数一次，不等就红）。

**面板是眼睛，不是账本**：三个数字的判定读的仍是 [docs/BENCHMARK.md](docs/BENCHMARK.md) 的落盘读数与 `scripts/bench/` 的原始档；Grafana 上的曲线不进入任何达标结论。

### WebSocket 调试页

[demo/ws-echo.html](demo/ws-echo.html)：地址栏 + 消息流 + 输入框，零依赖。它把 ADR-09 那件抽象的事变成可验的——**帧内容原样进出，网关只在连接层面做握手与转发**。手工模式连着不断开；带 `?auto=1` 时页面自己跑完那一串并写一个完成哨兵，**冒烟读的就是那个哨兵**。

三条**协议与浏览器的边界**，不是缺陷也不是偷懒（读数在 [ADR-26](docs/adr/ADR-26-ws-echo-upstream-and-headless-dom.md)）：

- **`429` 那句文案只能来自页面自己发的一条普通 HTTP 请求**。浏览器不暴露握手的 HTTP 状态码（实测：被限流的那 7 条只有 `code=1006`，没有任何码）。这条探针请求是真业务请求，网关那侧同样计入 `rate_limit_rejected`——冒烟断的是**页面数到的条数 == 网关自报的增量**（`文案 7 == 哨兵 got429 7 == Δrate_limit_rejected 7`，两边取数通道各留指纹文件）。
- **页面必须经网关同源提供**。跨源 `fetch` 会被 CORS 挡（实测控制台原文 `No 'Access-Control-Allow-Origin' header is present`），而网关不做静态文件服务（那是动 `src/`）。所以页面由上游 `--html` 发出去、浏览器一律从网关端口进。直接双击打开本地文件那一版只能看连接事件、读不到状态码——**这是同源规则，不是我少写了**。
- **回显发生在上游，不在网关**。浏览器要求服务端回帧未掩码，而客户端帧一定带掩码（实测把原样字节吐回去时 `onmessage` **一次都不触发**，直接断连）。所以"会解帧的对端"是演示夹具（`--ws-echo`，SHA-1/base64/掩码全自带），**网关与 ADR-09 一字未动**：它仍只做 101 relay + 之后不透明双向透传——真实拓扑本来就是这样，WebSocket 的端点是应用而不是代理。

**没做的两样照记**：这一节原先的规划文案里有「RTT」与「逐条带方向的帧」。RTT 我没做——它是页面自己掐表的一个数，不进任何判定，而契约 4.2 要的是"限流与摘除看得见"；「逐条带方向的帧」按 ADR-09 就**不该做**（页面一旦解释帧，就把"网关不解析"这件事搞混了）。不当作已经实现。

**故障注入的现场**（S5-06，2026-11-04 量完）：`tc netem` 在本机**能用**——但不是在宿主回环上用。
`uid=1000` 加 qdisc 要 `CAP_NET_ADMIN`（`sudo` 需交互认证），而 `unshare -Un --map-root-user` 出来的
非特权 user+netns 里能加、能生效，所以**每一臂都在一个全新的 ns 里跑，注入的作用域是那一个 ns 的 `lo`**，
宿主回环从头到尾是 `noqueue`（脚本开工前后各核一次）。判据读数与四条路在 [docs/ENVIRONMENT.md 5.4](docs/ENVIRONMENT.md)，
六臂对照与那条**被实测改写过的机制句**在 [docs/FAULT-INJECTION.md §25](docs/FAULT-INJECTION.md)：
网络层 delay 把建连也算进等效耗时，所以"慢与死的分界线"从 `probe_timeout` 移到 **`probe_timeout/4`**；
而 loss 那一臂经 TCP 重传把**探活**推过超时（业务侧却几乎全 200）——**探活这条路比业务更怕丢包**。
限流那半句**已经在 S5-04 里被 DOM 断言验过一次**，不靠眼睛。

---

## 诚实的边界

做不到的事写在纸上，不靠读者自己踩。

**热重载的定义域**（[ADR-10 追加节](docs/adr/ADR-10-hot-reload-not-hot-restart.md)）：

- **限流参数改了不生效**（拒载并点名）。`rate`/`burst` 是 `const` 成员、桶不回收：换新桶表等于排水期两张表各带一份满预算 + 已积累令牌静默清零；改成可变参数等于老键永远用旧参数——那才是字面意义的半套生效。两条都比"不接受这次改动"更糟，所以我选窄的那边：**要改限流参数就重启**。
- **段的出现/消失改了不生效**（同样拒载）。`routes`/`health`/`rate_limit` 段在不在决定装不装那一环 filter、建不建那张表——那是链的**形状**，不是链的内容。
- **`source: mysql` 的组与文件重载互斥**（启动就退出码 2）：节点表已经有一条发布路径，再叠一条就是两个节拍往同一个下标空间写两份真相。
- **一条请求的失败记账用的是"记账那一刻"的参数**，不是它当初做路由决定时那一代。替它的代价是每连接 +16 B 并撞 `sizeof(Connection)==528` 那条 `static_assert`——拿每连接内存换一条不破的不变式，**不换**。
- **生效延迟报的是毫秒**：133–205 ms（节拍 200 ms，粒度由节拍决定），**没有一处写成"秒级"**。

**命令行与配置文件各管什么**：监听地址、`--workers`、Redis 参数、`--config-reload-ms` 这些**部署事实**留在命令行、不进配置文件，也不参与重载；路由/后端表/健康与熔断参数才是配置文件的东西。两个来源同时给＝拒绝。

**限流**：桶键数上界 16384，溢出键共用一只桶并计数；Redis 那一路的余额上报有一个结算周期的滞后（上界 = 该键一周期内的放行数），这条代价写在 B-017 第四节。

**WebSocket**：不做消息级限流与消息级熔断，只能做连接级（[ADR-09 代价段](docs/adr/ADR-09-websocket-passthrough.md)）。

**成本读数要带着分辨率读**：`p50/QPS 无可分辨`**不等于"免费"**——B-018 那一档的分辨率是一个 HDR 桶 ≈128 µs/请求，比 B-008 那把 8.7 µs 的尺大一个数量级以上，"无可分辨"只排除"掉进 µs 级的悬崖"、**不排除个位数 µs**。B-020 更直接：每请求那一格**读不出方向**（off 臂自己跨轮漂了 24%），所以"arm 便宜"这句我不说，而同一轮的常驻那一格 **+19.1 MiB** 无歧义（两臂组内极差 <0.3 MiB）。**两句分开写，不拿后者的"读不出"去洗前者。**

**"干净"都带反证**：ASan / LSan / UBSan / TSan 我全部用故意写坏的样本验过**能红**；阳性验证必须真打红，第一版打不红的逐条弄清为什么（**不改用例凑红**）。也留着一份不体面的清单——每一轮"仪器侧我自己错了哪几处"都在 [FAULT-INJECTION](docs/FAULT-INJECTION.md) 的 §20.1 / §21.2 / §22.2 / §23.2 里逐条列着，其中相当一部分**如果没被抓住，会被读成比事实更强的结论**。还有一次"拆了也不红"的诚实记录：把隧道收尾从 `kFlushThenClose` 换成 `kDiscardOutbound` 全绿，因为 4096 字节的回显早被内核套接字缓冲吸收了——那条判据因此归 S1-04 的既有用例，不归这条。

**一条至今没能归因的事实**：真脚本在全跑形状（真容器 / 真子进程 + 真浏览器）下会打 FAIL 而 `echo $?` 读成 0，而 11 组最小复现与 4 个截断版全是 rc=1——我没找到具体那一句。处置不是解释而是绕开：**判定写进 `verdict.txt`，门禁读盘**。

---

## 明确不做的东西

每条都有 ADR 写明理由，不装作它们不存在：

> HTTP/2 / gRPC / QUIC · TLS 握手 · 第三方网络库（asio / muduo / brpc） · etcd / xDS / K8s / Service Mesh · 动态服务发现 · **响应缓存**（缓存是**另一种网关形态**——Apache Traffic Server 的全部价值就在缓存，做了它 p99 就不再度量流量治理） · WebSocket 帧解析（只做 Upgrade 后透传） · 多进程 `SO_REUSEPORT` · Web 管理后台 / 登录 / 配置可视化编辑器（那些是 CRUD，且会把评审注意力从 p99 拉到 UI） · OpenTelemetry 与分布式追踪 · 多副本下的 Redis 计数一致性实测 · 加权负载均衡（要做先进 ADR-19 并连带写清它与健康检查摘除安全阀分母的相互作用）

还有一个**反向承诺**：**io_uring 对比分支、连接对象池 / 自定义分配器、日志异步化全部挂起**。只有当压测 + perf 火焰图拿出证据证明那是真实瓶颈时才立项，且先写 ADR。**在没有证据的情况下优化，是空壳项目的典型形态。**

---

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
         smoke-s4-04 smoke-s4-05 smoke-s4-06 smoke-s5-01 smoke-s5-02 smoke-s5-03 smoke-s5-04; do
  PRESET=debug bash scripts/$s.sh         # 十三条冒烟。需要 Docker 的两条是 s3-04（MySQL 8.0.36）与 s4-04（redis:7.2.5）：
                                          #   容器不在位时脚本在阶段 0 自己拒跑，不换一个顶上去
                                          # s5-03 / s5-04 的判定读 build/smoke-*/verdict.txt，不信退出码
                                          # s5-04 还要一条工具依赖：Windows 侧 chrome.exe 无头取 DOM，
                                          #   不在位同样拒跑（登记在 ADR-26 第四节与 ENVIRONMENT 5.2）
done

scripts/bench/s1-p99.sh                   # SLO #1 标准口径（canonic 由参数自己判，非标那轮不进文档）
scripts/bench/s1-p999.sh                  # SLO #1 的 p999 半边：每臂自报观察者 CPU；1000 并发那档目前判"取不到"
scripts/bench/c100k.sh                    # SLO #2：多目标地址长连接稳态 + 每连接对内存 + HDR 分位
scripts/bench/s2-qps-vs-nginx.sh          # SLO #3 五档：nginx 三档 + 网关 + 直连天花板
scripts/fault/s3-05-matrix.sh             # 故障注入第一轮五类；第二轮叠加五类见 s4-07-matrix.sh
```

非标准口径与排水档这类"怎么把旋钮拧对"的写法，写在上面每个脚本自己的 `--help` 与 [docs/SLO.md](docs/SLO.md) 的脚本索引里，README 不抄第二份。「ASan 干净」这句话不该靠我手敲，所以有一条命令能复现它——**同理每个数字都能一键重跑**，这是契约第 7 节对这几份文档的完成判据；**长跑判定只认落盘输出**。

---

## 阶段门禁与文档地图

| 阶段 | 内容 | 状态 | 门禁读数 |
|---|---|---|---|
| **S1** | 单连接转发 + 第一份 p99 | ✅ 八条全 DONE，已 push | ASan/UBSan 干净（带反证）、真 curl 10000 请求全数对上、`fd` 与存活连接归零、第一份 p99 |
| **S2** | 多 Reactor + 连接池 + 跨线程销毁 | ✅ 八条全 DONE，已 push `47bca67..dcccca5` | **TSan 零 data race**；#2 达标、#1 未达标但第一次测到多 Reactor 有效（33 ms → 4–6 ms 带）、100k 负载下排水 **0.947 s** |
| **S3** | 路由 + 负载均衡 + 健康检查 + 安全阀 | ✅ 六条全 DONE，已 push `e54ae61..17b9234` | 摘除 **202 ms**、误摘 **0/3480**、安全阀 10 台全死停在 **1/10**、五类故障矩阵全过 |
| **S4** | 限流 + 熔断 + 超时与重试防放大 | ✅ 九条全 DONE，已 push `17b9234..0f54343` | 429 生效、熔断恢复时间线、Redis 三段降级、重试放大 **1.50 / 2.00** 实测、配对账两处退化 |
| **S5** | 热重载 + 指标 + 面板 + 调试页 + 终判 | **进行中**：S5-01…S5-04 DONE；剩 S5-05（compose）、S5-06（netem）、S5-07（终判）、S5-08（收尾） | 三 preset 各 **376/376**、非 gtest 警告各 **0 条**、`tsan` 日志 `ThreadSanitizer` **0 次**（374 PASSED + 2 SKIPPED）、**十三条冒烟**逐条 ✗ 计数 **0** |

**逐条验收的读数不在这里。** 每条任务的可断言验收句与实测数字在 [TASKS.md](TASKS.md) 表格末列；压测与成本对照在 [docs/BENCHMARK.md](docs/BENCHMARK.md)（B-001…B-020，含未达标记录与**口径变更节**）；故障注入的预期 vs 实测在 [docs/FAULT-INJECTION.md](docs/FAULT-INJECTION.md)（§1–§23）；阶段门禁对账在 [docs/ROADMAP.md](docs/ROADMAP.md)。**同一份证据抄第二份，迟早会变成两份互相分叉的**——所以这一节只留结论与指针。

<details>
<summary><b>文档地图与 26 条 ADR</b>（点开）</summary>

| 文档 | 内容 |
|---|---|
| [CLAUDE.md](CLAUDE.md) | **唯一契约**：身份约定、三个数字、三条禁令、门禁四件套、版本锁定表 |
| [TASKS.md](TASKS.md) | 唯一真相源：任务 ID / 可断言验收标准 / 状态 / 实测数字 / 已裁清单 |
| [docs/SLO.md](docs/SLO.md) | 三个数字的目标与**写死的**测量条件、脚本索引 |
| [docs/ENVIRONMENT.md](docs/ENVIRONMENT.md) | 环境自检实测版本、唯一实质偏离（libmysqlclient）、sanitizer 阳性验证、perf 能否采样、Docker 与抓取通路、无头浏览器仪器 |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | **已实现的现状**（严格区分"已实现"与"已定但未实现"）：模块 1.1…1.37 + 第 5 节踩坑索引（格式固定「现象 / 根因 / 防复发的断言」，写不出第三条的明写"缺"） |
| [docs/BENCHMARK.md](docs/BENCHMARK.md) | 压测方法 + vs Nginx 流程 + 实测（含未达标照记与口径变更节） |
| [docs/FAULT-INJECTION.md](docs/FAULT-INJECTION.md) | 故障矩阵预期 vs 实测 + 每轮阳性验证与仪器自错照记 |
| [docs/INTERVIEW.md](docs/INTERVIEW.md) | 60 秒故事，以及"没有它会怎样"的实测版 |
| [docs/ROADMAP.md](docs/ROADMAP.md) | 五阶段与每阶段**门禁结果**、一键复现、遗留账目 |

**每条 ADR 都写清"我放弃什么"**：01 HTTP/1.1 · 02 epoll ET + io_uring 挂起 · 03 多线程 Reactor · 04 网络层自研 · 05 Redis 只做分布式计数与降级 · 06 MySQL 配置与审计 · 07 不引 etcd/xDS/K8s · 08 不做响应缓存 · 09 WebSocket 只透传 · 10 热重载不是热重启 · 11 filter 链 · 12 摘除比例安全阀 · 13 应用层故障注入为主 · 14 连接所有权 · 15 per-worker 池 · 16 请求-响应配对 · 17 C100K 与 HDR 测量手段 · 18 路由 filter · 19 负载均衡与配置文件 · 20 MySQL 节点表 · 21 超时与限流 · 22 自产错误响应 · 23 connect 失败重试 · 24 指标暴露面 · 25 旁路 percentile exporter · 26 演示页的握手对端与取数仪器

</details>

---

## 许可

MIT — 见 [LICENSE](LICENSE)。
