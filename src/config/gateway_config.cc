#include "config/gateway_config.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "config/json.h"

namespace l7gate::config {
namespace {

std::string join_keys(const JsonValue& obj) {
  std::string text;
  for (const auto& member : obj.members) {
    if (!text.empty()) text += ", ";
    text += member.key;
  }
  return text;
}

// 「这个字段必须存在，且必须是这个类型」。失败时给一句能定位的话。
bool need(const JsonValue& obj, std::string_view key, JsonValue::Kind kind,
          const JsonValue** out, std::string* error, std::string_view where) {
  const JsonValue* value = obj.find(key);
  if (value == nullptr) {
    *error = std::string(where) + " 缺字段 " + std::string(key) + "（这一层已有的键：" +
             join_keys(obj) + "）";
    return false;
  }
  if (value->kind != kind) {
    *error = std::string(where) + " 的 " + std::string(key) + " 类型不对：要" +
             (kind == JsonValue::Kind::kString ? "字符串" : kind == JsonValue::Kind::kArray
                                                       ? "数组"
                                                       : kind == JsonValue::Kind::kObject
                                                           ? "对象"
                                                           : "整数") +
             "，读到的是" + value->kind_name();
    return false;
  }
  *out = value;
  return true;
}

// 未知字段拒绝。忽略它们是"配了一个网关不认识的东西"最省事的写法，
// 也是最难查的写法：文件看起来是对的，行为看起来是稳的，只有语义是错的。
bool forbid_unknown(const JsonValue& obj, std::initializer_list<std::string_view> allowed,
                    std::string* error, std::string_view where) {
  for (const auto& member : obj.members) {
    const bool known = std::find(allowed.begin(), allowed.end(), member.key) != allowed.end();
    if (!known) {
      *error = std::string(where) + " 有本配置不认识的字段 " + member.key +
               "（认识的是：" + [&] {
                 std::string text;
                 for (std::string_view name : allowed) {
                   if (!text.empty()) text += ", ";
                   text += name;
                 }
                 return text;
               }() +
               "）";
      return false;
    }
  }
  return true;
}

bool parse_hash_key(const std::string& text, route::HashKey* out, std::string* error) {
  if (text == "target") {
    out->source = route::HashKey::Source::kTarget;
    out->header_name.clear();
    return true;
  }
  constexpr std::string_view kPrefix = "header:";
  if (text.rfind(kPrefix, 0) == 0 && text.size() > kPrefix.size()) {
    out->source = route::HashKey::Source::kHeaderName;
    out->header_name = text.substr(kPrefix.size());
    return true;
  }
  *error = "hash_key 只能是 \"target\" 或 \"header:<名字>\"，读到的是 \"" + text + "\"";
  return false;
}

}  // namespace

bool build_gateway_config(const JsonValue& document, GatewayConfig* out, std::string* error) {
  if (!document.is_object()) {
    *error = "配置顶层必须是对象（有 upstreams 与 routes 两个键）";
    return false;
  }
  if (!forbid_unknown(document, {"upstreams", "routes", "health"}, error, "配置顶层")) return false;

  const JsonValue* upstreams = nullptr;
  if (!need(document, "upstreams", JsonValue::Kind::kArray, &upstreams, error, "配置顶层")) {
    return false;
  }
  if (upstreams->items.empty()) {
    *error = "upstreams 是空数组：至少要有个上游组";
    return false;
  }

  // 返回去重后的目标下标；失败（下标空间用尽 / 同一后端两个 health_path）写 *why。
  auto find_or_add_endpoint = [&](const Endpoint& endpoint,
                                  std::string* why) -> std::optional<std::uint16_t> {
    for (std::size_t i = 0; i < out->endpoints_.size(); ++i) {
      if (out->endpoints_[i].address == endpoint.address && out->endpoints_[i].port == endpoint.port) {
        // 去重的 S3-03 补充（ADR-19 追加第四条）：同一台后端只允许一个探活定义。
        // 两个组给同一台机器配了不同的 health_path，"活着"就有两个答案——拒绝。
        if (out->endpoints_[i].health_path != endpoint.health_path) {
          *why = "同一后端 " + endpoint.address + ":" + std::to_string(endpoint.port) +
                 " 在两处带着不同的 health_path（\"" + out->endpoints_[i].health_path +
                 "\" 与 \"" + endpoint.health_path + "\"）：一台机器不能有两个\"活着\"的定义";
          return std::nullopt;
        }
        return static_cast<std::uint16_t>(i);
      }
    }
    if (out->endpoints_.size() > static_cast<std::size_t>(route::kMaxUpstreamTarget)) {
      *why = "目标下标空间用尽（上限 " + std::to_string(route::kMaxUpstreamTarget) + "）";
      return std::nullopt;
    }
    out->endpoints_.push_back(endpoint);
    return static_cast<std::uint16_t>(out->endpoints_.size() - 1);
  };

  for (std::size_t i = 0; i < upstreams->items.size(); ++i) {
    const JsonValue& group = upstreams->items[i];
    const std::string where = "upstreams[" + std::to_string(i) + "]";
    if (!group.is_object()) {
      *error = where + " 不是对象，读到的是" + group.kind_name();
      return false;
    }
    if (!forbid_unknown(group, {"name", "balance", "hash_key", "vnodes", "targets"}, error, where)) {
      return false;
    }
    const JsonValue* name = nullptr;
    const JsonValue* balance = nullptr;
    const JsonValue* targets = nullptr;
    if (!need(group, "name", JsonValue::Kind::kString, &name, error, where)) return false;
    if (!need(group, "balance", JsonValue::Kind::kString, &balance, error, where)) return false;
    if (!need(group, "targets", JsonValue::Kind::kArray, &targets, error, where)) return false;
    if (name->text.empty()) {
      *error = where + " 的 name 是空串";
      return false;
    }
    route::Balance strategy{};
    if (!route::parse_balance(balance->text, &strategy)) {
      *error = where + " 的 balance 只认 round_robin / least_conn / consistent_hash，读到的是 \"" +
               balance->text + "\"";
      return false;
    }
    std::uint16_t group_index = route::kNoUpstreamGroup;
    if (!out->groups_.add_group(name->text, strategy, &group_index)) {
      *error = where + " 的 name 与前面某个组重复，或组数超过下标空间：" + name->text;
      return false;
    }
    route::UpstreamGroup* built = const_cast<route::UpstreamGroup*>(out->groups_.group_at(group_index));
    if (const JsonValue* hash_key = group.find("hash_key"); hash_key != nullptr) {
      if (hash_key->kind != JsonValue::Kind::kString ||
          !parse_hash_key(hash_key->text, &built->hash_key, error)) {
        if (hash_key->kind != JsonValue::Kind::kString) {
          *error = where + " 的 hash_key 必须是字符串";
        }
        return false;
      }
    }
    if (const JsonValue* vnodes = group.find("vnodes"); vnodes != nullptr) {
      if (vnodes->kind != JsonValue::Kind::kInt || vnodes->number <= 0 ||
          vnodes->number > 1000000) {
        *error = where + " 的 vnodes 要落在 1..1000000，读到的是 " + vnodes->kind_name();
        return false;
      }
      built->vnodes = static_cast<std::uint32_t>(vnodes->number);
    }
    if (targets->items.empty()) {
      *error = where + " 的 targets 是空数组：一个没有后端的组，选不出任何后端";
      return false;
    }
    for (std::size_t j = 0; j < targets->items.size(); ++j) {
      const JsonValue& member = targets->items[j];
      const std::string mwhere = where + ".targets[" + std::to_string(j) + "]";
      if (!member.is_object()) {
        *error = mwhere + " 不是对象，读到的是" + member.kind_name();
        return false;
      }
      if (!forbid_unknown(member, {"address", "port", "health_path"}, error, mwhere)) return false;
      const JsonValue* address = nullptr;
      const JsonValue* port = nullptr;
      if (!need(member, "address", JsonValue::Kind::kString, &address, error, mwhere)) return false;
      if (!need(member, "port", JsonValue::Kind::kInt, &port, error, mwhere)) return false;
      if (address->text.empty()) {
        *error = mwhere + " 的 address 是空串";
        return false;
      }
      if (port->number <= 0 || port->number > 65535) {
        *error = mwhere + " 的 port 要落在 1..65535";
        return false;
      }
      std::string health_path = "/";
      if (const JsonValue* path = member.find("health_path"); path != nullptr) {
        if (path->kind != JsonValue::Kind::kString) {
          *error = mwhere + " 的 health_path 必须是字符串，读到的是" + path->kind_name();
          return false;
        }
        if (path->text.empty() || path->text[0] != '/') {
          *error = mwhere + " 的 health_path 必须以 / 开头且非空（探活发的是 GET <path>），读到的是 \"" +
                   path->text + "\"";
          return false;
        }
        health_path = path->text;
      }
      const Endpoint endpoint{address->text, static_cast<std::uint16_t>(port->number), health_path};
      std::string why;
      const std::optional<std::uint16_t> target = find_or_add_endpoint(endpoint, &why);
      if (!target.has_value()) {
        *error = where + "：" + why;
        return false;
      }
      if (!out->groups_.add_member(group_index, *target)) {
        *error = mwhere + " 与这个组里前面的成员指向同一个后端";
        return false;
      }
    }
  }

  // routes 段可以没有（`has_routes=false` → main.cc 一个 filter 都不装，链是空的）。
  out->has_routes = document.find("routes") != nullptr;  if (const JsonValue* routes = document.find("routes"); routes != nullptr) {
    if (!routes->is_array()) {
      *error = std::string("routes 必须是数组，读到的是") + routes->kind_name();
      return false;
    }
    for (std::size_t i = 0; i < routes->items.size(); ++i) {
      const JsonValue& rule = routes->items[i];
      const std::string where = "routes[" + std::to_string(i) + "]";
      if (!rule.is_object()) {
        *error = where + " 不是对象，读到的是" + rule.kind_name();
        return false;
      }
      if (!forbid_unknown(rule, {"match", "pattern", "upstream"}, error, where)) return false;
      const JsonValue* match = nullptr;
      const JsonValue* pattern = nullptr;
      const JsonValue* upstream = nullptr;
      if (!need(rule, "match", JsonValue::Kind::kString, &match, error, where)) return false;
      if (!need(rule, "pattern", JsonValue::Kind::kString, &pattern, error, where)) return false;
      if (!need(rule, "upstream", JsonValue::Kind::kString, &upstream, error, where)) return false;
      const std::uint16_t group_index = out->groups_.group_index_by_name(upstream->text);
      if (group_index == route::kNoUpstreamGroup) {
        *error = where + " 指向一个不存在的上游组：" + upstream->text;
        return false;
      }
      // 空前缀是合法的 catch-all；空正则不合法（它会匹配空 target，而那种规则没人能读懂）。
      const bool ok = match->text == "prefix"
                          ? out->routes_.add_prefix(pattern->text, group_index)
                          : match->text == "regex"
                                ? out->routes_.add_regex(pattern->text, group_index)
                                : false;
      if (!ok) {
        *error = where + " 装不上：match 只认 prefix / regex，而 regex 的 pattern 必须是能编译的正则"
                         "（读到的是 match=\"" + match->text + "\" pattern=\"" + pattern->text + "\"）";
        return false;
      }
    }
  }

  // health 段可选（S3-03，ADR-19 追加第四条）：段不存在 = 健康检查整套不装配，
  // 存在则五个字段全部可选、缺省值在 HealthPolicy 里。严格性原样适用：
  // 未知字段 / 类型不对 / 取值越界，全部整体拒绝。
  out->health_.present = document.find("health") != nullptr;
  if (const JsonValue* health = document.find("health"); health != nullptr) {
    if (!health->is_object()) {
      *error = std::string("health 必须是对象，读到的是") + health->kind_name();
      return false;
    }
    if (!forbid_unknown(*health,
                        {"max_fails", "fail_timeout_ms", "probe_interval_ms", "probe_timeout_ms",
                         "max_ejection_percent"},
                        error, "health")) {
      return false;
    }
    auto need_int = [&](std::string_view key, std::int64_t low, std::uint64_t high,
                        std::int64_t* out_value) {
      const JsonValue* v = health->find(key);
      if (v == nullptr) return true;  // 可选：缺省值留在 HealthPolicy 里
      if (v->kind != JsonValue::Kind::kInt) {
        *error = std::string("health 的 ") + std::string(key) + " 必须是整数，读到的是" +
                 v->kind_name();
        return false;
      }
      if (v->number < low || v->number > static_cast<std::int64_t>(high)) {
        *error = std::string("health 的 ") + std::string(key) + " 要落在 " +
                 std::to_string(low) + ".." + std::to_string(high) + "，读到的是 " +
                 std::to_string(v->number);
        return false;
      }
      *out_value = v->number;
      return true;
    };
    std::int64_t v = 0;
    route::HealthPolicy& policy = out->health_.policy;
    if (!need_int("max_fails", 1, 1000000, &v)) return false;
    if (health->find("max_fails") != nullptr) policy.max_fails = static_cast<std::uint32_t>(v);
    if (!need_int("fail_timeout_ms", 1, 86400000, &v)) return false;
    if (health->find("fail_timeout_ms") != nullptr) policy.fail_timeout_ms = v;
    if (!need_int("probe_interval_ms", 0, 86400000, &v)) return false;
    if (health->find("probe_interval_ms") != nullptr) policy.probe_interval_ms = v;
    if (!need_int("probe_timeout_ms", 1, 3600000, &v)) return false;
    if (health->find("probe_timeout_ms") != nullptr) policy.probe_timeout_ms = v;
    if (!need_int("max_ejection_percent", 0, 100, &v)) return false;
    if (health->find("max_ejection_percent") != nullptr) {
      policy.max_ejection_percent = static_cast<std::uint32_t>(v);
    }
  }

  // 建环。只有 consistent_hash 的组装，别的组不付费也不需要它。
  std::vector<std::string> endpoint_keys;
  endpoint_keys.reserve(out->endpoints_.size());
  for (const Endpoint& endpoint : out->endpoints_) {
    endpoint_keys.push_back(endpoint.address + ':' + std::to_string(endpoint.port));
  }
  for (std::uint16_t g = 0; g < static_cast<std::uint16_t>(out->groups_.size()); ++g) {
    const route::UpstreamGroup* group = out->groups_.group_at(g);
    if (group == nullptr || group->balance != route::Balance::kConsistentHash) continue;
    route::UpstreamGroup* mutable_group = const_cast<route::UpstreamGroup*>(group);
    if (!route::build_hash_ring(mutable_group, endpoint_keys)) {
      *error = "组 " + group->name + " 的一致性哈希环建不起来（成员或端点表有空）";
      return false;
    }
  }
  return true;
}

bool load_gateway_config(std::string_view path, GatewayConfig* out, std::string* error) {
  // 先把名字落到一个 string 变量上再构造：`std::ifstream file(std::string(path));` 是
  // most vexing parse——它被读成一个函数声明，报的错还长得像类型问题（本轮实测踩过一次）。
  const std::string file_name(path);
  std::ifstream file(file_name);
  if (!file.is_open()) {
    *error = "打不开配置文件 " + std::string(path);
    return false;
  }
  // 一次读进字符串再解析：解析器要的是"整份文本 + 偏移"，边读边解析会把报错位置变成流位置。
  std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  if (file.bad()) {
    *error = "读配置文件 " + std::string(path) + " 时出错";
    return false;
  }
  if (text.size() > (1u << 20)) {
    *error = "配置文件超过 1 MiB（" + std::to_string(text.size()) + " 字节）——这不像一份配置";
    return false;
  }
  JsonValue document;
  if (!parse_json(text, &document, error)) return false;
  return build_gateway_config(document, out, error);
}

}  // namespace l7gate::config
