# ADR-10 · 配置热重载（进程内双 buffer），不是热重启；命名不许混称

- 状态：已采纳
- 日期：2026-10-26
- 相关：契约 3.1、S5、11.4

## 我选择什么

**配置热重载（hot reload）**：新配置解析进一块全新的双 buffer，构造完成后原子替换指针；旧配置由引用计数持有，**等走完在它上面的存量请求自然结束再释放**。整个过程不换二进制、不 fork 新进程、不转移监听 fd。

## 为什么不叫「热重启」，这个词差在哪

Envoy 的 **hot restart** 是**进程级二进制热升级**：新进程起来，老进程把 listening fd 通过 Unix socket 传过去，老进程进入 drain 逐步退出，统计计数通过共享内存继承。它解决的是「换代码不换流量」，代价是一整套双进程协调机制。

**两者差一整个层级。** 我要解决的是「换配置不换进程」。混称的后果很实际：别人问我「你的热重启怎么传 fd」时，我答不上来，因为我的实现里根本没有 fd 传递——一问就穿。所以 README 和所有文档统一用**热重载 / hot reload**，出现「热重启」必须是特指进程级升级并明确说明本项目不做。

契约 11.4 把这条列为面试自检题，我把它固化在这里。

## 真正的难点：已开始的请求该用哪份配置

这不是配置问题，**它是 C++ 对象生命周期问题**。一个请求处理到一半，配置被替换、它引用的路由表和上游节点对象被析构，就是 use-after-free。我的答案：

- 请求处理入口取一次配置快照（shared_ptr / 引用计数 +1），整个请求生命周期持有一份。
- 替换是指针级原子操作，老配置对象在最后一个持有者释放时才销毁。
- **这条是 ASan 与 TSan 的重点靶区**，S5 门禁要给出干净报告，不接受「逻辑上应该没问题」。

顺带说清：这也和 ADR-03 的取舍连在一起——正因为线程共享地址空间，引用计数才是够用的机制；多进程模型下同一件事得跨进程协调，复杂度完全不同。

## 我放弃什么

- 不做配置版本回滚 UI、不做 diff 展示（契约 1.3 禁管理后台）。回滚就是改回 MySQL 里的值再触发 reload。
- 不做 watch 推送式更新；周期 reload 的收敛延迟代价已在 ADR-07 记账。

## 与 Nginx 的对照

Nginx 的 `reload` 是介于两者之间的形态：master 保留、worker 全部优雅退出并由新 worker 接管，配置在进程间通过重新解析文件达成一致。我会在 README 的对比段说明「我的热重载比 Nginx 的 reload 更轻（不换 worker），但可升级的东西也更少（只换配置不换代码）」——两边都讲清楚，不挑对自己有利的说法。

---

# 追加节 · S5-02 落地的形状（2026-11-03）

上面那几节是 2026-10-26 写的意图，这份追加节把意图钉成实现契约。四条硬前置先摆出来，
后面每一节都在兑现其中的某一条：

- **已裁第 ⑨ 条第 3 项**：触发源 = 循环线程定时 `stat` + 内容哈希。`inotify` 与 `SIGHUP` 已被否，本追加节不重开、实现里也不许顺手改。
- **已裁第 ② 条**：`--config` 只装策略；监听 / `--workers` / redis / nodes 参数只走命令行。重载不得成为这条切分的例外——违反就拒绝并点名。
- **ARCH 1.16 与 [src/net/reactor.h:212-220](file://wsl.localhost/Ubuntu/home/archer/workspace/l7gate/src/net/reactor.h#L212)**：链在循环线程里无锁逐条读，运行期不许改。
- **ADR-20 第四节**三条（发布是原子的不可变快照 / 装配完整才发布 / 目标下标空间 ever-seen 只增不减）在文件重载上原样成立，不另立一套。

## 一、重载的定义域（能换什么，不能换什么）

**能换**（就是 TASKS S5-02 验收句① 括号里那三样）：

1. `routes` 段的规则集（增、删、改规则；组名引用仍在装配校验里判存在）。
2. `upstreams` 段的**内容**：组里的成员集、每组的 `balance` / `hash_key` / `vnodes`（环在新快照里重建，见第三节）。
3. `health` 与 `circuit` 段的**字段值**（阈值、超时、窗口、半开门、`max_ejection_percent`）。

**不能换**（改了 = 这一轮 reload 拒绝并点名，旧表继续服务）：

4. **装配形状**：哪几个段存在。`routes` / `health` / `rate_limit` 段出现或消失决定装不装那几个 filter、建不建 `HealthTable`——那是链的形状，不是链的内容。链的形状只在启动时定（第四节讲为什么不换成指针）。
5. **`rate_limit` 段的任何变化**（包括只改 `rate` 的数字）。理由写在下面第二节，这一条是本轮最重要的一次"宁可窄"。
6. **命令行来源的任何参数**（已裁②）。文件里出现 `listen` / `workers` / redis / nodes 的键 = 顶层未知字段拒绝（[src/config/gateway_config.cc:98-101](file://wsl.localhost/Ubuntu/home/archer/workspace/l7gate/src/config/gateway_config.cc#L98)），启动时同样、运行期重载同样，同一个校验器。
7. **首装含 `source: mysql` 组的网关不许 arm 文件重载**：`--config-reload-ms > 0` 且文件里有 mysql 组 = **启动即拒绝并点名**。理由：节点表已经有一条发布路径（ADR-20），再叠一条就是两个节拍、两份真相在往同一个下标空间写；这正是已裁② 那句「合并规则是迟早被人改错的地方」要避免的形状。**代价照记**：这台机器上"节点表 + 文件策略同时热更新"没有实现，要做需另立任务与追加节，不在 S5-02 顺手做。

### 为什么第 5 条宁可窄（这一段是本追加节的裁处，不是省事）

限流的 `rate` / `burst` 是 `const` 成员（[src/filter/rate_limit.h:143-144](file://wsl.localhost/Ubuntu/home/archer/workspace/l7gate/src/filter/rate_limit.h#L143)），桶在某个键第一次出现时按这两个值构造，而**桶的生命周期 = `RateLimiter` 的生命周期、不回收**（同文件 132-134 行那条写成契约的注释）。于是"改限流参数"只有两种做法，两种都不干净：

- **换新 `RateLimiter`**：桶表整张换代。代价一：排水期两张表并存，同一条限额在两张桶表上各自有满预算 → 换代的几十毫秒里实际放行量可以超过配置值。代价二：所有键已积累的令牌清零 = 一次静默的限流行为变化，而"静默改流量行为"正是这个配置层从第一天就拒绝的事（`gateway_config.h` 顶部那段：`rate`/`burst` 没有缺省值就是这个立场）。
- **让参数变成原子可变**：老桶继续用旧速率活着（桶不回收 → 那些键**永远**用旧参数），只有新出现的键用新参数。这是**字面意义上的半套生效**，验收句④ 直接判死。

两条都比"不重载这一段"更糟，所以我选窄的那边：参数改了 → 拒载 + 点名 + 记账，旧桶表继续按原参数跑。要改限流参数就重启（这是演示面的一条真实限制，README 与 S5-03 面板说明里要写出来，不许装作没有）。

## 二、触发源：循环线程的一拍里做三件事

新增命令行参数 `--config-reload-ms MS`（范围 `0..86400000`，**缺省 0 = 不重载**，与 `--nodes-reload-ms` 同形、同"0 只在启动读一次"语义）。缺省关死的第二个好处：既有九条冒烟的关闭态键集合 golden（`scripts/golden/serve-keys-off.txt`，60 格）不用改，"关闭态与修前二进制逐行相同"那条判据原样成立。

节拍落在**指定那一个循环线程**（多 worker = `worker_index == 0`；单 Reactor 臂 = 那条循环）：

1. 每次循环醒来用**已经无条件在做**的那次 `monotonic_ms()`（[src/net/reactor.cc:332](file://wsl.localhost/Ubuntu/home/archer/workspace/l7gate/src/net/reactor.cc#L332)）判断是否到点；没到点一个 syscall 都不发。
2. 到点：`stat` 配置文件，比 `st_mtime` + `st_size`。没变 → 只记一次检查账，不读文件。
3. 变了：读文件 → 内容哈希 → 与当前世代的哈希比。**相同就不发布**——"查了一遍不等于发布了一次"是 ADR-20 第五节那对计数器立的口径（[src/main.cc:811-812](file://wsl.localhost/Ubuntu/home/archer/workspace/l7gate/src/main.cc#L811)），这里原样兑现：`touch` 不算一次重载。哈希用 FNV-1a 64 位自实现，不为此引依赖（禁令三）。

为什么不把解析搬进独立线程（S3-04 那条 mysql reload 线程的形状）：那条线程存在的理由是**阻塞 IO 的毫秒级往返**（ADR-20 第四节第一条），而 `stat` 加读一个已在 page cache 里的小文件不是同一类事。裁下的原话是"循环线程定时 `stat`"，我按字面执行，并给它配一条上界把风险量化：

- **文件大小上界 1 MiB**：`stat` 报的 `st_size` 超过它 → 这一轮拒载并点名（"配置文件过大，不在循环线程解析"）。这条既是防御（一个 100 MB 的文件会把 worker 循环按住几百毫秒），也是一格可断言的正反界：写一个超限文件确实会被拒（下界），正常文件正常发布（上界不越）。
- **每次发布的自报耗时** `config_last_publish_duration_ms`：解析 + 校验 + 建环 + 建快照全在里面，实测报，不口头保证"很快"。

节拍与唤醒代价的边界：`wait_timeout_ms` 缺省 200（[src/net/reactor.h:131](file://wsl.localhost/Ubuntu/home/archer/workspace/l7gate/src/net/reactor.h#L131)），所以**节拍 ≥ 200 ms 时不新增任何一次唤醒**（这条循环本来就每 200 ms 醒一次）；节拍 < 200 ms 才会像探活那样把超时夹小（[src/net/reactor.cc:298-302](file://wsl.localhost/Ubuntu/home/archer/workspace/l7gate/src/net/reactor.cc#L298) 是既有先例），代价是 `event_batches` 与 SLO #2 的空闲 CPU —— 实测报，不推断。

## 三、双 buffer 是什么东西（本追加节的核心）

**一个类型、一个发布点、一次原子 store。**

- `struct ConfigSnapshot`：**不可变**、**装配完整**的一份世代。装的是 `RouteTable`（含已编译的正则）、`NodesView` 形状的上游表（组表 + ever-seen 目标列表，环已建好）、`HealthPolicy`（含 `CircuitPolicy`）、内容哈希、世代号。它是 `const` 读的对象，构造之后没有任何写点。
- `ConfigPublisher`：`std::atomic<std::shared_ptr<const ConfigSnapshot>>`——与 [src/route/nodes.h:109](file://wsl.localhost/Ubuntu/home/archer/workspace/l7gate/src/route/nodes.h#L109) 那一个 `load`/`publish` 同一家形状、同一对 memory order（acquire / release）。**全仓库"配置换代"只有这一次 store**，在 watcher 那一拍里发生；worker 侧永远只 `load`。
- 上游表那半边复用 S3-04 已经跑通的一整套 worker 侧机器：文件世代的成员就是走同一张 `TargetRegistry`（`地址:端口` intern、下标只增）、同一个 `build_nodes_view`，扩容仍由 owner 线程在对账 tick 里做那四个私有面 + 共享表启用数（[src/net/reactor.cc:956-975](file://wsl.localhost/Ubuntu/home/archer/workspace/l7gate/src/net/reactor.cc#L956)）。**不新增第二套对账。**

环在新快照里重建（`build_hash_ring` 在发布前跑完，装配期建环的纪律原样成立）；`vnodes` 改了、成员增删了，迁移率按新环算——这是"整张表换一根指针"而不是"改一张正在被读的环"，ADR-20 第四节第二条的禁止项在这里不会被我绕过。

**为什么不"整条链换一根指针"**（[src/net/reactor.h:212-220](file://wsl.localhost/Ubuntu/home/archer/workspace/l7gate/src/net/reactor.h#L212) 那句预告的形状）：那句预告的前提是"运行期要改链"，而本轮定义域里链的形状不变（第一节第 4 条）。我把策略从链上对象里搬出去——链上三个 filter 持有的**活态**（`LbState` / `HealthTable` / `RateLimiter`）本来就该跨代存活，**策略**改读世代快照。于是：

- 那条注释真正禁止的事（往正在被循环读的 vector 里 push）没有发生，链一个字节都不动。
- 每请求不多一次 `shared_ptr` 拷贝。代价是别的：见第四节的读取窗口。
- 若将来真要段的出现/消失，那必须是新任务，并且走"整条链换一根指针"那一根——本追加节替它把形状留着，不预先造（契约 3.2 禁空壳）。

## 四、「存量请求走完旧表」的机制（验收句① 的后半句，写机制不写结果）

先说清今天的事实：主路径上链只有一个调用点，`RequestInfo` 是栈上临时对象，链跑完之后那条请求往下带的是**下标**，不是指针（[src/net/reactor.cc:1516-1552](file://wsl.localhost/Ubuntu/home/archer/workspace/l7gate/src/net/reactor.cc#L1516)；`Connection` 里只有 `upstream_target_` / `retry_target_` 两个 `std::uint16_t`，没有任何配置指针）。所以"走完旧表"不是靠给请求挂一个配置对象做到的——**是靠"它后面根本不需要新配置"做到的**。机制拆成三条可断言的性质：

1. **读取窗口是同步的一段**。每条请求在自己的那一帧里 `load` 一次世代指针（一次原子读，与 [src/filter/lb_filter.cc:73-82](file://wsl.localhost/Ubuntu/home/archer/workspace/l7gate/src/filter/lb_filter.cc#L73) 今天每请求取节点表快照同形），把它的裸指针放进 `RequestInfo`，链上三个 filter 读的都是这同一份。窗口结束时 `shared_ptr` 随帧释放——**一次请求只认一代**，不会前半段读 K 后半段读 K+1。
2. **窗口之外只留下"世代无关但代际稳定"的坐标**。请求在窗口里产出的是 `upstream_target`（全局目标下标）；下标 → 端点地址的解析表是 ever-seen 的、只追加、从不替换（第三节），所以一条已经决定了"发往 3 号成员"的请求，在 K+1 世代里查到的还是同一个 `地址:端口`。**改一个成员 = 新下标 = 新成员**，删一个成员 = 槽位留着但从候选集剔除——这两条 S3-04 已经裁过的语义，在文件重载上同样是"存量请求不需要新配置"的落点。
3. **reload 不碰任何连接**。不关下游、不断上游、不 drain、不迁 fd、不重置池（这正是它与 nginx reload 差的那一层：nginx 的 reload 要退 worker）。池桶向量的扩容只发生在 owner 线程的对账 tick，且 `pool_bucket()` 返回的桶指针今天只在单个函数体内使用（[src/net/reactor.cc:854-862](file://wsl.localhost/Ubuntu/home/archer/workspace/l7gate/src/net/reactor.cc#L854) 的三个调用点都在一帧之内）——"resize 会不会让在飞请求手上的引用悬空"这条要跑出来的答案是"根本没有跨帧引用"，而它能不能红由退化 H3 判（第六节）。

零截断就是这三条的合取，不是"我数了一遍响应条数"。断言的形状：切换瞬间在飞 N 条，每条拿到**完整的**响应（体长正确、不是被关断的连接），且**新表的路由不命中任何一条已经在飞的请求**（它们用的是 K 世代的规则）；同时 `connections_constructed == connections_destructed` 不因为换表而漂移。

### 一处混合世代，明知而留着

一条请求的**失败记账**（5xx / 超时进 `HealthTable`）发生在链窗口之外，读的是记账那一刻的世代参数，不是那条请求做路由决定时的世代。后果有界：换代表后的一拍里，K 世代开始的请求可能按 K+1 的阈值被计一次失败。我接受它，因为替它的做法是给 `Connection` 挂一个 `shared_ptr`：那要多 16 字节/连接、要动 `sizeof(Connection) == 528` 那条 static_assert（[src/net/connection.h:386-394](file://wsl.localhost/Ubuntu/home/archer/workspace/l7gate/src/net/connection.h#L386)），而 SLO #2 的每连接预算正是按这个尺寸讲的。**用每连接内存去换一条不破任何不变式的精度，不换。** 这句要一起进 README 与 S5-03 的说明，不许只留在 ADR 里。

`HealthTable` 自己那颗 `policy_`（[src/route/health.h:294](file://wsl.localhost/Ubuntu/home/archer/workspace/l7gate/src/route/health.h#L294)，今天构造后无人写）在 armed 时改成"世代持有者传入"：worker 那一拍把当前世代的策略 `shared_ptr` 缓存在自己身上（owner 私有，每拍一次原子读），事件路径用的是缓存里那份引用，每事件不多一次 refcount。重载关着时 `policy_` 原样读、一次原子都不碰。

## 五、旧配置的释放判据（验收句②）+ 记账

`ConfigSnapshot` 的构造与析构各打一次数（与 [src/net/stats.h:40-41](file://wsl.localhost/Ubuntu/home/archer/workspace/l7gate/src/net/stats.h#L40) 那对 `connections_constructed` / `destructed` 同一口径、同一个理由：**在册数 = 构造 − 析构，不是"表里有几个"**）。于是：

- 「旧快照在最后一个引用释放前不回收」是**测出来的**：用例持有一根 K 世代的 `shared_ptr`，发布 K+1，断言构造数 +1 而析构数**不动**、在册 = 2；放开那根，断言析构 +1、在册回到 1。
- 稳态（没有请求在跑）重载一轮 → 在册回到 1，不累积。这条同时是"引用计数泄漏"的反证：如果谁把世代长期钉住（比如错放在 `Reactor::config_` 里忘了换），这个数会停在 2 以上并且不再回来。
- 世代号 `config_gen` 单调 +1，只在成功发布时进；拒载不动它。

新增键（**只在 arm 时发**，关闭态一个都不发）：`config_gen`、`config_checks`、`config_publishes`、`config_rejects`、`config_rejects_syntax`、`config_rejects_scope`、`config_snapshots_constructed` / `_destructed` / `_live`、`config_last_publish_epoch_ms`、`config_last_publish_duration_ms`。拒载原因是**分类计数而不是自由文本**——面板要能画，脚本要能等；每一次拒载同时往 stderr 打一行带具体字段的话（校验器本来就说"是哪一项"，见 `gateway_config.h` 顶部那段严格性契约）。

## 六、坏配置拒绝且不半套（验收句④）的机制

- **校验与装配全部发生在新对象里**：读 → `parse_json` → `build_gateway_config` → 建环 → 建 `NodesView` → 建快照。任何一步失败就返回，把半成品丢掉，**在册世代纹丝不动**——因为换代只有第三节那一次 `store`，而它排在所有步骤之后。
- **同一条校验代码**：重载走 `build_gateway_config`，和启动、和用例是同一个函数（`gateway_config.h` 那句"分成两条的话，用例验的是没上线的那一条"在这里同样是硬要求）。我不为 reload 写第二份校验。
- **三类坏配置都有各自的落点**（验收句④ 列的三样）：语法错 → `config_rejects_syntax`；字段越界（类型/取值域/未知字段）与超大小上界 → `config_rejects_scope`；**两源冲突** = 文件里出现命令行来源的键，或 `source` 与 `targets` 同给 → `config_rejects_scope`（同一把尺，因为它就是第一节第 6、7 条的运行期形态）。
- **唯一的例外要在纸上看得见**：失败的那一轮可能已经把新端点 `intern` 进 ever-seen 注册表（`build_nodes_view` 在装配成员时就 intern）。后果是有界的：下标空间出现**没被任何已发布组引用的槽位**，它不接流量、不进候选集、不占安全阀分母（分母读的是已发布快照的组），代价与 ADR-20 第四节那条"删掉的成员保留槽位"同族。**我不为它做回收**——那是没有证据的聪明。断言形状：坏配置里写一台新地址 → 拒载 + 在册世代不变 + 后续正常配置仍能发布。
- 上界必配下界：`config_publishes` 在坏配置那轮**确实不动**要有对照，而对照就是同一脚本里紧接着那次好配置确实 +1。

## 七、代价（三条，都要实测进数字，不许停在"应该不贵"）

1. **健康表容量按全下标空间**。arm 了文件重载的网关，`HealthTable` 的容量走 [src/main.cc:760-769](file://wsl.localhost/Ubuntu/home/archer/workspace/l7gate/src/main.cc#L760) 那一格已有的选择（节点表启用时 = `kMaxUpstreamTarget + 1`）：因为文件重载同样能带进任意多的新地址，而槽位不能运行期 resize（ADR-20 第四节第三条）。价格 = `Entry` × 65535 的常驻内存，**用 S5-01 的 `rss_kib_now` 两臂量**（arm vs 不 arm，同一装配、同一负载），报绝对数。
2. **watcher 那一拍的解析耗时**：`config_last_publish_duration_ms` 实测；同时报它对那一拍之后第一条请求的影响（在 watcher 所在 worker 上，解析期间它不服务别的连接——这是"不在循环线程外再起一条线程"这个选择的真实代价，写出来）。
3. **热路径每请求一次世代 `load`**（arm 时）：主尺按已裁⑨⑤ 那条走——每请求 `epoll_modifications` 整数等式（B-019 那条线，定标 20,000 条/连接那批口径），p50/QPS 只当带外报警器，且"无可分辨"旁边固定写死本档分辨率 ≈128 µs/请求那句限定。关闭态不多一次原子读、不多一次 syscall、不多一个键。

## 七附 · 有人提议把健康表容量改成「按文件大小推导」，那条推导不成立（2026-11-03 自查）

第七节第 1 条那个 19.1 MiB 摆出来之后，我自己提过第三条路：**不拍预留数，而是按「1 MiB 的合法配置
最多能报出多少个成员」在构造期推导一次容量**。实现之前把它算穿，**结论是错的，不能做**，错在两点：

1. **单位算错**：最小成员文本是 `{"address":"a","port":1}` 那一类，约 24 字节，不是我以为的 ~50 字节。
   于是推导值 ≈ 41,943，省下的只有 19.1 MiB 里的约 7 MiB——**我当时报的「大约两万槽、≈6 MiB」是估的，没算过**，
   这条照记在 FAULT-INJECTION §21.2 第 7 条。
2. **更要命的是那条上界根本不封跨代累积**。`ever-seen` 下标空间是**只增不减**的（ADR-20 第四节）：
   第一代把文件填满可以烧掉 4 万个下标，**第二代换一批全新的成员再烧 4 万个**，累计 8 万 > 41,943。
   「单代之内最多能报出多少成员」管不住「历代一共报出多少」，所以任何按单代字节数推导出来的容量，
   都会在某一轮合法重载之后变成越界写或静默截断——那正是本任务验收句④ 判死的「半套」形状。

所以现在这台网关的容量只有两个站得住的选择，我选第一个、第二个留给项目所有人裁：

- **（已执行）维持下标空间上界 `kMaxUpstreamTarget + 1` = 65,535 槽**（`kMaxUpstreamTarget` 本身是 65,534，
  由 `router.h` 里 `std::uint16_t` 的值域决定）：它是
  值域的自然界，任何世代、任何累积都越不过它；代价就是实测到的 **+19.1 MiB 一次性常驻**（B-020，
  两臂组内极差 <0.3 MiB）。面板与 README 引用这格要写「含热重载的预留槽位容量」，不许画成「网关自己吃内存」。
- **（待裁）引入一个跨代累积上界**：注册表累计到 N 个下标之后，带新成员的世代一律拒载并点名
  （`config_rejects_scope` 多一个理由）。省内存，但**新增一类「配置合法却不生效」的失效**，而且 N 除了
  由类型决定的 65,535（= `kMaxUpstreamTarget + 1`）之外没有来路——动的是「什么配置能生效」的边界，属口径。本轮 `src/` 一字未动。
## 八、验收句 → 断言落点（谁守哪一句）

| 验收句 | 落点 |
|---|---|
| ① 新请求用新表、存量走完旧表、零截断 | ctest `S5_02_*`：切换瞬间 N 条在飞逐条完整 + 新路由不命中在飞请求；冒烟 `smoke-s5-02.sh` 阶段 2（真进程：改文件 → 在飞 + 新请求两批各自的落点） |
| ② 旧配置释放判据可断言 | ctest：第五节那三格（钉住 → 析构不动 → 放开 → 析构进 → 在册回 1）；`connections_constructed == destructed` 跨重载不漂移 |
| ③ 生效耗时实测 | 冒烟阶段 3，B-015 的形状：T0 = 脚本写完文件（墙钟），T1 = 网关第一次用新表服务到请求；`config_last_publish_epoch_ms` 落在 [T0, T1] 内；两轮起报，粒度 = 节拍，**报毫秒数** |
| ④ 坏配置拒绝 + 有账 + 不半套 | ctest 三类各自 + 冒烟阶段 4（语法错 / 越界 / 文件里写 `listen` 三臂，逐臂断 `config_rejects*` +1 且 `config_publishes` 不动、旧表仍在服务） |
| ⑤ 命令行来源不参与重载 | ctest：`--config-reload-ms` 本身不进文件；文件含命令行键 → 拒载并点名；启动时 mysql 组 + arm → 退出码 2 且话里指名那条切分 |

## 九、阳性验证清单（能红才叫绿，实现那轮逐条跑红再还原）

- **H1 半套发布**：把 `publish` 挪到校验之前（或让建环失败也发布）→ ④ 那三条与"在册世代不变"必须红。
- **H2 世代不回**：让 `ConfigSnapshot` 的析构不发生（比如把世代塞进一个只增的容器）→ ② 那格"放开后析构 +1、在册回 1"必须红。
- **H3 换代不碰连接**：在 `sync_config_snapshot` 里遍历注册表把在册连接逐个关掉（那才是热重启的形状）→ 端到端那条"在飞三条各拿完整旧后端响应"当场红。**这一格替换了原计划**：我本来打算把 `&snapshot->groups` 的引用故意存到帧外，用 ASan 去抓跨帧悬垂（本节第二点点名的那个重点靶区）——**那条本轮没执行**，因为它要先故意写一段未定义行为才能存在，而"世代在链窗口之外不再被读"这件事有一个更直接的守门对象：换代到底动不动连接。所以这里照记一句实话——**ASan 那一臂 364/364 通过只说明"没有红"，不等于"那个形状被验过"**；要真验它得立一条故意的 UB 用例，归 S5 之后或 S5-08 门禁前决定，别把它算进本轮的五处。
- **H4 越界也生效**：把命令行来源键的未知字段拒绝在 reload 路径上关掉 → ⑤ 必须红。
- **H5 世代不进请求**：链调用点把 `RequestInfo::config` 写成 `nullptr`（两个读表的 filter 于是退回构造时那份兜底表）→ 端到端那条"换代后新请求用新表"必须红。这一格守的是**一次读取 = 一次决定**：如果每条链各自去 `load` 一次发布点，换代恰好落在两环之间时，路由指过去的那个组下标可能在新表里根本不存在，或者更坏——存在、但是另一个组的成员。红的方式是"新请求没换表"，不是崩溃。
**执行结果（2026-11-03 实现那轮，名单与红数在 `docs/FAULT-INJECTION.md` §21.1）**：五处全部真打红——H1 红 4 条、H2 红 2 条、H3 红 1 条（端到端）、H4 红 5 条、H5 红 1 条（端到端）。**H1 与 H2 第一版 0 红**：H1 原本打在 `snapshot == nullptr` 那一格，而六类拒绝全都到不了那里（语法/字段/越界/形状/超大都在装配之前就返回了）；H2 原本加在 `reload_tick` 里，而断引用计数的两条用例是**直接调 `publish()`** 的。两处都改打在「所有写者都必经的那一处」才有红——这条与本节第二点「ASan/TSan 的重点靶区、不接受逻辑上应该没问题」是同一条立场。
- 每条都要"第一版没红就先弄清为什么没红"（§15.3 / §16.3 / §20.2 那三次的教训），不许改用例来凑红。

## 十、这一节没做什么（防被读成做了）

不改链的形状；不重载限流参数；不做节点表与文件策略同时热更新；不做配置版本回滚与 diff（上面第一节第 7 条、原第三节各一条）；不给配置做推送（`inotify` / `SIGHUP` 已裁否）；不做下标空间回收；不在本轮量 `read_disarmed_connections` 在 100k 拓扑下的成本（B-019 那两笔"没测"仍挂着，归 S5-07）。
