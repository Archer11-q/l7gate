# l7gate

一个单机部署的 L7 流量治理网关：解析 HTTP/1.1 请求，按策略路由、限流、熔断后转发给上游，并对整个过程给出**可复现**的性能与故障边界数据。

不是"又一个高性能 Web 服务器"，不是框架，也不是功能清单。对外形态是一个可以 `docker compose up` 起来、替换 Nginx 位置的网关。

## 现在的状态

**S1 前三步（构建骨架、动态缓冲、HTTP/1.1 解析器）已完成，网关本体还没有。** 我不在这段含糊其辞：仓库当前能构建、能跑测试、能自述编译期环境事实，`net/Buffer` 带高水位停读语义并且测到了单连接峰值内存的口径，`http/request_parser` 能对 37 类畸形输入返回定义好的错误码——但两个部件还没有被 epoll 事件循环接起来，一个请求也转发不了。下面是每个数字的实际出处，没有的一律标"未测"。

| 项目 | 状态 |
|---|---|
| 动态缓冲 `net/Buffer` | 已实现（S1-02）：连续、带高水位、`read()` 到 `EAGAIN` 才算一轮结束；默认配置下 1 KiB keep-alive × 1000 轮实测 `peak_capacity=4096` |
| HTTP/1.1 解析器 `http/request_parser` | 已实现（S1-03）：纯函数、不碰 fd，CL 与 chunked 两种 body，22 个错误码；`release -O3` 下 595 MB/s / 5.5M req/s（单线程、无 syscall，**归因用，不是 SLO**） |
| HTTP/1.1 转发 | 未实现（S1-04 / S1-06） |
| 尾延迟 p99 | **未测**（S1 门禁交付第一份） |
| QPS vs Nginx | **未测**（且 Docker 尚未接入，基线起不来） |
| 100k 长连接每连接内存 | **未测**（S2 之后；当前只有 buffer 层的容量口径，不等于每连接内存） |
| 限流 / 熔断 / 路由 / 热重载 | 未实现（S3–S5） |

## 三个数字

项目的全部价值锚点是这三个数，测量条件写死在 [docs/SLO.md](docs/SLO.md)：

1. **转发尾延迟**——网关自身引入的增量 p99 ≤ 1.0 ms，p999 ≤ 5.0 ms（同机回环、1 KB 响应体、keep-alive、1000 并发、稳态 60 s）
2. **长连接资源占用**——100k WebSocket 长连接稳态，每连接 ≤ 20 KB，空闲 CPU < 5%
3. **吞吐**——QPS ≥ **Nginx 同环境实测值的 60%**（16 vCPU、1 KB keep-alive 回显）

第三个数还有附加产出：与 Nginx 的差值必须定位到 HTTP 解析、内存分配、连接管理、日志写盘、系统调用次数中的哪几项。「差多少」谁都能测，**「差在哪」才是这个项目**。

关于性能的一句实话：我选 epoll **边缘触发**，但 ET 并不比 LT "更先进"，两者性能差别极小。选它的真实理由是它逼出正确的 Buffer 设计——一次必须读到 `EAGAIN`（[ADR-02](docs/adr/ADR-02-epoll-et-io-uring-deferred.md)）。

这句"逼出"现在有实物支撑了：`net/Buffer` 的扩容策略我改了两版，两版都是被内存用例打回来的——固定 64 KiB 申请把每连接容量下限抬到 128 KiB，改成按 hint 预留仍然抬到 8 KiB，而 SLO #2 的预算是 20 KB。细节与最终口径在 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) 第 1.3 节。

## 与 Nginx 的功能差异（不是性能差异）

在流量治理这个维度上，本项目功能不比 Nginx 开源版少：

- Nginx OSS 只有被动的 `max_fails` / `fail_timeout`；**主动健康检查是 Nginx Plus 才有的功能**
- Nginx **没有内置熔断器**
- 熔断带**最多摘除比例安全阀**（默认 10%，对齐 Envoy 的 `max_ejection_percent`）——判据失准时最坏只损失 10% 容量，防止熔断本身变成雪崩放大器（[ADR-12](docs/adr/ADR-12-circuit-breaker-ejection-cap.md)）

这两件事不能混着说：**我比 Nginx 多的不是速度，是治理策略**；吞吐基线仍然是 Nginx，而且目标是它的 60%，不是超过它。

## 架构

多线程 Reactor（one loop per thread）+ epoll ET + 配置驱动的 filter 链。现状模块与实际实现程度见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)——那份文档严格区分"已实现"和"已定但未实现"。

数据流（目标形态；当前 `Buffer` 与解析器各自被单测直接驱动，还没被事件循环串起来，可执行文件里唯一的真实路径仍是 `--self-check`）：

```
client ──epoll ET──▶ Reactor ──▶ Buffer ──▶ HTTP/1.1 parser
                                             │
                                     filter chain（配置决定顺序）
                                             │
                              路由 ──▶ 负载均衡 ──▶ 上游连接池
                                             │
                                     熔断 / 限流（共享状态）
```

网络层全部在本仓库内自行实现，不引入 asio / muduo / brpc（[ADR-04](docs/adr/ADR-04-no-third-party-network-lib.md)）——"网络层怎么写的"就是这个项目要交付的东西。

## 快速开始

环境要求见 [docs/ENVIRONMENT.md](docs/ENVIRONMENT.md)。**构建与压测必须在 Linux/WSL2 原生文件系统内进行**，放在 Windows 挂载盘（`/mnt/d` 之类）上会让 9P 的 IO 抖动直接进 p99，测出来的数既不属于网关也不属于 Nginx。

```bash
cmake --preset debug          # GCC 15.2，日常开发
cmake --build --preset debug
ctest --preset debug          # 当前 36/36（6 工具链 + 9 buffer + 21 解析器）

./build/debug/l7gate --self-check
# version=0.1.0 compiler=gcc 15.2.0 __cplusplus=202002 gtest=1.14.0 \
#   sanitizers=none build_type=Debug malloc=glibc-malloc
```

三个构建形态与各自的用途：

| preset | 编译器 | 用途 |
|---|---|---|
| `debug` | GCC | 日常开发 |
| `release` | GCC | **压测唯一允许**——sanitizer 构建测出来的是检查开销的延迟 |
| `asan-ubsan` | Clang | S1 门禁（`-O0` 是刻意的，见下） |
| `tsan` | Clang | S2 门禁：连接对象跨线程传递与异步销毁的唯一自动化验证手段 |

> `debug` / `release` 之外的一切数字都不进 `BENCHMARK.md`；sanitizer preset 一律用 `-O0`，因为在 `-O1` 起，"可被证明无副作用的坏写入"会被优化掉，ASan 什么都不报——那种"干净"是假的。

任务收尾时用 `bash scripts/verify-all-presets.sh` 一次性拿到三个 preset 的构建退出码、警告计数与 `ctest` 结果——「ASan 干净」这句话不该靠我手敲，而该有一条命令能复现。

## 明确不做的东西

每条都有 ADR 写明理由，不装作它们不存在：HTTP/2 与 gRPC 与 QUIC、TLS 握手、第三方网络库、etcd/xDS/K8s/Service Mesh、动态服务发现、**响应缓存**（缓存是另一种网关形态，做了它 p99 就不再度量流量治理）、WebSocket 帧解析（只做 Upgrade 后透传）、多进程 `SO_REUSEPORT`、Web 管理后台、自研压测客户端（除 C100K 长连接场景）。

还有一个反向承诺：**io_uring、连接对象池、日志异步化全部挂起**。只有当压测 + perf 火焰图拿出证据证明那是真实瓶颈时才立项，且先写 ADR。在没有证据的情况下优化，是空壳项目的典型形态。

## 文档

| 文档 | 内容 |
|---|---|
| [CLAUDE.md](CLAUDE.md) | 唯一契约：身份约定、边界、SLO、决策、门禁 |
| [TASKS.md](TASKS.md) | 任务表（唯一真相源）：ID / 可断言验收标准 / 状态 / 实测数字 |
| [docs/SLO.md](docs/SLO.md) | 三个数字的目标与写死的测量条件 |
| [docs/ENVIRONMENT.md](docs/ENVIRONMENT.md) | 环境自检实测版本、版本偏离的理由、sanitizer 阳性验证 |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | 已实现的现状 |
| [docs/adr/](docs/adr/) | 13 条决策记录 |

`docs/BENCHMARK.md`、`docs/FAULT-INJECTION.md`、`docs/ROADMAP.md` 等有数据之后再建——空的证据文件比没有更糟。

## 许可

MIT。见 [LICENSE](LICENSE)。
