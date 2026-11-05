# ENVIRONMENT —— 环境自检实测记录

> 这份文件是第 2 节三个 SLO 数字的**坐标轴**。数字离开环境就没有意义：`100k 连接每连接 ≤ 20 KB` 直接由 malloc 实现决定，`p99 ≤ 1.0 ms` 直接由内核与 CPU 代数决定。所以我把它入库，并且要求每次压测报告都引用这里的版本号，而不是另写一套。
>
> 本文档刻意**不包含**机器私有标识（用户名、真实绝对路径、机器名、IDE 配置）。凡是复现所必需的规格（内核、CPU 数、内存上限、组件版本、分配器）一律保留——去掉这些就等于把证据链的坐标轴擦掉，别人复现不出我的 p99 时无从判断差异来自哪里。

## 1. 一键自检（复跑入口）

任何新会话开工前先跑这个，输出与下表不符就是环境漂移，先修环境再动代码。

```bash
# 在 WSL 内执行，不要在 Windows 原生 shell 里跑
lsb_release -ds            # 发行版
uname -r                   # 内核，契约要求 ≥ 5.15
g++ --version | head -1    # 契约要求 ≥ 11
cmake --version | head -1  # 契约要求 ≥ 3.22
clang++ --version | head -1  # 契约要求 ≥ 14，TSan 唯一自动化验证手段
wrk --version | head -2    # HTTP 压测
dpkg-query -W hey          # hey 没有 --version 选项，实测只能从 dpkg 取版本
clang++ -fsanitize=thread -g -o /tmp/_rt /dev/null 2>&1 | tail -1  # 冒烟：sanitizer 运行时是否真能链接
docker --version           # Redis/MySQL/Prometheus/Grafana/Nginx 基线的载体
nproc                      # 必须 = 16，否则 SLO #3 的测量条件不成立
free -g | head -2          # 内存上限，写进每次压测报告
ldd --version | head -1    # glibc —— 决定每连接内存的那个 malloc
perf --version             # S5 尾延迟归因依赖
git config user.name; git config user.email   # 提交前置检查
```

## 2. 实测结果（首次自检：2026-10-26）

| 组件 | 契约锁定 | 实测 | 判定 |
|---|---|---|---|
| 发行版 | Ubuntu 26.04 LTS | Ubuntu 26.04 LTS | ✅ |
| 内核 | ≥ 5.15 | 6.18.40.1-microsoft-standard-WSL2 | ✅ |
| GCC | ≥ 11 | g++ 15.2.0 | ✅ |
| CMake | ≥ 3.22 | 4.2.3 | ✅ |
| Clang | ≥ 14 | clang 21.1.8（dpkg `1:21.1.6-71`）+ `libclang-rt-21-dev` `1:21.1.8-6ubuntu1`；三个 sanitizer 已用阳性样本验证 | ✅（符号化缺 `llvm-21-tools`，见 2.2） |
| wrk | apt 最新版 | 4.1.0（`4.1.0-4build3`），epoll 模式 | ✅ |
| hey | apt/官方 release | 0.1.4（`0.1.4+ds-2`）；**它没有 `--version`**，版本要从 dpkg 取 | ✅ |
| hiredis | v1.2.0 | `1.2.0-6ubuntu4` | ✅ **字面命中契约** |
| libmysqlclient | 8.4.x | `8.4.11-0ubuntu0.26.04.1`，`mysql_config --version` = 8.4.11 | ⚠️ 唯一实质偏离，见第 3 节 |
| GoogleTest | v1.14.0 | apt 仅提供 1.17.0 → 改用 FetchContent 锁 v1.14.0 | ✅ 不改契约 |
| prometheus-cpp | v1.1.0 | apt 无源，需 FetchContent 源码构建 | ⏸ S5 才用到 |
| glibc | 未锁 | 2.43（Ubuntu GLIBC 2.43-2ubuntu2.4） | 分配器默认 glibc malloc，SLO #2 必须记录 |
| OpenSSL | 未锁 | 3.5.5 | 仅作传递依赖，本项目不做 TLS |
| perf | 未锁 | 7.0.14 | ⚠️ 待验证，见第 4 节 |
| vCPU | SLO #3 写死 16 | 16 | ✅ |
| 内存 | 未锁 | ≈11 GB（WSL 分配上限） | ✅ 100k 长连接目标够用 |
| Docker CLI | 必需 | **可用（2026-10-26 23:40 复查恢复）**：`docker --version` = 29.4.3。这一项当天掉过两次线（23:3x 复查时本发行版内找不到 `docker`，用户重开集成后恢复），**恢复后的第一次重跑就是 #3 基线第二轮**（B-005 附，`build/bench/s2-nginx-20261026-234014/`，五档跑通、比值 203.74%）。中间那次断线造成一轮作废读数（档 2 的 3,148.41 是引擎正在退出时取到的），已按作废记在 BENCHMARK 那条括号里、不进任何表。 | ✅ 影响 **#3 基线的可复现性**——集成的稳定性是 S5 终判的一项前提；修法在 Docker Desktop 图形界面侧，属共享环境，我不自行改配置 |
| GitHub SSH | 必需 | 认证成功（`ssh -T git@github.com` 返回 `Hi <user>!`） | ✅ |
| git | 必需 | 2.53.0，`user.name` / `user.email` 已配置 | ✅ |

### 2.1 二次自检（同日晚，wrk / hey / clang 装齐后）

`clang`、`wrk`、`hey`、`libhiredis-dev`、`libmysqlclient-dev` 已由用户在 WSL 终端 apt 安装完成。但**契约里的「必装 Clang」到这一步还没真正满足**：Clang 的 sanitizer 运行时库文件不存在。

```
/usr/bin/x86_64-linux-gnu-ld.bfd: cannot find
  /usr/lib/llvm-21/lib/clang/21/lib/x86_64-pc-linux-gnu/libclang_rt.tsan.a
clang++: error: linker command failed with exit code 1
```

三个 sanitizer 逐个实测，**全部链接失败**：

| 命令 | 结果 |
|---|---|
| `clang++ -fsanitize=address -g` | ❌ 链接失败 |
| `clang++ -fsanitize=undefined -g` | ❌ 链接失败 |
| `clang++ -fsanitize=thread -g` | ❌ 链接失败 |

根因：Ubuntu 的 `clang` 元包不带 `compiler-rt`，要单独安装 **`libclang-rt-21-dev`**（apt 候选 `1:21.1.8-6ubuntu1`，实测确认当前未安装）。

作为对照，我把同一组 sanitizer 在 **g++ 15.2.0** 上各编跑一遍，**三个全部构建并运行成功**（address / undefined / thread）。这个事实只用来证明「不是 WSL2 内核或库栈的问题，就是缺一个包」。**我不拿 g++ 的结果冒充契约要求的 Clang 验证**——契约 9.1 指定 Clang 是连接对象生命周期的唯一自动化验证手段，S2 门禁的「TSan 干净」指的就是这套工具链。g++ 旁证保留，正式结论等 `libclang-rt-21-dev` 装上后重跑得出。

**当时的结论：开工前置条件未满足。** S1 门禁要求「ASan/UBSan 干净」、S2 门禁要求「TSan 干净」，两者在当时机器上都无法执行，所以 S1 代码没有启动。后续见第 2.2 节。

### 2.2 三次自检：补齐 compiler-rt 后，用阳性样本证明 sanitizer 真的在检测

安装 `libclang-rt-21-dev`（实测装到 `1:21.1.8-6ubuntu1`）后，三个 sanitizer 全部链接并运行成功。但**「能编译能跑」不是验证**——一个静默失效的 sanitizer 比没有更危险，它会让「ASan 干净」这句话变成假的。所以我用**故意写坏的程序**做阳性测试，逐个确认它们真的会报：

| 检测项 | 阳性样本 | 实测结果 |
|---|---|---|
| ASan | `delete p; *p = 42;` | ✅ `ERROR: AddressSanitizer: heap-use-after-free ... WRITE of size 4` |
| LeakSanitizer（ASan 内置） | `for(i<5) new int(i);` 指针丢弃 | ✅ `detected memory leaks` / `SUMMARY: 20 byte(s) leaked in 5 allocation(s)` |
| UBSan | `INT_MAX + 1` | ✅ `runtime error: signed integer overflow: 2147483647 + 1 cannot be represented in type 'int'` |
| TSan | 两个线程各自 `g++` 十万次 | ✅ `WARNING: ThreadSanitizer: data race (pid=…)` |

**两个必须记下来的踩坑（不然日后会误判 sanitizer 失灵）：**

1. **`-O1` 会把「可被证明无副作用的坏写入」优化掉。** 同一个 UAF 样本，`-O1` 下 ASan 什么都不报（写入被 dead store elimination 删除，程序正常退出），`-O0` 下立刻报出 heap-use-after-free。**做 sanitizer 阳性验证必须 `-O0 -g`。** 这条也反向提醒我：将来跑 ASan 干净检查时用的构建类型，不能拿一个开了激进优化的构建来充当证据。
2. **漏检的块如果仍被全局指针指着，属于 "still reachable"，LSan 默认不报。** 我第一次的泄漏样本用了全局链表头，结果是干净的——那不是程序没问题，是测试写错了。悬空分配（指针丢失）才会被抓到。

**仍然存在的工具链缺口（不阻塞开工，但必须记账）：**

ASan 报告里的堆栈**无法符号化**，输出是裸地址并伴随警告：

```
==11220==WARNING: invalid path to external symbolizer!
==11220==WARNING: Failed to use and restart external symbolizer!
SUMMARY: AddressSanitizer: heap-use-after-free (/tmp/sym+0x118071) (BuildId: 6632637d…)
```

根因是缺 `llvm-symbolizer`，由 apt 包 **`llvm-21-tools`**（候选 `1:21.1.8-6ubuntu1`）提供，实测未安装。它不在契约 9.1 的锁定表里，所以我不把它算作「版本不满足」的开工门槛；但它的缺省会让**非干净**情况下的定位从「看函数名」退化成「拿 BuildId + 地址手动 addr2line」，在 S1-02/S1-04 这类内存与生命周期任务里会实打实地拖慢我。已列为待补项。

**开工前置条件状态：满足。** Clang 21.1.8（≥14 ✅）+ 三个 sanitizer 阳性验证通过 + wrk 4.1.0 + hey 0.1.4 就位，S1 代码自 `S1-01` 起启动。Docker 仍不可用，属 S3 之前的阻塞项，不影响 S1。

### 2.3 wrk 的 `Socket errors: … timeout` 那一格，在这台机器上测不到触发条件（2026-11-04 实测，S5-08）

B-022 追记那笔未归因的 socket timeout 需要一个"它到底由什么触发"的答案，所以拿一台
`accept()` 之后**永不回字节**的 python 对端（`/home/archer/s5-08-wrk-timeout-probe.sh`，4 条连接）量了一次：

| 档 | wrk 的输出 | 读法 |
|---|---|---|
| `-t1 -c4 -d4s` / `-d8s` / `-d13s`（不带 `--timeout`） | `0 requests in …`，**整份输出里没有 `Socket errors` 那一行** | 对端不回字节的形状**根本不进那条计数** |
| `-t1 -c4 -d4s --timeout 2` | 同样 `0 requests`、同样没有 `Socket errors` 行 | 显式把超时压到 2 s 也不报 → 不是我推测的"缺省 10 s"那回事 |

**结论只有一句，而且是一句关于仪器的话**：`wrk 4.1.0-4build3` 在"连接建好、请求发出、对端永不回字"这台
假对端上**不产 timeout 计数**，所以我无法用它复现或证伪"那 48 条 timeout = 客户端等响应超时"这个读法。
这条记在这里，是给下一个碰那笔账的人挡路的：**别拿 `Socket errors: timeout` 当"客户端等了 N 秒"的仪器**——
它真正的触发条件在本机这台仪器上取不到。（B-022 追记第四节有同一组数。）

## 3. 唯一实质偏离：libmysqlclient

契约原本锁 `libmysqlclient 8.0.x`，但 Ubuntu 26.04 的 apt 源里只有 8.4.11，8.0 既无源、在 Oracle 侧也已停止支持。我按 26.04 的现实把锁定值改为 8.4.x，理由和边界都写在这里，不装作它没发生：

- 服务端镜像仍锁 `mysql:8.0.36`（未改）。8.4 客户端连 8.0 服务端在 Oracle 的支持矩阵内是允许的方向（新客户端连旧服务端）。
- **这条兼容性目前只是推断，不是事实。** 要到 S3 第一次真正建立 MySQL 连接时实测取证，写进 `FAULT-INJECTION.md` 或首次连通记录里才算成立。
- **✅ 已取证（2026-10-30，S3-04）**：`l7gate_mysql_probe` 五步全通（connect → CREATE TABLE → INSERT → SELECT 读回 → DELETE），`client=8.4.11 ↔ server=8.0.36`。环境：Docker Desktop WSL Integration（server 29.4.3）、镜像 `mysql:8.0.36`（digest `sha256:a53272402242…`）、端口发布形态 `127.0.0.1:13306 → 3306`。探针可复跑：`build/release/tools/l7gate_mysql_probe --port 13306`。**8.4 客户端 ↔ 8.0 服务端兼容性从"推断"转正为"实测成立"。**
- 客户端 8.4 / 服务端 8.0 是否一并对齐到 8.4，留到 S3 触到 MySQL 时决策，现在不动镜像 tag——镜像 tag 一旦中途更换，历史数据就断代。
- **客户端侧就位取证（2026-10-30，S3-04 认领时）**：`libmysqlclient-dev 8.4.11-0ubuntu0.26.04.1` 已在（用户此前 apt 安装，`mysql_config --version` = 8.4.11、`--libs` 带出 `-lmysqlclient -lz -lzstd -lssl -lcrypto -lresolv -lm`）；探针（`g++ -std=c++20` 编译 + 链接 + 运行时 `mysql_get_client_info()`）返回 **8.4.11**——只验库、不建连接，服务端连通取证仍待 Docker WSL Integration（阻塞在项目所有人侧，已量化报告：Desktop 未运行 + WSL Integration 未开启）。

### 3.1 hiredis 就位取证（2026-11-02，S4-04 认领时；只验库、不建连接）

契约 9.1 锁 `hiredis v1.2.0`，本机**字面命中且已安装**——这一格与第 3 节那条 libmysqlclient 的偏离不同源，没有版本偏离要论证，要取证的是"命中的两端真能对话"：

- `dpkg -l`：`libhiredis-dev 1.2.0-6ubuntu4` + `libhiredis1.1.0 1.2.0-6ubuntu4`；`/usr/include/hiredis/hiredis.h` 的 `HIREDIS_MAJOR/MINOR/PATCH` = **1/2/0**；`pkg-config --modversion hiredis` = **1.2.0**。
- **SONAME 是 `libhiredis.so.1.1.0`**（`objdump -p` 实测）——ABI 号与 API 号不是一回事，apt 的运行时包名 `libhiredis1.1.0` 就是按前者取的。契约锁的是 API 版本 v1.2.0，命中。
- **探针（形状同第 3 节 mysql 探针：编译 + 链接 + 运行时符号解析，不建连接）**：`g++ -std=c++20 -Wall -Wextra ... -lhiredis` 编译链接 rc=0，`ldd` 实际解析到 `libhiredis.so.1.1.0 => /usr/lib/x86_64-linux-gnu/libhiredis.so.1.1.0`，`redisFormatCommand("EVAL %s %d %s %d", "return 1", 0, "k", 1)` 产出 **49 字节**、与手写 RESP 逐字节相等（`byte_exact=1`）。
- **撞出一条实现期的雷（记进 ADR-05 追加节第一节末条）**：那个出参按文档要用 `free()`；我先按 hiredis 里存在的另一个口 `redisFreeSdsCommand()` 去释放，**当场 `free(): invalid pointer` 中止（rc=134）**。两个释放口不通用，看名字猜不出来。
- **✅ 服务端连通取证已完成（2026-11-02，S4-04）**：`l7gate_redis_probe` 七步全通
  （connect → `PING`→`PONG` → `INFO server`=**7.2.5** → `SCRIPT LOAD`（产品那份 Lua，678 字节，
  sha `27f391e42f3e…`）→ 四格确定性 `EVALSHA` 读数与本地令牌桶逐格对账 → `HGETALL` → `DEL`），
  `client=hiredis-1.2.0 ↔ server=redis-7.2.5`，`rc=0`。环境：Docker Desktop WSL Integration
  （client/server 29.4.3）、镜像 `redis:7.2.5`（digest `sha256:3aaec283e6e5…`）、端口发布
  `127.0.0.1:16379 → 6379`、`--cpuset-cpus 15`（容器内 `cpuset.cpus.effective=15` 实测生效）。
  复跑：`PRESET=debug bash scripts/smoke-s4-04.sh`（阶段 0/1），或直接
  `build/debug/tools/l7gate_redis_probe --port 16379`。读数与 CPU 分配写在 `BENCHMARK.md` **B-017**。
  **镜像身份的核验缺口与 nginx 那次同形**：本机直连 `registry-1.docker.io` 建不起连接，我无法把这个
  digest 与官方仓库比对——它来自 5.1 那个 mirror，照记，不假装核验过。
- 本机另有 `redis:8.0` 镜像、`apt` 的 `redis-server` 候选是 **8.0.5**：契约 9.1 锁 `7.2.5`、
  契约 9.3 明写 Redis 一律走 Docker，所以我**不用它们顶包**，也不动 tag（换 tag 历史断代）。

## 4. perf 能否真正采样（**已验证 2026-11-02**：单进程可以，系统级不行）



`perf 7.0.14` 已安装，但运行内核是 `6.18.40.1-microsoft-standard-WSL2`，两者版本不匹配。WSL2 的定制内核历来对 `perf_event_open` 支持不全，而 SLO 附加产出（「与 Nginx 的差值必须定位到具体原因」）明确要求用 perf 火焰图作证。

- ~~这项我没有验证~~ → **已于 2026-11-02（S4-05 期间）验证完，结论在下面的 4.1**：能用，但只有一种形态能用。
- 当时的计划就是"S4 期间跑一次 `perf stat -a sleep 1` 与最小火焰图冒烟"。跑完了：`-a` 系统级这条路
  **不通**（没有 PMU、record 直接失败），**单进程这条路通**（硬件事件 5918 样本、软件事件带栈）。
  所以备选（async-profiler / eBPF）不用启用，也不需要为它写 ADR——但 S5 的火焰图必须按 4.1 那三条约束取。

### 4.1 已验证（2026-11-02，S4-05 期间把上面那条计划执行掉了）

实测基线：`kernel.perf_event_paranoid = 2`（`sysctl` 直读）。结论不是"perf 能用/不能用"一句话，
而是**三种形态分别成立与否**——S5 的火焰图只能用其中一种：

| 形态 | 命令 | 实测 |
|---|---|---|
| 系统级计数 | `perf stat -a sleep 1` | rc=0，但 `cpu-cycles:u` 报 **`<not supported>`**（同行走 `nan GHz  cycles_frequency`）→ 系统级这条路没有 PMU |
| 系统级记录 | `perf record -g -a sleep 1` | `Adjusting ... >= 2: Disallow CPU event access` → `Failure to open any events for recording`，`perf report` 说落盘文件 **zero-sized** → 系统级采样直接不可用 |
| 单进程·硬件事件 | `perf record -- <1.4 s 的忙循环>` | **5918 样本**；`perf stat -e cycles,instructions --` 同进程拿到 **6,002,662,598 cycles / 2,400,108,728 instructions（IPC 0.40）** |
| 单进程·软件事件 | `perf record -e cpu-clock -F 99 -g -- <忙循环>` | **140 样本**、带调用栈（`perf report --stdio`：100% Self 在 `main`，栈底 `_start`）→ **火焰图的最小可用形态就是这一条** |
| `/proc/schedstat` | 上面每条 record 都伴随 | `Failed to open /proc/schedstat` —— 警告非致命（这台 WSL2 内核没那个文件），不影响样本 |

**对 S5 差值归因的三条硬约束（现在就写死，不要到 S5 才发现）**：① **必须由 perf 直接拉起被测进程**
（`perf record -g -- ./build/release/l7gate …`），不能事后 `perf record -p <pid>` attach——paranoid=2
下 ptrace 那条路不通；② `-a` 系统级与内核态符号都拿不到，所以火焰图**只覆盖用户态**，
网关自引入的增量里"内核时间去哪了"这一格今天没有直接证据，只能从 syscall 计数侧面推（B-013/B-016
那套每请求 `epoll_ctl` 逐数闭合的读法就是侧面证据）；③ 对照基线 nginx 在容器里跑，同一条约束对它
同样成立，所以两边的取法必须一样（都是"由 perf 拉起自己的进程"），不能一边 attach、一边拉起。

**抬 `kernel.perf_event_paranoid` 属环境侧决定，我没动**：抬到 1/0/-1 能换来 `-a` 系统级与内核态帧，
代价是本机任何进程都能读全系统事件。这不是我能替项目所有人决定的事，所以这里只把"今天这个设置下
能测什么"记全。如果 S5 需要内核态火焰图，那是先写 ADR 再改环境，不是先改了环境再找理由。

### 4.2 S5-07 把上面第 ③ 条（「nginx 对照臂同法」）当成判据现测了一次：**在容器这一侧不通**

上面那句「两边的取法必须一样」是 2026-11-02 写下的**计划**，不是一条被验证过的能力。
S5-07 本轮把它当判据量了（探针 `/home/archer/s5-07-perf-probe.sh`，全部带 timeout）：

| 路 | 读数 | 结论 |
|---|---|---|
| 宿主原生，由 perf 直接拉起被测进程 | `perf record -e cpu-clock -F 99 -g -- …` 在 wrk 满负荷的网关上出 **3K 样本**、`perf report --stdio` 有符号（`Samples: 3K of event cpu-clock:u`） | 4.1 那一种形态**在 HEAD 上复现成立**，这是本轮归因段的证据来源 |
| `nginx:1.25.4` 镜像里有没有 perf | `docker run --rm --entrypoint sh nginx:1.25.4 -c 'command -v perf'` → **`NO_PERF_IN_IMAGE`**（同一发里 `curl=/usr/bin/curl`、`wget=无`） | 基线镜像自带不了 perf，"同法"不能靠镜像本身 |
| 宿主 perf + 它的依赖库挂进 `ubuntu:26.04`（与宿主 glibc 同代） | 带 `--cap-add=CAP_PERFMON --security-opt seccomp=unconfined`：record **能跑、能落盘**（只警告 `kptr_restrict` 限制了内核符号）；**不带 cap**：`Failure to open any events for recording` | 容器里 `perf_event_open` 的开关是 **caps**，不是内核；glibc 同代是第二个必要条件 |
| 宿主 perf 挂进 `nginx:1.25.4`（Debian 12 / glibc 2.36） | `/probe/perf: symbol lookup error: /probe/perflib/libc.so.6: undefined symbol: __tunable_is_initialized, version GLIBC_PRIVATE` | **ADR-27 第一节那条"挂一半 glibc 比不挂更糟"在同一批镜像上原样复现**——这条路直接堵死 |
| `nginx:1.25.4` 里 apt 装 perf | **2026-11-05 已裁：不加**（项目所有人那句"按你建议的顺序完成即可"＝逐条按我给的推荐，推荐是不加）。这条路要给**基线镜像加层**，属契约 9.1 的镜像内容变更 + 禁令三 | 唯一剩下的"同法"路径，**裁的是代价不是能力**：2026-11-04 那发探针已证明加得上、出得来图（`linux-perf` 装得上、由 perf 直接拉起 nginx 有 3K 样本 / 62 条 `ngx_` 符号，读数在 TASKS「已知待决事项」第一笔），但分母身份一变，B-005/B-022 那些比值从此不同轴 |

**所以对 #3 差值归因的诚实口径是**：网关侧的用户态火焰图有（上面第一行），
**nginx 侧的用户态火焰图在这台机器上取不到**，原因是两条实测（镜像里没有 perf；宿主 perf 挂进去因
glibc 不同代而 `symbol lookup error`），而不是"没做"。要么动基线镜像（**2026-11-05 已裁：不加**，代价与理由在
TASKS 已裁清单第 ⑪ 条），所以这一格的 nginx 侧**永久**只能用**结构对照**代替——终判档用的就是它：`worker_processes auto`（16）对 `--workers 16`、
关 `access_log` 那一档、单 worker 那一档、以及每请求 `epoll_ctl` 的整数等式（B-013/B-016/B-018/B-019）。
这条限制进 BENCHMARK 的 #3 归因段，不进火焰图那一格冒充已取到。
**2026-11-02 项目所有人裁定：不抬，保持 `paranoid=2`**，S5 的火焰图就按上面那一种形态取
（由 perf 直接拉起被测进程、只有用户态栈，nginx 对照臂同法）——这条不再是待决项。

## 5. Docker Desktop WSL Integration（当天开过、掉过两次线；#3 基线已能复现）

Redis / MySQL / Prometheus / Grafana / Nginx 基线全部走 Docker（契约 9.3）。**用户已于 2026-10-26 在 Docker Desktop 里开启本发行版的 WSL Integration**，实测：`docker --version` = 29.4.3（build 055a478）、`docker compose version` = v5.1.3、`docker info` 引擎侧 Server 29.4.3 / storage driver `overlayfs`——**引擎这一侧真的可用**，本文件第 2 节原先那句「❌ 阻塞项」已按实测改成半开状态。

剩下的阻塞在**拉镜像**这一步，取证如下（全部本机实测，不是推测）：

| 探测 | 结果 |
|---|---|
| `docker pull nginx:1.25.4` | `failed to resolve reference ... docker.mirrors.ustc.edu.cn ... no such host` —— Docker 配置里指定的 registry mirror 域名已经不存在 |
| `getent hosts registry-1.docker.io / index.docker.io / auth.docker.io` | DNS **都能解析** |
| `curl --max-time 12 https://registry-1.docker.io/v2/` | **`000`**（连接没建成）——直连 Docker Hub 走不通 |
| `curl https://www.baidu.com` / `https://github.com` | **200** —— 不是"这台机器没有外网" |
| `curl https://docker.m.daocloud.io/v2/`、`https://docker.xuanyuan.me/v2/`、`https://quay.io/v2/` | **401**（registry API 活着，正常要 token）；`https://hub.rat.dev/v2/` 302 |

所以形状很清楚：**网络是通的，坏的是"指定了一个已经死掉的 mirror，而直连官方源又被拦"**。这不是我能靠改配置修好的——registry-mirrors 在 Docker Desktop 那一侧（`Settings → Docker Engine` 的 JSON），属于用户的机器与共享环境，按契约 10.1 那条"环境类阻塞先停先报"的精神，我停在这里请示，给了三个候选（换 mirror / 允许经第三方镜像站 pull 后 tag 成 `nginx:1.25.4` 并把来源不可核验记进口径 / #3 留账后补）。

**这条直接决定 SLO #3 能不能判定**：#3 的判定式是 `QPS_网关 / QPS_nginx ≥ 60%`，而 `nginx:1.25.4` 是契约 9.3 锁定的基线镜像 tag——中途换 tag 会让历史数据断代，所以我不动 tag，也不接受"换个别的 web server 顶一下"那种替代（那等于换一个分母）。S1 门禁第 1 条里的 **#3 在当时那一轮只有自测 QPS**（经网关 21,582.93 / 直连 57,170.55，B-004），按项目所有人的裁决记为「不合格，等镜像源解决后补测」——**这一条已经在同日 5.1 之后补到位**（B-005 有 nginx 三档 + 网关 + 直连天花板五档读数），但补到之后我仍把 #3 记成「有读数、判定暂缓」，理由全写在 B-005，不在这里重复。

### 5.1 mirror 已由用户换掉，基线跑通了（同日）；但容器拓扑给 #3 留了一条不可忽略的偏差

用户在 Docker Desktop 里换掉失效 mirror 之后，`docker pull nginx:1.25.4` 成功，`scripts/bench/s2-qps-vs-nginx.sh` 五档全部跑通（基线记录见 `docs/BENCHMARK.md` B-005）。**镜像身份的核验有一条缺口，照记**：本机直连 `registry-1.docker.io` 与 `hub.docker.com` 都建不起连接（`000`），所以我**无法把这个本地 digest 与官方仓库的 digest 比对**；本地能看到的事实是 tag = `1.25.4`、容器内 `nginx -v` = `nginx/1.25.4`、`image id = sha256:9ff236ed47fe…`，来自换好之后配的那个 mirror。这一条不影响 nginx 自己报告的行为，但意味着"基线来源可信度"依赖那个 mirror  faithfully 镜像了 Docker Hub——我没有办法在这台机器上证明它，所以写在这里而不是假装核验过。

**容器的网络可达性是实测出来的，不是猜的**（这直接决定 #3 的测量条件能不能成立）：

| 探测 | 结果 | 结论 |
|---|---|---|
| `docker run --network host` 后从 WSL 看 `ss -ltn` / `curl 127.0.0.1:80` | 监听表里没有 `:80`，curl `000` | **`--network host` 不共享本发行版的网络命名空间**——容器那条路走不通，别指望它绕开 NAT |
| WSL → 容器 bridge IP（`172.17.0.2:80`） | `000` | bridge IP 从 WSL 侧不可路由 |
| WSL → `127.0.0.1:发布端口`（`-p`） | **200** | 唯一可测的入口就是端口发布 |
| 容器 → `$(WSL eth0 IP):上游端口` | **200 + 1024 字节** | 出向要走容器→WSL 那一跳；上游因此必须绑 `0.0.0.0`（绑 `127.0.0.1` 容器够不到） |
| 容器（`prom/prometheus:v2.51.2`，busybox `wget`）→ `host.docker.internal:宿主端口`，加 `--add-host=host.docker.internal:host-gateway` | **取回 `# HELP elapsed_ms`** | S5-03 新增的这条路：抓取面在**宿主机**上（网关 `--metrics-port` 与 exporter），所以入库的 `deploy/prometheus/prometheus.yml` 两个 target 都写成 `host.docker.internal:<port>`；被测方必须绑 `0.0.0.0`，理由与上一行同一个 |
| 容器 → `172.17.0.1:宿主端口` | `Connection refused` | Docker Desktop 这台机器上 `172.17.0.1` 不是宿主网关；别把它当 `host-gateway` 的替代写法（我第一轮试过） |

> 同一条路上我还测到过 `10.255.255.254` 超时——**那一条不作数**：我的取地址命令 `ip -4 -o addr show scope global` 把 `lo` 上那个 `/32` 当成了第一个"全局地址"，所以那一次打的是回环地址而不是 eth0。上面第 4 行（2026-10-30 那次）与这一轮对 eth0 的读法并不冲突，冲突的是我那一行的取数方式。

于是 **#3 的「同机、同参数」在网络这一维上做不齐**：nginx 的客户端 leg 多一次端口发布（DNAT/userland proxy）、上游 leg 多一次容器出向，而网关两侧都是原生回环。方向上这是**拖累 nginx**，所以拿它算出来的 `QPS_网关/QPS_nginx` 对**我**偏乐观——B-005 的比值和结论都带着这条声明，#3 是否算"达标"由项目所有人裁，我不给自己发这张证明。另外两条同场记下的不齐项：nginx 容器用的是它自己镜像的 libc/分配器（契约要求「同一分配器记录」，这一条没满足）；镜像默认 `worker_processes auto` = 16 worker，而 S1 按 ADR-03 就是单 Reactor（多线程是 S2 的内容）——所以 B-005 额外跑了「关 access_log」与「单 worker」两个**只差一个变量的对照组**，用来把"日志写盘"和"并发分摊"这两维从差值里拆出来。

**同日 23:3x 复查追加一条**：仓库搬到 `workspace/` 之后重跑门禁时，`docker` 在本发行版里又不可用了（WSL Integration 掉线）。这一条只影响 **#3 基线的可复现性**（S5 终判要求那一轮能重跑），不影响 B-005 已记录的数据与 `build/bench/s2-nginx-*/` 里的原始输出。修它在 Docker Desktop 的图形设置里，属共享环境，按纪律报给项目所有人、我不自行改动。顺带一句值得留档的：`s2-qps-vs-nginx.sh` 遇到镜像不在本地时是**直接拒绝并说明原因**（`✗ 本机没有 nginx:1.25.4 镜像（契约锁定的基线 tag，我不换）`），而不是悄悄换个别的 web server 顶一个数出来——这正是契约 8.2 禁令一在脚本层该有的样子。

**同日晚些时候（23:40–23:46）的状态更正**：项目所有人重开 Docker 之后集成恢复，`nginx:1.25.4` 五档在**新路径**上完整重跑成功（B-005 附），上面那句"目前不可复现"就此作废——现在它可复现，且已经复现过一次。**这条掉线本身要留在文档里，因为它是 S5 的一项风险而不是一段插曲**：那一轮断线在档 2 留下了一个 3,148.41 的读数（引擎正在退出时取到的），它离正常值差 3 倍以上，却因为"这一档不是判定档"而看起来人畜无害；如果它落在档 1（判定分母），单轮取数就会把一个 206% 说成 600% 或者反过来。**集成会掉线 = 会出现"看起来合理但来自退化环境"的读数**，这就是取样规程要求"每改动至少两轮、报极差"的另一条理由，也是 S5 终判前必须确认 `docker info` 与 `nginx` 容器状态的原因。跑完这一轮我按项目所有人的要求清场，收尾复查（23:53）实测：`wrk`/`l7gate`/`nginx`/`test_upstream` 相关进程 **0 个**、1808x/18099 监听端口**全空**、`docker ps -a` 里本项目的容器已全部删除（只剩一个与本项目无关的 `tinrpc-server`，`Exited (137) 2 months ago`，我没碰它）。

**2026-11-02（S4-04 认领日）第三次不可用，且这次不是集成开关**：Docker Desktop 整机没运行。取证逐条：Ubuntu 发行版内 `docker` 命令不存在、`/var/run/docker.sock` 与 `/run/docker.sock` 都没有、`/mnt/wsl/` 下只剩 `resolv.conf`（没有 `docker-desktop` 共享）、2375/2376 未监听；从 WSL 直接调 Windows 侧的 `docker.exe info` 报 `failed to connect to the docker API at npipe:////./pipe/dockerDesktopLinuxEngine`；`tasklist` 里 `Docker Desktop.exe` 与 `com.docker.backend.exe` 均无；`wsl -l -v` 显示 `docker-desktop` 发行版 **Stopped**。后果：`redis:7.2.5` 在不在本机**无法判定**（`docker images` 需要引擎），S4-04 的连通取证与降级三段容器侧卡在这一条上——**修它在图形界面侧，属共享环境，报项目所有人，我不自行改动**。

**同日 2026-11-02 恢复**：项目所有人启动 Docker Desktop 之后集成回来了（`docker --version` 29.4.3、
`docker info` 引擎侧 server 29.4.3 / `NCPU=16`），`redis:7.2.5` 经 5.1 那个 mirror **pull 成功**
（本机原先只有 `redis:8.0`，我没拿它顶包），S4-04 的取证与冒烟都在这一轮跑出来（B-017、
FAULT-INJECTION §14）。这一条掉线仍然是**第三次**，所以那句"集成会掉线 = 会出现看起来合理但来自
退化环境的读数"照旧成立：本轮的代价不是错读数，而是**整整一条任务的前置被卡住半天**——S4-04 的
认领提交（`7edff8f`）里我明写了"本提交不含任何容器侧结论"，就是这个原因。

### 5.2 WSL interop 能把 Windows 侧的 Chrome 当取数仪器用（2026-11-04 实测，S5-04 / ADR-26）

S5-04 的验收句⑤ 要求"取数证据用 DOM 断言"，而本机没有 Linux 侧浏览器（实测 `chromium`、
`chromium-browser`、`google-chrome`、`google-chrome-stable`、`firefox` **全部 MISS**，
`node` 也不在 PATH 上——它在 `~/tools/node/bin` 下）。可用的通道只有一条：**从 WSL 里直接执行
Windows 侧那个 exe**。

| 项 | 实测值 / 事实 |
|---|---|
| Chrome 版本 | **156.0.8078.80**（`powershell.exe (Get-Item …chrome.exe).VersionInfo.ProductVersion`；`chrome.exe --version` **不给版本号**，它会把浏览器拉起来——我第一次就撞上了，别再照契约 9.2 那个姿势去取） |
| 可执行路径 | `/mnt/c/Program Files/Google/Chrome/Application/chrome.exe`，`test -x` 为真，WSL interop 能执行 |
| 取 DOM 的姿势 | `--headless=new --disable-gpu --no-first-run --user-data-dir=<Windows 路径> --virtual-time-budget=30000 --dump-dom <url>` |
| 单轮耗时 | **1.11 s**（三轮各 1.11 s，含 Chrome 冷启动）——所以"重试到 8 次"这条不贵 |
| 单次快照可靠吗 | **不可靠**：同参数连跑三轮，第三轮只抓到 `connect` 那一行（预算 8000 那一档也只到 `sent`，15000 才抓到 `recv`）。判定必须是"有限重试 + 完成哨兵"，不是跑一次 |
| 输出行尾 | **CRLF**（Windows 进程写的）。`grep -x` 这类整行匹配会全部落空，取数前要 `tr -d '\r'`（快照按原样留档，那是证据） |
| profile | 必须给**专用** `--user-data-dir`（我用 `%TEMP%\l7gate-s5-04-profile`），绝不能挂到用户自己的 Chrome 配置上 |
| Windows→WSL 的可达性 | 顺带纠正一条我之前记着的印象：Windows 侧**能**打进 WSL 的端口，`127.0.0.1:<port>` 与 WSL 的 NAT IP（本轮 `172.28.252.175`）两路 `curl` 都是 200。这与 5.1 那条"容器 → `172.17.0.1` refused"是**两个不同的网络**（一个是宿主↔WSL，一个是容器↔WSL），别混用 |

**这条为什么算新依赖**：`chrome.exe` 不在契约 9.1 的锁定表里、版本随 Windows 侧自动更新漂移，
而 `scripts/smoke-s5-04.sh` 现在用它取 DOM。按禁令三，登记在 ADR-26（第四节第 2 条），
并照 `smoke-s3-04`/`smoke-s4-04` 对 Docker 的先例：**依赖不在时脚本在阶段 0 自己拒跑**（`verdict.txt`
写 `refused=1`），不换个浏览器顶上去，也不把 DOM 那一半静默降级成"没做"。
它不参与任何 SLO 测量——三个数字的读数仍然只认 `scripts/bench/` 的落盘档与退出账。

### 5.3 编排这一层的通路事实（2026-11-04 实测，S5-05 / ADR-27）

S5-03 留给 S5-05 的那笔账——**容器之间能不能按服务名解析**——本轮第一次真测，四条都落在这里。
探针落在 `/home/archer/s5-05-probe/`（不进仓库；每台探针容器自己 `down`，收尾 `--filter name=l7gate-` 为空）。

| 探测 | 读数 | 结论 |
|---|---|---|
| compose 用户自定义网络里，prom 容器 `wget http://gateway-stub/` | **第 0 次尝试即 `200 OK`**；容器内 `/etc/resolv.conf` 是 `nameserver 127.0.0.11`，`docker inspect` 的 `Aliases` 就是服务名 | **服务名解析在 compose 网络里通**。5.1 那一路"不共享网络命名空间"讲的是**宿主↔容器**与**默认 bridge**，不等于容器↔容器 |
| 同一姿势，把网关配置里的 `address` 写成主机名（`localhost`，能解析的那种） | `HTTP 502`、退出账 `upstream_connect_failed=1`、`responses_completed=0`；对照臂 `127.0.0.1` 是 `200 + seq=1` | **网关不做名字解析**（`inet_pton`，`src/net/socket.cc:16-35`）。所以凡被网关引用的地址只能写 IPv4 字面量 |
| compose 声明 `ipam` 静态子网 `172.31.250.0/24` 并给容器 `ipv4_address` | 同网络另一台容器 `wget http://172.31.250.20/` → `200 OK` | 静态 IPAM 在这台 Docker Desktop 上生效；编排据此把地址写成文件里的事实 |
| prom 容器 `wget http://host.docker.internal:18777/`（18777 是**另一台容器**发布到宿主 `127.0.0.1` 的端口，宿主侧同一端口先读到 200） | **`200 OK`**；对照 `http://172.31.250.1:18777/` → `Connection refused` | 容器经 `host.docker.internal` 打得通别的容器的**发布端口** → 入库的 `prometheus.yml` / grafana 数据源**不需要改值**，S5-03 测的仍是同一份 |
| 退化：删掉 prometheus 那台的 `extra_hosts` 后整栈重跑 | **两条 target 仍然全 up**；容器里 `/etc/hosts` 没有那一行，而名字照样解析到 `192.168.65.254` | **更正 5.1 与入库配置里那句"不带上就变假目标"**——它说重了。本机不依赖那一行也能解析；留着它是**不押在 daemon 的自动行为上**（换引擎/发行版那个自动名字未必存在）。已就地写进 `deploy/prometheus/prometheus.yml` 与 compose 的注释 |
| 契约锁的镜像里谁能承载我们的二进制 | 网关要 `GLIBC_2.38` + `GLIBCXX_3.4.31`；`redis:7.2.5`/`nginx:1.25.4` 是 glibc 2.36、`mysql:8.0.36` 是 2.28、prometheus 是 busybox、grafana 是 musl。把宿主库挂进 `redis:7.2.5` → `symbol lookup error: undefined symbol: __tunable_is_initialized` | **五个锁定镜像一个都不行**，而"挂一半宿主 glibc"比不挂更糟。这是 S5-05 需要一份 ADR 的全部原因 |
| `docker pull ubuntu:26.04`（走 daemon 里配的 mirror） | rc=0，digest `sha256:6a06111c3b0a8600feadb270aec771dc239bf407b25625f93f44ab00e11f4d3c`，`ldd --version` = `Ubuntu GLIBC 2.43-2ubuntu2.4` | **与宿主构建环境逐字同代**。它的 apt 里 `libmysqlclient24=8.4.11-0ubuntu0.26.04.1`、`libhiredis1.1.0=1.2.0-6ubuntu4` 与契约 9.1 字面命中，所以壳镜像装的是契约已点名的版本，不是新增第三方依赖。判定镜像在位一律以 `docker image inspect` 为准（口径⑰ 那族） |
| `docker manifest inspect ubuntu:26.04`（直连 registry） | 超时（`registry-1.docker.io` 建不起连接） | 与 5.1 那条一致：**manifest 那一维仍无法与官方对账**，本机 digest 来自配置的 mirror，照记不假装核验过 |

**编排占用的名字与端口**（compose project name = `l7gate`，所以容器一律 `l7gate-<service>-1`，网络 `l7gate-obs`）：
发布的宿主端口是 `127.0.0.1:18080`（业务）/ `18522`（观测面）/ `18090`（exporter）/ `19090`（Prometheus）/ `13000`（Grafana），
容器内部 `18081/18082` 两台上游不发布。**这套端口与 `smoke-s5-01/02/03` 那几条是同一批数**，所以四者不能同时跑；
`smoke-s5-05.sh` 阶段 0 逐端口探活，被占就 `die`。B-017 那格在编排层的落点：`cpuset: "15"` 只给 redis 一段，
前提 `docker info` 的 `NCPU=16` 由阶段 0 断住。

### 5.4 `tc netem` 的可用性与作用域（2026-11-04 实测，S5-06 / ADR-13 那条"仅作交叉验证"的落地）

契约 9.3 与 S5 拆解都留着一条"WSL2 内核不一定支持 netem"的悬念。**本轮量完了，结论是「通」**，
但**通的地方不是宿主回环**——把四条路分开记，因为最容易犯的是把"加不上"读成"内核不支持"。

| 路 | 读数 | 结论 |
|---|---|---|
| A. 宿主原生回环（uid=1000） | `tc qdisc add dev lo root netem delay 10ms` → `RTNETLINK answers: Operation not permitted`；`modprobe sch_netem` → `could not insert: Operation not permitted`；`sudo -n` → 要交互认证 | **加不上，但这是权限墙不是内核墙**。缺的是 `CAP_NET_ADMIN`，不是模块 |
| A′. 同一个内核里的模块本体 | `/lib/modules/…/kernel/net/sched/sch_netem.ko` 的 `vermagic` = `6.18.40.1-microsoft-standard-WSL2 SMP preempt mod_unload modversions`，与 `uname -r` **逐字相同** | 根来了就能加载；而且 C/D 两路证明 netem 在这颗内核里**真能生效** |
| B. 非特权 netns（第一发，**不算数**） | `unshare -Un`（**漏了 `--map-root-user`**）→ ns 内 `uid=65534`、`CapEff=000000000000000`、加 qdisc 与建 veth 全 EPERM | **这是我的仪器错**：没映射 uid 就等于没有 caps，把"我用错了参数"读成"userns 这条路不通"就错了 |
| C. 非特权 user+netns + uid 映射 | `unshare -Un --map-root-user` → `uid=0`、`CapEff=00001ffffffffff`、`tc qdisc add dev lo root netem delay 10ms` **加上**，读回 `qdisc netem … limit 1000 delay 10ms` | **这是 S5-06 的执行场所**：不需要 root、不碰宿主、每臂一个全新 ns、可复跑 |
| D. 容器（`--cap-add=NET_ADMIN`） | 容器自己的 `eth0` 上加 netem 成功（`qdisc netem 8001: root … delay 10ms`）；对照不带 `NET_ADMIN` → EPERM | 另一条通路；且 `docker run ubuntu:26.04 uname -r` 报的**也是** `6.18.40.1-microsoft-standard-WSL2`，与 WSL 同一颗内核串 |

**生效的毫秒数（带对照臂，各自全新的 ns，回环上的一问一答）**：对照臂 `qdisc noqueue` →
`connect 0.07–0.18 ms`、单向回包 **0.01 ms**；注入臂 `delay 10ms` → `connect **20.14–20.27 ms**`、
单向回包 **10.05–10.25 ms**。**握手为什么是 20 ms 而不是 10 ms**：一次往返在回环上走**两个方向的各一次出向**，
所以按机制读成 2×delay；这一格不是"量歪了"，是横轴的单位（见 FAULT-INJECTION §25.1 那条被实测改写的判据）。
`delay 20ms 5ms loss 25%` 同样加得上；UDP 发 200 收 137（观测 31.5%，配置 25%），
qdisc 自己数 `Sent 200 pkt (dropped 63, overlimits 0 requeues 0)`——两处互证同一个数，
差 6.5 个百分点属伯努利抽样方差（n=200、p=0.25 时 σ≈3.1%），不是 netem 不准。

**两条要记住的作用域事实**：**① 注入的作用域是那一个 netns 的 `lo`**，宿主回环从头到尾是
`qdisc noqueue 0: root refcnt 2`（脚本开工前与收尾各核一次，逐字相同才判过）；
**② 观测面不在注入的作用域之外**——Prometheus 也好、冒烟的 curl 也好，取数走的都是同一条被注入的回环，
所以 loss 臂会出现"抓取偶发超时"（实测 14 次采样空读 1 次）。这一条既是坑也是产出：
**观察者的超时必须比注入的往返长**，否则"没读到"会被读成"没发生"（S5-06 的退化 D2 就是打在这一格上，
它同时把 `p50=1013 ms` 这种"看着像有延迟"的读数判了死——下界按机制给才有牙）。

**工具版本原样记录**（契约 9.2 的姿势）：`tc utility, iproute2-6.19.0, libbpf 1.6.3`；
`uname -r = 6.18.40.1-microsoft-standard-WSL2`。
**就地更正两处旧读数**：TASKS 的 S5-06 行与交接段一直写 `6.18.33.1-microsoft-standard-WSL2`，
本轮实测是 `6.18.40.1`（`docs/ENVIRONMENT.md` 第 3 节与第 7 节的 perf 那一行早就是 40.1，
是这两处没跟着改）。`tc` 不在契约 9.1 的锁定表里，但契约 9.3 **本来就点名了 `tc netem`**
（"仅作交叉验证，WSL2 内核不一定支持"），所以它不是新增依赖，是契约里预期过的手段；
版本按上面那行记录，判定一律以 `tc qdisc show` 读回的**内核回执**为准，不以"命令存在"为准。

## 6. 文件系统位置（会影响数据的一条）

仓库与全部构建、压测都在 WSL 原生 ext4 内。当前仓库位置是 `/home/archer/workspace/l7gate`（2026-10-26 从 `/home/archer/l7gate` 平移进来，同发行版同文件系统，所以契约 9.4 的条件与全部历史数字继续成立；搬家的代价与复跑记录见 TASKS 交接段的「搬家」一条）。Windows 侧只能经 UNC 通道（`\\wsl.localhost\<distro>\<home>\workspace\l7gate` 形式）查看和编辑同一批文件。

把构建或压测放到 `/mnt/c`、`/mnt/d` 这类 drvfs/9P 挂载点是禁止的：9P 的 IO 抖动会直接进 p99/p999，测出来的数既不属于网关也不属于 Nginx。详细理由记在契约第 9.4 节。

### 6.1 脚本侧的端口带（S5-01 加这一格，因为它开始占新端口）

冒烟与压测脚本用的是**写死的 185xx / 1808x 带**（`smoke-s4-05.sh` 18500–18502、`smoke-s4-06.sh` 18509、`s1-p99.sh` 18081），ctest 一律 bind 端口 0 由内核分配（`reactor.h:47-48` 那句"这个 0 是功能不是便利"）。S5-01 新增的占用：

| 端口 | 谁在用 | 备注 |
|---|---|---|
| 18520 / 18521 | `scripts/smoke-s5-01.sh` 的网关 / 假上游 | 脚本启动前逐端口探活，被占就 `die` 而不是带着它往下跑 |
| 18522 | 同一脚本的 `--metrics-port`；也是 `scripts/bench/s5-01-metrics-cost.sh` 的观测端口 | 避开上面那几条冒烟已经在用的 18500–18509 |
| 18183 / 18184 / 18199 | `scripts/bench/s5-07-qps-symmetric.sh`（#3 的对称档）：网关容器发布口 / nginx 容器发布口 / 原生上游 | 两侧都只走 `-p` 到 `127.0.0.1`——**这一档的全部意义就是让两侧多同一次端口发布**；上游绑 `0.0.0.0`，两个容器都从 bridge 出去打它 |
| 18188 / 18189 | `scripts/bench/s5-07-flamegraph.sh` 的被测网关与它的上游（火焰图档，perf 直接拉起被测进程） | 该档不做 SLO 判定，只产归因证据；nginx 分支另占 18198，但那一臂本轮**取不到**（原因见 4.2） |
| 18213 / 18214 | `scripts/bench/s5-11-disarmed-walk.sh`（由 `s5-11-walk-matrix.sh` 驱动）的业务端口与观测端口 | S5-11 那一格：off/on 两臂**逐字同参、只差 `--metrics-port` 的有无**，所以两个端口必须是固定的同一对，否则差值里就混进了端口差异。脚本启动前逐端口探活，被占就 fatal（`smoke-s5-05` 那族的同款门）。它不与编排同跑（那套占 18080/18522/18090/19090/13000），但**与 c100k 主档占的 18083 无关**，两条可以相邻跑 |

另外两条与端口有关、容易被当成可自由决定的事实：**观测面的绑定地址跟着 `--listen` 走**（缺省 `127.0.0.1`，容器里部署方写 `0.0.0.0` 时它才对外，S5-05 编排要按这条摆 `-p`），**`--metrics-port 0` 不是"关闭"**——`0` 在这台机器上已经是"内核给端口"（本轮实测：拿到 34387 并抓取成功），关闭的形态是"标志缺席"。

S5-05 的编排占的是**同一批端口**（18080/18522/18090/19090/13000，容器内部 18081/18082 不发布），
所以它与 `smoke-s5-01/02/03/05` 不能同时跑；那张表的读数与"谁占哪个口"写在 **5.3**，这里只留指路不留第二份真相。
`scripts/smoke-s5-05.sh` 阶段 0 逐端口探活，被占就 `die` 而不是带着它往下跑。

S5-07 的两条新脚本另开一条**容器对照带**，为的是能与上面所有东西同时存在、不抢编排的口径：

| 端口 | 谁在用 | 备注 |
|---|---|---|
| 18199 | `scripts/bench/s5-07-qps-symmetric.sh` 的原生上游（绑 `0.0.0.0`） | 两个代理容器都从这条打它；启动前逐端口探活 |
| 18183 / 18184 | 同一脚本里**网关容器 / nginx 容器**的发布端口（容器内 18080 / 80） | 两侧都只走 `-p` 到 `127.0.0.1`，这就是这一档"拓扑对称"的全部内容 |
| 18188 / 18189 | `scripts/bench/s5-07-flamegraph.sh` 的被测网关与它的上游 | 火焰图档：wrk 满负荷 60 s 期间由 perf 直接拉起被测进程 |
| 18198 | 同一脚本 `ARM=nginx` 那一分支的 nginx 发布端口 | 本轮该分支**取不到证据**，原因在 4.2；端口留着是因为拒跑也要占位才测得出通路 |

### 6.3 S5-12 的 netem 端口带与「必须独跑」（2026-11-05 加）

| 端口 | 谁用 | 为什么是这一段 |
|---|---|---|
| `18960` / `18961–18963` / `18965` | `scripts/fault/s5-12-arm.sh`：网关、三台上游、指标面 | 与 S5-06 的 `1895x` 错开，两条驱动同时跑或事后比对都能一眼分辨。臂内监听活在**每臂自己的 netns** 里，宿主上永远看不到这些端口——所以「宿主无 1896x 残留」那条门只证明没泄漏到宿主，不证明臂内干净 |

**两条硬规矩（本轮各撞过一次）**：① **netem 臂必须独跑**——点火与测量并发时 r1 后半（loss 轴）的计数被污染，而 delay 轴 75/80 这种边际点根本经不起 CPU 争用；之后所有批次改成「等前一批落 `END` 再开下一批」。② 上游子进程的命令行里**不带落盘路径**（只有 `--listen/--fixed-body`），按路径清理抓不到它们，中断后只能按 PID 收——本轮收了 4 个孤儿（三个 `1896x` 上游 + `smoke-s5-01` 留的 `18521`）。

### 6.2 `c100k.sh` 与 S5-11 的端口带（2026-11-05 加，S5-11 第一次撞出来的）

第 2 行那句"18520/18521/18522 被 `smoke-s5-01.sh` 占"之外，还有一格事实要写在同一张表旁边：
**`c100k.sh` 缺省占 `18083`，而 6.1 表里 `s2-qps-vs-nginx.sh` 那批也写 `1808x`**——本轮我先踩了
`FATAL: 端口 18083 已被占用（本机已有监听）`（上一条冒烟留下的 `l7gate_bench_upstream` 还挂在上面）。
S5-11 因此**另开一条带**：`18213 / 18214`（`s5-11-disarmed-walk.sh` 的业务端口与观测端口，矩阵串行跑必用同一对，
见 `scripts/bench/s5-11-walk-matrix.sh` 里 `GATEWAY_PORT/METRICS_PORT`）。三条要记住的：

- **一臂的失败会连锁**：`fatal` 那条路原来不收自己起的上游与网关，漏下一臂就占住 18213 **并吃掉约 28k 个临时端口**，
  于是下一臂的夹具连 `bind :0` 都失败（实测 `make_listen_socket errno=98`）。所以这条链上现在有两道门：
  一臂脚本自己的 `trap cleanup EXIT`（TERM → 20 秒不退才 KILL 并点名是哪一根），以及矩阵**起臂之前的清场门**
  （查到残留进程或端口仍被监听就中止整组，不带残留继续）。
- **容量顶是 28,232，取 n 要留余量**：`workers=1` 那一臂的回源容量 = `min(1, E) × 28,232`。我第一轮把 n 写成
  28,232 本身，结果 `established=28,231 / failed=1`——**贴天花板开就是必然失败**；正式档取 **28,000**。
- **CPU 取数的分辨率要先说**：`HZ=100` → 一格 10 ms。小拓扑（n=500/1,000）上 on−off 只有 0~2 格，
  这种档**只能判"在本机分辨率之下"，不能判"没有成本"**——B-023 的读数因此全部出自 28k 与 100k 两档。

## 7. 尚未确定的环境项

| 项 | 状态 | 定夺时机 |
|---|---|---|
| **100k 长连接的两个前置条件**（2026-10-27 实测，**推翻了我之前写在四处文档里的说法**） | **① fd 上限不是环境阻塞**：`ulimit -n` soft=**10240**、hard=**1048576**，实测 `bash -c 'ulimit -n 300000'` **当场成功**——抬 soft 不需要 root、不需要改机器配置，脚本里一行或进程自己 `setrlimit` 就够（`fs.file-max` = 9.2e18、systemd `DefaultLimitNOFILE`=1048576，都不卡）。**② 真卡住 100k 的是临时端口**：`ip_local_port_range` = 32768–60999 = **28,232 个源端口**，压测客户端对同一个（目标 IP，目标端口）最多开约 2.8 万条连接，**抬 fd 完全解决不了它**。要到 100k 只有两条路：多目标端口/多地址，或者改测量口径——两条都属测量方案设计。 | 之前四处写的"S2/S3 之前必须由环境侧解决 rlimit"是**错的**（把可自抬的 soft limit 当成了机器配置），已就地改成"①不是问题、②是"；②的方案随 S2 的 `c100k.sh` 定，届时按规矩补一条 ADR |
| 内存分配器（glibc / jemalloc / tcmalloc） | 目前默认 glibc malloc 2.43；契约 SLO #2 要求必须记录 | S1 首次记录 RSS 时定稿；换分配器属版本/依赖变更，先写 ADR |
| `cpuset` 绑核方案（Redis 与网关、wrk 的核分配） | **已定稿并实测生效（2026-11-02，ADR-05 追加节第七节 + BENCHMARK B-017）**：只给 Redis 容器绑核 `--cpuset-cpus 15`（引擎侧 `NCPU=16` 与发行版 `nproc=16` 同一视野），证据两处——`HostConfig.CpusetCpus=15` 且容器内 `/sys/fs/cgroup/cpuset.cpus.effective=15`；容器另加 `--save "" --appendonly no`（计数是易失状态，BGSAVE 会把 CPU 抖进尾延迟）。**网关与压测工具刻意保持未绑核**：契约 3.3 要的是"Redis 不许抢走网关正在用的核"，而把网关/wrk 钉到核上会让新读数与 B-008/B-012/B-013/B-016 这些未绑核的历史档不可比。**这条不是完整隔离**（网关仍可被调度到 15 号核），边界写在 B-017 第一节末尾 | S4-08 与 S5 任何带 Redis 的压测都按 B-017 那张表复述 CPU 分配。**编排层的落点（2026-11-04，S5-05）**：`deploy/compose/docker-compose.yml` 里 `cpuset: "15"` 只给 redis 那一段、全栈只此一条，同一判据由 `scripts/smoke-s5-05.sh` 阶段 1（文件里只有一条且落在 redis 段）+ 阶段 9（`HostConfig` 与容器内 `cpuset.cpus.effective` 两处生效、网关那台读回空）守着，阶段 0 还先断 `docker info` 的 `NCPU=16`——那个数不成立时"15 号核"这个写法本身就是假事实 |
| tc netem 在 6.18 WSL2 内核可用性 | **已量完（2026-11-04，S5-06）：通，但通的地方不是宿主回环**——四条路（A 宿主原生回环加不上＝权限墙不是内核墙、A′ `sch_netem.ko` 的 `vermagic` 与运行内核逐字相同、C 非特权 user+netns + `--map-root-user` 能加能生效＝S5-06 的执行场所、D 容器 `--cap-add=NET_ADMIN` 也通）与全部读数在 **5.4**；内核串就地更正为 `6.18.40.1-microsoft-standard-WSL2`，工具版本 `iproute2-6.19.0 / libbpf 1.6.3` | 已定稿，不再挂"未验证"。注入的作用域是**那一个 netns 的 `lo`**（宿主回环全程 `qdisc noqueue`，开工前后各核一次）；netem 实测读数在 FAULT-INJECTION **§25**，两条没打的退化与三格没测登记在同节，归 S5-08 处置 |
