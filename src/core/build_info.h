#pragma once

#include <string>

namespace l7gate::core {

// 编译期事实。BENCHMARK.md 的完成判据要求"含 CPU 绑核、分配器、各组件版本号"，
// 其中组件版本号这一半由这里给出运行时可查询的形式，
// 免得压测报告里的环境是靠人肉抄录 —— 抄录就有抄错的一天。
struct BuildInfo {
  std::string version;          // 项目版本
  std::string compiler;         // 编译器与版本（__clang_version__ / __VERSION__）
  std::string cxx_std;          // __cplusplus 宏值
  std::string gtest_version;    // FetchContent 实际拉到的 GoogleTest 版本
  std::string sanitizers;       // "address,undefined" / "thread" / "none"
  std::string build_type;       // CMAKE_BUILD_TYPE
  std::string malloc_impl;      // 分配器实现，SLO #2 必须记录
};

BuildInfo current();

// 把 BuildInfo 渲染成单行 key=value，供脚本 grep / 落盘。
std::string render_one_line(const BuildInfo& info);

}  // namespace l7gate::core
