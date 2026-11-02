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

S1-07 收尾那一轮是三 preset 各 **104/104** + 0 警告，S1-08 之后重跑成 **112/112** + 0 警告；两轮的"非 gtest 警告 = 0"都是门禁自己数出来的，不是我口报的。**S2-05（优雅停止/排水）收尾这一轮是三 preset 各 132/132、非 gtest 警告 0 条、`tsan` 零 data race**（`verify-all-presets.sh` 的三行输出，逐行看，契约纪律六）。

**反证（没有这一段，上面的"干净"就只是我的一句话）**：

- 工具会报警这件事在 `ENVIRONMENT.md` 2.2 做过阳性验证：故意写坏的样本下 ASan 报 UAF、LSan 报 5 处泄漏、UBSan 报 signed overflow、TSan 报 data race。
- LSan 的**端到端**反证在 S1-04 做过，本任务沿用同一条路径：故意留一条不释放的连接 → `detected memory leaks` + 退出码 1；删掉 → 退出码 0。
- 本轮新加的一条同类反证：asan/tsan 首跑各报 **1 条非 gtest 警告**（`tools/bench_upstream_main.cc` 里一个只用于注释目的的 `constexpr`，clang 报 `-Wunused-const-variable`，gcc 的 debug preset 不报）。它证明"非 gtest 警告 = 0"这条门禁真的会响。删掉那个未使用常量后归零——**一个没人引用的常量是一句关于结构的谎**，删它比给它找个假用途诚实。
- 优雅停止的反证同样本轮现形：第一版 p99 脚本在 1000 并发还在跑的时候直接 `kill -TERM`，报告里是 `fd_end=351`、`live_connections=342`、`epoll_unregistrations` 比 `registrations` 少 344。**这不是泄漏**——循环退出时那 171 对连接确实还没被收尾，当时网关的停止语义是"不再接受新连接 + 退出循环"，不排水在飞连接。修法是把观测条件摆正：脚本先静置 5 s 再 SIGTERM，然后**断言** `live_connections==0`、`fd_start==fd_end`、上游 `refused_*==0`（`fatal`，不是 `warn`）。静置后仍不为零才是真漏。改完之后标准口径那一轮是 `fd 9→9`、`live_connections=0`、`constructed==destructed==6008`。**（S2-05 起这句"不排水"本身已经过时：SIGTERM 现在走排水，见下面第 6 节那一行与 ARCHITECTURE 1.20。但这条反证仍然作数——它证的是"脚本的观测条件不能靠我的印象"，那个结论跟停止语义怎么定无关；而且正因为排水会主动关连接，"静置后断言归零"从"绕开一个边界"变成了"验证一条功能"。）**
- **S1-08 这一轮 TSan 抓到的是我自己新写的用例**，值得原样记下来：`S1_08_FilterReactor.PassThroughFiltersChangeNothingInTheForwardingPath` 第一版在流量跑完之后、循环还在转的时候读 `epoll_registrations()`/`epoll_modifications()`——那是 `Epoll` 里的普通 `uint64_t`，循环线程正在自增。TSan 报 data race，ctest 里红一条。修法不是豁免、不是加 mutex，是把读法摆正：**先 `stop_loop()`（join 循环线程）再读那两个数**——这正是 `stats.h` 第 25 行那条"非原子观测只能在循环停下之后读"的纪律，而它这次抓的是写纪律的人自己。**这条抓取的副产品比用例本身更有价值**：它证明"TSan 干净"这句话在本仓库不是自我声明，也证明那条纪律不是装饰。（一个我没归掉的现象，照记：门禁那一轮 tsan 的汇总行是 `1 tests failed out of 85`，而我随后单独重跑同一 preset 是 `1 failed out of 112`——85 这个数说明那一轮 ctest 提前停了，为什么停我没有查，改完两条都跑满 112 全绿。）
- **S2 多线程这两轮的 TSan 反证（摘锁阳性验证，2026-10-27）**：S2-01 与 S2-03 各做过一次同一形状的反证——临时摘掉 handoff 队列那把 `std::lock_guard<std::mutex>` 后重跑 `tsan`，S2-01 的并发用例当场被报出 **3 处** data race、S2-03 更狠的 `StopDuringActiveChurnShutsDownCleanly`（负载活跃时停网关）报出 **14 处**；还原那把锁后三 preset 各 **123/123** 全绿、零 race。这证明"TSan 干净"在多线程阶段仍是**可证伪的绿**：红队用例真把 acceptor↔worker 那条唯一的跨线程边界压到了"少一把锁就当场红"的程度，而不是"跑了一遍没报"。红队用例（`S2_03_CrossThreadDestruction` 三条）攻的是 per-worker 池（ADR-15）之后**仍然存在**的两个跨线程面——(a) 负载活跃时 stop、(c) handoff 的 fd 在 worker 注册前被对端关；而"连接被非 owner 线程持有 shared_ptr"那类 harder 竞态**没有出现**，因为所有持有者都在同一个 worker 线程内（销毁协议定稿在 ADR-14）。asan/LSan 侧：churn 用例创建销毁上千条连接，零 UAF、零泄漏。
- **S2-05 这一轮的 TSan 反证（把跨线程标志做成普通 bool，2026-10-28）**：排水引入的跨线程接触面只有两个量（`draining_` 与 `drain_deadline_ms_`），所以反证就打在这两个上面——临时把 `draining_` 换成普通 `bool draining_plain_`（写入来自信号/测试线程，读取在循环线程），`tsan` preset 下 **6 条 `S2_05_GracefulStop` 里 5 条当场报 data race**（5 处 "Write of size 1 at 0x72540000042c by main thread"）；把标志还原成 `std::atomic<bool>` 之后三 preset 各 **132/132** 全绿、零 race。这条与上面那条"摘 handoff 锁 → 报 3/14 处"是同一类反证：**"TSan 干净"在这里仍然是一句可证伪的话，不是我的自述**。（栈仍然是裸地址——`llvm-21-tools` 缺失这条挂起项没变，定位靠地址 + BuildId 手查。）
- **排水机制本身的三次"能红才叫绿"（不是 sanitizer，是逻辑判据）**：① 去掉扫描里"在飞>0 就等"那条守卫 → 101/105/106 三条同时红（在飞请求被砍）；② 摘掉"隧道先于在飞判据"那条分支 → 104 红，而且是**先挂到 1.26s 的兜底再红**（隧道的 101 不走 `kComplete`，在飞计数永不为零，所以那条分支是承重的）；③ 关掉 accept 层的排水拒绝 → 105 红（`requests_parsed` 从 1 变 2，新连接被服务了）。**还有一次"拆了也不红"，原样记下来**：把隧道收尾从 `kFlushThenClose` 改成 `kDiscardOutbound`，6 条**全绿**——4096 字节的回显早在排水之前就被内核套接字缓冲吸收了，网关出向是空的，两种写法观察上等价。我试过把载荷抬到 3 MiB 想让它变红，结果**连正确实现也整段送不完**（配对开始收尾后上游侧 EPOLLIN 已摘，还压在 socket 里的那段没人再读）。所以"硬关之前先刷完已缓冲字节"这句判据**不归这条用例**，归 S1-04 的 `UpstreamBreakClosesBothSidesAfterFlushingResponse`（那条能红），用例注释与本节都按这个边界写，不给它配大于其证据的说法。

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
| B-5 | `--high-water-mark 4096` + 一条 100 KiB 的 POST（**停读线低于一条请求**） | 这条请求既不该被丢，也不该被报错：它停在停读线上 | 冒烟阶段三：curl 退出码 28（超时）且**零字节**，网关 `requests_parsed=0`、`upstream_opened=0`、`gateway_replies_sent=0`、`pause=1`、`resume=0`、`bytes_read_downstream=4096`，而**停在停读线上这件事现在由 `drain_read_disarmed_at_entry=1` 报出来**；S2-05 之后这条连接最终被兜底强关（`drain_forced_timeout=1`、`live_connections=0`、`fd 8→8`、RSS 不随请求字节涨） | ✅ 这是 1.15 那条边界的实测形态。**这一格的观测口径在 S2-05 换过一次，换的原因写在这里而不是含糊过去**：SIGTERM 从"硬停"改成"排水"之后，那条卡住的连接不再留在注册表里等我在循环停下之后去读 `read_disarmed_connections`——它被兜底收尾了，那个数因此回 **0**，而 0 是排水做对了的表现，不是"没触发"。判据改由**排水的第一次扫描**就地取（取的人正是循环线程，注册表此刻归它独占，所以不违反 `stats.h` 那条"非原子观测只能在循环停下后读"的纪律）。ctest 侧 `connections_with_read_disarmed()` 的严格断言一条没动 |
| B-6 | 上游 accept 后回**半条响应头**就关（`/dies-mid-head`，17 字节、无 CRLFCRLF） | 头之前断 → 自产 502；半条头不外漏；且必须有直连对照 | 冒烟阶段四：直连确实拿到没说完的头（17 B 中没有 CRLFCRLF）；经网关 `HTTP/1.1 502` + `Content-Length: 12` 实到 12 B + `Connection: close`，文件里没有任何 200 的痕迹；`upstream_truncated_before_head=1`、`in_body=0`、`gateway_replies_sent=1`、`responses_completed=0` | ✅ 关闭原第 6 节第二条。与 F-2 的**不对称方向相反**是设计：那里直连与经网关必须一致（头已发出），这里必须不一致（半条头没有"已经发出的响应"可供改写） |

四条 B-* 的每阶段都独立重启上下游，并各自跑生命周期四重检查（`live_connections==0`、`constructed==destructed`、`fd_start==fd_end`、**`read_disarmed_connections==0`**）。最后一条是 S1-07 新加的：背压改动的是订阅掩码，最容易留下的就是"连接带着摘掉的 EPOLLIN 被关掉"。B-5 是 S1 那一轮唯一一处**期望它不为 0** 的阶段（注入的就是一条永不完结的连接，见 1.15），那里断的是另外三件事：退出码 0、`fd_end=8`、RSS 不随请求字节涨。**S2-05 之后 B-5 这一格改成"期望停下之后为 0、期望排水快照里为 1"**——同一条现象换了读取时刻，没有换判据强度，也没有哪个阶段悄悄少断了一件事（`scripts/smoke-s1-07.sh` 阶段三现在两条都断：`drain_read_disarmed_at_entry=1` + `drain_forced_timeout=1` + `live=0`）。

**本任务最有信息量的一次阴性结果：被删掉的"钉住上游接收窗口"实验。** 测试上游的 stall 原本带第二个动作——stall 期间把每条连接显式设 `SO_RCVBUF=4096`（内核翻倍记账 → 通告窗口 8 KiB），目的是让应用的停读线成为第一道墙。三组证据说明它既没换来更早的撞线、又毁掉了用例要证的东西：

1. 钉与不钉，平台期落在同一个位置：隧道方向 1,802,405 B（钉）vs 1,898,661 / 1,897,637 B（不钉）。第一道墙是**网关自己的 `SO_SNDBUF`**（这台机器上自动调到 ≈1.9 MiB），不是上游的接收窗口。
2. `release_receive_window()`（放开时设 `SO_RCVBUF=4 MiB`）**没有实现它注释里承诺的事**：`ss -tin` 取证显示夹具侧 `rcv_wnd:8192` 在放开之后 40 s 内一次没回涨（Linux 的 `window_clamp` 不因重设大值而放开），网关侧 `rwnd_limited` 从 4,852 ms 涨到 39,904 ms 且恒为 100.0%，`notsent` 每 5 s 恰好减 196,608 B = 24 × 8192，即一档一个 RTO（`rto:204`）。恢复吞吐被 TCP 的零窗口探测计时器钉死，用例红的是内核计时器而不是网关的恢复链路。
3. 于是隧道那条在 asan-ubsan 下跑到 **48,058 ms**，超过 ctest 的 `TIMEOUT 30`。**按纪律改的是测试，不是超时**：整段钉窗机器（`clamp_receive_window`/`release_receive_window`/`window_clamped`/`stall_clamps_window_`）删除，两组装在同一个位置的数字写在 `tests/test_upstream.h` 的删除记录里。

同一个理由还产出了一次**用例自我修正**：256 条 POST（2,113,684 B）在 asan 下平台期恰好等于整个载荷——那是写侧把货倒进了内核路径，不是网关摘读，断言会为一个不成立的理由通过。加载荷抬到 384 条（3,170,580 B）之后两个 preset 都停在 2.93 MB 附近，平台期才只能由"网关不再 read"产生。

**新发现、本任务不修的边界（照记）**：一批 pipelined 请求里带 `Connection: close` 的那条，会在**第一条响应完结时**就被兑现——意图的字面意思就是"下一条响应之后收尾"，而"下一条"在 400 条同时在飞的批次里就是第一批里的那一条。实测形态：冒烟阶段二第一版只拿到 1 条响应 + EOF，而 `bytes_down_to_upstream == bytes_read_downstream == 3,300,800`、`requests_parsed=400` 全部对得上，所以这不是丢字节，是"一条连接上同时在飞多条请求"这件事没有语义支撑。S1 声明的是 keep-alive **复用**（一条连接顺序处理请求），不是 pipeline；改法是数够条数后由测试自己关连接，那条边界留在下面第 6 节。**（S2-04/ADR-16 已闭合这条：加了在飞请求计数，close 在它自己那条响应之后才兑现、之后的请求不再转发；见第 6 节该行与 ARCH 1.19。）**

## 6. 还没注入的故障（记在这里，别当成"已验证"）

| 缺口 | 为什么现在没有 | 归属 |
|---|---|---|
| ~~`SIGTERM` 时在飞连接的**排水**~~ | **S2-05 已闭合（2026-10-28）**：SIGTERM/SIGINT 走 `request_drain()`——摘掉接受（accept 到就关，监听 fd 留到析构）、在飞请求逐条答完 + 刷完出向、空闲连接立即关、`--drain-timeout-ms`（默认 5000）到点强关、隧道硬关。机制在 ARCHITECTURE 1.20，反证在第 3 节最后两条 | 已做（`S2_05_GracefulStop` 六条 + `scripts/smoke-s2-05.sh` 五阶段；还掉的债在 ROADMAP 的 S1→S2 债表） |
| 排水窗口里**上游侧**的在飞响应如果永远不回（上游被 kill -9、或它自己也在下线） | 现在的答案是"到点强关，客户端看到截断"——这条路径本身没被注入过（用例里的在飞请求最后都答完了，只有半包那条走了兜底） | S3 的健康检查/摘除注入面：`kill -9` 上游 + 排水同时发生 |
| 上游进程被 `kill -9`（连 backlog 里的半连接一起消失） | 冒烟脚本目前用 `kill -TERM` 收尾上游；SIGKILL 下的 accept 失败路径只有 ctest 覆盖 | S3（健康检查/摘除的注入面） |
| ~~一批 pipelined 请求里的 `Connection: close` 应在它自己那条响应之后兑现~~ | **S2-04/ADR-16 已闭合**：配对模型加了在飞请求计数（转发 +1 / 响应完结 −1），close 只在"带 close 那条请求的响应完结、在飞归零"时兑现；close 之后的请求不再转发（循环体内查 `close_after_forward`），随配对关闭丢弃 | 已做（`S2_04_Pairing` 三条 + 旧条件阳性验证会红，ARCH 1.19 / ADR-16）|
| 跨机链路上停读线的实际尺寸 | 本任务的所有数字都是同机回环的：回环上内核路径吸收 ≈1.9 MiB，应用的线基本没活干；真实链路（带宽延迟积大）才是 256 KiB 默认量的主场 | S4 的 wrk 跨机组 |
| 配置 reload 时上游连接抖动 | 配置热重载在 S5 | S5 |
| 100k 空闲长连接下的 fd/内存 | **部分闭合（S2-06，2026-10-28）**：25,000 连接对实测 **17.47 KiB/对**、稳态 30 s 散布 0 KiB、空闲 CPU 占单核 0.17%、排水 0.5 s、`constructed==destructed==50000`、`fd 39→39`；100k 名义档也验证了**接入侧与生命周期**在 100k 规模下闭合（`downstream_accepted=100000`、`accept_errors=0`、`live=0`、`constructed==destructed==128231`、`fd 39→39`、排水 1.0 s）。**没闭合的是"每连接内存"这一格本身**——100k 连接对开不出来，卡在网关回源那一跳的单元组容量 28,232（`established=28231`）。原先记的 `ulimit -n`=10240 那条硬前置 **2026-10-27 已实测推翻**（hard=1048576，soft 可自抬，不是环境阻塞） | 归 **ADR-17 末条（2026-10-28 已裁＝候选 A：端点列表 + per-worker 静态映射）**；落地与 100k 实测归新增任务 **S2-06a**，#2 的判定随之第一次可做 |
| 回源端点耗尽／上游永久不可达时网关怎么表态 | S2-06 第一次把它测出来：71,769 条下游连着、请求留在缓冲里，网关**既不关下游也不回错误响应**，客户端只能自己 30 s 超时（`handshake-timeout=71769`、`upstream_connect_failed=75708` 含 3,939 次重试）。不伪造状态码是 S1-06 定的口径，所以这一格现在是**空的**而不是坏的 | S4（请求超时 + 自产响应）；重试的退避与摘除归 S3（健康检查）/ADR-12（ejection cap） |

## 7. 测量工具自己的反证（S2-06，2026-10-28）

压测工具不进 ctest（`tools/` 的语义是**被构建、不被断言**），所以"它报的数是真的吗"这个问题
没有测试可依赖。这一节就是它的替代物：**三条等式 + 一条量纲口算**，全部本轮实测。

| 判据 | 实测 | 它能拦住什么 |
|---|---|---|
| 客户端 `requests_done` == 网关 `requests_parsed` == 网关 `responses_completed` == 上游 `requests` | **12000 / 12000 / 12000 / 12000**（1 条连接×2000 + 50 条×200 两次合并） | 工具自己吞掉或重复计一条请求。吞掉的话分位数是"更少事件"的分布，重复计的话 QPS 虚高，两种都不红 |
| `bytes_down_to_upstream` == 条数 × 请求字节数 | 720,000 == 12000 × **60**（那条 GET 恰好 60 B） | 请求段被截断/粘包后还在计数——字节等式先塌，条数等式可能还撑着 |
| `bytes_up_to_downstream` == 条数 × 响应字节数 | 13,068,000 == 12000 × **1089**（头 65 + 体 1024） | 响应只收到头就算完结：那样延迟会小一个数量级而条数照对，**只有字节等式抓得住** |
| `mean × 并发数 ≈ 吞吐窗口 ÷ 请求数`（量纲口算） | 50 条并发：mean **176.9 µs** × 50 = 8.8 ms ↔ 窗口 40 ms / 10000 条 = 4.0 ms/条同一量级；1 条：mean 184.7 µs ↔ 窗口 439 ms / 2000 = 220 µs | 延迟数被工具自己的循环节奏污染（本轮就抓到一次，见下面第 2 条） |

**两次"工具自己把数读错"，都靠上面这些等式现形，不是靠读代码看出来的**：

1. **握手 p50 随连接数线性增长、与建连速率无关**（250→18.0 ms、500→33.8、1000→71.7、2000→143.4；把 `--ramp-ms` 从 2000 拉到 10000，500 条那一档**一模一样**是 32.3 ms）。限速配额按"每轮循环睡满 50 ms"算，而 `epoll_wait` 一有事件就返回 → 整批 connect 挤在最初几十毫秒里，测的是内核 accept 队列不是网关。修成按**经过时间**线性摊开后：500 条/2 s→**1.25 ms**、500 条/10 s→**422 µs**、4000 条/8 s→**1.82 ms**，延迟重新跟着速率走。
   - 这中间我做错过一次归因：先用"两个客户端进程各 250 条并发"去区分"客户端的锅还是网关的锅"，看到两条都变成 48 ms 就判给了网关。**那个判据不成立**——两个客户端带着同一个缺陷，burst 只是合并成了更大的 burst。真正推翻它的是控制实验（500 条**直连上游**、不经网关，p50 **152 µs**）。记下来是因为这类"看起来是隔离变量的对照"最容易骗到我。
2. **闭环每请求延迟 mean 4.2 µs，而窗口÷条数是 468 µs**——两个数互相打脸。根因：注册时为了拿 connect 结果带的 `EPOLLOUT` 在请求写完后没摘，电平意义下永远就绪，于是每轮事件都重打一次计时起点，量出来的是"从缓冲区取数的耗时"。修完 mean **177 µs**（50 并发）/ **185 µs**（1 并发），并且 `self_cpu_ticks` 从 26 掉到 10——那个没摘的 EPOLLOUT 同时在让工具自旋。

**隧道形态的 25k 那一档（`build/bench/tier-25k-final/`）**：`established=25000`、`failed=0`、
`upgrades_101=25000`、`upstream_opened=25000`（1:1，ADR-15 的池在这条路径上不摊薄）、
`accept_errors=0`、`downstream_rejected_over_capacity=0`、`upstream_connect_failed=0`、
`constructed==destructed==50000`、`fd 39→39`、`live=0`、稳态 30 s 内 RSS 散布 **0 KiB**、
每连接内存 **17.47 KiB/对**（目标 ≤20）、网关空闲 CPU **0.17%（占单核）/ 0.01%（占整机 16 核）**、
排水 **0.5 s** 内进程退干净（`live=0`、`constructed==destructed==50000`、`fd 39→39`）。
**这句在 S2-06a 被我自己更正过一次**：原文写"排水收干净 25000 条隧道"，而那一轮
`drain_tunnels_hard_closed=0`、`drain_closed_idle=0`、`connections_closed=50000`——隧道是客户端
退出时的 FIN 级联关掉的，排水没活干；0.5 s 只是网关退出生效的时间。**"S2-05 在规模下的第一次
真实使用"这个说法因此也撤回**：规模档里真正走过硬关的是 100k 名义档那轮的 `drain_tunnels_hard_closed=28231`。
这一档是**非标准口径**（`planned_pairs=25000` ≠ SLO #2 的 100k），所以它进的是"最大可测档 +
工具与拓扑的现状"，不是 #2 的判定；判定卡在第二跳的端口天花板上，见 ADR-17 最后一条。

**100k 名义档那一轮还多暴露两件事，都按原样记（`build/bench/tier-100k/`）**：

1. **回源打满之后，网关不表态**。`downstream_accepted=100000` 而 `established=28231`：那 71,769 条下游连上了、请求留在下游缓冲里，网关**既不自产错误响应、也不关下游**（`forward_request` 拿 `nullptr` 就 `return false`，注释写的是"请求还留在下游缓冲里，没丢"——它确实没丢，它只是永远等不到）。于是**每一方都在等**：客户端等到自己的 30 s 握手上限（`handshake-timeout count=71769`，一条 `connect` 失败都没有，因为墙不在下游侧），网关则在反复重试回源（`upstream_connect_failed=75708 > 71769`，多出的 3,939 次是重试）。"上游连不上时多久表态、怎么表态"是 S4 请求超时与自产响应那一格——**这一轮第一次让这个空格子可见**。它不是 bug（不伪造状态码是 S1-06 就定下的口径），但没有故障注入之前没人能证明它是空的。
2. **一次"跑完了"的假象，差点变成一个不存在的数字**。为了证明 `--upgrade` 没把 B-001…B-005 的基线带走，我用 `nohup … &` 复跑了一轮 `s1-p99.sh`；父 shell 一退，WSL 把整条链回收了，日志停在 `[proxy] 稳态 60s`，而后台任务回报 **completed exit 0**。我在没读到终局的情况下把它当成"全绿、增量 0.540 ms"说了一遍。**那个数字是不存在的**，发现后停笔重跑，真值是 **31.150 ms**（B-006 附）。防复发的形式写进了 BENCHMARK 那条记录里：**压测数字只从落盘的 summary 文件取**（`s1-p99.summary.txt` 存在且 `canonic=1` 才算跑完），后台任务的退出码不是完成证明。

### S2-06a 新增的三条工具反证（2026-10-28 深夜，回源多端点落地前/中）

裁决走 ADR-17 候选 A 之后，我先跑的是**不改 `src/` 的探针**：`l7gate_c100k` 直连 4 个目标地址上的
`l7gate_bench_upstream --upgrade`（**不经网关**），问一件事——单个上游进程能不能真的持有 100k 条已
accept 的隧道。这一问抓出三条，都属于"工具自己的账"，所以记在这一节而不是 ARCHITECTURE。

1. **观察者自己会造出一堵假墙，签名很好认：对端的服务计数满了，而自己的 established 冻住、且冻得均匀。**
   第一轮（`--ramp-ms 120000` = 833 条/秒、握手上限 30 s）：`established=64282 / planned=100000`、
   `failed=35718` **全部是 `handshake-timeout`**，`not_101=0`、`peer_closed=0`；同轮上游
   `accepted=100000 requests=100000 responses_written=100000 upgrades=100000 bytes_written=12,900,000`
   （= 100k × 129 B 的 101 头）——**上游一条都没漏**。而客户端的 `established` 从 60,958 起在 5 个进度行里
   一动不动、`started` 还在从 65,904 爬到 100,000，每目标 established 均匀停在
   16,070/16,071/16,072/16,069。均匀排除了"某个地址先耗尽"，剩下的读法只有一个：**它自己在算不过来**。
   - **判别实验**（同机、同 binary、只改两个参数：`--ramp-ms 300000` = 333 条/秒、`--handshake-timeout-ms 120000`）：
     `established=100000 / planned=100000`、`failed=0`、每目标 **25,000**。所以那一轮是**吞吐受限，不是卡死**。
     握手分位随窗口一起变好也支持这个读法：快 ramp p50 1,184 / p99 **9,699,328** µs；慢 ramp p50 **1,008** / p99 **108,544** µs。
   - **没归掉的那一格，按原样留着**：客户端单线程循环在 ~100k fd 下具体慢在哪一次 syscall，我没有证
     （`perf` 与 6.18.40.1-WSL2 内核的匹配性仍是 `ENVIRONMENT.md` 第 4 节的未验证项）。这条记录因此只到
     "吞吐受限、可用更慢的 ramp 绕开"，不到"根因是 epoll_ctl 的反查"那种我没测过的说法。
   - **它的后果进了口径**：SLO #2 的 100k 档从此用 `RAMP_MS=300000 / HANDSHAKE_TIMEOUT_MS=120000`。
     动的是工具参数，不是目标值也不是分母（禁令二没被碰），SLO 写死的条件只有"握手后空闲 + 达标后静置 ≥60 s"，
     这两条一条没少。

2. **稳态窗口的右边界是被测方的性质还是我取数的定义？这一次是我的。**
   E=4 的 25k 档复跑报出空闲 CPU **1.57%**（B-006 同档是 **0.17%**），我第一反应是"多端点让网关变忙了"。
   同机 A/B 把它否了也把它解释了：`UPSTREAM_ENDPOINTS=1` 那一轮 RSS 逐 KiB 相同（441,204）而 CPU 回到 **0.21%**，
   说明不是多端点；再按样本逐格看 CPU tick 增量，E=4 那轮**前 14 格是 +0/+1、最后一格是 +42**——
   客户端进程一退出，它持有的 25,000 条连接同时 FIN，网关收尾 5 万个连接对象的 CPU 落在了"空闲窗口"里。
   修法是把窗口右界钉在客户端还活着的那一刻（`CLIENT_END_TS`，awk 里 `$4+0 <= end+0`，并把右界打进报告行），
   修完 E=4 复跑 **0.21%**，与 E=1 一致、与 B-006 的 0.17% 同带。
   - **防复发的形式**：窗口取"客户端存活期内最后 15 个样本"，汇总行里写 `右界 ts=$CLIENT_END_TS`——
     没有那一行，下一轮污染会被读成"网关在 100k 下变忙了"，而那句话我拿不出第二个数支撑。
   - 这一条与"报告里一片 0 比一片 ? 危险"是同一类：**取数定义本身要能被证据推翻，不能只靠我读数时的印象**。

3. **夹具自己的每连接成本第一次有数，而且它不进被测账**：100k 条隧道下上游进程
   `VmRSS 4,104 → 13,276 KiB`、Δ = **9,172 KiB = 0.09 KiB/连接**、`fds=100,006`、`refused_over_capacity=0`。
   顺手给它加了一个按目的地址的 accept 分桶（`accepted_by_addr=`，只在 `--upgrade` 形态下取——
   SLO #1 那条路径一个 `getsockname` 都不该多），于是报告里"每端点 6,250 条 / 25,000 条"是**实测**的，
   不是 `连接对 ÷ E` 算出来的。同一个理由让我当年去实测 28,232 而不用 `60999−32768` 代替它。
   原始档：`build/bench/probe-upstream-25000/`、`probe-upstream-100000/`（快 ramp，污染轮）、
   `probe-upstream-100000-longramp/`（判别轮）。

### S2-07 新增的四条工具反证（2026-10-29，p999 / 排水档 / 构成表）

1. **观察者造假墙有两种签名，这是第二种：不是 `established` 冻住，而是两条臂都被自己的 CPU 抬高、
   而且抬的幅度不一样。** SLO #1 的 p999 增量第一次算出来了（1000 并发闭环、workers=16：
   直连 p999 3,648 µs、经网关 35,840 µs → 增量 32.192 ms），看着像"网关的尾巴很糟"。
   同一份分布的 **p99 增量是 26.304 ms，而 wrk 那条线在同一档上量到 4.160–6.290 ms——差 6 倍**，
   差的就是仪器。客户端自报栏把它说清了：`self_cpu_ticks ÷ elapsed` = 直连 **0.99 核**、
   经网关 **1.00 核**（单线程 epoll 循环，它自己的排队直接进它记录的时间戳）。
   - **我顺手做了一件该做的事**：闭环数响应体原本是逐字节走状态机，1000 并发下 186 M 次/秒把自己压满；
     改成**整块计数**（判据仍是"读满 `--expect-body-bytes`"，语义一字未改）。改完直连臂跑到 714k–815k
     消息/秒、仍然贴着 1 核——**瓶颈换了名字没消失**，所以那一档"取不到"的结论照记，不写成"修好了"。
     每消息的仪器成本在两臂是 1.2 µs（直连）vs 18.7 µs（经网关），这个不对称本身就是"两臂同仪器 →
     误差相消"这个假设不成立的证据。
   - **防复发的形式**：`s1-p999.sh` 每一臂都打印观察者占单核比例，≥0.9 就打 `⚠ 只可作下界读`；
     而判定必须与**另一台仪器**（wrk 的 p99）交叉对照。低并发（100 条）那一档观察者退到 0.39 核，
     增量 p999 = 5.420 ms（workers=1）/ 3.764 ms（workers=16）能读，但 `canonic=0`，
     **拿它填 #1 就是换口径**，所以只作归因实验。
2. **`drain_tunnels_hard_closed` 这一格第一次不是 0——但"绿"是被压红之后才成立的。**
   `c100k.sh` 加 `DRAIN_UNDER_LOAD=1`（客户端仍持有连接时发 SIGTERM）之后，25k/100k 两档报
   `hard_closed == established`（25,000 / 100,000）、`forced_timeout=0`、`live=0`、
   `constructed==destructed`、fd 回基线，排水 **0.305 s / 0.947 s**（BENCHMARK B-010）。
   阳性验证把兜底压到 `DRAIN_TIMEOUT_MS=1`：同一档 `hard_closed=0`、`forced_timeout=2000`，
   **两条断言同时红、脚本真退出码 1**。所以那句"默认 5000 ms 在 100k 下站得住"是被证伪过一次才成立的。
   - 本档的客户端退出码**按设计是 1**（网关先没了 → 客户端按"收到信号，提前结束"收工，而那个非 0
     正是它自己的"这一轮不进文档"标志）。这条写在脚本与 summary 里，因为它是唯一一处
   "非 0 不等于失败"的地方，不写清楚下一次就会被读成丢负载。发信号那一刻的前提由另一栏负责：
     `at_signal: gateway_fd=200040 gateway_rss_kib=1751032`。
3. **sanitizer 会把堆会计整个弄瞎，而"跳过"不等于"通过"。** 构成表两条新用例在 `asan-ubsan`/`tsan`
   下红成 `0 > 0`：ASan/TSan 换了分配器，`mallinfo2()` 一律报 0（同一段 VmRSS 反而 +12,660 KiB/连接，
   那是影子内存）。修法不是把阈值放宽，而是 `heap_accounting_available()`（编译期宏 **加上** 运行期
   `mallinfo2()>0`）+ `GTEST_SKIP`，打印里明写"数值那一半不判"；结构那一半（容量 4096、scratch 已释放、
   两侧进隧道、三个计数）在任何构建都判。三 preset 因此是 **各 137 条全绿，其中 asan/tsan 有 2 条 Skipped**。
   同一条立场写在 ARCHITECTURE 1.10（`k_rss_is_instrumented`）——但那次我只是绕开，这次是**仪器本身报 0**，
   所以必须显式区分"不判"与"判过"。
4. **等式按单臂比，就会把"共用仪器"读成"上游重复计数"。** 两个 arm 共用同一个 `bench_upstream` 进程，
   而它只在退出时打终局 → 标定那轮报 `客户端 proxy 2,000,000 ≠ 上游 4,000,000`，看起来像上游算重了。
   真相是 4,000,000 = direct 2M + proxy 2M。等式的正确形式写进断言：
   **`direct + proxy == upstream`**，网关一侧另立一条 `requests_done == parsed == completed`
   （两次标定分别闭合在 4,000,000 与 3,500,000）。这条和 S2-06 那条"四条等式"是同一纪律的延伸：
   **测量工具自己要有等式**，而等式必须写清它比的是哪一段。


## 8. S3-01 的阳性验证与反证（2026-10-29，路由那一环）

门禁第 2 条的立场没变：**能红的绿才算数**。这一轮四处故意破坏各自怎么红的，以及**第一次没红的那一处**，
都原样记在这里——最后那条比前三条值钱，因为它证明"用例够不够"这件事我不能写完就相信。

| 我把代码改成什么 | 哪条用例该红 | 实测 |
|---|---|---|
| 摘掉 `pump_response` 在"在飞归零"时补的那一次 `forward_pending_requests` | `S3_01_Routing.PipelinedTargetSwitchIsHeldThenRedriven` | **红，而且是挂死**：102 ms → 5,486 ms，红在第二次 `read_response` 超时上。这就是 ADR-18 第五节那笔代价的收据——不补不是"慢一点"，是那条被压住的请求永远不会有人再叫醒它 |
| 归还时不看连接自己那条目标，一律进默认目标的桶（= 退回 S2 的单池形状） | 期望是 `S3_01_Routing.PoolBucketsKeepTargetsApart` | **第一次跑：两条相关用例全绿**。不是代码没问题，是我的用例只走到"没有后续请求会去碰那个放错桶的条目"那一步。补了第 4 步（两个目标的池各有热连接时再来一条 `/a`）→ 重打同一个补丁 → **红，5,245 ms 挂死在读超时**：放错桶的连接被目标核对认出来、按"要有干净边界才换"压住，而它等不到那个边界。失败形态是"不动"，不是"发错后端"——两样都不接受，但记要按观察到的记 |
| 正则用 `regex_search` 代替 `regex_match`（丢掉全串锚定） | `S3_01_RouteRegex.IsAnchoredNotSubstring` | **红**：`/ap` 命中了 `/api/x`，而期望是没有匹配 |
| 前缀改成"第一条命中就返回"（丢掉最长匹配优先） | `S3_01_RoutePrefix.LongestMatchWinsNotLexicographicOrder` + `EmptyPrefixIsCatchAll` | **两条同时红**。前者把 `/api/v2/users` 送到了 `/ap` 的后端；后者更安静——只比长度而不看 `!found` 的话，空前缀（catch-all）永远选不上，**表里明明有一条兜底规则、请求却得到 404** |

**其余三件与 S3-01 相关的观测面**：

1. **`tsan` preset 164/164，日志里 `ThreadSanitizer` 出现 0 次**。分桶是 S3 新增的第一处并发面（池的归属
   没变，变的是容器形状），所以这条是 S3 门禁硬条件之一。**反证方向我没做真**：要证"跨 worker 借会红"，
   得先把那条路径写出来——今天不假装测过，这句写在 ADR-18 后果段而不是这里。
2. **`asan-ubsan` / `tsan` 各 2 条 Skipped，报"164/164"时必须一起说**（`S2_07_MemoryComposition` 那两条
   堆会计用例：sanitizer 换分配器后 `mallinfo2()` 一律报 0）。**跳过＝不判，不是通过**，与 §7 第 3 条同一条立场。
3. **并行负载暴露出既有用例的一处取样错（不是回归）**：`ctest -j 8` 下
   `S1_04_Reactor.TenThousandSequentialRequestsLeaveNothingAlive` 报 `live_connections=2 ≠ 1`，而单跑 5/5 全绿。
   根因是它看到 `pool_returns==1` 就**立刻**读对象计数，而下游对象要到事件批次末的 `retire_closed()` 才析构
   （六步关闭第 5 步）——那一刻 `live==2` 是合法中间态。改法是把"立刻读"换成 `wait_until(==1)`，
   **期望值一个字没动**。这是 `stats.h` 第 25 行那条"非原子观测只能在循环停下后读"的第 N 次应用，
   同一条也记进了 ARCHITECTURE §5。

**成本那一格（BENCHMARK B-012）不在这张表里**，因为它判的是"有没有可分辨的退化"而不是"能不能红"：
空链 vs 路由链同机交替各 2 轮，每请求 p50/p99/p999 的跨臂差**不超过 A 臂自己的组内极差**，
每请求 `epoll_ctl` 四份落在 4.0000–4.0005 → 判"**无可分辨成本**"；那个名义上 0.4% 的 QPS 优势按
顺序效应记（B 臂在每一对里都是第二个跑的），**不读成路由让网关变快了**。

## 9. S3-02 的阳性验证与三条工具侧发现（2026-10-30，负载均衡与配置文件这一层）

**先说立场**：§8 那条"第一次没红"的教训是这一节的尺子。S3-02 的每条断言都单独打红过一次，
改回即全绿，`grep -rn 'TEMP-POSITIVE' src/ tests/` 现在是 0 条——**没红过的断言我不写进文档**。

| 我把代码改成什么 | 哪条用例该红 | 实测 |
|---|---|---|
| 环查找改成"取环上第一个点"（`return ring.front().second`） | `S3_02_MigrationRate.AddingOneBackendMovesAtMostOneInNewTotal` + 端到端粘住那条 | **两条红**，但**只有 N=3 那档抓住了它**（48.10% → 100.00%）；**N=7 那档读到 0.00%，反而"满足" ≤ 12.5%**。理由是：所有 key 都不再参与选择时，两张环给出的映射处处相同、差集是空集 → **一条只看上界的判据挡不住"哈希根本没生效"**。这条实测把 ADR-19 的判据读法改成"上界 + 散布"一对，记在 ADR-19 追加第二条 |
| 轮询游标钉成常量 0 | `S3_02_RoundRobinCursor.CyclesPerGroupAndEachGroupHasItsOwn`、`S3_02_LoadBalanceFilter.RoundRobinWritesMemberTargetsInOrder`、`S3_02_LoadBalance.RoundRobinCyclesAcrossBackendsOnOneKeepAliveConnection` | **三条同时红**（纯函数、filter、按后端计数各一层）。12 条 ÷ 3 台应当各 4 条，常量游标下变成 12/0/0 |
| `least_conn` 的比较符从 `<` 翻成 `>=` | `S3_02_LoadBalance.LeastConnectionsSendsNewWorkToTheIdleBackend`、`S3_02_LoadBalance.LeastConnectionsReleasesCapacityWhenTheDownstreamGoesAway` | **两条红**：前者把新请求派给正忙的后端、撞 2 s 读超时；后者拿到 64 B（错的那一台）。方向断言不依赖绝对值，所以换机器不会跟着变红 |
| 解析器把"未知字段"从拒绝改成忽略 | `S3_02_ConfigAssembly.RejectsUnknownFieldInAGroup` | **只有这一条红**（其余 29 条照绿）。附带一条好读的性质：`banlace` 那种**键名**拼错被两条独立拒绝同时抓住（不认识的键 + 缺 `balance`），所以那一格不依赖这道门 |
| 解析器把"重复键"从拒绝改成接受 | `S3_02_ConfigJson.RejectsDuplicateKeyInOneObject` | **只有这一条红** |
| 解析器把"空 `targets`"从拒绝改成允许 | `S3_02_ConfigAssembly.RejectsEmptyTargets` | **只有这一条红** |

**发现一：交接段"已验证"那一格只覆盖到 debug，两个 clang preset 当时是编译失败的。**
`src/config/json.h` 用 `std::vector<std::pair<std::string, JsonValue>>` 做递归——`JsonValue`
在自己完整之前就要实例化 `pair`，而 libstdc++ 的 `pair` 要求两边都完整。**GCC 15 放得过、
clang 21 放不过**，所以 `asan-ubsan` 与 `tsan` 两个 preset 从我接手到修好之前是 `build exit=2`。
改法是类内声明 `struct Member;`、类外定义（`std::vector<Member>` 走的是 C++17 P0646 给 vector
开的那条"元素类型可以暂不完整"的路），两个编译器从此给同一个答案。
**为什么这件事记在故障注入文档里**：它是一次构建侧的静默失效——同一份代码在一个 preset 上绿、
在另外两个上根本没编，而当时的记录写的是"已验证"。

**发现二：`verify-all-presets.sh` 在构建失败时不响，它会拿旧二进制继续跑 ctest 并报通过。**
上面那两个 preset 报的是 `164/164`（S3-01 时代的旧产物），而真实用例数当时已经是 189。
脚本的形状是 `cmake --build ... > log` → 打一行 `build exit=$?` → **无条件**接着 `ctest --preset`。
`build exit=2` 那一行确实打出来了，但它不参与任何判定，红就这样被吞掉。
**这一格我没改脚本**（改它等于改门禁工具本身，而且它今天还在给我有用的三行输出），先把纪律记下来：
**看到三行 `tests passed` 不等于三个 preset 都构建成功——门禁的计数要看，门禁的退出码也要看**。
这是纪律六（警告要在三个 preset 分别看）的同号版本。

**发现三（旧账，B-013 的定标就是被它逼出来的）：B-012 那四轮的落盘自带 7 条 ✗。**
`build/bench/s3-01-cost-a1.driver.log` 里有 `requests_done=20594212 ≠ planned=650000000`、
`等式不闭合`，以及脚本自己那句 `本轮有 ✗，上面的数字不要用`。根因不是网关、也不是脚本坏：
`REQUESTS_DIRECT=6500000` 是**每条连接**的配额，100 条连接就是 650M 条请求，而
`l7gate_c100k` 有自己的总预算（约 120 s），在吃完配额之前就停了 —— 于是
`requests_done == requests_planned` 这道门**在这组参数下永远合不上**。B-012 的读数来自一个
119,998 ms 的真稳态窗口，但那一轮被工具自己判成"没资格进文档"。
**处置**：B-013 的配额按"窗口内真吃得完"定标（直连 100k/conn、经网关 18k/conn、稳态门 ≥50 s），
六轮全部 `rc=0`、✗ 计数 0；**B-012 要不要按同一定标复测是项目所有人的裁决**，我不回头改已记的数。

## 10. S3-03 的阳性验证（2026-10-30，健康检查这一层）

**先说立场**：§8/§9 的尺子原样用——每条断言单独打红过一次，改回即全绿，
`grep -rn 'TEMP-POSITIVE' src/ tests/` 是 0 条。

| 我把代码改成什么 | 哪条用例该红 | 实测 |
|---|---|---|
| `least_conn` 平手分支改回"取成员顺序第一条"（ADR-19 追加三的旧规则） | `S3_03_LeastConnTie.FullTieRotatesWithinTheTiedSet` + 两条 `S3_02_LoadBalance.LeastConnections*` 端到端 | **三条红**。纯函数那条是 B-013 那条落盘今天缺的断言（平手集合内不轮转 → 12 条全落成员 0）；端到端两条红在"第二条没有配对到轮询该给的下一个" |
| `candidates_for` 直接回全员（摘除对候选集不生效） | `S3_03_Candidates.RoundRobinRotatesOnlyWithinCandidates`、`...LeastConnSkipsEjectedEvenWhenIdlest` | **两条红**（被摘除的成员照常接到流量 / "最闲但被摘"的成员被选中）。`AllEjectedFallsBackToFullMemberSet` 照绿——它证的是回退路径，本就不经过候选集过滤 |
| 探活判据把 4xx 判死（`status < 400`） | `S3_03_HealthE2E.ProbesKeepA404BackendAliveWithoutTouchingBusinessCounters` | **一条红**：固定回 404 的后端在 3 s 内被摘——"一次路径配错引发全量摘除"就是这行代码写的 |
| 安全阀检查短路（`if (false && ejected_now >= …)`） | `S3_03_Health.ValveCapsEjectionAtConfiguredPercent` + `S3_03_HealthE2E.ValveCapsEjectionWhenEveryBackendFails` | **两条红**：纯函数那条 10 台全摘（期望恰好 1），e2e 那条 `ejected_now=2` 越过 floor(2×50%) |
| 5xx 失败不记账（条件恒假 → 全记成功） | `S3_03_HealthE2E.PassiveFiveXxEjectsAndFailTimeoutRevives` | **一条红**：两次 503 后没有被摘，被动判据形同虚设 |

**工具侧发现（照记，防复发）**：打红之后"改回"这一步我自己错了两次——还原的 `sed` 只删掉了
`// TEMP-POSITIVE` 注释、没还原被改的条件（`status < 400` 与 `if (false && …)` 原样留在代码里），
全套 246 条里就有两条持续红。**"删掉标记"不等于"还原了改动"**：临时改动的还原必须以
`git diff`（或对拍原句）为准，不是以标记注释消失为准。这条与 §8 的"阳性验证要真打红"是
同一族纪律的另一半：红要红得真，绿也要绿得真。

## 11. S3-04 的阳性验证与工具侧发现（2026-10-30，节点表 reload 这一层）

**先说立场**：§8/§9/§10 的尺子原样用——每条断言单独打红过一次，改回即全绿，
`grep -rn 'TEMP-POSITIVE' src/ tests/` 是 0 条。

| 我把代码改成什么 | 哪条用例该红 | 实测 |
|---|---|---|
| `LoadBalanceFilter` 忽略发布器（`if (false && nodes_ != nullptr)`，永远用构造给的文件表） | `S3_04_NodesE2E.ReloadAddsRemovesAndReAddsMembersWithoutRestart` | **红**：新成员 50 轮内接不到流量——reload 生效的请求侧入口就是这次 load |
| `Reactor::sync_nodes_snapshot()` 开头短路（对账 tick 与 ensure_upstream 懒对账一起断） | 同上 | **红**：新成员永远进不了 worker 的目标表，请求全部压在下游缓冲 |
| `build_nodes_view` 不拒绝 fetch 里文件没有的组 | `S3_04_Nodes.UnknownGroupNameIsRejectedWholesale` | **红**：ghost 组被安静吞掉而不是整批拒绝——数据库偷偷扩配置面 |
| `source` 与 `targets` 互斥检查跳过报错话（保留拒绝但话为空） | `S3_04_ConfigNodes.SourceAndTargetsAreMutuallyExclusive` | **红**：拒绝本身还在，但话里没点名——那条用例断的就是"点名"这一半 |

**工具侧发现（照记，防复发）**：① 冒烟脚本第一版的 DELETE 判定没有给 reload 生效宽限——
DELETE 后第一拍 curl 撞上旧快照的尾巴，"还没生效"被读成了"没生效"，两格假红；
给一个完整 reload 周期的宽限后同一路径全绿。**判定窗口必须覆盖被测机制的生效粒度**——
这条与 §8"阳性验证要真打红"是同一族：红要红得真，绿也要绿得真。② 收尾信号循环写成
`${PIDS[@]:1}` 漏掉 1 号上游，脚本 `wait` 一个永不退出的进程——bash 数组切片从 0 起，
`:1` 不是"跳过网关"是"跳过第一个成员"。

## 12. S3 的故障注入矩阵第一轮（2026-10-30，S3-05，ADR-13：全部应用层注入）

**这一节在问什么**：S3-03/04 把健康检查的机制造出来了（探活、被动记账、摘除、恢复、安全阀），
但"五类真实故障各自的摘除/恢复耗时"还没有系统量过。这一轮把五类故障逐一注入真进程
（release 网关 + 三台测试上游 + curl），每类给出预期 vs 实测。脚本 `scripts/fault/s3-05-matrix.sh`，
落盘 `build/fault/s3-05/`（含每类逐请求 trace）。

**统一配置**：3 成员 `round_robin` + catch-all，`max_fails=2`、`fail_timeout=60s`
（恢复只走探活成功这条路，计时干净）、`probe_interval=100ms`、`probe_timeout=300ms`、
阀 50%；成员 2（128 B）是被注入方，成员 2 的 `health_path=/probe`（类 5 需要）。
摘除判定 = 注入（T0）后**连续 20 条请求不见 128**；恢复判定 = 128 首次再现。

### 五类故障的预期 vs 实测

| 类 | 注入方式（应用层） | 预期 | 实测摘除 / 恢复 | 断言 |
|---|---|---|---|---|
| 1 connect refused | `kill -9` 上游进程 | 探活 connect 立即失败 → 摘除 ≈ max_fails×间隔 ≈ 200–600ms；恢复 ≤ 间隔+调度 | **273 ms / 89 ms** | ✓ |
| 2 后端 5xx | 上游所有路径回 503 | 探活死 + 业务被动 503 同源记账；量级同类 1 | **324 ms / 129 ms** | ✓ |
| 3 半开（连上不回字节） | `--stall-reads-ms 15000` | 每次探活吃满 300ms 超时 → 摘除 ≥ 2×(300+100)≈800ms；恢复 = stall 到期后第一拍探活成功 | **5280 ms / 119 ms**（归因见下） | ✓ |
| 4a 慢过探活超时 | `--delay-ms 600`（>300） | 探活超时判死 → 摘；**业务其实能通**——timeout 判据的固有取舍，"摘的是探活" | **2108 ms / —** | ✓ |
| 4b 慢在探活超时内 | `--delay-ms 100`（<300） | **不摘**：慢与死的分界线就是 probe_timeout，线内网关不表态（判断归 S4 的请求超时） | 3 秒内 128 仍在轮换，摘除 0 | ✓ |
| 5 探活失败业务正常 | `--path-status "/probe:503"`（业务 200） | 业务 200 也留不住它：探活是成员资格的唯一真源 | **359 ms / 90 ms**；摘除后直连该成员业务路径仍 200（判死的是探活，不是机器） | ✓ |

**类 3/4a 的摘除读数归因（不藏）**：5280/2108 ms 远大于探活侧的理论摘除时刻（≈0.8–1s，
网关自报的摘除计数在各自窗口内都 +1）。多出来的部分是**测量定义的成分**：摘除前的最后几条
业务请求挂在半开/慢成员上得不到响应，被 curl 的 5s max-time 逐条排空，而"连续 20 条不见
128"的判定要等这些在途请求排空——**排空时间主导了读数**。类 1/2/5 快是因为 connect
refused/5xx 立即失败、没有在途悬挂。两个读数的诚实表述：**探活侧摘除 ≤1s 量级；客户端视角
的"不再见到该成员"由在途请求的排空决定**。S3-06 的每请求成本对照与 S4 的请求超时落地后，
这一格值得用更细的口径重测（把"摘除完成"与"在途排空"分开计时）。

**恢复语义的全表一致性**：五类的恢复都只走"探活成功"（fail_timeout 钉在 60s 挡住到期
恢复的干扰），实测 89–129 ms——恢复耗时 ≈ 一个探活间隔 + 调度，与预期一致。

### 工具侧发现（照记，防复发）

1. **bash 数组在 here-string 里拼成一行**：`sed -n '2p' <<< "${PIDS[@]}"` 取不到第 2 个元素
   （整个数组是一行），`kill -9` 落空——症状是"成员杀不死 + 重启撞 EADDRINUSE"。
   教训：**数组元素用 `${arr[i]}` 或专用变量取，不用文本工具从展开结果里截**。
2. **kill -9 后立刻重绑同一端口可能撞 EADDRINUSE**：被杀进程的 socket 由内核代收，
   FIN_WAIT 系状态连 SO_REUSEADDR 都绕不开，要等对端（网关）把 EOF 处理完。`ss` 取证钉住
   了持有者（`bind-fail.log`）；脚本侧退避重试（100ms × 50）解决，不当成故障。
   最小复现（无网关在场）首次重试即成功——**在途连接越多，重绑窗口越长**。
3. **`set -u` 下的 `$i` 笔误**（`$try` 写成 `$i`）让重试循环第一次就崩——取证脚本自己也
   要按它要证明的纪律写。

---

## 13. 回源耗尽表态（S4-02，ADR-22）：S2-06 那 71,769 条旧债结账

**这一节在问什么**：S2-06 的 100k 名义档（`UPSTREAM_ENDPOINTS=1`，回源容量 28,232）实测
`downstream_accepted=100000` 而 `established=28231`，其余 **71,769 条请求在回源 connect
失败后留在下游缓冲里一言不发**，客户端 30–120 秒后以 `handshake-timeout` 收场。S4-02
把它们改成"connect 失败 → 自产 502 / 被摘尽 → 503 / 本地预算耗尽 → 503"（ADR-22）。
这一节记录**同场景重跑**，以及重跑过程中它替 S4-01 抓到的两个产品缺陷。

### 13.1 有效轮：71,769 条全部拿到完整 502（`build/bench/replay-b006-s402b/`）

命令与 `c100k.sh` 的 B-006 重放档同参，只把 ramp 拉到 S2-06a 判别实验用过的慢档
（833 条/秒那一档撞的是**观察者假墙**，见 13.3）：

```
CONNECTIONS=100000 TARGETS=4 UPSTREAM_ENDPOINTS=1 RAMP_MS=300000 HANDSHAKE_TIMEOUT_MS=120000 \
  OUT_DIR=build/bench/replay-b006-s402b bash scripts/bench/c100k.sh
```

| 观察点 | 旧形状（S2-06 名义档） | 本轮（S4-02 之后） |
|---|---|---|
| 客户端 `established` | 28,231 | **28,231**（同一堵容量墙） |
| 客户端 `not_101` | 0 | **71,769**（每条失败连接都收到完整 502 头） |
| `failure reason=handshake-timeout` | 71,769 条全部 | **0 条**（终局行之外没有任何 failure reason 行） |
| 客户端 `failed` | 71,769（全 timeout） | 71,769（**全 not_101**） |
| 网关 `gateway_replies_sent` | 0 | **71,769** |
| 网关 `gateway_replies_connect_failed` | —（该格当时不存在） | **71,769** |
| `gateway_replies_all_ejected` / `_over_capacity` | — | 0 / 0（本轮没有健康表、没撞 fd 预算） |
| `upstream_connect_failed` | 75,708 | **71,769** |
| `upstream_connect_timeout` / `head` / `body_idle` | — | **0 / 0 / 0**（没有一条走"等超时"） |
| `upgrades_101` / `upstream_opened` | 28,231 / 28,231 | 28,231 / 28,231 |
| 上游夹具 `accepted` / `peak_clients` | 28,231 / — | 28,231 / **28,231**（无 churn，隧道没被误杀） |
| 生命周期 | — | `constructed==destructed==128231`、`live=0`、fd 41→41 |
| 客户端观察者 | — | `self_cpu_ticks=19,678` / `elapsed=510,020 ms` ≈ 单核 3.9%——观察者不是瓶颈 |

**判读**：容量墙一个字节没动（28,231 vs 28,231），变的是墙前那 71,769 条的**结局**——
从"挂到客户端放弃"变成"一条完整 502"。`not_101` 是客户端自己数的"头收齐但不是 101"，
与网关 `gateway_replies_connect_failed=71769` 逐数相等；三种超时计数全 0 说明这些 502
不是等出来的。单条的上限由 ctest 钉着（`RefusedUpstreamReplies502AndCountsIt` ≤2s、
`EveryDeadUpstreamRequestGetsIts502WithinFiveSeconds` 64 条批量 <5s）。

**脚本自己的 ✗ 照记（它们为 E=4 正式档设计）**：前置容量门（"每个回源端点要扛 100000 >
28232"与"计划 100000 > 本轮容量 28232"）、`upstream_connect_failed≠0` 门、客户端
`established≠planned` 门全亮——本轮引用的是**表态计数**，不是每连接内存/空闲 CPU；
那两格本来就 `measurable=0`。`c100k_rc=1` 是这个原因，不是回归。

**首轮作废档（`build/bench/replay-b006-s402/`）为什么留着**：833 条/秒的跑法下客户端
`established=72268 / failed=27732 全 handshake-timeout / not_101=0`——观察者假墙（S2-06a
已登记的形状），**同时**它暴露了 13.2 的隧道缺陷：网关侧 `upstream_head_timeout=100000`、
`gateway_replies_sent=100000`，上游 `peak_clients=8390`。两件事叠在一起，那一轮的容量
与表态读数都不可解释，作废；留着是为了让"缺陷是怎么被逼出来的"有据可查。

### 13.2 同轮抓到的三个缺陷（都不是 S4-02 新写的代码，是门禁的价值）

| # | 缺陷 | 实测形态 | 处置 |
|---|---|---|---|
| 1 | **101 之后响应头限期没有撤销**（S4-01） | E=1 首轮：`upstream_head_timeout=100000`、`gateway_replies_sent=100000`、上游 `peak_clients=8390`——每条空闲隧道 10 秒后被判 504 杀掉、重开，B-007/B-010 那种"空闲 100k 隧道"的口径不成立 | `maybe_enter_tunnel` 里撤销 `erase_upstream_deadline`；回归用例 `HeadTimeoutIsCancelledWhenTheTunnelStarts`（阳性验证：注释掉那行 → 当场红） |
| 2 | **504 的 `Content-Length` 写成 15、body 是 16 字节**（S4-01） | 把 504 补进 `GatewayRepliesAreCompleteAndSelfConsistent` 的逐条核对循环后当场红（S4-01 的用例只断状态行） | `Content-Length: 16`；504 进那条表 |
| 3 | **默认 `drain=5000` 违反 `drain ≥ head=10000` 启动校验**（S4-01） | 无参数启动退出码 2，六条冒烟全部拒绝启动；S4-01 行记的"五冒烟全绿"因此不成立 | 按 S2-05 裁定"要么调大"把默认 drain 抬到 10000；`smoke-s2-05.sh` 阶段三（显式 800）同传 `--upstream-head-timeout-ms 0`；S4-01 行就地更正 |

另有一条**门禁工具**缺陷：`verify-all-presets.sh` 的退出码被管道吞掉（tsan 全量 1 条失败
时脚本仍报 rc=0），且 build 失败仍拿旧二进制跑 ctest（原待裁⑥）。S4-02 已修：任一
configure/build/ctest 失败 → 脚本 rc=1，build 失败跳过该 preset 的 ctest。

### 13.3 一次瞬态，照记

tsan 全量在 `-j16` 下首次跑红了一条：`S2_03_CrossThreadDestruction.ConnectDisconnectStorm...`
`ok=478/480`（2 条客户端读超时），单跑 **5/5 绿（60–119 ms）**、随后全量**连跑三遍
269/269 绿**。归因为并行 ctest 的资源竞争（16 个 tsan 进程 + 本套件新增的 64 客户端/700ms
用例），不是产品缺陷；不据此改断言，也不据此说"全绿"——按"首红一次、复跑三绿"原样记。

### 13.4 这一轮的门禁面

三 preset 各 **269/269**、非 gtest 警告 **0 条**、tsan 日志 `ThreadSanitizer` 0 次；六条冒烟
（含需要 Docker 的 `smoke-s3-04`，MySQL 8.0.36 六阶段）`rc=0`、✗ 计数 0。

## 14. Redis 分布式计数的故障注入（2026-11-02，S4-04，ADR-05 落地追加节）

这一段注入的对象是**我新引入的那个外部依赖**：`redis:7.2.5` 容器（`127.0.0.1:16379` 端口发布，
cpuset 绑在 15 号核）。判据的形状与 S3-04 那一轮一致——**辅助依赖不能变成全局故障域**，但它一旦
参与决策，它不在的时候必须**有可断言的行为**，而不是"看起来还在跑"。

### 14.1 预期 vs 实测（五格，全部应用层注入，ADR-13）

| 注入 | 预期（ADR-05 正文那三条降级链） | 实测 |
|---|---|---|
| `docker stop l7gate-redis`（Redis 挂） | 拒绝能力不消失：限流退回 S4-03 的本地桶语义，既不放到无限也不全站 503；降级**有账**、不静默 | 同一台网关六连击仍是 **`200 200 200 429 429 429`**、`rate_limit_allowed/rejected=3/3`、`redis_settles_failed=7`（挂着期间每拍都有账）、终局 `live=0`、`constructed==destructed==7`、`fd 9→9` |
| 把服务端那一格冻成 0（`HSET <key> tokens 0 last=很远的将来`；脚本里 `elapsed<0` 夹成 0 → 补货恒为 0） | 授权必须**压过**本地补货：本地桶明明还有余粮也要少放，且这一格要单独计数 | `rate=1000 burst=1000` 下：冻格后**第二批** 5 条全 **`429`**、`rate_limit_capped_rejects=5`、`redis_settle_commands=4`；`HSET … last 0` 解冻后又 5 条全 `200`。**这一格同时就是"分布式不是装饰"那条断言** |
| `docker start l7gate-redis`（Redis 回） | 不需要任何人重启网关：下一次成功结算自己摘掉降级态、夹子重新按授权收紧 | 同一台进程 `redis_degraded` 从 1 回到 **0**、`redis_settles_ok=9`；换新路径再冻一次 → 5 条全 `429`、恢复那臂 `rate_limit_capped_rejects=5`。**"通路活着回来"由行为证明，不由仪器读数证明** |
| 服务端脚本不在（夹具回一次 `-ERR NOSCRIPT`；ctest 侧 `S4_04_Settler.NoscriptReloadsTheScriptAndAsksOnceMore`） | 重 `SCRIPT LOAD` 一次、**原样重试一次**；整拍不判失败、不进降级计数 | `script` 命令数 ≥2（初次 LOAD + 重 LOAD）、`redis_settles_failed=0`、`degraded=false`、授权照常落进夹子 |
| 命令半途被掐（夹具持续 refuse EVALSHA） | **上报值只在命令成功后推进**，否则那一笔消耗被丢掉 → Redis 多授权，那才是不安全的方向 | `last_eval_used` 在失败前后都是 **"3"**（同一笔被重报），成功之后不再重复上报（其后 350 ms 内 EVALSHA 条数不增长） |

一处**方向性的自我更正**要留在这里：我在 ADR 草稿里写过"夹子只会咬紧不会放松"。落地时发现这句只对
**夹子的写入**成立——上报用的是快照那一刻的 `used_total`，从快照到命令发出之间放行的条数这一拍不报，
于是服务端余额被**暂时高估**，上界 = 该键在一个结算周期内的实际放行数（最坏 `burst` 条），下一拍扣回。
所以多副本下的口径是"每个周期内的超额有界"，不是"任意时刻严格相等"。ADR-05 第二节与 ARCHITECTURE
1.30 的边界①已按这条改写，**上面五格的实测一个字不回头改**。

### 14.2 四处阳性验证（能红才叫绿）

| 改坏的那一处 | 红掉的用例 |
|---|---|
| 夹子不生效（`usable = tokens`） | 4 条：`CapLensRejectsButDoesNotSpendTheBucket`、`OverflowBucketAndReleaseAreProcessWide`、`GrantOfZeroBitesAndTheRejectionItCausesRefreshesIt`、`FailureOpensTheCapAndRecoversReclampingIt` |
| 先记账后发命令 | **恰好 1 条**：`FailedReportIsRetriedWithTheSameUsageNextTick`——隔离干净，这条断言只守这一件事 |
| 取消"只结有需求的桶"（每拍全表 EVALSHA） | 3 条：`IdleBucketsCostNoCommands`、`DemandedBucketIsSettled…`、`FailedReportIsRetried…` |
| `set_cap_milli` 不夹 `burst` | 3 条：`GrantAboveBurstCannotAdmitMore`、`DemandedBucketIsSettled…`、`Noscript…` |

还原用精确替换（不用 `sed`），还原后 `grep -r TEMP-POSITIVE src/ tests/ tools/` = **0**，再跑全量。

### 14.3 工具侧发现（四条，每条都留了"防复发的第三条"）

1. **`$(start_gateway …)` 起的进程不是本 shell 的孩子，`wait "$pid"` 当场返回"not a child"**。
   后果不是报错而是**一路假绿**：脚本在它还没退出时就去读退出账（读回空串），再拿同一个端口起下一臂
   （`EADDRINUSE`），于是后面每一臂测的都是第一臂那个还活着的进程。防复发：`stop_pid` 改成轮询
   `kill -0` 直到真退出，且每次启动前 `port_busy … && die`——端口不空就不跑，不带着脏状态往下测。
2. **一行多键 + 行首锚 = 四格静默读空**：`sed -n "s/^\[serve\] key=\(\[^ ]*\).*/\1/p"` 只命得中行首
   那一个键，其余返回空串，而**空串在判定里长得像"0"**（本轮 `capped_rejects=''` 就差点被判成 0）。
   防复发：退出账**一行一个键**（本来就是"统计每行一个 [serve] key=value，给脚本读"那条口径），
   且 `get_stat` 改成 `grep -o "[[:space:]]key="`，不再假设键在行首。
3. **夹具自己的两个错**：Redis 命令名大小写不敏感，产品发的是 `SCRIPT LOAD`，子命令按字面小写比会
   回成 `-ERR unknown command`——被测方看到的是"服务端拒绝"而不是我解析错；以及一次性故障标志
   （NOSCRIPT）必须**只对目标命令生效**，否则被同拍里的 PING 消化掉，故障根本没打到要测的那一格。
   防复发：夹具两处比较都按小写、且 `cmd == "evalsha"` 才消化标志；`set_refuse_evalsha(bool)` 用
   **持续**故障而不是"下一条"——降级态的断言要能在稳态下读，只耍赖一次的话下一拍就恢复了，
   `degraded` 落在哪一侧全看采样时机，那种用例绿了也不说明什么。
4. **`cd X && a` 之后的换行不继承 `cd`**：我那条手测命令的第二行在原 CWD 里找 `build/debug/l7gate`，
   报"No such file or directory"。防复发：复杂/带 `$` 的命令一律先写成 `/home/archer/*.sh` 再跑
   （交接段那条口径的同族，本轮又应验一次）。

### 14.4 这一轮的门禁面

三 preset 各 **291/291**（+11）、非 gtest 警告各 **0 条**、`tsan` 日志 `ThreadSanitizer` **0 次**
（新增并发面 = 一条结算线程 + 每桶两个原子；六条 settler 用例在 asan/tsan 两个 preset 下都跑）；
七条冒烟 `rc=0` 且 ✗ 计数 **0**（新增 `scripts/smoke-s4-04.sh` 六阶段，需 Docker；同批的
`smoke-s3-04` MySQL 也在位）。原始落盘在 `build/smoke-s4-04/`：探针日志、每臂网关日志与退出账。

## 15. 熔断补全的故障注入与阳性验证（2026-11-02，S4-05，ADR-12 追加二）

**注入形态全部是应用层的**（ADR-13）：坏的那台后端用 `l7gate_test_upstream --path-status /boom:503`
固定回 503，好的那台不设前缀、同样打 `/boom` 时回 200——"两台一好一坏"这个形状不需要 kill 进程、
不需要 `tc`，也不需要网关配合。恢复那一臂更省事：`/boom` 回 503、`/ok2` 走默认分支回 200，
所以"故障消退"是由**换一条路径**表达的，而不是中途改上游的配置。

**三条臂全部跑在 `probe_interval_ms: 0`（纯被动）下**，这不是偷懒而是 ADR-12 追加二第七节那条
已知弱点的直接后果：主动探活判活仍是一条"立即全量恢复"的出口，探活开着就会把半开绕过——
半开的取证必须在没有它的世界里做。

### 15.1 预期 vs 实测（`scripts/smoke-s4-05.sh` 六阶段，全部 rc=0 且 ✗=0）

| 阶段 | 预期（写成可断言的句子） | 实测 |
|---|---|---|
| 1 错误率摘除 | `max_fails=99` 那条够不着，所以摘除只能来自窗口；摘除之后它一条流量也接不到 | 24 条 `/boom` → `trips_rate=1`、`ejections=1`、健康那台 `ejections=0`；**坏的那台自报只收到 6 条**（样本下界 `minimum_requests=6`），其余 **18 条**全由健康那台接走；`half_open_admitted=0`（`fail_timeout=60s` 内没到期）；`blocked_by_valve=0` |
| 2 半开限流 + 立即回摘 | 到期只放 `B=2` 条；半开期内失败**一次即回摘**、不等 `max_fails`；每周期最多消耗 B 条 | 20 条 `/boom` ×120ms（跨 4 个 600ms 周期）→ `reopened=2`、`ejections=3`、`admitted=2`、`recovered=0`；**后端自报 6 条 == 摘除前 4 条 + 门放行 2 条**（逐条对上）；`admitted(2) ≤ reopened(2)+B(2)`；其余 14 条由健康那台接走 |
| 3 恢复时间线 | 到期→半开→攒满 `K=2` 条业务成功→全量；转正后它接得到流量 | 12 条 `/boom` 摘除（它自报 4 条 503，第 5 条起 503 不再出现）→ 等 800ms → 3 条 `/ok2`：`admitted=2`、`recovered=1`、`reopened=0`、`circuit_half_open_now=0`；**恢复后它自报接到 5 条 200**；**时间线两端钉在同一根墙钟轴**：摘除自报 `1793598369438ms` → 首次探测发出在其后 **832 ms**，转正在那 2 条探测之内 |
| 4 矛盾拒绝 | `circuit` 没 `health` → 退出码 2 且话里**同时点名两个段**；`K>B` 同理；越界值点名 | 三条全部 rc=2 且点名 ✓（`circuit`+`health`；`half_open_successes`+`half_open_requests`；`window_ms`） |
| 5 示例配置跑通 | `deploy/gateway-circuit.json`（生产形状：`max_fails=3`、探活 1s、`percent=10`、`minimum_requests=20`）能直接起、两条判据各管各的 | 六个数原样进横幅 ✓；6 条 `/boom` 里窗口只攒到 3 条样本 < 20 → **`trips_rate=0`**（体积下界）；而连续计数那条确实成立、却被安全阀挡下：**`ejections=0` 且 `blocked_by_valve=1`**——`floor(2×10/100)=0`，两台组想要摘除能力就得调比例（ADR-12 那条 floor 读法在真配置上的样子） |
| 6 生命周期 | 熔断这条链不留持有者 | `live_connections=0`、`constructed==destructed==23`、`fd 9→9` |

### 15.2 八处阳性验证（能红才叫绿）

每条都是"把实现退化成另一种看似合理的样子"，红数取的是**全量 318 条**那一轮（`grep TEMP-POSITIVE`
= 0 之后复跑，还原后 318/318）：

| # | 退化实现 | 红数 | 说明 |
|---|---|---|---|
| ① | 错误率判据分支短路（只留连续失败） | **10** | 6 条状态表 + 2 条窗口来源用例 + 4 条端到端全红——这一条红的最多，正是"S4-05 的主命题" |
| ② | 半开门不夹预算（到期即全量放行） | **3** | `ExpiryEntersHalfOpen…` + 两条候选集用例；端到端那两条不红是**结构性的**：`B=2、K=2` 时前两条本来就该被放行，转正之后也不再受门管——预算是否夹住由候选集那两条断，不由 e2e 断 |
| ③ | 业务成功不促成全量恢复（半开锁死） | **4** | 含 `ProbeEvidenceAloneNeverCloses…`（探活证据不算数那一半也在这条的保护范围里） |
| ④ | 到期不迁移半开（= S3-03 的旧语义） | **10** | 读侧与 `sweep_expired` 两处一起短路；红的正是"旧语义的用例要按新语义改写"那一批 |
| ⑤ | 摘除时不清窗口 | **1**（**第一版是 0**） | 见 15.3 第一条——这条红数的意义不在数字，在于它一开始没红 |
| ⑥ | 探活样本进窗口（仪器写进被测方） | **1** | `ProbeFailuresFeedStrikesButNeverTheWindow` 单独抓住；这条用例是**因为先做了⑥才发现需要它**（原本没有覆盖"探活不污染分母"的断言） |
| ⑦ | 进半开就把安全阀的位退还 | **7** | 含端到端那一臂（`ejected_now` 的口径一变，恢复窗口就露出全量放行） |
| ⑧ | 探活判活仍然立即全量恢复 | **3** | 第七节那条"明知而留着的弱点"如果被顺手改掉，红的就是这三条——留着的理由也正是它们 |

### 15.3 用例侧与工具侧的发现（四条，都是"看起来在测、其实测的是别的东西"）

1. **⑤ 第一版 0 红——是我的用例压不住桶边界，不是实现有罪。** 原写法是"摘除 → 等半开恢复 →
   再打一条失败 → 断不许立刻再摘"，时间线跨了 2000ms 而窗口只配了 1000ms：那些旧失败**本来就
   该被自然滑窗清掉**，所以把 `window_clear` 短路掉也照样绿。改成把整条时间线压在**同一个桶内**
   （`window_ms=100'000` → `bucket_ms=12'500`，全程 `t<1200`）之后，短路才当场红。
   教训与 ADR-12 追加一、ADR-19 追加二同族：**判据断的是"窗口内"，用例就必须把窗口边界钉住**，
   否则它断的是"时间过去了"。
2. **夹具的 `requests_served()` 不数固定状态码那条分支**，于是"被摘除的后端还在接流量"这一句
   第一版是 `0 == 0` 恒真（口径④"一片 0 比一片 ? 危险"在**仪器**这一侧的形态：仪器看不见那个事件，
   读数就永远支持断言）。补一条独立计数 `path_status_responses_served()` 与汇总行
   `固定状态码响应数=`，`已服务请求数=` 的老口径原样不动（S4_02 有一条用例的注释按它写着，
   我不回头改已记的数）。
3. **这台机器的 `date +%s%3N` 不认 `%3N`**：它把 `%s` 与九位 `%N` 连着打出来，拿它当毫秒做减法
   得到一个 19 位的怪物（`1793596374351834311ms`），而且**不会报错**。改 `date +%s%N | cut -c1-13`，
   并给这个差补一道区间断言（600..20000ms）——时间轴的单位错了必须自己响，不能靠读数看起来怪。
4. **`echo "== 阶段 5：\`deploy/gateway-circuit.json\` …"` 里的反引号被当命令替换**：脚本去
   `exec` 了那个 json（`Permission denied`），而日志里那个名字直接消失。横幅文案里的文件名不要用
   反引号包，用直写；同类风险在 here-doc 未加引号时更大（口径⑥那一族的又一变体）。

### 15.4 这一轮的门禁面

三 preset 各 **318/318**（+27：15 状态表 + 2 候选集 + 6 配置面 + 4 端到端）、非 gtest 警告各 **0 条**、
`tsan` 日志 `ThreadSanitizer` **0 次**（新增并发面 = 每 target 一把窗口 mutex + 状态字与半开周期两个
原子；本轮 tsan 全量**首跑即 318/318**，S2_03 那条 churn 瞬态未复现，记"本轮未复现"而不是"已消失"）、
**八条冒烟** `rc=0` 且 ✗ 计数 **0**（新增 `scripts/smoke-s4-05.sh`，**不需要 Docker**）。
每请求成本（成功侧一次 mutex 内的整数加、失败侧加 8 格滚动和与判据比较、半开门只在有半开成员时参与）
归 **S4-08** 对 B-008 的 8.7 µs 与 B-016 的 p99 两把尺一起报。原始落盘在 `build/smoke-s4-05/`
（每臂网关日志、退出账与两份上游日志）。

---

## 16. connect 失败重试的故障注入与阳性验证（2026-11-02，S4-06，ADR-23）

**这一节在问什么**：重试是一个**会自己制造第二次请求**的功能，所以它的全部验收压在两件事上——
放大倍数是不是被预算夹住了（上界），以及它是不是**真的**发生过、而且落到了另一台（下界）。
只看上界的话"从不重试"的实现同样满足；这条形状我在 ADR-19 追加二（迁移率）与 ADR-12 追加二
第八节（熔断四条）各吃过一次教训，本轮是第三次照它写。

### 16.1 预期 vs 实测（`scripts/smoke-s4-06.sh` 五阶段，全部 rc=0 且 ✗=0）

注入手段全是应用层的（ADR-13）：**"死成员"= 那个端口上有进程被 stop_and_join 关掉之后不再有人监听**，
于是内核回 RST → connect 被拒。脚本启动前先断言这五个端口**都空着**——两个"要死的端口"没人监听
这件事就是注入本身，被占用等于这一臂的注入不成立。

| 臂 | 注入 | 预期 | 实测 | 断言 |
|---|---|---|---|---|
| 1 一死一活（round_robin，20 条，无 health 段） | 目标 0 拒绝连接、目标 1 正常 | 每条首试被拒的请求被另一台接走；`retries ≈ 请求数/2`；**后端实收 == 客户端请求数**（一条都没被复制）；客户端零 502 | **20 条全 200**、`upstream_retries=10`、`upstream_connect_failed=10`、`gateway_replies_sent=0`、`after_retry=0`；活的那台**自报 20 条 == 客户端 20 条**；尝试数 **30 : 20 = 放大 1.50**（上界 2.00）；整臂耗时 157 ms | ✓ |
| 2 两台全死（10 条） | 两个端口都没人监听 | 每条恰好花掉一次预算（`retries == 请求数`，级联会读到 >请求数）；重试用掉之后**仍只有一条表态** | 10 条全 **502**、`retries=10`、`upstream_connect_failed=20 == 请求数 ×(1+预算)`、`gateway_replies_sent=10`、`connect_failed格=10`、`after_retry=10`；放大 **20 : 10 = 2.00（顶格）** | ✓ |
| 3 两台都活（12 条） | 无 | 一条都不重试 | `retries=0`、`upstream_connect_failed=0`、两台自报 **6/6**、合计 12 == 客户端 12 | ✓ |
| 4 半开 + 一台死（24 条，`health`+`circuit`、`max_fails=1`、`fail_timeout=600`、`B=2`、`probe_interval_ms=0`） | 目标 0 拒绝连接 | 被钉进半开的那几条探测请求**不换台**：它们的条数应当**等于**当场 502 的条数，且 `after_retry=0`、`recovered=0` | `half_open_admitted=1`、`half_open_reopened=1`、`ejections=2`、`half_open_recovered=0`；**`gateway_replies_connect_failed=1 == admitted`** 且 `after_retry=0`；`retries=1`（摘除生效前那一条用完预算）；24 条全部拿到完整表态（200×23 + 502×1） | ✓ |
| 5 生命周期（一死一活，8 条） | 同臂 1 | 重放丢弃的那条配对不留对象、fd 收支平衡 | `fd 7→7`、`live_connections=0`、`constructed==destructed==13` | ✓ |

**三处互背书**（不是网关自己数自己）：客户端侧 curl 的状态码条数、网关退出账
`upstream_retries` / `gateway_replies_*` / `upstream_connect_failed`、**上游进程自报**的
`已服务请求数 + 固定状态码响应数`。放大这件事的主语是"后端执行了几次"，所以只有第三处算答案，
前两处算过程。臂 1 那两把尺一个读到 1.50、一个读到 1.00，正是"重试救活但不复制"的形状。

### 16.2 十处阳性验证（能红才叫绿；红数=该退化下真变红的用例条数）

| # | 退化（改回"没有这个功能时长什么样"） | 红数 | 红的是哪些 |
|---|---|---|---|
| ① | **从不重试**（`Reactor::retry_eligible` 一律说不） | **6** | `RefusedFirstMember…`、`SyncConnectFailure…`、`BothMembersDead…`、`EjectedFallback…`、`Pipelined…`、`CloseIntent…` ← 下界那一半的钉子 |
| ② | **级联重试（异步支）**：重放那条连接仍然留着备选 | **2** | `BothMembersDead…`（`retries` 从 10 涨破请求数 → 上界红）、`EjectedFallback…` |
| ②′ | **级联重试（同步支）**：`ensure_upstream` 那一步不盖"已用掉"记号 | **1** | `SyncRetryStampsTheSpentBudgetToo`（第三次尝试让 `upstream_opened` 从 1 变 2） |
| ③ | **重试判据放宽到 5xx**（会计半边） | **1** | `FiveXxIsNeverRetriedAndBackendsSeeExactlyN` |
| ④ | **备选算成同一台**（`(i+1)%n` 写成 `i`） | **8** | 两条 `S4_06_RetryTarget` 单元 + 六条端到端 |
| ⑤ | **不做落点资格检查**（摘除中的成员也能被重放绕过） | **2** | `S4_06_RetryEligible…`、`EjectedFallbackSetIsNotARetryTarget` |
| ⑥ | **资格问句改成消费半开名额**（复用 `admit` 而不是纯读） | **1** | `S4_06_RetryEligible.ReadsStateButNeverTouchesTheGate`（读 50 次之后 `half_open_admitted` 从 0 变 50） |
| ⑦ | **重放只搬字节、不搬在飞计数** | **1** | `ReplayedRequestsStillAnswerToTheHeadTimeout`（客户端挂死） |
| ⑧ | **重放不转一次性配对意图** | **1** | `CloseIntentTravelsWithTheReplayedBytes`（`pool_returns` 从 0 变 1：一次性后端被当热连接归还进池） |
| ⑨ | **备选推进了轮询游标**（把备选当成第二次选择） | **15** | `S4_06_RetryTarget` 两条 + S3-02/S3-03/S4-05/S3-05 共 13 条分布类用例 |

**②′、⑦、⑧ 第一版都是 0 红**，本轮把它们逐个补到能红（过程写在 §16.3 第 3 条）。
还原用精确替换 + `grep -rn TEMP-POSITIVE src/ tests/ tools/` = **0** 之后复跑全量 **333/333**。
驱动器：`/home/archer/s4-06-positive.py`（七处）与 `/home/archer/s4-06-positive2.py`（补测三处），
锚点命中数≠1 就拒绝改并写明"没测成"。

### 16.3 工具侧与用例侧的发现（四条，都是"看起来在测、其实测的是别的东西"）

1. **只编 `l7gate_tests`，冒烟跑的是上一版 `l7gate`**——本轮第一条 ✗（`after_retry=0`）不是实现问题：
   我改完归因只重编了测试目标，`build/debug/l7gate` 还是改动前的产物，冒烟于是测了一个旧二进制。
   这是口径⑪"构建失败仍拿旧产物跑 ctest 并报通过"的**同族第三次应验**，只是这次换了一条通道：
   ctest 那侧我防住了（`verify-all-presets.sh` 已改成 build 非 0 就停），冒烟那侧没防。
   现在的规矩：**源码改动之后跑冒烟前，必须先 `cmake --build build/debug`（全目标）**，
   不是 `--target l7gate_tests`。反过来说，那条 ✗ 也当场证明了 `after_retry` 这格不是恒真——
   旧实现读 0、新实现读 10，仪器真的在看这件事。
2. **哈希臂的 path 不能写死**：`Pipelined…` 那条第一版用固定 path `/pin`，间歇读到 `retries == 0`。
   根因不在实现：环上的点由**端点键**（含内核分配的夹具端口）建，同一个 key 这一轮落在那台死的、
   下一轮落在活的——写死 path 等于**每轮重掷一次硬币决定这条用例有没有东西可测**
   （ADR-19 追加二那条"样本太小"的同族，只是这次决定的是有无而不是数值）。
   修法不是加大样本，而是**用同一个 `select_from_ring` 反查**，在跑之前把 key 钉到"落点=死的那台"，
   钉不上就 ASSERT 失败——判据由构造给出，不由运气给出。
3. **三处 0 红逼出的三个发现**（这一条最值得留着，因为它给的不是"改好了"而是"为什么会漏"）：
   - **②′ 同步支**：单看同步支的级联是不可见的——同步支的落点如果**成功**了，那条盖着的备选
     永远不会被读。补的用例刻意做成**同步失败串异步失败**（首试 `255.255.255.255` 当场拒、
     落点是关掉的回环端口稍后拒），两个揭晓点串起来才看见那一个记号。
   - **⑦ 在飞计数**：响应总能回来，所以"在飞数没搬"在 `responses_completed`、`live`、`pool`
     三格上都不留痕迹。它唯一露相的地方是**限期登记的条件**——`on_upstream_event` 握手成功那一支
     只在"手上还有在飞请求"时才登记响应头限期。所以补的用例是"落点那台连上但不回字节"：
     在飞没搬 → 限期不登记 → 客户端挂死。**一条只在失败路径的某个条件上才可见的账，
     必须专门造一条让那个条件成为主路径的用例。**
   - **⑧ close 意图**：仪器先走了一步——`TestUpstream` 默认看到 `Connection: close` 就自己断连，
     于是那条配对无论网关怎么决定都会消失，`pool_returns` 恒 0。补法是给夹具加一个旋钮
     `set_ignore_close_header(true)`（只在 start 之前设），让它"回完就把连接留着"。
     这个旋钮不是为了模拟不守规矩的后端：**网关把配对标成一次性是网关自己的语义，
     不能依赖后端配合才成立**——而原夹具的行为恰好让这件事不可观测，正是口径⑮讲的
     "仪器看不见那个事件时，读数永远支持你的断言"。
4. **两处按原样留着的不可断言边角**（与 ADR-22 那条 `epoll_ctl` 同形，写清楚而不是假装测过）：
   ① 重放时 `queue_bytes` 装不下——判据是同一条 256 KiB 出向水位，而 A 的出向本就受它夹着、
   B 那一刻出向必空（借来的要求空、新建的是空的）→ 端到端没有注入手段，处置是"不丢字节、
   按 `over_capacity` 表态"；② ③ 那处"5xx 也重试"的退化只能改**会计半边**（把计数器加在 5xx 点），
   因为在这个设计里请求字节已经被整体搬走、下游缓冲不再有那条请求，**真重发无法表达**——
   这不是我漏了一处判据，是"复制"这条风险被结构挡住的证据：`Σ 后端自报实收 ≤ 客户端请求数`
   那条断言仍然写着，它在架构变了的那天会立刻有用。

### 16.4 这一轮的门禁面

三 preset 各 **333/333**（318 → 333：`S4_06_Retry` 10 条端到端 + `S4_06_RetryTarget` 3 条 +
`S4_06_RetryEligible` 1 条 + `S4_06_ConnectionLayout` 1 条）、非 gtest 警告 **debug/asan/tsan 各 0 条**、
`tsan` 全量日志里 `ThreadSanitizer` 出现 **0 次**（新增并发面 = 零：所有新状态都在 owner 循环线程私有，
共享面只多了一次只读原子读）。证据：`/home/archer/s4-06-presets.log`、`s4-06-tsan-ctest.log`。

**九条冒烟按 `PRESET=debug` 逐条跑**（`/home/archer/s4-06-smokes.log` + 补跑 `/home/archer/s4-06-docker-fill.log`）：
`smoke-s1-04 / s1-06 / s1-07 / s2-05 / s3-02 / s4-05 / s4-06` 七条**首跑就 `rc=0` 且 ✗=0**
（新增的 `smoke-s4-06.sh` 五阶段全绿，不需要 Docker）；
**`smoke-s3-04` 与 `smoke-s4-04` 在门禁批次里 rc=1、✗=0——两条都在阶段 0 自己拒跑**，理由是同一条环境侧
阻塞：Docker Desktop 引擎停了（`docker-desktop` 发行版 Stopped、`/var/run/docker.sock` 不存在、
2375/2376 关闭、发行版内 `docker` 只剩 Windows 侧那个 exe），MySQL 与 redis:7.2.5 两个容器都起不来。
两条各自的死法是设计好的：s3-04 报「起容器失败」、s4-04 报「本机没有 redis:7.2.5（契约锁定的 tag，
我不换）」——**都不换个顶上去**，所以那一格当时按"未跑"记。**项目所有人启动引擎后单跑这两条补绿**
（`smoke-s3-04 rc=0 ✗=0`、`smoke-s4-04 rc=0 ✗=0`，都是首跑即绿）→ **本轮收尾口径是九条全绿**，
而 feat 提交 `7413550` 的信息里写的是提交那一刻的真实状态（两条未跑），不回头改。
紧随 tsan 全量之后的端口残留瞬态本轮未出现。


### 16.5 §12 类 1 那格的重测（ADR-23 第十一节末那条范围更正的执行处）

`scripts/fault/s3-05-matrix.sh` 原样重跑（`PRESET=release`，rc=0；**第一轮的落盘整份留档在
`build/fault/s3-05-round1/`**，第二轮写在 `build/fault/s3-05/`）。第一轮的五条读数不作废，
下面按"变了什么 / 没变什么"两栏对照：

| 类 | 第一轮（无重试） | 第二轮（重试落地） | 判读 |
|---|---|---|---|
| 1 connect refused | 摘 **273** / 恢 89 ms | 摘 **321** / 恢 **139** ms | 同带（预期 200–600 / ≤100–500）。**单轮读数不判"改动有效或无效"**（`docs/SLO.md` 取样规程），这一格只报"仍在带内" |
| 2 后端 5xx | 324 / 129 | 361 / 107 | 同带，重试不该管它 ✓ |
| 3 上游半开（连上不回字节） | **5280** / 119 | **5349** / 69 | 差 69 ms，**小于判定法自己的粒度**（curl 逐条 + "连续 20 条不见 128"的窗口 + 100 ms 探活拍）。这一格"不该被重试碰到"的**机制性**证据不在时间读数上，在 ctest 那条 `StalledMemberTimesOutAndIsNotRetried`（`upstream_retries == 0` 是精确断言）——时间读数只能说"没看出变化"，不能说"证明没变" |
| 4a 慢过探活超时 | 2108 / — | 2102 / — | 同上，差 6 ms |
| 4b 慢在超时内 | 不摘 | 不摘 ✓ | 未变 |
| 5 探活失败业务正常 | 359 / 90 | 320 / 99 | 同带 |
| 成员 2 被摘总数 | 5 | **5** ✓ | 五次注入各摘一次，重试没有改变摘除次数 |

**这一轮真正新增的那一格**在网关自己的退出账里（`build/fault/s3-05/gateway.log`）：

```
upstream_connect_failed = 2
upstream_retries        = 2
gateway_replies_connect_failed = 0
gateway_replies_after_retry    = 0
```

读法：**整个五类矩阵跑下来只发生了 2 次 connect 失败，两次都被重放到另一台并成功，
所以客户端一条 502 都没有拿到。**重试落地之前，这同两次失败就是两条 502
（S4-02 的形状）。这一格同时说明"重试不是每次都触发"——它的触发条件是 connect 失败，
而矩阵里绝大多数的坏是探活超时/5xx，两者都不该重放，读数也确实是 0。

**类 1 摘除耗时没有因为重试而变快**（321 vs 273 ms，同带）：摘除判据在这个配置里由探活驱动
（`probe_interval=100ms`、`max_fails=2`），业务侧的两次 connect 失败只贡献 2 个失败样本，
重试没有替代探活。这条要说清，否则很容易被读成"重试让摘除更快了"。

---

## 17. 自发表态路径上的配对账（2026-11-02，S4-06a，ADR-19 追加第五条）

**这一节在问什么**：`LbState` 的配对账是 `least_conn` 那把尺（"当前被配给下游的上游连接条数"）。
S4-06 实现重试时撞见一条漏账：**六类自发表态（异步 connect 502、两类超时 504、头前截断 502、
解析错 4xx、已配对时的 404/429）共用 `fail_pair_with_reply` 这个出口，而它为了不把 close-why
传染给对端会先解链——`close_connection` 判"该不该减账"用的正是"进来时 `peer_fd` 有没有效"，
解链把这个判据抹掉了，于是那笔 +1 从此没人减**。尺只加不减，least_conn 会长期把那台后端读成
"更忙"，而且**永不自愈**。

**注入方式**：不需要注入——这是一条**账本一致性**缺陷，判据是"尺上的数"与"注册表实况"对不对得上。
先量后修（临时探针建、跑、原样还原，md5 相同、残留 0）：

| 观测 | 修前（探针实测） | 应当是 |
|---|---|---|
| `LbState::pairings(0)` | **1** | 0 |
| `live_connections` | 0 | 0 |
| `upstream_opened` | 1 | 1（这就是那笔 +1 的来源） |
| `connections_closed` | 2 | 2（两侧都关了，账上却还挂着一笔） |
| `pool_borrows` / `pool_returns` | 0 / 0 | 0 / 0（不是池的路径） |

**修完之后**（`S4_06a_PairingAccount` 四条，形状是**成对**的）：异步 connect 502、响应头超时 504、
已配对连接上的 429 各钉一格"表态之后归零"，并且每条都连着读"`upstream_opened ≥ 1`（那笔 +1 确实
发生过）"——**只断归零挡不住"从不加账"的实现**；第四条钉上界：两条下游各配一条上游（账 = 2），
在其中一个上打一条畸形请求，只许放掉它自己那一笔，`pairings` 必须还剩 1。

### 17.1 两处阳性验证（这条改动没有回归信号，绿不可能是证据）

| 退化 | 红数 | 红的是哪些 |
|---|---|---|
| ① **退回旧行为**（解链处一次都不减） | **4** | `S4_06a_PairingAccount` 全部四条 |
| ② **改成减两次** | **1** | 只有 `OneReplyDebitsExactlyOnePairing`（前三条在 `note_unpaired` 夹 0 之后仍读成"干净的 0"） |

**②这一行是本节最值得记的东西**：它是"第四条代替不了前三条"的实测理由，不是修辞。
一个只会"减到 0"的实现能让前三条全绿，而它已经把别人的账也抹掉了——夹 0 这个安全网
恰好把这类错误变得**看起来无害**。所以"多减一次"必须用**还有别的账挂着**的形状去抓。

### 17.2 修之前它是绿的，修之后它也是绿的

全量 **337/337** 在改动前后都通过：既有端到端**没有任何一条**读 `pairings`，而纯函数那四条
（`balance_test.cc:398-411`）够不到跨层调用。**这就是"改动没有回归信号"的确切含义**——
它不是"改动很安全"，是"如果我不自己写用例，就没有任何东西能告诉我修没修对"。
所以 S4-06a 的验收主体是那两处退化，不是那四条绿灯。

### 17.3 语义变更的代价（照记）

漏账偶然造成过一种"故障回避"：刚表过态的后端读数变高 → `least_conn` 少给它流量。修完之后
这层偶然消失，线上表现是"故障台被放回更多流量"。**不要把这件事读成保护**——回避由摘除、熔断与
安全阀负责（ADR-12），尺只该量当前配对。真要验证回避消失没有，看 `health`/`circuit` 那两条路径。
已记档影响：B-013 的 `least_conn` 两臂（98.3% 吸附、每请求换后端 p50 +1,664 µs / QPS −36%）
是**修前读数**，已在 B-013 就地追记一条"修后要重跑才能引用"，原数不改。

**门禁面**：三 preset 各 **337/337**（333 → 337，四条新用例）、非 gtest 警告 debug/asan/tsan 各 **0 条**、
`tsan` 全量日志里 `ThreadSanitizer` **0 次**、**九条冒烟全部 `rc=0` 且 ✗=0**（本轮 Docker 在位，
`smoke-s3-04`/`smoke-s4-04` 是在门禁批次里一次跑过的）。证据：`/home/archer/s4-06a-presets.log`、
`s4-06a-tsan-ctest.log`、`s4-06a-smokes.log`、`s4-06a-positive.log`。

---

## 18. S4 故障注入矩阵第二轮（2026-11-02，S4-07，ADR-13：全部应用层注入）

**这一节在问什么**：§12 那五类量的是"会选人、会摘人"的网关。S4 之后它还会**拒绝**（429）、会**限期**
（504）、会**问 Redis**、会把恢复**收进半开这道门**、还会**换一台重试**。这一轮问的不是这五件功能
各自好不好——那各自都有行——而是**把它们叠在旧故障上之后：账还对不对，恢复到底走哪条口**。
后半个问题就是 ADR-12 追加二第七节挂给 S4-07 的那条裁定，本轮用实测把它答掉了（18.4）。

落盘两份：`build/fault/s4-07/`（叠加五类，新脚本 `scripts/fault/s4-07-matrix.sh`，`PRESET=release`，
**rc=0、✗=0**）与 `build/fault/s4-07-base/`（对照臂 = `s3-05-matrix.sh` 的第三轮，**rc=0、✗=0**）。
§12 是第一轮、§16.5 是第二轮，**第二轮那份落盘原地保留**——`s3-05-matrix.sh` 写死了 `OUT`，
本轮给它加了 `FAULT_OUT_DIR` 覆盖（工具改动，不动 `src/`），否则就是拿新一轮去盖已归档的读数。

### 18.0 对照臂：S4 全装完之后，基线那五类漂了没有

同一份脚本、同一份配置（只装 `health`，不装 `circuit`/`rate_limit`/Redis）、同一个判据
（"连续 20 条不见 128"），三轮对照：

| 类 | §12 第一轮 | §16.5 第二轮 | 本轮第三轮 | 判读 |
|---|---|---|---|---|
| 1 connect refused | 摘 273 / 恢 89 | 摘 321 / 恢 139 | 摘 **275** / 恢 **86** | 同带（预期 200–600 / ≤100–500） |
| 2 后端 5xx | 324 / 129 | 361 / 107 | **335** / **100** | 同带 |
| 3 连上不回字节 | 5280 / 119 | 5349 / 69 | **5361** / **55** | 同带；**这一读数被 18.2 类 7 拆开解释了** |
| 4a 慢过探活超时 | 2108 | 2102 | **1584** | 三轮极差 **524 ms（25%）**，照记。这一格的读数成分与类 3 同族（在途排空 + 判定窗口），单轮读数不判有效无效 |
| 4b 慢在超时内 | 不摘 | 不摘 | 不摘 ✓ | 未变 |
| 5 探活失败业务正常 | 359 / 90 | 320 / 99 | **361** / **104** | 同带 |
| 成员 2 被摘总数 | 5 | 5 | **5** ✓ | 五次注入各摘一次 |
| 成员 2 探活失败 | 68 | — | **65** | 同一量级 |

**结论形状**：S4 那四个机制默认不装配 = 五类读数没动，这条不是推理而是三轮对照。
而 4a 那一格的极差 524 ms 提醒了一件事——**这几格里凡是"排空主导"的读数，本来就带这个散布**，
所以本轮凡引用带内/带外都写成"带"，不写"值"（取样规程自 S2 强制）。

### 18.1 类 6：限流触发 × connect refused（叠加重试）

一死一活两成员、`rate_limit{rate:1,burst:5}`、12 条同 path 突发（`workers=1`，顺序确定）。

| 预期（认领时写下的可断言句子） | 实测 |
|---|---|
| 上界：`allowed + rejected == 12` | `5 + 7 == 12` ✓ |
| 上界：**后端实收 == allowed**（重试不多耗一个令牌，也不复制请求） | 活台自报 **5** == allowed **5** ✓ |
| 上界：`upstream_opened ≤ allowed + retries`（被 429 的那几条一个上游字节都不发） | `4 ≤ 5+3=8` ✓ |
| 下界：`retries ≥ 1` 且 `rejected ≥ 1` | `retries=3`、`rejected=7` ✓ |
| 归因：`connect_failed == retries`（每次被拒都换台且只换一次） | `3 == 3` ✓ |
| 客户端侧：`200 条数 == allowed`、`429 条数 == rejected` | 状态行 **`200×5 + 429×7`** ✓ |
| 重试兜住了回源失败 | `gateway_replies_connect_failed=0`、`requests_parsed=12` ✓ |

这一格真正拿到的是**令牌口径的那条等式**：限流数的是"进门几条"，重试数的是"回源试几次"，
两者叠在一起时 `后端实收 == allowed` 才证明重试没有偷偷多花配额——这条在 S4-03 与 S4-06
各自行里都写不出来，因为当时没有另一个机制叠在旁边。

### 18.2 类 7：超时撞线 × 连上不回字节（把 §12 欠的那两笔计时分开报）

§12 类 3 当年只能这么写："探活侧摘除 ≤1s 量级；客户端视角的读数由在途请求的排空主导"——
前半句是**推断**。本轮把响应头限期从默认 10000 ms 压到 **800 ms**，其余全部沿用 §12 的配置：

| 量 | 读数 |
|---|---|
| **网关自报**摘除时刻（`health_t1_…_last_ejection_epoch_ms` − 脚本 T0，同一根墙钟轴） | **492 ms**（另两轮 540 / 740 ms） |
| **客户端侧**"连续 20 条不见 128" | **1262 ms**（另两轮 1192 / 1209 ms）；同配置不限期那一臂（18.0 类 3）是 **5361 ms** |
| 撞线计数 | `upstream_head_timeout=1`、`upstream_connect_timeout=0`、`upstream_body_idle_timeout=0` ✓ 撞的是哪一格说得清 |
| 客户端实到 | **1 条 504**（不是 curl 自己 5 s 放弃） |
| 越界探针 | `upstream_retries=0` —— 超时不在重试定义域内（ADR-23 第一节第三行）。**这一格的"看得见非零"由类 6 的 `retries=3` 与类 10 的 `retries=20` 背书**，不是恒真 |
| 判据联动 | `ejections=1` —— 超时计入健康判据（ADR-21 规则 1） |

**S4-01 验收句里"在途排空口径结账：两类计时在本任务后可以分开报"到这一格才算真的交掉**：
两笔计时现在一个是自报值（492 ms 量级）、一个是客户端值（1.2 s 量级），
而且客户端那一笔随限期变化（800 ms 限期 → 1.2 s；默认 10 s 限期 → 5.4 s）——
"排空主导"这句从归因变成了可复现的读数关系。

### 18.3 类 8：Redis 降级 × 后端 5xx + 熔断窗口（两臂只差 Redis 在不在）

两成员（一台恒 503）、`rate_limit{rate:1,burst:8}` + `health{max_fails:99, probe 0}` + `circuit{minimum_requests:6}`、
`--redis-db 127.0.0.1:16379 --redis-settle-ms 200`、`redis:7.2.5` 容器 cpuset 绑 15 号核。
每臂三段：涓流 16 条（900ms/条，喂窗口）→ 突发 12 条（顶穿令牌桶）→（仅 8b）把容器起回来再突发 12 条。

| 预期 | 8a（Redis 全程在位） | 8b（网关在没有 Redis 的世界里起来，段中起回来） |
|---|---|---|
| 窗口摘除与 Redis 无关（辅助依赖不是全局故障域） | `trips_rate=1 ejections=1`，摘除时刻 = 注入后 **10146 ms** | 同读数，摘除时刻 = 注入后 **10133 ms**——**差 13 ms**，两臂的 503 条数逐数相等（段 A 各 6 条、坏台自报各 6 条） |
| 降级有账、不静默 | `settles_ok=74 settles_failed=0` | `settles_failed=75`（挂着每拍都有账） |
| **降级期间拒绝能力不消失** | 突发段 429×6 | 突发段（此时 Redis 仍挂着）**429×6**、退出账 `rejected=16` ✓ |
| 通路活着回来，不重启网关 | — | 终局 `redis_degraded=0`、`settles_ok=7 ≥ 1`；且回来之后夹子真的咬人：段 C 12 条里 **429×10**、`rate_limit_capped_rejects=1` |
| 计数守恒（限流不替别的机制背账） | `allowed+rejected = 22+6 = 28 = 16+12` ✓ | `24+16 = 40 = 16+12+12` ✓ |

`settles_failed` 这一格在 8a 是 **0**、在 8b 是 **75** —— 本轮所有"某某没发生"的断言都配了
一个读数非零的对照臂，这条是其中最直白的一条。

### 18.4 类 9：熔断半开 × "探活活、业务死"——本轮最重要的两格

注入形状：坏的那台 `--path-status "/biz9:503"`（**业务恒 503**）而它的探活路径 `/probe` 照旧 200
（**探活活**）。五条臂只差装配与流量节奏；每臂两段：段 1 故障在（40 条 `/biz9`）、段 2 故障撤
（换 `/ok9` → 这台回 200/128B，用体长认是谁答的，同 §12 的判法）。

| 臂 | 装配 | 段 1 节奏 | 暴露面（**坏台自己数的 503**，客户端数逐数相等） | 摘除 | 半开放行 | 全量恢复 | 恢复耗时 |
|---|---|---|---|---|---|---|---|
| 9a | `health`(probe 100) + `circuit`，`max_fails=99` | 涓流 60 ms | **20**/40 | 15 | **16** | `recoveries=1` == `half_open_recovered=1` | 94 ms |
| 9b | `health`(probe **0**) + `circuit` | 涓流 60 ms | **8**/40 | 3 | 4 | 1 | 95 ms（上一轮同一臂 252 ms，见下面的散布照记） |
| 9c | **只装 `health`**(probe 100)、`max_fails=2` | 涓流 60 ms | **20**/40（**永久全量的一半**） | **0** | 0 | 0 | 95 ms |
| 9d | 同 9a | **突发**（40 条尽快） | **11**/40 | 6 | 7 | 1 | 78 ms |
| 9e | 同 9c | **突发** | **12**/40 | 6 | 0 | `recoveries=6` | 15 ms |

#### 18.4.1 第一条：文档写错了一件事，实测把它纠正过来

ADR-12 追加二第七节 bullet 2 与 ARCHITECTURE 1.31 已知边界① 都写着
"**主动探活判活 → 立即全量恢复**（S3 已裁第三条③的判据原文，本轮**没动**）"，
并由此推出"半开在 `probe_interval < fail_timeout` 时是弱路径"。
9a 的读数把这句判掉了：**摘除 15 次、半开放行 16 次、探活成功 40 次，而 `recoveries=1` 逐数等于
`half_open_recovered=1`**——也就是这台成员每一次被解除都经过半开那道门，唯一一次走到全量是段 2 里
攒满 `K=2` 条业务成功转正的那一次；**探活那 40 次判活，没有一次把它直接放回全量**。
这与 `tests/health_test.cc` 的 `ProbeSuccessWakesIntoHalfOpenNotFullTraffic`（S4-05 阳性验证 ⑧
红的那三条之一就是它）说的是同一件事，也与 `src/route/health.cc:160-163` 一致。

所以"两条恢复出口并存"这句话**没有作废，但它的适用面被写错了**：真实的那条第二出口住在
**没装 `circuit` 的那一档**——9e 的 `ejections=6 == recoveries=6` 且 `admitted=0`，
每次摘除都由探活解除成**全量**。9a 与 9e 是同一个故障、同一份流量、只差 `circuit` 那一段配置，
两条出口的区别第一次被量成了两个可对照的读数，而不是一句"本轮没动"。
已按这条改写 ADR-12 追加三与 ARCHITECTURE 1.31 边界① / 1.25 那句"探活成功 = 立即 revive"。

#### 18.4.2 第二条：新边界——只装 health 时，"探活活、业务死"的成员**摘不掉**

9c 是本轮唯一一条**与我自己认领时写下的预期相反**的格。我预期它会横跳（摘除 ≥1、admitted=0），
实测 **`ejections=0`、`probe_ok=40`、暴露 20/40 条 503 永久不停**。根因是两条已裁规则撞在一起：

- 已裁第 3 条④说探活失败与业务失败**汇入同一个连续计数**；
- `note_probe_result(alive=true)` 会把这个连续计数**清零**（`health.cc:145`），这是 S3-03 立的、
  并且 §12 类 5 正是靠它把"探活死、业务活"的成员摘掉的。

涓流档下落到这台的业务失败约每 120 ms 才一条（round_robin 两台、60 ms 一条），而探活每 100 ms
清一次账 → `max_fails=2` 永远够不着。9e（突发档）失败到达率高于探活清零节奏，就摘得动（6 次）。
**所以这条边界的准确表述是节奏，不是"能不能"**：只装 `health` 且开着探活时，一台"探活会答、
业务全 503"的成员，在业务失败比探活稀疏的流量下**永远不会被摘除**，它会一直接全量的一半流量。

这与 §12 类 5 合起来才是完整的一对：**探活只能证明"这台机器会答 `/probe`"，不能证明"它能服务业务"**。
S3 的连续判据把前者当成了后者（类 5 那一格因此摘得掉，反过来的这一格因此摘不掉）。
管这一形状的唯一判据是**错误率窗口**（它不被探活清零、且探活样本根本不进窗口，1.31 第一节那条），
而窗口属于 `circuit` 段——所以"S3-03 只装 health 够用"这句在 S4 之后要补一个限定条件。
处置：写进 ADR-12 追加三 + ARCHITECTURE 1.25/1.31 的已知边界，**不在本轮改实现**（这条不是缺陷，
是已裁判据在没装第二个判据的那一档下的固有覆盖面；要收窄它就得装 `circuit`，那是配置动作）。

#### 18.4.3 裁定取数：两条恢复出口要不要并成一条

ADR-12 追加二第七节交给 S4-07 的四个数，本轮量到的是（矩阵里三臂同注入同流量，只差装配）：

| 判据 | 读数 |
|---|---|
| 合并的**代价**（健康成员回归慢多少） | 9a（探活→半开→2 条业务成功）= **94 ms** vs 9c/9e（探活→立即全量）= **95 / 15 ms** —— **有流量时读不出代价**；纯被动等到期那一档 9b = **95 ms**（上一轮同一臂 252 ms，本轮 95 ms，这一格带内散布按 18.0 那条一起看） |
| 合并的**收益**（坏成员放回多少） | 涓流档 9a=20 vs 9c=20（**等效，门不夹**）；突发档 9d=11 vs 9e=12（**几乎等效**） |
| 为什么收益在这一档看不见 | 9e 那一档 S3 的连续判据本身就是一次只放 `max_fails=2` 条，与 `B=2` 同量级；门真正夹住暴露面那一格是 §15.1 阶段 1 用 `max_fails=99` 量到的 **打 24 条只让它接 6 条** |
| 而这一档最贵的那件事其实不是"并存" | 9c：**不装 `circuit` 时这台成员根本摘不掉**（18.4.2）。合并与否都不改变这一格；装不装 `circuit` 才改变 |

**裁定（2026-11-02，本轮按项目所有人的委托裁，理由与代价都进 ADR-12 追加三）**：
**不并**，且**没有新任务要立**。三条理由：
① 装了 `circuit` 的配置里**两条出口已经并了**（9a 实测：每次解除都经半开），这条不是在裁意图，
是在裁一个已经落地的实现事实——把文档那句过期话改掉即可，改代码反而会把它弄成两份真相；
② 没装 `circuit` 的那一档**没有"半开"这个状态可去**（半开位与门都由 `circuit` 段的装配决定，
1.31 第二节），要把探活改成"到半开"等于要求所有 `health` 配置都装 `circuit`，
那是把 S3 已裁第三条③的判据原文换成另一个功能开关，并会作废 §12 与 B-014 那两份按
"探活即全量"跑出来的归档读数——ADR-12 第七节写的那条理由（我不回头改已记的数）在这一档仍然成立；
③ 合并的代价本轮量不出来（94 ms vs 95 ms 同带），收益已经在位（门在装 `circuit` 时管着每一次解除），
**所以没有实测逼这件事**。
代价照记：只装 `health` 那一档仍然有 18.4.2 那条边界，且它不由"并口"解决——它由装 `circuit` 解决。

### 18.5 类 10：重试放大 × 熔断错误率窗口（失败样本翻倍落在哪里）

两台端口都不绑（connect 必拒）+ `probe_interval_ms=0` + `circuit{minimum_requests:20}`，打 24 条。

| 预期 | 实测 |
|---|---|
| 上界：`upstream_connect_failed ≤ 请求数 × (1 + 预算)` | **44 ≤ 24×2 = 48** ✓（构成 = 摘除前 20 条×2 + 摘除后 4 条×1） |
| 下界：翻倍真的发生（`> 请求数`） | **44 > 24** ✓ —— ADR-23 第六节"一条客户端请求注入 2 个失败样本"是实测 |
| 重试停在摘除那一刻 | `retries=20 < 24` ✓ 且**逐数等于** `minimum_requests=20`（每台每请求各 1 笔样本 → 第 20 条客户端请求上两台同时达标）；摘除后没有 `kAllow` 的落点，资格判据把放大掐住了 |
| 两台都被窗口摘掉 | `ejections a=1 b=1`、`trips_rate a=1 b=1` ✓ |
| 一次表态仍只有一条 | `gateway_replies_after_retry=20`、客户端 **502×20 + 503×4 == 24** 条（那 4 条是全员被摘后的 `gateway_replies_all_ejected=4`）✓ |

对照臂 **10c**（一死一活 + `circuit{minimum_requests:12}`，24 条）钉的是另一头：
`死台 ejections=1`、**`活台 trips_rate=0 且 ejections=0`** —— 重试成功的那一条给落点那台的是
一笔**成功**样本，它不背别人的失败；`retries=12`（摘除前每条换一次台）、**活台自报实收 24 == 客户端 24**
（死台被试了 12 次、每条都被换台救走，**一条都没多**）。

### 18.6 生命周期

11 臂的网关退出账全部 `live_connections=0`、`connections_constructed == connections_destructed`
（16 / 27 / 30 / 42 / 37 / 68 / 82×5）。fd 那一格见 18.7 第 3 条——它第一次出现了两档判法。

### 18.7 工具侧与用例侧的发现（四条，都是"看起来在测、其实测的是别的东西"）

1. **我把 `gateway_replies_sent` 当成了"回源失败的表态"**，类 6 第一版当场红（`replies_sent=7` 而我
   期望 0）。真相是 **429 本身就是一条自产响应**：ADR-22 那张表里 404/429/502/503/504 全走
   `fail_pair_with_reply` 同一个出口，所以限流的那 7 条每条都计一次 `replies_sent`。
   正确口径是两条：`gateway_replies_connect_failed == 0` 才表达"重试兜住了"，
   `replies_sent == rejected` 才表达"一条请求一条表态"。**归因：我没把那张表读全，不是实现有第二份口径。**
2. **夹具的汇总行是退出时才打的**——在它还是活的时候读 `已服务请求数=` / `固定状态码响应数=`
   读回来是**空串**，而空串在判定里长得像 0。类 10c 那条"活台自报 ≤ 24"于是绿在 `0 ≤ 24` 上，
   而这一格的本意是**等号**。这是口径⑮ 的第三种形态（前两种是"仪器不数那个事件"与"一行多键 +
   行首锚"）。防复发：`upstream_*` 两个读句一律排在 `stop_pid` 之后；凡是"这就是它该等于的数"
   的断言不写成 `≤`。
3. **`fd_end` 是在析构之前采样的**（`main.cc:879` 那条注释就是这条），而在飞的**探活 fd 要到
   `~Reactor` 才关**（`reactor.cc:167-173`：摘 epoll → `close_now()`）。所以装探活的臂退出瞬间
   必然带着 0..槽位数 条探测 fd——本轮实测 Δ = 0/2/3（c9-d 是 0，c9-a/c/e 是 2，c7 是 3）。
   S4-04/05/06 那三份"fd 9→9"**全都出自没开探活的臂**，这条判据从来没在探活臂上验过，本轮第一次撞。
   处置：fd 分两档判（没开探活 = 逐数相等；开探活 = Δ ≤ 槽位数量级并写明采样点），
   **既不写成"泄漏"，也不写成"已验证无泄漏"**——后者我没有证据，前者的证据要等一条能在退出后
   数 fd 的用例。
4. **机制预期与文档原文冲突时，那一格不能写成硬断言**。本轮加了一个中间档 `note()`：
   守恒类不变式（计数相加等于请求数、后端实收等于 allowed、两把尺逐数相等）走 `fail()`，
   而"我认领时按 ADR 那句话预期它走 X、实测走 Y"这种格走 `note()` + 照记 + 归因。
   如果 9c 那一格当时红着，脚本就是在指着一个**实现的真实性质**喊错——而错的那份是文档。
   §12 当年对类 3/4a 用的正是这个形状（读数照记 + 归因），本轮只是把它写成了脚本里的一档。

### 18.8 这一轮的门禁面

本轮不动 `src/`（改的是 `scripts/fault/` 两份脚本与文档），但门禁按 S4 的口径照跑：
三 preset 全量、九条冒烟（跑前 `cmake --build build/debug` **全目标**，口径⑪ 同族那条）、
`tsan` 0 race、矩阵两跑 rc=0 且 ✗=0。逐条读数在 `build/fault/s4-07/`（含每臂网关日志、退出账、
上游自报日志、逐请求 trace）与 `build/fault/s4-07-base/`；脚本 stdout 落盘
`/home/archer/s4-07-matrix.log`（第一跑，两处判据 bug）、`s4-07-matrix2.log`、`s4-07-matrix3.log`
（判据修正后的正式读数，本文引用这一份）、`s4-07-base.log`（对照臂）。

## 19. 压测线自己的两处采样点，以及一条追认（2026-11-02，S4-08）

**这一节在问什么**：S4-08 要交"每请求成本"，靠的是 `s1-p999.sh` 那条线（B-008…B-016 都用它）。
本轮第一次把**装了探活的臂**放到它的门下面跑，于是量到的不是网关的故障，而是**仪器对不上被测方**——
具体是两处"两边的计数取在不同瞬间"。**这一节的注入对象是取数脚本自己**，按 ADR-13 的口径它同样要落盘：
仪器错不会让网关变坏，但它会让网关的读数变假。

### 19.1 第一处：fd 那一条硬门与在飞探活（追认 B-016 的两臂带着看不见的 ✗）

| 事件 | 读数 |
|---|---|
| HEAD 上跑 BS3 同形臂（`HEALTH_PROBES=1`，一成员 + 探活 100 ms） | `✗ fd 没回基线：9 → 10` → 整臂 rc=1、"上面的数字不要用" |
| 回查 B-016 当年落盘 `b016-bs3-r1` / `-r2` 的生命周期行 | **都是 `fd=9->10`**——同一对 `FD0/FD1`，同一条从 S2-07 起就没动过的硬 `die`（`git log -S"fd 没回基线"` 只命中 `dcccca5`） |
| 为什么当年没人看见 | **✗ 只打在 stdout**，`s1-p999.summary.txt` 里 ✗ 计数是 **0**；驱动没把 stdout 留档 |
| 根因（不是网关的泄漏） | `fd_end` 在析构**之前**采样（`main.cc:879` 那条注释），在飞探活 fd 到 `~Reactor` 才关（`reactor.cc:167-173`）→ 装了探活的臂在退出瞬间必然带 0..槽位数 条探测 fd。**与 S4-07 口径⑰ 同源**，这次是在压测线上撞的第二回 |
| 修法与它的阳性验证 | `HEALTH_PROBES=1` 时按 `Δ ≤ 探活槽位数 × worker` 判并打印 Δ（实测 `Δ=1 ≤ 槽位 1 × worker 1` → 过）；**缺省 0 时逐字沿用相等判据**（同一修好的脚本跑没装探活的臂 → `fd 9→9` 照旧、rc=0）。把预算临时压成 0 重跑同一臂 → 当场红：`✗ fd 增量 Δ=1 超出探活槽位预算 0`、`rc=1`、"上面的数字不要用"（`/home/archer/b018-fdgate-red.log`）；还原用精确替换，`grep -rn TEMP-POSITIVE src/ tests/ tools/ scripts/` = **0 处**。**门修完必须能红，否则就是换了个恒真** |

**影响面照记**：B-016 的"p99 抬 512/768 µs、归因主动探活"那一格来自这两条带 ✗ 的臂，
已在 BENCHMARK 的 B-016 追记里标成**待接管**，由 B-018 的 BS3-on-HEAD 两轮接替；
**已记的数一个不改**（690/738、4.0000→4.0003/4.0004、各分位与 QPS 都没被这条门质疑，被质疑的是
"形式资格"那句说法）。A0 两臂是 `9->9`，从来干净，所以"待裁⑤：B-012 不复跑"的依据不受影响。

### 19.2 第二处：探活等式差 1——"逐数相等"是运气不是性质

同一臂上游的 `accepted=112 = 直连 20 + 网关上游 20 + 探测 72`，而网关自报 `probe_ok=71 / probe_fail=0`。
**差的那 1 条是退出瞬间已发出 GET、响应还没回来的探测**：上游数进去了，网关两边都不结算。
根因同 19.1——两边的账取在不同瞬间（网关的退出账打在 `~Reactor` 之前，上游是收到 SIGTERM 之后才打）。
所以 B-016 那句"增量逐数等于 `probe_ok`"当时成立，是因为那一瞬恰好没有探测在飞。
修法：探活口径改成有界账 `probe_ok ≤ 差值 ≤ probe_ok + probe_fail + 槽位数 × worker`，
并保留"逐数相等时打印逐数相等"的分支（修好后同一小档再跑一臂，确实落在 `71 == 71` 那一侧）。

### 19.3 顺手抓到的第三条：一条会打谎话的 ✓

`s1-p999.sh` 里那行无条件的"等式：direct + proxy == upstream"，在装了探活的臂上打出的是
**`4000 + 12000 == 16071`**——三个数相加根本不等，却带着 ✓ 的前缀。
这是口径⑮ 的第三种形态：**仪器不是看不见那个事件，它看见了、但打的是别人的账**。
现在按 `HEALTH_PROBES` 分支只打各自成立的那句。

### 19.4 本轮我自己撞的两条工具错（都在跨 `wsl.exe` 那条线上）

1. **`pkill -f "test_upstream --listen 127.0.0.1:1858…"` 把父 shell 签掉了**（exit 15）：
   那个模式字面出现在我自己这条命令行里，`pkill -f` 于是也匹配到了包裹它的 bash。
   防复发：**清场按 PID 杀**（先 `pgrep` 拿号、再 `kill -9 $pids`），要 `pkill -f` 就保证模式
   不出现在自己的命令行里。这条与口径⑥ 同族——**跨 `wsl.exe` 传的命令串会被吃一层引号，
   凡是带 `$`/引号/反引号/管道的一律先落文件再跑**（本轮同一族又应验两次，见第 2 条）。
2. **`echo "rc=$?"` 在跨 `wsl.exe` 内联的命令里读到的是外层的值**，于是我一度以为
   `s1-p999.sh` 有"有 ✗ 却 rc=0"的退出码 bug。实际脚本尾部是 `exit "$FAILED"`，正确无误——
   **红的是我的取数方式，不是被测方**。修法同上一条：把命令写成 `/home/archer/*.sh` 再跑，
   rc 在脚本内部取（本轮 `b018-gatecheck.sh` 就是这么写的，两条门的 rc 都读到了 0/1 的真值）。

### 19.5 取数窗口自己的两个新事实（B-018 那 16 轮里量出来的）

1. **直连臂的稳态门有刀口**。这台机器直连能跑到 ~20 万 QPS，而 `REQUESTS_DIRECT=100,000 条/连接 × 100 连接 = 10M 条`
   → 窗口 ≈ 50.0 s，正好压在 `STEADY_MIN_MS=50000` 的下沿。**本轮 16 轮里两轮被它判红**：
   `49,995 ms`（差 5 毫秒）与 `49,600 ms`（差 400 毫秒），两轮的数因此都不作证据——其中一轮正是
   "重试跨版本对照"的一臂，所以那一对补跑了反序的 r3/r4。历史档 B-008…B-016 同参数没撞这条，
   是因为那些轮次的直连落在 16.6–19.1 万 QPS 之间、窗口 52–60 s。**这是定标与机器速度对上之后产生的
   边缘，不是被测方的毛病**。**处置：登记待裁第 ⑧ 条 → 项目所有人 2026-11-02 裁 A**，
   `REQUESTS_DIRECT` 抬到 **120,000**（经网关臂 `18,000` 一字未动，所以被测侧仍然可比），
   新档两成员臂又跑了两对四臂、全干净（直连窗口 61,468 / 66,500 / 69,180 / 78,363 ms、`✗` 计数 0），
   成本判定就此不再依赖任何作废轮次；**判红的那两轮落盘原样留着不删**，它们是这条变更的来路。
   可比性与"为什么否掉 B（动测量条件）/ C（继续白跑）"写在 [BENCHMARK](BENCHMARK.md) 末尾的口径变更节。
2. **这条线在一个批次里有 ±512–768 µs 的时段漂移**。组 II 早两轮"BASE 比 HEAD 慢 512 / 768 µs"，
   而**把顺序反排**之后晚两轮 p50 / p99 / p999 **逐数相等**（4,480 / 6,016 / 7,296）——
   那个差跟着时段走、不跟着二进制走。**"同机交替 + 至少两轮"这条规程本轮第一次被当成对照变量本身来量**：
   只看 r1 会把漂移读成成本，方向还正好相反（"加了重试的那版更快"）。
   可复现的形状写死一句：**任何 A/B 配对都要把顺序反过来再排一轮**，否则"谁先跑"与"谁更快"分不开。

### 19.6 这一轮的门禁面

**三 preset 各 337/337、非 gtest 警告各 0 条、`tsan` 全量日志 `ThreadSanitizer` 出现 0 次、
九条冒烟全部 `rc=0` 且 ✗ 计数 0**（跑冒烟之前先 `cmake --build build/debug` 全目标，口径⑪ 同族；
`smoke-s3-04`/`smoke-s4-04` 在门禁批次里一次跑过，Docker 全程没掉线）。
证据：`/home/archer/s4-08-presets.log`、`s4-08-tsan-ctest.log`、`s4-08-smokes.log`、
`s4-08-debug-allbuild.log`。B-018 的读数与判定见 [BENCHMARK B-018](BENCHMARK.md)；
本轮改的是 `scripts/bench/s1-p999.sh`（两条门 + 一行文案 + 一个新旋钮 `GATEWAY_BIN`）
**与 `scripts/bench/` 下三份 b018 模板 + 两份驱动/抽取脚本，`src/` 一字未动**
（`git diff --name-only | grep -c ^src/` = 0）。

