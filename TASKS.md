# TASKS —— 唯一真相源

> 状态机：`PENDING` → `RUNNING` → `DONE`（或 `BLOCKED`，必须写明卡在什么）。一次会话只推进一个任务，认领即置 `RUNNING`。
> **验收标准必须是可断言的句子**，写不成断言的就不是验收标准。
> 不在本表里的功能，一律不实现（契约 8.2 禁令一）。

## 当前进度

阶段 **S1**，**S1-01～S1-04 已完成**，**S1-05（keep-alive 复用与 Upgrade 透传）待认领**。开工前置条件已满足：`clang 21.1.8` + `libclang-rt-21-dev`、`wrk 4.1.0`、`hey 0.1.4`、`curl 8.18.0` 就位，三个 sanitizer 我用故意写坏的样本做过阳性验证（ASan 报 UAF、LSan 报 5 处泄漏、UBSan 报 signed overflow、TSan 报 data race），证据在 `docs/ENVIRONMENT.md` 第 2.2 节。

**S1-04 认领时的四处核对**（开工前对契约/TASKS/SLO 做交叉一致性检查的结果，逐条记状态，改了的和没改的都写在这里）：

- **已改（一处）**：原验收句「连接对象在关闭后**由 buffer 引用计数**释放」写不成断言——`Buffer` 是连接对象的成员，它不持有对连接对象的引用，能归零的是连接对象自身的所有权计数。已按上表改写。所有权模型随之定死：`shared_ptr<Connection>` 由 reactor 的注册表持有，关闭时先摘 epoll、进 pending-destroy 列表、**在本轮事件批处理完之后**才析构；这一步不是为了优雅，是因为 ET 下一次 `epoll_wait` 可以返回同一批里的多个事件，fd 在批内被 close 后立刻可能被新连接复用，先析构就会让批内后续那条事件拿着悬垂指针操作一个不属于它的 fd。
- **未改、记为卡点二（本任务已按分工落地）**：「`curl` 打 10000 个请求」留在验收列按原文执行，但它进不了 ctest——10000 次 curl 是 10000 次进程 fork 加 connect，而 S1-03 给每条用例设的 `TIMEOUT 30`（依据是全套 asan-ubsan 实测 <3s）会把它判失败。分工：ctest 内的 10000 请求用仓库内客户端（单 keep-alive 连接顺打，debug 2.49s），字面意义的 curl 10000 次由 `scripts/smoke-s1-04.sh` 跑并把数字落进本行——**已落，见 S1-04 行「字面意义的 curl 10000」那一段**。这里必须记一个坑：脚本第一版写的是 `"http://127.0.0.1:$PORT/r{1..10000}"`，而 `{1..N}` 是 bash 的 brace expansion，curl 的 glob 只认 `[1-N]` 与 `{a,b,c}` 这类**集合**，于是它把 `{1..10000}` 原样当路径吞掉，**只发了 1 个请求，退出码 0、耗时 0.02s**——三个"看起来成功"的信号同时成立。抓到它的唯一原因是那条 `requests_parsed == 10000` 断言。所以断言进脚本，不进我的印象。
- **未改、记为卡点三**：门禁四件套第 1 条要 S1 交「QPS（第一份）」，而 `docs/SLO.md` 表里 #3 的首次可测阶段是 S2、达标判定定义成与 nginx 同环境比值 ≥60%（nginx 未安装、Docker WSL Integration 未开）。S1 门禁收尾时那一格只能记成**不设达标判定的自测 QPS**，`SLO.md` 的目标与阶段都没动；这个措辞冲突留给阶段门禁时由用户裁决。
- **未改、记为卡点四（本任务只交实际分配额，歧义原样保留）**：SLO #2 的分母「连接数」在代理拓扑下有歧义——一个客户端连接配一个上游连接是两个连接对象，而每连接 20 KB 的判定要到 S2/S3 才做。本任务不改 SLO 口径，只把每连接实际分配额实测出来写进数字列：`sizeof(Connection)=400`，debug 下 128 条空闲下游连接摊出 **4.47–5.72 KiB/连接**（本任务四次运行，按区间记），出向 Buffer 与 scratch 在纯空闲场景一个都没建。**这个数不含歧义的两种读法中任何一种的裁决**——它测的是"每条连接对象"，而一个客户端连接现在会占两个连接对象（只是回源那条的出向/入向都按需才建）。留到 S2 建池时一并由我请示。
- **边界（已确认，并已按此执行）**：S1-04 建事件循环 + 上下游两个连接对象 + 双向 pump，验收只打生命周期与 fd/epoll 计数；响应的转发正确性（逐字节一致、chunked、上游断开的 5xx、第一份 p99）全部留在 S1-06。
- **分配策略（已确认，已落地）**：出向 `Buffer` 惰性创建，`ParseScratch` 懒建且可在隧道化后释放（释放路径本任务落地，真正的触发点在 S1-05 的 Upgrade）。理由就是卡点四那笔账：空闲长连接不该为它不写的方向预支 4 KiB。实测：128 条空闲连接场景 `outbound_buffers_created=0`、`scratches_created=0`（这条被写成断言，不只是注释）。**踩到的坑记一笔**：第一版的观测 getter 写成了惰性创建版本（`Buffer* outbound()` / `ParseScratch* scratch()`），于是 `EXPECT_EQ(conn.scratch(), nullptr)` **自己把 scratch 建了出来**、断言随即失败——"检查有没有"这个动作改变了答案。修的是接口（拆成 `has_*()` 观测版 + 私有 `*_or_create()`），不是那行测试。

**S1-04 收尾新增的一条自定规矩（卡点五，不改契约也不改 SLO，只改我自己的测试纪律）**：这批用例最初靠轮询原子计数来等状态，我改成了**先拿内核级证据、再等收尾计数**——`shutdown(SHUT_WR)` → `read()` 返回 0（对端真的关了，这是内核的记录）→ 才等 `live_connections` 归零 → 才读非原子的 fd/epoll 观测。原因是轮询计数当同步条件有两类假信号，两类我都撞到了：计数到 0 但其实还没关（断言过早通过），以及改完之后有两条断言从"看着通过"变成"合理失败"——它们原本靠的就是那次抢跑。这条规矩的边界写在 `src/net/stats.h`：**生产逻辑里绝不允许**拿观测计数当同步条件，测试里等计数只在"同一批事件处理里紧跟内核级观测之后的那几步收尾"这一种情况下合法；非原子观测（`live_connection_count()`、`Epoll` 的计数器）不在这条规则内，只能在循环线程 join 之后读。

顺带两处必须记的执行细节：`/proc/self/fd` 在测试里看到的是**整个进程**，客户端与网关同进程时一条连接占两个 fd（我第一次漏了客户端那 128 个），且 `opendir` 的 dirfd 自己也进数（每次调用都 +1，抵消得掉）；curl 的 URL glob 用 `[1-N]` 与 `{a,b,c}`，`{1..N}` 是 bash 的写法、curl 会把它当普通字符吞掉。

仍未解决、但不阻塞 S1 的两项：`llvm-21-tools`（ASan 堆栈符号化，非契约锁定项）、Docker WSL Integration（S3 之前必须开）。

**一处流程偏离，记账（S1-04 收尾时更新过）**：契约 8.3 要求「一次会话只推进一个任务」。S1-01 提交（`b911321`）之后、S1-02 置 RUNNING 的同一会话里发生了上下文压缩，压缩后我继续把 S1-02 推完了，所以该次会话实际覆盖两个任务。我没有为此回退已完成的代码，但把它写成偏离而不是当作没发生。S1-02 收尾后我按这条规矩停住、请示是否新会话再开 S1-03，得到的指示是「接着推」——所以 **S1-03 与本会话前半段的 S1-01/S1-02 同批推进，这是显式授权的结果而不是我又越界一次**。S1-04 同样在这条授权覆盖的会话里推进到完成，并且**中途又发生了一次上下文压缩**：压缩后我从"reactor 测试全红之前的最后五条失败"接着推到了文档同步与提交。这条偏离我按原样记着，不当作没发生；它带来一个实际后果我要写明白——压缩摘要里的数字可能是旧的，所以本次收尾**所有进文档的数字都是重跑一遍取的**（三 preset 各 60/60、冒烟 10000 请求、每连接内存三个 preset 各一次），没有一个是抄摘要的。

## 交接 —— 新会话从这里接

> 这一段是契约第 6 节阅读顺序的第 1 步补充：读完 CLAUDE.md 和本节，你就知道"现在在哪、下一步做什么、什么不许动"。第 2–6 步照契约原文走，**不要跳过 ADR**。

**仓库状态**（S1-04 收尾后）：**S1-04 的全部代码与文档在提交 `9bca50a`**，之后的提交只可能是文档同步；工作树干净，**未 push**（契约 8.3：push 只在阶段门禁收尾时执行）。四个 preset 全部构建通过、我方警告 0 条；`debug` / `asan-ubsan` / `tsan` 各 **60/60**。

**一条命令拿到今天的门禁证据**（必须在 WSL 原生文件系统内跑，不能在 `/mnt/d`）：

```bash
cd /home/archer/l7gate
bash scripts/verify-all-presets.sh                       # 三 preset 构建 + ctest + 警告计数
PRESET=debug ./scripts/smoke-s1-04.sh                    # 真 curl 10000：fd/存活连接/字节数全绿
./build/debug/l7gate --self-check                        # 编译期事实单行，压测报告直接 grep
```

**下一个任务：S1-05（keep-alive 复用与 Upgrade 透传）**，状态 PENDING，认领即置 RUNNING。开工前必读三处：本表 S1-05 行的验收标准；`docs/ARCHITECTURE.md` 第 3 节第三条——`Connection::release_scratch()` **已经落地且有作废断言，但没有任何调用点**，本任务负责给出它的触发点（101 之后），并且**不许**顺手写成"请求结束就释放"（那样 keep-alive 下一条请求会重新分配那 2048 字节，等于白省）；第 1.6 节第四条——观测 getter 一律不分配，惰性创建只允许发生在真要用的路径上。

**本轮授权范围（2026-10-26 明确）**：一路推进到 **S1 门禁四件套**齐活并完成该阶段的首次 `git push`，中途不再逐个任务请示。契约 8.3「一次会话只推进一个任务」在这段授权覆盖期内被显式放宽，我在收尾时按下方「流程偏离」继续记账，不当作没发生。**授权不覆盖下列三件事**——它们必须停下来由我请示：(a) 任何 SLO 目标值或测量口径的改动；(b) 任何新依赖或版本变更（先 ADR）；(c) 卡点三与卡点四那两条口径冲突的裁决，留到 S1 门禁收尾时一并定。

**仍然挂起、不阻塞 S1-05 的三项**：`llvm-21-tools`（ASan 堆栈符号化，非契约锁定项）；Docker WSL Integration（S3 之前必须开，否则 Redis/MySQL/Prometheus/Grafana/Nginx 基线全不可用）；WSL `ulimit -n` = 10240（S2 的 100k 场景之前必须解决）。

**接手纪律，两条是我自己踩过才写下来的**：一，上下文压缩之后**摘要里的数字一律不抄进文档**，重跑一遍取——本任务收尾时就是这样做的（三 preset 全量、冒烟、每连接内存各重跑）；二，从 Windows 侧（UNC 路径）编辑过的脚本会**丢掉执行位**，跑脚本前 `chmod +x`，否则 `timeout` 会报 `Permission denied` 那种指向错误地方的错。

## S1 —— 单连接转发


**本阶段要解决的真问题**：上下游速度不匹配导致缓冲区无限堆积（背压雏形）。这是 S1 的门禁交付物之一，不是事后补的 bug——所以 S1-07 必须在 S1 内完成，不许推到 S2。

| ID | 任务 | 验收标准（可断言） | 状态 | 实测数字 |
|---|---|---|---|---|
| S1-01 | 构建骨架：CMake + GoogleTest v1.14.0（FetchContent）+ 三种 sanitizer preset | `cmake -B build -S .` 与 `cmake --build build` 在 WSL 原生文件系统下退出码为 0；`-Sanitize=address,undefined` 与 `-Sanitize=thread` 两个 preset 各自能配置并构建出可执行；`ctest` 跑通至少 1 个空壳单测；GoogleTest 拉取的 tag 在构建日志里可断言等于 `v1.14.0` | **DONE** | 三 preset 全绿：debug 6/6（15s）、asan-ubsan 6/6（20s）、tsan 6/6（20s）；我方代码警告 0 条；GoogleTest 1.14.0 经源码解析断言通过 |
| S1-02 | 动态缓冲 `Buffer`：可读写、连续、带高水位 | 对 10000 次随机「写 n 字节 / 读 m 字节」序列，读回的字节流与写入序列完全一致且无越界；ASan/UBSan 全程零报告；可断言「一次 `read()` 直到 `EAGAIN` 才认为本轮可读事件处理完毕」这条 ET 契约在 buffer 层成立 | **DONE** | 三 preset 全绿：debug 15/15（0.28s）、asan-ubsan 15/15（0.84s）、tsan 15/15（1.07s），我方代码警告 0 条。完整性（上限放宽到 64 MiB 的专用配置）：10000 次随机操作 written=9,049,376 / read=5,048,397 / peak_capacity=4,194,304 / 逐字节一致。单连接峰值内存（**默认配置**）：1 KiB keep-alive × 1000 轮 → initial=4096、**peak_capacity=4096**、final=4096，每轮 `read_syscalls > 1`（1000/1000，证明每轮都以 EAGAIN 收尾） |
| S1-03 | HTTP/1.1 请求解析器（纯函数，不碰 fd） | 合法样本解析出的 method / target / version / headers / body 与逐字段断言相等；`Content-Length` 与 `chunked` 两种 body 都覆盖；对 12 类畸形输入（非法 chunk-size、header 无空行结尾、换行不是 CRLF、超长头、请求行缺字段等）解析器返回错误码而非崩溃、不死循环 | **DONE** | 三 preset 各 **36/36**（debug 0.74s / asan-ubsan 2.61s / tsan 3.47s），我方警告 0 条；畸形输入用例 **37 类**（契约要求 ≥12），全部返回定义好的错误码，ASan/UBSan 对 40000 例随机字节（含 NUL、裸 CR/LF、控制符）0 报告。「不死循环」靠 ctest `TIMEOUT 30` 变成门禁，不是靠我口头断言。**解析吞吐（release `-O3`，108 字节请求，单线程无 syscall）= 595.0 MB/s / 5,509,658 req/s**（同一用例 debug `-O0` 是 88.1 MB/s，所以数字必须连构建类型一起记）。纯函数的代价也测了：3867 字节头部被逐字节 dribble 时重扫 7,474,911 字节、耗时 9.0 ms，而单次完整解析 4.6 µs（**1956×**）—— 缓解（连接对象记已扫描游标）归 S1-04。每连接 scratch = `sizeof(HeaderEntry)` 32 × 64 = **2048 字节**，在 Buffer 的 4096 之外另算 |
| S1-04 | 单 Reactor：epoll ET 事件循环 + 上下游连接对象 | `curl` 打 10000 个请求（由 `scripts/smoke-s1-04.sh` 执行，见下方卡点二），事件循环存活、fd 数不随请求数单调增长（无泄漏）；**连接对象在关闭后引用计数归零并析构——10000 请求后存活连接数 `== 0`，ASan/LSan 报告零泄漏**；epoll 注册/注销次数与连接建立/关闭次数逐项相等可断言 | **DONE** | **三 preset 各 60/60**（最后一次全量验证：debug 4.08s / asan-ubsan 7.53s / tsan 8.44s），我方警告 **0 条**（新增 24 条用例：3 buffer 写出侧 + 11 连接 + 10 事件循环）。**字面意义的 curl 10000（卡点二那条分工的另一半，`scripts/smoke-s1-04.sh`，curl 8.18.0、debug 构建）：3.00 s 完成，网关 `elapsed_ms=3067`、`requests_parsed=10000`、`requests_rejected=0`、`downstream_accepted=1`、`upstream_opened=1`（10000 请求只回源一条连接）、上游侧自报服务数 10000、`live_connections=0`、`connections_constructed=connections_destructed=2`、`fd_start=fd_end=9`、外部采样 fd 基线 8 → 峰值 10（涨幅 2 ≤ 8）、RSS 4136→4360 KiB。另一次运行是 3.05 s / `elapsed_ms=3118` / RSS 4140→4364，差异在噪声量级，两次都全绿。** 阶段一另有一条 `curl -sS -i` 断言：`HTTP/1.1 200 OK` 回来且响应体恰好 64 字节。epoll 计数逐项对照：`registrations=4`（监听+eventfd+1下游+1上游）＝常驻 2 + `downstream_accepted(1)` + `upstream_opened(1)`，`unregistrations=2`＝`connections_closed=2`，`modifications=40000`（每请求挂/摘 EPOLLOUT 各一次）；注册/注销计数直接取自 `Epoll` 自己的计数器，不是我手工 `++` 的第二份。**S1-03 两笔债的还款数字：`header_scan_bytes=838,894` 与 `bytes_down_to_upstream=838,894` 完全相等（每字节只扫一次；无游标的 dribble 场景原本是 7,474,911）**，`parse_calls=10000`。每连接内存（**口径，不是 SLO #2 判定**）：`sizeof(Connection)=400`；128 条空闲下游连接 debug 下 **4.47–5.72 KiB/连接**（最后一次 4.62，RSS 5636→6228；本任务四次运行 4.47/4.62/5.66/5.72，抖动是 glibc malloc 取整，按区间记不报单点）；`outbound_buffers_created`/`scratches_created` 在纯空闲场景为 0（写成断言，不只是注释），惰性分配生效。sanitizer 结论：ASan/UBSan 60/60 零报告、**LSan 零泄漏（且做过反向对照：故意留一条不释放的连接 → `detected memory leaks` + 退出码 1，删掉 → 退出码 0）**；TSan 60/60 零 data race。`TIMEOUT 30` 余量：最慢单条 3.45s ≈ 8.7×。ctest 内 10000 请求（仓库内客户端、单 keep-alive 连接顺打）debug 2.49s / asan-ubsan 3.45s / tsan 3.31s。**sanitizer 下每连接 RSS 分别 11.31（asan）/ 20.81（tsan）KiB，tsan 那个数超过 20 KiB —— 那是插桩开销，预算判定按 `k_rss_is_instrumented` 只在 debug/release 做，见 ARCHITECTURE 1.10** |
| S1-05 | keep-alive 复用与 Upgrade 透传 | 同一 curl 连接发 3 个顺序请求，全部成功且各自响应内容正确对应；请求间不重建下游连接（fd 计数器可断言）；对 `Connection: keep-alive` 与 `Connection: close` 分别按语义处理；带 `Upgrade: websocket` 的请求在 101 之后走纯透传路径，单测断言帧内容不被解析、不被改写（ADR-09） | PENDING | —（前置：S1-04 已落地 `Connection::release_scratch()` 并有作废断言，本任务负责给出它的**触发点**——101 之后。故意不留"请求结束就释放"的自动路径，那样 keep-alive 的下一条请求会重新分配，2048 字节白省） |
| S1-06 | 回源转发 + 响应回写闭环 | `curl -v` 经网关请求真实上游，状态码、`Content-Length`、body 与直连上游逐字节一致；上游返回 chunked 时转发结果与直连一致；上游中途断开时网关返回可定义的 5xx 且不残留半包 | PENDING | **第一份 p99：—** |
| S1-07 | 背压雏形：缓冲高水位与 fd 暂停读 | 人为制造上游比下游慢（慢读上游）时，网关缓冲增长到配置的高水位即停止；停止后能观测到网关不再消费下游可读事件（epoll 侧可断言），且内存不继续增长；水位回落后自动恢复读取 | PENDING | 高水位触发时 RSS：— |
| S1-08 | filter 链骨架（空壳顺序，无业务 filter） | 一条空 filter 链与一条含 2 个标记 filter 的链，执行顺序与配置顺序一致且可被单测断言；新增 filter 不修改转发主路径的任何一行代码；filter 接口签名允许后续放置限流/熔断/路由（S3/S4 复用，不需返工） | PENDING | — |

**S1 门禁四件套**（缺一不进 S2）：

1. 三个数字快照 → `docs/BENCHMARK.md`：p99（实测）、QPS（第一份）、每连接内存（单连接场景下先记录口径，100k 场景留到 S2/S3 实做）
2. ASan / UBSan 干净（+ S1-05 Upgrade 透传验证结果）→ `docs/FAULT-INJECTION.md`
3. 文档按契约第 7 节矩阵同步（`ARCHITECTURE.md`、`README.md` 架构段）
4. 提交记录：一个任务一次提交，提交信息带当次自测数字

## S2–S5 —— 进入阶段时再拆

按契约 8.2，功能必须在 TASKS.md 里才允许实现，但**过细的提前拆解会写成幻想中的任务表**。我在每个阶段门禁通过时才拆下一阶段，因为 S2 之后的任务内容取决于 S1 实测暴露的缺陷——这正是契约第 4 节设计原则里「被逼出来的问题」的含义。

| 阶段 | 一句话目标 | 门禁硬条件 | 拆解状态 |
|---|---|---|---|
| S2 | 多 Reactor + 上游连接池 + 连接对象跨线程传递与异步销毁 | **TSan 干净**（这也是必须先装 Clang 的原因） | 未拆 |
| S3 | 路由（前缀/正则）+ 负载均衡（轮询/最少连接/一致性哈希）+ 主动与被动健康检查 + 最多摘除比例安全阀 | 摘除耗时实测、误摘率实测、故障注入结果 | 未拆 |
| S4 | 限流（本地令牌桶 + Redis 分布式计数与降级）、熔断（三态机 + 半开）、超时与重试防放大 | 429 真实生效数据、熔断恢复时间线、Redis 降级验证 | 未拆 |
| S5 | 双 buffer 配置热重载（引用计数延迟释放）、Prometheus 指标、Grafana 面板、vs Nginx 压测与差值归因 | 三个数字终稿、vs Nginx 归因报告、故障注入全表 | 未拆 |

## 已知待决事项（不在 S1 范围，记账防遗忘）

| 项 | 卡在哪 | 归属 |
|---|---|---|
| Docker Desktop 对 Ubuntu 的 WSL Integration 未开启 | 需在 Docker Desktop GUI 操作；不开则 Redis/MySQL/Prometheus/Grafana/Nginx 基线全部不可用 | S3 之前 |
| WSL 里 `ulimit -n` = 10240 | S1-04 的用例最多同时 265 个 fd，撞不到；S2 的 100k 长连接场景必须先解决 rlimit，否则第一条实测数字会是 `accept_errors` 而不是每连接内存 | S2 开工前 |
| `perf 7.0.14` 与内核 `6.18.40.1-microsoft-standard-WSL2` 版本不匹配，能否真采样未验证 | S5 的差值归因依赖火焰图，必须提前用最小冒烟验证 | S4 期间 |
| libmysqlclient 8.4 客户端 ↔ `mysql:8.0.36` 服务端兼容性 | 目前只是推断，需 S3 首次连通实测取证 | S3 |
