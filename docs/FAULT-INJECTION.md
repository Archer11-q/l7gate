# FAULT-INJECTION —— 预期 vs 实测

这份文件只记一件事：**当对端 behaving 坏掉时，网关做了什么，和我说的是不是一回事**。格式固定成三列——预期、实测、结论；未达标照记（契约第 7 节对这份文件的完成判据原文就是这五个字）。

两条纪律先写在这里：

一，**每条故障都要有"直连对照"**。上游断掉时客户端拿到半条响应，这件事本身不证明网关正确——必须先证明直连同一个上游也是那个形状，网关才可能被归因。B-001 的直连组、下面每个阶段的"直连确实一个字节都没拿到"都是这个用途。

二，**sanitizer 的"干净"必须带反证**。一个从不报警的检查器和一个干净的代码库，从输出上看完全一样。所以每次记"ASan/LSan 零报告"时，同一段要给出它**会**报警的证据。

三，**被否掉的注入手段也要留数字**。为了让某条线"一定能被顶到"而给对端插一堵墙，测出来的就不再是网关的行为。这类手段的删除记录必须带着"钉与不钉落在同一个位置"的两组数，否则下一个人会把它当作"当时的环境不行"重新加回来。第 5 节末尾那条就是这种记录。

四，**修掉一个会静默失败的 bug，必须留下"哪条断言让它再也回不来"**。现象与根因谁都会记，缺了第三条就等于承认这个 bug 还活着。全部条目的索引与模板在 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) 第 5 节（那边一行一条、只指路不复制），本节仍然按故障类别记完整的三列——**两份不是重复**：本节是"当时预期什么、实际发生什么"，第 5 节是"这个坑归哪条用例管"。写不出第三条的记录，我在索引里明写"缺"，不留空行假装齐了。

---

## 1. 上游侧故障（S1-06 的四类，实测于 `scripts/smoke-s1-06.sh` 阶段四~七，preset=debug，body=1200 B 钉死）

| # | 注入 | 预期 | 实测 | 结论 |
|---|---|---|---|---|
| F-1 | 上游 accept 之后**一个字节都不回**就关（路径 `/dies`） | 下游拿到可定义的 5xx；`upstream_truncated_before_head=1`；`gateway_replies_sent=1`；`responses_completed=0`；fd/连接归零 | 下游拿到 `HTTP/1.1 502 Bad Gateway`，自带 `Content-Length: 12` 且 body 实到 12 字节、带 `Connection: close`；四个计数逐一等于预期；`live_connections=0`、`constructed==destructed==2`、`fd 9→9`。直连同一上游是 `curl` 退出码 52（Empty reply from server）、**零字节** | ✅ 符合预期。502 不可能是转发产物（直连没有任何字节可转） |
| F-2 | 上游回完整响应头 + **半条 body** 就关（`/dies-mid-body`，切在 body 中点） | 头原样送达；body 恰好送达已见的那半；**不追加第二条响应**；`upstream_truncated_in_body=1`；`gateway_replies_sent=0` | 200 头原样、body 恰好 600 字节（不多不少）、文件里没有第二条响应；两个计数等于预期；`curl` 自己返回退出码 18（传输中断）——第三方客户端也认这是截断；且**这一种截断下直连与经网关的可见部分逐字节一致** | ✅ 符合预期。表态方式是连接终止，不是伪造内容 |
| F-3 | 上游回**坏 chunk 框架**（非 hex 的 chunk-size，`/bad-chunk`） | 头之后不向客户端漏任何字节；断流；`upstream_truncated_in_body=1`；`gateway_replies_sent=0` | 头之后零字节漏出（非 hex 的 size 行没被当正文转发）；`curl` 判失败（退出码 18）；两个计数等于预期 | ✅ 符合预期。框架坏了没有"之后"可言，游标停在断掉那一字节 |
| F-4 | 上游回**永不完结的响应头**（持续吐 header 字节，不写 `CRLFCRLF`，`/endless-head`） | 头超出预算即判死，**不无限缓冲**；下游拿到 502；`upstream_truncated_before_head=1`（与 F-1 同一口径） | 拿到 502；`upstream_truncated_before_head=1`、`gateway_replies_sent=1`；`rss_end_kib=4388`（与正常阶段同量级，没有随上游吐字节增长） | ✅ 符合预期。**"超长未齐头"与"EOF 在头之前"落在同一个计数上**——这是口径决定，不是巧合：下游视角两者都是"一条响应都没收到" |

四类故障的每一阶段都独立重启上游与网关，并各自跑同一条生命周期三重检查（`live_connections==0`、`connections_constructed==connections_destructed`、`fd_start==fd_end==9`）。这条不是装饰：F-1 的 502 走的是"不对称关闭"路径（先解链、再分别关），如果解链漏了，级联的 `kDiscardOutbound` 会把刚排进缓冲的 502 当残留字节丢掉——那时候掉的是 `gateway_replies_sent`，而它恰好是这一列里唯一能证明"网关确实表过态"的数。

## 2. 下游侧畸形请求（S1-06 阶段八，`/dev/tcp` 手写三条）

curl 不会替我发这些字节，所以这一段的请求是 shell 手写的：`no-host=31`、`CONNECT=50`、`TRACE=38` 字节。

| 注入 | 预期 | 实测 | 结论 |
|---|---|---|---|
| 缺 `Host` 的请求行合法 | 400，且**不回源** | `HTTP/1.1 400 Bad Request`，body 12 字节送达 | ✅ 映射表兑现 |
| `CONNECT host:443 HTTP/1.1`（authority-form） | 501（不是 400：它语法合法，只是 S1 不做隧道语义） | `HTTP/1.1 501 Not Implemented`，`Content-Length: 16` 与 body 字节数一致 | ✅ 状态码分得开，没有一律 400 |
| `TRACE / HTTP/1.1`（合法方法） | **照常回源、拿到 200** | 上游的 200 回来了，body 是钉死的那 1200 字节 | ✅ 解析器没有方法白名单：网关不替上游拒绝自己能服务的东西 |
| 口径核对 | `requests_rejected=2`、`gateway_replies_sent=2`、`upstream_opened == requests_parsed == 1` | 逐项相等 | ✅ **被拒的请求一条都不该回源**，这条等式就是那句断言 |

这一段第一版是错的：我原本期望 `TRACE → 501`，跑出来是 200。先写了一条单跑 CONNECT 的探针证明实现正确（CONNECT 确实 501），再按响应字节数（38）反推出我当时发的其实是 TRACE——错在测试的期望，不在网关。**"合法方法"与"网关不接的形态"是两件事**，这条现在写进 `reply_status_for_parse_error` 的注释里。

## 3. Sanitizer 面（`bash scripts/verify-all-presets.sh`，本轮 2026-10-26，S1 收尾 = S1-07 + S1-08）

| 检查 | 预期 | 实测 | 结论 |
|---|---|---|---|
| ASan + UBSan（`-O0 -g` 强制） | 112 条全过、零报告 | `asan-ubsan` **112/112**，build exit=0，非 gtest 警告 **0 条** | ✅ |
| LSan（随 ASan） | 零泄漏 | 112/112 内无 leak 报告 | ✅，反证见下 |
| TSan | 零 data race | `tsan` **112/112**（12.7 s），含 4 条 `S1_07_Backpressure` 闭环——这一组是**跨线程**的：测试线程原子切 `set_stall_reads(bool)` 与 `wait_until` 采样，循环线程改 `armed_events`/探测队列 | ✅，而"曾经红过一条"见反证最后一条 |
| debug（gcc） | 全过 | `debug` **112/112**，0 警告 | ✅ |

S1-07 收尾那一轮是三 preset 各 **104/104** + 0 警告，S1-08 之后重跑成 **112/112** + 0 警告；两轮的"非 gtest 警告 = 0"都是门禁自己数出来的，不是我口报的。

**反证（没有这一段，上面的"干净"就只是我的一句话）**：

- 工具会报警这件事在 `ENVIRONMENT.md` 2.2 做过阳性验证：故意写坏的样本下 ASan 报 UAF、LSan 报 5 处泄漏、UBSan 报 signed overflow、TSan 报 data race。
- LSan 的**端到端**反证在 S1-04 做过，本任务沿用同一条路径：故意留一条不释放的连接 → `detected memory leaks` + 退出码 1；删掉 → 退出码 0。
- 本轮新加的一条同类反证：asan/tsan 首跑各报 **1 条非 gtest 警告**（`tools/bench_upstream_main.cc` 里一个只用于注释目的的 `constexpr`，clang 报 `-Wunused-const-variable`，gcc 的 debug preset 不报）。它证明"非 gtest 警告 = 0"这条门禁真的会响。删掉那个未使用常量后归零——**一个没人引用的常量是一句关于结构的谎**，删它比给它找个假用途诚实。
- 优雅停止的反证同样本轮现形：第一版 p99 脚本在 1000 并发还在跑的时候直接 `kill -TERM`，报告里是 `fd_end=351`、`live_connections=342`、`epoll_unregistrations` 比 `registrations` 少 344。**这不是泄漏**——循环退出时那 171 对连接确实还没被收尾，网关的停止语义是"不再接受新连接 + 退出循环"，不排水在飞连接。修法是把观测条件摆正：脚本先静置 5 s 再 SIGTERM，然后**断言** `live_connections==0`、`fd_start==fd_end`、上游 `refused_*==0`（`fatal`，不是 `warn`）。静置后仍不为零才是真漏。改完之后标准口径那一轮是 `fd 9→9`、`live_connections=0`、`constructed==destructed==6008`。
- **S1-08 这一轮 TSan 抓到的是我自己新写的用例**，值得原样记下来：`S1_08_FilterReactor.PassThroughFiltersChangeNothingInTheForwardingPath` 第一版在流量跑完之后、循环还在转的时候读 `epoll_registrations()`/`epoll_modifications()`——那是 `Epoll` 里的普通 `uint64_t`，循环线程正在自增。TSan 报 data race，ctest 里红一条。修法不是豁免、不是加 mutex，是把读法摆正：**先 `stop_loop()`（join 循环线程）再读那两个数**——这正是 `stats.h` 第 25 行那条"非原子观测只能在循环停下之后读"的纪律，而它这次抓的是写纪律的人自己。**这条抓取的副产品比用例本身更有价值**：它证明"TSan 干净"这句话在本仓库不是自我声明，也证明那条纪律不是装饰。（一个我没归掉的现象，照记：门禁那一轮 tsan 的汇总行是 `1 tests failed out of 85`，而我随后单独重跑同一 preset 是 `1 failed out of 112`——85 这个数说明那一轮 ctest 提前停了，为什么停我没有查，改完两条都跑满 112 全绿。）

## 4. S1-05 的 Upgrade 透传验证结果（门禁四件套第 2 条点名的一项）

| 项 | 预期 | 实测 |
|---|---|---|
| 101 之后不再按 HTTP 读 | 两帧二进制字节逐字节回显比对通过；`requests_parsed`/`parse_calls` 停在 1（隧道里**零解析动作**） | ✅ `UpgradeRequestSwitchesToRawTunnelAfterThe101` |
| 只有 `Upgrade:` 头、没有 `Connection: upgrade` 记号 | 不进隧道，101 当普通响应字节漏下，`upgrades_101=0` | ✅ `UpgradeHeaderWithoutConnectionTokenIsNotATunnel`（测试对端刻意比网关宽松，这个不对称组合证明点燃隧道的按钮在网关手里） |
| 101 头被 TCP 切成三段 | 拼回原样、`parse_calls==1` | ✅ `TunnelSurvivesSplitResponseHead` |
| `release_scratch()` 的调用点 | 全仓**唯一**一处，且在 101 头排进下游出向缓冲之后 | ✅ 结构上不存在"请求结束就释放"的自动路径（接口没有那条路径，不是靠约定） |
| TSan 下的握手闭环 | 零 data race | ✅ tsan preset 内 5 条握手用例全过 |

## 5. S1-07 的背压与慢读面（实测于 ctest `S1_07_Backpressure` 四条 + `scripts/smoke-s1-07.sh` 四阶段）

| # | 注入 | 预期 | 实测 | 结论 |
|---|---|---|---|---|
| B-1 | 上游 accept 后**一个字节都不读**（照旧持有连接），下游连打 384 条完整 POST | 网关摘掉这条下游 fd 的 EPOLLIN；入向读取累计进入平台期；上游开始消费之后**不需要客户端再写新请求**就自动恢复，384 条响应逐条对号 | `RequestDirectionStopsReadingAndRecoversWithoutLoss`：pause/resume 各 ≥1，平台期 **2,922,610 B**（debug）/ **2,930,867 B**（asan），载荷 3,170,580 B，384 条响应的 body 序号逐条核对通过 | ✅ 「水位回落自动恢复」在请求方向成立，且恢复靠的是探测读而不是下一次事件 |
| B-2 | 1 MiB 响应 + 客户端**一个字节都不读** | 响应方向也要停读，而且不丢 | `ResponseDirectionSurvivesASlowClientByteForByte`：pause/resume ≥1、撞线 4..22 视时序而定，客户端逐字节取回 **1,048,644 B** | ✅ 方向对称：压强来自客户端时同一条链生效 |
| B-3 | 隧道里压 2,742,440 B 非 HTTP 形态字节（0x00/0x0D/0x0A 混在其中）+ 上游停读 | 摘读、平台期、恢复后**一个字节不少**，且帧字节一次都不进解析器 | `TunnelDirectionPausesAtTheWaterLineAndDeliversEveryByte`：平台期 **1,898,661 B**(debug) / **1,897,637 B**(asan)，`bytes_tunnel_down == 载荷`、`bytes_tunnel_up == 载荷 + 101 头`、`requests_parsed` 全程 = 1 | ✅ 关闭 1.13 留的隧道缺口；同时是 ADR-09 的一次复查 |
| B-4 | 真进程：`/dev/tcp` 在一条 keep-alive 上连打 **400 条** POST（3,300,800 B）打在 `--stall-reads-ms 8000` 的上游上 | stall 期间网关停读、写侧卡在 `write()`；stall 结束后 400 条响应全部到达 | 冒烟阶段二：`pause=1..5 resume=1..5 撞线=47..192`、`requests_parsed=400`、`responses_completed=400`、`bytes_read_downstream == bytes_down_to_upstream == 3,300,800`、`upstream_opened=1`、状态行 400/400 | ✅ 关闭原第 6 节第一条（慢读之后的恢复） |
| B-5 | `--high-water-mark 4096` + 一条 100 KiB 的 POST（**停读线低于一条请求**） | 这条请求既不该被丢，也不该被报错：它停在停读线上 | 冒烟阶段三：curl 退出码 28（超时）且**零字节**，网关 `requests_parsed=0`、`upstream_opened=0`、`gateway_replies_sent=0`、`pause=1`、`resume=0`、`bytes_read_downstream=4096`、**`read_disarmed_connections=1`** | ✅ 这是 1.15 那条边界的实测形态。`read_disarmed_connections=1` 是「网关不再消费下游可读事件」在全仓唯一被**直接读出来**的证据 |
| B-6 | 上游 accept 后回**半条响应头**就关（`/dies-mid-head`，17 字节、无 CRLFCRLF） | 头之前断 → 自产 502；半条头不外漏；且必须有直连对照 | 冒烟阶段四：直连确实拿到没说完的头（17 B 中没有 CRLFCRLF）；经网关 `HTTP/1.1 502` + `Content-Length: 12` 实到 12 B + `Connection: close`，文件里没有任何 200 的痕迹；`upstream_truncated_before_head=1`、`in_body=0`、`gateway_replies_sent=1`、`responses_completed=0` | ✅ 关闭原第 6 节第二条。与 F-2 的**不对称方向相反**是设计：那里直连与经网关必须一致（头已发出），这里必须不一致（半条头没有"已经发出的响应"可供改写） |

四条 B-* 的每阶段都独立重启上下游，并各自跑生命周期四重检查（`live_connections==0`、`constructed==destructed`、`fd_start==fd_end`、**`read_disarmed_connections==0`**）。最后一条是 S1-07 新加的：背压改动的是订阅掩码，最容易留下的就是"连接带着摘掉的 EPOLLIN 被关掉"。B-5 是唯一一处**期望它不为 0** 的阶段（注入的就是一条永不完结的连接，见 1.15），那里断的是另外三件事：退出码 0、`fd_end=8`、RSS 不随请求字节涨。

**本任务最有信息量的一次阴性结果：被删掉的"钉住上游接收窗口"实验。** 测试上游的 stall 原本带第二个动作——stall 期间把每条连接显式设 `SO_RCVBUF=4096`（内核翻倍记账 → 通告窗口 8 KiB），目的是让应用的停读线成为第一道墙。三组证据说明它既没换来更早的撞线、又毁掉了用例要证的东西：

1. 钉与不钉，平台期落在同一个位置：隧道方向 1,802,405 B（钉）vs 1,898,661 / 1,897,637 B（不钉）。第一道墙是**网关自己的 `SO_SNDBUF`**（这台机器上自动调到 ≈1.9 MiB），不是上游的接收窗口。
2. `release_receive_window()`（放开时设 `SO_RCVBUF=4 MiB`）**没有实现它注释里承诺的事**：`ss -tin` 取证显示夹具侧 `rcv_wnd:8192` 在放开之后 40 s 内一次没回涨（Linux 的 `window_clamp` 不因重设大值而放开），网关侧 `rwnd_limited` 从 4,852 ms 涨到 39,904 ms 且恒为 100.0%，`notsent` 每 5 s 恰好减 196,608 B = 24 × 8192，即一档一个 RTO（`rto:204`）。恢复吞吐被 TCP 的零窗口探测计时器钉死，用例红的是内核计时器而不是网关的恢复链路。
3. 于是隧道那条在 asan-ubsan 下跑到 **48,058 ms**，超过 ctest 的 `TIMEOUT 30`。**按纪律改的是测试，不是超时**：整段钉窗机器（`clamp_receive_window`/`release_receive_window`/`window_clamped`/`stall_clamps_window_`）删除，两组装在同一个位置的数字写在 `tests/test_upstream.h` 的删除记录里。

同一个理由还产出了一次**用例自我修正**：256 条 POST（2,113,684 B）在 asan 下平台期恰好等于整个载荷——那是写侧把货倒进了内核路径，不是网关摘读，断言会为一个不成立的理由通过。加载荷抬到 384 条（3,170,580 B）之后两个 preset 都停在 2.93 MB 附近，平台期才只能由"网关不再 read"产生。

**新发现、本任务不修的边界（照记）**：一批 pipelined 请求里带 `Connection: close` 的那条，会在**第一条响应完结时**就被兑现——意图的字面意思就是"下一条响应之后收尾"，而"下一条"在 400 条同时在飞的批次里就是第一批里的那一条。实测形态：冒烟阶段二第一版只拿到 1 条响应 + EOF，而 `bytes_down_to_upstream == bytes_read_downstream == 3,300,800`、`requests_parsed=400` 全部对得上，所以这不是丢字节，是"一条连接上同时在飞多条请求"这件事没有语义支撑。S1 声明的是 keep-alive **复用**（一条连接顺序处理请求），不是 pipeline；改法是数够条数后由测试自己关连接，那条边界留在下面第 6 节。

## 6. 还没注入的故障（记在这里，别当成"已验证"）

| 缺口 | 为什么现在没有 | 归属 |
|---|---|---|
| `SIGTERM` 时在飞连接的**排水** | 现在的停止语义不排水（见第 3 节反证最后一条，以及 S1-07 冒烟阶段三那条**故意留着不排**的连接）。真正的优雅停机（停止接受 → 等在飞走完 → 超时兜底）不在 S1 范围 | S2 之后，与连接池同期 |
| 上游进程被 `kill -9`（连 backlog 里的半连接一起消失） | 冒烟脚本目前用 `kill -TERM` 收尾上游；SIGKILL 下的 accept 失败路径只有 ctest 覆盖 | S3（健康检查/摘除的注入面） |
| 一批 pipelined 请求里的 `Connection: close` 应在**最后一条**响应之后兑现 | S1 不声明 pipeline 语义（见第 5 节最后一段）。要做的是"未完结请求计数 + 每条响应的配对归属"，那属于请求-响应配对模型本身 | 决定支持 pipeline 的任务（S1 范围外，需要 ADR） |
| 跨机链路上停读线的实际尺寸 | 本任务的所有数字都是同机回环的：回环上内核路径吸收 ≈1.9 MiB，应用的线基本没活干；真实链路（带宽延迟积大）才是 256 KiB 默认量的主场 | S4 的 wrk 跨机组 |
| 配置 reload 时上游连接抖动 | 配置热重载在 S5 | S5 |
| 100k 空闲长连接下的 fd/内存 | ~~`ulimit -n` 现在 10240，先决条件没到位~~（2026-10-27 实测：hard=1048576，soft 可自抬，**不是环境阻塞**；真前置是临时端口 28,232 个，见 `ENVIRONMENT.md` 第 7 节）；S1-07 新加的那 8 B/连接与本轮 5.81 KiB/连接读数同样没有 100k 规模的验证 | S2/S3，随 `c100k.sh` 的口径设计一起解决 |
