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
| Docker CLI | 必需 | **可用且基线镜像已就位**：Docker 29.4.3（build 055a478）、Docker Compose v5.1.3、引擎侧 Server 29.4.3 / storage driver `overlayfs` / `NCPU=16` / `MemTotal=12407373824`（与本机 `nproc=16`、`MemTotal=12116576 kB` 同一台机器同一上限，**不是另起一台小 VM**）；`nginx:1.25.4` 已 pull 成功，id/digest `sha256:9ff236ed47fe39cf1f0acf349d0e5137f8b8a6fd0b46e5117a401010e56222e1`，容器内 `nginx -v` = **nginx/1.25.4**（契约 9.1 锁定的 tag 字面命中） | ✅ 已解锁（2026-10-26 由用户换掉失效 mirror 之后） |
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

## 3. 唯一实质偏离：libmysqlclient

契约原本锁 `libmysqlclient 8.0.x`，但 Ubuntu 26.04 的 apt 源里只有 8.4.11，8.0 既无源、在 Oracle 侧也已停止支持。我按 26.04 的现实把锁定值改为 8.4.x，理由和边界都写在这里，不装作它没发生：

- 服务端镜像仍锁 `mysql:8.0.36`（未改）。8.4 客户端连 8.0 服务端在 Oracle 的支持矩阵内是允许的方向（新客户端连旧服务端）。
- **这条兼容性目前只是推断，不是事实。** 要到 S3 第一次真正建立 MySQL 连接时实测取证，写进 `FAULT-INJECTION.md` 或首次连通记录里才算成立。
- 客户端 8.4 / 服务端 8.0 是否一并对齐到 8.4，留到 S3 触到 MySQL 时决策，现在不动镜像 tag——镜像 tag 一旦中途更换，历史数据就断代。

## 4. 待验证风险：perf 能否真正采样

`perf 7.0.14` 已安装，但运行内核是 `6.18.40.1-microsoft-standard-WSL2`，两者版本不匹配。WSL2 的定制内核历来对 `perf_event_open` 支持不全，而 SLO 附加产出（「与 Nginx 的差值必须定位到具体原因」）明确要求用 perf 火焰图作证。

- 这项我没有验证——S1/S2 还用不到它，但**绝不能到 S5 才发现用不了**。
- 计划：S4 期间跑一次 `perf stat -a sleep 1` 与最小火焰图冒烟，结果补进本文件。如果不通，备选是微软官方 WSL 内核的 perf 支持路径，或退到 `async-profiler`/eBPF 之外的用户态统计手段（届时按 8.2 禁令先写 ADR）。

## 5. Docker Desktop WSL Integration（2026-10-26 已开启，但镜像源仍阻塞 SLO #3）

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

于是 **#3 的「同机、同参数」在网络这一维上做不齐**：nginx 的客户端 leg 多一次端口发布（DNAT/userland proxy）、上游 leg 多一次容器出向，而网关两侧都是原生回环。方向上这是**拖累 nginx**，所以拿它算出来的 `QPS_网关/QPS_nginx` 对**我**偏乐观——B-005 的比值和结论都带着这条声明，#3 是否算"达标"由项目所有人裁，我不给自己发这张证明。另外两条同场记下的不齐项：nginx 容器用的是它自己镜像的 libc/分配器（契约要求「同一分配器记录」，这一条没满足）；镜像默认 `worker_processes auto` = 16 worker，而 S1 按 ADR-03 就是单 Reactor（多线程是 S2 的内容）——所以 B-005 额外跑了「关 access_log」与「单 worker」两个**只差一个变量的对照组**，用来把"日志写盘"和"并发分摊"这两维从差值里拆出来。

## 6. 文件系统位置（会影响数据的一条）

仓库与全部构建、压测都在 WSL 原生 ext4 内。Windows 侧只能经 UNC 通道（`\\wsl.localhost\<distro>\<home>\l7gate` 形式）查看和编辑同一批文件。

把构建或压测放到 `/mnt/c`、`/mnt/d` 这类 drvfs/9P 挂载点是禁止的：9P 的 IO 抖动会直接进 p99/p999，测出来的数既不属于网关也不属于 Nginx。详细理由记在契约第 9.4 节。

## 7. 尚未确定的环境项

| 项 | 状态 | 定夺时机 |
|---|---|---|
| 内存分配器（glibc / jemalloc / tcmalloc） | 目前默认 glibc malloc 2.43；契约 SLO #2 要求必须记录 | S1 首次记录 RSS 时定稿；换分配器属版本/依赖变更，先写 ADR |
| `cpuset` 绑核方案（Redis 与网关、wrk 的核分配） | 未定 | S4 引入 Redis 限流时，方案写进 `BENCHMARK.md` |
| tc netem 在 6.18 WSL2 内核可用性 | 未测（契约 9.3 已预期可能不支持） | 仅作交叉验证，不阻塞主路径 |
