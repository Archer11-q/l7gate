# ARCHITECTURE —— 已实现的现状

> 契约第 6 节第 2 步要求这里写的是**已实现的现状，不是规划**。所以我用两个明确分开的部分：第 1 节是仓库里真实存在、能构建能跑的东西；第 3 节是已定但未实现的结构意图，只到「接口形状」这一层，避免把愿望写成事实。
>
> 现状一句话：**仓库里还没有网关，但有了它的第一个部件。** 构建骨架、编译期事实查询、以及一个带高水位的动态缓冲 `net/Buffer`。转发路径、epoll、解析器、filter 链、路由、限流、熔断全部未实现。

## 1. 当前真实存在的模块

```
l7gate/
├── CMakeLists.txt          构建契约：C++20、警告集、sanitizer 组合、依赖锁版本
├── CMakePresets.json       四个构建形态（debug / release / asan-ubsan / tsan）
├── scripts/
│   └── verify-all-presets.sh  三 preset 全量构建 + ctest + 警告计数，任务收尾用
├── src/
│   ├── main.cc             入口：--self-check / --help，其余路径明确返回"未实现"
│   ├── core/
│   │   ├── build_info.h    BuildInfo 结构 + current() + render_one_line()
│   │   └── build_info.cc   编译期与运行期事实的采集实现
│   └── net/
│       ├── buffer.h        Buffer + Config + ReadResult：接口、不变量、口径都写在头文件
│       └── buffer.cc       读满即停 / 压缩 / 扩容 / 水位停止 / 空仓归还
└── tests/
    ├── CMakeLists.txt      gtest_discover_tests，标签按阶段打（s1），任务级筛选靠套件名
    ├── build_info_test.cc  6 条工具链断言（S1_01_Toolchain）
    └── buffer_test.cc      9 条缓冲断言（S1_02_Buffer），含 10000 次随机读写与 1 KiB keep-alive 稳态
```

### 1.1 `core/build_info`

唯一的功能模块。它存在的原因不是"要有个模块"，而是**压测报告的环境字段不能靠人肉抄录**：契约第 7 节矩阵要求任何一次压测都在 `BENCHMARK.md` 里留下「CPU 绑核、分配器、各组件版本号」，而抄录终有一天抄错。所以把这些事实做成二进制自述的一部分，`l7gate --self-check` 输出单行 `key=value`，脚本直接 grep 落盘。

采集口径：

| 字段 | 来源 | 为什么是编译期而不是外部命令 |
|---|---|---|
| `compiler` | `__clang_version__` / `__VERSION__` | 报告里写 `docker exec ... gcc --version` 会与实际编译所用的编译器无关，preset 切换就失真 |
| `__cplusplus` | `__cplusplus` 宏 | 契约锁 C++20；`CMAKE_CXX_EXTENSIONS OFF` 下标准值必须是 `202002` |
| `gtest` | 配置期从拉下来的源码解析，再以宏注入 | 见第 2.2 节，防止 apt 的 1.17.0 悄悄替换契约锁的 1.14.0 |
| `sanitizers` | `__has_feature(...)` + `__SANITIZE_*__` 双路探测 | 见 1.2 |
| `malloc` | `__GLIBC__` 判定，当前为 `glibc-malloc` | **SLO #2 的每连接内存直接由分配器决定**（契约第 2 节明写），留空等于指标定义不完整 |
| `build_type` | `CMAKE_BUILD_TYPE` 注入 | 空值意味着 preset 没起作用 |

### 1.2 为什么 sanitizer 状态要正面断言

`tests/build_info_test.cc` 里的 `RequestedSanitizersAreActuallyCompiledIn` 是这一层唯一有真实防御价值的测试，逻辑是：配置期把「我要求开哪些 sanitizer」以 `L7GATE_SANITIZERS_EXPECTED` 宏注入，运行期用编译宏探测实际生效的组合，两者必须一致。

它防的失效场景很具体：`-fsanitize` 被某个 preset 写错、或被上层 `CMAKE_CXX_FLAGS` 覆盖掉。**这时二进制照样能构建、测试照样全绿，而「ASan 干净」这句门禁结论已经作废。** 一个静默失效的 sanitizer 比没有 sanitizer 更危险。

顺带锁了一条编译期不可能出现的组合：`address` 与 `thread` 同时存在（两者互斥，链接会失败，但如果构建配置错乱到这种状态，测试要指名道姓报出来）。

### 1.3 `net/Buffer`：ET 逼出来的部件，也是每连接内存的第一处口径

它是 ADR-02 的直接产物：edge-triggered 要求一次 `read()` 循环跑到 `EAGAIN` 才算处理完本轮可读事件，于是必须有个地方存"读到了但还没被解析掉的字节"。buffer 是这条语义的必需品，不是优化项。

接口形状：`Config{initial_capacity=4 KiB, max_capacity=1 MiB, high_water_mark=256 KiB}`；`read_until_blocked(fd)` 返回 `ReadResult{bytes_read, read_syscalls, would_block, peer_closed, limit_reached, errno_value}`；写入侧走 `writable_region(hint)` + `commit_written(n)`，和"从 fd 读完再提交"是同一条代码路径，所以测试用真管道而不是 mock 就能覆盖到生产路径。

三条我认为值得单独写下来的决定，都是被自己的测试逼出来的，不是设计时想到的：

**一、`hint` 只代表"本轮至多想读这么多"，不代表预留。** 扩容只在 `writable_bytes() == 0`（真满）时发生。这一条改了两版才对：第一版单次申请固定 64 KiB，直接把每连接容量下限抬到 128 KiB —— 那是 SLO #2「每连接 ≤ 20 KB」预算的六倍；第二版按 hint 预留空间，1 KiB 的 keep-alive 报文照样把容量顶到 8 KiB。现在 `PeakCapacityUnderOneKiBKeepAliveWorkload` 实测 **peak_capacity = 4096**，与 `initial_capacity` 相等，一次扩容都没发生。读申请量本身也做成自适应（`read_hint_` 只在一次 read 把申请量**读满**时翻倍，上限 64 KiB），慢连接不会替我垫付内存。

**二、高水位是事件，不是错误。** `limit_reached` 与 `would_block`、`peer_closed` 三者互斥可断言，且 `limit_reached` 的含义是"我在内核之前就停了"——这正是 S1-07 能在水位回落后恢复读取的前提；如果停的原因是 EAGAIN，ET 下就没有新事件可等，那才是真卡死。为了让这条语义可依赖，单次 read 的申请量被夹在"距水位还剩多少"之内（第一版没夹，实测 hwm=4096 时一次读进 8192）。于是有一条硬不变量：**`read_until_blocked()` 返回时 `readable_bytes() <= high_water_mark`**。

**三、空仓时把容量还回去。** 读空且容量超过 2× `initial_capacity` 就缩回初始值。阈值取 2 倍是为了不在 1 倍上下抖 realloc。这条的动机很直接：一条传过大 body 的连接如果不归还，那块内存就终身占着——而对 100k 长连接来说，"终身"就是稳态本身。

实测数字（`TASKS.md` S1-02 行有同一份）：默认配置 1 KiB keep-alive × 1000 轮 → initial/peak/final 全部 4096，1000/1000 轮的 `read_syscalls > 1`；数据完整性用刻意放宽限制线（64 MiB）的专用配置跑 10000 次随机读写 → written=9,049,376 / read=5,048,397 / peak_capacity=4,194,304，逐字节一致。后者**不能**拿去当 SLO #2 的每连接内存——它描述的是 buffer 自己的形状上限，不是真实报文下的稳态。

## 2. 构建契约（这几条是有理由的，不是风格）

- **C++20 + `CMAKE_CXX_EXTENSIONS OFF`**：我要标准 C++20，不是 GNU 扩展下的"看起来能用"。
- **生成器是 Unix Makefiles，不是 Ninja**：Ninja 不在契约 9.1 锁定表里，而 8.2 禁令三对"引入"这件事不分构建工具还是库。ADR-04 的自研边界只覆盖网络层，我没给自己开构建工具的口子。
- **`-Werror` 默认 OFF**：clang 21 是新版本，把新增警告直接升级成构建失败会让工具链升级变成阻塞项。
- **警告集是显式列举的**（`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wnon-virtual-dtor -Wcast-align -Wunused -Woverloaded-virtual`）。`-Wconversion`/`-Wsign-conversion` 对网关这类大量做长度与偏移运算的代码是有效防线；`net/buffer.cc` 全是这类运算（`size_t` 偏移、`ssize_t` 返回值、容量夹取），实测我方警告仍为 0 条——这条警告的代价目前看是划算的，真正的解析代码写出来后再检验一次。
- **sanitizer 用字符串而不是逐个开关**（`L7GATE_SANITIZERS="address,undefined"`）：S1 门禁要求的是**组合**，而 address 与 thread 互斥，用一堆 bool option 表达互斥关系比用字符串更绕且更容易配错。
- **sanitizer preset 强制 Debug（`-O0 -g`）**：实测发现 `-O1` 起会把"可被证明无副作用的坏写入"优化掉，ASan 于是什么都不报（详见 `ENVIRONMENT.md` 2.2）。所以在优化构建上跑 sanitizer 得出的"干净"结论是假的。
- **`release` preset 是 `BENCHMARK.md` 唯一允许的构建**：sanitizer 构建测出来的尾延迟是检查开销的延迟，不是网关的延迟。

## 3. 已定但未实现的结构意图

只写到接口形状，实现归属看 `TASKS.md`。

- **`http/request_parser`（S1-03）**：**纯函数，不碰 fd**。这条约束是刻意的：解析器的半包/粘包与畸形输入用例（12 类）必须能在不建立任何 socket 的情况下跑，否则这些用例会被事件循环的时序牵着，测不出解析本身。它消费的正是 1.3 那个 `Buffer` 的可读区。
- **`net/reactor`（S1-04 → S2 多线程化）**：单线程 epoll ET 起步，S2 才加线程。连接对象的销毁路径从 S1 就按"可能被别的线程持有"设计，避免 S2 返工——这是 ADR-03 那笔「线程共享地址空间、代价是并发正确性」的具体支出。**S1-07 的背压钩子已经在 buffer 侧就位**（`ReadResult::limit_reached` + 「返回时 `readable_bytes() <= high_water_mark`」这条不变量），还没实现的是 reactor 侧：收到 `limit_reached` 就摘掉该 fd 的 EPOLLIN、等下游写完再把读事件加回来。
- **`filter/chain`（S1-08）**：空壳先立。验收标准是「新增 filter 不修改转发主路径的任何一行代码」，所以现在还没有主路径可改——顺序与接口形状在还没有功能诱惑的时候定下来，是这个任务唯一的目的。

## 4. 已知噪声与待补项

| 项 | 现状 | 影响 |
|---|---|---|
| clang 21 对 `gtest-printers.h` 报 `-Wcharacter-conversion` | 已把 gtest 头按 `-isystem` 处理并**在 flags.make 里确认生效**，警告仍然逃逸——clang 21 这条新警告不受系统头抑制 | 每个 clang preset 固定 1 条第三方噪声。我**不加** `-Wno-character-conversion`：那会同时关掉我自己代码里的同一警告，用消噪换 blindness 不划算 |
| ASan 堆栈无符号化 | 缺 `llvm-21-tools`（提供 `llvm-symbolizer`），报告退化成裸地址 + BuildId | 门禁要求"ASan 干净"不受影响；一旦真出问题，定位要手动 addr2line。已记 `ENVIRONMENT.md` 2.2 |
| `build/` 下每个 preset 各自下载一次 googletest | 三个 preset 三次 FetchContent | 构建耗时 15–20s 里含下载。可接受；共享 `_deps` 会引入 preset 间状态耦合，不值得 |
