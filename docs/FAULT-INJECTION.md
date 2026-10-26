# FAULT-INJECTION —— 预期 vs 实测

这份文件只记一件事：**当对端 behaving 坏掉时，网关做了什么，和我说的是不是一回事**。格式固定成三列——预期、实测、结论；未达标照记（契约第 7 节对这份文件的完成判据原文就是这五个字）。

两条纪律先写在这里：

一，**每条故障都要有"直连对照"**。上游断掉时客户端拿到半条响应，这件事本身不证明网关正确——必须先证明直连同一个上游也是那个形状，网关才可能被归因。B-001 的直连组、下面每个阶段的"直连确实一个字节都没拿到"都是这个用途。

二，**sanitizer 的"干净"必须带反证**。一个从不报警的检查器和一个干净的代码库，从输出上看完全一样。所以每次记"ASan/LSan 零报告"时，同一段要给出它**会**报警的证据。

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

## 3. Sanitizer 面（`bash scripts/verify-all-presets.sh`，本轮 2026-10-26）

| 检查 | 预期 | 实测 | 结论 |
|---|---|---|---|
| ASan + UBSan（`-O0 -g` 强制） | 100 条全过、零报告 | `asan-ubsan` **100/100**，build exit=0，非 gtest 警告 **0 条** | ✅ |
| LSan（随 ASan） | 零泄漏 | 100/100 内无 leak 报告 | ✅，反证见下 |
| TSan | 零 data race | `tsan` **100/100**，含 7 条 `S1_06_Reactor` 闭环与 5 条握手闭环用例 | ✅ |
| debug（gcc） | 全过 | `debug` **100/100**，0 警告 | ✅ |

**反证（没有这一段，上面的"干净"就只是我的一句话）**：

- 工具会报警这件事在 `ENVIRONMENT.md` 2.2 做过阳性验证：故意写坏的样本下 ASan 报 UAF、LSan 报 5 处泄漏、UBSan 报 signed overflow、TSan 报 data race。
- LSan 的**端到端**反证在 S1-04 做过，本任务沿用同一条路径：故意留一条不释放的连接 → `detected memory leaks` + 退出码 1；删掉 → 退出码 0。
- 本轮新加的一条同类反证：asan/tsan 首跑各报 **1 条非 gtest 警告**（`tools/bench_upstream_main.cc` 里一个只用于注释目的的 `constexpr`，clang 报 `-Wunused-const-variable`，gcc 的 debug preset 不报）。它证明"非 gtest 警告 = 0"这条门禁真的会响。删掉那个未使用常量后归零——**一个没人引用的常量是一句关于结构的谎**，删它比给它找个假用途诚实。
- 优雅停止的反证同样本轮现形：第一版 p99 脚本在 1000 并发还在跑的时候直接 `kill -TERM`，报告里是 `fd_end=351`、`live_connections=342`、`epoll_unregistrations` 比 `registrations` 少 344。**这不是泄漏**——循环退出时那 171 对连接确实还没被收尾，网关的停止语义是"不再接受新连接 + 退出循环"，不排水在飞连接。修法是把观测条件摆正：脚本先静置 5 s 再 SIGTERM，然后**断言** `live_connections==0`、`fd_start==fd_end`、上游 `refused_*==0`（`fatal`，不是 `warn`）。静置后仍不为零才是真漏。改完之后标准口径那一轮是 `fd 9→9`、`live_connections=0`、`constructed==destructed==6008`。

## 4. S1-05 的 Upgrade 透传验证结果（门禁四件套第 2 条点名的一项）

| 项 | 预期 | 实测 |
|---|---|---|
| 101 之后不再按 HTTP 读 | 两帧二进制字节逐字节回显比对通过；`requests_parsed`/`parse_calls` 停在 1（隧道里**零解析动作**） | ✅ `UpgradeRequestSwitchesToRawTunnelAfterThe101` |
| 只有 `Upgrade:` 头、没有 `Connection: upgrade` 记号 | 不进隧道，101 当普通响应字节漏下，`upgrades_101=0` | ✅ `UpgradeHeaderWithoutConnectionTokenIsNotATunnel`（测试对端刻意比网关宽松，这个不对称组合证明点燃隧道的按钮在网关手里） |
| 101 头被 TCP 切成三段 | 拼回原样、`parse_calls==1` | ✅ `TunnelSurvivesSplitResponseHead` |
| `release_scratch()` 的调用点 | 全仓**唯一**一处，且在 101 头排进下游出向缓冲之后 | ✅ 结构上不存在"请求结束就释放"的自动路径（接口没有那条路径，不是靠约定） |
| TSan 下的握手闭环 | 零 data race | ✅ tsan preset 内 5 条握手用例全过 |

## 5. 还没注入的故障（记在这里，别当成"已验证"）

| 缺口 | 为什么现在没有 | 归属 |
|---|---|---|
| 上游比下游慢（慢读）撞高水位后的**恢复** | 背压动作（摘 EPOLLIN）本身就是 S1-07 的交付物；现在能证"撞水位不涨"，不能证"水位回落恢复读" | S1-07 |
| 上游 accept 后**半条响应头**就关（头切在中间） | 与 F-1/F-4 同一条 before_head 路径，但**分片到达**的时序只在 ctest 里有（reactor 用例），没有走真 curl | S1-07 冒烟顺带补 |
| `SIGTERM` 时在飞连接的**排水** | 现在的停止语义不排水（见第 3 节反证最后一条）。真正的优雅停机（停止接受 → 等在飞走完 → 超时兜底）不在 S1 范围 | S2 之后，与连接池同期 |
| 上游进程被 `kill -9`（连 backlog 里的半连接一起消失） | 冒烟脚本目前用 `kill -TERM` 收尾上游；SIGKILL 下的 accept 失败路径只有 ctest 覆盖 | S3（健康检查/摘除的注入面） |
| 配置 reload 时上游连接抖动 | 配置热重载在 S5 | S5 |
| 100k 空闲长连接下的 fd/内存 | `ulimit -n` 现在 10240，先决条件没到位 | S2/S3 + 先解决 rlimit |
