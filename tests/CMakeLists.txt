include(GoogleTest)
include(FindThreads)
find_package(Threads REQUIRED)

add_executable(l7gate_tests
  build_info_test.cc
  buffer_test.cc
  connection_test.cc
  reactor_test.cc
  request_parser_test.cc
  upgrade_test.cc
)
target_link_libraries(l7gate_tests PRIVATE
  l7gate_core
  GTest::gtest
  GTest::gtest_main
  # S1-04 的测试对端跑在自己的线程里（ reactor 也必须能被另一个线程 stop），
  # 所以这里要真正的线程支持。tsan preset 已经带 -pthread，debug/release 靠
  # Threads::Threads 补上——少了它 std::thread 在 GCC 上能编译、运行时才炸，
  # 那是最糟的一类"构建通过、测试诡异"。
  Threads::Threads
)

# 标签按**阶段**打，不按任务打。原因：gtest_discover_tests 的 PROPERTIES 作用于
# 整个可执行目标，我没法给同一目标里的 build_info_test 和 buffer_test 分别打
# s1-01 / s1-02 —— 硬打就会像之前那样，15 条用例全被标成 s1-01，是个假索引。
# 任务级筛选走 gtest 套件名（S1_01_Toolchain / S1_02_Buffer / S1_03_Parser）：
# ctest -R S1_03_Parser。
#
# TIMEOUT 是 S1-03 验收标准里「不死循环」那条的**执行手段**。单元测试本身没法
# 断言"没有死循环"——真死循环时进程只是不返回。给它 30 秒，超时就让 ctest 判失败，
# 这条要求才从一句承诺变成一道门禁。30 秒的依据：最慢的 preset 是 sanitizer 构建，
# 而当前全量用例（含 40000 例 fuzz 与吞吐用例）在 asan-ubsan 下实测 < 3 秒，
# 余量足够到不会因为机器负载误杀，又短到死循环不会把整轮验证挂住。
gtest_discover_tests(l7gate_tests
  DISCOVERY_TIMEOUT 60
  PROPERTIES LABELS s1 TIMEOUT 30
)

# 同一个 TestUpstream 类，第二次编译成一个"不是测试"的可执行文件，给
# scripts/smoke-s1-04.sh 当上游。它不进 ctest（没有用例可跑），所以这里不调
# gtest_discover_tests，也不链 gtest——只链它真正需要的 l7gate_core 和线程。
add_executable(l7gate_test_upstream test_upstream_main.cc)
target_link_libraries(l7gate_test_upstream PRIVATE
  l7gate_core
  Threads::Threads
)
