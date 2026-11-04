# ADR-27 · 一键起全套的形状：静态 IPAM 而不是服务名、运行时壳镜像而不是把产物 COPY 进去

## 这一节在裁什么

S5-05 要把「网关 + Redis + MySQL + Prometheus + Grafana + 两个假上游」收成一条 `docker compose up -d`。
拆解那句「它没有 ADR 前置」覆盖的是**镜像不换 tag**，它没覆盖我在这条任务里先测出来的三件事：

1. **网关这一侧认不了服务名**——它的地址解析是 `inet_pton(AF_INET)`（[src/net/socket.cc:16-35](file://wsl.localhost/Ubuntu/home/archer/workspace/l7gate/src/net/socket.cc#L16)），
   配置里的 `address` 只能是 IPv4 字面量。这一条直接决定 compose 里网关到上游那一段只能怎么写。
2. **要让网关进容器，就得有一个能承载它的镜像**——契约 9.1 锁的五个镜像**一个都承载不了**（实测下面第一节）。
   这就是禁令三说的那件事：不是换 tag，是**加镜像**。所以这一份 ADR 存在。
3. **抓取通路要不要跟着编排改**——答案是不要，而且这是个实测结论：容器经 `host.docker.internal`
   能打到**别的容器的发布端口**，而 `deploy/prometheus/prometheus.yml` 与 `deploy/grafana/**` 写的正是那个形状。
   于是入库那两份的 **target 值一个字不动**（那两份顶上关于 `extra_hosts` 的注释按退化 C4 的实测更正过，见第三节末），
   S5-03 那条冒烟测的还是它自己那一份。

## 一、实测（2026-11-04 本会话，四条，全部带读数）

仪器落在 `/home/archer/s5-05-probe/`（临时目录，不进仓库；探针脚本自己 `down` 清场，`docker ps -a --filter name=l7gate-` 收尾为空）。

| # | 测什么 | 读数 | 由此定下的形状 |
|---|---|---|---|
| P-1 | **容器之间能否按服务名解析**（S5-03 留下的那笔账，本轮第一次测） | compose 用户自定义网络 `l7gate-probe_default`，容器内 `/etc/resolv.conf` 是 `nameserver 127.0.0.11`，`docker inspect` 的 `Aliases` 就是服务名；prom 容器 `wget http://gateway-stub/` **第 0 次尝试即 `200 OK`**（`Server: nginx/1.25.4`、`Content-Length: 615`），60 秒那道门没用上。同网络按容器 IP 也 200 | 服务名解析**通**。但**只有自己做名字解析的那一方用得上**（Prometheus、Grafana），网关用不上——见 P-2 |
| P-2 | **网关能不能把 `address` 写成主机名** | 同一台上游、同一份配置，只把 `address` 从 `127.0.0.1` 换成 `localhost`：`HTTP 502`、退出账 `upstream_connect_failed=1`、`gateway_replies_connect_failed=1`、`responses_completed=0`；对照臂 `address=127.0.0.1` 是 `HTTP 200` + `seq=1` 的正文 | 网关→上游/Redis 那一段**只能写 IPv4 字面量** → compose 用**显式静态 IPAM**，把地址写成文件里的事实（验收句③ 要的就是这个） |
| P-3 | **静态 IPAM 在这台 Docker Desktop 上生效吗** | 子网 `172.31.250.0/24`、`ipv4_address: 172.31.250.20` 起容器后，同网络另一台容器 `wget http://172.31.250.20/` 拿到 `200 OK` | 生效。编排里所有会被**网关**引用的地址都走这一条 |
| P-4 | **容器经 `host.docker.internal` 能不能打到别的容器的发布端口** | `stub-a` 发布 `127.0.0.1:18777`（宿主侧 curl 同一端口 `200` 先钉住）；prom 容器 `wget http://host.docker.internal:18777/` → **`200 OK`**。对照：`wget http://172.31.250.1:18777/` → `Connection refused`（静态网段的网关地址不是那条代理通路） | 入库的 `host.docker.internal:18522/18090/19090` 在容器拓扑里**同样成立** → 那两份配置的 **target 值不改**（注释按 C4 更正，见第三节末），S5-03 不受涟漪 |

另外四条与本任务直接相关的环境读数：

- **契约锁的五个镜像没有一个承载得了我们的二进制**：网关实际需要的符号上界是 `GLIBC_2.38` + `GLIBCXX_3.4.31`；
  `redis:7.2.5` 与 `nginx:1.25.4` 是 Debian 12（glibc **2.36**）、`mysql:8.0.36` 是 glibc **2.28**、
  `prom/prometheus` 是 busybox、`grafana/grafana` 是 musl。把宿主的库挂进 `redis:7.2.5` 想绕过这一条，
  当场 `sh: symbol lookup error: /hostlib/libc.so.6: undefined symbol: __tunable_is_initialized, version GLIBC_PRIVATE`
  （**挂一半的 glibc 比不挂更糟**，这条要留着，因为它是这个方案最容易被想到的岔路）。
- **`ubuntu:26.04` 拉得到，而且与宿主同代**：`docker pull` rc=0，digest `sha256:6a06111c3b0a8600feadb270aec771dc239bf407b25625f93f44ab00e11f4d3c`，
  `ldd --version` = `Ubuntu GLIBC 2.43-2ubuntu2.4`——与宿主**逐字同一版本**。判定镜像在位一律以 `docker image inspect` 为准，不以拉取日志为准（§22.2 第 1 条同族）。
- **它的 apt 里那两个库与契约 9.1 字面命中**：`libmysqlclient24` 候选 `8.4.11-0ubuntu0.26.04.1`（9.1 锁「libmysqlclient 8.4.x，实测候选 8.4.11」）、
  `libhiredis1.1.0` 候选 `1.2.0-6ubuntu4`（9.1 锁 hiredis v1.2.0，apt 那一行就是它）。所以运行时镜像里装的是**契约已经点名的版本**，不是新增第三方依赖。
- **我们的二进制在容器里真能起**：只挂产物、不挂宿主库时缺 `libmysqlclient.so.24`（`error while loading shared libraries`，符合预期）；
  把上面那两个库装进镜像后，`--serve --listen 0.0.0.0:18080 --metrics-port 18522` 起来并自抓回
  `HTTP/1.1 200 OK` + `Content-Type: text/plain; version=0.0.4` + `# HELP elapsed_ms`（这一步我先用 `ubuntu:24.04` 挂宿主库验过通路，再用 26.04 验的正形状）。

## 二、我选择什么（六件）

1. **八个服务**：`gateway`、`upstream-a`、`upstream-b`、`bench-exporter`、`redis`、`mysql`、`prometheus`、`grafana`。
   其中 `bench-exporter` 是 **ADR-25 第六节点名的第四个独立服务**：它有自己的容器、自己的进程地址空间，
   只读挂载 `build/bench`；**不进网关容器、不共享网关内存、不因为编排而改成读 `/proc` 或读 `ReactorStats`**。
   「同一个镜像、两个容器」不违反这一条——那条规矩守的是内存与计数，不是 inode。
2. **五个基础设施服务用契约 9.1 锁的 tag**（`redis:7.2.5`、`mysql:8.0.36`、`prom/prometheus:v2.51.2`、`grafana/grafana:10.4.2`），
   `nginx:1.25.4` 不进这个栈（它是 S5-07 的对照基线，混进演示栈只会让「谁在被测」变模糊）；
   **我们自己产物那台**是一个本地构建的运行时壳镜像 `l7gate/runtime:s5-05`。tag 里没有一条 `latest`。
3. **`l7gate/runtime:s5-05` 是壳，不是承载物**：`FROM ubuntu:26.04` + `apt-get install libmysqlclient24 libhiredis1.1.0`，
   **三个二进制都不 COPY 进镜像**，运行期以只读 bind mount 挂进 `/app`。理由不是省事，是**只留一份真相**：
   契约 9.4 要求产物在 ext4 里构建，把产物烤进镜像就会同时存在「镜像里那一份」和「盘上那一份」，
   而面板与冒烟读的必须是**你刚构建的那一个字节**（S5-03 为「测的不是入库那一份」这条连锁红过一轮，§22.2 第 1 条）。
   连带后果写进 README：**先构建，再起栈**；产物不在时冒烟在阶段 0 拒跑。
4. **地址形状**：`gateway` 的配置里上游与 Redis 都写**静态 IPv4**（`172.31.250.21/22/41`），子网 `172.31.250.0/24` 由 compose 自己声明；
   Prometheus 与 Grafana 继续用 `host.docker.internal` + 发布端口（P-4），所以 `deploy/prometheus/prometheus.yml` 与 `deploy/grafana/**` **一字不动**。
   这一条不是偏好，是 P-2 逼出来的：给网关写服务名 = 它连不上 = 演示栈看起来起来了而路由全 502。
5. **Redis 的 CPU 分配按 B-017 复述，只绑 Redis**：`cpuset: "15"` 只给 redis 那一台，**网关与工具不绑**。
   B-017 那句话要原样带过来：契约要的是「Redis 不许抢走网关正在用的核」，不是「Redis 与网关各占一半机器」；
   网关仍可被调度到 15 号核上，那条竞争没被消除，只是不再扩散。
   另加一条事实校验：`docker info` 的 `NCPU` 必须是 16，否则「15 号核」这个写法本身就是假事实（冒烟阶段 0 断它）。
6. **容器这一栈只服务演示面，不服务判定**：三个数字的读数继续只认 `scripts/bench/` 的落盘摘要与退出账。
   网关进容器就多一次 DNAT/userland proxy 进数据路径——这正是 B-005 那条「赢的是对照组」批评的根据本身，
   所以这里明写：**演示面可以，判定档不许**；要拿容器拓扑与 nginx 比（已裁第 ⑨ 条第 4 项那一臂），归 S5-07，
   且必须按同一形态把**两侧**都放进容器。

## 三、边界

- **`src/` 与 `tests/` 一字不动**。本任务全部落在 `deploy/`、`scripts/`、文档。P-2 那条「网关认不了主机名」不是缺陷，
  也不许顺手去修：它守的是「地址是部署事实、写成字面量就没有第二份真相」，改成走 resolver 等于把 DNS 拉进数据路径。
- **ADR-10 追加节的定义域不许被编排越过**：这一栈**不 arm 文件重载**（`--config-reload-ms` 缺席），
  限流参数改了不生效那句照 README 原样有效；编排里没有一条「reload 会重读命令行来源」的暗示。
- **演示页那条路不进 compose**。S5-04 已经写明 `--ws-echo` / `--html` 那台上游是**仪器与假上游**，
  别在编排里当生产后端；它只实测了「上游 `--html` + 页面经网关同源」这一条通路，与本栈的静态 IPAM 形状不是一回事。
- **MySQL 起而不用**，这一格是明写的「没做」，不是遗漏：把它接成路由来源要 `--nodes-db` + `source: mysql` 那一组，
  而 ADR-10 追加节第七条规定「首装含 mysql 组的网关不许 arm 文件重载」——接进来的代价是**当场取消这台网关的热重载演示面**，
  那是定义域的收缩，不是编排的自由度。要不要付这个代价归项目所有人裁，不在本任务顺手做。
- **清理只按 `l7gate-` 前缀**：compose 的 project name 就叫 `l7gate`，容器名一律 `l7gate-<service>-1`；
  脚本里**不出现任何全量 `docker rm`/`docker kill`/`docker system prune`**。同机 `rgbt-*`、`knowstack-*`、`tinrpc-*` 与本项目无关。
- **「地址必须写 IPv4 字面量」这一条不是编排层的偏好，它有一个暴露面的连带后果，本轮实测抓到、不在本任务修**：
  配置校验只挡空串、不挡字符集，而 `health_t<i>_<addr>_<port>_<metric>` 那一族把 `address` **原样拼进指标名**，
  ADR-24 的机械变换只处理 `.` 与 `:`。于是带连字符的地址（任何主机名）会造出 Prometheus 名字法里非法的指标名，
  后果不是那一格坏，是**整份 `/metrics` 抓失败**（退化 C1 的实测 `lastError = invalid metric type "a_18081_ejections counter"`，
  网关整体 down、101 个键一起丢）。修法三条摆在 TASKS「已知待决事项」第一条（前移到校验层 / 扩大机械变换 / 维持现状加限制），
  其中两条动 `src/`、一条动键名口径 → 归项目所有人裁。**本任务因此给编排加了一条自我约束**：
  `deploy/gateway-compose.json` 里的地址必须与 compose 里的静态 IP 逐字相同，这一致性别处有断言守着（冒烟阶段 1）。
- **`extra_hosts` 那句"不带上就变假目标"被本轮实测否掉了，已就地更正**（退化 C4）：删掉它两条 target 仍然全 up，
  容器里 `/etc/hosts` 没有那一行而名字照样解析到 `192.168.65.254`（Docker Desktop 的内置 DNS 会给）。
  留着它的理由换成**不押在 daemon 的自动行为上**；`deploy/prometheus/prometheus.yml` 顶上那段与 compose 里那台的注释
  都已按实测改写（**只改注释，target 的值一个字母没动**，所以 S5-03 测的仍是同一份——复跑 `PASS checks=40 fails=0`）。

## 四、代价（明码，不是「应该不贵」）

1. **多了一个 base 镜像**：`ubuntu:26.04`（本机新增，digest 见第一节）+ 一个 apt 层。它不在契约 9.1 的表里，
   按禁令三就是这份 ADR 存在的理由。它的版本不是我选的，是**与宿主构建环境逐字相同**那一个（glibc 2.43-2ubuntu2.4）。
2. **compose 不能脱离构建目录跑**：产物是挂进去的，不是打包进去的。README 的起法第一行因此是构建命令，不是 `docker compose up`。
3. **静态网段是全局资源**：`172.31.250.0/24` 实测可用；如果哪天与别的 bridge 冲突，`up` 直接失败——
   脚本阶段 0 因此先断「这个栈起得来」，而不是在红了一串之后去找原因。
4. **多一次 DNAT 进数据路径**：见第二节第 6 条。这一条在演示面上读不出来（它不进任何判定），
   但它是「不许拿这一栈发达标证明」的全部理由，所以要在 README 与面板说明里出现，不能只留在 ADR。
5. **apt 装库依赖容器出网**：`docker build` 时要能到 `archive.ubuntu.com`（实测 `apt-cache policy` 出候选，
   而 `docker manifest inspect` 直连 `registry-1.docker.io` 是超时的——两条路不同，别把后者当成前者不成立的证据）。
6. **`redis_settles_ok` 那一格把 Redis 变成活的依赖**：演示网关确实带 `--redis-db`，Redis 起得慢会先走降级三段
   （S4-04 的既有行为），所以冒烟断的是「**确实发生过** settle」这个下界，不是「settle 一直成功」这个上界。

## 五、验收句 → 断言落点（`scripts/smoke-s5-05.sh`，阶段号是实现之后的实际落点，不是规划时的编号）

脚本十一个阶段（0–10）、**66 项判定**，判定同时写进 `build/smoke-s5-05/verdict.txt`（只认落盘，见 §22.2 第 5 条）。

| S5-05 验收句 | 落点 |
|---|---|
| ① 一条 `up -d` 之后：网关健康、两个上游被路由到、targets up、面板出数 | **阶段 2** 八台全 healthy + 网关那台的**健康判据是"观测面答得出 `# HELP`"**而不是"端口 accept 了"（`healthcheck-gateway.sh` 顶上写了为什么）；**阶段 3** 宿主侧业务端口 12 条全拿到带 `seq=` 的正文 + `requests_parsed` 增量恰好 12 + `upstream_connect_failed` 增量 0；**阶段 8** 两台上游**各自**自报接走过（`6 + 6 == 12`，与网关那一格同轴）；**阶段 5** Prometheus 两条 target 全 up 且 `scrapeUrl` 就是入库那两个端口 + exporter 那一格与盘上**逐数相等**；**阶段 6** 面板经 provisioning 入库 + 从 JSON 取出的 20 条表达式逐格非空（六格 `1/3/2/4/4/5`） |
| ② 全部镜像走契约锁定 tag，`latest` 一条不许出现 | **阶段 1** 从 compose 文件里取 `image:` 行：四条锁定 tag 逐字命中 + 自产壳镜像一条 + **`latest` 出现 0 次**（判的是**文件**，不是 `docker images` 的输出——后者会把同机别人的 `latest` 也算进来，那就不是这一栈的事实） |
| ③ 端口发布拓扑写成事实而不是假设 | **阶段 1** 断文件：没有 `network_mode`、`cpuset` 只有一条且落在 redis 段、6 条 `ipv4_address` + 1 条显式子网、`extra_hosts` 两处、**网关配置里的两个地址 == 编排里钉死的两个静态 IP**、入库那两份的 target 值字面未动；**阶段 4** 是这一句的正身：对照容器在同一张网络里把 `address` 写成服务名，**两条请求都拿到 502** 且它自己的观测面自报 `upstream_connect_failed=2`——P-2 那条实测从此是一条会红的断言 |
| ④ 停栈后没有残留，且绝不碰同机别人的容器 | **阶段 10** `down` 后 `docker ps -a --filter name=l7gate-` 计数 0、`docker network ls --filter name=l7gate-` 计数 0；同阶段另一条读 `rgbt-*`/`knowstack-*`/`tinrpc-*` 的条数与**阶段 0 的开工前快照相等**（碰了别人就红）。对照容器（阶段 4 那一台）按名清掉并单独判一条 |
| ⑤ compose 里的 Redis 按 B-017 复述 CPU 分配（只绑 Redis） | **阶段 0** 断 `docker info` 的 `NCPU=16`（不成立则"15 号核"这个写法本身就是假事实）；**阶段 1** 断文件里 `cpuset` 只有一条且在 redis 段；**阶段 9** 两处生效证据（`HostConfig.CpusetCpus=15` **且** 容器内 `cpuset.cpus.effective=15`，同 B-017）+ **网关那台读回空**；另加两格 redis 自身的持久化事实（`save` 值为空、`appendonly=no`，阶段 2） |
| 三条限定语（沿用，不许在编排里丢） | **阶段 7** 逐条 grep：面板里 `不能读成「没有坏后端」`、`只覆盖首装那批成员`、`含热重载的预留槽位容量` 三句 + README 里 `只服务演示面`、`先构建` 两句（少一句红一条）。这一栈**不 arm 文件重载**，所以阶段 10 还断一条：退出账里一个 `config_` 键都不发（ADR-10 那条关闭态口径原样成立） |

## 六、阳性验证清单（能红才叫绿，实现那轮逐条跑红再还原）

| # | 退化 | 打在哪 | 不许变成什么 |
|---|---|---|---|
| C1 | 把 `upstream-a` 的地址从静态 IP 换成服务名 `upstream-a` | compose + 配置这一处（所有写者必经的那一处） | 「容器间 DNS 反正通」——它通，但**网关不做名字解析**（P-2 实测 502）。这一条红的是阶段 4 的正身 |
| C2 | 把某个镜像 tag 换成 `latest` | compose 的 `image:` 行 | 判定读的是**文件**，不是 `docker images` 输出 |
| C3 | 给 `gateway` 也加 `cpuset`（把「只绑 Redis」写成「都绑」） | compose 的 redis/gateway 两处 | B-017 那句复述与事实分家 |
| C4 | 删掉 `extra_hosts` | compose 的 prometheus/grafana 两处 | 「targets up」读不到宿主发布端口 → 第 5 阶段红，而不是静默换第二条通路 |
| C5 | 让收尾 `down` 故意漏一台 | 清理路径 | 残留判据（句④）要能数出那一条，而不是"我记得我清了" |

**执行结果照记（2026-11-04 实现那一轮）**：五处全部真打红，红数 **9 / 2 / 2 / 1 / 3**，
名单、每处红在哪几格、C1 里那五条级联红的全部归因、以及本轮我自己那六处仪器侧错（其中一处会造出**假绿**）
都在 FAULT-INJECTION **§24**。两处必须点名给读这份 ADR 的人：**C4 的机制预测错了**（第二节第 4 条与第三节那条更正）；
**C1 顺手抓到一条被测方的暴露面缺陷**（第三节第二条，登记在 TASKS「已知待决事项」，本任务不动 `src/`）。

## 七、这一节没做什么

- 没做 K8s/Helm、没做多网关副本、没做 TLS（都在 S5「明确不做」那张单子上）。
- 没把演示页/`--ws-echo` 那条路搬进编排。
- 没接 `--nodes-db`（第三节的理由），也没给网关加任何新键。
- 没有量容器拓扑下的任何 SLO 数字；S5-07 那一臂要量的时候，按本节第二节第 6 条的口径重新立。
