#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include "route/balance.h"
#include "route/health.h"
#include "route/nodes.h"
#include "route/router.h"

// 配置热重载的双 buffer（S5-02，ADR-10 落地追加节第三节）。
//
// 三样东西各管一件事，形状全部照抄 ADR-20 第四节那一节已经跑通的东西：
//
//   **ConfigSnapshot** —— 一份**装配完整**的策略世代：路由表（正则已编译）、上游表快照
//     （`NodesView`，环已建）、健康与熔断策略。构造之后没有任何写点：读者拿到的永远是
//     "整一代"，不存在"读到半张环"那一刻（ADR-20 第四节第二条禁止的就是那个）。
//     它的构造/析构各打一次数——验收句②「旧快照在最后一个引用释放前不回收」就是靠
//     这三格测出来的，不是靠"逻辑上 shared_ptr 会延命"说的（口径与
//     `stats.h` 那对 `connections_constructed`/`destructed` 同一条：在册 = 构造 − 析构）。
//
//   **ConfigPublisher** —— 发布点：`atomic<shared_ptr<const ConfigSnapshot>>`。
//     **全网关"配置换代"只有这一处 store**，写者只有一个（watcher 那一拍）；worker 侧
//     一律 `load`。加在它身上的账是"检查/发布/拒载"三格分开的——查了一遍不等于发布了
//     一次，这条口径来自 ADR-20 第五节那对 `nodes_reloads_*`。
//
//   **ConfigReloadHook** —— worker 循环那一拍的接口。实现住在 `config/`（它要读文件、
//     要跑 JSON 校验器），`net/` 只认这一个虚函数：这样 net 层不但不依赖 config 层，
//     连"配置文件长什么样"都不知道。nullptr = 不跑重载，零行为差异（`nodes.h` 那条
//     「nullptr 是实现位置不是一条 if」的口径在这里同样成立）。
//
// 与节点表发布器（`NodesPublisher`）的关系：**互斥**。首装里有 `source: mysql` 组的网关
// 在启动时就被拒绝 arm 文件重载（ADR-10 追加节第一节第 7 条），所以一台网关只有一个
// 策略发布点，不存在两条节拍往同一个下标空间写的合并规则（已裁② 的理由在这里原样适用）。
namespace l7gate::route {

struct ConfigSnapshot {
  ConfigSnapshot() noexcept;
  ~ConfigSnapshot() noexcept;

  // 全仓库同时活着的世代数（观测用；判定要用的是等式 live == constructed - destructed）。
  static std::uint64_t constructed() noexcept;
  static std::uint64_t destructed() noexcept;
  static std::uint64_t live() noexcept;

  std::uint32_t generation = 0;    // 首装 = 1，每次成功发布 +1；拒载不动
  std::uint64_t content_hash = 0;  // 这一代来自的文件内容哈希（FNV-1a 64）

  // 路由表。文件里没有 `routes` 段时它是空的——**空表读作"不装路由 filter"**，
  // 与 `GatewayConfig::has_routes` 同一条语义（不是"有一条 catch-all"）。
  RouteTable routes;

  // 健康与熔断策略。`HealthTable` 的策略槽指向的就是这一根（同一份真相，没有抄本）。
  std::shared_ptr<const HealthPolicy> health;

  // 上游表快照：组表（成员 = ever-seen 目标下标）+ ever-seen 目标列表。
  // **永不为 nullptr**——装配不完整就不发布，这是发布点的不变式，不是调用方的检查项。
  std::shared_ptr<const NodesView> nodes;
};

// 发布点 + 重载这一族的账。
class ConfigPublisher {
 public:
  std::shared_ptr<const ConfigSnapshot> load() const noexcept {
    return current_.load(std::memory_order_acquire);
  }
  // 唯一的换代处。**只在装配完整之后调用**（调用方负责，见 `ConfigReloader`）。
  void publish(std::shared_ptr<const ConfigSnapshot> snapshot) noexcept {
    current_.store(std::move(snapshot), std::memory_order_release);
  }
  bool armed() const noexcept { return current_.load(std::memory_order_acquire) != nullptr; }

  // --- 账（验收句④「拒绝有账」与第五节那 11 个键的落点）---------------------------
  void note_check() noexcept { checks_.fetch_add(1, std::memory_order_relaxed); }
  void note_publish(std::int64_t epoch_ms, std::int64_t duration_ms) noexcept {
    publishes_.fetch_add(1, std::memory_order_relaxed);
    last_publish_epoch_ms_.store(epoch_ms, std::memory_order_relaxed);
    last_publish_duration_ms_.store(duration_ms, std::memory_order_relaxed);
  }
  // 语法错与"越界/不在定义域"分两格：面板要能各画一条线，而"这一轮为什么没生效"
  // 的两种答案对运维来说不是同一件事（一个是文件写坏了，一个是我改了不许改的东西）。
  void note_reject_syntax() noexcept {
    rejects_.fetch_add(1, std::memory_order_relaxed);
    rejects_syntax_.fetch_add(1, std::memory_order_relaxed);
  }
  void note_reject_scope() noexcept {
    rejects_.fetch_add(1, std::memory_order_relaxed);
    rejects_scope_.fetch_add(1, std::memory_order_relaxed);
  }

  std::uint64_t checks() const noexcept { return checks_.load(std::memory_order_relaxed); }
  std::uint64_t publishes() const noexcept {
    return publishes_.load(std::memory_order_relaxed);
  }
  std::uint64_t rejects() const noexcept { return rejects_.load(std::memory_order_relaxed); }
  std::uint64_t rejects_syntax() const noexcept {
    return rejects_syntax_.load(std::memory_order_relaxed);
  }
  std::uint64_t rejects_scope() const noexcept {
    return rejects_scope_.load(std::memory_order_relaxed);
  }
  std::int64_t last_publish_epoch_ms() const noexcept {
    return last_publish_epoch_ms_.load(std::memory_order_relaxed);
  }
  std::int64_t last_publish_duration_ms() const noexcept {
    return last_publish_duration_ms_.load(std::memory_order_relaxed);
  }
  std::uint32_t generation() const noexcept {
    const std::shared_ptr<const ConfigSnapshot> snap = load();
    return snap == nullptr ? 0u : snap->generation;
  }

 private:
  std::atomic<std::shared_ptr<const ConfigSnapshot>> current_{nullptr};
  std::atomic<std::uint64_t> checks_{0};
  std::atomic<std::uint64_t> publishes_{0};
  std::atomic<std::uint64_t> rejects_{0};
  std::atomic<std::uint64_t> rejects_syntax_{0};
  std::atomic<std::uint64_t> rejects_scope_{0};
  std::atomic<std::int64_t> last_publish_epoch_ms_{0};
  std::atomic<std::int64_t> last_publish_duration_ms_{0};
};

// worker 循环那一拍要调的东西。`now_ms` 是循环自己已经读过的那个单调毫秒——
// 传进来是为了让"到点没到点"这件事不额外发一次 clock 读数（ADR-10 追加节第二节）。
class ConfigReloadHook {
 public:
  ConfigReloadHook() = default;
  ConfigReloadHook(const ConfigReloadHook&) = delete;
  ConfigReloadHook& operator=(const ConfigReloadHook&) = delete;
  virtual ~ConfigReloadHook() = default;

  // **不许抛**：调用点在事件循环里，抛出就是 std::terminate。所有失败走计数器与 stderr。
  virtual void reload_tick(std::int64_t now_ms) noexcept = 0;
};

}  // namespace l7gate::route
