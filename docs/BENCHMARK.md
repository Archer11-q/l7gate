# BENCHMARK —— 压测记录

规矩只有三条，都是为了让"半年后这份数字还能不能用"这个问题有答案：

一，**一次压测一条记录**，原始数字（不是截图里的均值）+ 结论 + 环境参数一起写。契约第 7 节对这份文件的完成判据点名了三样：CPU 绑核、分配器、各组件版本号。少任一条，这份记录就只是"我记得当时挺快"。

二，**口径变更必须留一条记录**：改了哪条、为什么改、改前改后是否可比。不许把不可比的两次放在一起画趋势线。

三，**达标判定只认 `canonic=1` 的那一轮**（`scripts/bench/` 里的脚本自己判口径，不由我嘴上说）。非标准口径的跑法允许存在，但它的身份是**归因实验**，标题里就写明。

---

## B-001 · SLO #1 转发尾延迟 —— 第一份取数

| 项 | 值 |
|---|---|
| 日期 | 2026-10-26 14:45–14:48（本地） |
| 被测 | `l7gate` @ 工作树（HEAD `58558bf` + 未提交的 S1-06 代码，提交哈希由 `feat(S1-06)` 补记） |
| 构建 | `release` preset（`-O3`，无 sanitizer）；`l7gate` sha256 前缀 `b4e0cdc224441d3c` |
| 口径 | `canonic=1`：1 KiB 响应体 / 1000 并发连接 / 稳态 60 s / 预热 10 s / wrk 8 线程 / hey 30 s 交叉验证 |
| 上游 | `l7gate_bench_upstream`（本仓库 `tools/`，单线程 epoll ET，固定 1 KiB 体，`--max-clients 3256`） |
| 原始输出 | `build/bench/s1-p99-20261026-144509/`（`*.wrk.txt`、`*.hey.txt`、`gateway.log`、`upstream.log`、`s1-p99.summary.txt`） |
| 复现 | `scripts/bench/s1-p99.sh`（先跑 `scripts/bench/env-snapshot.sh` 落环境） |

### 环境（`build/bench/env/env-20261026-145124.txt`）

WSL2 / Ubuntu 26.04 LTS / Linux 6.18.40.1-microsoft-standard-WSL2 / AMD Ryzen 7 7435H / `nproc`=16 / 3094 MHz。
**绑核：未做**（脚本不使用 taskset/cpuset；绑核方案按 `ENVIRONMENT.md` 第 7 节留到 S4 定稿。这不是口径变更，是从未声明过绑核）。cpufreq governor 在 WSL 下不可读。
**分配器：glibc**（`ldd` 显示 `libc.so.6`，`LD_PRELOAD` 为空；本机虽装有 `libjemalloc.so.2` / `libtcmalloc`，但都没链上——换分配器按契约 8.2 要先 ADR，而它直接改变下面每个数字的定义）。glibc 2.43，gcc 15.2.0，clang 21.1.8，cmake 4.2.3，wrk debian/4.1.0-4build3，hey 0.1.4+ds-2，curl 8.18.0。
`ulimit -n`=10240，`somaxconn`=4096，`ip_local_port_range`=32768–60999，THP=`always`，仓库在 **ext4**（不是 9P/drvfs，契约 9.4）。

### 结果

| 组 | p50 | p75 | p90 | p99 | QPS |
|---|---|---|---|---|---|
| 直连上游（wrk） | 16.350 ms | 17.330 ms | 18.430 ms | **24.850 ms** | **59,860.19** |
| 经网关（wrk） | 53.350 ms | 57.030 ms | 61.390 ms | **82.390 ms** | **18,229.47** |
| **增量（经网关 − 直连）** | 37.000 ms | 39.700 ms | 42.960 ms | **57.540 ms** | −41,630.72（−69.5%） |

hey 交叉验证（连接模型不同，只看量级）：直连 p50/p90/p99 = 12.3 / 14.8 / 22.1 ms @ 77,557.66 QPS；经网关 = 50.0 / 59.6 / 69.7 ms @ 19,548.33 QPS。两个工具给出的增量落在同一量级（57.5 ms 与 47.6 ms），说明这个尾巴不是某个工具的形状。

自报账（网关侧，与 wrk 口径独立）：`requests_parsed=1,872,093`、`responses_completed=1,871,777`、`parse_calls=1,872,093`、`header_scan_bytes == bytes_down_to_upstream == 127,818,483`（每字节只扫一次这条不变量在 185 万请求上仍然成立）、`gateway_replies_sent=0`、`upstream_truncated_before_head=0`、`upstream_truncated_in_body=0`、`high_water_hits=0`。
生命周期：`live_connections=0`、`fd_start=fd_end=9`、`connections_constructed == connections_destructed == 6,008`、`epoll_registrations(6,010) − 2 == epoll_unregistrations(6,008)`（那 2 是监听 fd 与 eventfd，常驻不注销）。
压测上游侧：`accepted=6,105`、`refused_over_capacity=0`、`refused_addfail=0`、`peak_clients=1,102`（容量 3,256，仪器没有限速）。

### 结论：**未达标**，且 p999 半边当前无法判定

SLO #1 的目标是增量 **p99 ≤ 1.0 ms、p999 ≤ 5.0 ms**。实测增量 p99 = **57.540 ms**，超目标 57 倍。按契约 8.2 禁令二，目标值不动，这里照记并归因。

p999 不是"没测到"而是**这套工具链取不到**：wrk 4.1.0 的 `--latency` 只打 50/75/90/99 四行，hey 0.1.4 的分位表只到 99%。要拿到任意分位需要换/加发压工具（wrk2、vegeta、自研皆算新依赖），按契约 8.2 禁令三要先 ADR，按交接段授权 (a)(b) 两项都要停下来请示——所以这一格记"无法判定"，不记 0。

### 归因

**一，这个 57.54 ms 里绝大部分是排队，不是网关的加工成本。** wrk 是闭环模型：1000 条连接，每条"发一条、等一条"，于是稳态下 `延迟 ≈ 连接数 / QPS`（Little 定律）。验算：直连 1000/59,860 = 16.7 ms ≈ 实测 p50 16.35 ms；经网关 1000/18,229 = 54.9 ms ≈ 实测 p50 53.35 ms。两组都贴合到 3% 以内。所以"经网关 p99 = 82 ms"读作"网关把服务速率降到了 1/3.3，于是每条连接在闭环里多排了 38 ms 的队"，而不是"网关每请求多花 38 ms"。

**二，换算成每请求服务时间，网关引入的是 ≈38 µs，不是 38 ms。** 直连时上游单线程 16.7 µs/请求；经网关时瓶颈线程（网关）54.9 µs/请求。差值 ≈38 µs 是网关自己的 CPU 成本。旁证：把并发降到 50（`build/bench/s1-c50/`，非标准口径，仅作归因不作判定），增量 p99 从 57.54 ms 塌到 **1.36 ms**、p50 从 37.0 ms 塌到 **1.09 ms**——尾巴随并发线性收缩，正是排队主导的形状，而残下的 ~1 ms 量级才是每请求成本在闭环里的表现。

**三，每请求成本的可观测来源：单 Reactor + 每请求 5.0 次 `epoll_ctl`。** `epoll_modifications=9,359,517 / requests_parsed=1,872,093 = 5.0`。S1 的形状本来就是一条事件循环线程（ADR-03、S1-04），挂/摘 `EPOLLOUT` 各一次 × 两个方向 + 收尾一次是这套 ET 写法的固有支出。这两条都是**已计划的后续任务**要动的地方：多 Reactor 与连接池在 S2（`TASKS.md` S2 行），背压动作在 S1-07；本任务不改结构去追数字。

**四，尾延迟的放大机制说明 S1 的这个数字不能横向比。** 同一份网关代码，1000 并发下增量 57.5 ms、50 并发下 1.36 ms。SLO #1 写死了 1000 并发，所以判定按 57.5 ms 记未达标；但真正要向"≤1.0 ms"逼近的路径是每请求服务时间（38 µs 这一头）与并发分摊（S2 那一头），把这条写在这里是为了下一次改代码时有正确的成功判据，而不是去调口径。

### 另两个数字（门禁四件套第 1 条要求同场记录）

**自测 QPS（经网关，wrk 闭环 1000 并发，60 s 稳态）= 18,229.47**；直连同一上游 = 59,860.19。按卡点三，S1 只交这个数字、不设判定基线（vs Nginx 的基线在 S3/S5，Docker 尚未可用）。

**每连接内存（口径快照，不是 SLO #2 的判定）**：`sizeof(Connection)=512`（S1-04 时是 400，S1-06 加上 `ResponseTracking` + `ChunkedProgress` 后 +112 B）；单连接入向 buffer `peak_capacity=4096`（`S1_02_Buffer.PeakCapacityUnderOneKiBKeepAliveWorkload` 在本轮 100 条里继续通过）；空闲 128 条下游连接由 `S1_04_Reactor.MemoryPerIdleConnectionIsMeasured` 每轮自测，本轮读数 **RSS 5,640→6,320 KiB、增量 680 KiB、每连接 5.31 KiB**、fd 峰值 265。**负载态**另有一条：B-001 这一轮 1000 对连接在飞时 `VmRSS` 从 4,072 KiB 涨到 23,800 KiB，即 ≈19.7 KiB/连接对（含每连接 4 KiB 入向 buffer、惰性出向 buffer、header scratch 与分配器取整；SLO #2 的口径是**100k 空闲长连接**，这条负载态观测不参与判定，写在这里是为了让 S2 之后的曲线有个起点）。

### 可比性声明

下一次要和本记录比，需逐项对上：`release` preset、未绑核、glibc 分配器、同一个 `l7gate_bench_upstream`、`CONNECTIONS=1000 / DURATION=60s / WARMUP=10s / WRK_THREADS=8 / BODY_BYTES=1024`、`canonic=1`。其中任何一项变了，两条数字就只是并列关系，不是趋势。

---

## B-002 · 并发对照（归因实验，非标准口径，不作判定）

`CONNECTIONS=50 WRK_THREADS=4 DURATION=5s WARMUP=1s`，其余同 B-001。原始输出 `build/bench/s1-c50/`。

| 组 | p50 | p75 | p90 | p99 | QPS |
|---|---|---|---|---|---|
| 直连 | 0.633 ms | 0.720 ms | 0.807 ms | 1.010 ms | 72,940.69 |
| 经网关 | 1.720 ms | 1.840 ms | 1.970 ms | 2.370 ms | 27,509.96 |
| 增量 | 1.087 ms | 1.120 ms | 1.163 ms | **1.360 ms** | −45,430.73（−62.3%） |

用途只有一条：证明 B-001 的尾延迟随并发收缩，从而把"排队"与"每请求成本"分开。**这份数字不得进任何达标判定**——它的并发与时长都不是 SLO #1 写死的那一套。

---

## 待补（SLO 脚本索引里还没落地的）

| 项 | 阻塞点 |
|---|---|
| `s2-qps-vs-nginx.sh`（#3） | Docker WSL Integration 未开，`nginx:1.25.4` 基线镜像跑不起来；绑核方案也要在 S4 前定稿 |
| `c100k.sh`（#2） | 依赖自研 C100K 长连接客户端与 S2 的多 Reactor；`ulimit -n` 当前 10240，100k 场景必须先解决 |
| p999 的可取数方案 | wrk 4.1.0 / hey 0.1.4 都不产出 99.9% 分位；换工具属新依赖，需 ADR + 请示（见 B-001 结论第二条） |
