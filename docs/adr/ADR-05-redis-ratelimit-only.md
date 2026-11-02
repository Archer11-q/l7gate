# ADR-05 · Redis 只承担分布式限流计数，及其失效降级

- 状态：已采纳（降级链三条一字未动；**S4-04 已落地：机制＝授权额度 + 本地桶执行、键口径、hiredis 引入与 `redis:7.2.5` 的连通取证**——文末「落地追加（2026-11-02）」第一节起。**契约 9.1 那句"未取证前不算既成事实"在 2026-11-02 由探针七步全通撤销**，镜像身份的核验缺口照记在同节）
- 日期：2026-10-26（追加 2026-11-02 两次：认领时的机制与取证状态、实现时的实测与踩坑）
- 相关：契约 3.1、3.3、3.4、11.5；ADR-20（阻塞 IO 不进 worker 循环的先例 + 版本取证形状）、ADR-21 第二/四节（令牌桶数学与键口径——本 ADR 只加聚合形态，不重定义它）、ADR-19（策略/进程形态分家：`--redis-*` 全走命令行）、ADR-14（跨线程接触面）、ADR-12（安全阀与 Redis 无关，写明防混）

## 我选择什么

Redis 在本项目里的职责**只有分布式限流计数**。不做缓存、不做 session 存储、不做配置分发、不做服务注册。

## 为什么 Redis 必须出现

不是为了「用上 Redis」。单机令牌桶在**多副本部署下会各自放行**：阈值配 1000 QPS，跑 3 个副本就是实际 3000 QPS 打到上游，限流形同虚设。计数这件事必须有一个全局唯一事实源，这就是 Redis 登场的唯一理由（契约 3.1）。

**「没有它会怎样」的标准答案**：限流在多副本下失准，上游收到的流量是配置的 N 倍，故障时反而放大压力。

## 失效模式才是重点，「用了」不是

工业界做法（Envoy 官方 ratelimit）是：Redis 不可用会导致请求被拒。我不照抄这个默认行为，因为「限流依赖挂了 → 全站 503」是把一个辅助组件变成了全局故障域。我的降级链是三层：

1. **热备**：Redis 主从，主挂切从（本项目在单机语境下用单实例 + 明确的失效行为验证，热备属于部署形态说明，不假装它解决了问题）。
2. **放行模式（fail-open）**：Redis 连接超时 / 命令失败 / 不可达，判定超时后**放行**请求，同时打点计数并把「限流失准」暴露成可观测状态——静默降级比降级更危险。
3. **严格本地令牌桶兜底**：降级期间用比配置阈值**更严格**的本地阈值顶住，保证最坏情况是少放一些，而不是无限放到上游。

这条链必须真跑出来：契约 4.1 要求 S4 门禁交出「Redis 降级验证」的实测，方法就是 `FAULT-INJECTION.md` 里应用层停掉 Redis 后的时间线。

## 硬性约束：压测时给 Redis 绑核

契约 3.3 写死了：**压测时必须用 `cpuset` 把 Redis 绑到固定核，并把 CPU 分配写进 `BENCHMARK.md`。** 否则 Redis 抢 CPU 会污染 p99，测出来的尾延迟归因到错误的组件上。这是数据洁净度问题，不是可选优化项。绑核方案目前还没定，记在 ENVIRONMENT.md 第 7 节。

## 依赖

hiredis v1.2.0，Ubuntu 26.04 的 apt 候选 `1.2.0-6ubuntu4` 与契约字面命中，所以不改契约。

## 落地追加（2026-11-02，S4-04）：授权额度机制、键口径、取证状态与降级三段

这一节只加实现边界，不回头改上面那条降级链的语义。

### 一、hiredis 的引入与取证状态（禁令三）

**我用 apt 的 `libhiredis-dev 1.2.0-6ubuntu4`，不引任何封装层，也不自己写 RESP 客户端。**

- 字面命中，没有偏离要论证：`dpkg -l` = `1.2.0-6ubuntu4`（`libhiredis-dev` + `libhiredis1.1.0` 同版本）、`/usr/include/hiredis/hiredis.h` 的 `HIREDIS_MAJOR/MINOR/PATCH` = **1/2/0**、`pkg-config --modversion hiredis` = **1.2.0**。
- **一个会被问到的细节**：SONAME 是 `libhiredis.so.1.1.0`——ABI 号和 API 号不是一回事（apt 的运行时包就叫 `libhiredis1.1.0`）。契约 9.1 锁的是 hiredis **v1.2.0**（API 版本），命中；运行时按 SONAME 加载，`ldd` 实测 `libhiredis.so.1.1.0 => /usr/lib/x86_64-linux-gnu/libhiredis.so.1.1.0`。
- 不自己写 RESP 客户端的理由与 ADR-04 不同源，别混着讲：ADR-04 的自研边界是**网络层**（epoll ET + Reactor），hiredis 不做事件循环——它只做字节编解码与连接建立，socket 仍归本仓库的 `net/`。契约 9.1 把 hiredis 列为锁定依赖，我照它引入。
- **客户端侧就位取证已完成（2026-11-02，只验库、不建连接）**：`g++ -std=c++20 -Wall -Wextra` 编译 + 链接 rc=0，`redisFormatCommand("EVAL %s %d %s %d", "return 1", 0, "k", 1)` 产出 **49 字节**、与我手写的 RESP `*5\r\n$4\r\nEVAL\r\n$8\r\nreturn 1\r\n$1\r\n0\r\n$1\r\nk\r\n$1\r\n1\r\n` **逐字节相等**（`byte_exact=1`）。
- **服务端 `redis:7.2.5` 的连通取证未完成**：Docker Desktop 未运行（实测：发行版内无 `docker`、`/var/run/docker.sock` 与 `/mnt/wsl/docker-desktop` 不存在、2375/2376 关闭、Windows 侧 `docker.exe info` 报 `npipe:////./pipe/dockerDesktopLinuxEngine` 不存在、`wsl -l -v` 里 `docker-desktop` = Stopped）。hiredis 1.2.0 ↔ Redis 7.2.5 没有版本偏离可论证，**取证的是"字面命中的两端真能对话"**——含 EVALSHA 与 NOSCRIPT 回退那一条。**取证前上面那句「必须真跑出来」不撤销。**
- **一条当场撞到的 API 坑（实现期的雷，写在这里）**：`redisFormatCommand` 的出参要用 `free()` 释放；我按 hiredis 存在的另一个口 `redisFreeSdsCommand()` 去释放，**当场 `free(): invalid pointer` 中止（rc=134）**。两个释放口不通用，看名字猜是猜不出来的。

### 二、机制：Redis 授予额度、本地桶执行（项目所有人 2026-11-02 裁）

**Redis 不重新定义限流数学，它只回答"这个进程在这一刻最多还能放多少"。** 本地那只桶（S4-03/ADR-21 第四节）继续是执行器，`rate`/`burst`/键/溢出全不动；Redis 线程每拍给每个桶挂一个**上限夹子**。

- **结算线程一条**（`std::thread`），与 ADR-20 的 reload 线程同形状：Lua EVAL 是阻塞往返，不进 worker 事件循环。本仓库在 S3-04 已把这条写成明文先例（`main.cc` 里那句"MySQL 查询是阻塞 IO，不进 worker 事件循环"），Redis 没有理由破例。
- **Lua 桶住在 Redis**：每键一个 hash 存 `tokens_milli` + `last_ms`。结算线程上报自上次结算以来的放行数 `used`、带上 `now_ms`，脚本做与本地逐位同一套数学（补货 `rate×elapsed` 夹到 `burst`、扣 `used`、再夹 `burst`），**返回剩余毫 token**；结算线程把返回值写进该桶的 cap 原子。
- **本地准入**：`try_acquire` 的时间补货照旧，补完在 CAS 循环内夹一道 `min(tokens, cap)`——夹子在 CAS 里生效，多 worker 之间没有两套答案。`cap` 缺省 = `burst`（= 不夹），降级时也设回它。
- **时钟由网关提供，Redis 不调 `TIME`**：脚本里 `redis.call("TIME")` 会让"非确定命令之后写"这条限制咬人（EVAL 复制形态下直接报错），客户端给 `now_ms` 顺带让两端用同一个单调钟——单机语境下这是**加强不是妥协**。代价照记：跨主机部署时"同一个钟"不再成立，归部署形态，不在本任务假装解决。
- **夹子的写入只会咬紧不会放松**：授权低于本地补货时少放，`set_cap_milli` 又把它夹在 `burst` 以内，所以一次授权永远不会使本地多放。**但"上报滞后"是反方向的，必须一起写清**：结算上报的是快照那一刻的 `used_total`，从快照到命令发出之间这个键又放行的条数这一拍不会被报到，于是服务端余额**暂时被高估**，上界 = 该键在一个结算周期内的实际放行数（最坏 `burst` 条——本地桶满时一个周期内最多就放这么多），下一拍扣回。这条是本接法的固有代价，不是缺陷未报：要消掉它只能每请求问一次，而那两条我已经否掉。多副本下的聚合口径因此是"**每个周期内的超额有界**"，不是"任意时刻严格相等"。
- **跨线程流动的东西只有两样**（cap 原子 + `used` 原子读）；连接对象、fd、per-worker 状态一概不流动——ADR-14 一字不动，新增并发面是"一条线程 + 每桶两个原子"，与 S3-04 同一形状，`tsan` 零 data race 照旧是可证伪命题。
- **Redis 连接只属于结算线程**：单条同步连接（`redisConnectWithTimeout`），命令超时 = `--redis-timeout-ms`；不做重连状态机以外的花样——失败计数 + 下一拍重试就是恢复路径的全部。

**为什么不选另两条接法**（裁过的岔路留在这儿，防下一次重开）：

- **每请求同步问 Redis**：数字最严，但它把阻塞往返塞进 worker 事件循环，同一条 worker 上其余连接一起被 RTT 拖住，直接违反 S3-04 那条先例，也会把 SLO #1 的 4–6 ms p99 带（B-008）变成"Redis 的延迟"。
- **每请求异步问（链加 defer）**：语义最纯，代价是给 `Decision` 加第三种答复"压住等回调"（改 ADR-11/ADR-18 的链形状与 reactor 热路径）、每 worker 一条 hiredis 异步连接进自己的 epoll、还要补一条挂起连接的超时表态。改动面与回归面都远大于本任务的验收所需——而"每请求都过全局计数"这件事的**多实例收益**，恰恰是 TASKS「明确不在 S4 做」里点名不做的（单机项目验不了多网关实例）。

### 三、键口径（承 ADR-21 第二/四节）

Redis 侧键 = `l7gate:rl:v1:<kind>:<hash16>`，`kind ∈ {path, hdr:<头名>}`，`<hash16>` = 本地键字符串的 FNV-1a 64 位（环那套的两个常数，ADR-19 追加节钉过的发布向量）转 16 位十六进制。

- **定长、无编码**：path 键含原始 query、`header:` 键的基数与字节形态由客户端控制——把它们原样拼进键名，Redis 侧的键长就不可算、`redis-cli` 里会看到任意字节。哈希掉换来一件更硬的东西：键与 S4-03 本地键**同源**（同一个 `RateLimiter` 算出来的那个字符串），不存在"第二套键定义"。
- **碰撞方向是"更严"不是"更松"**：两个不同键撞到同一个 64 位哈希 → 共享同一只全局桶 → 只会少放。16384 个键下生日碰撞概率 ≈ 16384²/2⁶⁵ ≈ **7.3e-12**，我不为它做任何反查表。
- 本地键 → Redis 键的映射基数仍是 **16384**（ADR-21 第四节第 4 条那个硬上界），溢出键共用 Redis 侧的 `l7gate:rl:v1:ovf` 一只桶——与本地溢出桶同一条退化，不是第二格账。结算只遍历"自上次结算以来被用过"的键（每桶一个 `used` 原子做判据），不是每拍全表 16384 次 EVAL。

### 四、开关与参数：只走命令行（ADR-19 的切分原样适用）

是否跨进程聚合是**部署事实**，不是流量策略——所以 `rate_limit` 段**不加新字段**，开关就是"给没给 `--redis-db`"，与 `--nodes-schema` 那条完全同形。

| 参数 | 缺省 | 语义 |
|---|---|---|
| `--redis-db <host:port>` | 不给 = 分布式不启用 | Redis 端点（经 Docker 端口发布，本机 `127.0.0.1:16379`） |
| `--redis-settle-ms MS` | 200 | 结算周期（100..60000）；与 ADR-21 超时扫描同一粒度 |
| `--redis-timeout-ms MS` | 1000 | 单次连接/命令超时（100..60000）；一个挂死的 redis 不能把结算线程变成僵尸 |

- **矛盾即拒绝**（退出码 2，话里点名）：给了 `--redis-db` 却没装限流（`rate_limit` 段不存在）= "给 Redis 参数但没有任何桶可聚合"；`--redis-settle-ms` 小于扫描粒度还说得过去，但 `--redis-settle-ms ≥ burst/rate×1000` 时夹子永不咬人（本地补货总先满）——这一格**不报错、写进文档**，因为它只是"没生效"不是"错"。

### 五、降级三段（验收主战场）

1. **Redis 挂**（`docker stop` 或端口不通）：连续 `--redis-settle-ms` 拍失败 → `redis_settles_failed` 递增、`rate_limit_degraded=1`、各桶 cap 设回 `burst`（= 回到 S4-03 的纯本地语义）。**拒绝能力不消失**——本地桶照旧按 rate/burst 拒绝，429 继续出。
2. **Redis 回**：下一次成功结算 → `rate_limit_degraded=0`、cap 重新按授权夹住、`redis_settles_ok` 继续增长。恢复不需要人工动作，形状同 ADR-20 的"恢复后第一次成功 reload 换上新表"。
3. **启动时 Redis 不可用**：`--redis-db` 给了就连不上 → 网关**照常启动**、直接进降级态、后续自动接上。理由与 S3-04 同一条：一个辅助依赖不能变成全局故障域（正文那条立场在启动路径上的落点）。

### 六、观测面（键名两条装配路径一致，承 S4-03 纪律；**一行一个键**）

`redis_settles_ok` / `redis_settles_failed` / `redis_degraded`（0|1）/ `redis_settle_commands` / `rate_limit_capped_rejects`。启动横幅 `redis=on addr=… settle_ms=… timeout_ms=… kind=…`，退出账一并打。

- **为什么单独强调"一行一个键"**：这些数只在退出时打，而冒烟脚本用 `^\[serve\] key=` 这种行首锚取数——五个键挤一行时，只有行首那个读得到，其余四格读回来是**空串**，空串在判定里长得像"0"。本轮第一版冒烟脚本就是这样一路"绿"过去的（`capped_rejects=''` 被当成了 0）。这是 CONTRACT 那条"报告里一片 0 比一片 ? 危险"的第 N 次应验，所以口径写进正文：**统计行一律一行一个键**。
- `redis_grant_last_milli` 从本节草稿里删掉了——**它没有第一个读者**：授权是 per-key 的，一个"最后一次"的标量读数回答不了任何问题，而 Redis 侧那格 `HGET <key> tokens` 才是能核对的东西（冒烟阶段 2 就是拿它当"消耗真记在服务端账上"的证据）。
- `redis_settle_commands` 是**累计**发出的 EVALSHA 条数，不是"最后一拍的条数"：后者在安静收尾那一拍会报 0，等于一个没有读者的形状。它守的是"结算只遍历有需求的桶"（实测：六连击 + 3 拍 → `settles_ok=3 / settle_commands=1`）。

### 七、cpuset 绑核（正文最后一条硬性约束的落点，也结掉 ENVIRONMENT 第 7 节那一格）

**只给 Redis 绑核**：`docker run --cpuset-cpus 15`（本机 `nproc`=16）。网关与压测工具**保持既有"未绑核"形态**——契约 3.3 那条防的是"Redis 抢 CPU 污染 p99"，绑 Redis 就够；把网关和 wrk 也钉到核上会让新读数和 B-008/B-016 的历史档**不可比**（取样规程的规矩四是这么写的）。绑核方案、`--cpuset-cpus` 的实际生效证据与读数一起进 `BENCHMARK.md`，Docker Desktop 虚拟机自己的 CPU 数与发行版的 16 是否同一件事，取证时一并核。

### 八、验收形状（进 TASKS 的就是这几句）

- **连通取证**：`l7gate_redis_probe` 与 `redis:7.2.5` 真跑通（connect → PING 读回 `PONG` → EVAL 令牌桶脚本 → HGETALL 读回桶状态 → DEL 清场），客户端 1.2.0 ↔ 服务端 7.2.5 逐条打印，进 ENVIRONMENT 第 3 节与 BENCHMARK。
- **数字一致（同一个键比）**：`rate=1/s`、`burst=3`、`settle=200ms`，同 path 六连击 → 启用分布式与不启用**两侧都恰好 `200 200 200 429 429 429`**、`rate_limit_allowed/rejected` 与 S4-03 的读数逐条相等、`rate_limit_capped_rejects=0`。
- **夹子真的咬人**（阳性验证的形状）：把 Redis 里那只桶预先掏空 → 同一批六连击必须**少放**且 `rate_limit_capped_rejects>0`；这一条同时是"分布式不是装饰"的断言。若它和 `capped_rejects=0` 那条一起红，才说明夹子是活的。
- **降级三段**：`docker stop` 后 429 仍能出（本地桶顶住）、`redis_degraded=1`；容器重开后 `redis_degraded=0`、`redis_settles_ok` 继续增长、夹子重新生效——全程 `requests_parsed` 不掺结算线程的命令数（仪器不进被测方，与探活同一条立场）。
- **成本对照**：启用分布式后每请求增量 = CAS 循环里多一次 `min`（同一原子字），结算线程的 Redis RTT 单独报；对照 B-008/B-016 两把尺，抬出带就照记。
- **门禁面**：三 preset 各全绿（用例数 = 280 + 本任务新增）、非 gtest 警告 0 条、`tsan` 零 data race、六冒烟 `rc=0` 且 ✗=0。

### 九、不做（禁令一挡住顺手做）

AUTH 与 TLS（Redis 只经 `127.0.0.1` 端口发布可达，契约 1.3 不做 TLS 是同一条立场；要接进真环境必须先补 ADR）；多网关实例的一致性实测（TASKS「明确不在 S4 做」）；Redis 侧键的 LRU 回收（与 ADR-21 第四节"本地不回收"同一条决定）；`CLUSTER`/多分片；fail-closed（Redis 挂就拒全站——正文第三节明确拒绝的那个默认行为）。

## 落地追加（2026-11-02，S4-04 实现与冒烟时）：取证已到位、四处阳性验证、五条新踩的坑

### 一、连通取证完成（第一节那句"不算既成事实"的前提撤销）

`l7gate_redis_probe` 七步全通，`client=hiredis-1.2.0 ↔ server=redis 7.2.5`，`probe_rc=0`：
connect → `PING`→`PONG` → `INFO server`=`7.2.5` → `SCRIPT LOAD`（产品那份脚本，**678 字节**，
sha `27f391e42f3e1edfb9a3636a3f99e8c6c4239b1c`）→ 四格确定性 `EVALSHA` 读数
（`fresh_full 3000 / after_burst 0 / refill_1s 1000 / clamp_burst 3000`，逐格 ✓，且与本地
`TokenBucket` 在同一组参数下对账：满桶连取 4 次放 3 拒 1）→ `HGETALL` 读回
`tokens`/`last` → `DEL` 清场。键口径也上了线：探针打的那格是 `l7gate:rl:v1:path:cb5a1f0453dfc04a`。
镜像 `redis:7.2.5`，digest `sha256:3aaec283e6e5…`；**身份核验有一条与 B-005 同形的缺口**：
本机直连 `registry-1.docker.io` 建不起连接，我无法把这个 digest 与官方仓库比对，它来自
ENVIRONMENT 5.1 那个 mirror（照记，不假装核验过）。

绑核生效是可断言的，不是写了个参数：`HostConfig.CpusetCpus=15` 且容器内
`/sys/fs/cgroup/cpuset.cpus.effective=15`，`docker info` 的引擎侧 `NCPU=16` 与发行版
`nproc`=16 同一视野（Docker Desktop 的虚拟机就是这台 WSL2 内核，`INFO` 里
`os=Linux 6.18.40.1-microsoft-standard-WSL2`）。**这条不是完整隔离**，写清边界：网关与压测
按第七节的取舍保持未绑核，所以它们仍可被调度到 15 号核上——我绑的是"Redis 不许到处跑"，
不是"Redis 与网关各占一半机器"。

### 二、验收读数（同一次 `scripts/smoke-s4-04.sh`，rc=0、✗=0）

- **数字一致**（`rate=1 burst=3`、同一个 path `/same`）：不接 Redis `200 200 200 429 429 429`
  / 接 Redis 逐条相同；两臂退出账同为 `allowed/rejected=3/3`；`rate_limit_capped_rejects=0`；
  `redis_settles_ok=3` 而 `redis_settle_commands=1`（六连击只花一条上报命令，16384 个冷键
  不该换来 16384 次 EVALSHA 的那条证据）；服务端那格 `tokens=0 < 3000` —— 消耗记在Redis 账上。
- **夹子真的咬人**（`rate=1000 burst=1000`）：宽裕时 5 条全放行；把那一格冻成
  `tokens=0 / last=999999999999` 之后，**第二批** 5 条全 `429`、`rate_limit_capped_rejects=5`、
  `redis_settle_commands=4`；解冻后又 5 条全放行。这一格同时是"分布式不是装饰"的断言——
  本地桶在 1000 的额度下绝不可能空。
- **降级三段**：`docker stop` 后同一台网关继续按配置阈值表态（六连击仍是 `3×200 + 3×429`，
  拒绝能力没消失，也没有一秒钟的 5xx），`redis_settles_failed=7`（挂着期间每拍有账，不是
  静默降级）；`docker start` 之后 **不需要重启网关**，`redis_degraded` 自己回到 0、
  `redis_settles_ok=9`；恢复后再冻一次那一格 → 5 条全 `429`，聚合通路是活的。
- **生命周期**：`live_connections=0`、`connections_constructed==destructed`、`fd 9→9`
  ——结算线程那条 hiredis 连接没漏 fd（这条如果漏，多出来的正是那个 fd）。
- **进程侧**：`--redis-db` 没配 `rate_limit` 段 → rc=2 且话里同时点名两个名字；
  `--redis-settle-ms 50` → rc=2；`--redis-db 127.0.0.1:notaport` → rc=2；参数域内的合法组合
  （settle=100 timeout=2000）照常启动。

### 三、阳性验证四处（能红才叫绿；还原后 `grep -r TEMP-POSITIVE src/ tests/ tools/` = 0）

| 改坏的那一处 | 红掉的用例 |
|---|---|
| 夹子不生效（`usable = tokens`） | 4 条：`CapLensRejectsButDoesNotSpendTheBucket`、`OverflowBucketAndReleaseAreProcessWide`、`GrantOfZeroBitesAndTheRejectionItCausesRefreshesIt`、`FailureOpensTheCapAndRecoversReclampingIt` |
| **先记账后发命令**（失败时把这笔消耗丢掉） | 恰好 1 条：`FailedReportIsRetriedWithTheSameUsageNextTick`——隔离干净，说明这条断言只守这一件事 |
| 取消"只结有需求的桶"（每拍全表 EVALSHA） | 3 条：`IdleBucketsCostNoCommands`、`DemandedBucketIsSettled…`、`FailedReportIsRetried…` |
| `set_cap_milli` 不夹 `burst` | 3 条：`GrantAboveBurstCannotAdmitMore`、`DemandedBucketIsSettled…`、`NoscriptReloadsTheScriptAndAsksOnceMore` |

### 四、本轮新踩的坑（每一条都留下了"防复发的第三条"）

1. **`$(start_gateway …)` 起的进程不是本 shell 的孩子，`wait "$pid"` 当场返回"not a child"**。
   后果不是报错而是**一路假绿**：脚本在它还没退出时就去读退出账（读回空串）、再拿同一个端口
   起下一臂（`EADDRINUSE`），于是后面每一臂测的都是第一臂那个还活着的进程。防复发：
   `stop_pid` 改成轮询 `kill -0` 直到真退出，且**每次启动前断言端口是空的**（`port_busy … && die`）。
2. **一行多键 + 行首锚 = 四格静默读空**（第六节那条）。防复发：退出账一行一键 + `get_stat`
   改成 `grep -o "[[:space:]]key="`（不再假设键在行首）。
3. **`cd X && a` 之后的换行不继承 `cd`**：我那条手测命令的第二行在原 CWD 里找
   `build/debug/l7gate`，报"No such file or directory"。防复发：复杂/带 `$` 的命令一律写成
   `/home/archer/*.sh` 再跑（交接段那条口径的同族，本轮又应验一次）。
4. **脚本里不要硬编码被测方算出来的键名**：`bucket_keys` 扫出来再冻；scan 顺序不保证，所以
   冻之前先 `FLUSHDB` 让账上只剩一格。
5. **授权按拍更新 → 冻格后的第一批仍可能放行**，而那批放行正是制造下一次上报的需求。
   判据打在第二批上——这不是脚本在等运气，这就是第八节"跨窗偏差上界"的实测形状。
6. 夹具侧一条：Redis 的命令名大小写不敏感，产品发的是 `SCRIPT LOAD`——子命令按字面小写比
   会把这条回成 `-ERR unknown command`，被测方看到的是"服务端拒绝"而不是"我解析错了"。
   同理，NOSCRIPT 这类一次性故障必须**只对目标命令生效**，否则会被同拍里的 PING 消化掉。
