# ARCHITECTURE —— 已实现的现状

> 契约第 6 节第 2 步要求这里写的是**已实现的现状，不是规划**。所以我用两个明确分开的部分：第 1 节是仓库里真实存在、能构建能跑的东西；第 3 节是已定但未实现的结构意图，只到「接口形状」这一层，避免把愿望写成事实。
>
> 现状一句话：**仓库里有了一个能转发请求的网关骨架，但它还不认识 HTTP 的语义。** `l7gate --serve` 起一个单线程 epoll 边缘触发循环，下游收到的完整请求按**原始字节**搬到上游、上游的响应按原始字节搬回来，keep-alive 复用同一条回源连接——10000 个真 curl 请求跑完 fd 回到基线、存活连接归零。它不做状态码映射、不改写响应、不解析 Upgrade、畸形请求只关连接不回 400，这些全部是 S1-06/S1-05 的活。filter 链、路由、负载均衡、限流、熔断、多线程、连接池仍然未实现。

## 1. 当前真实存在的模块

```
l7gate/
├── CMakeLists.txt          构建契约：C++20、警告集、sanitizer 组合、依赖锁版本
├── CMakePresets.json       四个构建形态（debug / release / asan-ubsan / tsan）
├── scripts/
│   ├── verify-all-presets.sh  三 preset 全量构建 + ctest + 警告计数，任务收尾用
│   └── smoke-s1-04.sh      真 curl 冒烟：单请求逐字节可见 + 10000 请求后 fd/连接归零
├── src/
│   ├── main.cc             入口：--self-check / --serve / --help
│   ├── core/
│   │   ├── build_info.h    BuildInfo 结构 + current() + render_one_line()
│   │   └── build_info.cc   编译期与运行期事实的采集实现
│   ├── net/
│   │   ├── fd.h/.cc        UniqueFd：fd 的 RAII。"漏 fd" 从这里开始可被类型系统抓住
│   │   ├── socket.h/.cc    非阻塞/CLOEXEC/listen/异步 connect/SO_ERROR，不含读写
│   │   ├── epoll.h/.cc     Epoll：add/modify/remove/wait，**无条件或进 EPOLLET**，自带计数器
│   │   ├── stats.h         ReactorStats：全原子的观测面（「观测不是同步手段」）
│   │   ├── buffer.h        Buffer + Config + ReadResult：接口、不变量、口径都写在头文件
│   │   ├── buffer.cc       读满即停 / 压缩 / 扩容 / 水位停止 / 空仓归还 / 写到 EAGAIN
│   │   ├── connection.h    Connection：一条 fd + 入向/出向 Buffer + 解析游标 + view 生命周期
│   │   ├── connection.cc   已扫描游标推进 / 惰性分配 / generation 作废判定 / 收尾释放
│   │   ├── reactor.h       Reactor + Config：六步关闭、ET accept、延后析构的接口与理由
│   │   └── reactor.cc      事件循环、上下游配对、双向 pump、背压挂起点的形状
│   └── http/
│       ├── request_parser.h    三态返回 + 22 个错误码 + ParsedRequest 的字段口径
│       └── request_parser.cc   请求行 / 头部区 / CL 与 chunked 框架的校验与定位
└── tests/
    ├── CMakeLists.txt      gtest_discover_tests：标签按阶段打（s1），任务级筛选靠套件名，TIMEOUT 30
    ├── pipe_fixture.h      非阻塞管道夹具，S1-02 与 S1-03 共用
    ├── test_upstream.h     仓库内极简 HTTP 上游：单测夹具，也是冒烟脚本的服务端
    ├── test_upstream_main.cc  上面那个类的独立可执行版（l7gate_test_upstream）
    ├── build_info_test.cc  6 条工具链断言（S1_01_Toolchain）
    ├── buffer_test.cc      9 条缓冲断言（S1_02_Buffer），含 10000 次随机读写与 1 KiB keep-alive 稳态
    ├── request_parser_test.cc  21 条解析断言（S1_03_Parser），含 37 类畸形输入与 40000 例随机字节
    ├── connection_test.cc  11 条（S1_04_Connection）+ 3 条写入侧（S1_04_BufferWrite）
    └── reactor_test.cc     10 条（S1_04_Reactor），含真 curl 之外的 10000 请求生命周期用例
```

套件计数是 `ctest --test-dir build/debug -N` 数出来的，不是我从文件里点出来的：**6 工具链 + 9 Buffer + 21 解析 + 3 Buffer 写入侧 + 11 连接 + 10 事件循环 = 60**。S1-04 交出去 24 条。

### 1.1 `core/build_info`

S1-04 之前它是仓库里唯一的功能模块，现在 `net/` 已经把它比下去了；它留下来的理由仍然是那一条，而不是"要有个模块"：**压测报告的环境字段不能靠人肉抄录**——契约第 7 节矩阵要求任何一次压测都在 `BENCHMARK.md` 里留下「CPU 绑核、分配器、各组件版本号」，而抄录终有一天抄错。所以把这些事实做成二进制自述的一部分，`l7gate --self-check` 输出单行 `key=value`，脚本直接 grep 落盘。

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

**S1-04 给它补了两处，都不是重构而是缺了就无法转发。** 一是写出侧的对称件 `write_to_fd(fd)`：把可读区一直写到 `EAGAIN` 或写空，边写边消费，返回 `WriteResult{bytes_written, write_syscalls, would_block, peer_closed, errno_value}`。它用 `::send(fd, ..., MSG_NOSIGNAL)` 而不是 `::write`，这条**不是风格**——对端已关时 `write()` 会发 SIGPIPE，默认动作是杀死进程；一个网关在客户端中途断连时把自己打死，而压测里客户端断连是每天几百次的常态事件。我选 `MSG_NOSIGNAL` 而不是进程级忽略 SIGPIPE，因为后者把"信号"变成整个进程的隐式全局状态，测试里谁都能改，而前者作用域只有这一次调用。`EPIPE`/`ENOTCONN` 归一成 `peer_closed`：那是"对端不在了"，不是"我写坏了"，S1-06 要按这个分别决定回 5xx 还是静默断开。

二是 `generation()`。凡是会让已发出的指针作废的改动（消费、memmove 压缩、容量归还、clear）都自增它。`ParsedRequest` 里全是 `string_view`，指向的就是这块内存，而 reactor 每次读都可能触发压缩——**作废这件事必须可查询，不能只写在注释里等我记得**。S1-03 那版靠"调用顺序规范"约束（解析→取用→再 consume），S1-04 把它变成一个能被断言的数：`GenerationOnlyMovesWhenViewsActuallyDie` 要求**纯追加读入不改代次**（读位置没动、地址没换，view 还有效），而 `consume` 与 `clear` 必须改。追加不该作废是有讲究的——事件循环每读一段就作废一次的话，一条还没被取用的请求会在下一次可读事件里变成"从没解析过"，重解析就变成常态而不是例外。

配套加了 `high_water_mark()` getter。理由和 1.4 第四条同源：出向背压要问"这一整块还装得下吗"，如果调用方自己复制一份配置里的限制，同一个限制就有了第二个真相来源。

### 1.4 `http/request_parser`：校验 + 定位，不重写

签名只有一个：`parse_request(data, len, scratch, config, out) -> {status, error}`。不发 syscall、不碰 fd、不持有跨调用状态。三态是 `kOk` / `kNeedMoreData` / `kError`，22 个错误码各有 `to_string`（`ErrorCodesHaveUniqueNames` 断言无重名无漏名，因为 S1-06 要按它们映射 400/411/413/501）。

四件事值得写下来：

**一、半包永远不是错误，这条被我写成了一条全称断言。** `EveryPrefixOfAValidRequestIsNeedMoreOrOk` 把四条合法请求各自截断到**每一个**长度逐个喂：结果只能是 `kOk` 或 `kNeedMoreData`。逐条列举半包形态只能覆盖我想得到的形态，而"把截断判成错误"的后果是网关拿正常慢客户端当攻击处理、回 400 断连——全称断言一次封掉这一整类。同一条思路用在畸形输入侧：37 类样本（契约要求 ≥12）必须返回**指定**错误码，且 `MalformedVerdictsAreStableAcrossReuse` 要求同一字节重复 20 次判定不变，防的是"靠 scratch 残留状态才恰好判对"。

**二、我留着 raw 字节，不解码 chunked。** `declared_body_bytes` 是解码后的长度，`consumed` 是含框架的原始长度，`raw_body` 只在 CL 场景给出。这两个数分开，S1-06 才能既逐字节透传又知道真实体量。重写框架的代价是"与直连上游逐字节一致"这条验收永远证不了。

**三、CL/TE 冲突与 obs-fold 一律拒绝。** CL+TE 并存、TE 多值或重复、多个不一致的 CL、多个 Host、行首 SP/HT 的旧式折行、裸 LF 与孤立 CR——这些是 CL.TE / TE.CL 走私的全部载荷形态，代理没有"猜"的余地。RFC 允许接收方忽略的 leading CRLF 我也拒绝，理由写在代码里而不是藏起来：忽略会让转出去的字节和收进来的不一致。CONNECT 与 absolute-form 给的是 `kNotImplemented` 而不是 `kTargetInvalid`，"畸形"和"合法但我还不做"必须可区分。

**四、限制只留一个真相来源。** header 条数上限原来是两处各写一份：`ParserConfig::max_header_count` 和 `ParseScratch` 的容量。结果配置里那份**从来没被读过**——用例给 scratch 分 64 槽、config 写 8，第 9 条 header 照样通过，限制静默失效。现在容量就是限制本身，config 不再重复；`ParseScratchMemoryFootprintIsBounded` 里留了回归断言（同一段 10 条 header 的字节，8 槽必须拒、64 槽必须过）。这和 1.3 那条"hint 不是预留"是同一类错误：**看起来有两个旋钮的东西，通常只有一个在起作用。**

实测数字（`TASKS.md` S1-03 行有同一份）：三 preset 各 36/36、我方警告 0 条；ASan/UBSan 对 40000 例随机字节（字母表里塞了 NUL、裸 CR/LF、控制符、CRLF 与十六进制字符）0 报告，且每条返回的 view 都被断言落在调用方给的那段字节之内。解析吞吐在 `release`（`-O3`）下 595.0 MB/s / 5,509,658 req/s（108 字节请求、单线程、无 syscall），同一用例 `debug -O0` 只有 88.1 MB/s——所以这个数字必须连构建类型一起记。

**已知代价，测成了数字，也还掉了。** 纯函数意味着每补一段字节就从头重扫：3867 字节的头部被逐字节 dribble 时，3866 次调用共重扫 7,474,911 字节、耗时 9.0 ms，而单次完整解析只要 4.6 µs（1956×）。这是慢速攻击面，不是抽象的担忧。缓解手段（连接对象记已扫描游标）归 S1-04——它不能塞进解析器，否则上面那条"可重复喂入"的可测性就没了。**S1-04 已经还掉这笔债**：真 curl 10000 请求（每条 83.9 字节头部）跑完，`header_scan_bytes = 838,894`，与转出去的字节数 `bytes_down_to_upstream = 838,894` 完全相等——每个字节只被扫过一次，一次重扫都没有；同一场景按 S1-03 那种无游标喂法是七百多万字节。逐字节 dribble 的连接层用例 `HeaderDribbleScanIsLinearNotQuadratic` 把这条锁成断言。另外「不死循环」这条验收不靠口头断言：ctest 每条用例带 `TIMEOUT 30`，真死循环会被判失败，Sanitizer 抓不到的东西由构建层抓（S1-04 最慢单条 3.4 s，余量 8.6×）。

### 1.5 `net` 的底座：`UniqueFd` / `socket` / `Epoll` / `ReactorStats`

四个小件，每个的存在都能追溯到验收句里的一句话，不是"网络层总得有这些"。

`UniqueFd` 是为了让「fd 数不随请求数单调增长」这条断言有可为之的结构：一个靠每条 return 路径手写 `close()` 的连接层，这个断言迟早会红，而红了之后我根本不知道是哪条路径漏的。收进类型之后，"漏 fd" 等价于 "对象没析构"，后者 ASan/LSan 直接告诉我。它刻意不提供到 `int` 的隐式转换——`if (fd)` 会把 `kInvalidFd(-1)` 当 true。

`socket.h/.cc` 只负责"把一个 fd 弄成事件循环可以信任的形状"（非阻塞、CLOEXEC、bind/listen、发起 connect），**不管读写**：读是 `Buffer::read_until_blocked`，写是 `Buffer::write_to_fd`，两边都带 ET 语义。这里最容易写错的一处我单独写了函数：异步 connect 的结果要等可写事件，**而可写不等于连上了**，必须再问 `SO_ERROR`——只看 `EPOLLOUT` 就认为握手成功，等于把 RST 当连接。

`Epoll` 是薄封装（创建 / 增删改 / 等待），它必须做的一件事是把 `EPOLLET` 在 add/modify 里**无条件或进去**。理由不是洁癖：ADR-02 选的就是边缘触发，而"某个地方漏了一个 EPOLLET"的表现不是报错，是行为悄悄退化成水平触发——读写照样工作，`Buffer` 的"读到 EAGAIN 为止"从必需变成冗余，等我发现时它已经是压测曲线里一个解释不清的差异。`wait()` 内部把 `EINTR` 重试掉而不是冒出来，因为信号打断是正常事件，冒出来会让调用方误判"这次等待没发生"。

它还自带 `registrations_/unregistrations_/modifications_/wait_calls_/events_delivered_`，而且**只统计成功的 ctl 调用**。验收句要断言「epoll 注册/注销次数与连接建立/关闭次数逐项相等」，那就得有一个与我自述无关的来源：如果两边都由我手工 `++`，那个相等只是我写了对称的两行代码，什么都证明不了。

`ReactorStats` 全是 `std::atomic<std::uint64_t>`，读一律 `relaxed`。单线程语义下这不是必须的（只有循环线程写），但它是 S2 门禁要验的第一件事——连接对象跨线程传递时的数据竞争——的基线：现在写成原子，等 S2 真加线程时"这条线有没有竞争"才有前后对照，而不是从零开始猜。同时立了一条规矩：**观测不是同步手段**。谁拿计数当"等到 `requests_parsed == 10000` 就往下走"的条件，那是把观测面当锁用。

### 1.6 `net/Connection`：一条 fd，和 S1-03 欠的两笔债的还款地点

`Connection` = `UniqueFd` + Role（下游/上游）+ State（kConnecting/kOpen/kClosing/kClosed）+ 入向 `Buffer` + 惰性出向 `Buffer` + 惰性 `ParseScratch` + 解析游标 + view 生命周期。它只回答两个问题：「这个 fd 上到了什么字节」「这些字节里有没有一个完整的、合法的请求」。**它不做路由、不做转发决策、不碰 epoll**——那些是 reactor 的策略。分开不是审美：S1-08 的 filter 链要插在策略侧，如果策略埋在连接对象里，「新增 filter 不改主路径任何一行」那条验收就只能靠重构来实现。

四处值得单独写：

**一、对端是一个 `int`，不是 `shared_ptr`。** 上下游成对、又都进同一个注册表，两个 `shared_ptr` 互相持有就是循环引用，那 20 KiB 永久留着，而 LSan 只会在测试里告诉我"泄漏"、不会告诉我"是两个对象抱着不死"。所以对端记 `peer_fd_`，需要时由 reactor 查注册表拿；查不到就是对端已经关了，那正好是 pump 该停的信号。

**二、已扫描游标 `scan_pos_` 是相对可读区起点的，不是绝对偏移。** Buffer 每次压缩都会 `memmove` 把内容整体搬到起点，绝对偏移一压缩就作废，那就等于造一个需要到处记得同步的隐藏状态。相对位置压缩/扩容都不动。游标只做一件事——回答"头部区结束没有 / 条数超限没有"，**不裁决合法性**：一旦这里也判 CR/LF 是否成对，判定就有了两个来源，而 1.4 第四条刚因"配置写了、代码没读"翻过一次车。合法性只由 `parse_request` 说。

**三、view 作废是可查询的，不是靠守规矩。** `request()` 返回 `nullptr` 的语义是"这份解析结果已经不能用了"，判定靠 1.3 那个 `generation()`：解析时记下 Buffer 的代次和 scratch 的 epoch，取用时对不上就作废。用过期 view 的后果不是崩溃，是把上一个请求的 Host 或 Content-Length 安到下一个请求上——那是走私，只不过是我自己造的。作废之后不是丢掉，是**重解析**（`InvalidatedViewsAreReparsedNotDropped`），而重解析是同一条请求的第二次解析动作：`parse_calls` 要记两次，`requests_parsed` 只能记一次，靠 `request_parsed_counted_` 这一个标志分开。少了它，要么计数虚高，要么重复解析死循环。

**四、出向 Buffer 和 scratch 都惰性创建，而且观测 getter 永不分配。** 空闲长连接不该为"我可能要说点什么"预支 4 KiB，100k 空闲连接每人挂一份 2048 字节的 scratch 就是 200 MB，正好把 SLO #2 的预算吃满。这条我踩过一次具体的坑：第一版把 getter 写成了惰性创建版本（`Buffer* outbound()` / `ParseScratch* scratch()`），于是测试里一句 `EXPECT_EQ(conn.scratch(), nullptr)` **自己把 scratch 建了出来**、断言随即失败——"检查有没有"这个动作本身改变了答案。现在外部只拿得到 `has_outbound()` / `outbound()`（const 指针）这种不会分配的版本，惰性创建收进私有的 `*_or_create()`，只允许发生在 `queue_bytes` / `on_readable` / `on_writable` 三条真要用的路径上。**修的是接口，不是那行测试。**

实测（`TASKS.md` S1-04 行有同一份）：`sizeof(Connection) = 400` 字节；128 条空闲下游连接（debug preset，非 sanitizer）每连接摊出 **4.47–5.72 KiB**（本任务内四次运行分别是 4.47 / 4.62 / 5.66 / 5.72）；这个抖动是 glibc malloc 的取整粒度而不是泄漏，所以我把它当区间记、不当单点记。`HeaderDribbleScanIsLinearNotQuadratic`、`SequentialRequestsOnOneConnectionAdvanceCursor`、`ConsumeInvalidatesParsedViews`、`ReleaseScratchInvalidatesViews`、`OutboundAndScratchAreLazy`、`QueueBytesRefusesWholeBlockAboveHighWaterMark`、`UpstreamRoleNeverParses`、`OversizedHeadersRejectedWithoutParsing` 八条锁住上面四件事。

### 1.7 `net/Reactor`：单线程 epoll ET 循环，第 0 步到第 5 步的关闭

它承担三件每件都不是风格问题的事：

**一、延后析构。** 一次 `epoll_wait` 可以在同一批里返回多个事件。如果在批内就把连接对象析构掉，批里后面那条事件携带的 `Connection*` 就是悬垂指针，而它接下来会被解引用去 close 一个**已经被新连接复用的 fd 编号**。ASan 能抓到 UAF，抓不到"合法但错的人"。所以关闭被拆成六步（编号从 0 起，因为第 0 步是后来才补的那一步）：**（0）出向还有货就只挂 EPOLLOUT、不碰 fd** →（1）摘 epoll →（2）解除对端反向引用 →（3）出注册表 →（4）`close_fd()` →（5）`retiring_` 在事件批处理结束时兑现析构。第 0 步是被测试逼出来的：上游回完就断（以及 S1-06 的截断判定）都会走到"决定关闭"这条路径，而响应此刻正躺在下游的出向缓冲里——直接 `close(fd)` 不是"少发一个响应"的语义问题，是我自己把已经接受的字节丢掉了。于是关闭有两种意图：`kFlushThenClose`（默认，等 `flush_outbound` 收尾）与 `kDiscardOutbound`（写已经失败了，留着也不会出去，立刻关）。只有第二种能看到 `outbound_pending() > 0` 还继续关，所以"决定要关却没有 EPOLLOUT 可等"的状态不可能出现——那条漏挂的 EPOLLOUT 就是一条永久挂住的 fd。注册表反查是第二层防护（一层防悬垂、一层防张冠李戴）：`user_data` 里的 `Connection*` 拿回来后必须和 `connections_[fd]` 对得上，对不上就当陈旧事件丢掉，不猜。

**二、accept 也要读到 EAGAIN。** 边缘触发不只对读数据成立，对监听 socket 一样。超容量时那一段写的是 `continue` 而不是 `return`：把新连接关掉之后**必须继续 accept 把队列排空**，带着没排空的队列退出，ET 下这个监听事件不会再投第二次，之后的新连接全滞留在 backlog 里——最典型的边缘触发卡死，表现是"过一会儿不接新连接了"，不是报错。`OverCapacityRejectionDoesNotStallTheListener` 就是照着这个写的：打到 `max_connections` 之后，再来一条新连接仍然要能建起来。

**三、一次事件里把所有完整请求走完。** `forward_pending_requests` 是个循环而不是单次尝试。ET 只说"自上次读以来有新字节"，不会再说第二次；如果只处理第一条就返回，第二条请求会一直躺在入向缓冲里，直到客户端**再写一次**才被看见——而 keep-alive 客户端正在等它的响应，永远不会再写。表现同样是偶发挂住（`PipelinedRequestsInOneSegmentAreAllForwarded`）。

转发策略只有一条：**把 `parsed.consumed` 长度的原始字节排进上游出向缓冲，然后消费掉下游这些字节**。不改写、不重新分帧、不做状态码映射。装不下就整块不转、下游也不消费——背压的正确方向是"让对端慢下来"，不是"把我没地方放的数据扔掉"。上游断开时跟着关下游（本任务没有连接池，也没有截断判定），这个决定在 `on_upstream_event` 里只写了一处，S2 建池之后要改就改那一行。

`stop()` 与 `run()` 的关系是这个任务里我最想留下的一处教训。最初我用一个 `running_` 同时表达"我在循环里"和"我还没被要求停"，那么 `run()` 开头的 `store(true)` 就会把在此之前送达的 `stop()` 直接抹掉，表现是循环线程永远不停。现在拆成 `stop_requested_`（release/acquire）与 `running_`，`stop()` 先落决定再敲 eventfd，顺序反过来会留一个窗口：门敲了、循环醒来一看标志还是"不停"、接着回去等，而那次等待可能再也没有事件叫醒它（自唤醒的计数已经被这次消费掉了）。`StopBeforeRunIsNotSwallowed` 锁这一条。跨线程的 `stop()` 只做一件跨线程的事：往 eventfd 写 8 字节，不碰任何循环内部状态。

实测：三 preset 各 **60/60**（debug 4.08s / asan-ubsan 7.53s / tsan 8.44s），我方警告 0 条；TSan 下 10000 请求用例 3.31 s **零 data race 报告**；真 curl 10000 请求下 `epoll_registrations=4`（监听 + eventfd + 1 下游 + 1 上游）、`epoll_unregistrations=2`、`modifications=40000`（每个请求挂/摘一次 EPOLLOUT），空闲 400 ms 期间事件批次不再增长（EPOLLOUT 刷完就摘，SLO #2 的「空闲 CPU < 5%」第一条就压在这个动作上）。

### 1.8 `--serve` 入口、`l7gate_test_upstream` 与冒烟脚本

`main.cc` 现在有两条真实路径：`--self-check`（S1-01 起）和 `--serve --listen <ipv4:port> --upstream <ipv4:port> [--serve-requests N] [--max-connections N]`。后者是 S1-04 的透传路径，退出时打印 27 行 `[serve] key=value` 给脚本读，包括 `fd_start`/`fd_end`、`rss_start_kib`/`rss_end_kib`、全部 `ReactorStats` 计数和 epoll 三个计数器。**读不到就报 -1，绝不拿 0 冒充"没有泄漏"。** SIGINT/SIGTERM 走同一条优雅停止：handler 只做一次无锁 store + `reactor->stop()`（`write(2)` 在 async-signal-safe 清单上，而这不是风格问题）。我**没有**给它加 `--idle-timeout-ms` 之类的开关——把一个还没实现的能力包装成命令行参数，等于给它发一张存在的证明。

`l7gate_test_upstream`（`tests/test_upstream_main.cc`）是上面那个类的独立可执行版，给冒烟脚本当真实上游。为什么不用 `python3 -m http.server`：它的响应头、keep-alive 行为、断开时机都不受我控制，而 S1-06 的验收恰恰要断言"逐字节一致"和"上游中途断开"；为什么不用 nginx：它没装，且 S5 的基线锁定不该在这个任务里顺手发生。它打印的是**内核实际选定的端口**（bind 到 0），这条在 S1-02 之后救过我：手工分配端口会在 CI 式反复运行里撞车。

`scripts/smoke-s1-04.sh` 分两阶段。阶段一：一条 `curl -sS -i`，断言响应头回来了、响应体长度恰好等于上游的 `--body-bytes`、网关 `requests_parsed=1`。阶段二：**curl 收完 10000 个请求之后**才 SIGTERM 网关（顺序就是这条用例——提前停网关，测的是"边收边停"，不是"收完之后什么都没留下"），读网关自报的 `[serve]` 统计，逐项断言 `requests_parsed==10000`、`requests_rejected==0`、`downstream_accepted==1`、`upstream_opened==1`、`live_connections==0`、constructed==destructed、`fd_start==fd_end`、上游侧自报服务数==10000、外部采样 fd 峰值相对基线涨幅 ≤8。

这里有个必须记下来的坑：我第一版写的是 `"http://127.0.0.1:$PORT/r{1..10000}"`。`{1..N}` 是 bash 的 brace expansion，curl 的 glob 只认 `[1-N]` 和 `{a,b,c}` 这种**集合**，于是它把 `{1..10000}` 原样当路径字符吞掉，只发了 **1 个请求**——而退出码是 0、耗时 0.02 s、`-w '%{num_requests}'>` 也没报错。三个"看起来成功"的信号同时成立。抓到它的唯一原因是那条 `requests_parsed == 10000` 的断言。所以这条断言进的是脚本而不是我的印象。

### 1.9 观测纪律：这条是这个任务里最贵的一课

上面那些测试最初全靠轮询原子计数来等状态，我把它们改成了**先拿内核级证据，再等收尾计数**。顺序是：客户端 `shutdown(SHUT_WR)` → 等到 `read()` 返回 0（对端真的关了，这是内核的记录）→ 才去等 `live_connections` 归零 → 才读 `count_open_fds()` 和非原子的 epoll 计数器（这些只能在循环线程 join 之后读）。

为什么改：轮询计数当同步条件会出两类假信号，两类我都撞到了。一类是"计数到 0 但其实还没关"，断言过早通过；另一类更糟——我改完之后有两条断言从"看着通过"变成"合理失败"，因为它们原本靠的就是那次抢跑。同时 `/proc/self/fd` 在测试里有它自己的坑：**客户端和网关在同进程**，所以一条连接占两个 fd，而 `opendir` 的 dirfd 自己也数进去了（它在每次调用里都 +1，所以抵消得掉）。`MemoryPerIdleConnectionIsMeasured` 的峰值我第一次写成 `fd_with_reactor + 128`，实际是 `+2×128`——漏了客户端那 128 个。用 `live_connection_count()`（map 的 size）当观测也会错：那是非原子的、跨线程读的状态。`count_open_fds()` 因此刻意不分配内存，`opendir → 数 → closedir` 一条直线。

### 1.10 一处刻意的例外：sanitizer 构建不判 20 KiB 预算

`MemoryPerIdleConnectionIsMeasured` 的预算断言现在被 `k_rss_is_instrumented` 挡住：sanitizer 构建下每连接 RSS 含插桩开销（本任务实测 asan-ubsan 11.31–16.28 KiB、**tsan 20.81–22.69 KiB，两次都超过 20 KiB 预算**），而它测的不是网关的内存，是插桩的内存。留在那里判的话，只有两种后果：把诊断指向"网关内存超标"这个假结论，或者逼我把 SLO 阈值改松——后者是契约明令禁止的动作（**永远不许为了让数字过线而改 `SLO.md` 的目标值**）。所以 sanitizer 构建打印数字 + 说明为什么这里不判，判定归 debug/release。

TSan 那个数超过预算不是无害的信息，它告诉我一件事：**如果 S2 的线程模型让每条连接多挂一把 mutex 或一份 per-thread 缓存，20 KiB 的预算会真的紧张**——虽然插桩本身不是那个成本，我不能拿 22.69 当"网关超标"的证据，但它足够让我在 S2 拆任务时把"每连接内存"列成 TSan 之外的第二条观测线，而不是等 S3 才发现。



### 1.11 关于「每连接内存」目前能说到哪一步

S1-04 交的是**口径**，不是 SLO #2 的判定。能说的：128 条空闲连接摊出来的每连接增量在 debug 下是 4.47–5.72 KiB（含测试夹具自身），`sizeof(Connection)=400`，出向 Buffer 与 scratch 都没建（`outbound_buffers_created`/`scratches_created` 在纯空闲场景为 0）。不能说的：100k 连接下的每连接内存——那是 S2 之后的实测项，`TASKS.md` 那一行仍然写"未测"，我不打算用 128 条的数外推 1000 倍。

### 1.12 「零泄漏」这句话是有反证的

`TASKS.md` S1-04 行会写"ASan/LSan 报告零泄漏"，而这句话本身没有信息量——一个根本没启动 sanitizer 的构建也说得出同样的话。所以我做了一次阴性对照的反向操作：**故意留一条不释放的连接对象**，重跑 `asan-ubsan`，LSan 报出 `LeakSanitizer: detected memory leaks` 且退出码为 1；把那行删掉，退出码回到 0。同一个二进制里同一条断言，一个方向能红、另一个方向才叫绿。

这和我在 S1-01 就写进 `build_info` 的那条断言（`RequestedSanitizersAreActuallyCompiledIn`）是同一件事的两半：先证明 sanitizer 真的编译进去了，再证明它真的会报。两半都过，"ASan/LSan 干净"才是一条可复现的结论，而不是我的一句自述——契约第 7 节要求任务收尾时有 `bash scripts/verify-all-presets.sh` 这一条命令能复现它，就是为此。

## 2. 构建契约（这几条是有理由的，不是风格）



- **C++20 + `CMAKE_CXX_EXTENSIONS OFF`**：我要标准 C++20，不是 GNU 扩展下的"看起来能用"。
- **生成器是 Unix Makefiles，不是 Ninja**：Ninja 不在契约 9.1 锁定表里，而 8.2 禁令三对"引入"这件事不分构建工具还是库。ADR-04 的自研边界只覆盖网络层，我没给自己开构建工具的口子。
- **`-Werror` 默认 OFF**：clang 21 是新版本，把新增警告直接升级成构建失败会让工具链升级变成阻塞项。
- **警告集是显式列举的**（`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wnon-virtual-dtor -Wcast-align -Wunused -Woverloaded-virtual`）。`-Wconversion`/`-Wsign-conversion` 对网关这类大量做长度与偏移运算的代码是有效防线；`net/buffer.cc` 全是这类运算（`size_t` 偏移、`ssize_t` 返回值、容量夹取），实测我方警告仍为 0 条。S1-04 把这条警告在真正的转发代码上复核了一次，结论在下面最后一条。
- **sanitizer 用字符串而不是逐个开关**（`L7GATE_SANITIZERS="address,undefined"`）：S1 门禁要求的是**组合**，而 address 与 thread 互斥，用一堆 bool option 表达互斥关系比用字符串更绕且更容易配错。
- **sanitizer preset 强制 Debug（`-O0 -g`）**：实测发现 `-O1` 起会把"可被证明无副作用的坏写入"优化掉，ASan 于是什么都不报（详见 `ENVIRONMENT.md` 2.2）。所以在优化构建上跑 sanitizer 得出的"干净"结论是假的。
- **`release` preset 是 `BENCHMARK.md` 唯一允许的构建**：sanitizer 构建测出来的尾延迟是检查开销的延迟，不是网关的延迟。
- **测试目标显式链 `Threads::Threads`（`find_package(Threads REQUIRED)`）**：S1-04 的测试把 reactor 跑在别的线程里、也要从别的线程停它。`tsan` preset 自带 `-pthread`，但 `debug`/`release` 不带——少了线程支持时 `std::thread` 在 GCC 上**能编译**、运行时才炸，那是最糟的一类"构建通过、测试诡异"。库本体 `l7gate_core` 不链线程：单线程 reactor 到 S2 才需要，提前链上等于给"我已经线程安全了"发一张空头证明。
- **`l7gate_test_upstream` 是被构建出来的、不是被测试出来的**：它复用 `TestUpstream` 那个类，编译成独立可执行文件给冒烟脚本当上游，所以它链 `l7gate_core` 和线程，但**不链 gtest、不注册进 ctest**。同一个类同时服务单测和外测，"上游的行为"就只有一份定义。
- **警告集的代价在 S1-04 复核过一次，仍然划算**：`-Wconversion`/`-Wsign-conversion` 在 `net/` 这五个新文件（全是 `size_t` 偏移、`ssize_t` 返回值、`static_cast` 夹取）里我方警告 **0 条**；`-Wshadow` 抓到过一次真实的遮蔽：给 `TestUpstream::start()` 加端口参数时，函数里的局部 `const int port` 把参数盖掉了。`-Werror` 默认关（第 3 条），所以它没有让构建失败——是 `verify-all-presets.sh` 里那句「非 gtest 警告 = 0」把它当场暴露出来的，我因此把名字改成 `requested_port`/`bound_port`。**这正好说明警告不该升级成错误、但计数必须升级成门禁**：前者拦住了工具链升级，后者一次遮蔽都没放过。这三条警告在这个任务里的性价比是实测的，不是信仰。

## 3. 已定但未实现的结构意图

只写到接口形状，实现归属看 `TASKS.md`。S1-03 结尾挂着的两笔债（**已扫描游标**、**view 生命周期**）已经在 S1-04 还掉，还款记录在 1.6 和 1.4 的末尾，这里不再列为待办。

- **S1-07 的背压：钩子齐了，动作没做。** Buffer 侧（`ReadResult::limit_reached` + 「返回时 `readable_bytes() <= high_water_mark`」）和 Connection 侧（`queue_bytes` 装不下整块就返回 false）都就位，`high_water_hits` 也在统计。缺的是 reactor 里那一步：**收到 `limit_reached` 就摘掉该 fd 的 EPOLLIN，等对端腾出空间再把读事件加回来**。现在的行为是撞水位后继续让 EPOLLOUT/EPOLLIN 挂着、靠 `forward_pending_requests` 返回 `kBlocked` 把字节留在原地——不丢数据，但也不压对端，所以「内存不继续增长」这条能过、「停止消费下游可读事件」那条不能过。这个缺口是我明知故留的：S1-04 的验收只打生命周期，把背压混进去会让两条结论互相解释不清。
- **转发语义（S1-06）**：`on_downstream_readable` 里 `round.reject` 那一行现在只做一件事——关连接。它就是 ParseError→400/411/413/501 那张映射表将插入的位置，22 个错误码在 S1-03 就已经 `to_string` 齐了。响应侧目前完全不认识状态码、不判定截断、不做 chunked 框架的透传验证。
- **Upgrade 透传（S1-05）**：`release_scratch()` 已经落地并且有断言（`ReleaseScratchInvalidatesViews`），但**没有人调用它**——真正的触发点是 Upgrade 握手完成。留一个没人调的函数比留一个"顺手在请求结束时释放"的函数诚实：后者会把 keep-alive 的下一个请求打回重新分配，那个 2048 字节就白省了。
- **多 Reactor + 上游连接池（S2）**：连接对象的销毁路径从 S1 就按"可能被别的线程持有"设计（六步关闭 + `retiring_`），`ReactorStats` 全原子也是给 TSan 留的基线——这是 ADR-03 那笔「线程共享地址空间、代价是并发正确性」的具体支出。`on_upstream_event` 里"上游 EOF 就跟着关下游"那一处，是建池之后唯一要改的地方。
- **`filter/chain`（S1-08）**：空壳先立。验收标准是「新增 filter 不修改转发主路径的任何一行代码」——S1-04 之后主路径真的存在了（`forward_pending_requests` → `forward_request` → `pump_upstream_to_downstream`），所以这条验收现在才有意义：它检验的是"我能不能在不动这三条函数的情况下插进去"。
- **`--serve` 还不是产品形态**：`--serve-requests` 是"解析满 N 条就停循环"，它可能在最后一条响应送达之前就 break（帮助文本里明写）。真正的服务生命周期（配置加载、优雅排水、指标暴露）都不在 S1 范围里。

## 4. 已知噪声与待补项

| 项 | 现状 | 影响 |
|---|---|---|
| clang 21 对 `gtest-printers.h` 报 `-Wcharacter-conversion` | 已把 gtest 头按 `-isystem` 处理并**在 flags.make 里确认生效**，警告仍然逃逸——clang 21 这条新警告不受系统头抑制 | 每个 clang preset 固定 1 条第三方噪声。我**不加** `-Wno-character-conversion`：那会同时关掉我自己代码里的同一警告，用消噪换 blindness 不划算 |
| ASan 堆栈无符号化 | 缺 `llvm-21-tools`（提供 `llvm-symbolizer`），报告退化成裸地址 + BuildId | 门禁要求"ASan 干净"不受影响；一旦真出问题，定位要手动 addr2line。已记 `ENVIRONMENT.md` 2.2 |
| `build/` 下每个 preset 各自下载一次 googletest | 三个 preset 三次 FetchContent | 构建耗时 15–20s 里含下载。可接受；共享 `_deps` 会引入 preset 间状态耦合，不值得 |
| WSL 里 `ulimit -n` = 10240 | S1-04 的用例最多同时用 265 个 fd，撞不到；`max_connections` 默认 4096 也够不到 | S2 的 100k 长连接场景必须先解决 rlimit（`docs/ENVIRONMENT.md` 记了当前值），否则那条 SLO 测出来的第一行会是 `accept_errors` |
| 冒烟脚本的 fd 采样是 0.2 s 一轮的外部观测 | 它能证"曲线不爬升"，证不了"每个瞬间都不爬升" | 所以 fd 归零那条判定以**网关自报的 `fd_start`/`fd_end`** 为准（同一个进程、同一次 `getrlimit` 视野），外部采样只做旁证 |
