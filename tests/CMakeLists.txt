include(GoogleTest)

add_executable(l7gate_tests
  build_info_test.cc
)
target_link_libraries(l7gate_tests PRIVATE
  l7gate_core
  GTest::gtest
  GTest::gtest_main
)

gtest_discover_tests(l7gate_tests
  DISCOVERY_TIMEOUT 60
  PROPERTIES LABELS "s1-01"
)
