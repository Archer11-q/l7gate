# 压测/联调用的辅助可执行文件。它们**不是**产品的一部分，也不进 ctest。
#
# 单独立一个目录而不是塞进 tests/：tests/ 的语义是"这些代码构成验收证据"，
# 而这个目录里的东西只负责把网关推到需要测量的条件下。混在一起的话，
# "哪段代码被断言过"这个问题就没有答案了——而那是这套测试资产唯一值钱的地方。

add_executable(l7gate_bench_upstream bench_upstream_main.cc)
target_link_libraries(l7gate_bench_upstream PRIVATE l7gate_core)

# S2-06 / ADR-17：SLO #2 的 100k 长连接客户端。和上面同一个理由待在这里——它被构建、
# 不被断言；契约 1.3 只允许"长连接场景"自研压测客户端，这一条就是那唯一一处许可的落地。
add_executable(l7gate_c100k c100k_main.cc)
target_link_libraries(l7gate_c100k PRIVATE l7gate_core)

# S3-04 / ADR-20：首次连通取证（8.4 客户端 ↔ mysql:8.0.36 服务端）的最小可复跑工具。
# 不链 l7gate_core：它验的是客户端库本身，链上产品代码反而让"是库的问题还是我的问题"
# 变成两句话。库由 find_path/find_library 定位（apt 的 libmysqlclient-dev）。
find_path(L7GATE_MYSQL_INCLUDE_DIR mysql.h PATHS /usr/include/mysql)
find_library(L7GATE_MYSQLCLIENT_LIB mysqlclient)
add_executable(l7gate_mysql_probe mysql_probe_main.cc)
if(L7GATE_MYSQL_INCLUDE_DIR AND L7GATE_MYSQLCLIENT_LIB)
  target_include_directories(l7gate_mysql_probe PRIVATE ${L7GATE_MYSQL_INCLUDE_DIR})
  target_link_libraries(l7gate_mysql_probe PRIVATE ${L7GATE_MYSQLCLIENT_LIB})
else()
  message(WARNING "libmysqlclient-dev 未找到：l7gate_mysql_probe 会编译失败（S3-04 前置缺失）")
endif()

# S4-04 / ADR-05 追加节：Redis 首次连通取证（hiredis 1.2.0 ↔ `redis:7.2.5`）。
# 与上面那条相反的取舍：**这条链 l7gate_core**。要取证的不只是"库连得上"，还有"产品里那
# 份 Lua 脚本与那套键口径在这台服务端上按我算的方式工作"——探针自己抄一份脚本，证的就是
# 另一份东西了。hiredis 由 l7gate_core 传递带进来（根 CMakeLists 的 find_library REQUIRED）。
add_executable(l7gate_redis_probe redis_probe_main.cc)
target_link_libraries(l7gate_redis_probe PRIVATE l7gate_core)
