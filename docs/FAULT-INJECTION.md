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



---

## 20. S5-01 的阳性验证，以及本轮仪器侧抓到的自己人（2026-11-03，ADR-24）

### 20.1 仪器侧：本轮有九处是**我自己的工具**错了，其中四处会伪装成结论

这一节按 S4-08 的 §19 同一立场写：**仪器错了必须在纸上看得见**。这一阶段的判定全靠仪器，
其中第 3、5、6、6b 条如果没被抓到，会各自产出一条**比事实更强**的结论——这四条是本节的存在理由。
**6b 尤其难看**：它是 §19.4 那一条（S4-08 记过"以为脚本有'有 ✗ 却 rc=0'的 bug，红的是我的取数方式"）
的**同族第二次**，而且这次我把错的结论写进了三份文档并当面报了给项目所有人。

| # | 错在哪 | 症状 | 怎么被抓到的 | 修法 |
|---|---|---|---|---|
| 1 | `MetricsServer::stop()` 里顺手关掉自己的监听 fd 与 eventfd，而 `fd_end` 在 `report_stats` 里才采 | 开启态 `fd 12→10`（首尾不等） | 现成断言：`smoke-s5-01.sh` 阶段 2 那条 `fd_start == fd_end`（九条冒烟共用的生命周期判据） | 常驻 fd 留到析构；`stop()` 只 join 线程 + 收仍在册的抓取连接（与网关"监听 fd 留到析构"同一条口径） |
| 2 | 冒烟脚本阶段 3 的三条 curl 打在**已经退出**的网关端口上（阶段 2 收尾时 SIGTERM 了） | `curl -w %{http_code}` 给 `000`，被我原来的写法拼成 `000000`，读起来像"网关回了 000" | 三条 ✗ 同时出现而阶段 2 全绿——形态不对就先查臂的存活，不查实现 | 阶段 3 自己起一台；并把"坏请求之后仍要有一次 200"写成下界 |
| 3 | **`s5-01-metrics-cost.sh` 开头无条件 `rm -rf $OUT`** | 补跑某一臂时，**已合格的其它臂落盘被一起删掉**，判定只剩补跑那一臂 | 判定输出"有效轮数不足"，而我明明跑过四臂 | 只有起跑全新轮次才清空；`ONLY_ROUNDS` / `ARMS` / `SKIP_RUNS` 三条复算路径一律不删 |
| 4 | `sed -n 's/^Requests/sec: …'` 用 `/` 当分隔符，而模式里就带着 `Requests/sec` | `sed: unknown option to 's'`，QPS 那一列读成 `?` | 表格里只有 QPS 列全 `?`、其它列正常 | 分隔符换 `@`，并把"这行里带 `/`"写进注释 |
| 5 | **判定用的 awk 把每一行同时塞进 off 与 on 两个数组** | 打印出"跨臂差 +0.000000"——那是**同一个数组和自己相减**，一条真差都没有 | 把逐臂原始行与判定行并排看：四臂 `p50` 互不相等，判定却报差为 0 | 按臂前缀分流（`^off` / `^on-r`）；注释里写明"修的是路由，不是结论" |
| 6 | 合格性判据设成**我自己另外的**窗口区间 `[45,120] s`，而不是读取代数脚本自己的判定 | `off-r1` 被 `s1-p999.sh` 的稳态门判红（经网关窗口 **49,873 ms < 50,000 ms**、它末尾 `exit "$FAILED"` 所以**确实退了 1**，父脚本也记了 `！`），可我的窗口兜底把它读成"合格"放进了判定（49,873 ≥ 45,000） | 后来按 `✗`/`rc` 复核时才对齐：**门的判定必须原样接过来，不要另设一个近似的判据** | 合格性读**取数脚本自己的 rc**，窗口只做兜底（`WIN_LO` 抬到与门同一个 50,000 ms）；那一臂剔出判定并补跑 |
| 6b | **我去别人的输出里找自己的数**：`grep -o "rc=[0-9]*" $dir.run.log \| tail -1` | 命中的是生命周期行里的 `gw_rc=0`（网关进程的退出码），不是取数脚本的退出码 → 我据此断言"`s1-p999.sh` 判红却 `rc=0`"，并把这句写进了 BENCHMARK / SLO / FAULT-INJECTION 三处**和给项目所有人的汇报里** | 复核同一条 grep 的语义时才发现；**这条是 §19.4 那两条取数错的同族第二次**（S4-08 已经记过一次"以为脚本有'有 ✗ 却 rc=0'的 bug，红的是我的取数方式"），同一个结论摔两次 | 驱动自己把退出码写成 `$dir.rc` 文件，判定时读这个文件；文档里那句错话就地更正（不改任何已记读数，改的是我的说法） |
| 7 | 在成本脚本**正在运行时**编辑它 | bash 从文件偏移继续读，循环之后那段可能执行到改过的文本 | 事后核对：四臂落盘由子进程写、完好；但那份判定用的是旧解析结果 | 结论一律以 `SKIP_RUNS=1` **对落盘重算一遍**为准（这条流程正是 §19「只认落盘输出」的兑现） |
| 8 | 把 `ratio() { awk '…' }` 压成一行写 | `syntax error: unexpected end of file from '{' command on line 95`——报的错位在文件末尾 | `head -94` 能过、单独取第 95 行也过不去，才定位到 `}' }` 这种连写 bash 5 不认 | 函数体一律多行；把这症状留在注释里，因为它报的行号距真因几百行 |

一条同族的旧账本轮又兑现了一次：**取数的行选错了，比取不到更危险**。`s1-p999.sh` 与 `s1-p99.sh`
的 summary 里 `经网关` 那一行会出现不止一次（wrk 档 + hey 档 / 直连档 + 经网关档），
`head -1` 拿到哪个取决于表格顺序。所以三个取值函数全部改成**从原始落盘按锚取**
（wrk 档读 `proxy.wrk.txt` 的 `50%` 行与 `Requests/sec` 行；p999 档按 `awk $1=="经网关"` 读
`p50 / 窗口 / QPS`），并在代码注释里写明为什么不读渲染好的表。这是口径⑰那一族的第三次出现。

### 20.2 四处退化全部真打红（能红才叫绿）

本轮新增的断言一共四组，我逐组做一次"故意写坏"，**红的是哪几条用例是跑出来的，不是推出来的**：

| # | 退化 | 预期靶 | 实际红掉的用例（实测名单） |
|---|---|---|---|
| M1 | `handle_request` 的 404 分支改成"任意路径都回 exposition"，并把原 200 分支变成不可达 | `BadRequests…` | **#306 `S5_01_MetricsE2E.BadRequestsCountAsFailedScrapesWithoutBreakingTheSurface`**（预期靶，红）**＋ #304 `ScrapesNeverEnterBusinessCounters`**——后者不在预期里：这条退化顺带把 `/metrics` 正路的响应也吞掉了（`c.reply` 留空 → 客户端读超时）。**它是被"正路必须真的通"那半边断言抓到的**，值得留着：说明 #304 不只是在守"计数不动" |
| M2 | `ReactorGroup::start_listen` 去掉 `observations_enabled_` 判据，**无条件分配观测槽** | 关闭态那条结构断言 | **#305 `S5_01_MetricsE2E.DisabledSurfaceAllocatesAndWritesNoObservationSlots`** 一条红（`loop_observations() == nullptr` 当场不过），其余九条全绿——正中预期：这一格只有那一条守 |
| M3 | 渲染器**跳过 `# TYPE` 行** | 格式与 E2E 的正文断言 | **4 条红：#172 `FullAssemblyEmitsEveryKeyOnceWithItsOwnTypeLine`、#173 `UnassembledSectionsStayAbsentExactlyLikeTheExitDump`、#175 `TripsRateIsTypedAsTheCountItReallyIs`、#304 `ScrapesNeverEnterBusinessCounters`**（E2E 那条断的是正文里必须有 `# TYPE requests_parsed counter`）。#174 与 #176 不红，是**对的**：#174 只扫样本行的名字合法性、#176 只读 `# HELP`，两者都不依赖 `# TYPE` |
| M4 | `sanitize_metric_name` 不变换（点号原样发进 metric 名） | 名字法则与清单 | **#172、#174 `DottedAndColonTargetNamesBecomeUnderscores`、#175** 三条红 |

四处退化跑完全部还原，复跑核对：`grep -rn "TEMP-POSITIVE" src/ tests/ scripts/` **0 处**、
debug 全目标 `cmake --build` rc=0、`ctest` **347/347**。

**一处不算退化、但形状相同的东西写进了用例里**：#304 的第四段——同一条
`GET /metrics` 字节原样发给**业务端口**，`requests_parsed` 必须 +1。这不是我改出来的坏实现，
而是"如果观测面走主端口会发生什么"的正面对照：它证明前三段那一串"一动不动"不是因为
服务不存在（ADR-24 第七节 ② 要求的那条下界）。

### 20.3 本轮的门禁面

**三 preset 各 347/347、非 gtest 警告各 0 条、`tsan` 全量日志 `ThreadSanitizer` 出现 0 次**
（新增的并发面就是那一条采集线程跨线程读每 worker 的原子槽；槽是 `alignas(64)`、
`ReactorStats` 本来就是全 worker 共享的原子表，所以这条线程没有碰到任何非原子量——
`stats.h:25` 那条纪律的合法读法就是这个形状）。
`scripts/smoke-s5-01.sh`：**26 项 ✓、✗ 计数 0、退出码 0、跳过 1 项**（那一臂没装 `health`，
所以 `health_ejected_now` 的 HELP 限定语只能由 ctest #176 覆盖；**跳过＝不判，不是通过**）。
成本那格见 [BENCHMARK B-019](BENCHMARK.md)。证据落盘：`/home/archer/s5-01-foundation.log`
（改 `src/` 之前的 337/337 基线）、`s5-01-presets.log`、`s5-01-tsan-ctest.log`、
`s5-01-smoke-r4.log`、`s5-01-cost-p999.log`、`s5-01-storm8.log`、`s5-01-wrk-rejudge.log`。

**本轮 `src/` 改了七个文件、新增七个**：改的是 `src/net/reactor.{h,cc}`、
`src/net/reactor_group.{h,cc}`、`src/filter/rate_limit.{h,cc}`、`src/main.cc`；
新增的是 `src/metrics/{metrics_render,metrics_server}.{h,cc}`、`src/net/loop_observation.h`、
`src/net/proc_sample.{h,cc}`。
其中**动到被测方热路径的只有两处**，都不在每请求路径上：`TokenBucket::try_acquire` 里
"夹子咬到"那一格多一个 relaxed `fetch_add`（429 支路），以及循环已有那一拍里多一次
`snapshot_ != nullptr` 判空（关闭态恒假）。其余全部是新线程与渲染层。

## 21. S5-02 的阳性验证，以及配置热重载在真进程里抓到的东西（2026-11-03，ADR-10 落地追加节）

### 21.1 五处退化，其中两处第一版 0 红（改用例之外的东西才叫修）

按 §20.2 的同一立场：红的是哪几条用例是**跑出来的**，不是推出来的。每处改的是 `src/`，
临时标记 `TEMP-POSITIVE`，跑完逐处还原并核对残留 0 处，再复跑 `S5_02_*` 17/17。

| # | 退化（改坏的地方） | 实际红掉的用例（实测名单） | 备注 |
|---|---|---|---|
| H1 | **半套发布**：`build_config_snapshot` 里上游表建完就 `return snapshot`，路由那一半还没装进去 | **4 条红：`S5_02_Snapshot.RouteReplayKeepsOrderPatternsAndGroupIndexes`、`S5_02_Reloader.GoodReloadSwapsRoutesHealthPolicyAndMemberTableTogether`、`S5_02_Reloader.SyntaxErrorKeepsTheOldGenerationServing`、`S5_02_Publisher.OldGenerationIsNotFreedUntilTheLastReferenceGivesItUp`** | **第一版 0 红**：退化本来打在 `snapshot == nullptr` 那一格（"装配失败也 publish"），而六类拒绝**全都到不了那里**（语法/字段/越界/形状/超大都在装配之前就返回了）——打一条没人走的分支等于给自己的判定发空头证明。改打在装配的中途才有红。第四名是意外收获：`OldGeneration…` 断的是"钉住时不析构"，多一根半套快照的针就把在册数改了，说明它守的不止自己那格 |
| H2 | **引用计数漏一格**：`ConfigPublisher::publish` 顺手把新世代另存进一个静态 `vector` | **2 条红：`S5_02_Publisher.OldGenerationIsNotFreedUntilTheLastReferenceGivesItUp`、`S5_02_ReloadE2E.InFlightRequestsFinishOnTheOldGenerationWhileNewOnesUseTheNewOne`** | **第一版 0 红**：退化加在 `ConfigReloader::reload_tick` 里，而断引用计数的两条用例是**直接调 `publish()`** 的。改打在发布点本身——那才是"所有写者都走这一处"的那个位置，也是这条性质唯一的守门人 |
| H3 | **换代时把在册连接关掉**（那才是热重启的形状）：`sync_config_snapshot` 里遍历注册表逐个 `close_connection` | **1 条红：`S5_02_ReloadE2E.…`**（在飞那三条当场被截断） | 这条是"reload 不关任何连接"那句的唯一读者。它红了才说明那三条不是"恰好没连上" |
| H4 | **越界也生效**：`in_scope` 闸门短路（`if (false && !in_scope(...))`） | **5 条红：`RateLimitParametersAreOutOfScopeAndRefusedByName`、`RateLimitKeyChangeIsRefusedByName`、`SectionAppearingOrVanishingIsRefusedBecauseItIsChainShape`、`MysqlGroupIsRefused…`、`RejectLedgerIsConserved`** | 定义域那一族的账是守恒式（`rejects == syntax + scope`），闸门一坏守恒先红——这条红得最有说服力 |
| H5 | **世代不进请求**：链调用点把 `.config = gen.get()` 写成 `nullptr`（两个 filter 退回构造时那份兜底表） | **1 条红：`S5_02_ReloadE2E.…`** | "一次读取 = 一次决定"这条的落点。红的方式是"新请求没换表"，不是崩溃 |

一处**没测**（写在纸上，不留成口头保证）：追加节第六节末那条"失败的一轮可能已经把新端点 intern 进
ever-seen、烧掉几个没人引用的槽位"——本轮**造不出触发它的输入**。校验器与 1 MiB 上界把
`build_config_snapshot` 的失败面挡在外面（成员下标越界在 `build_gateway_config` 那一步就先拒了），
所以 intern-then-fail 这条路径今天只有代码形状、没有实测读数。要量它得能造出"装配中途失败"的输入，
归 S5 之后；不拿"逻辑上到不了"当作已经验过。

### 21.2 仪器侧：本轮有七处是**我自己的工具**错了

| # | 错在哪 | 症状 | 怎么被抓到的 | 修法 |
|---|---|---|---|---|
| 1 | **退出账把 11 个 `config_*` 打在了一行里**，而冒烟用行首锚取数 | `config_publishes=` 与 `config_snapshots_live=` 都读成空 → "总账对不上"和"世代没释放"两格同时红 | 打开 `gateway.log` 看那一行：11 格在**同一行**里，只有行首那一格能被行首锚命中。这是 S4-05 记过的同一格（一行多键 + 行首锚 = 其余几格静默读成空串，而空串在判定里长得像 0）——**同一条坑第三次踩** | `report_config_reload` 改成一 key 一行（与 `report_stats` 同一形状），理由写在函数头上；**不改判定脚本去迁就我写歪的账** |
| 2 | `date +%s%3N` 在本机**不截断**，给的是 19 位纳秒 | 生效延迟读成 `143794952 ms` 与 `-1614335120784833846 ms`（第二轮是负的），区间判断一起红 | 红得太荒谬：一个 200 ms 节拍的机制不可能量出 1.4 亿毫秒。**先怀疑取数方式，不先改判据** | `now_ms() { echo $(( $(date +%s%N) / 1000000 )); }`，并在脚本里写明"轴上的单位必须是毫秒"。修正后的实测是 **156 ms / 193 ms**（节拍 200 ms） |
| 3 | 在飞那格的"上游自报数"在**停上游之前**读 | `slow_served=?`，而客户端三条确实各拿到 192 字节体——一格红一格绿，形态不对 | 那一句是 `l7gate_test_upstream` **退出时**打的，我把读它放在了 `kill` 之前 | 先 `kill -TERM` + `wait` 再读日志。与 §20.1 第 7 条同族：**读一台进程的账，要在它把账写完之后** |
| 5 | 取 wrk 的 QPS 用 `sed -n 's/^Requests\/sec: \([0-9.]*\).*/\1/p'`，而 wrk 打的是**两个**空格 | 四行 QPS 全读成 `?`，而同一段代码里按字段取的 p50 有数——**一格空、隔壁那格正常**就是这类模式的指纹 | 判定表里只有 QPS 那一列全 `?`（§20.1 第 4 条同一形态） | 改 `awk '/^Requests\/sec:/ {print $2}'`，并**对已落盘的 `$dir.wrk.txt` 重取数**，不重跑负载（判定只认落盘输出） |
| 6 | 脚本头注释承诺了 `ONLY_ROUNDS` / `ARMS` / `SKIP_RUNS` 三个补跑旋钮，循环里**没实现** | 我按 `ONLY_ROUNDS=3` 起跑，实际把三轮全跑了一遍（6 次 45 s 负载） | 补跑的日志里出现了 r1/r2 两行——注释里有旋钮而代码不读它，只能从「它跑了我以为不会跑的轮」看出来 | 三个旋钮本轮补实现，含「补跑路径一律不删目录」那格守卫。这条是 `max_header_count` 那条雷的形状：**写了没人读就是空头证明** |
| 4 | `cleanup()` 末尾那句 `wait` 把脚本退出码盖成 0 | 冒烟打印 `SMOKE-S5-02 FAIL` 而 `SMOKE_RC=0`——判定与退出码分家，正是 B-016 那两条"带着看不见的 ✗ 的臂"的形状 | 我按 `SMOKE_RC` 读结论时发现与屏幕上的 FAIL 矛盾，才去核对 trap | trap 里 `local rc=$?` → 收尾 `exit "$rc"`。脚本的 FAIL 必须让 rc 也红，否则判定等于没跑 |

| 7 | **报给自己的一个数是估的**：提出「按文件大小推导健康表容量」时说「大约两万槽、≈6 MiB」，而最小成员文本约 24 字节 → 单代真上界 ≈41,943 槽、只省约 7 MiB；更致命的是上界必须封**跨代累积**（`ever-seen` 只增不减，第二代换一批全新成员就翻倍），单代推导根本不成立 | 代价：谁按我那个数做决策，会高估一倍收益、还会引进一类新的越界失效 | 实现之前把这条推导算了一遍（就是这一遍算出累积问题），所以 `src/` 一行没改 | 建议撤回并把「为什么这条推导不成立」照记在 ADR-10 追加节「七附」；这条与 §20.1 第 6b 条同族：**报出去的数要先确认是算出来的还是估出来的** |

另有**一条不属于仪器、是被测方**的：`main` 里留着一根首代快照的 `shared_ptr`（为了把策略交给健康表），
于是 `config_snapshots_live` 永远读成 2 而不是 1。这条是判据抓到的（第 21.2 节第 1 条修好之后才看得见），
修法是只留**那一格策略**的针、整份快照不留。写在这里是因为它证明"引用计数那一格不是装饰"：
它守的正是我自己会犯的那种错。

### 21.3 本轮的门禁面

三 preset 各 **364/364**（347 → 364，+17 条新用例）、非 gtest 警告各 **0** 条、`tsan` 日志
`ThreadSanitizer` **0** 次；**十一条冒烟**全绿（九条不依赖 Docker 的 + `smoke-s3-04` / `smoke-s4-04` 两条真容器，逐条 rc=0 且 ✗=0）；新增的 `scripts/smoke-s5-02.sh` 是**十一阶段 40 项 ✓、✗ 0、rc=0**，其中阶段 11 是 `--workers 4` 那条多 worker 臂（四个 worker 都换到第二代、而 `config_publishes` 只有 1 次）。
警告那一格本轮先在 clang 那两个 preset 上各多 1 条（`metrics_render.cc` 把 `uint64_t` 喂给吃
`int64_t` 的 gauge，`-Wsign-conversion`），GCC 的 debug preset 不报——**同一条代码在三个 preset 里
警告数可以不一样**，所以"非 gtest 警告 0 条"必须三个 preset 各数一遍，不能拿 debug 那一格代表全部门禁。

动到被测方的地方：`src/net/reactor.{h,cc}`（世代发布点、每请求一次 `load`、对账 tick 与钩子调用点）、
`src/filter/chain.h`（`RequestInfo` 多一个借用指针）、`src/filter/{route_filter,lb_filter}.cc`
（读世代、回落构造）、`src/route/{health,balance}.{h,cc}`（策略槽 + 组容量）、`src/main.cc`、
`src/metrics/*`（新增一族键，只在 arm 时发）。`src/config/gateway_config.cc` 一字未动——
**重载用的就是启动那一条校验路径**，没有第二份。
**每请求热路径上新增的只有一件事**：arm 时那一次 `atomic<shared_ptr>` 的 `load`（与 S3-04 每请求取
节点表快照同一格）；关闭态多的是一次判空。成本与常驻两臂的读数在 BENCHMARK **B-020**。

## 22. S5-03 的阳性验证，以及演示面自己抓到的东西（2026-11-03，ADR-25）

### 22.1 四处退化，其中两处**在第一版判据上根本打不红**

按 §20.2 / §21.1 的同一立场：红的是哪几条断言是跑出来的，不是推出来的。每处改的是仪器自己
（`tools/`、`deploy/` 与脚本），临时标记 `TEMP-POSITIVE`，跑完逐处还原并核对残留 **0 处**。
判定信号读的是 `build/smoke-s5-03/verdict.txt` 与日志里的 ✗ 名单，**不是退出码**（原因见 22.2 第 5 条）。

| # | 退化（改坏的地方） | 实际红掉的断言 | 备注 |
|---|---|---|---|
| E1 | **缺字段补 0**：exporter 对一个没有 `增量 pXXX =` 行的批次照样发一条 `…increment… 0` | **1 条红**：阶段 5「截断摘要：增量那一族对它整个不出现」 | 第一版即红。这条守的是 ADR-25 第三节第 1 条——把「没读到」画成 0 的曲线比没有曲线更坏 |
| E2 | **面板与查询分家**：把第 4 格第一条表达式改成 `health_ejected_now_TEMP_POSITIVE_E2`（一个不存在的键） | **1 条红**：阶段 8「第 4 格：4 条里只有 3 条查到 series，空的是 [#1]…」 | 这一格原来只判「至少一条有数」，四条里坏三条也能过；改成**每条都要有数**之后才抓得住 |
| E3 | **拿仪器替代被测方**：互背书等式的"网关自报"那一半也改从 Prometheus 取 | **1 条红**：阶段 9「这一轮没留下两条不同的取数通道」 | **第一版打不红**：那一版没有"取数通道"这一格断言，两边都读 Prometheus 会静悄悄相等。补法是 `metric()` / `prom_query()` 各写一份 `prov-*.txt` 指纹，阶段 9 断它们存在且不同 |
| E4 | **抹掉口径标签**：exporter 的 `labels()` 不再发 `canonic` 与 `conns` | **1 条红**：阶段 8「分位那一族少了 canonic 或 conns 标签」 | **同样第一版打不红**：那一版只在文案里 grep 措辞，不看查询结果里的标签。非标准档与 SLO 写死那档的区分必须在**数据**里，不在标题里 |

E3 与 E4 是同批打的（两处退化各自红各自那一条，✗ 计数 2、名单两条），E1、E2 各单独一轮。
另有三条**结构性**守门（不是退化，是断言形状）：① 阶段 9 的反向对照（`responses_completed` 与 429 那条
不相等）——不写这条，"取错键也能绿"；② 阶段 11 的 `requests_parsed` 恰好等于我自己发的条数（1+80=81），
配 `metrics_scrapes_ok=151` 那一格下界——把「抓取不进被测方」在真容器、151 次抓取的形状下再钉一遍；
③ 阶段 12 的 `fd 12→12`、`constructed==destructed==86`，以及「没 arm 就没有 `config_` 键」——
S5-02 那条关闭态口径不因演示面而松动。

### 22.2 仪器侧：本轮有 **11 处**是我自己的工具错了

| # | 错在哪 | 症状 | 怎么被抓到的 | 修法 |
|---|---|---|---|---|
| 1 | `deploy/prometheus/prometheus.yml` 写死 18522/18090，而冒烟脚本另用了 18933/18934 | 两条 target 全 down → 第 6 阶段红、第 8 阶段六格全红、第 9 阶段 Prometheus 侧全空，**屏上 12 条 ✗ 里 10 条是这一条造成的** | 打开 `targets.json` 看 `discoveredLabels`：里面是 `host.docker.internal:18522` 而网关在 18933 | 脚本端口改成与入库配置一致，并在端口旁写一行理由：**测的必须是入库那一份**（一次错、多处红，别按 ✗ 条数找 bug） |
| 2 | `LIMITED="$G_PORT/limited"` 拼出 `http://18930/limited`——没有主机 | 那 60 条 curl 一条都没打到网关，429 恒 0；同时把阶段 11 的"恰好等于我发的条数"拽成 21 vs 92 | 两条断言同时红，而 `requests_parsed` 那 21 条恰好等于我发的**另两批**之和 | 补上 `127.0.0.1:`；curl 条数改成脚本自己累加的 `BUSINESS_CURLS`，不再手写常量 |
| 3 | 限流键是 `path`，我用 40 条**各不相同**的路径去制造 429 | `rate_limit_rejected` 恒 0，读起来像"限流没生效" | 同一批 curl 换成同一条路径后立刻 34 条 429 | 每条路径一只新桶（burst=5）→ 突发必须打在同一条路径上。这条是被测方语义，不是代码错，但我第一轮把它当成了仪器失灵 |
| 4 | 从面板 JSON 取表达式：按 `"id": "`（带引号）找格号，而 JSON 里是 `"id": 1,`；转义引号 `\"` 没还原 | 六格全判空（第一次）；第二次仍全判空，因为 `sub(/^ *"expr"…/)` 要求行首而 `"expr"` 前还有 `"refId": "A",` | 把提取出的 `panel-exprs.tsv` 用 `cat -A` 打开：格号有了、表达式却整行带着 `refId` | 先 `.*"expr": *"` 贪婪定位，再把 `\"` 换占位符、在第一个真引号处截断、还原。**修了两次才对**——两次都靠"把中间产物落盘再看"，不靠重读代码 |
| 5 | **退出码传染**：`FAILED=1`、`exit "$FAILED"` 走到了（`TEMP-DEBUG` 钉死），进程状态仍是 0 | 「SMOKE-S5-03 FAIL」打在屏上而 `SMOKE_RC=0`——判定与 rc 分家，**§21.2 第 4 条那一族的第四次** | 我在门禁里按 rc 读结论时发现与屏幕矛盾，才去查 trap | 消融 **11 组**（v6–v9、v11–v14、v16–v22）+ **4 个截断版**（head -n 131/200/290/400/470）全部 rc=1；真脚本全跑就是 0，**我没归因到具体那一句**。处置：判定写进 `build/smoke-s5-03/verdict.txt`，门禁读盘；不拿"最小复现都能过"当作没问题 |
| 6 | `fail()` 用布尔当计数（`FAILED=1`），汇总句却写「✗ 计数 $FAILED」 | 屏上 6 条 ✗ 而汇总说 1 | 同一屏幕上 ✗ 行数与计数对不上 | 分成 `FAILED`（布尔，判 rc）与 `FAILS`（计数，进汇总） |
| 7 | 半开是**瞬时态**：我在同一个循环里"发一条业务请求 + 读一次 gauge" | `circuit_half_open_now` 全程 0，读起来像"到期→半开那条路没走通" | 逐目标键 `half_open_reopened` 有数——半开确实进过、又被我那条请求当场关掉 | 摘除之后停止发业务请求、只读观测面（到期由循环那一拍的 `sweep_expired` 推进），`half_open_timeout_ms=8000` 留窗口。**这条不是取数错，是我把 1.31 第三节那句"一次业务失败立即回摘"忘了** |
| 8 | exporter 的 `sockaddr_in::sin_port` 忘了 `htons` | `bind`/`listen` 都返回成功、横幅照打，`ss -ltn` 里没有那个端口、curl 被拒；而 `--once` 那条路径完全正常 | "一端能用、另一端拒"的形态（§20.1 第 4 条同族） | 补 `htons(...)`。这条与 #1 叠在一起，才让第 6 阶段看起来像纯配置问题 |
| 9 | 我把 `health_t_ejections_total_check` 这个**我想象出来的键**写进了面板第 5 格 | 那一格会永远空（Prometheus 里没有这个名字） | 提交前把面板所有 `expr` 与网关 `/metrics` 的键集合对了一遍 | 换成 `{__name__=~"health_t.*_ejections"}`。**"面板上画得出来"不等于"键存在"** |
| 10 | 用 Write 通道改 `scripts/*.sh` 会丢执行位 | 两次 `Permission denied`（第二次就在我 `chmod +x` 之后又编辑了一次） | 命令直接报错 | 每次运行前 `chmod +x`；入库靠 `git add` 记 mode 位 |
| 11 | `scripts/verify-all-presets.sh` 的 grep 只留 `tests passed\|tests failed` 两行 | debug 那行报「1 tests failed out of 218」而**失败用例的名字被滤掉**（`stopOnFailure: true` 提前停，218 是跑到第几条、不是总数） | 我去复核名字，发现手里只有一个数字 | grep 补 `Test #.*Failed` 与 `***`。**与 B-016 追认一同形**：红只打在 stdout、汇总里一行不留——写观测纪律的人第四次踩自己的纪律 |

22.2 第 5 条是本轮唯一**没能归因**的一条，我不把它写成"已修"：两处设防（trap 里 `exit`、主体末尾 `exit`）
在最小复现里都是 rc=1，在真脚本里都是 rc=0。所以这一条的结论只能是"门禁改读落盘判定"，
而"rc 为什么在这种形状下传不出来"留着——**归因不出来的东西，不许用一句"环境奇怪"结掉**。

### 22.3 本轮的门禁面

**用例数不变**：本轮 `src/` 与 `tests/` **一字未动**（`git diff --stat` 只有 `tools/`、`deploy/`、
`scripts/`、文档四族），所以门禁面是"不回退"而不是"抬升"：

- 三 preset 各 **364/364**（`scripts/verify-all-presets.sh`：debug / asan-ubsan / tsan，各 configure rc=0、
  build rc=0、ctest rc=0），非 gtest 警告各 **0 条**；`build/release` 另建一次（README 快速开始走 release）
  也是 rc=0、非 gtest 警告 0 条。
- **tsan 单独再跑一遍并留全量日志**（`./build/tsan/tests/l7gate_tests` → `/home/archer/s5-03-tsan.log`）：
  362 PASSED + 2 SKIPPED（`S2_07_MemoryComposition` 那两条，跳过＝不判），日志里 `ThreadSanitizer` **0 次**。
  为什么不只信 ctest：`--preset` 那条路径只在失败时打印用例输出，**race 警告不会让任何用例变红**，
  信 ctest 的 rc 就等于把"0 race"这句话建在一个不读它的通道上（同 §19.3 那条会打谎话的 ✓）。
- **十二条冒烟**逐条 ✗ 计数 **0**（新增第 12 条 `scripts/smoke-s5-03.sh`：十二阶段 40 项判定、
  `build/smoke-s5-03/verdict.txt` = `PASS checks=40 fails=0`）。
  照记一条不体面的：`smoke-s5-01` 第一次跑是 `fatal: 端口 18521 已被占用` —— 那是我自己那一轮被中断的
  驱动留下的尾巴，不是它的回归；复跑通过。**不写成"一开始就过"**（先例是 S3-04/S4-04 那两条因引擎停机
  在阶段 0 自拒、引擎恢复后补绿，按时间顺序记）。
- 本轮 rc 这一信号**不可用**（22.2 第 5 条），所以"逐条 rc=0"这句在 S5-03 这一条上换成
  "逐条 ✗ 计数 0 + s5-03 读 verdict.txt"。其余十一条的 rc 与 ✗ 计数同时读，都干净。

### 22.4 本轮没做与没量的（写在纸上）

- **面板截图**：验收句① 给的是"截图/dashboard.json 入库"这一个或，我交的是 JSON + 那 20 条表达式在
  Prometheus 里的查询原文（`build/smoke-s5-03/p8-panel*-expr*.json` 全落盘）。截图这条路本机不可靠
  （内置浏览器面板受限、截图与 rAF 不可信），不拿它当判据。Grafana 侧入库证据走
  `/api/dashboards/uid/l7gate-gateway-overview`（阶段 7），那也是 API 而不是眼睛。
- **分位那一格的"连续"**：exporter 发的是**批次级**读数（跑一轮压测进一格），不是运行期连续监测——
  面板第 2 格的标题与 `# HELP` 都这么写。要做成连续的，得给网关加 histogram/summary，那是翻 ADR-24 的活。
- **canonic=1 那一档在冒烟里没跑**：冒烟第 5 阶段的 fixture 取的是 `build/bench/` 下已有的落盘摘要
  （本轮读到的是 B-019 那批 `canonic=0` 的），所以面板上那条标准档曲线要有标准档的落盘文件才画得出来——
  这是**数据在不在**的问题，不是判据松了。S5-07 终判那轮会产出 canonic=1 的两批。
- **`s1-p99.sh` 那一条 wrk 线的摘要没进 fixture**：本轮 fixture 只用 HDR 那一份；wrk 解析路径由
  阶段 5 的逐数对盘 + `build/bench/s1-p99-*/` 的实际读数覆盖到（我在实现时用它验过 ms→µs 那一支）。
- **exporter 自己的资源占用没量**（它是仪器、不在被测方路径上；ADR-25 第五节用"先压测后起监测"
  这条顺序规矩处理了同机抢 CPU 的风险，而不是靠一条没测的账）。

## 23. S5-04 的阳性验证，以及演示页这条线自己抓到的东西（2026-11-04，ADR-26）

这一条任务的被测面只有一页 HTML 和一个测试夹具，但它把我这一族坑几乎各撞了一遍：**取数方式错、
预言错、门槛吸收证据、跨系统换行符**。全部照记，读数一个没改。

### 23.1 五处退化（W1–W5），其中两处**第一版红在别处**

| 编号 | 退化打在哪个机制上 | 干净跑的样子 | 红的样子 | 第一版的问题与处置 |
|---|---|---|---|---|
| **W1** | `--ws-echo` 回帧时在载荷前加一个字节（`tests/ws_frame.h` 的 text/binary 支路） | 句① 两条 `recv n len=6 val=s504-*` 与发送值逐字相等 | ✗2：第 1 条与第 2 条"消息流里没有那两行逐字相等的记录" | 一次到位。同一处退化也被 ctest 的 `out.substr(2) == msg` 抓住（那条断言是精确相等，不是长度相等） |
| **W2** | accept 不按 key 现算、改抄 RFC 样例那一串固定值 | 浏览器 `open`，冒烟成交轮 | ✗2（冒烟）：8 轮全空哨兵 + 六次同轴无成交轮 | **第一版冒烟红而 ctest 12/12 全绿**——我的端到端用例用的正是样例键，它的正确 accept 恰好等于抄死的那一串，"现算"与"抄样例"在这一格读不出区别。换成一个非样例键（`x3JJHMbDL1EzLkh9GBhXDw==` → `HSmrc0sMlYUkAGmm5OPpG2HaGWk=`，值由 python3 现算）之后 ctest 也红（1 FAILED）。**这是"用例的键选得刚好让退化看不见"那一族** |
| **W3** | 429 那条等式的被测方那一半换成页面自己的数（`D_REJECTED=$GOT429` + 指纹两边同源） | 三处逐数相等：文案 7 == 哨兵 got429 7 == Δrate_limit_rejected 7 | ✗1：「互背书形状不成立：两条通道同源」 | **等式本身在退化下仍然绿**（页面==页面恒真）——红的是我补的那道通道守卫（`prov-429.txt` 两行、断不同源 + 被测方必须是 `http://` 读数）。没有守卫时这条退化是 0 红。形状照 ADR-25 第八节 E3，但**必须先有守卫再打红**，否则"阳性验证"是自证 |
| **W4** | 页面收到第一条回显就 `close()`（句④ 那格要的东西不发生） | `same-connection recv=2 readyState=1` 那一行在 | ✗10，其中句④ 那格明确红：「没有 same-connection 那一行」 | **第一版只红 2 条、且都红在门槛上**：脚本把"哨兵是 DONE"当成后续所有判定的收货条件，W4 让八轮全判 INCOMPLETE → 阶段 4–8 整段被跳过，句④ 那一格根本没被执行。改成像读证据而不是像收货：有结论（DONE/INCOMPLETE）就收，`DONE` 自己只是一条判定。改完红 10 条 |
| **W5** | 页面载入即写死一个漂亮的 `DONE …… recv=2 got429=7` 哨兵，并把自动序列当场停住 | 哨兵与消息流 mutually 一致 | ✗13：句①②④ + 网关 `Δupgrades_101=0` + 上游自报 `1/3 → 0/0` + 退出账界 | 这条正是 ADR-26 第六节要的形状：**硬编码的哨兵骗过了收货，骗不过逐条证据**。它同时证明"哨兵不是证据本身"，证据是消息流那一格 + 两条独立通道 + 上游自报 |

W4 那跑还带出一条**我没归因的差分**：退化跑里读到 `live_connections=1`、`constructed=7 / destructed=6`，
而干净跑同一格是 `live=0` 且配对相等。我没有把这条归因到机制上（它出现在退化路径上、不在本任务定义域内），
按"退化跑出现、干净跑没出现"照记；S5-06/S5-07 若再见到同一形状，单独立项查池侧那条连接的收尾。

### 23.2 仪器侧：本轮我有 **11 处**自己的工具错了（多数会造出比事实更强的结论）

| # | 错在哪 | 如果没被抓到会被读成什么 | 怎么被抓到的 | 处置 |
|---|---|---|---|---|
| 1 | **凭印象写 SHA-1 基准值**（`sha1('a'×56)` 我写了一个看着像的十六进制串） | 单测会钉住一个错的期望值，实现反而被判错 | 用 python3 现算对了一遍才发现不同（真值 `c2db330f…`） | 三条摘要基准（空串、`abc`、56/64 长输入）与四条 accept **全部改为独立工具现算**；这条也解释了我为什么把另一条 accept 的记诵值（`…AGmm7OPpuTmB1rQ=`）换成现算的 `…AGmm5OPpG2HaGWk=` |
| 2 | 端到端用例把 101 头长写成 `read_exact(&head, 133)` | 换成另一档 accept 长度就"读不到→超时"，看起来像上游不回话 | 手算长度发现两档不同（现算 28 字符 / 假值 40 字符） | 改成按 `\r\n\r\n` 终止串读（`read_head`） |
| 3 | 端到端用例用了 **RFC 样例键** | 见 W2：抄死样例值也能全绿，ctest 对这一格是盲的 | 打 W2 时"冒烟红、ctest 绿"这个矛盾本身 | 换非样例键，并把理由写进用例注释 |
| 4 | 把 `DONE` 当**后续所有判定的收货门槛** | 退化只红在门槛上，句④ 那格根本没跑，却像"已经判过" | 打 W4 时红数比预期少且红错格 | 有结论就收货，`DONE` 降为一条普通判定 |
| 5 | 哨兵取数用 `awk '/<output id="s504-verdict">/'` **不锚行首**，而那串字面量在页面注释里也出现 | 把注释里的「是\*\*完成哨兵\*\*」当成哨兵内容读，八轮全判"没拿到"，看起来像页面根本没跑 | 屏幕上一致地报 Δrejected=7、Δupgrades=1（机制明明是通的）而哨兵为空 | 取数器改为**行首锚定**；并加一条诊断：空哨兵时打印"行首就是该标签的行有几条"（0 条＝取数器问题，1 条＝页面没跑完）——这一族是 §20.1 第 6 次应验 |
| 6 | **整篇文档当证据**（`grep -c 'recv text'` / `'429 rate limited'`） | `--dump-dom` 含页面 `<script>` 源码，源码里就写着那几串 → 假绿（实测整篇命中 2 vs 限定 1；文案那格 9 vs 7） | 我先用整篇数出 2 次命中，觉得数不对才去看来源 | 判定只读限定元素；并把"整篇 > 限定"做成一条对照判定（阶段 7），让这条坑变成被钉住的规矩而不是我的教训 |
| 7 | 没处理 **CRLF**（Windows 侧 `chrome.exe` 写出的快照行尾是 `\r\n`） | `grep -x 'sent s504-a'` 整行匹配全部落空 → 句①②④ 一起红，红的是我的行尾处理 | 一次跑里"消息流明明有内容却判 ✗"，把快照 `cat -A` 打开看到 `^M$` | 取数统一 `tr -d '\r'`，快照文件按原样留档（那是证据） |
| 8 | 预言 `upgrades_101 ≥ 3` | 干净跑读到 2 就红，看起来像漏计了升级 | 该键是**整个进程**累计值，等于实际跑过的轮数（本轮 stage3 与 stage9 各一次成交） | 改成可推导的双侧界 `2 ≤ UPG ≤ ROUNDS`（下界＝已证明的成交轮，上界＝跑过的握手次数）；**不预言一个猜出来的数** |
| 9 | 预言每轮"回显帧数 = 2" | 读到 3（或累计 5）就被判成"多回了一帧"，像实现越界 | 数了一遍帧型：两条消息 + 一条 **close 的回帧**（RFC 6455 要求对 close 回一条） | 按机制改成 3/轮并把理由写进脚本；这是"机制预期与观测不符→先弄清观测的是什么"那一族 |
| 10 | 阶段 9 两轮**共用一台上游进程** | 空哨兵那一轮已经回了 2 帧，累计 5 ≠ 预言 4；看起来像"回了一帧多余的" | 第一次跑就撞上（fails=1） | 改成**一台新进程只负责一轮**：不成交就整台重启，成交那一轮的自报数天然干净。重试次数不进账本 |
| 11 | 探针命令串里两处 **shell 变量被 wsl.exe 吃掉**、以及 `pkill -f 'probe_ws_raw.py'` 匹配到自己的命令行 | 耗时读成 `0.00`；以及自己把自己杀掉（rc 15），看起来像"命令没跑起来" | 输出与预期形状不符，回看项目记忆里那两条通道坑 | 带变量/循环的命令一律先 Write 成脚本文件；pkill 模式加括号首字符 `[p]robe…` |

一句总结这一节为什么值得写下来：**这 11 条里没有一条是被测方的错**，而其中 6 条（#1、#3、#4、#5、#6、#8）
如果没被抓住，都会产出**比事实更强**的结论——"实现算不出 accept""句④ 已判过""整篇文档就是证据"
"退化没红说明判据松了"这类。演示面这一族的危险不在于它难写，而在于它**很容易让人用眼睛替判定背书**。

### 23.3 本轮的门禁面

**用例数抬升**（本轮 `src/` 仍一字未动，动的是 `tests/` 与 `scripts/`、`demo/`）：
新增 `tests/ws_frame_test.cc` **12 条**（握手值 4 条 + base64/SHA-1 基准 2 条 + 帧 5 条 + 真连接端到端 2 条，
其中端到端那两条一条钉"开着 `--ws-echo` 时 accept 现算 + 回未掩码帧"、一条钉"**默认档不许变**"）。

- **三 preset 各 376/376**（364 → 376，`scripts/verify-all-presets.sh`：debug / asan-ubsan / tsan
  各 configure rc=0、build rc=0、ctest rc=0），**非 gtest 警告各 0 条**；落盘 `/home/archer/s5-04-gate.log`。
- **tsan 全量直接跑**（`build/tsan/tests/l7gate_tests` → `/home/archer/s5-04-tsan-run.log`）：
  **374 PASSED、0 FAILED、日志里 `ThreadSanitizer` 出现 0 次**（374 + 2 SKIPPED = 376，
  跳过的仍是 `S2_07_MemoryComposition` 那两条，跳过＝不判）。不只信 ctest 的 rc，理由同 22.3。
- **十三条冒烟**逐条 ✗ 计数 **0**，且第十三条读落盘判定：
  `smoke-s5-03` → `PASS checks=40 fails=0`、`smoke-s5-04` → **`PASS checks=29 fails=0`**（新增的第 13 条，
  十个阶段 29 项判定，含"整篇命中 9 > 限定命中 7"那条取数方式对照）。
  `smoke-s5-04` 这一条的 rc **本轮读到的是 0**，但它第一次判红（`fails=1`，阶段 9 那格）时 rc 同样是 0——
  22.2 第 5 条那一族在这条新脚本上**又应验了一次**，所以判定只认 `verdict.txt`。
  `smoke-s2-05` 在本轮仍然绿（✗=0），这是"默认档逐字节没变"那条断言的外部证据：
  它那句"收到的恰好 = 101 头 + 发出的 2048 帧字节"如果被我重组过一次就会红。
- **五处退化的红数**（W1 2 条 / W2 冒烟 2 条 + ctest 1 条 / W3 1 条 / W4 10 条 / W5 13 条）
  全部真打红、逐条还原后复跑 `verdict.txt = PASS`，落盘在 `build/fault/s5-04/{w1,w2,w3,w4,w5}.log`
  与两份干净跑（`clean-2.log`、`clean-final.log`）；还原后 `grep -rn 'TEMP-W' tests/ demo/ scripts/ src/` 为空。

### 23.4 本轮没做与没量的（写在纸上）

- **页面没做 RTT 与"逐条带方向的帧"**：契约 4.2 的三块（地址栏 / 消息流 / 输入框）齐了，
  README 那一节原先的规划文案里那两样按偏离照记在 README 里，没偷偷算成已实现。
- **`--ws-echo` 那台上游的资源占用与 SHA-1 每次握手的成本没量**：它是夹具，不参与任何 SLO 测量；
  要量也得先回答"量它服务于哪个数字"（契约 1.2）。
- **`wss://` 与分片帧没验**：契约 1.3 不做 TLS；夹具对分片帧直接判畸形（不拼装），
  浏览器发小消息不会产生分片，所以这条路径本轮**没有输入可造**——形状与 §21.1 末那条"造不出输入"同族。
- **句③ 的 `wasClean` 那一格没进判定**：实测稳定读到的是 `closed code=1005`，`wasClean` 我没取
  （ADR-26 第五节明写了这一点，不留"看起来判过了"的印象）。
- **浏览器与网关同机的干扰没量**：这条线不产出三个数字里的任何一格，所以按 ADR-25 第五节那条
  顺序规矩处理（不得与已归档的 canonic 读数混比），而不是靠一条没测的账。

## 24. S5-05 的阳性验证，以及编排这一层抓到的东西（2026-11-04，ADR-27）

### 24.1 五处退化（C1–C5），以及 C1 顺手抓到的一条被测方事实

每一处都是**改在"所有写者必经的那一处"**（compose 文件、网关配置、脚本的清理路径），不是改在判据的反面。
红数是 `build/smoke-s5-05/verdict.txt` 的 `fails=`，不是屏幕上的行数（§22.2 第 6 条那条立场）。

| # | 退化 | 红在哪几格 | 红数 |
|---|---|---|---|
| C1 | 把网关配置里第一台上游的 `address` 从静态 IP 改成服务名 `upstream-a` | 阶段 1（两处写同一个事实不一致）、阶段 3（`upstream_connect_failed 0→2`）、阶段 8（`upstream-a=0` 一条都没接到）、阶段 5–6 连带五条格 | **9** |
| C2 | 把壳镜像的 tag 换成 `latest`（四处） | 阶段 1「自产物那三台走 l7gate/runtime:s5-05」+「latest 出现 4 条」 | **2** |
| C3 | 给 `gateway` 也加 `cpuset: "0"` | 阶段 1「cpuset 落在[gateway redis ]」+ 阶段 9「网关也被绑成 0」 | **2** |
| C4 | 删掉 prometheus 那台的 `extra_hosts` | 阶段 1「extra_hosts 出现 1 次」 | **1** |
| C5 | 让收尾的 `down` 不执行 | 阶段 10「残留 8 条」+「还有 1 条网络」 | **3**（其中一条是我自己的取数错，见 24.2 第 4 条） |

**C1 里那 2 次的红值得单独记，因为它不是"路由坏了"**：配置只挡空串、不挡字符集，所以带连字符的
地址能一路走到观测面——`health_t0_upstream-a_18081_ejections` 这个名字在 Prometheus 名字法里非法
（`[a-zA-Z_:][a-zA-Z0-9_:]*`），后果不是那一格坏，是 **`/metrics` 整份抓失败**：
`targets.json` 里 `lastError = invalid metric type "a_18081_ejections counter"`，那台网关整体变 down，
101 个键一起丢，于是第 1/3/4/5/6 格全空。**级联的九条红里只有三条是路由的直接后果**——
这正好是 §22.2 第 1 条那句"别按 ✗ 条数找 bug"的又一次应验，只不过这次坏的是暴露面自己。
ADR-24 那套机械变换当时只处理了 `.` 与 `:`，因为它假设 `address` 是 IPv4 字面量；
而"网关自己只认字面量"这条约束（本节 P-2）恰好意味着**带非法字符的地址是走得进配置的**。
这条不在 S5-05 的定义域里，`src/` 与 `tests/` 一字未动，登记成待裁（TASKS「已知待决事项」）。

**C1 之后仍然绿的那一格要点名**：`0 + 12 == 12`（上游自报之和 == 我发的条数）在退化下**照样绿**——
因为第一台连不上就摘除（`max_fails=2`）、剩下的请求被重试/直接派给第二台，所以"和等于我发的"这一句
挡不住"只有一台接到"。真正抓住它的是同阶段那条**下界**（`每台各自 ≥1`）。
上界配下界这条规矩在这一格里不是口号：少写那一条，C1 就少一处红。

**C4 的机制预测被实测否掉了，照记**：我原先（以及 `deploy/prometheus/prometheus.yml` 顶上那句注释）
写的是"不带上 `extra_hosts`，这两个 target 一起变成假目标"。真删掉重跑，**两条 target 仍然全 up**——
容器里 `/etc/hosts` 确实没有那一行，而名字照样解析到 `192.168.65.254`（Docker Desktop 的内置 DNS 会给）。
所以那句说重了，已就地更正两处注释（`prometheus.yml` 与 compose 里那台的说明）并把冒烟的 fail 文案改成
它实际守的东西：**判的是文件里写没写，不承诺"不写就抓不到"**。留着那一行的理由换成
"不把抓取通路押在 daemon 的自动行为上"。这是"机制预期与观测不符 → 先弄清观测的是什么"那一族的又一次。

还原后 `grep -rn "TEMP-" deploy/ scripts/ src/ tests/ demo/` = **0 处**，复跑 `PASS checks=66 fails=0`。

### 24.2 仪器侧：本轮我有 **6** 处自己的工具错了（其中一处会造出假绿）

| # | 错在哪 | 如果没被抓到会被读成什么 | 怎么被抓到的 | 处置 |
|---|---|---|---|---|
| 1 | `grep -c 'network_mode'` 数到了 compose **头部注释里那句禁令本身** | 第一跑就报"出现 network_mode 共 1 条"——看起来像编排真的写了 host | 我读 ✗ 行的时候认出来那一条是注释（文件第 5 行） | 锚成 YAML 键：`^ *network_mode:`。**这一族本轮踩了两次**，见第 4 条 |
| 2 | `redis-cli CONFIG GET save` 的输出是**两行**（键名行 + 值行），我把整个输出当值读 | `save` 关掉那一格永远判"没关掉"；反过来如果我"顺手"接受非空，就等于把一件没验的事写成验过 | 红之后把原样输出打开看：第一行就是字面量 `save` | `tail -n +2` 取值，并把 `appendonly=no` 一起读进来（两格而不是半格） |
| 3 | `docker compose logs` 每行带 `<service>-<n>` 加一条竖线再加空格的前缀，而退出账那些行的锚是**行首**的 `[serve]` | 三条生命周期判定红（这还算诚实），而**"没 arm 就不发 config_ 键"那一条会因为整行读不到而空判成立 → 假绿** | 同一阶段里 `live_connections` 读成"空"而 `grep -c live_connections` 明明有 1 条，两处矛盾才逼我去看行首 | 改 `logs --no-log-prefix`，并加一条**非空守卫**：读不到行首 `[serve]` 就先把"红的是取数"写死，再让下游判定自己红 |
| 4 | `grep -c 'host.docker.internal:host-gateway'` 又数到注释（我为 C4 新写的那段解释里含这一串） | C5 那跑多红一条与退化无关的格子，看起来像"残留判据把 extra_hosts 也判了" | 红的位置在阶段 1，与 C5 改的东西毫无关系 | 锚成列表项 `^ +- "host\.docker\.internal:host-gateway"`。**同族第二次**（第 1 条），区别只是第一次红在头部注释、第二次红在我为退化新写的注释——**为解释而写的注释本身就是取数器的陷阱** |
| 5 | 对照臂第一版的载荷解析照抄了 S5-04 的形状（读响应体的 `case` 模式），而这一格要读的是网关自报的计数 | 读数会稳定拿到空值，于是"红在连不上"这一格其实没被执行——与 §23.2 第 4 条同一形状 | 手一碰就知道模式对不上 | 给对照容器也开 `--metrics-port`，直接读 `upstream_connect_failed`（两条请求两条失败，`2`），把"红在哪一格"钉住 |
| 6 | 门禁脚本里数 tsan 的用例数用了 `grep -c '^\[  SKIPPED \]'`——**gtest 收尾还会把跳过的那几条再列一遍**（`[  SKIPPED ] 2 tests, listed below:` 加两条清单项），于是同一条被数进去两次 | 会写成「374 PASSED + **5** SKIPPED」，比事实强一条（376 的账对不上却看起来"有出处"） | 落盘日志里 `[  PASSED  ] 374 tests.` 与 `[  SKIPPED ] 2 tests, listed below:` 两行本身就把正确的数报给我了，两处矛盾才回看计数式 | 读汇总行那一格（`^\[ *(PASSED\|SKIPPED\|FAILED) *\]`），要清单就 `grep -v "listed below"`；**376 = 374 + 2 这条加法进判定**，不进我的口算 |

一句总结：**这 6 条没有一条是被测方的错**，其中第 3 条如果没被抓到，会产出一条**比事实更强**的结论
（"关闭态不发 config_ 键"在容器这一层被"验过"，而它一次都没有被执行）。编排这一层的判定全都靠取字符串，
所以取字符串的锚一错，整条栈的绿就只是我的 grep 的绿。

### 24.3 本轮的门禁面（2026-11-04，S5-05 收尾实测，落盘 `/home/archer/s5-05-gate*.log`）

本任务**不动 `src/` 与 `tests/`**，所以门禁面读的是"不回退"而不是"抬升"：

- `cmake --build build/debug -j 16` 全目标 rc=0（口径⑪：跑冒烟之前必须全目标）；
- `scripts/verify-all-presets.sh` rc=0，三行都齐 —— debug / asan-ubsan / tsan **各 376/376**、
  `warning_total=0` 且**非 gtest 警告各 0 条**；
- **tsan 全量直跑** `build/tsan/tests/l7gate_tests`：rc=0、`[  PASSED  ] 374 tests.` +
  `[  SKIPPED ] 2 tests`（跳过的是 S2_07 那两条 `mallinfo2` 在 sanitizer 下恒 0 的格子，跳过＝不判）、
  日志里 **`ThreadSanitizer` 出现 0 次**。不只信 ctest 的 rc——race 警告不让任何用例变红；
- `cmake --build --preset release -j 16` rc=0、非 gtest 警告 0 条，并用 `L7GATE_PRESET=release`
  起了一次栈（验 README 那条默认路径）：`up -d --build` rc=0、业务面 `200` + 正文 `seq=1...`、
  `requests_parsed=1`、`down` rc=0 且残留 0 条；
- **十四条冒烟逐条 rc=0 且 ✗ 计数 0**（第 14 条是本轮新增的 `smoke-s5-05.sh`，十一个阶段 66 项判定、
  `build/smoke-s5-05/verdict.txt` = `PASS checks=66 fails=0`；第 12、13 条同样只读 verdict.txt：40 项 / 29 项）。
  真容器那两条（`smoke-s3-04` MySQL 8.0.36、`smoke-s4-04` redis:7.2.5）这一轮在门禁批次里一次跑过，没补跑；
- 收尾核查：`l7gate-` 前缀的残留容器 **0** 条、网络 **0** 条，同机 `rgbt-*`/`knowstack-*`/`tinrpc-*`
  开工前后同为 **6** 条。

一句不体面的照记：本轮第一次跑 `smoke-s5-05` 就有 5 条红，**五条全是我自己的取数错**（§24.2 第 1–3 条），
被测方那一侧一条没红——这不代表编排简单，代表这一层的判定几乎全是"从文本里取一个数"，
所以取数器的锚一错，绿与红都不作数。

### 24.4 本轮没做与没量的（写在纸上）

- **没有量容器拓扑下的任何一个性能数字**：这一栈只服务演示面（ADR-27 第二节第 6 条）。
  进容器多一次 DNAT/userland proxy，那一格的成本**本轮没有读数**，也不许被读成"免费"或"可忽略"——
  它归 S5-07 那一臂（已裁第 ⑨ 条第 4 项：网关与 nginx 同拓扑、两侧都进容器），届时按取样规程跑。
- **Redis 绑核那一格在编排里只验了"生效"，没验"隔离"**：两处证据（`HostConfig` + `cpuset.cpus.effective`）
  证明 Redis 只在 15 号核上跑，而**网关仍可被调度到那一格**——这条竞争没被消除，B-017 的取舍原样有效。
- **MySQL 起而不用**：健康、可达、schema 建好，但没有一条请求因为路由需要而走到它。
  接 `--nodes-db` 的代价（撞 ADR-10 追加节第七条，那台就不能 arm 文件重载）写在 ADR-27 第三节，归项目所有人裁。
- **`--ws-echo`/`--html` 那条演示页通路没进编排**：S5-04 只实测过"上游发页面 + 页面经网关同源"，
  而这一栈的形状是静态 IPAM；两条通路混在一起没有实测支持，所以不混。
- **壳镜像里那两个库的版本没做与宿主的逐字比对**：compose 构建时装的是 ubuntu:26.04 的 apt 候选
  （`8.4.11-0ubuntu0.26.04.1` / `1.2.0-6ubuntu4`，与契约 9.1 字面命中），而**宿主的 .so 是构建产物链接的那一份**——
  两处版本号同源，但不是同一个 inode。这个区别值得记着，因为一旦有人拿"镜像里也是宿主那份"去解释什么，
  这句话没有证据。
- **静态网段的可复用性没测**：`172.31.250.0/24` 本轮可用；如果哪天与别的 bridge 冲突，
  `up` 会直接失败（脚本阶段 0 判"起不来"，不是判"起来了但几格空"）。没有测"冲突时会不会静默换网段"，
  因为那需要先造一个冲突条件，而它不属于任何一条验收句。
- **S5-04 留的那条未归因差分本轮没再遇到**：`live_connections=1 / constructed 7 / destructed 6` 那个形状
  在 S5-05 的干净跑与五处退化里都没出现（容器里读到的是 `live=0` 与 `constructed==destructed`）——
  照记一次"没再见到"，不写成"已消失"。
## 25. S5-06 的 netem 现场，以及一条被实测改写的机制句（2026-11-04，ADR-13 落地）

### 25.1 六臂对照：注入的是网络，不是那台上游

**这一节在问什么**：§12/§18 的故障全是**应用层**注入——让上游自己慢、自己不读字节、自己回 503。
S5-06 问的是同一批判据在**延迟/丢包来自网络层**时还站不站得住。脚本
`scripts/fault/s5-06-netem.sh` + `scripts/fault/s5-06-arm.sh`，每臂一个全新的非特权 user+netns
（可用性判据与四条路在 ENVIRONMENT **5.4**），配置按 §12 的统一形状：3 成员 `round_robin` + catch-all、
`max_fails=2`、`fail_timeout=60s`（恢复只走探活）、`probe_interval=100ms`、`probe_timeout=300ms`、阀 50%。
**两臂的读数不要求与 §12 相等**，也不该相等——见下面那两条定义差异。

| 臂 | 注入 | 逐目标 ejections 之和 | 阀挡 | 客户端单条 p50 | 状态码 |
|---|---|---|---|---|---|
| clean | 不加（`qdisc noqueue`） | **0** | 0 | **12 ms** | 全 200，序列 `231231231231`（三台各 4 条） |
| d50 | `delay 50ms` | **0** | 0 | **312 ms** | 全 200 |
| d100 | `delay 100ms` | **1** | **27** | **612 ms** | 全 200；`qdisc` 自数 `Sent 2304 pkt` |
| d400 | `delay 400ms` | **2** | **155 →（退出账）410** | **2413 ms** | 全 200，`all_denied_fallback=0`、`ejected_now` 峰值 1 |
| l20 | `loss 20% limit 1000` | **18** | — | — | 全 200；`qdisc` 自数 `Sent 2237 pkt (dropped 555)` |
| d1500 | `delay 1500ms` | **3** | 452 | **9013 ms**（max 12013） | 全 200（默认 10s 响应头限期在这一档仍站得住） |

**先记那条被实测改写的机制句（本轮最该读到的一格）**：认领那一轮我写下的预期是
"100ms < `probe_timeout` 300ms → 与 §12 类 4b 同形，**不摘**"。实测 d100 摘了 1 次、阀挡了 27 次。
归因不是判据坏了，是**横轴换了单位**：应用层 `--delay-ms` 只加在"上游回话"那一段，
一次探活的等效耗时 ≈ 2×delay；网络层 delay 加在**每一次单向穿越**上——建连的 SYN/SYN-ACK 也算——
一次探活 ≈ **4×delay**，所以 4×100=400 已越过 300。**"慢与死的分界线就是 `probe_timeout`"这条判据仍然成立，
但网络层档的分界线在 `delay ≈ probe_timeout/4`（≈75ms），不是 300ms。**
这句话不是嘴上改一改：我为它加了一臂 **d50** 作证伪试验（4×50=200 < 300 → 该不摘），
实测 ejections=0、valve=0、p50=312 ms ≥ 4×50 —— **模型过了这一格才算立住**。

**两条定义差异（不许把对照读成等价）**：
1. **网络层注入不区分成员**。§12 只让成员 2 出问题，健康的两台照常；这一栈整条 ns 回环一起慢，
   三台**一起**超 `probe_timeout` → 于是造出了应用层矩阵从来没有的形状：**ADR-12 的安全阀在挡摘除**
   （d400 臂 `blocked_by_valve` 从 155 涨到退出账的 410，而 `ejected_now` 峰值只有 1、
   `all_denied_fallback=0`——**摘到上限为止，不是全都摘干净**）。这一格是 §12/§18 拿应用层注入做不到的。
2. **§12 那条"连续 20 条请求不见 128"的判据在这里没有定义**。它靠"只有被注入那台回 128 B"来识别成员；
   网络层注入下三台同样慢、响应体被我刻意钉成同长（`M1…/M2…/M3…` 只换前缀不换长度，
   免得响应大小成为第二个变量），所以识别不出"哪台坏了"。摘除这一侧改用网关自报的
   `health_ejected_now` / 逐目标 `health_t<i>_…_ejections` / `blocked_by_valve`，客户端那一侧只报时延分布。

**loss 那一臂也说反过一次**：我原先批注"重传后响应仍在 `probe_timeout` 内，所以不摘"，
而实测 ejections=**18**。机制是 20% 丢包撞上 TCP 重传（Linux RTO 起算 ~200 ms 且翻倍），
把探活的等效耗时推过 300 ms；同一臂业务侧却几乎全 200（重传对应用透明）。
所以这一格的读法是：**探活这条路比业务更怕丢包**——它是个短小、周期性、单连接、
超时钉在 300 ms 的请求，而业务连接上有 keep-alive 与 10 s 的限期兜底。

**顺带一条对 S5-07 有用的读数**：经网关的一条请求在回环上要走**至少 4 次单向穿越**
（客户端→网关、网关→上游、上游→网关、网关→客户端），直连只 2 次。所以 d100 臂 p50=612 ms
≈ 4×100 + 处理，而 d50 臂 312 ms ≈ 4×50 + 处理——**网络层注入对"经网关"的放大至少是 2 倍**。
S5-07 的容器对照臂（已裁⑨④）与 nginx 比较时，这个"逐跳加"的性质必须一起报，
否则同一个 netem 参数在两条拓扑下测的不是同一个东西。

### 25.2 仪器侧与判定侧：本轮我自己错了 **7** 处（其中一处让我把红读成了事实）

| # | 错在哪 | 症状 / 如果不抓会读成什么 | 处置 |
|---|---|---|---|
| 1 | 臂的 `summary.txt` 把好几个键写在**同一行**，而取数器是 `sed s/^键=//p` | 第一跑 42 项里 **28 条红**，看起来像"netem 把网关搞坏了"；实际每个键读到的都是"我之后那一整行" | 改成**一行一键**。这一族在 §21.2 第 4 条登记过，本轮我撞在**写产出**这一侧，不只是读别人 |
| 2 | 读了一个**不存在的键** `health_ejections` | 空串 → `ej=${ej:-0}` 变 0 → "对照臂没摘除"这条**假绿**，而注入臂的红也全错位 | 运行期面上能读的只有 `health_ejected_now` / `health_ejections_blocked_by_valve` / `health_all_denied_fallback` + 逐目标族；累计次数 = 逐目标求和。**写判据之前先核键名存在**，这句进脚本注释 |
| 3 | `tc -s qdisc` 的统计在**第二行**，我 `head -1` 抓第一行 | `Sent`/`dropped` 永远空 → "注入发生过"那条下界全红，被读成"netem 没生效" | `grep -o 'Sent.*' \| head -1`（先找再截） |
| 4 | 观察者超时比注入的往返短（拿 `--max-time 3` 读 `delay 1500ms` 的观测面） | 采样 6/6 全空、`p50` 读成 **1013 ms**（那是被自己的超时掐断的位置，不是延迟）——**"看着像有延迟"的读数其实是失灵的形状**；整臂跑了 5 分钟被 `timeout 400` 砍掉 | `SAMPLES`/`MTIME` 变成每臂参数；失灵分两类判：①系统性读空 → 作废判死，②丢包打到观察者自己 → 界内且收尾读成功则按预期处理（这条本身是产出：**观测面不在注入作用域之外**） |
| 5 | `unshare -Un` 漏了 `--map-root-user` | ns 内 `uid=65534`、`CapEff=0`，加 qdisc 与建 veth 全 EPERM → 会读成"userns 这条路不通"，真相是我参数用错 | 补测带映射那一发才成立（ENVIRONMENT 5.4 的 B/C 两行就是这么留下的） |
| 6 | 以为新 netns 的 `lo` 需要自己 `ip addr add 127.0.0.1/8` | 报 `Error: ipv4: Address already assigned`（不致命，但那是我猜的机制） | 去掉；`ip link set lo up` 之后地址已自带。实测优先于直觉 |
| 7 | bash 字符串里嵌了 ASCII 双引号（`"不相等那一档"`、`"经网关"` 两处） | `bash -n` 当场语法错 | 改用「」。**写中文注释时的引号选择会影响可执行性**，这条不体面但要记 |

### 25.3 阳性验证：两条真打红，其中一条**补了断言才红得全**

| # | 退化 | 打在哪 | 红数 |
|---|---|---|---|
| D2 | 把 d1500 臂的观察者 `--max-time` 从 20 s 压到 1 s | `NMTIME` 那一格（该臂所有读数的必经之处） | **2**：①类失灵红（6/6 空读 + 收尾累计读为空）；同时 `p50=1013 ms 连 4×1500 都不到` 也红——**下界按机制给才有牙**，只看"有没有延迟"会把超时截断读成延迟 |
| D3 | 把逐目标键名写坏（`health_t${i}X_…`） | `t_ej_sum()` 里那一串键名 | 第一版只红 **2**（d100、d400"该摘却没摘"），而 **clean 与 d50 的"不该摘"两臂空判绿**——键名坏 = 读回空 = 求和 0，与"确实没发生"在数上不可区分。补一条 `metric_ejection_key_reads == 3` 的非空守卫再打 → **6 条红**（四臂全红） |

D3 按 §22.1 的立场写清楚：**不是先把退化打红再补断言，是发现退化能造出假绿之后先补断言、再重打**。
第 5 条与第 4 条同属"**挂死的作业不会自己报错**"那一族，本轮靠"下一个动作先查一次进度"才没白等。

**没打的两条，如实登记**（不算已完成）：**谎报注入**（某臂不加 qdisc 却留着"注入发生过"的判定路径）
与**清场失灵**（收尾不 `down` / 动物主 qdisc，让宿主核对那两条红）——本轮**没有真打**。
前者的判据（`Sent`/`dropped` 下界）被 D2/D3 的失灵形状间接触过，后者只是"判据存在"，
而**"判据存在"不等于"判据被打红过"**。这两格留给 S5-08 门禁或单独立项时补，不写成已完成。

### 25.4 两轮同机复跑与门禁面

**同一套代码连跑两轮**（取样规程"每改动 ≥2 轮、报极差"在这一栈的形状）：
run4 = `PASS checks=50 fails=0`（加 `key_reads` 守卫之前），run5 = `PASS checks=56 fails=0`（之后，
六臂各多一条守卫判定 = +6 条）。两轮的机制级读数对得上，极差都在"每次请求都要重新握手"的量级内：

| 格 | run4 | run5 | 怎么读 |
|---|---|---|---|
| clean p50 | 12 ms | 12 ms | 分母，两轮逐字相同 |
| d50 p50 | 312 ms | 314 ms | 极差 2 ms |
| d100 p50 | 612 ms | 613 ms | 极差 1 ms |
| d400 p50 | 2413 ms | 2413 ms | 极差 0 |
| d1500 p50 / max | 9013 / 12013 ms | 9013 / 12014 ms | 极差 1 ms |
| d400 `blocked_by_valve`（运行中 → 退出账） | 155 → 410 | 155 → 412 | 阀挡的次数跟着采样节拍走，**这一格不该要求逐字相同**，读的是"量级 + 一直在挡" |
| l20 `dropped` | 555 | 728 | 同上：丢包计数随两轮发包总数变（`Sent` 2237 → 3015 pkt），**读比值不读绝对值**（555/2237≈24.8%、728/3015≈24.1%，都围着配置的 20% 偏高——与 25.1 那句伯努利方差同源） |

**门禁面**（本任务 `src/` 与 `tests/` 一字未动，所以这串是"没碰代码"的证据，不是抬升）：见下一条提交信息，
三 preset 各 376/376、非 gtest 警告各 0 条、tsan 全量 `ThreadSanitizer` 0 次、十四条冒烟逐条 ✗ 计数 0，
落盘 `/home/archer/s5-06-gate*.log`。**注入结束后宿主回环仍是 `qdisc noqueue 0: root refcnt 2`、
`l7gate-` 前缀容器 0 条残留**——这一栈最该守住的就是这句。


### 25.5 本轮没做与没量的（写在纸上）

- **没测 netem 在容器网络（veth/bridge）上的形状**：本轮的注入都在非特权 netns 的 `lo` 上。
  S5-05 那套 compose 的容器之间走 veth + bridge，`--cap-add=NET_ADMIN` 的容器里 qdisc 加得上
  （ENVIRONMENT 5.4 的 D 行），但**没有拿它跑过一臂矩阵**——那是"网络层注入 × 容器拓扑"两件事叠一起，
  属 S5-07 的对照臂，不在本任务顺手做。
- **没做按成员区分的网络层注入**。§12 的类 4a/4b 是"只有成员 2 慢"，本轮是"整条回环一起慢"。
  要按成员注入得给每台单独一条 veth 并分别挂 qdisc（跨 ns 协调），那是另一套夹具，
  测的也不再是"同一批判据换个延迟来源"。**明写没做，不是遗漏**。
- **没量吞吐/QPS**：每臂个位数到十几条请求，只量时延分布与计数，不产任何 SLO 数字。
- **loss 与摘除之间没做阈值扫描**：l20 臂 ej=18 是单点读数（含 eject→recover 反复），
  没量"多大 loss 开始让探活判死"，也没取 `recoveries` 那一族来拆那 18 次里有几次是恢复后再摘的。
- **d50 是模型的单点证伪**：`probe_timeout/4` 这条界只用 50 与 100 两点夹住，没扫到 75 ms 附近；
  "≈/4"来自 4×delay 这个机制量纲，不是拟合出来的常数。

---

## 26. S5-08 把 §25.3 欠的两条退化补打了（2026-11-04，门禁收尾）

**这一节在问什么**：§25.3 末原样登记着两条**没打**的退化——**谎报注入**（某臂不加 qdisc 却留着"注入发生过"
那条判定路径）与**清场失灵**（收尾不 down / 动物主 qdisc，让宿主核对那两条红），立场是
「**判据存在 ≠ 判据被打红过**」。这一节就是那两格的兑现，顺带把 S5-07 欠的那笔 socket timeout 的
**取数手段**（不是那笔归因）补齐。手段：`/home/archer/s5-08-fire-degradations.sh`，改的只有
`scripts/fault/` 那两个脚本，`src/`、`tests/`、`demo/`、`deploy/` 一字未动。

### 26.1 D4 谎报注入（把 `if [[ -n "$SPEC" ]]` 改成 `if [[ -n "" ]]`，d100 一臂）

这一臂**根本没挂 qdisc**，而"注入发生过"的下界判据原样留着。跑 `ARMS=d100` → **✗ 3 条**
（`verdict.txt` = `FAIL checks=13 fails=3`）：

| ✗ | 红在哪一格 | 这条红的含义 |
|---|---|---|
| 1 | `qdisc 没有发包计数：'Sent 0 bytes 0 pkt (dropped 0, overlimits 0 requeues 0)'` | **这条就是 §25.3 欠的那一格**：下界读的是 qdisc 自己的发包数，空挂一个 qdisc 与"注入真的跑过这条路"在数上分得开 |
| 2 | `这一臂没摘（ej=0）——与本轮第一次实测（ej=1、valve=27）不同` | 摘除臂的"该摘却没摘"红——它红的是**预期落空**，不是机制失灵 |
| 3 | `p50=12 ms 连 4×delay 都不到：注入没落到这条路径上，或计时取错了` | 延迟下界红：12 ms 正是 clean 臂的量级，说明这一臂其实什么都没注入 |

**为什么三条一起红是好事而不是坏事**：那一臂的三条判据（发包计数、摘除、时延）指向同一件事，
任何一条单独存在都不够——**上一轮 D3 学的正是这个**（键名坏会让"不该摘"那两臂空判绿）。

### 26.2 D5 清场失灵（①开工前的宿主基线读成假串 ②收尾之前故意在宿主留一个 18950 监听）

跑 `ARMS=clean` → **✗ 3 条**（`verdict.txt` = `FAIL checks=13 fails=3`）：

| ✗ | 红在哪一格 | 说明 |
|---|---|---|
| 1 | 阶段 0：`宿主回环不是 noqueue（qdisc netem 9999: root refcnt 2 delay 9999ms）` | 我改的是**基线那一行的赋值**，所以这条红在开工前的自检上——**这暴露了我第一版的取巧**：我以为只会红收尾那一条，其实脚本在阶段 0 也拿同一个变量判过一次。**判据被打了两次红，比一次更说明它有牙**，但这不是原计划的那一条，照记 |
| 2 | 收尾：`宿主回环从 qdisc netem 9999: … 变成 qdisc noqueue 0: root refcnt 2 —— 本脚本绝不该动它` | 这是原计划的那一条：收尾的"与开工前逐字相同"确实会红 |
| 3 | 收尾：`1895x 还有 1 个监听没退` | 残留监听那条也红：我先起 `python3 -m http.server 18950`，**判据跑完之后**才 kill 它 |

**§25.3 那句"动物主 qdisc"这一半仍然没打，而且这台机器上打不出来**：宿主回环挂 qdisc 需要
`CAP_NET_ADMIN`，uid=1000 加不上（ENVIRONMENT 5.4 的 A 行实测 `Operation not permitted`）——
**我连"把宿主回环弄脏"这个状态都造不出来**。所以这一半的处置不是"补打了"，而是改判成
"**基线比对那条判据被打红过**（上面第 1、2 条），而它要防的那个动作本身在这台机器上不可实现"。
这句不写成已闭合。

### 26.3 还原与复跑

`grep -rn 'TEMP-D' scripts/ src/ tests/ demo/ deploy/` → **0 处**；两个脚本与 `/home/archer/s5-08-backup/`
里的备份 `cmp` **逐字节相同**（还原走 `cp -p`，不是手改回去）。还原后复跑 `ARMS="clean d100"`：

- `build/fault/s5-06/verdict.txt` = **`PASS checks=22 fails=0`**（`clean` 与 `d100` 两臂同轮跑完的判定面：
  阶段 0 两条自检 + 每臂各自的守卫与机制项 + 收尾两条宿主核对）。
- d100 那臂的机制级读数与 §25.4 的 run4/run5 同带：`Sent=2304 pkt`、`ejections 之和=1`、`valve=27`、
  单条 **p50=612 ms**（run4 612 / run5 613）。**这一格是本轮的第三个同机读数**，三次数值只差 1 ms。
- clean 臂 p50=12 ms、`constructed==destructed`（d100 是 15）、三台各 4 条的 round_robin 序列原样。
- 收尾宿主回环 `qdisc noqueue 0: root refcnt 2`、1895x 无残留监听、`l7gate-` 容器 0 条。

### 26.4 本轮仪器侧我自己错的地方（与 §20.1/§21.2/§22.2/§23.2/§24.2/§25.2 同一族）

| # | 错在哪 | 如果不抓会读成什么 | 处置 |
|---|---|---|---|
| 1 | **报数前没核落盘**（第三次）：交接段与「已知待决事项」把两发不三发的 timeout 条数合并成了一句"16/48" | 一笔未归因的账连**条数与出现侧别**都是错的，后面任何归因都建立在错的数上 | 认领那一轮先 `grep -H 'Socket errors\|Non-2xx' build/bench/s5-07-sym*/*.wrk.txt` 扫全部落盘，逐发列出（B-022 追记第一张表）；此后每次引用都指到"哪一发哪个文件" |
| 2 | **`measure()` 一失败就 `return`**，把最需要条数的那一发的条数丢了 | 这笔账永远归不了因，而且看起来像是"证据取不到"的环境问题 | 改成"先记条数再判作废"，并落 `<档>.errors.txt` + `gateway.exit.keys.txt` |
| 3 | 用 `--port` 调 `l7gate_bench_upstream`（它的旋钮是 `--listen`），对端**根本没起**，脚本还往下跑 | 会读成"容器里的网关不起退出账" | 探针加一条 `kill -0 $pid` 的存活检查：对端没起就**判这一档作废**，不交一份空数 |
| 4 | 探针第一版把时长写成 `4s/8s` 带单位，`$((dur + 6))` 直接语法级失败 | 屏幕上只看到"没有 Socket errors 行"，会读成"wrk 的 timeout 计数不响应"——**其实是我的对端没起** | 改成纯数字秒 + 存活检查；第二版才是可用的负结果（见 B-022 追记第四节） |
| 5 | 想拿 `sed` 插入含 `|` 的替换串（`ss … | grep …`），分隔符撞车 | 脚本半改半不改，跑出来的红不可信 | 改用 `sed /锚/i\` 插入整行；改完立刻 `grep -c TEMP` 核对命中行数（这条抓到了第 1 行没打上的情况） |
| 6 | D5 的假基线同时让**阶段 0 的自检**也红了，我第一版会把它写成"预期外的一条红" | 少记一条：同一变量在脚本里被用了两次，这件事本身是判据的形状 | 原样记进 §26.2 第 1 行，不删那条红 |
| 7 | 用 `wsl.exe -d Ubuntu -- bash -c "…$f…"` 传带变量的循环，`$f` 被吃成空串（项目记忆里那条坑） | 三条 `cat` 全报"No such file"，看起来像日志没落盘 | 带变量/循环一律先 Write 成脚本再 `bash` 跑（本轮后面全部照这条走） |

### 26.5 本节没做的

- 没有重跑 netem 六臂全矩阵（那是 S5-06 的门禁面，§25.4 的两轮原样有效）；本节只重跑了
  `clean` 与 `d100` 两臂，因为它们才是这两条退化所在的判定面。
- 没有给 netem 加"按成员区分"的网络层注入、也没扫 loss/delay 阈值（§25.5 那两格原样欠着）。
- 那笔 socket timeout 的**归因**没有在这里闭合，只闭合了它的**取数手段**；状态见 BENCHMARK B-022 追记。

---

## 27. S5-09 把那条"能整车失效"的输入挡在装配期，以及我自己在这条线上错的十处（2026-11-04，ADR-28）

**这一节在问什么**：S5-05 的退化 C1 抓到一条被测方事实——配置里的 `address` 只挡空串、不挡字符集，
而 `health_t<i>_<address>_<port>_<metric>` 那一族把值**原样拼进指标名**，带连字符的地址于是能造出
Prometheus 名字法里非法的键名，后果不是那一格坏，是**整份 `/metrics` 抓失败**（实测
`lastError = invalid metric type "a_18081_ejections counter"`，那台网关 101 个键一起丢）。
S5-09 按项目所有人 2026-11-04 那句"按你的建议来"走**修法①**：`address` 只收 IPv4 字面量，
拒载 + 点名（语义与代价在 **ADR-28**，任务行在 TASKS 的 S5-09）。

### 27.1 三处阳性验证：红集合是分得开的，而且有两处与我的预期不一样

点火手段：`/home/archer/s5-09-fire-v.sh`（改的是 `src/` 里的实现，用完 `cp` 还原 + `cmp` 逐字节核对 +
`grep -rn 'TEMP-V' src/ tests/ scripts/` 为空）。用例集 `S5_09_*:S5_02_Reloader.*` = 18 条。

| # | 退化打在哪 | 红掉的用例 | 与 ADR 第七节预期的差异 |
|---|---|---|---|
| 干净门 | 什么都不改 | **0 条**（18 条全绿） | — |
| V1 | `gateway_config.cc` 那条 `if` 改成永假（校验摘掉） | **4 条**：`RejectsAnythingTheSocketLayerCannotParse`、`RejectsWholeDocumentWhenAnyTargetIsBad`、`WhateverPassesTheValidatorYieldsLegalPrometheusNames`、`S5_09_Reloader.HostnameAddressIsRefusedByNameAndKeepsTheOldGeneration` | 符合预期，而且**节点表那一臂与谓词对照臂没红**——这正是它们各自独立存在的理由：V1 只证明"文件那一支的检查有牙" |
| V2 | 检查改成只查 `targets[0]` | **1 条**：`RejectsWholeDocumentWhenAnyTargetIsBad` | 比我写的"④ 红而 ① 红在另一格"**更干净**：① 的样本每个配置只有一个 target（下标 0），所以 V2 下它仍然绿——① 与 ④ 本来就是两格不同的东西，这一对红集合把"只查第一个"这种漏法单独照出来了 |
| V3 | `is_ipv4_literal` 退化成"非空即合法" | **6 条**：V1 那四条 + `PredicateAgreesWithTheSocketLayerItGuards` + `S5_09_NodesRow.AddressMustBeAnIpv4Literal` | **与预期不符的一条**：我在 ADR 里写"V3 → ①②③ 全红"，实际 **② `AcceptsEveryLiteralShape` 不红**。这不是用例坏了，是它断的本来就不是"谓词严格"——② 断的是"合法那一支的行为没变"，宽松谓词不伤它。按纪律照记并就地更正 ADR 第七节那句预期（**不改用例凑红，也不改我的预期让它显得对**） |
| 还原门 | 还原之后 | **0 条**（18 条全绿），`TEMP-V` 残留 0 处，两文件与备份 `cmp` 逐字节相同 | — |

### 27.2 仪器侧本轮我自己错了 **10** 处（其中两处会把"没跑"读成"没红"）

| # | 错在哪 | 症状 / 不抓会读成什么 | 处置 |
|---|---|---|---|
| 1 | gtest 的 `--gtest_filter` 正向模式之间是**冒号**，我写成逗号（`S5_09_*,S5_02_Reloader.*`） | 屏幕上是 `Running 0 tests from 0 test suites` + 我的脚本报"红 0 条"——**三次降级全被读成"退化打不红"** | `fire()` 里加一条 `ran>0` 守卫：一条用例都没跑到就判这一跑**作废**，不交一份空数（D3 那一族的第二次，这次撞在我自己的点火脚本上） |
| 2 | 还原用 `cp -p`：备份的旧 mtime 比降级时编出的 `.o` **更旧**，make 判定"不用重编" | 三次降级的红集合**一模一样**（都是上一轮残留的那颗退化谓词），看起来像"三条退化等价" | 还原改不带 `-p` 的 `cp` + 每次点火前先跑一次**干净门**（还原态不绿就没资格点火）+ `rebuild()` 里把"这一跑真重编了哪个 .o"打出来 |
| 3 | 我自己脚本里"红 N 条"那格读 `grep '[  FAILED  ] N tests'`，而 gtest **单数时打 `1 test`** | V2 明明红 1 条，汇总那格印成"红 0 条"——与 §24.2 第 6 条（把清单行与汇总行一起数）同源：**读汇总行要先看单复数** | 结论一律以 FAILED **清单行**为准，汇总行只当旁证；这一格原样记在表里 |
| 4 | 用例夹具 `one_target` 的模板里多带了一个引号（`{"address":"` + 已带引号的值） | 三条新用例红在"这条该被接受，却被拒了：偏移 76 对象里期望逗号或右花括号"——红的是我的字符串拼接，不是被测方 | 模板改成 `{"address":`（值令牌由调用方带引号），并加一个 `quote()`；失败信息里的**偏移**是把我说清楚的第一手证据 |
| 5 | `for (const std::string& address : {"127.0.0.1", …})` 触发 `-Wrange-loop-construct` **2 条警告** | 门禁第 2 条"非 gtest 警告各 0 条"当场被弄脏（我自己的改动弄脏的） | 循环变量改成按值；还原后的构建日志 `warning:` 计数 **0** |
| 6 | 第一版把 `localhost` 放进"键名应当非法"那组样本 | 那条断言会**假绿反了方向**：`health_t0_localhost_18081_ejections` 本来就合法（纯字母），红了的是我的判据 | 组 B 只收**机械变换管不到**的字符（连字符 / 空格 / `%`），`.` 与 `:` 明确写出"不在这一组，因为 ADR-24 第五节已经换掉了"；这一处错就地写进用例注释 |
| 7 | bind 对照臂里放了 `172.31.250.21`（编排子网里的地址，本机没有） | 红在 `make_listen_socket(...).ok()`，看起来像"谓词在说谎" | 这一臂只留本机真有的 `127.0.0.1` / `0.0.0.0`，`172.31.250.21` 改判"谓词说合法但不等于本机有它"——**两件事分开断** |
| 8 | 探针（nginx 加 perf 层那一发）第一版 `docker commit -c "--entrypoint …"` | `-c` 要的是 **Dockerfile 指令**，写成命令行开关就静默不产出镜像；后面每一步都报 `unknown: failed to resolve reference` 去远端拉那个不存在的 tag | 改成不带 `-c` 的 commit + 运行期 `--entrypoint`，并加一条 `docker images -q` 的存在性断言：commit 仍失败就**当场收手**，不交一份空探针 |
| 9 | 同一发探针里 `l7gate_bench_upstream --port`（真旋钮是 `--listen`）且没做存活检查 | 对端根本没起，后面全空——与 §26.4 第 3/4 条**同一族的第三次**（旋钮名猜的 + 对端没起不报错） | 参数按 `--help` 原文写；起完立刻 `kill -0 $pid`，没起就判这一档作废 |
| 10 | **我新增的那条下界臂把另一条等式顶偏了**：`smoke-s5-05` 阶段 4 原来的那一臂是 502、一个字节都不到上游，我把它换成"合法 IPv4 真服务一条"之后，阶段 8 那条"上游自报之和 == 我发的业务条数"当场少 1 条 | 门禁首跑第 14 条 `rc=1`、`✗ 1`：`上游自报之和 13 != 我发的 12`。红的是**我的改动没结算自己的账**，不是被测方 | 等式右边改成 `主路径 12 + 对照臂自报 responses_completed`，而那个数**从对照那台网关自己的观测面取**（取不到就判红，**不许拿空串冒充 0**）。修完复跑 `PASS checks=69 fails=0` |

### 27.3 门禁面（2026-11-04 本会话实测，判定全部读落盘）

- `cmake --build build/debug -j 16` 全目标 **rc=0**、`warning:` 计数 **0**（`s5-09-buildall.log`）；
  `ctest --test-dir build/debug` **383/383**（376 → 383，+7 条全在 S5-09 那三组里；`s5-09-ctest3.log`）。
- `bash scripts/verify-all-presets.sh` **rc=0**：三 preset 各 **383/383**、非 gtest 警告各 **0** 条
  （每行都是 `build exit=0 warning_total=0 非gtest警告=0`；`s5-09-presets.log`）。
- **tsan 直跑** `build/tsan/tests/l7gate_tests` rc=0：**381 PASSED + 2 SKIPPED**、日志里
  `ThreadSanitizer` 出现 **0 次**（`s5-09-tsan.log`；跳过的两条仍是 `S2_07_MemoryComposition` 那两格）。
- **十四条冒烟**：首跑十三条 `rc=0` 且 ✗ 行 0；**第 14 条 `smoke-s5-05` 首跑 `rc=1`、✗ 1 条**——
  就是我上面第 10 处自错。修正后复跑：`build/smoke-s5-05/verdict.txt` = **`PASS checks=69 fails=0`**
  （66 → 69 是这条臂多出来的三条判定：装配期被拒、点名到实际读到的值、端口根本没开），
  阶段 4 那四条与阶段 8 的等式一起绿。`s5-03` = `PASS checks=40 fails=0`、`s5-04` = `PASS checks=29 fails=0`。
- 清场：`l7gate-` 前缀容器残留 **0** 条、同机 `rgbt-*`/`knowstack-*`/`tinrpc-*` 与开工前同为 **6** 条、
  宿主回环 `qdisc noqueue 0: root refcnt 2`。
- 阳性验证那一轮的还原门：还原后 18 条用例 **0 红**、`TEMP-V` 残留 0 处、两文件与备份 `cmp` 逐字节相同。

### 27.4 本轮没做与没量的（写在纸上）

- **真 MySQL 那一臂没验**：节点表的降级只落在 ctest 的纯函数臂（`S5_09_NodesRow`）上；
  `scripts/smoke-s3-04.sh` 没有加"往节点表插一行主机名"这一格。要加就得动那条冒烟的夹具，
  代价与形状记在这儿，不当成已完成。
- **`--upstream` 命令行那一支的行为没改、也没重验**：ADR-28 第四节给了它结构上产不出那些键的理由
  （`--config` 与 `--upstream` 互斥、逐目标族只在装了 `health` 段时才发）。它今天给主机名仍是 502 +
  `upstream_connect_failed`，那条账归 S5-05（ENVIRONMENT 5.3），本任务没碰。
- **旧那条 502 判据的形状被这条任务改了**：`scripts/smoke-s5-05.sh` 阶段 4 原来断"起得来、两条 502、
  自报 `upstream_connect_failed≥1`"，现在断"装配期就被拒（`rc=2` + 话里点到实际读到的值 + 观测端口根本没开）+
  下界那一台合法 IPv4 照常 200"。**S5-05 当时那个实测本身没被推翻**（那是修前的二进制），被改的是
  今天再跑一次会看到什么——所以这条臂同时也是 S5-09 的一条外部证据：拿修前的实现跑它，红的是
  "对照容器居然起来了"那一格。
- 没有跑任何性能读数：这条改动只在装配/重载那一拍加一次 `inet_pton`，每请求路径一个字节都没动，
  所以 B-019/B-020/B-021 那几格既不需要重跑、也不许被我这句话升格成"没成本"——它的成本本来就不在那条线上。

---

## 28. S5-10 结案两笔待裁：一支"引用面断言"先被打红过（2026-11-05，纯文档条）

**这一节在问什么**：这条任务一行代码都不改，交的是"两笔裁定写回账本"。它唯一的失效形态是
**我以为我改干净了**——所以判据不能是我的说法，得是一支同时能判"改前有那句旧说法"与"改后有那句新说法"的脚本。

### 28.1 手段与红集合

`/home/archer/s5-10-refs.sh`（不进仓库，跟 §25/§26/§27 那几发点火脚本同一落点），21 个落点、两个模式：

| 模式 | 目标树 | 读数 | 这一格在防什么 |
|---|---|---|---|
| `selftest`（**阳性验证**） | `git archive HEAD \| tar -x` 到 `/home/archer/s5-10-headtree/`（只读导出副本，不碰工作树、不动 stash） | **✗ 37 条、`VERDICT FAIL`** | 防"这支脚本根本不会红"。37 的拆法：16 个替换型各红 2 条（"目标树仍含旧说法" + "目标树缺新说法"）、4 个追加型各红 1 条（"改前就含新说法——我没改这一处"）、S5-10 那一行的存在性红 1 条 |
| `verify` | 工作树 | **`checks=21 fails=0 VERDICT PASS`**（`/home/archer/s5-10-refs.verify.txt`） | — |

**为什么这不走过场**：替换型每条的第三个条件要求"HEAD 里必须有那句旧说法"。锚错一个根本不存在的旧句子，
脚本会打「这条断言扫的是空」——那一格在这次跑里 **0 次命中**，所以那 21 个落点全都对上了真实存在的旧文本。

### 28.2 门禁面（判定读 `/home/archer/s5-10-gate.verdict.txt`，10:17:49 → 10:26:20）

全目标构建 rc=0 且 `warning:` 0 条；`ctest` **383/383**；`verify-all-presets.sh` rc=0——debug / asan-ubsan / tsan
三行都是 `build exit=0 warning_total=0 非gtest警告=0` 且各 **383/383**；tsan 直跑 **383 ran = 381 PASSED + 2 SKIPPED**
（32,973 ms）、`ThreadSanitizer` 出现 **0 次**；**十四条冒烟逐条 rc=0 且 ✗ 行 0**，第 12/13/14 条读
`verdict.txt`＝`PASS checks=40 fails=0` / `29` / `69`，真容器三条在批次里一次跑过。
本会话开头 Docker 引擎不在（`wsl -l -v` 报 `docker-desktop Stopped`、`npipe … dockerDesktopLinuxEngine` 找不到），
已上报并由项目所有人启动解决；恢复后 `NCPU=16`、契约锁的五个镜像 id 与账本逐字相同、`l7gate-` 残留 0 条、别人 6 条不变。

### 28.3 本轮仪器侧我自己错的 **5** 处

| # | 错在哪 | 不抓会读成什么 | 处置 |
|---|---|---|---|
| 1 | 门禁脚本用 `grep -c '^\[  PASSED  \]'` 数 **行数**当用例数，tsan 那一格读到 `passed=1 skipped=5` | 盘上的字面是 `[  PASSED  ] 381 tests.` 与 `[  SKIPPED ] 2 tests`，我报的 1/5 两者都不是——**"跳过 5 条"这种数一旦进文档就没人再查** | 判据改读汇总行的字面数（381 + 2）。与 §27.2 第 3 条"数用例以清单行为准"同一族的第四次踩 |
| 2 | 冒烟 ✗ 行的判据第一版写成 `grep -cE '^ *✗'` | 会把脚本自己那句「✗ 计数 0」的汇总行算进去（§24.2 第 6 条那一族），十四条全被误判成有红 | 改成"含 ✗ 且不含 计数"两道过滤，正式跑全部 `cross_lines=0` |
| 3 | S5-10 那一行的**插入位置**：我拿「候补三条」那行标题当锚去替换 | 标题被我顶掉，而且新行与上面的表之间留了一个空行 → markdown 把 S5-10 读成一张**没有表头的孤表** | `grep -n '^| S5-'` 逐行看结构，两处各自改回（删空行 + 把标题插回） |
| 4 | 红数先写成 **21**（那是落点数），实际条数是 **37**（落点 × 每个落点的条件数） | 报进 TASKS 与 ROADMAP 两处，"判据红过几次"这件事本身就是这条任务的交付物之一 | 三处就地改回 37 并给出拆法。**这是"报数前核落盘"那一族的第四次**，而这次核的就是我自己刚生成的那份 verdict——`tail -2` 花两秒，我省了 |
| 5 | 探针脚本第一版把 `command -v docker` 的通路当成"集成还在"的证据（`bash -l` 下 PATH 带 `/mnt/c/…/bin`，命中的是 Windows CLI） | 会把"引擎没起"误报成"集成掉线"，两种处置归不同的人 | 改看三件：`/usr/local/bin/docker` 与 `/var/run/docker.sock` 与 `/mnt/wsl/docker-desktop` 在不在，再加 `wsl -l -v` 的 `docker-desktop` 状态 |

### 28.4 本节没做的

- 没删本地那条 `l7gate/runtime:latest`（`3b2744438d79`）——删镜像不可逆，等项目所有人一句话；compose 文件里 `latest` 仍是 0 次，判据没被污染。
- 没重跑任何性能读数，也没动 `tools/`、镜像、`src/`：B-021/B-022 的读数原样有效。
- 没把 §26 那种"补打被测方退化"搬进来——这条没有可打红的被测方判据，能被打红的是我自己的引用面脚本，它已经先红过一次。


