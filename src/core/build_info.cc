#include "core/build_info.h"

#include <sstream>

#ifndef L7GATE_VERSION
#define L7GATE_VERSION "unknown"
#endif
#ifndef L7GATE_GTEST_VERSION
#define L7GATE_GTEST_VERSION "unknown"
#endif

namespace l7gate::core {
namespace {

std::string detect_sanitizers() {
  std::ostringstream os;
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
  os << "address,";
#endif
#if __has_feature(undefined_behavior_sanitizer) || defined(__SANITIZE_UNDEFINED__)
  os << "undefined,";
#endif
#if __has_feature(thread_sanitizer) || defined(__SANITIZE_THREAD__)
  os << "thread,";
#endif
#else
#if defined(__SANITIZE_ADDRESS__)
  os << "address,";
#endif
#if defined(__SANITIZE_THREAD__)
  os << "thread,";
#endif
#endif
  std::string s = os.str();
  if (s.empty()) return "none";
  s.pop_back();  // 去掉尾逗号
  return s;
}

// 分配器实现。SLO #2 的"每连接 ≤ 20 KB"直接由它决定（契约第 2 节明确写了这条），
// 所以不能留空。判据：glibc 下 malloc 与可执行文件同进程；
// 换成 jemalloc/tcmalloc 的方式是 LD_PRELOAD，进程内可用 weak 符号探测。
std::string detect_malloc() {
#if defined(__GLIBC__)
  return "glibc-malloc";
#else
  return "unknown";
#endif
}

}  // namespace

BuildInfo current() {
  BuildInfo info;
  info.version = L7GATE_VERSION;
#if defined(__clang__)
  info.compiler = std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
  info.compiler = std::string("gcc ") + __VERSION__;
#else
  info.compiler = "unknown";
#endif
  info.cxx_std = std::to_string(__cplusplus);
  info.gtest_version = L7GATE_GTEST_VERSION;
  info.sanitizers = detect_sanitizers();
#ifdef L7GATE_BUILD_TYPE
  info.build_type = L7GATE_BUILD_TYPE;
#else
  info.build_type = "unset";
#endif
  info.malloc_impl = detect_malloc();
  return info;
}

std::string render_one_line(const BuildInfo& info) {
  std::ostringstream os;
  os << "version=" << info.version
     << " compiler=" << info.compiler
     << " __cplusplus=" << info.cxx_std
     << " gtest=" << info.gtest_version
     << " sanitizers=" << info.sanitizers
     << " build_type=" << info.build_type
     << " malloc=" << info.malloc_impl;
  return os.str();
}

}  // namespace l7gate::core
