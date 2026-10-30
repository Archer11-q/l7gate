#include "route/nodes.h"

#include <algorithm>

namespace l7gate::route {

std::uint16_t TargetRegistry::intern(const NodeTarget& target) {
  const std::string key = target.address + ":" + std::to_string(target.port);
  const std::lock_guard<std::mutex> guard(mutex_);
  auto it = by_endpoint_.find(key);
  if (it != by_endpoint_.end()) return it->second;
  if (targets_.size() > static_cast<std::size_t>(kMaxUpstreamTarget)) {
    return kNoUpstreamTarget;  // 下标空间用尽；调用方拒绝整批而不是挤掉谁
  }
  const auto index = static_cast<std::uint16_t>(targets_.size());
  targets_.push_back(target);
  by_endpoint_.emplace(key, index);
  return index;
}

std::uint16_t TargetRegistry::size() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return static_cast<std::uint16_t>(targets_.size());
}

const NodeTarget* TargetRegistry::at(std::uint16_t index) const {
  const std::lock_guard<std::mutex> guard(mutex_);
  if (index >= targets_.size()) return nullptr;
  return &targets_[index];
}

std::vector<std::string> TargetRegistry::endpoint_keys() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  std::vector<std::string> keys;
  keys.reserve(targets_.size());
  for (const NodeTarget& target : targets_) keys.push_back(target.endpoint_key());
  return keys;
}

std::shared_ptr<const NodesView> build_nodes_view(const GroupTable& file_groups,
                                                  const NodesFetch& fetch,
                                                  TargetRegistry& registry,
                                                  std::string* error) {
  // fetch 里的组名必须都在文件里（store 侧已拒过一次，这里再拦一道：
  // build_nodes_view 的调用方不止 store，用例就喂任意 fetch）。半张快照不发布。
  std::unordered_map<std::string, const NodesFetch::GroupMembers*> by_name;
  for (const NodesFetch::GroupMembers& group : fetch.groups) {
    if (file_groups.group_by_name(group.name) == nullptr) {
      *error = "快照装配失败：节点表里出现文件没有的组 \"" + group.name + "\"";
      return nullptr;
    }
    by_name.emplace(group.name, &group);
  }

  auto view = std::make_shared<NodesView>();
  // 组的下标顺序 = 文件顺序，reload 永远不改它（LB filter 与装配具按这个下标找组）。
  for (std::uint16_t g = 0; g < static_cast<std::uint16_t>(file_groups.size()); ++g) {
    const UpstreamGroup* def = file_groups.group_at(g);
    if (def == nullptr) {
      *error = "文件组表在第 " + std::to_string(g) + " 个组处缺定义";
      return nullptr;
    }
    std::uint16_t index = kNoUpstreamGroup;
    if (!view->groups.add_group(def->name, def->balance, &index)) {
      *error = "快照装配失败：组 " + def->name + " 建不进新表";
      return nullptr;
    }
    UpstreamGroup* built = const_cast<UpstreamGroup*>(view->groups.group_at(index));
    built->vnodes = def->vnodes;
    built->hash_key = def->hash_key;
    built->source_mysql = def->source_mysql;
    if (by_name.count(def->name) == 0) {
      // 静态组：成员原样带过来。
      for (const std::uint16_t target : def->targets) {
        if (!view->groups.add_member(index, target)) {
          *error = "快照装配失败：静态组 " + def->name + " 的成员带不过去";
          return nullptr;
        }
      }
    } else {
      // MySQL 组：成员经注册表 intern 成 ever-seen 下标。
      for (const NodeTarget& member : by_name[def->name]->members) {
        const std::uint16_t target = registry.intern(member);
        if (target == kNoUpstreamTarget) {
          *error = "快照装配失败：组 " + def->name + " 的成员 " + member.endpoint_key() +
                   " 拿不到目标下标（下标空间用尽）";
          return nullptr;
        }
        if (!view->groups.add_member(index, target)) {
          *error = "快照装配失败：组 " + def->name + " 的成员 " + member.endpoint_key() +
                   " 重复";
          return nullptr;
        }
      }
    }
  }
  // 环在发布前建好：发布出去的东西不需要任何人再"补一步"。
  // 例外：source=mysql 且成员还没到岗的组（targets 为空）没有环可建——它的空成员
  // 表态（502）就是"未到岗"，环随第一份有成员的快照一起发布。
  const std::vector<std::string> keys = registry.endpoint_keys();
  for (std::uint16_t g = 0; g < static_cast<std::uint16_t>(view->groups.size()); ++g) {
    UpstreamGroup* built = const_cast<UpstreamGroup*>(view->groups.group_at(g));
    if (built->balance != Balance::kConsistentHash) continue;
    if (built->source_mysql && built->targets.empty()) continue;
    if (!build_hash_ring(built, keys)) {
      *error = "快照装配失败：组 " + built->name + " 的环建不起来";
      return nullptr;
    }
  }
  view->targets.clear();
  for (std::uint16_t i = 0; i < registry.size(); ++i) {
    const NodeTarget* t = registry.at(i);
    if (t == nullptr) {
      *error = "快照装配失败：目标表在第 " + std::to_string(i) + " 个下标处缺行";
      return nullptr;
    }
    view->targets.push_back(*t);
  }
  return view;
}

void NodesPublisher::note_reload(bool ok, std::int64_t epoch_ms, std::int64_t duration_ms) noexcept {
  (void)duration_ms;  // 留给日志；计数面只认 ok/失败与最后发布时刻
  if (ok) {
    reloads_ok_.fetch_add(1, std::memory_order_relaxed);
    last_publish_epoch_ms_.store(epoch_ms, std::memory_order_relaxed);
  } else {
    reloads_failed_.fetch_add(1, std::memory_order_relaxed);
  }
}

}  // namespace l7gate::route
