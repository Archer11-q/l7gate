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
| Clang | ≥ 14 | clang 21.1.8（dpkg `1:21.1.6-71`）——**已装，但 sanitizer 运行时缺文件** | ⚠️ 见第 2.1 节 |
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
| Docker CLI | 必需 | **容器不可用**：WSL Integration 未开启 | ❌ 阻塞项（非 S1） |
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

**开工前置条件状态：仍未满足。** S1 门禁要求「ASan/UBSan 干净」、S2 门禁要求「TSan 干净」，两者在当前机器上都无法执行，所以 S1 代码仍未启动。

## 3. 唯一实质偏离：libmysqlclient

契约原本锁 `libmysqlclient 8.0.x`，但 Ubuntu 26.04 的 apt 源里只有 8.4.11，8.0 既无源、在 Oracle 侧也已停止支持。我按 26.04 的现实把锁定值改为 8.4.x，理由和边界都写在这里，不装作它没发生：

- 服务端镜像仍锁 `mysql:8.0.36`（未改）。8.4 客户端连 8.0 服务端在 Oracle 的支持矩阵内是允许的方向（新客户端连旧服务端）。
- **这条兼容性目前只是推断，不是事实。** 要到 S3 第一次真正建立 MySQL 连接时实测取证，写进 `FAULT-INJECTION.md` 或首次连通记录里才算成立。
- 客户端 8.4 / 服务端 8.0 是否一并对齐到 8.4，留到 S3 触到 MySQL 时决策，现在不动镜像 tag——镜像 tag 一旦中途更换，历史数据就断代。

## 4. 待验证风险：perf 能否真正采样

`perf 7.0.14` 已安装，但运行内核是 `6.18.40.1-microsoft-standard-WSL2`，两者版本不匹配。WSL2 的定制内核历来对 `perf_event_open` 支持不全，而 SLO 附加产出（「与 Nginx 的差值必须定位到具体原因」）明确要求用 perf 火焰图作证。

- 这项我没有验证——S1/S2 还用不到它，但**绝不能到 S5 才发现用不了**。
- 计划：S4 期间跑一次 `perf stat -a sleep 1` 与最小火焰图冒烟，结果补进本文件。如果不通，备选是微软官方 WSL 内核的 perf 支持路径，或退到 `async-profiler`/eBPF 之外的用户态统计手段（届时按 8.2 禁令先写 ADR）。

## 5. Docker Desktop WSL Integration

Redis / MySQL / Prometheus / Grafana / Nginx 基线全部走 Docker（契约 9.3），但 `docker` 在 Ubuntu 发行版内报的是「could not be found in this WSL 2 distro, activate the WSL integration」。修复要在 Docker Desktop 图形界面里做：`Settings → Resources → WSL Integration → 勾选 Ubuntu`。

这一步影响的是 S3 之后的组件依赖，S1（单连接转发 + 第一份 p99）不阻塞。开启后本文件第 2 节 Docker 行需回填 `docker --version` 与 `docker compose version` 实测值。

## 6. 文件系统位置（会影响数据的一条）

仓库与全部构建、压测都在 WSL 原生 ext4 内。Windows 侧只能经 UNC 通道（`\\wsl.localhost\<distro>\<home>\l7gate` 形式）查看和编辑同一批文件。

把构建或压测放到 `/mnt/c`、`/mnt/d` 这类 drvfs/9P 挂载点是禁止的：9P 的 IO 抖动会直接进 p99/p999，测出来的数既不属于网关也不属于 Nginx。详细理由记在契约第 9.4 节。

## 7. 尚未确定的环境项

| 项 | 状态 | 定夺时机 |
|---|---|---|
| 内存分配器（glibc / jemalloc / tcmalloc） | 目前默认 glibc malloc 2.43；契约 SLO #2 要求必须记录 | S1 首次记录 RSS 时定稿；换分配器属版本/依赖变更，先写 ADR |
| `cpuset` 绑核方案（Redis 与网关、wrk 的核分配） | 未定 | S4 引入 Redis 限流时，方案写进 `BENCHMARK.md` |
| tc netem 在 6.18 WSL2 内核可用性 | 未测（契约 9.3 已预期可能不支持） | 仅作交叉验证，不阻塞主路径 |
