#include "route/balance.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

namespace l7gate::route {
namespace {

// FNV-1a 的两个常数按 64 位版本的字面值写死（offset basis 与 prime）。
// 它们不是"参数"：换掉任何一个都等于换掉整张 key→后端 的映射，所以这里给名字而不给入口。
constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;  // 0xcbf29ce484222325
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;               // 0x100000001b3

// FNV-1a 之后的一道终值混合（splitmix64 的三步移位-乘法）。**这是实测逼出来的，不是装饰**：
// 环的点键形如 `组名#序号#端点`，而同一个组里的成员之间只有末尾那截 `127.0.0.1:18081` /
// `…18082` 不同。FNV-1a 是逐字节的 `h = (h ^ c) * prime`，只差末位字节意味着两次哈希只差
// `Δ × prime`——相对 2^64 的量级是 6e-8，于是每个成员的 160 个点全挤在彼此 6e-8 的窗口里，
// 环不是"均匀撒点"而是"一簇一簇"。实测（仓库内可复跑的那条用例
// `S3_02_MigrationRate.AddingOneBackendMovesAtMostOneInNewTotal`，10,000 个确定性 key；
// 把下面的 `ring_hash` 换回裸 `fnv1a64` 就是这组数）：
// N→N+1 的迁移率是 **48.10%（N=3）/ 40.10%（N=7）**，理想界是 25% / 12.5%，
// 而且**把 vnodes 从 160 抬到 10,000 一点都不改**（48.39% / 52.74%，那轮是仓库外的一次性扫参，
// 5 套端口基线 × N=2/3/7/15 取的是一致偏高的那一侧）——因为簇数和簇宽都不由 vnodes 决定。
// 加这道终值之后，同一条用例的读数是 **24.41% / 11.11%**，两条都落回理想界内。
//
// 代价按 ADR-19 第五条原样记：**换哈希等于换掉所有 key→后端 的映射**（迁移率 100%），那是
// 运维事件不是代码细节；所以这里改的是"哈希函数"这一层并被写进 ADR，而不是悄悄换个更聪明的
// 点键拼法。ADR 给的另一个旋钮是 vnodes，实测它治不了这个病。
std::uint64_t mix64(std::uint64_t h) noexcept {
  h ^= h >> 30;
  h *= 0xbf58476d1ce4e5b9ULL;
  h ^= h >> 27;
  h *= 0x94d049bb133111ebULL;
  h ^= h >> 31;
  return h;
}

}  // namespace

const char* to_string(Balance balance) noexcept {
  switch (balance) {
    case Balance::kRoundRobin: return "round_robin";
    case Balance::kLeastConnections: return "least_conn";
    case Balance::kConsistentHash: return "consistent_hash";
  }
  return "?";  // 全部三个值都已列出；走到这里只可能是枚举加了新值
}

bool parse_balance(std::string_view text, Balance* out) noexcept {
  // 刻意只认这三个字面量，也不做大小写折叠：配置文件里 `Balance` 与 `balance` 是两个不同的
  // 键名这件事不该由我猜。拼错的策略名退化成"用了缺省策略"是典型的安静失效——
  // 它会跑、会均衡、只是不是你要的那一种。
  if (text == "round_robin") { *out = Balance::kRoundRobin; return true; }
  if (text == "least_conn") { *out = Balance::kLeastConnections; return true; }
  if (text == "consistent_hash") { *out = Balance::kConsistentHash; return true; }
  return false;
}

bool GroupTable::add_group(std::string_view name, Balance balance,
                           std::uint16_t* group_index) noexcept {
  if (name.empty()) return false;
  if (group_by_name(name) != nullptr) return false;  // 组名重复：两条组抢一个名字，没有合理答案
  if (groups_.size() > static_cast<std::size_t>(kMaxUpstreamGroup)) return false;
  UpstreamGroup group;
  group.name = std::string(name);
  group.balance = balance;
  *group_index = static_cast<std::uint16_t>(groups_.size());
  groups_.push_back(std::move(group));
  return true;
}

bool GroupTable::add_member(std::uint16_t group, std::uint16_t target) noexcept {
  UpstreamGroup* g = group >= groups_.size() ? nullptr : &groups_[group];
  if (g == nullptr) return false;
  if (target == kNoUpstreamTarget) return false;
  // 同一组里同一个后端出现两次 → 拒。理由写在头文件里：它会让"这个组有几个后端"有两个答案，
  // 而 N→N+1 的迁移率用例第一个就被它骗（我加的第二个成员根本不是新后端）。
  if (std::find(g->targets.begin(), g->targets.end(), target) != g->targets.end()) return false;
  g->targets.push_back(target);
  return true;
}

const UpstreamGroup* GroupTable::group_at(std::uint16_t group) const noexcept {
  if (group >= groups_.size()) return nullptr;
  return &groups_[group];
}

const UpstreamGroup* GroupTable::group_by_name(std::string_view name) const noexcept {
  for (const UpstreamGroup& group : groups_) {
    if (group.name == name) return &group;
  }
  return nullptr;
}

std::uint16_t GroupTable::group_index_by_name(std::string_view name) const noexcept {
  for (std::size_t i = 0; i < groups_.size(); ++i) {
    if (groups_[i].name == name) return static_cast<std::uint16_t>(i);
  }
  return kNoUpstreamGroup;
}

const std::vector<std::uint16_t>* GroupTable::members_of(std::uint16_t group) const noexcept {
  const UpstreamGroup* g = group_at(group);
  return g == nullptr ? nullptr : &g->targets;
}

void LbState::note_paired(std::uint16_t target) noexcept {
  if (target >= target_pairings_.size()) return;  // 越界只可能是装配漏项；不猜、不扩表
  ++target_pairings_[target];
}

void LbState::note_unpaired(std::uint16_t target) noexcept {
  if (target >= target_pairings_.size()) return;
  // 夹到 0：宽松/坏上游多回一次收尾时不让 uint32 下溢成 42 亿，
  // 那会让"这条后端最闲"的判据永远选不中它（和 ADR-16 那个在飞计数下溢同一个理由）。
  if (target_pairings_[target] == 0u) return;
  --target_pairings_[target];
}

std::uint32_t LbState::pairings(std::uint16_t target) const noexcept {
  if (target >= target_pairings_.size()) return 0u;
  return target_pairings_[target];
}

std::size_t LbState::advance_rr(std::uint16_t group, std::size_t member_count) noexcept {
  if (member_count == 0u) return 0u;
  if (group >= rr_cursor_.size()) return 0u;
  // 游标本身就存成 mod member_count，而不是"一直自增、用时取模"：后者的游标会在
  // 长期运行后触到 uint32 的边界，回绕那一刻"下一个是谁"会变——那是一条会偶发、
  // 又没有任何用例能稳定抓到的不均。存 mod 值就没这件事。
  const std::size_t picked = rr_cursor_[group] % member_count;
  rr_cursor_[group] = static_cast<std::uint32_t>((picked + 1u) % member_count);
  return picked;
}

std::uint64_t fnv1a64(std::string_view text) noexcept {
  std::uint64_t hash = kFnvOffsetBasis;
  for (const char c : text) {
    hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
    hash *= kFnvPrime;  // 64 位溢出是 well-defined 的回绕，FNV 的定义就用的是这个低 64 位
  }
  return hash;
}

// 环上用的那一个哈希：FNV-1a 之后再走一道终值混合。**key 和点必须走同一个函数**——
// 只把点混合、key 还按裸 FNV 取值，等于拿一把没刻度的尺去量一把被拉直过的尺：
// URI 之间常常只差末位（`/orders/1` vs `/orders/2`），裸 FNV 会把它们也挤成一小簇，
// 于是"同一 key 恒定同一后端"仍然成立，但哪些 key 落在哪一段就又是簇状的。
std::uint64_t ring_hash(std::string_view text) noexcept { return mix64(fnv1a64(text)); }

bool build_hash_ring(UpstreamGroup* group, const std::vector<std::string>& endpoint_keys) noexcept {
  if (group == nullptr) return false;
  group->ring.clear();
  if (group->balance != Balance::kConsistentHash) return true;  // 不读环的策略不需要建，也别给它建一份没人读的
  if (group->targets.empty()) return false;  // 空组：装配期就该挡在这里
  if (group->vnodes == 0u) return false;     // vnodes=0 → 环是空的，"选一个"永远无解
  std::string point;
  for (const std::uint16_t target : group->targets) {
    if (target >= endpoint_keys.size()) {
      group->ring.clear();  // 不留半张环：缺一个成员的环，它的迁移率数字不可解释
      return false;
    }
    const std::string& key = endpoint_keys[target];
    for (std::uint32_t v = 0; v < group->vnodes; ++v) {
      point = group->name;
      point += '#';
      point += std::to_string(v);
      point += '#';
      point += key;
      group->ring.emplace_back(ring_hash(point), target);
    }
  }
  // 破平规则写死（哈希值相同则按目标下标）：不写这一条，同一份配置在两次构建里可能给出
  // 两个不同的环顺序，而"同一 key 恒定同一后端"那条断言就会开始随机红。
  std::sort(group->ring.begin(), group->ring.end(),
            [](const std::pair<std::uint64_t, std::uint16_t>& a,
               const std::pair<std::uint64_t, std::uint16_t>& b) {
              if (a.first != b.first) return a.first < b.first;
              return a.second < b.second;
            });
  return true;
}

std::optional<std::uint16_t> select_from_ring(const UpstreamGroup& group,
                                              std::string_view key) noexcept {
  if (group.ring.empty()) return std::nullopt;
  const std::uint64_t h = ring_hash(key);
  auto it = std::lower_bound(group.ring.begin(), group.ring.end(), h,
                             [](const std::pair<std::uint64_t, std::uint16_t>& node,
                                std::uint64_t value) { return node.first < value; });
  // 环是圆的：超过最后一个点就回到第一个。写成 end→begin 而不是"取 begin"的另一处调用，
  // 是为了让"回绕"这件事在代码里只出现一次。
  if (it == group.ring.end()) it = group.ring.begin();
  return it->second;
}

bool is_candidate(const std::vector<std::uint16_t>& sorted_candidates,
                  std::uint16_t target) noexcept {
  return std::binary_search(sorted_candidates.begin(), sorted_candidates.end(), target);
}

std::optional<std::uint16_t> select_from_ring(const UpstreamGroup& group, std::string_view key,
                                              const std::vector<std::uint16_t>& candidates) noexcept {
  if (group.ring.empty()) return std::nullopt;
  // 候选集 = 全员：与两参版本逐点同解，直接走快路径（健康检查没摘人时不付扫描的钱）。
  if (candidates.size() == group.targets.size()) {
    return select_from_ring(group, key);
  }
  const std::uint64_t h = ring_hash(key);
  auto it = std::lower_bound(group.ring.begin(), group.ring.end(), h,
                             [](const std::pair<std::uint64_t, std::uint16_t>& node,
                                std::uint64_t value) { return node.first < value; });
  // 最多绕环一整圈：环上每个点都查一遍谓词，全员被摘时落到 nullopt，
  // 由调用方回退全员集合——"宁可发给坏后端，不让请求无去处"（自产响应归 S4）。
  for (std::size_t seen = 0; seen < group.ring.size(); ++seen) {
    if (it == group.ring.end()) it = group.ring.begin();
    if (is_candidate(candidates, it->second)) return it->second;
    ++it;
  }
  return std::nullopt;
}

}  // namespace l7gate::route
