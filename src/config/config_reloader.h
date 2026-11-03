#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "config/gateway_config.h"
#include "route/config_snapshot.h"

// 配置热重载的执行体（S5-02，ADR-10 落地追加节第二、三、六节）。
//
// 它站在 `config/` 而不是 `net/`，理由只有一个：**"文件长什么样"是配置层的事**。
// worker 的循环只经 `route::ConfigReloadHook` 那个虚函数拍一下自己，net 层因此
// 不知道 JSON、不知道字段清单、也不知道哪些东西不许重载——它只负责"到点了问一句"。
//
// 一次 tick 做四件事，顺序不可换（每一件事都可能把这一轮结束掉）：
//
//   一、到点判定：用循环传进来的单调毫秒比一次时间戳。没到点**一个 syscall 都不发**。
//   二、`stat` 比 mtime + size。没变就只记一次检查账（`config_checks`）就走。
//   三、变了才读文件 + 算内容哈希（FNV-1a 64）。哈希相同 = 有人 `touch` 了一下，
//       **不算一次重载**——"查了一遍不等于发布了一次"是 ADR-20 第五节那对计数器立的口径。
//   四、装配：解析 → 校验（与启动、与用例同一个 `build_gateway_config`，不另起一份校验）
//       → 建环 → 建快照。全部成功才 `publish` 一次；任何一步失败都只加拒载账，
//       在册世代纹丝不动（半套发布在这台网关里没有代码路径，因为换代只有那一次 store）。
//
// 定义域外的事一律拒载并点名（第一节那五条）：装配形状变化、限流参数变化、
// mysql 组、命令行来源的键、文件超过 1 MiB。
namespace l7gate::config {

// 从一个已经校验完的 `GatewayConfig` 造出完整的一代快照。
// 失败返回 nullptr 并在 `error` 里留下一句话，**不留下半张快照**。
// 环在返回之前建好（装配期建环的纪律在快照上原样成立，ADR-20 第四节第二条）。
// `registry` 是 ever-seen 目标表：文件里的成员按 `地址:端口` 对齐到既有下标，新地址追加新下标。
// **注意**：新地址在本函数里就被 intern 了，而函数后面还可能失败——失败那一轮的代价是
// 下标空间里多出几个"没被任何已发布组引用"的槽位，它不接流量、不进候选集、
// 不占安全阀分母（分母读的是已发布的那张表），理由与 ADR-10 追加节第六节末同。
std::shared_ptr<const l7gate::route::ConfigSnapshot> build_config_snapshot(
    const GatewayConfig& config, std::uint64_t content_hash, std::uint32_t generation,
    l7gate::route::TargetRegistry& registry, std::string* error);

// "这一份配置装出了什么形状"——重载定义域检查比的就是它（追加节第一节第 4、5、7 条）。
// 为什么不带整份 `GatewayConfig` 当对照：它是不可复制也不可移动的（表禁止抄本，
// `router.h` 那条），而这里要的只是几个布尔与两个数字。把比较面收窄成这七个字段
// 本身就是那条定义域的落点：**不在这张结构里的东西都可以重载**。
struct ScopeShape {
  bool has_routes = false;
  bool health_present = false;
  bool rate_limit_present = false;
  std::uint64_t rate_per_sec = 0;
  std::uint64_t burst = 0;
  bool by_header = false;
  std::string header_name;
  bool any_mysql_group = false;
};

ScopeShape scope_shape_of(const GatewayConfig& config) noexcept;

class ConfigReloader : public l7gate::route::ConfigReloadHook {
 public:
  ConfigReloader(std::string path, std::int64_t interval_ms,
                 std::shared_ptr<l7gate::route::ConfigPublisher> publisher,
                 std::shared_ptr<l7gate::route::HealthTable> health,
                 std::shared_ptr<l7gate::route::TargetRegistry> registry,
                 const GatewayConfig& initial) noexcept;

  // 循环那一拍调的入口。不许抛（`ConfigReloadHook` 那条契约）。
  void reload_tick(std::int64_t now_ms) noexcept override;

  // 当前已发布那一代来自的文件内容哈希（启动时装配那一代也要登记它，
  // 否则第一次 tick 会把"文件没变"读成"变更待发布"）。
  void note_published(std::uint64_t content_hash) noexcept;

  // 供用例与冒烟读的那一格：这一轮 reload 为什么被拒（最后一次拒载的原因原文，
  // 空 = 从来没被拒过）。退出账与 `[serve]` 行都打它，因为分类计数回答不了
  // "到底是哪个字段"。
  const std::string& last_reject_reason() const noexcept { return last_reject_reason_; }

  // 算一份配置文件的内容哈希（首装那一代也要登记它，否则第一次 tick 会把"文件没变"
  // 读成"变更待发布"）。**与 tick 里那一次同一个函数、同一个大小上界**——启动算一份、
  // 运行期算另一份的话，两份实现迟早给出两个答案，表现就是"改了文件而网关不动"。
  static bool file_content_hash(const std::string& path, std::uint64_t* hash,
                                std::string* error) noexcept;

  // 配置文件的大小上界（与 `load_gateway_config` 读文件时那一条同一个数，
  // 不是我为运行期新立的规矩——两份上界迟早不一样高）。
  static constexpr std::size_t kMaxConfigBytes = 1u << 20;

 private:
  // 第一节那五条"不许换"的检查。返回 false 时 `reason` 里是一句点名的话。
  bool in_scope(const ScopeShape& next, std::string* reason) const noexcept;

  std::string path_;
  std::int64_t interval_ms_ = 0;
  // 写侧拿的是**非 const** 的发布点（它要记账）；worker 侧拿的是 `shared_ptr<const ...>`。
  // 同一个对象、两种持有者视角，账只有一个人能记这件事因此是类型层面写死的。
  std::shared_ptr<l7gate::route::ConfigPublisher> publisher_;
  std::shared_ptr<l7gate::route::HealthTable> health_;
  std::shared_ptr<l7gate::route::TargetRegistry> registry_;
  // 当前在册那一代的形状（比较"变了的是不是不许变的东西"）。
  ScopeShape current_shape_;
  std::uint64_t published_hash_ = 0;
  std::int64_t next_check_ms_ = 0;
  // 上一次 `stat` 看到的 (mtime, size)——用它决定要不要读文件。
  std::int64_t last_mtime_ns_ = -1;
  std::uint64_t last_size_ = 0;
  std::string last_reject_reason_;
};

}  // namespace l7gate::config
