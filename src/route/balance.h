#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "route/router.h"  // kNoUpstreamTarget：组成员用的就是目标表那一套下标，不另立第二个哨兵值

// 负载均衡（S3-02，ADR-19）。
//
// 这里放三样东西：一张**上游组表**（一条路由指向的那个"后端集合"）、一个**一致性哈希环**、
// 一份**每 worker 私有的均衡状态**。三样都不是 filter，也不是 reactor 的策略层——
// 它们是数据与纯计算，选哪一个后端这件事由 `filter::LoadBalanceFilter` 在这一份数据上做。
//
// 为什么均衡状态必须 per-worker（ADR-19「为什么」第一条）：跨 worker 读同一张计数表就是
// 共享可变状态，而 ADR-14/15 整个线程模型的价值全在"连接与循环内状态只被 owner 线程碰"。
// 代价也照写在这里，不等 S3-06 才发现：**这里买到的均衡是 per-worker 的**，
// 16 个 worker 各自轮询时，某个后端收到的连接数是 16 个独立计数器的叠加，瞬时可以不均。
namespace l7gate::route {

// 组下标的两个哨兵（`kNoUpstreamGroup` / `kMaxUpstreamGroup`）**在 `route/router.h` 里**，
// 这里只 include 不重复定义：同一份限制有两个抄本就是 `max_header_count` 那次的形状。

enum class Balance : std::uint8_t {
  kRoundRobin,      // 组内游标自增，取模
  kLeastConnections, // 组内"当前被配给下游的连接条数"最小者
  kConsistentHash,  // 环上第一个 >= hash(key) 的节点（末尾回绕）
};

const char* to_string(Balance balance) noexcept;

// 解析配置里的 "round_robin" / "least_conn" / "consistent_hash"。
// **只认这三个字面量**，其他一律返回 false：`balance` 拼错必须让启动失败，
// 不能退化成"用了缺省策略"那种安静形态（同 `--upstream` 与 `--route` 的立场）。
bool parse_balance(std::string_view text, Balance* out) noexcept;

// 一致性哈希的 key 来源（ADR-19 第五条：这不是实现细节，是产品语义——
// key 选什么决定的是"哪一批请求会一起搬家"）。
struct HashKey {
  enum class Source : std::uint8_t { kTarget, kHeaderName };
  Source source = Source::kTarget;  // kTarget = 原始 origin-form target（含 query，不解码）
  std::string header_name;          // 仅 kHeaderName 有值；大小写不敏感取第一个命中
};

// 默认虚拟节点数。**这个数不是推导出来的**，它只是一个起点：S3-02 的用例要在 N→N+1 时
// 把迁移率量出来（不同 vnodes 的散布进 BENCHMARK），而不是让 160 这个数字替我做结论。
// 声明在 `UpstreamGroup` 之前：它的默认值要用到它，而 C++ 的成员初始化器读的是
// 名字在**声明处**已可见的实体（这条顺序搞反过一次，报的是"kDefaultVnodes 不在范围内"）。
inline constexpr std::uint32_t kDefaultVnodes = 160;

// 一个上游组：一串目标下标 + 用哪种策略在它们之间选。
struct UpstreamGroup {
  std::string name;
  Balance balance = Balance::kRoundRobin;
  HashKey hash_key;
  std::uint32_t vnodes = kDefaultVnodes;
  std::vector<std::uint16_t> targets;   // 全局目标下标（`Reactor::Config::upstream_targets` 的值域）
  std::vector<std::pair<std::uint64_t, std::uint16_t>> ring;  // 仅 kConsistentHash：按哈希值排序
  // S3-04（ADR-20 第二节）：成员来自节点表（文件里 `"source": "mysql"`）。
  // **与 `targets` 互斥**（装配期拒绝双给）；装载成功前 targets 为空——
  // 空成员组的 LB 语义（502）就是"成员还没到岗"的表态。
  bool source_mysql = false;
};

// 组表。装配期建好、运行期只读——**这条时序纪律和 filter 链、路由表是同一条**：
// 环与游标的容器都不在运行期变长，所以读它们不需要锁，也不需要 TSan 替我盯着。
class GroupTable {
 public:
  GroupTable() = default;

  // 加一个组。`name` 重复 = 配置写错，返回 false 且不留半条组。
  bool add_group(std::string_view name, Balance balance, std::uint16_t* group_index) noexcept;

  // 给第 group 组加一个成员。`targets` 允许重复吗——**不允许**，同一地址:端口在一个组里
  // 出现两次会让"这个组有几个后端"这件事有两个答案（N→N+1 的迁移率用例首先就被它骗）。
  bool add_member(std::uint16_t group, std::uint16_t target) noexcept;

  // 建环这件事没有"表级"入口：环需要每个目标的端点串，那是装配方手里的数据，
  // 所以按组调用下面那个自由函数 `build_hash_ring`。我原来在这里留过一个
  // `GroupTable::build_rings()` 空成员函数，随即删掉——**一个"看起来做了但没做"的入口
  // 比没有更坏**，它会让读代码的人以为环已经被建过了。
  std::size_t size() const noexcept { return groups_.size(); }
  bool empty() const noexcept { return groups_.empty(); }
  const UpstreamGroup* group_at(std::uint16_t group) const noexcept;
  const UpstreamGroup* group_by_name(std::string_view name) const noexcept;
  std::uint16_t group_index_by_name(std::string_view name) const noexcept;  // 没有则 kNoUpstreamGroup

  // 仅用例/日志用：某个组的成员下标列表。
  const std::vector<std::uint16_t>* members_of(std::uint16_t group) const noexcept;

 private:
  std::vector<UpstreamGroup> groups_;
};

// 每 worker 一份的均衡状态（ADR-14：只被 owner worker 的循环线程碰，**全非原子**）。
//
// 为什么单独成一个类而不是塞进 `Reactor` 的成员：`LoadBalanceFilter` 也要读它，而 filter
// 在 `Reactor` 构造之后、循环线程启动之前就要装好。两边拿同一份 `shared_ptr` 指向的对象，
// 归属仍然只有一个 worker（`ReactorGroup` 每 worker 造一份），所以"共享"这个词在这里
// 意味着"同一线程内的两个持有者"，不是"两个线程的持有者"——这条区别是 TSan 会不会红的分界，
// 所以写出来而不是留给读代码的人猜。
class LbState {
 public:
  explicit LbState(std::size_t target_count, std::size_t group_count) noexcept
      : target_pairings_(target_count, 0u), rr_cursor_(group_count, 0u) {}

  // 一条上游被配给下游（借出或新建）时 +1；它被归还进桶或被淘汰时 −1。
  void note_paired(std::uint16_t target) noexcept;
  void note_unpaired(std::uint16_t target) noexcept;
  std::uint32_t pairings(std::uint16_t target) const noexcept;

  // 轮询游标：返回这次该用的偏移量并把游标推进。**推进发生在同一次调用里**，
  // 因为"取"和"加"分成两个方法就会有人漏掉其中一个，而漏掉的后果是同一个后端被连选。
  std::size_t advance_rr(std::uint16_t group, std::size_t member_count) noexcept;

  // S3-04（ADR-20）：ever-seen 增员后由 **owner worker** 在对账 tick 里扩容配对账。
  // 只能在 owner 循环线程调用（与 note_paired 同一条纪律）；resize 保住已有账目，
  // 新槽位记 0（新成员没有历史配对）。组数不扩：组是文件的静态段，reload 不改它。
  void ensure_capacity(std::size_t target_count) noexcept;

  std::size_t target_count() const noexcept { return target_pairings_.size(); }

 private:
  std::vector<std::uint32_t> target_pairings_;
  std::vector<std::uint32_t> rr_cursor_;
};

// FNV-1a 64 位（ADR-19 第五条）。为什么不用 `std::hash`：它的取值**跨实现不保证稳定**，
// 而"同一 key 恒定同一后端"与"N→N+1 的迁移率"这两条判据一旦依赖一个我控制不了取值的函数，
// 报告里那些数就变成"在这台机器的这一版标准库上成立"。
// 代价写清楚：换哈希函数等于换掉所有 key→后端 的映射（迁移率 100%），那是运维事件不是代码细节。
//
// **环上实际用的是下面的 `ring_hash`，不是这个裸函数。**这条偏离是实测逼出来的，
// 数字与理由都记在 `balance.cc` 里那段终值混合的注释旁边（迁移率 48.10% → 15.13%）。
std::uint64_t fnv1a64(std::string_view text) noexcept;

// 一致性哈希真正的那个哈希：`mix64(fnv1a64(text))`——FNV-1a 之后加一道 splitmix 终值。
// 裸 FNV-1a 只差末位字节的两个输入，哈希值只差 `Δ × prime`（相对 2^64 是 6e-8），
// 而组里的成员天生就只差末尾那截端口号，于是每个成员的点全挤在一簇里、环不均匀。
// key 与点**必须走同一个函数**，否则"哪些请求落在哪一段"仍然是簇状的。
std::uint64_t ring_hash(std::string_view text) noexcept;

// 建环：每个成员 `vnodes` 个点，点键 = ring_hash(组名 + '#' + 虚拟节点序号 + '#' + 该目标的端点串)。
// **端点串进键里**，这样同一台后端换端口时它的环位置也变——这不是省事，是不让
// "两个成员哈希撞在同一个点"这件事被组名遮蔽（撞点的后果是那两个成员永远同进同出）。
// `endpoint_keys[target]` 由装配方给（下标 = 全局目标下标）；**有任何一个成员取不到键就返回
// false 且不留下半张环**：环缺一个成员的话，迁移率的实测数字会"按我少算的那个数"成立，
// 而那条断言是本任务唯一能证伪"我实现了哈希环"的东西。
// 排序后同键的点按下标破平，所以同一份输入必然得到同一个环（可重复喂入那条纪律在这里的形态）。
bool build_hash_ring(UpstreamGroup* group, const std::vector<std::string>& endpoint_keys) noexcept;

// 在已建好的环上选一个成员。空环返回 nullopt（组的 `targets` 为空这件事在装配期就被挡，
// 这里返回 nullopt 只可能是"装配漏建环"，调用方必须把它当成错误而不是"没选中"）。
std::optional<std::uint16_t> select_from_ring(const UpstreamGroup& group,
                                              std::string_view key) noexcept;

// 带候选集的环查找（S3-03）：从 hash(key) 落点起顺时针走，**跳过不在候选集里的成员的点**，
// 最多绕环一整圈；全员的点都不在候选集里返回 nullopt（调用方回退到全员集合）。
// 候选集 = 全体成员时结果与两参版本逐点相同——健康检查没摘任何人时零行为差异。
// 候选集必须按成员下标升序给（`is_member` 用二分；乱序的候选集是调用方的装配错误，
// 二分查不到会把"被摘除的成员"当成健康的放回来——那是摘除语义整体的失败）。
std::optional<std::uint16_t> select_from_ring(const UpstreamGroup& group, std::string_view key,
                                              const std::vector<std::uint16_t>& candidates) noexcept;

// 候选集判成员（二分）。lb_filter 的三个策略共用这一个谓词：候选集"是不是升序"
// 只在这一处被依赖，注释只需要写在这里。
bool is_candidate(const std::vector<std::uint16_t>& sorted_candidates,
                  std::uint16_t target) noexcept;

}  // namespace l7gate::route
