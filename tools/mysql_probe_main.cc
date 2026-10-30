// S3-04 首次连通取证（ADR-20 第二节，契约 9.1）：libmysqlclient 8.4 客户端 ↔
// `mysql:8.0.36` 服务端真实跑通的最小可复跑工具。它只验连通与基本表操作，
// 不是网关的一部分、不参与任何 SLO 测量。
//
// 取证即断言：connect、CREATE TABLE、INSERT、SELECT 读回、DELETE 五步任何一步
// 失败都退出码 1 并打出服务端/客户端版本与出错那一格——"能连上"和"能用"是两个命题。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include <mysql/mysql.h>

namespace {

int fail(MYSQL* m, std::string_view step) {
  std::fprintf(stderr, "[probe] ✗ %s 失败: errno=%u %s\n", std::string(step).c_str(),
               m != nullptr ? mysql_errno(m) : 0u, m != nullptr ? mysql_error(m) : "n/a");
  return 1;
}

bool query(MYSQL* m, const char* sql, std::string_view step) {
  if (mysql_query(m, sql) != 0) {
    fail(m, step);
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::string host = "127.0.0.1";
  std::uint16_t port = 13306;
  std::string user = "l7gate";
  std::string password = "l7gate-pw";
  std::string database = "l7gate";
  for (int i = 1; i < argc; ++i) {
    const std::string_view flag = argv[i];
    const char* value = (i + 1 < argc) ? argv[i + 1] : nullptr;
    if (flag == "--host" && value != nullptr) { host = value; ++i; }
    else if (flag == "--port" && value != nullptr) { port = static_cast<std::uint16_t>(std::atoi(value)); ++i; }
    else if (flag == "--user" && value != nullptr) { user = value; ++i; }
    else if (flag == "--password" && value != nullptr) { password = value; ++i; }
    else if (flag == "--database" && value != nullptr) { database = value; ++i; }
    else { std::fprintf(stderr, "参数错误: %s\n", std::string(flag).c_str()); return 2; }
  }

  MYSQL* m = mysql_init(nullptr);
  if (m == nullptr) return fail(nullptr, "mysql_init");
  // 连接超时 5 秒：一个挂死的 mysqld 不能把取证变成挂住。
  unsigned int timeout = 5;
  mysql_options(m, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);

  std::printf("[probe] client=%s\n", mysql_get_client_info());
  if (mysql_real_connect(m, host.c_str(), user.c_str(), password.c_str(), database.c_str(),
                         port, nullptr, 0) == nullptr) {
    return fail(m, "mysql_real_connect");
  }
  const char* server = mysql_get_server_info(m);
  std::printf("[probe] server=%s\n", server);

  // 五步取证：建表 / 写 / 读回 / 改 / 删。
  if (!query(m, "CREATE TABLE IF NOT EXISTS l7gate_nodes ("
                "group_name VARCHAR(64) NOT NULL,"
                "address VARCHAR(45) NOT NULL,"
                "port SMALLINT UNSIGNED NOT NULL,"
                "health_path VARCHAR(128) NOT NULL DEFAULT '/',"
                "PRIMARY KEY (group_name, address, port)"
                ")", "CREATE TABLE")) return 1;
  if (!query(m, "INSERT INTO l7gate_nodes VALUES ('probe', '127.0.0.1', 18001, '/healthz') "
                "ON DUPLICATE KEY UPDATE health_path='/healthz'", "INSERT")) return 1;
  if (!query(m, "SELECT group_name, address, port, health_path FROM l7gate_nodes "
                "WHERE group_name='probe'", "SELECT")) return 1;
  MYSQL_RES* res = mysql_store_result(m);
  if (res == nullptr) return fail(m, "mysql_store_result");
  const my_ulonglong rows = mysql_num_rows(res);
  MYSQL_ROW row = rows > 0 ? mysql_fetch_row(res) : nullptr;
  const bool roundtrip_ok = row != nullptr && std::strcmp(row[0], "probe") == 0 &&
                            std::strcmp(row[3], "/healthz") == 0;
  mysql_free_result(res);
  std::printf("[probe] roundtrip=%s (rows=%llu)\n", roundtrip_ok ? "ok" : "MISMATCH",
              static_cast<unsigned long long>(rows));
  if (!roundtrip_ok) return 1;
  if (!query(m, "DELETE FROM l7gate_nodes WHERE group_name='probe'", "DELETE")) return 1;

  mysql_close(m);
  std::printf("[probe] ✓ 8.4 客户端 ↔ %s 服务端连通与表操作全部成立\n", server);
  return 0;
}
