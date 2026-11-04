# ADR-28 · 地址合法性前移到配置校验层：`address` 只收 IPv4 字面量（S5-09，修法①）

日期 2026-11-04。落笔顺序按契约 8.2：**这份 ADR 之前 `src/`、`tests/` 一字未动**。
本任务形照 S2-06a / S4-06a——阶段关门之后追加的一条，挂在 `TASKS.md`「## S5」段末，ID **S5-09**。

## 这一节在裁什么

S5-05 冒烟的退化 C1 抓到一条被测方事实（FAULT-INJECTION §24.1、ARCHITECTURE 1.38 末段）：配置校验只挡空串、
不挡字符集，而逐目标健康键 `health_t<i>_<address>_<port>_<metric>` 把 `address` **原样拼进指标名**，
ADR-24 第五节的机械变换只处理 `.` 与 `:`。于是带连字符的地址（任何主机名）能一路走进观测面，造出
Prometheus 名字法 `[a-zA-Z_:][a-zA-Z0-9_:]*` 里非法的指标名，后果不是那一格坏，是**整份 `/metrics` 抓失败**
（实测 `lastError = invalid metric type "a_18081_ejections counter"`，那台网关整体 down、101 个键一起丢）。

三条修法里项目所有人 2026-11-04 选了 **①：配置校验层直接只收 IPv4 字面量**。本 ADR 就是它的语义。

## 一、为什么这不是一条新规矩，而是把一条既有事实前移

网关的地址解析本来就是 `inet_pton(AF_INET)`（`src/net/socket.cc` 的 `to_sockaddr`，`socket.h:64/70` 那两个
入口的参数名就叫 `ipv4_addr`）。S5-05 实测过它的后果：同一份配置只把 `address` 从 `127.0.0.1` 换成
`localhost`，就是 `HTTP 502` + `upstream_connect_failed=1` + `responses_completed=0`，对照臂 `200 + seq=1`
（ADR-27 第一节第 ② 条、ENVIRONMENT 5.3）。

也就是说：**那个成员今天就已经连不上**，它只是同时把整份观测面也拖死了。本 ADR 不新增任何"能配什么"的范围，
它做的是把"我自己只认字面量"这件事从**运行期静默失败**挪到**装配期拒载 + 点名**。

## 二、裁处：谓词必须与被消费者同源，不许另写一份

- 新增 `bool is_ipv4_literal(std::string_view) noexcept`，放在 `src/net/socket.h`，实现放在 `socket.cc` 里，
  **就是 `::inet_pton(AF_INET, …)` 那一次调用**（`to_sockaddr` 内部同源复用，不留两份定义）。
  理由：手解四段点分十进制或写正则会造出"校验说合法、连接说非法"的分叉——那种分叉最后一定由读文档的人付账。
  分层上不绕：`net/` 从来不含 `config/` 的头（本仓库里 `metrics_server.h` 已经含过 `net/socket.h`），
  所以 `config → net/socket.h` 是新的、单向的一条边。
- 调用点两处，都在既有校验的"空串检查"之后：
  1. `src/config/gateway_config.cc` 的 target 解析（`address->text.empty()` 那一格下面）；
  2. `src/config/nodes_store.cc` 的节点表行解析（`address.empty()` 那一格下面）。
- **错误话必须点到具体位置与实际读到的值**，形状照 S5-02 那句既有的
  `[serve] config_last_reject=配置顶层 有本配置不认识的字段 listen（认识的是：…）`，例如
  `upstreams[0].targets[1] 的 address="upstream-a" 不是 IPv4 字面量（网关按 inet_pton 解析，主机名不会生效）`。

## 三、为什么不放在渲染层（修法②被否掉的理由）

把 `sanitize_metric_name` 扩成"非 `[A-Za-z0-9_]` 一律转下划线"确实能让 `/metrics` 活下来，而且对 IPv4 字面量
逐字不变、历史键名可比性保得住。但它等于承认"主机名是合法配置"：名字合法了，成员照样连不上
（`inet_pton` 不认它），于是把一个**早失败**改成一条**永远不可用的静默成员**——那比整车失效更难查。
修法③（维持现状 + 文档加一句限制）把同一条输入留在合法域里，更差。

## 四、定义域：这一条只管两个来源，`--upstream` 命令行不动

- **管**：配置文件 `upstreams[].targets[].address`；MySQL 节点表的 `address` 列（`source: mysql` 的组）。
  两者都是逐目标健康键的来源，都进同一个 `target_names`。
- **不管**：`--upstream` 命令行。理由有两条，第二条是硬事实：
  1. 它是部署事实不是流量策略（已裁②那条切分），把它从"运行期 502"改成"启动失败"是改 CLI 语义，另一件事；
  2. **它结构上产不出那些键**——`--config` 与 `--upstream` 只能给一个（`main.cc:721` 那句参数错误），
     而逐目标族只在装了 `health` 段时才发（`main.cc:881` 那个 `if (gateway_config.health().present)`），
     命令行那一支没有 `health` 段。所以主机名走命令行时的后果仍是 502 + `upstream_connect_failed`，
     与 S5-05 的实测一致，本任务不改它也不掩盖它。

## 五、键名与计数：一个键都不新增，一字口径都不动

- 文件那一支的拒载走**既有那一类**：`build_gateway_config` 失败在重载路径上记 `config_rejects_scope`
  （`config_reloader.cc:265`，stderr 那句话的前缀本来就是"配置重载被拒（字段）"），
  启动路径上仍是 rc=2 并点名（S5-02 已把这条形状钉成用例）。
- 节点表那一支走既有的**整批拒载、保留上一份**，账记在 `nodes_reloads_failed`。
- `health_t<i>_…` 那一族的生成规则（含 ADR-24 第五节的 `.`/`:` → `_`）**一个字不动**——
  所以 S5-01 的 91 键 golden、`smoke-s5-01` 的键集合机械互证、面板那 20 条表达式全部原样有效。
  这条"没动口径"要做成断言，不做成我的说法（见第六节 ②）。

## 六、断言落点（验收句 → 用例）

| 验收句 | 落点 |
|---|---|
| ① 非法地址：启动 rc=2 + 点名；重载那一轮 `config_rejects_scope` +1、`config_publishes` 一动不动、旧世代仍在服务 | `tests/config_test.cc` 新组 `S5_09_*`（纯函数 + 校验器）＋ `tests/config_reload_test.cc` 的重载臂；真进程形状进 `scripts/smoke-s5-02.sh` 的"坏配置"那一阶段（它本来就是一类一次、后面紧跟一次好配置） |
| ② 下界：同一份配置只换成合法 IPv4 → 确实发布，且 `/metrics` 那一族键名与换之前逐字相同 | `S5_09_…AcceptsLiteralAndKeepsKeyNames` + 冒烟里那条键集合机械互证（S5-01 的形状复用，不新写第二份） |
| ③ 节点表一行非法 → 整批拒载、保留上一份（`nodes_reloads_failed` +1、在册成员数不变） | `tests/nodes_test.cc` 的行校验臂（不连库，直接喂行）；真 MySQL 那一臂进 `scripts/smoke-s3-04.sh`，代价见第九节 |
| ④ 一合法一非法混在同一个 targets 里 → 整份拒载、不半套 | `S5_09_…RejectsWholeDocumentWhenOneTargetIsBad`（断言的是"在册那一代连被碰过都没有"，不是"响应条数") |
| ⑤ 阳性验证至少三处真打红 | 见第七节 |
| ⑥ 门禁不回退 | 三 preset + tsan + 十四条冒烟 + 60/91 键 golden 逐行相同 |

## 七、阳性验证（必须真打红，第一版打不红就查为什么，不改用例凑红）

- **V1** 把 `gateway_config.cc` 里那条新检查整块摘掉 → ① 与 ④ 两条用例应红。
- **V2** 把检查改成"只查 `targets[0]`" → ④ 那条应红，而 ① 仍红在单 target 那一臂（**红在不同格才算两条独立判据**）。
- **V3** 把 `is_ipv4_literal` 退化成"非空即合法" → ①②③ 全红（② 那条证"合法那一支也依赖真解析"，
  如果它不红，说明 ② 断的不是我想断的东西，先修判据再打）。

**执行后更正（2026-11-04，实测红集合在 FAULT-INJECTION §27.1，原预期照留着不删）**：
V3 实际红 6 条 = ①的拒绝臂 + ④ + 守卫 + 重载臂 + 谓词对照臂 + 节点表臂，而**②不红**——
上面那句"如果它不红说明 ② 断错了对象"我自己兑现了：② 断的是"合法那一支的行为没变"，
宽松谓词不伤它，所以 ② **该不红**；改的是这句预期，不是用例。V2 实际只红 ④ 一条（比原预期更干净：
① 的样本每个配置只有一个 target，下标 0 那一条仍然被检查，所以它在 V2 下合法地绿着）。

## 八、不在本任务做（契约 8.2 禁令一）

- 不改 `sanitize_metric_name`、不改任何键名、不动 SLO 与口径表。
- 不给 `--upstream` 加名字解析（那是"网关要不要做 DNS"这件更大的事，契约里没许诺过）。
- 不动 `--config` 与 `--upstream` 的互斥规则。
- 不顺手碰另两笔待裁（nginx 基线镜像加 perf 层、`l7gate_c100k` 多线程观察者）——那是项目所有人的排期。

## 九、代价汇总（照记，不是免责声明）

- 定义域收窄是真实的：**以前**"配了主机名但网关连不上、观测面整车失效"是一种可观察的坏；
  **以后**那种配置**根本起不来 / 那一轮不生效**。运维上少了一种静默形态，多了一种启动失败——这正是选它的理由，
  但必须写进 README 的配置段，不许只在 ADR 里躺着。
- 节点表那一支的真 MySQL 臂要改 `scripts/smoke-s3-04.sh`（或 `smoke-s5-05` 的那份配置），
  它需要容器在位；本轮如果只做纯函数臂，就在文档里明写"真库那一臂没测"，不含糊过去。
- 每请求路径**一个字节都不加**：检查只在装配/重载那一拍发生，`Connection` 与 `sizeof` 不变，
  所以 B-007/B-021 那两格每连接内存读数不受影响（这也是为什么不动键名比动渲染层更便宜的另一个理由）。

## 十、落盘顺序与本 ADR 之后要同步的文档

顺序：本 ADR → `src/net/socket.h`+`socket.cc`（谓词）→ `gateway_config.cc` + `nodes_store.cc`（两个调用点）
→ 用例 → 三处阳性验证真打红 → 门禁面（三 preset + tsan + 十四条冒烟）。
同步：`README.md` 配置段与"诚实的边界"（那条限制从"会整车失效的输入"改成"拒载并点名"）、
`ARCHITECTURE.md` 1.38 末段那条**没修**的缺陷记录更正为已修 + 第 5 节踩坑索引加一行、
`FAULT-INJECTION.md` 新增 §27（三处阳性验证与本轮仪器侧自错）、
`TASKS.md` S5-09 行置 DONE + 「已知待决事项」里那条连字符缺陷结案、
`ROADMAP.md`「## S5」债表那一格更新（这条结掉，另两笔仍挂着）。
`BENCHMARK.md` 不新增读数，因此不动。
