# TASKS —— 唯一真相源

> 状态机：`PENDING` → `RUNNING` → `DONE`（或 `BLOCKED`，必须写明卡在什么）。一次会话只推进一个任务，认领即置 `RUNNING`。
> **验收标准必须是可断言的句子**，写不成断言的就不是验收标准。
> 不在本表里的功能，一律不实现（契约 8.2 禁令一）。

## 当前进度

阶段 **S1**，**S1-01、S1-02 已完成**，下一个认领 **S1-03（HTTP/1.1 解析器）**。开工前置条件已满足：`clang 21.1.8` + `libclang-rt-21-dev`、`wrk 4.1.0`、`hey 0.1.4` 就位，三个 sanitizer 我用故意写坏的样本做过阳性验证（ASan 报 UAF、LSan 报 5 处泄漏、UBSan 报 signed overflow、TSan 报 data race），证据在 `docs/ENVIRONMENT.md` 第 2.2 节。

仍未解决、但不阻塞 S1 的两项：`llvm-21-tools`（ASan 堆栈符号化，非契约锁定项）、Docker WSL Integration（S3 之前必须开）。

**一处流程偏离，记账**：契约 8.3 要求「一次会话只推进一个任务」。S1-01 提交（`b911321`）之后、S1-02 置 RUNNING 的同一会话里发生了上下文压缩，压缩后我继续把 S1-02 推完了，所以本次会话实际覆盖两个任务。我没有为此回退已完成的代码，但把它写成偏离而不是当作没发生；从 S1-03 起恢复一个会话一个任务。

## S1 —— 单连接转发

**本阶段要解决的真问题**：上下游速度不匹配导致缓冲区无限堆积（背压雏形）。这是 S1 的门禁交付物之一，不是事后补的 bug——所以 S1-07 必须在 S1 内完成，不许推到 S2。

| ID | 任务 | 验收标准（可断言） | 状态 | 实测数字 |
|---|---|---|---|---|
| S1-01 | 构建骨架：CMake + GoogleTest v1.14.0（FetchContent）+ 三种 sanitizer preset | `cmake -B build -S .` 与 `cmake --build build` 在 WSL 原生文件系统下退出码为 0；`-Sanitize=address,undefined` 与 `-Sanitize=thread` 两个 preset 各自能配置并构建出可执行；`ctest` 跑通至少 1 个空壳单测；GoogleTest 拉取的 tag 在构建日志里可断言等于 `v1.14.0` | **DONE** | 三 preset 全绿：debug 6/6（15s）、asan-ubsan 6/6（20s）、tsan 6/6（20s）；我方代码警告 0 条；GoogleTest 1.14.0 经源码解析断言通过 |
| S1-02 | 动态缓冲 `Buffer`：可读写、连续、带高水位 | 对 10000 次随机「写 n 字节 / 读 m 字节」序列，读回的字节流与写入序列完全一致且无越界；ASan/UBSan 全程零报告；可断言「一次 `read()` 直到 `EAGAIN` 才认为本轮可读事件处理完毕」这条 ET 契约在 buffer 层成立 | **DONE** | 三 preset 全绿：debug 15/15（0.28s）、asan-ubsan 15/15（0.84s）、tsan 15/15（1.07s），我方代码警告 0 条。完整性（上限放宽到 64 MiB 的专用配置）：10000 次随机操作 written=9,049,376 / read=5,048,397 / peak_capacity=4,194,304 / 逐字节一致。单连接峰值内存（**默认配置**）：1 KiB keep-alive × 1000 轮 → initial=4096、**peak_capacity=4096**、final=4096，每轮 `read_syscalls > 1`（1000/1000，证明每轮都以 EAGAIN 收尾） |
| S1-03 | HTTP/1.1 请求解析器（纯函数，不碰 fd） | 合法样本解析出的 method / target / version / headers / body 与逐字段断言相等；`Content-Length` 与 `chunked` 两种 body 都覆盖；对 12 类畸形输入（非法 chunk-size、header 无空行结尾、换行不是 CRLF、超长头、请求行缺字段等）解析器返回错误码而非崩溃、不死循环 | PENDING | 解析吞吐（用于归因，非 SLO）：— |
| S1-04 | 单 Reactor：epoll ET 事件循环 + 上下游连接对象 | `curl` 打 10000 个请求，事件循环存活、fd 数不随请求数单调增长（无泄漏）；连接对象在关闭后由 buffer 引用计数释放，ASan 在 10000 请求后报告零泄漏； epoll 注册/注销次数与连接数一致可断言 | PENDING | — |
| S1-05 | keep-alive 复用与 Upgrade 透传 | 同一 curl 连接发 3 个顺序请求，全部成功且各自响应内容正确对应；请求间不重建下游连接（fd 计数器可断言）；对 `Connection: keep-alive` 与 `Connection: close` 分别按语义处理；带 `Upgrade: websocket` 的请求在 101 之后走纯透传路径，单测断言帧内容不被解析、不被改写（ADR-09） | PENDING | — |
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
| `perf 7.0.14` 与内核 `6.18.40.1-microsoft-standard-WSL2` 版本不匹配，能否真采样未验证 | S5 的差值归因依赖火焰图，必须提前用最小冒烟验证 | S4 期间 |
| libmysqlclient 8.4 客户端 ↔ `mysql:8.0.36` 服务端兼容性 | 目前只是推断，需 S3 首次连通实测取证 | S3 |
