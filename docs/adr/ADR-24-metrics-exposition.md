# ADR-24 · 指标暴露面：自实现纯文本 `/metrics`、独立 `--metrics-port`、汇聚不给 worker 循环加一次 syscall（S5-01）

- 状态：已采纳（S5-01 起草；本 ADR 落盘在 `src/` 之前，走的是契约 8.2 第一条的顺序）
- 日期：2026-11-03
- 相关：ADR-04（那张「用现成 / 自己写」的表里 `prometheus-cpp` 那一行留的正是这个分支）、契约 9.1 原文（「若编译成本过高，允许改用纯文本 `/metrics` 端点自行实现——**但必须先写 ADR**」）、已裁第 ⑨ 条第 1/2 项、ADR-17（「测量手段变更要留记录」的先例）、ADR-12 追加三 + ARCHITECTURE 1.31 边界⑥（`health_ejected_now` 那句限定）、ADR-19 追加第四条（探活不是 `Connection`）、B-018（每请求 `epoll_modifications` 那把结构性尺 + 「本档分辨率 ≈128 µs/请求」那句限定）、S2-07/B-011（RSS 在 128 连接尺度不是可用仪器）、口径第 3 条「仪器不进被测方」

## 这一节在裁什么

已裁第 ⑨ 条第 1/2 项把两条岔路钉死了：**自实现纯文本 `/metrics`，不引 `prometheus-cpp v1.1.0`；观测面走独立 `--metrics-port`，默认关闭，关掉时代码路径与今天逐字节相同**。这两句是结论，不是实现。这一节把它摊成可写的代码形状，要回答的是起草时躲不掉的六个问题：

1. 「不引库」到底不引到什么程度——文本格式的语法正确性由谁兜底；
2. 独立端口上跑的这条服务，凭什么不算「另一条业务路径」；
3. 「仪器不进被测方」在这一条线上的**具体**含义（不是再抄一遍口号，是逐条给出代码落点）；
4. 全 worker 的计数怎么汇聚，而**不给 worker 循环加一次 syscall**——这里有一处真矛盾：退出账的 77 个键里有一族是**每 worker 各一份的非原子量**，今天根本不允许在循环还转着的时候读；
5. 键名与 `# TYPE`：有一条族的键名在 Prometheus 的语法里是**非法**的，而验收句写的是「键名不许改」；
6. 判据形状：五条验收各自怎么写成断言，阳性验证（真打红）在哪。

编号说明：ADR-23 起草时我已经把 24 这个号留给「下一个新依赖/新形态决策」，本节就是它。

---

## 一、依赖形态：一个第三方依赖都不加，文本语法由我自己的单测钉死

契约 9.1 那行允许我二选一，我选自实现，理由按已裁第 ⑨ 条第 1 项的三条展开，但把「自实现」的边界写清——**它不只是「不引 prometheus-cpp」，还包括不引任何 HTTP 解析**：

| 这一面上我要写的东西 | 我不写的东西 |
|---|---|
| `GET /metrics` 的**请求行**识别（method + target + version 三段，够判就行） | 完整 HTTP 报文解析、header 表、chunked、keep-alive 状态机 |
|  exposition 文本渲染（`# TYPE` 行 + `name value` 行） | OpenMetrics、`# EOF`、`# UNIT`、exemplars、`_created` 系列 |
| 一个只有三拍的连接状态机（读→写→关） | filter 链、`Connection`、`Buffer`、配对模型（ADR-16） |

判断标准沿用 ADR-04 那句：**它是否承载本项目要证明的能力**。指标文本格式不承载——它是一段 40 行的字符串拼装；而「运行期观测面不能改动被测路径」承载，所以那部分我必须自己设计（第四、五节）。

代价我照记，不粉饰：**文本格式的正确性没有第三方兜底**，出了非法字符、`# TYPE` 漏了一条、值里写出 `NaN`，Prometheus 只会把整个 scrape 判失败或静默丢序列。所以格式的约束必须进 ctest（第七节判据 ① 那组），而不是靠「看着像对」。契约 9.1 那行的原话是「必须先写 ADR」，我理解为：**选了这条路，格式的责任就从库转到了我的测试上**，这笔账要在账本上可见。

**格式选择记死**：Prometheus text exposition format **0.0.4**（`Content-Type: text/plain; version=0.0.4; charset=utf-8`）。不选 OpenMetrics，因为它要求 `# EOF` 与 counter 名的 `_total` 后缀，而后一条与「键名不许改」正面冲突（第五节）。

---

## 二、端口形态：`--metrics-port <PORT>`，缺省=关闭，`0`=内核选端口

- **只有一个新命令行旋钮**，不配进 `--config`。理由是 `main.cc:459-460` 已经写过的那条切分：「它是部署事实不是流量策略，所以不进配置文件（与 `--nodes-schema` 同一条切分）」——观测面开不开、开在哪个端口，是部署事实。已裁②的切分（`--config` 只装策略、监听与 `--workers` 留在命令行）在这里必须原样成立。
- **缺省值 = 不打开**。开启形态用「标志缺席」表达，不用 `0` 表达，因为 `0` 在这个仓库里已经有含义：`Reactor::Config::listen_port = 0` 是「由内核选端口」，`reactor.h:47-48` 明写着「测试全靠它避免端口撞车，所以这个 0 是功能不是便利」。所以我用 `std::optional<int>`（哨兵 `-1`）表示关闭，`--metrics-port 0` 仍然表示「内核给我一个端口」——**测试就靠这一条，不需要端口表**。范围 `0..65535`，越界退出码 2（与其余数值标志同形，`main.cc:603-613` 那个 `--workers` 分支就是模板）。
- **绑定地址 = `--listen` 的那个 ipv4 地址**，不新增 `--metrics-ip`。缺省 `--listen` 是 `127.0.0.1`（`reactor.h:46`），所以缺省形态下 `/metrics` 只在本机可见；容器里部署方已经要把 `--listen` 写成 `0.0.0.0:P`，那时观测面与业务面同样可达，这是 S5-05 编排要处理的事实，不在这里发明第二条暴露路径。**代价照记**：`--listen 0.0.0.0` 的部署里 `/metrics` 对外可读，本 ADR 不提供鉴权（3.3 明写不做的那一类），靠部署侧网络隔离。
- **不与业务端口共用一个 socket**。已裁②第 2 项给的理由在这里成立且我不重复论证：走主端口就要被 catch-all 路由命中、进限流键、给「每请求成本」掺一条额外路径。

### 关闭时的形态判据（这是验收 ⑤ 的落点，不是注释）

「逐字节相同」这句我不能只说，要写成能被脚本判的形态。我把它钉成三件：

1. **不 spawn 线程、不创建监听 fd、不创建 `Epoll` 实例、不创建 eventfd**——即 `--metrics-port` 缺席时，第四节那个每 worker 快照槽的指针恒为 `nullptr`，第五节的聚合代码一次都不执行；
2. **退出账的键集合与今天完全相同**——`[serve]` 那 77 个固定键 + 每目标 10 键，一个不多一个不少（新增的 `metrics_*` 三键只在开启时出现，理由见第六节）；
3. 循环里唯一残留的差异是每拍（不是每事件）一次 `nullptr` 判空，量级与 `sync_nodes_snapshot()` 的「空转前的一次判空」（`reactor.cc:327-329`）同一族。

**这三件里我能给硬断言的是前两件**，第三件是成本判据，归第七节 ③/④ 那两臂实测。

---

## 三、「仪器不进被测方」在这一条线上的具体含义

这条口径在 S3-03（探活）与 S4-05（错误率窗口）各有一次落地，两次的共同形状是**按类型排除，不是按 if 分支排除**：探活不是 `Connection`，所以 `requests_parsed` 那个唯一自增点（`connection.cc:184-187`）对它**结构上不可达**（`reactor.h:468-472` 那句「这条是验收，不是注释」）。我按同一形状做，落成五条可核对的断言：

| # | 含义 | 代码落点 | 反证（哪一条会被写坏） |
|---|---|---|---|
| 1 | 抓取连接**不是** `Connection`，不进 filter 链、不进路由、不进限流键 | 指标服务自带一个三拍状态机 + 固定小缓冲，走探活那一族的先例（`reactor.h:478-497` 的 `Probe`：自己的 `char request[512]`、`::send`/`::recv`，绕开 `Buffer`） | 若有人图省事把 metrics 监听塞进 `ReactorGroup` 的 accept 派发 → 抓取会进 `downstream_accepted`/`requests_parsed`/`responses_completed`/`connections_constructed`，第七节 ② 那条 ctest 立刻红 |
| 2 | 抓取连接**不占** worker 的 epoll，所以不动 `epoll_*` 三键 | 指标线程有自己的 `Epoll` 实例；`Epoll::modifications_` 是**每实例**的量（`epoll.h:68-72`），第四节的聚合只累加 worker 的那几份 | 若把 metrics 监听注册到某条 worker 的 epoll → 每次 scrape 给那条 worker 加 `EPOLL_CTL_ADD/MOD/DEL`，③ 那臂的「每请求 `epoll_modifications` 逐数相等」会被 scrape 节拍污染 |
| 3 | 抓取**不叫醒**任何 worker | 汇聚方向是「属主线程按拍自己抄」，采集线程**只读原子**；不使用 `Reactor::handoff` / `eventfd` 唤醒那条路（那条路会让每条 worker 多一次 `epoll_wait` 返回，把 `event_batches` 与 SLO #2 的空闲 CPU 那一格改成仪器造成的数） | 改成「scrape 时广播索取」→ `event_batches` 随 scrape 次数增长，B-007 的空闲 CPU 判据失真 |
| 4 | 抓取**不拿**业务热路径上的锁去做长活 | 唯一会拿的锁是 `RateLimiter::mu_` 里一次 O(1) 的 `buckets_.size()`；O(K) 走查（`rate_limit.cc:119-122`，`kMaxKeys = 16384`）不在 scrape 路径上——见下 | 若 scrape 持 `mu_` 走 16k 个桶，而每条请求都要同一把锁（`rate_limit.cc:71`），一次抓取就把 16 条 worker 的限流判据排在后面——**这就是仪器进被测方最便宜的实现方式** |
| 5 | 仪器自己的账**不混进**业务键 | 新增且仅新增三个键：`metrics_scrapes_ok` / `metrics_scrapes_failed` / `metrics_render_bytes`，只有指标线程碰它们，永远不进 worker 线程 | 若把它们并进 `ReactorStats`（那张全 worker 共享的原子表）→ 仪器的写进了被测方的表，口径脏了 |

第 4 条那个例外必须单独交代，因为它是我在本 ADR 里唯一一处**为了让运行期能拉，去改动被测方代码**的地方：

> `rate_limit_capped_rejects`（「本地夹子替 Redis 挡下的拒绝数」）今天只在退出账出现，而它的取值要拿桶表 mutex 走一遍全表（`rate_limit.cc:119-122`，注释 `rate_limit.h:105-107` 原话是「只在退出报告与用例里调，它要拿桶表的锁」）。pull 模型下「只有退出才有」等于面板上永远没有这一格。
> **我的裁法**：在 `RateLimiter` 上加一个聚合原子 `capped_rejects_total_`，与各桶自己在「夹子咬到」那一格旁边同拍 `fetch_add`（`rate_limit.cc:53` 那一行的邻域，**不是每请求**，只在 429 支路上）。锁走查保持原样、退出账一字不改。
> **代价照记**：这是仪器导致的一处被测方改动，落点在 429 路径。它的成本**不许声称免费**——第七节 ③ 那臂必须带上「Redis 在位且夹子真咬到」那一档（S4-04 冒烟阶段 4 的冻结形态就是现成的注入器），量出来写进 BENCHMARK；B-018 当年取的是 `capped_rejects=0` 那一档，那一档量不到它。

---

## 四、全 worker 计数怎么汇聚，而不给 worker 循环加一次 syscall

先给地基事实，因为它把问题砍掉一半：**`ReactorStats` 不是「每 worker 一份」，是全 worker 共享一份、50 个字段全是 `std::atomic<std::uint64_t>`、读写全 `relaxed`**（`reactor_group.h:30`：「`ReactorStats` 全 worker 共享一份（全原子，并发 fetch_add 安全、聚合自动成立）」；`main.cc:318-319`：「多 Reactor 下这是共享 stats 的自动聚合（全原子）」）。所以 77 个键里 **50 个「汇聚」根本不成立**——没有要求解，采集线程 `group.stats()`（`reactor_group.h:159-160`）做 relaxed load 就完事。已裁④给的两个选项里，这一半天生就是「原子表直读」。

麻烦在剩下那一族。`epoll_registrations` / `epoll_unregistrations` / `epoll_modifications` / `read_disarmed_connections` 这四个键今天**不是**从 `ReactorStats` 取的，而是 `ReactorGroup` 在 `run()` 返回、全部 worker join 之后，遍历 worker 求和得到的（`reactor_group.cc:305-309`，聚合入口的守卫注释 `reactor_group.h:162-167` 写着「只在 `run()` 返回之后调用」）。它们的底层是普通 `std::uint64_t`（`epoll.cc:40-51` 里那句 `++modifications_`），而 `stats.h:25` 的纪律原文是：

> 非原子的观测（epoll 计数、live_connection_count()）不进这条规则，它只能在循环线程 join 之后读。

这条纪律不是形式：S1-08 在循环还转着的时候读 `epoll_registrations()`，TSan 当场报 data race，修法是「先 `stop_loop()` 再读」，记在 ARCHITECTURE 的坑索引里。**所以「运行期可读」这一格没有现成路径，必须新造**——这才是 S5-01 真正的工程内容，不是字符串拼装。

### 我的选型：属主线程按拍抄进原子槽（已裁④的「owner worker 各自快照 + 采集线程汇总」）

```cpp
// src/net/loop_observation.h（新增）
struct alignas(64) LoopObservation {            // 每 worker 独占一根 cache line
  std::atomic<std::uint64_t> epoll_registrations{0};
  std::atomic<std::uint64_t> epoll_unregistrations{0};
  std::atomic<std::uint64_t> epoll_modifications{0};
  std::atomic<std::uint64_t> read_disarmed_connections{0};
};
```

- `Reactor` 多一个 `LoopObservation* snapshot_`，**关闭时恒为 `nullptr`**；`ReactorGroup` 持一个 `std::vector<LoopObservation>`，声明顺序必须在 `workers_` **之前**（`reactor_group.h:185-189` 那条析构顺序规矩是给 `stats_` 立的，同一根理由：worker 持有指向它的裸指针）。单 Reactor 那一臂同样给它一格，两臂形态对齐。
- 写入点在循环**已有的那一拍**（`reactor.cc:314-332` 那段 tick，`wait_timeout_ms` 缺省 200 ms）：`handle_events` 之后，三个 O(1) 的量直接 `store` 属主自己那份 `Epoll` 计数。
- **为什么不把 `Epoll` 的计数器本身改成原子**（另一条看似更省事的路线）：今天 `++modifications_` 打在属主自己的私有内存上，16 条 worker 零争用；改成一张共享原子表就把 4 次/请求的 RMW 全压到同一根 cache line 上，而那正是 B-018 用来判「限流/熔断/Redis 三件不进 syscall 面」的同一把尺——**为了观测去改动被观测的那条指令，方向反了**。改成每 worker 一份原子（padding 好）能避开争用，但它仍然把热路径上的 `++` 换成 `lock xadd`，每请求 4 次。快照路线的代价是「每 200 ms 一次拷贝」，与每请求 4 次不在一个分母上。
- **O(n) 那一格单独降频**：`connections_with_read_disarmed()`（`reactor.cc:408-419`）要遍历注册表。它自己的注释就写着「它不是计数器（不需要循环在跑的时候也准）」，所以我按每秒至多一次抄（`kMetricsSampleEveryMs = 1000`），而不是每拍。**时间源不新增**：`monotonic_ms()`（`reactor.cc:34-38`，`steady_clock`）在同一个迭代里已经被 `enforce_upstream_timeouts(monotonic_ms())`（`reactor.cc:332`）无条件调过一次，我把那一处提成局部变量复用，循环里的 clock 读数一拍都不多。100k 连接拓扑下这一格的实测成本我没量过——**写「没量过」**，归 S5-07，别当成免费。
- **为什么这不违反 `stats.h:25`**：那条纪律管的形态是「别的线程读循环线程正在写的非原子量」。我没有换读者，我换的是**属主线程自己把值抄进一个原子槽**——抄的动作发生在唯一有权读那张注册表的线程上，采集线程读的是原子。TSan 的判据不变，这一条正是我要它在 tsan preset 下证明的东西。
- **聚合语义与退出账一致**：`/metrics` 上这三个键仍是「所有 worker 的那几份 `Epoll` 实例之和」，与 `reactor_group.cc:305-309` 同一个求和集合。两处唯一的差别是新鲜度：**运行期读数最多滞后一拍（200 ms），退出账仍是 join 之后的精确和**，脚本的历史判据一律继续走退出账，我不改它们。
- **一处必须继承的口径陷阱**：`ReactorGroup` 那个求和**不含** accept 侧的 `accept_epoll_`（`reactor_group.h:193`），指标面必须沿用这个「不含」关系。否则「每请求 `epoll_modifications` ≈4.0003」这把尺的分母分子关系会变，B-018 的历史读数当场不可比。同理指标线程自己的 `Epoll` 实例一个字节都不进这三键——这条是第三节反证 2 的同一件事。

### 进程级那一族：`fd_*` / `rss_*` / `elapsed_ms`

`fd_start`/`fd_end`/`rss_start_kib`/`rss_end_kib` 不是计数键，是**生命周期两端的两次采样**（`main.cc:348-351`），`fd_start` 在 `start_listen` 之后量、`fd_end` 在析构之前量，靠「常驻项两边都抵消」来判「连接 fd 有没有漏」（`main.cc:879-883` 那段注释）。运行期没有「end」这个位置。我的裁法在第五节的键表里，其中一处要记成对验收句 ① 的**显式例外**。

采样动作放在**采集线程**上（读 `/proc/self/fd` 与 `/proc/self/status`）：syscall 落在观测者自己身上，这正是 `--metrics-port` 独立成一条线程的理由之一——第四节标题那句「不给 worker 循环加 syscall」在物理上就是靠这个线程边界成立的。继承 `count_open_fds()` 已知的两个坑：`opendir` 的 dirfd 自己也进数（每次调用 +1，抵消得掉），以及 B-011 那条结论——**RSS 在 128 连接这个尺度不是可用仪器**，`rss_kib_now` 那一格的面板措辞必须带这句限定。

`elapsed_ms` 的**时钟原点必须与退出账同一个**：`main.cc:942` 那个 `started`。采集线程比它先起（要在 `fd_start` 之前建好监听，见下），所以原点走一个由 `main` 在 `group.run()` 之前写入的共享原子时间戳，指标服务读它。**不自带原点**，否则两个同名键的口径分叉。

### 顺序硬前置（这条不改会打断全部历史 fd 断言）

指标监听 fd **必须在 `count_open_fds()` 取 `fd_start` 之前创建好**（group 臂 `main.cc:882`，单 Reactor 臂 `main.cc:1006`），两侧就都把它算进常驻项，`fd_start == fd_end` 这条贯穿九条冒烟的生命周期判据原样成立。延迟创建（比如首次 scrape 时才 bind）会当场打破它——这是一个会被现成断言抓住的错误，属于好事，但顺序本身要在 ADR 里写死而不是等测试提醒。

---

## 五、键名与 `# TYPE`：77 个固定键原样，两处必须裁的形状

全装配时退出账发 **77 个固定键**（`report_stats` 66 个 `put()` 调用点，其中 10 个在每目标 for 循环里；`report_rate_limit` 4 个；`report_redis` 5 个；尾部 `event_batches`/`workers` 2 个）+ **每目标 10 键 × `target_count`**。未装配的段（health / nodes / redis / rate_limit）今天不出现，指标面同样不出现——**键集合由装配决定，两处必须一致**，这是验收 ① 的判据形状。

### 冲突一（必须裁）：点号地址在 Prometheus 语法里非法，而验收句写「键名不许改」

`main.cc:364-381` 把每目标键拼成 `health_t<t>_<target_names[t]>_<后缀>`，而 `target_names[t]` = `address + "_" + port`（`main.cc:763`），于是线上真实发的是 `health_t0_127.0.0.1_18081_ejections`。text format 0.0.4 的名字法则是 `[a-zA-Z_:][a-zA-Z0-9_:]*`——**点号非法**，冒号是保留分隔符。这一族要么改名，要么整个 scrape 被 Prometheus 判失败。

我按**文档原意**落，不是另起一套口径：`main.cc:355` 的注释本来就写着「键名里的地址冒号换成下划线，让 grep 不用处理两套分隔符」，`main.cc:759` 的示例也本来就写的是 `health_t0_127_0_0_1_18081_ejections`——**是注释/示例与实现之间原本就有一道缝，实现发的是点号**（`main.cc:763` 拼的是 `endpoint.address + "_" + port`，`address` 带点）。指标面取 `.` 与 `:` → `_` 的机械变换，与那条示例逐字符相同；退出账继续发它今天发的东西**一字不改**（历史脚本 grep 的是点号形态：`smoke-s4-05.sh:158`、`s3-03-health.sh:125,180,183`、`smoke-s4-06.sh:286-289`，我核对过）。

**代价照记**：变换不可逆（`a.b` 与 `a_b` 理论上会并名）、地址与端口的边界在名字里丢了、S5-03 的图例要靠正则去切。**替代方案是给每个量加 label**（`health_ejections{target="127.0.0.1:18081",index="0"}`）——那才是 Prometheus 的正统写法、图例也干净，但它把 10×N 个键并成 10 个族，属于结构重命名，直接违反「键名不许改」的字面。**我不采**。如果项目所有人认为面板值得用 label 形态，那是对验收句 ① 的一次改写，请裁；我不在 S5-01 里顺手做。

### 冲突二：`fd_end` / `rss_end_kib` 运行期没有对应物

裁法：`fd_start`、`rss_start_kib` 原样发（那是启动时的一次真值，两处同口径）；运行期另发 `fd_open_now`、`rss_kib_now` 两个**新名字**；`fd_end` / `rss_end_kib` **只在退出账发**。把当前值标成 `fd_end` 是对名字撒谎——那个名字宣称的是「析构之前」这个生命周期位置。**因此验收句 ① 的「全部既有计数键」我按「计数/状态键」判，这四键里两键记为显式例外**，例外就记在本节，不藏在实现里。

### `# TYPE` 必须诚实，因为它决定 Grafana 上 `rate()` 能不能用

`# TYPE counter` 与 `gauge` 报错了，S5-03 那六格的查询会静默出空曲线，而这正是契约要「亲眼看到」的东西。全表按量的性质分（不是按 `prometheus-cpp` 的习惯分）：

| TYPE | 键 |
|---|---|
| `counter` | 只增不减的那 40 余个：`downstream_accepted`、`upstream_opened`、`upstream_connect_failed`、三类 `upstream_*_timeout`、`requests_parsed`、`requests_rejected`、`connections_constructed`、`connections_destructed`、`connections_closed`、两个 `bytes_down/up_*`、`header_scan_bytes`、`parse_calls`、`outbound_buffers_created`、`scratches_created`、`high_water_hits`、`backpressure_pause_count`、`backpressure_resume_count`、`bytes_read_downstream`、`bytes_read_upstream`、`upgrades_101`、`bytes_tunnel_down`、`bytes_tunnel_up`、`close_headers_honored`、`gateway_replies_sent` 及三个细分、`upstream_retries`、`gateway_replies_after_retry`、两个 `upstream_truncated_*`、`responses_completed`、`pool_borrows`、`pool_returns`、两个 `pool_evictions_*`、`drain_*` 全部 8 键、`accept_errors`、`downstream_rejected_over_capacity`、`epoll_registrations`、`epoll_unregistrations`、`epoll_modifications`、`rate_limit_allowed`、`rate_limit_rejected`、`rate_limit_overflow_keys`、`rate_limit_capped_rejects`、`redis_settles_ok`、`redis_settles_failed`、`redis_settle_commands`、`nodes_reloads_ok`、`nodes_reloads_failed`、每目标的 `ejections`/`probe_ok`/`probe_fail`/`recoveries`/`half_open_admitted`/`half_open_denied`/`half_open_recovered`/`half_open_reopened`、**`trips_rate`（见下）**、`health_ejections_blocked_by_valve`、`health_all_denied_fallback`、`metrics_scrapes_ok`、`metrics_scrapes_failed`、`metrics_render_bytes`、`event_batches` |
| `gauge` | 可上可下或「当前状态/瞬时采样」：`live_connections`、`read_disarmed_connections`、`fd_open_now`、`fd_start`、`fd_end`、`rss_kib_now`、`rss_start_kib`、`rss_end_kib`、`elapsed_ms`、`workers`、`health_ejected_now`、`circuit_half_open_now`、`redis_degraded`、`rate_limit_keys`、`nodes_last_publish_epoch_ms`、每目标的 `last_ejection_epoch_ms` |
| `untyped` | **不用**。这一型是给「不知道性质」的量兜底的，我这里每一个都知道 |

一处命名陷阱必须钉住，因为它是**我核对时才发现的**：`trips_rate` 这个名字读起来像比率，而它的底层是 `TargetHealthSnapshot::rate_trips_total`——`health.h:119` 原文「被**错误率窗口**摘掉的次数」，`health.cc:115` 每次摘除 `fetch_add(1)`。**它是 counter，不是比率**，`main.cc:376` 发出去的也是这个计数。指标面同值同口径（TYPE 记 `counter`），并给这一键写一条 `# HELP` 明说「这是次数不是比率；要比率请用 `rate()` 或对 `ejections` 求比」——**我不为了名字好看去改键名**（那是验收句 ① 禁止的事），也不为了 TYPE 省事把它谎报成 gauge。

`# HELP` 只给推不出语义的那十几键（`read_disarmed_connections`、`epoll_modifications`、`rate_limit_capped_rejects`、`upstream_retries`、`gateway_replies_after_retry`、`health_ejected_now`、`circuit_half_open_now`、`drain_read_disarmed_at_entry`、`rss_kib_now` 等），不追求全覆盖。**其中 `health_ejected_now` 那一条 HELP 必须带上 1.31 边界⑥ 的限定**——「`health_ejected_now=0` 不能读成『没有坏后端』」（已裁⑧ 量出的那条新边界；ROADMAP 债表第一行）。这条限定语在文本里出现一次，S5-03 的面板与 README 就有出处可指，不是各写各的。

**Prometheus 命名惯例的偏离照记**：惯例要求 counter 名带 `_total` 后缀，我这 77 个键没有一个带。这是「键名不许改」的直接代价，不是疏漏。

---

## 六、暴露面本身怎么跑

- **一条独立线程**，自己的 `Epoll` 实例，自己的 `eventfd` 停止通道——照 `ReactorGroup::start_listen` 的形状（`reactor_group.cc:44-…`：先 bind、学到端口、之后才起线程；`reactor_group.h:132-133` 明写 `start_listen` 故意不 spawn 线程，正是为了这个顺序）。套接字复用 `net::make_listen_socket`（`socket.h:64`，内含 `SO_REUSEADDR`，`socket.cc:114`）与 `net::get_socket_port`（`socket.h:59`，`--metrics-port 0` 时回读内核给的端口，测试靠它）。**不碰 `Connection`/`Buffer`/filter 链**（第三节 1）。
- 每连接三拍：非阻塞读到请求头结束符（至多 4 KiB，与探活那组固定缓冲同族）→ 渲染并写出 → 关闭。`GET /metrics` 之外一律 400，非法请求行 400，超限 431。**响应带 `Connection: close`**：不实现 keep-alive。抓取节拍是 15 s 量级，一条连接一次响应，代价是每 scrape 多一个 TCP 握手，收益是仪器里没有会话状态机可写——**观测面的代码量本身就是风险面**。
- 渲染**在 scrape 时现做**，不做预渲染缓冲、不加第二条定时线程：文本就是「键数 × 两三行」的量级（全装配 + 两台上游约两百多行、几 KB），成本落在采集线程，不落在 worker。文本全部是整数值，没有 `NaN`/`Inf` 通路（`0.0.4` 的语法里它们是合法 token，写进去就是把口径问题留给读者）。
- **停止时序：先停采集线程，再打退出账**，与 `main.cc:947-949` 那条「结算线程先停再打账，否则最后一拍与业务账互相插队」同一根理由。`workers` 那个键的语义不变：它数的仍是业务 worker，指标线程不是 worker，不进这一格。

---

## 七、判据形状：五条验收各自怎么写成断言

写不成断言的句子在 TASKS 表里不算验收。逐条给我的落点：

| 验收句 | 断言落点 | 阳性验证（能红才叫绿） |
|---|---|---|
| ① 抓一次 `/metrics` 得到 `# TYPE` 行与全部既有计数键，同名同口径 | ctest `S5_01_MetricsCatalog`：把渲染出的键名集合与一份**手写的期望清单**（77 + 10×N，含装配开关的四种组合）逐数比对；每个键上方有对应 `# TYPE` 行；`# TYPE` 行数 = 族数 | 往清单里加一个不存在的键、或把某个 `put()` 的键名改一个字母 → 用例红。手写清单的作用就是「改名不可能静默通过」 |
| ② 一条业务请求都不许多计 | ctest `S5_01_MetricsScrapeDoesNotCount`：`--metrics-port 0` 起组，业务端口一条请求都不打，scrape 5 次 → 断 `requests_parsed == 0`、`responses_completed == 0`、`connections_constructed == 0`、`live_connections == 0`；再放 1 条真业务请求 → 前两键各自刚好 = 1；再 scrape 5 次 → 仍是 1。**下界同拍断**：每次 scrape 必须拿到 200 + 非空 body + body 里有 `# TYPE`（否则「一动不动」可能只是因为根本没被服务） | 把 metrics 监听的 fd 交给 `Reactor::handoff` 走业务路径 → 前两键随 scrape 增长，用例当场红（我按 S3-03 那五条真打红的同一套做法做） |
| ③ 开与不开指标面，每请求 `epoll_modifications` 与 p50/QPS 无可分辨 | 新脚本 `scripts/bench/s5-01-metrics-cost.sh`：同机交替 ≥2 轮（取样规程），两臂只差 `--metrics-port`，参数与 B-013/B-016/B-018 逐字同形（workers=1 / 100 并发闭环）。**主尺是每请求 `epoll_modifications` 与 syscall 面**（已裁⑨第 5 项），p50/QPS 只报跨臂差 vs 组内极差 | 第三臂：把快照改成每事件写（`store` 挪进 `handle_events`）→ 每请求 `epoll_modifications` 不变但 p50 出带，证明这条臂**有分辨力**；量不出分辨力的臂不算证据 |
| ④ 汇聚点不许给 worker 循环加 syscall | 结构 + 读数两半：结构上 `git diff` 里 `handle_events`/`apply_events`/`arm_write` 三个函数一字未动，写入点只在已有那一拍；读数上 ③ 的三键（`epoll_registrations`/`_unregistrations`/`_modifications`）两臂**逐数相等**，不是「近似」。再加一条 ctest：**`--metrics-port` 缺席时四个槽恒为 0**（循环跑完 N 条请求后读槽），这就是「循环根本没往里写」的正面证据 | 把关闭态的 `nullptr` 判空去掉（无条件写槽）→ 那条 ctest 红；把 `accept_epoll_` 也计入求和 → ③ 的逐数相等红 |
| ⑤ 端口形态按裁定实现、默认关闭、关掉时代码路径与今天逐字节相同 | 冒烟脚本 `scripts/smoke-s5-01.sh` 阶段 1：不给 `--metrics-port` → 退出账 `[serve]` 行的**键名集合**与一份 golden 清单逐行相同（golden 从当前 HEAD 的一次实测日志生成并入库）、`fd_start == fd_end`、连接 `metrics_*` 三键不存在；阶段 2：给了 → `fd_start == fd_end` 仍成立（第四节那条硬前置的实测落点）、`metrics_scrapes_ok > 0` | 让指标线程在 `count_open_fds()` 之后创建监听 fd → 阶段 2 的 `fd_start == fd_end` 当场红；给 `metrics_*` 无条件出现在退出账 → 阶段 1 的键集合 diff 红 |

上界都配了下界：③ 的「无可分辨」不单独成立，同一批必须报组内极差与本档分辨率（照 B-018 那句限定的形状写死：「本档分辨率 ≈128 µs/请求，比 B-008 的 8.7 µs 大一个数量级以上，所以这句读不出『指标面免费』」）；② 的「计数不动」必须配「scrape 确实成功过」；⑤ 的「关掉时相同」必须配「开着时确实多一个监听 fd 且仍能收」。

三 preset 全绿 + tsan 零 data race 是本任务的硬前置（新增并发面就是一条线程跨线程读那些原子槽），S1-08 那次同类改动的 TSan 红是现成的反面教材。

---

## 八、不在本任务做（契约 8.2 禁令一，防范围扩张）

直方图与分位数（`# TYPE histogram`、`summary`）——分位由客户端自己算，SLO 那条线从 S2-06/ADR-17 起就是这么跑的，已裁⑨第 5 项再把 p99 降成带外报警器；`prometheus-cpp`；把指标写进 `--config`（部署事实，第二节）；鉴权 / TLS / `/healthz` / 推送网关 / OpenMetrics / exemplars；`process_*` 那族进程指标（那是 node_exporter 的活）；响应侧 `on_response` 钩子（TASKS「## S5」段候补第一条明写「先取证再立项」，本节就是取证的地方——**我不动链的形状**）；给 `least_conn` 那两臂重跑（B-013 修前读数那条债归 S5-07，不在这里顺手做）。

## 九、代价汇总（照记，不是免责声明）

1. 文本格式正确性没有第三方兜底，全靠第七节 ① 那组单测。
2. 运行期读数最多滞后一拍（200 ms），退出账才是精确和；两处名字与语义相同、新鲜度不同，读者要分清。
3. `read_disarmed_connections` 那一格在 100k 拓扑下的遍历成本**我没量**，只把它降到每秒至多一次并写明归 S5-07。
4. 仪器改动被测方一处：`RateLimiter` 的聚合原子（落在 429 支路），成本由 ③ 的第三臂说，不由我说。
5. Prometheus `_total` 命名惯例全表偏离，换来「键名不许改」字面成立。
6. `--listen 0.0.0.0` 的部署里 `/metrics` 对外可读，本仓库不做鉴权。
7. 多一条常驻线程与一个常驻 fd（开启态）。

## 十、落盘顺序与本 ADR 之后要同步的文档

顺序按契约 8.2 第一条：本 ADR 落盘 → `TASKS.md` S5-01 置 RUNNING（记认领）→ 才动 `src/`。实现完成后按契约第 7 节矩阵同步：`ARCHITECTURE.md` 新增一节（运行期观测面的形状与那两个例外键）、`README.md` 的运行期观测面段与新键表、`ENVIRONMENT.md` 的端口占用与 `--metrics-port` 事实、`SLO.md` 只在成本那一格加引用、`BENCHMARK.md` 新增 B-019（③ 那两臂）。镜像 tag 与 compose 的锁 tag 纪律属 S5-05，本节一个镜像都不碰。
