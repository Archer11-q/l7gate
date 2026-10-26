include(GoogleTest)

add_executable(l7gate_tests
  build_info_test.cc
  buffer_test.cc
)
target_link_libraries(l7gate_tests PRIVATE
  l7gate_core
  GTest::gtest
  GTest::gtest_main
)

# 标签按**阶段**打，不按任务打。原因：gtest_discover_tests 的 PROPERTIES 作用于
# 整个可执行目标，我没法给同一目标里的 build_info_test 和 buffer_test 分别打
# s1-01 / s1-02 —— 硬打就会像之前那样，15 条用例全被标成 s1-01，是个假索引。
# 任务级筛选走 gtest 套件名（S1_01_Toolchain / S1_02_Buffer）：ctest -R S1_02_Buffer。
gtest_discover_tests(l7gate_tests
  DISCOVERY_TIMEOUT 60
  PROPERTIES LABELS "s1"
)
