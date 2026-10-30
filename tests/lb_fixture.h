#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "route/balance.h"

// S3-02 用例共用的组装配具。
//
// 为什么测试要自己带一份装配动作：`GroupTable` 只给 const 访问器（`group_at` /
// `group_by_name`），而组的两样运行期属性——`hash_key`/`vnodes` 与那张哈希环——必须在装配期
// 写进去。产品侧的装配方 `config/gateway_config.cc` 用的就是 `const_cast` +
// `build_hash_ring`；用例走**同一条形状**，而不是另造一套"看起来一样"的装配代码——
// 同一个限制有两个抄本就是 `max_header_count` 那次的坑（1.23 与 ADR-18 第四节都记过）。
//
// 它存在的另一个理由是把"环到底建没建"这件事收在一个地方：`LoadBalanceFilter` 对
// 没建环的 consistent_hash 组回的是 502，如果每条用例自己手搓一遍建环步骤，就有 20 个
// 地方可能漏掉那一步，而漏掉的失败形态是"配置坏了"而不是"我忘了"。
namespace l7gate::testing {

// 建一个组、加成员、按 vnodes 建环（非 consistent_hash 策略不建环，`build_hash_ring`
// 自己会跳过）。失败时返回 false 并把原因写进 `error`——**用例必须 ASSERT 它成功**：
// 一个装配失败但继续往下跑的均衡用例，它给出的"分布"或"迁移率"没有意义。
inline bool add_balanced_group(route::GroupTable* table, std::string_view name,
                               route::Balance balance, const std::vector<std::uint16_t>& members,
                               const std::vector<std::string>& endpoint_keys, std::uint16_t* group,
                               std::string* error, std::uint32_t vnodes = route::kDefaultVnodes,
                               const route::HashKey& hash_key = {}) {
  if (!table->add_group(name, balance, group)) {
    *error = "add_group 失败（组名为空或重名）：" + std::string(name);
    return false;
  }
  route::UpstreamGroup* built = const_cast<route::UpstreamGroup*>(table->group_at(*group));
  built->vnodes = vnodes;
  built->hash_key = hash_key;
  for (const std::uint16_t target : members) {
    if (!table->add_member(*group, target)) {
      *error = "add_member 失败：组 " + std::string(name) + " 的成员 " + std::to_string(target);
      return false;
    }
  }
  if (!route::build_hash_ring(built, endpoint_keys)) {
    *error = "build_hash_ring 失败：组 " + std::string(name) + "（空组 / vnodes=0 / 成员取不到端点键）";
    return false;
  }
  return true;
}

// 一套给用例用的端点键：下标 = 全局目标下标，与 `Reactor::Config::upstream_targets` 同形。
inline std::vector<std::string> endpoint_keys_for(std::size_t count, std::uint16_t base_port = 18081) {
  std::vector<std::string> keys;
  keys.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    keys.push_back("127.0.0.1:" + std::to_string(base_port + i));
  }
  return keys;
}

}  // namespace l7gate::testing
