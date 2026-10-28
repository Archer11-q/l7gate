# ADR-16 · 请求-响应配对模型：支持 pipeline，`Connection: close` 在它自己的那条响应后兑现

- 状态：已采纳
- 日期：2026-10-27
- 相关：S1-07 留下的债（`ARCHITECTURE.md` 1.15 末、`FAULT-INJECTION.md` 第 6 节、2026-10-26 裁决"归 S2 配对模型"）、ADR-15（上游连接池；本 ADR 末尾更正我在拆解里"一在飞请求"的不准确说法）、ADR-09（隧道）、`docs/ARCHITECTURE.md` 1.14（响应框架泵）/1.19（本 ADR 落地）

## 我选择什么

**支持 HTTP/1.1 客户端 pipeline，并按 FIFO 把响应配回请求；`Connection: close` 在带它的那条请求的响应完结之后才兑现，而不是在第一条响应之后。**

具体模型（一条下游连接 = 一个有序的请求-响应管线）：

1. **一条下游可以同时有多个在飞请求**（pipeline）。它们按到达顺序被转发到这条下游**独占的那条上游**（ADR-15 的 per-downstream 生命周期配对），上游按同一顺序回响应，网关按 FIFO 原样回送给下游——响应绝不乱序，这是 HTTP/1.1 pipelining 的硬要求（RFC 9112 §6.3.2）。
2. **`Connection: close` 记在它所属的那条请求上**，网关数着在飞请求：转发一条就 +1、完结一条响应就 −1；当"带 close 的那条请求"的响应完结（在飞数归零）时才关闭这条配对，级联关下游。**不是第一条响应一完结就关**——那正是 S1-07 记的那笔债（400 条批次只兑现 1 条响应）。
3. **close 之后的请求是客户端违规**：RFC 9112 明确"发出 close 记号的一方不得在其后再 pipeline"。所以转发到带 close 的那条为止，**同一批次里它后面的请求不再回源**（`forward_pending_requests_body` 的 close 检查从"函数入口"挪进"循环体内每次迭代"，转发完 close 请求的下一轮立刻停），这些字节随配对关闭一起丢弃，不伪造响应、不报错——与 S1-06 对"close 之后的残包"的处置同一条口径。
4. **在飞数为 0 是上游可复用的前提之一**：ADR-15 的 `upstream_reusable` 增加"在飞请求 == 0"一条——一条还在等响应的上游不能被回收进池给下一条下游，否则响应会配错人。

## 为什么

**因为 S1 已经把 pipeline 的转发与 FIFO 响应跑通了，缺的只是 close 的兑现时机——补齐它比退回"不支持 pipeline"改动更小、语义更正确。** 这是项目所有人 2026-10-27 在 A、B 两案里选的 A（B 案是"串行化、一条下游同一时刻只有一个在飞请求"）。论据：

- `forward_pending_requests_body` 本来就在一次可读事件里把多条完整请求全部转发（`PipelinedRequestsInOneSegmentAreAllForwarded` 锁的就是它）；响应侧 S1-06 的框架泵按 FIFO 一条一条判完结、一条一条透传，`responses_completed` 逐条计。整条管线是对的，唯一错的是 `pump_response` 的 `kComplete` 分支只读一个 `close_after_response()` 布尔——它不区分"完结的是第几条响应"，于是 close 落在第一条上。**这是一个计数缺失，不是模型缺陷**，用一个在飞计数就能补正。
- 退回 B 案（串行化）反而要**改动** `forward_pending_requests_body` 的"一次读转多条"行为，去碰 `PipelinedRequestsInOneSegmentAreAllForwarded` 这条已立的断言——为了回避一个本来能补的小 bug，去动一条正确的既有性质，不划算。
- 支持 pipeline 不增加并发面：在飞计数是**每 worker、每连接**的普通成员（单线程拥有，ADR-14），不引入任何跨线程状态；它只在 owner worker 的循环里加减，TSan 无新增面。

## 我放弃什么

- **不实现服务端 pipeline 的重排/乱序优化**。HTTP/1.1 要求响应按请求顺序回，本模型就是死板的 FIFO。Envoy 之类对 pipelined 请求做并行回源再按序重排，那是吞吐优化，没有 SLO 证据之前不做（契约 3.2）。而且并行回源会立刻撞上 ADR-15 的"下游存活期独占一条上游"——要支持它得先支持 shared 池或每请求借还，那是另一条 ADR。
- **不支持一条配对上同时混 pipeline 普通请求和 upgrade**。upgrade 一旦进入就是专用隧道（ADR-09），与"前面还有普通请求在飞"的语义互斥；真实客户端不会这么发，网关也不为它定义行为（落到隧道/在飞计数的交叉，按现有 maybe_enter_tunnel 的"内容是 101 才转隧道"判定，不误伤普通响应）。

## 关于「一在飞请求」这个说法的更正（是我说错的，不是代码/ADR-15 的错）

我在 S2-02 的拆解耦合点注记和给你的汇报里写过一句"S2-02 先按「一条上游同一时刻只服务一个在飞请求」的保守口径落地、S2-04 再放宽"。**这句是我描述得不准**：S2-02 的代码原样保留了 S1 的行为——一条上游本来就能服务它那条下游的**多个**在飞请求（pipelining + 响应 FIFO）；ADR-15 正文写的也正是这个（"下游存活期独占上游、只在下游关闭时回收"，池的借还按下游生命周期、不按请求）。**所以代码和 ADR-15 都没有"一在飞请求"的限制，也不需要改。** 真正遗留的只有 S1-07 那条 close 兑现时机的债——`pump_response` 的 `kComplete` 只看一个 `close_after_response()` 布尔、在**第一条**响应完结就关，于是一批 pipelined 请求只兑现 1 条响应。本 ADR 正面解决它。（TASKS.md 里那句不准确的注记已就地更正、留删除线指向本 ADR；这是我改自己的措辞，不是改一条存在的 ADR 结论。）

## 后果与约束

- **改动面很小且局部**：`Connection` 加一个 `in_flight_requests_` 计数（转发 +1 / 响应完结 −1）；`pump_response` 的 `kComplete` 分支把"close 与否"从"看一个布尔"改成"看布尔 && 在飞归零"；`forward_pending_requests_body` 的 close 检查从入口移进循环体。三处都在 owner worker 线程内，无新锁、无新跨线程面。
- **`sizeof(Connection)` 会变**（加一个 `size_t`）：从 520 抬到 528（+8 B，仍 8 字节对齐）。这条进 1.11 的每连接内存口径账，SLO #2 的构成表要跟着记。
- **可断言的验收**：pipelined 批次"前 K−1 条普通 + 第 K 条 close"，断言响应**恰好 K 条**逐条送达、`responses_completed==K`、第 K 条之后连接才关（不是 1 条就关）；且 close 之后的请求不回源（`bytes_down_to_upstream` 只含前 K 条）。负例：批次里没有 close → 全部转发全部回（`PipelinedRequestsInOneSegmentAreAllForwarded` 继续绿，行为不变）。
- **不新增统计口径的改动**：`responses_completed`、`requests_parsed`、`close_headers_honored` 的定义都不变，变的是它们之间的**时序**（close 兑现点后移）。`requests_parsed` 仍按解析计数（含 close 之后被丢弃的那些，它们被解析了但没转发），`responses_completed` 仍按框架完结计数（到 close 那条为止），两者差额因此可解释（close 之后未回源的请求）。
