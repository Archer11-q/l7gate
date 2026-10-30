#include "config/nodes_store.h"

#include <algorithm>
#include <cstdint>
#include <string>

#include <mysql/mysql.h>

namespace l7gate::config {
namespace {

// 毫秒预算换算成 MySQL 的秒粒度选项：向上取整、至少 1（0 会被库当成"无限"）。
unsigned int timeout_seconds(std::int64_t ms) noexcept {
  if (ms <= 0) return 1;
  return static_cast<unsigned int>((ms + 999) / 1000);
}

bool valid_health_path(const std::string& path) noexcept {
  return !path.empty() && path[0] == '/';
}

}  // namespace

bool NodesStore::fetch(const std::vector<std::string>& mysql_groups,
                       route::NodesFetch* out, std::string* error) const {
  MYSQL* m = mysql_init(nullptr);
  if (m == nullptr) {
    *error = "mysql_init 失败";
    return false;
  }
  const unsigned int secs = timeout_seconds(params_.timeout_ms);
  mysql_options(m, MYSQL_OPT_CONNECT_TIMEOUT, &secs);
  mysql_options(m, MYSQL_OPT_READ_TIMEOUT, &secs);
  mysql_options(m, MYSQL_OPT_WRITE_TIMEOUT, &secs);

  if (mysql_real_connect(m, params_.host.c_str(), params_.user.c_str(),
                         params_.password.c_str(), params_.database.c_str(), params_.port,
                         nullptr, 0) == nullptr) {
    *error = "MySQL 连接失败: errno=" + std::to_string(mysql_errno(m)) + " " +
             mysql_error(m);
    mysql_close(m);
    return false;
  }

  const char* sql =
      "SELECT group_name, address, port, health_path FROM l7gate_nodes ORDER BY group_name, "
      "address, port";
  if (mysql_query(m, sql) != 0) {
    *error = "节点表查询失败: errno=" + std::to_string(mysql_errno(m)) + " " + mysql_error(m);
    mysql_close(m);
    return false;
  }
  MYSQL_RES* res = mysql_store_result(m);
  if (res == nullptr) {
    *error = "节点表结果读取失败: " + std::string(mysql_error(m));
    mysql_close(m);
    return false;
  }

  // 组序与表序一致（ORDER BY group_name）：同一组的行天然相邻，逐行归组即可。
  std::size_t matched = 0;
  for (MYSQL_ROW row = mysql_fetch_row(res); row != nullptr; row = mysql_fetch_row(res)) {
    const unsigned long* lengths = mysql_fetch_lengths(res);
    if (lengths == nullptr || row[0] == nullptr || row[1] == nullptr || row[2] == nullptr ||
        row[3] == nullptr) {
      *error = "节点表第 " + std::to_string(matched + 1) + " 行有 NULL 列";
      mysql_free_result(res);
      mysql_close(m);
      return false;
    }
    const std::string group(row[0], lengths[0]);
    const std::string address(row[1], lengths[1]);
    const std::string port_text(row[2], lengths[2]);
    const std::string health_path(row[3], lengths[3]);
    if (std::find(mysql_groups.begin(), mysql_groups.end(), group) == mysql_groups.end()) {
      *error = "节点表里出现文件没有声明为 source=mysql 的组 \"" + group +
               "\"：整批拒载，保留上一份（数据库不许偷偷扩配置面）";
      mysql_free_result(res);
      mysql_close(m);
      return false;
    }
    if (address.empty()) {
      *error = "节点表组 " + group + " 的某行 address 为空";
      mysql_free_result(res);
      mysql_close(m);
      return false;
    }
    const long port = std::strtol(port_text.c_str(), nullptr, 10);
    if (port <= 0 || port > 65535) {
      *error = "节点表组 " + group + " 的行 " + address + ":" + port_text +
               " 的 port 要落在 1..65535";
      mysql_free_result(res);
      mysql_close(m);
      return false;
    }
    if (!valid_health_path(health_path)) {
      *error = "节点表组 " + group + " 的行 " + address + ":" + port_text +
               " 的 health_path 必须以 / 开头且非空，读到的是 \"" + health_path + "\"";
      mysql_free_result(res);
      mysql_close(m);
      return false;
    }
    // 同组同一后端第二行：主键 (group,address,port) 挡得住 INSERT，但 SELECT 侧
    // 仍按"组内重复成员"校验一次——约束万一被人绕过（直接改表），这里接住。
    auto* members = [&]() -> std::vector<route::NodeTarget>* {
      for (auto& g : out->groups) {
        if (g.name == group) return &g.members;
      }
      out->groups.push_back(route::NodesFetch::GroupMembers{group, {}});
      return &out->groups.back().members;
    }();
    const route::NodeTarget candidate{address, static_cast<std::uint16_t>(port), health_path};
    if (std::any_of(members->begin(), members->end(),
                    [&](const route::NodeTarget& t) {
                      return t.address == candidate.address && t.port == candidate.port;
                    })) {
      *error = "节点表组 " + group + " 的成员 " + candidate.endpoint_key() + " 重复";
      mysql_free_result(res);
      mysql_close(m);
      return false;
    }
    members->push_back(candidate);
    ++matched;
  }
  mysql_free_result(res);
  mysql_close(m);
  (void)matched;
  return true;
}

}  // namespace l7gate::config
