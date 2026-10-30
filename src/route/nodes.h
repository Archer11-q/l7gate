#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "route/balance.h"
#include "route/router.h"

// 节点表的动态来源（S3-04，ADR-20 第二/四节）。
//
// 三样东西各管一件事，混任何一个都会造出第二份真相：
//
//   **TargetRegistry** —— "曾经见过的目标"的只增注册表：端点串 → 全局目标下标。
//     它存在的理由是 ADR-20 第四节那条：目标下标空间 **ever-seen、只增不减**——
//     `Connection::upstream_target_` 标签、池的桶、per-worker 配对账、健康表全部
//     按这个下标索引，reload 换表时存活成员必须拿到**同一个**下标，新成员**追加**
//     新下标。互斥锁保护；只有装配方（main 启动）与 reload 线程碰它，worker 线程
//     只经发布的快照读到下标，从不直接碰注册表。
//
//   **NodesView** —— 一份**装配完整**的快照：组表（成员指向 ever-seen 下标）+
//     ever-seen 目标列表。哈希环在发布前建好（装配期建环的纪律在快照上原样成立），
//     发布出去的东西不需要任何人再"补一步"。
//
//   **NodesPublisher** —— 发布点：`atomic<shared_ptr<const NodesView>>`。
//     reload 线程 store，worker 侧（LB filter / reactor 的对账 tick）load。
//     跨线程流动的只有不可变快照指针——连接对象、fd、per-worker 状态一概不流动，
//     ADR-14 的接触面不因 reload 变大（tsan 零 data race 是这条的裁判）。
namespace l7gate::route {

// ever-seen 目标表的一行。与 `Reactor::Config::Target` 的字段一一对应但不复用那个类型：
// route 层不依赖 net 层，main.cc 负责搬运（一份字段搬运胜过一层反向依赖）。
struct NodeTarget {
  std::string address;
  std::uint16_t port = 0;
  std::string health_path = "/";
  std::string endpoint_key() const { return address + ":" + std::to_string(port); }
};

class TargetRegistry {
 public:
  // 端点已见过 → 返回它的既有下标；没见过 → 追加新下标。
  // 下标空间用尽（> kMaxUpstreamTarget）返回 kNoUpstreamTarget。
  std::uint16_t intern(const NodeTarget& target);

  std::uint16_t size() const;
  // 越界返回 nullptr。调用方持锁纪律：reload 线程在 build 阶段整段持有锁读列表。
  const NodeTarget* at(std::uint16_t index) const;
  // 建环用的端点键（下标对齐 targets()）。
  std::vector<std::string> endpoint_keys() const;

 private:
  mutable std::mutex mutex_;
  std::vector<NodeTarget> targets_;
  std::unordered_map<std::string, std::uint16_t> by_endpoint_;
};

// 一次 MySQL 读到的成员集（按组名给，组必须已在文件里声明——校验在 store 侧做完）。
struct NodesFetch {
  struct GroupMembers {
    std::string name;
    std::vector<NodeTarget> members;
  };
  std::vector<GroupMembers> groups;
};

// 装配一份快照：文件侧的组定义（balance/vnodes/hash_key/静态成员）+ MySQL 侧的成员集。
// **任何一步失败都返回 nullptr 且不留下半张快照**——发布半张环等于让迁移率
// "按我少算的那个数"成立（ADR-19 对环缺成员的原话，对整张快照同样成立）。
// `file_groups` 是文件建出来的那张表（组的顺序 = 下标顺序，reload 不得改变它）；
// `fetch` 给 mysql 组的成员（文件组的静态成员从 `file_groups` 原样带过来）。
// `registry` 是**非 const**：新成员在这一步 intern 进 ever-seen（下标空间只增）。
std::shared_ptr<const struct NodesView> build_nodes_view(const GroupTable& file_groups,
                                                         const NodesFetch& fetch,
                                                         TargetRegistry& registry,
                                                         std::string* error);

struct NodesView {
  GroupTable groups;                     // 成员 = ever-seen 目标下标
  std::vector<NodeTarget> targets;       // ever-seen 目标列表（下标对齐）
};

// 发布点。**nullptr 语义 = 本网关不跑节点表 reload**（纯文件配置），持有方全部
// 走既有路径——零行为差异是"没装配"的实现位置，不是一条 if。
class NodesPublisher {
 public:
  std::shared_ptr<const NodesView> load() const noexcept {
    return current_.load(std::memory_order_acquire);
  }
  void publish(std::shared_ptr<const NodesView> view) noexcept {
    current_.store(std::move(view), std::memory_order_release);
  }
  bool has_snapshot() const noexcept { return current_.load(std::memory_order_acquire) != nullptr; }

  // --- 观测面（B-015 的"网关自报那一端"）-------------------------------------
  void note_reload(bool ok, std::int64_t epoch_ms, std::int64_t duration_ms) noexcept;
  std::uint64_t reloads_ok() const noexcept { return reloads_ok_.load(std::memory_order_relaxed); }
  std::uint64_t reloads_failed() const noexcept {
    return reloads_failed_.load(std::memory_order_relaxed);
  }
  std::int64_t last_publish_epoch_ms() const noexcept {
    return last_publish_epoch_ms_.load(std::memory_order_relaxed);
  }

 private:
  std::atomic<std::shared_ptr<const NodesView>> current_{nullptr};
  std::atomic<std::uint64_t> reloads_ok_{0};
  std::atomic<std::uint64_t> reloads_failed_{0};
  std::atomic<std::int64_t> last_publish_epoch_ms_{0};
};

}  // namespace l7gate::route
