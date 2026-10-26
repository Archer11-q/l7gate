#include "core/build_info.h"

#include <string>

#include <gtest/gtest.h>

#ifndef L7GATE_GTEST_VERSION
#define L7GATE_GTEST_VERSION "unknown"
#endif
// 配置阶段传进来的"我要求开哪些 sanitizer"。
// 这个断言的意义：如果 -fsanitize 在某个 preset 里被静默丢掉（例如
// preset 写错、或被上层 CMAKE_CXX_FLAGS 覆盖），二进制仍然能跑、
// 测试仍然全绿，而"ASan 干净"这句结论已经作废。所以必须正面断言。
#ifndef L7GATE_SANITIZERS_EXPECTED
#define L7GATE_SANITIZERS_EXPECTED ""
#endif

namespace l7gate::core {
namespace {

// 契约要求 C++20，且 CMAKE_CXX_EXTENSIONS OFF。
// C++20 下 __cplusplus == 202002L；Extensions ON 时 clang 会报 201703L+ 之类的
// GNU 值，所以这条同时锁住了"我用的确实是标准 C++20"。
TEST(S1_01_Toolchain, CxxStandardIsExactly20) {
  // 必须比数值本身。我第一版写成 to_string(__cplusplus)=="202002L"，
  // 而 to_string 给的是 "202002"（L 只是宏字面量的后缀，不在值里），
  // 测试直接开火 —— 抓到的是我的断言写错，不是构建有问题。
  // 这类自伤留痕，否则下次还会犯。
  EXPECT_EQ(__cplusplus, 202002L);
  EXPECT_EQ(current().cxx_std, "202002");
}

TEST(S1_01_Toolchain, GoogleTestVersionMatchesContractLock) {
  EXPECT_STREQ(L7GATE_GTEST_VERSION, "1.14.0");
  // 运行期再确认一次：链接进来的库和配置期看到源码目录时读到的版本一致。
  EXPECT_EQ(current().gtest_version, "1.14.0");
}

TEST(S1_01_Toolchain, RequestedSanitizersAreActuallyCompiledIn) {
  const std::string expected = L7GATE_SANITIZERS_EXPECTED;
  const std::string detected = current().sanitizers;
  if (expected.empty()) {
    EXPECT_EQ(detected, "none")
        << "preset 没要求 sanitizer，但二进制里检测到了：" << detected;
  } else {
    // 顺序不保证，逐项比对。
    for (const auto& want : {std::string("address"), std::string("undefined"), std::string("thread")}) {
      if (expected.find(want) != std::string::npos) {
        EXPECT_NE(detected.find(want), std::string::npos)
            << "要求了 " << want << " 但编译期宏没打开，实际检测到: " << detected;
      }
    }
    // address 与 thread 互斥，同时出现说明构建配置写错了。
    EXPECT_FALSE(detected.find("address") != std::string::npos &&
                 detected.find("thread") != std::string::npos)
        << "address 与 thread 不可同时启用，检测到: " << detected;
  }
}

TEST(S1_01_Toolchain, AllocatorIsRecordedForSlo2) {
  // SLO #2 要求必须记录 malloc 实现；留空等于指标定义不完整。
  EXPECT_NE(current().malloc_impl, "unknown");
  EXPECT_NE(current().malloc_impl.find("glibc"), std::string::npos)
      << "当前基线应为 glibc malloc，若换 jemalloc/tcmalloc 需先写 ADR";
}

TEST(S1_01_Toolchain, BuildTypeIsNeverSilentlyUnset) {
  EXPECT_NE(current().build_type, "unset");
  EXPECT_NE(current().build_type, "");
}

TEST(S1_01_Toolchain, RenderIsSingleLineAndParsableByScripts) {
  const std::string line = render_one_line(current());
  EXPECT_EQ(line.find('\n'), std::string::npos);
  for (const char* key : {"version=", "compiler=", "gtest=", "sanitizers=", "build_type=", "malloc="}) {
    EXPECT_NE(line.find(key), std::string::npos) << "缺字段 " << key << " 于: " << line;
  }
}

}  // namespace
}  // namespace l7gate::core
