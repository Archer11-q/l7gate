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
