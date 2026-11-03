#include "config/config_reloader.h"

#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string_view>
#include <utility>

#include "config/json.h"
#include "route/balance.h"
#include "route/health.h"

namespace l7gate::config {
namespace {

// 内容哈希用 `route::fnv1a64`——环上那套映射用的就是它（ADR-19 第五条），这里再造一份
// 字面量相同的实现就是两份真相：一份改了另一份没改的话，"文件内容变没变"这件事
// 会有两个答案。为什么自己写而不是"看起来够用就行"：这一格要回答的问题是
// "文件内容变没变"，而 `stat` 的 mtime 回答不了它（`touch`、同秒内改两次、
// 复制一份一模一样的文件都会动 mtime 而不动内容）。引一个哈希库是禁令三下的新依赖，
// 而 FNV-1a 是十几行、无状态、跨版本稳定的算法——**它不是密码学哈希，也不需要是**：
// 这里没有对手，只有编辑器与部署脚本。
std::uint64_t content_hash_of(std::string_view text) noexcept {
  return l7gate::route::fnv1a64(text);
}

std::int64_t epoch_ms_now() noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::int64_t monotonic_ms_now() noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// 读文件 + 算哈希（`ConfigReloader::file_content_hash` 与 tick 共用它，见头文件那条理由）。
bool read_config_file(const std::string& path, std::string* text, std::uint64_t* hash,
                      std::string* error) {
  struct stat stamped {};
  if (::stat(path.c_str(), &stamped) != 0) {
    *error = "stat 失败：" + path;
    return false;
  }
  // 大小上界在**读之前**判：那格防御要挡的是"把一百兆读进内存再决定不用它"。
  if (static_cast<std::uint64_t>(stamped.st_size) > ConfigReloader::kMaxConfigBytes) {
    *error = "配置文件 " + std::to_string(stamped.st_size) + " 字节 > 1 MiB：不在循环线程解析";
    return false;
  }
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    *error = "打不开配置文件 " + path;
    return false;
  }
  text->assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  if (file.bad()) {
    *error = "读配置文件 " + path + " 时出错";
    return false;
  }
  if (text->size() > ConfigReloader::kMaxConfigBytes) {
    *error = "配置文件 " + std::to_string(text->size()) + " 字节 > 1 MiB（stat 之后长胖了）";
    return false;
  }
  *hash = content_hash_of(*text);
  return true;
}

}  // namespace

bool ConfigReloader::file_content_hash(const std::string& path, std::uint64_t* hash,
                                       std::string* error) noexcept {
  std::string text;
  return read_config_file(path, &text, hash, error);
}

std::shared_ptr<const l7gate::route::ConfigSnapshot> build_config_snapshot(
    const GatewayConfig& config, std::uint64_t content_hash, std::uint32_t generation,
    l7gate::route::TargetRegistry& registry, std::string* error) {
  auto snapshot = std::make_shared<l7gate::route::ConfigSnapshot>();
  snapshot->generation = generation;
  snapshot->content_hash = content_hash;

  // 上游表：把每一个组的成员**按端点串**交给 `build_nodes_view`，让它经 ever-seen 注册表
  // 对齐下标。这里不能走"静态组成员原样带过来"那一支——新文件里的下标是**这一代自己算的**
  // 文件序（去重后按文件顺序 0..E-1），而 ever-seen 空间是跨代只增的；把前者当后者用，
  // 换代之后"3 号成员"就会指到另一台机器上（这正是 ADR-10 追加节第四节第 2 条禁止的事）。
  l7gate::route::NodesFetch fetch;
  for (std::uint16_t g = 0; g < static_cast<std::uint16_t>(config.groups().size()); ++g) {
    const l7gate::route::UpstreamGroup* def = config.groups().group_at(g);
    if (def == nullptr) {
      *error = "上游组表在第 " + std::to_string(g) + " 个组处缺定义";
      return nullptr;
    }
    l7gate::route::NodesFetch::GroupMembers members;
    members.name = def->name;
    for (const std::uint16_t target : def->targets) {
      if (target >= config.endpoints().size()) {
        *error = "组 " + def->name + " 的成员下标 " + std::to_string(target) +
                 " 越出端点表（装配漏检）";
        return nullptr;
      }
      const Endpoint& endpoint = config.endpoints()[target];
      members.members.push_back(
          l7gate::route::NodeTarget{endpoint.address, endpoint.port, endpoint.health_path});
    }
    fetch.groups.push_back(std::move(members));
  }
  snapshot->nodes =
      l7gate::route::build_nodes_view(config.groups(), fetch, registry, error);
  if (snapshot->nodes == nullptr) return nullptr;  // `error` 已经被它写好

  // 路由表：按**顺序**重放。正则在新表里重新编译（编译不过 = 装配失败 = 不发布，
  // 与启动时同一条形状；`add_regex` 自己保证失败不留半条规则）。
  for (std::size_t i = 0; i < config.routes().size(); ++i) {
    const l7gate::route::RouteTable::KindView rule = config.routes().rule_at(i);
    if (!rule.valid) {
      *error = "路由表第 " + std::to_string(i) + " 条读不回来（装配漏检）";
      return nullptr;
    }
    const bool ok = rule.kind == l7gate::route::Kind::kRegex
                        ? snapshot->routes.add_regex(rule.pattern, rule.upstream_group)
                        : snapshot->routes.add_prefix(rule.pattern, rule.upstream_group);
    if (!ok) {
      *error = "路由规则 /" + std::string(rule.pattern) + "/ 装不进新表";
      return nullptr;
    }
  }

  snapshot->health = std::make_shared<l7gate::route::HealthPolicy>(config.health().policy);
  return snapshot;
}

ScopeShape scope_shape_of(const GatewayConfig& config) noexcept {
  ScopeShape shape;
  shape.has_routes = config.has_routes;
  shape.health_present = config.health().present;
  const RateLimitSection& limit = config.rate_limit();
  shape.rate_limit_present = limit.present;
  shape.rate_per_sec = limit.rate_per_sec;
  shape.burst = limit.burst;
  shape.by_header = limit.by_header;
  shape.header_name = limit.header_name;
  for (std::uint16_t g = 0; g < static_cast<std::uint16_t>(config.groups().size()); ++g) {
    const l7gate::route::UpstreamGroup* def = config.groups().group_at(g);
    if (def != nullptr && def->source_mysql) shape.any_mysql_group = true;
  }
  return shape;
}

ConfigReloader::ConfigReloader(std::string path, std::int64_t interval_ms,
                               std::shared_ptr<l7gate::route::ConfigPublisher> publisher,
                               std::shared_ptr<l7gate::route::HealthTable> health,
                               std::shared_ptr<l7gate::route::TargetRegistry> registry,
                               const GatewayConfig& initial) noexcept
    : path_(std::move(path)),
      interval_ms_(interval_ms),
      publisher_(std::move(publisher)),
      health_(std::move(health)),
      registry_(std::move(registry)),
      current_shape_(scope_shape_of(initial)) {}

void ConfigReloader::note_published(std::uint64_t content_hash) noexcept {
  published_hash_ = content_hash;
  next_check_ms_ = monotonic_ms_now() + interval_ms_;
}

bool ConfigReloader::in_scope(const ScopeShape& next, std::string* reason) const noexcept {
  // 第一节第 7 条：节点表与文件策略两条发布路径不叠加。
  if (next.any_mysql_group) {
    *reason = "有组声明 source=mysql：节点表 reload 与文件重载不叠加（ADR-10 追加节第一节第 7 条）";
    return false;
  }
  // 第一节第 4 条：哪几个段存在是**装配形状**（决定装不装那个 filter、建不建那张表）。
  if (next.has_routes != current_shape_.has_routes) {
    *reason = std::string(next.has_routes ? "出现了" : "删掉了") +
              " routes 段：段的出现/消失改的是 filter 链的形状，不在重载定义域（ADR-10 追加节第一节第 4 条）";
    return false;
  }
  if (next.health_present != current_shape_.health_present) {
    *reason = std::string(next.health_present ? "出现了" : "删掉了") +
              " health 段：健康表建不建是装配形状，不在重载定义域（同上第 4 条）";
    return false;
  }
  // 第一节第 5 条：限流参数不参与重载。理由全文在追加节第一节末，这里只点名是哪一格变了。
  if (next.rate_limit_present != current_shape_.rate_limit_present) {
    *reason = std::string(next.rate_limit_present ? "出现了" : "删掉了") +
              " rate_limit 段：限流装不装是装配形状（第 4 条）";
    return false;
  }
  if (next.rate_limit_present) {
    if (current_shape_.rate_per_sec != next.rate_per_sec || current_shape_.burst != next.burst) {
      *reason = "rate_limit 的 rate/burst 从 " + std::to_string(current_shape_.rate_per_sec) +
                "/" + std::to_string(current_shape_.burst) + " 变成了 " +
                std::to_string(next.rate_per_sec) + "/" + std::to_string(next.burst) +
                "：限流参数不参与重载（桶不回收，改了只能让老键永远用旧参数＝半套生效，ADR-10 追加节第一节第 5 条）";
      return false;
    }
    if (current_shape_.by_header != next.by_header ||
        current_shape_.header_name != next.header_name) {
      *reason = "rate_limit 的 key 变了：键空间变了等于换一整套桶表，同一条理由（第一节第 5 条）";
      return false;
    }
  }
  return true;
}

void ConfigReloader::reload_tick(std::int64_t now_ms) noexcept {
  if (publisher_ == nullptr || interval_ms_ <= 0) return;
  if (now_ms < next_check_ms_) return;  // 没到点：一次整数比较，一个 syscall 都不发
  next_check_ms_ = now_ms + interval_ms_;
  publisher_->note_check();

  struct stat stamped {};
  if (::stat(path_.c_str(), &stamped) != 0) {
    // stat 失败不是一句"文件暂时不在"就过去的：它按**拒载**记账（旧表继续服务），
    // 因为运维看到的必须是"这一轮没有生效"这个事实，而不是"网关安静地什么都没做"。
    publisher_->note_reject_scope();
    last_reject_reason_ = "stat 失败：" + path_ + "（保留上一代继续服务）";
    std::fprintf(stderr, "配置重载被拒: %s\n", last_reject_reason_.c_str());
    return;
  }
  // 取到纳秒：`st_mtime` 只到秒，同一秒内改两次会被读成"没动"。内容哈希是第二道关卡，
  // 但它排在读文件之后——stamp 这一格若是秒级，那次变更连读的机会都拿不到。
  const std::int64_t mtime_ns =
      static_cast<std::int64_t>(stamped.st_mtim.tv_sec) * 1000000000ll +
      static_cast<std::int64_t>(stamped.st_mtim.tv_nsec);
  const auto size = static_cast<std::uint64_t>(stamped.st_size);
  if (mtime_ns == last_mtime_ns_ && size == last_size_) return;  // 磁盘上这份没动
  last_mtime_ns_ = mtime_ns;
  last_size_ = size;

  std::string text;
  std::uint64_t hash = 0;
  std::string why;
  if (!read_config_file(path_, &text, &hash, &why)) {
    publisher_->note_reject_scope();
    last_reject_reason_ = why;
    std::fprintf(stderr, "配置重载被拒: %s（来自 %s）\n", why.c_str(), path_.c_str());
    return;
  }
  if (hash == published_hash_) {
    // 内容一模一样：`touch` 了一下。不发布、不涨发布账——"查了一遍不等于发布了一次"。
    return;
  }

  const std::int64_t started_ms = monotonic_ms_now();

  // 与启动、与用例**同一个**校验器（`gateway_config.h` 那句"分成两条的话，用例验的是
  // 没上线的那一条"在这里同样是硬要求）。
  JsonValue document;
  if (!parse_json(text, &document, &why)) {
    publisher_->note_reject_syntax();
    last_reject_reason_ = why;
    std::fprintf(stderr, "配置重载被拒（语法）: %s（来自 %s）\n", why.c_str(), path_.c_str());
    return;
  }
  GatewayConfig next;
  if (!build_gateway_config(document, &next, &why)) {
    publisher_->note_reject_scope();
    last_reject_reason_ = why;
    std::fprintf(stderr, "配置重载被拒（字段）: %s（来自 %s）\n", why.c_str(), path_.c_str());
    return;
  }
  if (!in_scope(scope_shape_of(next), &why)) {
    publisher_->note_reject_scope();
    last_reject_reason_ = why;
    std::fprintf(stderr, "配置重载被拒（定义域外）: %s（来自 %s）\n", why.c_str(), path_.c_str());
    return;
  }

  const std::uint32_t generation = publisher_->generation() + 1u;
  std::shared_ptr<const l7gate::route::ConfigSnapshot> snapshot =
      build_config_snapshot(next, hash, generation, *registry_, &why);
  if (snapshot == nullptr) {
    publisher_->note_reject_scope();
    last_reject_reason_ = why;
    std::fprintf(stderr, "配置重载被拒（装配）: %s（来自 %s）\n", why.c_str(), path_.c_str());
    return;
  }

  // 顺序要紧：**先把策略换到新一代，再发布世代**。反过来就有一段"读得到新表、
  // 策略还是旧的"的窗口——那正是我在追加节第三节禁止的"拼出来的策略"。
  if (health_ != nullptr && snapshot->health != nullptr) {
    health_->set_policy(snapshot->health);
  }
  publisher_->publish(snapshot);
  published_hash_ = hash;
  current_shape_ = scope_shape_of(next);
  const std::int64_t duration_ms = monotonic_ms_now() - started_ms;
  publisher_->note_publish(epoch_ms_now(), duration_ms);
  std::fprintf(stderr,
               "[reload] gen=%u 生效：路由 %zu 条、组 %zu 个、目标 %zu 个、解析+装配 %lld ms\n",
               generation, snapshot->routes.size(), snapshot->nodes->groups.size(),
               snapshot->nodes->targets.size(), static_cast<long long>(duration_ms));
}

}  // namespace l7gate::config
