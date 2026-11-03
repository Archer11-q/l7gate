#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

// 上游健康状态表（S3-03，ADR-12 落地 + ADR-19 追加第四条；S4-05 加熔断的第二条判据与
// 半开门，ADR-12 追加二）。
//
// 这里只放**状态与决策**，不放探测：主动探活的连接机在 `Reactor` 里（它要 epoll 与
// 非阻塞 socket），被动失败判据的采集点也在 `Reactor`（connect 失败、5xx、截断）。
// 这份表回答的问题是 ADR-19 第三条留给 S3-03 的那一个——"这一组里**现在谁可用**"。
//
// 归属口径（为什么它放在 route/ 而不是 net/）：摘除改变的是"组里谁可被选中"，
// 它的第一个读者是 `LoadBalanceFilter` 的候选集；连接对象与 fd 一概不进这张表，
// 所以 ADR-14 的接触面不因它变大。
//
// 并发口径：**状态迁移全原子**，被同组全部 worker 共享一份（先例是 `ReactorStats`——共享的
// 原子计数面）。一个后端死没死是**关于后端的事实**，不是策略；策略（游标、配对数）
// 才是 per-worker 的。多 worker 各自记各自的成功/失败、各自 CAS 摘除/恢复，
// TSan 的可证伪命题照旧成立：`tsan` preset 零 data race。
//
// **一处例外，是有意的**：S4-05 的错误率窗口用一把 per-target 的 `std::mutex`
// （`window_mu`）。理由写在 ADR-12 追加二第一节——窗口是**统计**不是状态：状态迁移
// 仍然只走那一元字上的 CAS（"这个成员现在处于哪个状态"永远只有一个答案），
// 而环形桶的"清旧格 + 加新样本"两个动作压不进一次原子操作，硬做成无锁要么丢
// 本窗口的样本、要么把 16 个桶之前旧窗口的样本当成现在的（后者方向是**更凶**，
// 不能要）。临界区里只有整数加减，没有 syscall；先例是 S4-03 的限流桶表（口径⑬）。
namespace l7gate::route {

// S4-05（ADR-12 追加二）：熔断第二个输入与恢复的门。缺省值即那一节裁下的缺省。
// `enabled` 不是"配置里想写就写"的自由项：它等于配置文件里**存在** `circuit` 段
// （段不存在＝今天 S3-03 的语义逐字节不变，ADR-12 追加二第三节）。
struct CircuitPolicy {
  bool enabled = false;
  // 滑动窗口总时长。窗口实际切成 8 格（`kWindowBuckets`，内部常量、不做配置项），
  // `bucket_ms = max(1, window_ms / 8)` → **有效窗口 = 8 × bucket_ms**，比配置值短
  // 不到 7 ms：方向是更保守（旧样本提前出账），不是更宽。
  std::int64_t window_ms = 10000;
  // 样本下界：窗口内不足这么多条请求，错误率不判。没有这一格，"3 条里 2 条 5xx"
  // 就等于 66% 并把一台后端判死。
  std::uint64_t minimum_requests = 20;
  // 失败占比阈值（%）。判据用整数比较 `失败 × 100 ≥ 总 × 阈值`，不引浮点。
  std::uint32_t error_rate_percent = 50;
  // 半开放行预算 B：一个半开周期内最多放几条业务请求进这一台成员。
  std::uint32_t half_open_requests = 3;
  // 出半开要攒几条**业务**成功（必须 ≤ B，否则永远出不来——配置层整体拒绝）。
  std::uint32_t half_open_successes = 2;
  // 半开周期上限：期内没攒够（探测请求挂着、或者根本没流量）就回摘。
  // 半开不是终点站，四条出路里得有一条给它兜到期。
  std::int64_t half_open_timeout_ms = 3000;
};

// 健康检查策略。缺省值即 ADR-19 追加第四条定下的那五个（外加 S4-05 的 `circuit`，
// 默认不装配），配置文件里每个字段都可覆盖。
struct HealthPolicy {
  // 连续失败多少次判摘（nginx `max_fails` 同义）。nginx 缺省 1，这里取 3：
  // 探活场景下单次失败就摘会把一次网络抖动读成一台后端死了——误摘率那格替我付账。
  std::uint32_t max_fails = 3;
  // 摘除时长（nginx `fail_timeout` 同义，nginx 缺省 10s）。S4-05 起它是"摘除态"的时长，
  // 到期不再等于"全量放行"，而是"进半开"（ADR-12 追加二第二节）。
  std::int64_t fail_timeout_ms = 10000;
  // 主动探活间隔；**0 = 不主动探活**（只跑被动）。
  std::int64_t probe_interval_ms = 1000;
  // 单次探活的超时（连接 + 发送 + 响应头共用这一个预算）。
  std::int64_t probe_timeout_ms = 500;
  // 最多摘除比例（ADR-12，缺省 10% 一字未动）。分母 = 目标表去重后的成员数，
  // 可摘除上限 = floor(成员数 × 比例 / 100)——"被摘除容量 ≤ X%" 的字面读法。
  std::uint32_t max_ejection_percent = 10;
  // S4-05：第二条判据与半开门。
  CircuitPolicy circuit;
};

// 失败是从哪条路来的（S4-05，ADR-12 追加二第一节）。两条路**汇入同一条连续失败计数与
// 同一个摘除出口**（S3 已裁第三条④），但只有一条进错误率窗口：窗口是"业务请求的真实
// 体验"，探活是网关自己发的固定 GET，掺进去之后"这台后端的业务错误率是多少"就没有答案
// 了——那是口径第 3 条（仪器不进被测方）在这一层的延伸。
enum class FailureSource : std::uint8_t {
  kBusiness,  // 业务请求撞上的失败（5xx / 截断 / 头坏 / 三类超时 / connect 失败）
  kProbe,     // 主动探活判死
};

// 一次失败记录的结局。可观测、可断言——ADR-12 要的"触发时有可观测计数"落在
// 这四个值加上下面的计数器上，不是落在日志里。
// **判据的两种触发理由不在这里区分**（都返回 kEjected），区分归计数器
// `rate_trips_total` / `half_open_reopened_total` / `ejections_total`：调用方不关心
// 是谁摘的，而误摘率与恢复时间线的对账要的是"哪一格加了几"。
enum class FailureVerdict : std::uint8_t {
  kRecorded,       // 计了失败，还没到阈值
  kEjected,        // 这次失败触发了摘除（连续失败 / 错误率窗口 / 半开期内失败回摘）
  kValveBlocked,   // 到阈值了，但安全阀满了（ADR-12：故意保留一段错误）
  kAlreadyEjected, // 已在摘除期内的又一次失败：不计摘、不重复计摘
};

// 每请求一问的答案（S4-05 起候选集读的就是这一个问句，不再是 `is_ejected` 一问）。
// kProbeSlot 的意思是"这一台在半开，而门给了它一条名额"——`LoadBalanceFilter` 收到
// 这个答案时**只把它当唯一候选**（ADR-12 追加二第二节第 2 条：放行的那条必须真的落到它，
// 放回候选集"允许被选中"等于摆设，B-013 的 98.3% 吸附就是这条的实测理由）。
enum class Admission : std::uint8_t {
  kAllow,     // 健康：全量放行
  kProbeSlot, // 半开且门给了名额：这一条请求钉给它
  kDeny,      // 摘除中，或半开预算已耗尽
};

// 单个目标的可观测读数。全原子，运行中可读（拿它当同步条件仍然犯规——stats.h
// 那条纪律在这里同样成立，测试里等它之前先拿内核级证据）。
struct TargetHealthSnapshot {
  std::uint64_t failures_total = 0;     // 记进来的失败总次数（连续计数是内部状态，不在这里）
  std::uint64_t ejections_total = 0;    // 被摘除总次数（**误摘率与摘除耗时的分子来源**）
  std::uint64_t recoveries_total = 0;   // 恢复总次数（到期恢复与探活成功都算）
  std::uint64_t probe_ok_total = 0;     // 探活判活次数
  std::uint64_t probe_fail_total = 0;   // 探活判死次数
  std::int64_t ejected_until_ms = 0;    // 当前状态的到期时刻（单调毫秒；0 = 没被摘）
  std::int64_t last_ejection_epoch_ms = 0;  // 最近一次摘除的墙钟时刻（0 = 从未）；摘除耗时的"网关自报那一端"
  // --- S4-05（ADR-12 追加二第四节）：判据与恢复门的五格账，判据配对的"下界"就读在这里 --
  std::uint64_t rate_trips_total = 0;         // 被**错误率窗口**摘掉的次数
  bool half_open = false;                     // 此刻处于半开态
  std::uint64_t half_open_admitted_total = 0; // 被钉进半开周期的请求条数
  std::uint64_t half_open_denied_total = 0;   // 预算耗尽后在门口被挡的次数
  std::uint64_t half_open_recovered_total = 0; // 攒够证据、半开转全量恢复的次数
  std::uint64_t half_open_reopened_total = 0;  // 半开期内失败或到期未决而回摘的次数
};

// 一次策略读取（S5-02，ADR-10 追加节第四节末）。
//
// 为什么要有这么个东西而不是把 `policy_` 直接换成原子：熔断与摘除的阈值参数是文件重载
// 定义域内的东西（验收句① 括号里的 "circuit 参数"），而这张表被同组全部 worker 共享、
// 事件路径上每条请求都要读它——把策略换成运行期可换的东西，就必须同时回答
// "关闭态凭什么和今天逐字节相同"。答案是这根针：
//   · **重载没 arm**（`live_policy_` 为空）→ 借构造期那一份，一次分支、零原子操作。
//   · **arm 了** → 把当前世代的策略对象钉在返回对象里（一次原子 `shared_ptr` 读 + 一次
//     引用计数），引用在 view 的存活期内有效；换代由唯一的写者（watcher 那一拍）做，
//     读者永远拿到的是**整份**策略，不会读到"新窗口配旧半开门"那种拼出来的东西。
// 代价量在 B-020（arm vs 不 arm 两臂，主尺是每请求整数等式，不是 p99）。
class HealthTable;

class PolicyView {
 public:
  const HealthPolicy& operator*() const noexcept { return *policy_; }
  const HealthPolicy* operator->() const noexcept { return policy_; }

 private:
  // 只有 `HealthTable::policy_view()` 能造它：针要么由表内那份构造期策略借来（未 arm），
  // 要么由当前世代钉住（arm），两种都必须是表的私有判断，不能让调用方自己拼一个出来。
  // （构造函数私有而不是公开字段聚合初始化，是因为聚合要求成员全 public——那样谁都
  // 能造出一根没钉住任何东西的针。）
  friend class HealthTable;
  PolicyView(const HealthPolicy* policy, std::shared_ptr<const HealthPolicy> pin) noexcept
      : policy_(policy), pin_(std::move(pin)) {}

  const HealthPolicy* policy_ = nullptr;
  // 只有 arm 那一格非空。它指向的对象就是当前那份 `ConfigSnapshot` 里的策略成员——
  // 钉住策略就等于钉住世代，没有第二份抄本。
  std::shared_ptr<const HealthPolicy> pin_;
};

class HealthTable {
 public:
  // 窗口的分桶数是内部常量（ADR-12 追加二第一节：多一个旋钮只是多一类配置错误）。
  static constexpr std::size_t kWindowBuckets = 8;

  // `capacity` 是目标槽位的**构造期上界**（S3-04 节点表 ever-seen：新成员只能在这个
  // 空间里"启用"，不能让它运行期 resize——这张表被同组全部 worker 共享，运行期
  // 变长就是数据竞争）。0 = 容量与 target_count 相同（静态文件配置的现状形状，
  // 一字节不多花）。`target_count` 是**初始启用数**。
  //
  // `policy_reloadable`（S5-02）：true 时才允许 `set_policy`，读侧也才走那根针。
  // 它由装配方（`main.cc`）按"有没有 arm 文件重载"一次性定死，运行期不再变——
  // 所以关闭态那条分支是可预测的一次判断，不是一次原子读。
  HealthTable(std::size_t target_count, HealthPolicy policy,
              std::size_t capacity = 0, bool policy_reloadable = false) noexcept;

  // 失败判据成立时调用（被动：业务请求撞上失败；主动：探活判死，`source = kProbe`）。
  // `now_ms` 是单调毫秒（决策纯靠它，测试可以喂任意时间线）；`epoch_ms` 是墙钟毫秒，
  // 只进 `last_ejection_epoch_ms` 那格观测。
  FailureVerdict note_failure(std::uint16_t target, std::int64_t now_ms, std::int64_t epoch_ms,
                              FailureSource source = FailureSource::kBusiness) noexcept;
  // 活着判据成立时调用（业务响应 < 500 完整头）：连续失败计数清零。
  // **不解除摘除**——解除摘除只有两条路：摘除到期（进半开，S4-05）、探活成功（`revive`）。
  // `now_ms` 是 S4-05 加的：错误率窗口要它给样本落桶。**探活样本不走这里**
  // （ADR-12 追加二第一节：仪器不进被测方，窗口只装业务请求的真实体验）。
  void note_success(std::uint16_t target, std::int64_t now_ms) noexcept;
  // 主动探活一拍的结算（探活连接机的唯一入口）：判活 = probe_ok+1 + 连续计数清零 +
  // 解除摘除（`circuit` 装配时解除到的地方是**半开**，不是全量）；判死 = probe_fail+1 +
  // 走与被动同一条 note_failure 路径（`source = kProbe`）。
  // 两条路汇入同一个连续计数与同一个摘除出口，但**探活样本不进错误率窗口**
  // （`FailureSource` 那段），也不充当半开的成功证据——出半开只认业务请求的成功。
  // 反方向是故意的不对称：半开期内**探活判死仍然立即回摘**（成员资格上它一票否决，
  // S3-05 的口径"探活是成员资格的唯一真源"），fail-closed 的方向不需要两边对称。
  void note_probe_result(std::uint16_t target, bool alive, std::int64_t now_ms,
                         std::int64_t epoch_ms) noexcept;
  // 立即解除摘除（探活成功这条路的唯一入口）。返回是否由本次调用完成迁移
  // （多 worker 同时探活成功时 CAS 只有一个赢家；输家拿到 false，状态仍是"已解除"）。
  // `circuit` 装配时这一格迁移的目标是半开（ADR-12 追加二第七节把这条留在"全量出口"
  // 之外，是一条明知而留着的弱点，理由写在那里）。
  bool revive(std::uint16_t target, std::int64_t now_ms) noexcept;
  // 把已到期的状态推进一格（每轮循环 tick 扫一遍）：摘除到期 → 半开（装配熔断时）或
  // 健康（未装配时，S3-03 原样）；半开到期还没攒够证据 → 回摘。返回本次迁移的个数。
  // `epoch_ms = 0` 表示这条迁移不更新墙钟那一格（循环自己推的，时间线由外层脚本钉）。
  std::size_t sweep_expired(std::int64_t now_ms, std::int64_t epoch_ms = 0) noexcept;

  // "现在能不能被选中"——`LoadBalanceFilter` 每请求读的只有这一个问句（S4-05 起）。
  // 到期但还没被 sweep 的摘除在这里**按该进半开处理**，并由读到它的那个人做那次 CAS
  // 迁移（读侧顺手做迁移不是新规矩：`note_failure` 今天就在做"到期未扫顺手恢复"）。
  Admission admit(std::uint16_t target, std::int64_t now_ms) noexcept;

  // 这一台此刻是不是"摘除中"（`Admission::kDeny` 里由摘除贡献的那一半）。
  // 保留它是为了 S4-02 的表态归属：`note_health_failure` **之前**读它，用来分
  // "connect 失败的 502" 与 "全员被摘的 503"（口径⑫）。半开不是摘除——半开是我们
  // 主动放进去的探测，它失败就是普通的 502。
  bool is_ejected(std::uint16_t target, std::int64_t now_ms) const noexcept;

  // S4-06（ADR-23 第五节）：**重放落点的资格**。与 `admit` 回答的是同一个问题
  // （"这一台现在能接请求吗"），但它是**纯读**：不消费半开的探测名额、不做"到期未扫
  // 顺手进半开"的那次 CAS 迁移。为什么不复用 `admit`：
  //   ① 名额是恢复机的账（`half_open_admitted/denied` 只由均衡那一处决定），一条被重放的
  //      业务请求去占它，S4-05 刚钉住的那套配对断言就多了个没人数的入口；
  //   ② 重试只想知道"能不能落"，不想替循环推进状态——推进留给 `admit` 与 `sweep_expired`，
  //      否则同一个迁移有了两个发起者，正是 1.25 那条"记账点少是刻意的"要避免的事。
  // 取值口径与 `admit`/`is_ejected` 逐字对齐：健康 ✓；摘除中 ✗；半开 ✗（不管有没有名额）；
  // **到期未扫**：未装配熔断时按健康算（S3-03 的读侧原口径），装配熔断时 ✗——
  // 那一台已经归半开门管了，我不动它的门。
  bool retry_eligible(std::uint16_t target, std::int64_t now_ms) const noexcept;

  // 安全阀：可摘除上限 = floor(成员数 × max_ejection_percent / 100)。
  std::size_t max_ejected_count() const noexcept;
  // 读这一代的策略（见 `PolicyView` 那段：关闭态借构造期那份、零原子）。
  // 原来那个 `const HealthPolicy& policy()` 被它取代：返回引用在一个"策略可能已经换代"
  // 的表上是会悬垂的，而悬垂的方式正好是重载那一拍之后——留着一个只在关闭态安全的
  // 接口名，等于给后来人挖一条只有跑压测才会踩的坑。
  PolicyView policy_view() const noexcept;

  // 换代：唯一的写者是 watcher 那一拍（`ConfigReloader`），它在发布新世代**之前**调这一句，
  // 于是任何读到新世代的读者也一定能读到新策略。传进来的 `pin` 必须与快照里那份是同一个
  // 对象（同一根 `shared_ptr`），否则策略与表就又是两份真相。
  void set_policy(std::shared_ptr<const HealthPolicy> pin) noexcept;
  // 当前启用的目标数（valve 分母、sweep 范围、一切越界检查的界）。
  std::size_t target_count() const noexcept {
    return active_.load(std::memory_order_relaxed);
  }
  // ever-seen 增员（S3-04 reload）：把启用数抬到 n。多 worker 竞争取 max，
  // 槽位在构造期已全部置零，所以"启用"只是抬一个数。
  void ensure_active(std::size_t n) noexcept;
  std::size_t capacity() const noexcept { return entries_.size(); }

  // --- 观测面（全原子，聚合由读者自己做）-------------------------------------
  TargetHealthSnapshot snapshot(std::uint16_t target) const noexcept;
  // 此刻占着安全阀阀位的成员数（账本余额；与 max_ejected_count() 比对着读）。
  // **S4-05 起口径 = 摘除 + 半开**（ADR-12 追加二第二节第 5 条：把半开从账本里摘出去，
  // 10 台 10% 的组可以让 10 台各自半开、全组有效容量≈0，而账上写着"损失 0 台"——
  // ADR-12 量的是容量，不是状态名）。
  std::uint32_t ejected_now() const noexcept { return ejected_now_.load(std::memory_order_relaxed); }
  // 其中处于半开的有几台（退出账的 `circuit_half_open_now`）。
  std::uint32_t half_open_now() const noexcept {
    return half_open_now_.load(std::memory_order_relaxed);
  }
  // 到阈值但被安全阀挡住的次数（ADR-12 的"触发时有可观测计数"第二半）。
  std::uint64_t ejections_blocked_by_valve() const noexcept {
    return blocked_by_valve_.load(std::memory_order_relaxed);
  }
  // 候选集全空 → 回退全员发生了多少次（S4-05 的 `health_all_denied_fallback`；
  // 读者是 `LoadBalanceFilter`，它才是"空集"的现场）。
  std::uint64_t all_denied_fallbacks() const noexcept {
    return fallbacks_.load(std::memory_order_relaxed);
  }
  void note_all_denied_fallback() noexcept {
    fallbacks_.fetch_add(1, std::memory_order_relaxed);
  }

 private:
  // 状态一元字（ADR-12 追加二第五节）：`0` = 健康；非 0 = `(到期单调毫秒 << 1) | 状态位`，
  // 状态位 0 = 摘除、1 = 半开。三个状态仍由**一次 CAS** 迁移，所以"这个成员现在处于
  // 哪个状态"不允许有两个答案——本轮只是把词表从两个词扩到三个词。
  // 到期时刻 = now + 超时（两个超时都在配置层钉了 ≥1），所以非 0 值永不与"健康"撞车。
  static constexpr std::int64_t kHalfOpenBit = 1;
  static std::int64_t encode_open(std::int64_t deadline_ms) noexcept { return deadline_ms << 1; }
  static std::int64_t encode_half_open(std::int64_t deadline_ms) noexcept {
    return (deadline_ms << 1) | kHalfOpenBit;
  }
  static bool is_half_open(std::int64_t state) noexcept { return (state & kHalfOpenBit) != 0; }
  static std::int64_t deadline_of(std::int64_t state) noexcept { return state >> 1; }

  struct Entry;
  // 一次窗口记账（只有 `circuit.enabled` 时才发生）。出参是记账之后窗口内的
  // (请求数, 失败数)——判据要的就是这两个数，调用方不必再读第二次（读了也不是同一瞬间）。
  // 带 `policy` 入参而不是自己再读一次世代（S5-02）：一次判据里"窗口宽 = 哪一代"与
  // "阈值 = 哪一代"必须是同一个答案，否则换代的恰好那一刻会拼出一张没人配过的策略。
  void window_record(std::uint16_t target, std::int64_t now_ms, bool failure,
                     std::uint64_t* requests, std::uint64_t* failures,
                     const HealthPolicy& policy) noexcept;
  void window_clear(std::uint16_t target, std::int64_t now_ms,
                    const HealthPolicy& policy) noexcept;
  // 窗口分桶宽 = ⌊window_ms / 8⌋ 且 ≥1（ADR-12 追加二第一节）。熔断没装配时这个值不参与
  // 任何计算——窗口那条路整个被 `circuit.enabled` 挡在门外，一字节不多做。
  // 它从入参算而不是存成成员：S5-02 让 `window_ms` 成了可重载的东西，派生值再存一份
  // 就是第二份真相（`bucket_ms_` 那个成员因此被删掉，不是"留着不用"）。
  static std::uint64_t bucket_ms_of(const HealthPolicy& policy) noexcept;
  std::size_t max_ejected_count_for(const HealthPolicy& policy) const noexcept;

  // 摘除的 CAS。`from_half_open` 为真时是"半开期内回摘"：阀位已经占着，不再++，
  // 也不受安全阀阻挡（安全阀限的是**账本余额**，不是同一个成员的状态来回）。
  // 赢家负责计数器与 `last_ejection_epoch_ms`；输家不做账（S3-03 的纪律一字未动）。
  bool try_mark_ejected(std::uint16_t target, std::int64_t now_ms, std::int64_t epoch_ms,
                        bool from_half_open, const HealthPolicy& policy) noexcept;
  // 摘除到期 → 半开（CAS 赢家负责：清连续计数、清半开周期计数、`window_clear`）。
  bool try_enter_half_open(std::uint16_t target, std::int64_t expected_state,
                           std::int64_t now_ms, const HealthPolicy& policy) noexcept;
  // 摘除 → 全量恢复（CAS 赢家负责 `ejected_now_--`，因为这一台重新开始接全量流量了）。
  bool try_mark_healthy(std::uint16_t target, std::int64_t expected_state) noexcept;
  // 半开门：预算内抢到一个名额返回 true（低 32 位 = 已放行，高 32 位 = 成功证据）。
  bool try_take_probe_slot(std::uint16_t target, const HealthPolicy& policy) noexcept;
  // 记一条半开期内的业务成功；攒满 K 时由本函数尝试转健康（CAS 赢家才计恢复数）。
  void note_half_open_success(std::uint16_t target, const HealthPolicy& policy) noexcept;

  struct WindowBucket {
    std::uint64_t seq = 0;       // 这一格代表的绝对桶号（单调毫秒 / bucket_ms）
    std::uint32_t requests = 0;  // 上限：一格 4.29e9 条——比一台后端能撞到的量高两个数量级
    std::uint32_t failures = 0;
  };

  struct Entry {
    std::atomic<std::uint32_t> consecutive_failures{0};
    std::atomic<std::uint64_t> failures_total{0};
    std::atomic<std::uint64_t> ejections_total{0};
    std::atomic<std::uint64_t> recoveries_total{0};
    std::atomic<std::uint64_t> probe_ok_total{0};
    std::atomic<std::uint64_t> probe_fail_total{0};
    std::atomic<std::uint64_t> rate_trips_total{0};
    std::atomic<std::uint64_t> half_open_admitted_total{0};
    std::atomic<std::uint64_t> half_open_denied_total{0};
    std::atomic<std::uint64_t> half_open_recovered_total{0};
    std::atomic<std::uint64_t> half_open_reopened_total{0};
    // 状态的**唯一**字：见上面 kHalfOpenBit 那段。所有状态迁移都是对它的 CAS。
    std::atomic<std::int64_t> state_ms{0};
    std::atomic<std::int64_t> last_ejection_epoch_ms{0};
    // 半开周期内的两个计数打包在一个字里：低 32 = 已放行、高 32 = 已攒到的业务成功。
    // 打包是为了"进/出半开时一次 store 清零"，不留"清了放行没清成功"的中间态。
    std::atomic<std::uint64_t> half_open_cycle{0};
    // 错误率窗口（本文件头部那段例外说明）。
    std::mutex window_mu;
    std::array<WindowBucket, kWindowBuckets> window_buckets{};
    std::uint64_t window_seq = 0;
    std::uint64_t window_requests = 0;
    std::uint64_t window_failures = 0;
  };

  HealthPolicy policy_;
  // S5-02：当前世代的策略（只有 arm 了文件重载才会非空）。它指向的就是那份
  // `ConfigSnapshot` 里的 `health`——同一根 `shared_ptr`，不是抄本。
  // 写者只有 watcher 那一拍，且排在发布新世代之前；读者经 `policy_view()` 钉住它。
  std::atomic<std::shared_ptr<const HealthPolicy>> live_policy_{nullptr};
  // 一次性定死的开关（构造期由装配方给）：false 时 `policy_view()` 连那次原子读都不做。
  bool policy_reloadable_ = false;
  std::vector<Entry> entries_;  // 容量 = 构造期上界；[0, active_) 才是被启用的目标
  std::atomic<std::size_t> active_;
  std::atomic<std::uint32_t> ejected_now_{0};
  std::atomic<std::uint32_t> half_open_now_{0};
  std::atomic<std::uint64_t> blocked_by_valve_{0};
  std::atomic<std::uint64_t> fallbacks_{0};
};

}  // namespace l7gate::route
