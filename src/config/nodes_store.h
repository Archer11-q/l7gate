#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "route/nodes.h"

// 节点表的 MySQL 读取器（S3-04，ADR-20 第三/五节）。libmysqlclient 的唯一产品侧
// 使用点——C API 的调用面、错误处理与连接策略都收在这一个文件里，别处只认
// `NodesFetch` 这个结果类型（测试用例因此可以在没有 MySQL 的情况下喂快照）。
//
// 连接策略：**每次 fetch 开一条新连接、用完即关**，不做持久连接状态机。理由：
// reload 是低频动作（缺省 5s 一拍），而持久连接要处理"半开连接多久算死"这一整类
// 问题——那类问题的答案在 S4 的超时与重试格子里，今天引入它是提前设计。
// 代价：每拍一次握手（本机 <1ms），记在这里不藏着。
namespace l7gate::config {

struct NodesDbParams {
  std::string host = "127.0.0.1";
  std::uint16_t port = 3306;
  std::string user;
  std::string password;
  std::string database;
  std::int64_t timeout_ms = 2000;  // 连接/读/写共用一个预算；挂死的 mysqld 不能变成僵尸 reload
};

// ADR-28：一行的值域校验（address 非空且必须是 **IPv4 字面量**、port 落在 1..65535、
// health_path 以 / 开头）。它从 `NodesStore::fetch` 的循环里抽出来，只有一个理由：
// **节点表那一支也要能被 ctest 钉住**——`fetch` 要真连接，而这四条检查跟 MySQL 无关。
// 抽出前后行为逐字相同：同样的错误话（带组名与读到的值）、同样"任何一行不合格就整批失败"。
// 地址那一格之所以管：节点表的 address 也进 `target_names`，非字面量会造出 Prometheus
// 非法指标名、整份 `/metrics` 抓失败（S5-05 退化 C1 实测的形状）。
bool validate_node_row(std::string_view group, std::string_view address,
                       std::string_view port_text, std::string_view health_path,
                       std::uint16_t* port, std::string* error);

class NodesStore {
 public:
  explicit NodesStore(NodesDbParams params) noexcept : params_(std::move(params)) {}

  // 读 `l7gate_nodes` 全表。**任何一行不合格都整批失败**（error 点名是哪一行哪个
  // 字段），调用方保留上一份快照——"读不出就整体失败"与文件配置同一条立场：
  // 半张成员表会让"这个组有几个后端"有两个答案。
  // 校验沿用文件侧：port 1..65535、health_path 非空且 '/' 开头、地址非空、
  // 组名必须在 `mysql_groups` 里（文件里声明为 source=mysql 的那些）。
  bool fetch(const std::vector<std::string>& mysql_groups, route::NodesFetch* out,
             std::string* error) const;

 private:
  NodesDbParams params_;
};

}  // namespace l7gate::config
