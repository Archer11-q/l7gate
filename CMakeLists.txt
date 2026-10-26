cmake_minimum_required(VERSION 3.22)

project(l7gate
  VERSION 0.1.0
  DESCRIPTION "L7 traffic governance gateway (HTTP/1.1, C++20, epoll ET Reactor)"
  LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# 语言标准：契约要求 C++20。extensions OFF —— 我要的是标准 C++20，
# 不是 GNU 扩展下的"看起来能用"；__cplusplus 会被单测断言成 202002L。
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# 编译选项。
# -Werror 默认关：clang 21 是新版本，把新增警告直接升级成构建失败会让
# 工具链升级变成阻塞项。我要它在 CI 阶段显式打开，而不是默认状态碰运气。
# ---------------------------------------------------------------------------
add_library(l7gate_warnings INTERFACE)
target_compile_options(l7gate_warnings INTERFACE
  -Wall -Wextra -Wpedantic
  -Wshadow -Wconversion -Wsign-conversion
  -Wnon-virtual-dtor -Wcast-align
  -Wunused -Woverloaded-virtual
)
option(L7GATE_WERROR "Treat warnings as errors" OFF)
if(L7GATE_WERROR)
  target_compile_options(l7gate_warnings INTERFACE -Werror)
endif()

# ---------------------------------------------------------------------------
# Sanitizer：契约 9.1 指定 Clang 的 ASan/TSan/UBSan 是连接对象生命周期的
# 唯一自动化验证手段，S1 门禁要 ASan/UBSan 干净、S2 门禁要 TSan 干净。
#
# 这里刻意用 L7GATE_SANITIZERS 字符串而不是逐个开关：sanitizer 是要**组合**跑的
# （address,undefined 是 S1 门禁要求的组合），而 address 与 thread 互斥，
# 同时开会让链接失败——组合关系由 preset 表达比由一堆 option 表达更诚实。
#
# 注意实测踩坑（见 docs/ENVIRONMENT.md 2.2）：ASan 的阳性验证必须 -O0 -g，
# 开了优化后"可被证明无副作用的坏写入"会被 dead store elimination 删掉，
# sanitizer 什么都不报，看起来像干净。所以这里强制 -g 并禁止在 Release 上
# 出 sanitizer 结论。
# ---------------------------------------------------------------------------
set(L7GATE_SANITIZERS "" CACHE STRING "Comma-separated sanitizers (e.g. address,undefined | thread)")
add_library(l7gate_sanitizers INTERFACE)
if(L7GATE_SANITIZERS)
  target_compile_options(l7gate_sanitizers INTERFACE
    -fsanitize=${L7GATE_SANITIZERS} -fno-omit-frame-pointer -g)
  target_link_options(l7gate_sanitizers INTERFACE -fsanitize=${L7GATE_SANITIZERS})
  if(L7GATE_SANITIZERS MATCHES "thread")
    # TSan 需要真正的 -pthread（不只是链接 pthread），否则它会抱怨
    # 缺少 pthread 拦截。
    target_compile_options(l7gate_sanitizers INTERFACE -pthread)
    target_link_options(l7gate_sanitizers INTERFACE -pthread)
  endif()
  message(STATUS "l7gate: sanitizers = ${L7GATE_SANITIZERS}")
else()
  message(STATUS "l7gate: sanitizers = none")
endif()

# ---------------------------------------------------------------------------
# 第三方依赖。契约 1.3 禁止第三方网络库（ADR-04），这里只允许出现
# 测试/客户端库这一类不承载本项目考点的东西，且一律锁精确版本。
# ---------------------------------------------------------------------------
include(FetchContent)

# GoogleTest 锁定 v1.14.0。Ubuntu 26.04 的 apt 只有 1.17.0，
# 所以我从源码取、按契约字面锁定，不改契约（见 docs/ENVIRONMENT.md 第 2 节）。
# 用的是显式 tag 而不是分支名/latest —— 契约 9.1 禁止 latest。
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.14.0
  GIT_SHALLOW    TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# 第三方头文件按 SYSTEM 处理。clang 21 在 -Wconversion 家族下会对
# gtest-printers.h 报 -Wcharacter-conversion —— 那是别人的代码质量，
# 混进我的警告面只会淹没我自己的信号。
# 这里不用 FetchContent_Declare 的 SYSTEM 关键字：它要 CMake ≥ 3.25，
# 而契约 9.1 锁的下限是 3.22。我不该为了消一条警告去抬高构建门槛
# （那属于没必要的范围扩张），手动设属性在 3.22 上就成立。
# 注意目标是 gtest / gtest_main 这些**真实目标**，不是 GTest::gtest：
# 后者是 ALIAS，而 set_target_properties 不能作用在 ALIAS 上
# （实测报 "set_target_properties can not be used on an ALIAS target"）。
foreach(_gt_target gtest gtest_main gmock gmock_main)
  if(TARGET ${_gt_target})
    get_target_property(_gt_inc ${_gt_target} INTERFACE_INCLUDE_DIRECTORIES)
    if(_gt_inc)
      set_target_properties(${_gt_target} PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_gt_inc}")
    endif()
  endif()
endforeach()

# 断言实际拉到的版本。注意：GOOGLETEST_VERSION 是 googletest 自己
# add_subdirectory 作用域里的变量，父目录读不到（首次实配时取到空串，
# 断言以"版本漂移"的形式开火 —— 那是我的取值方式错了，不是真漂移）。
# 所以直接从拉下来的源码 CMakeLists.txt 里把它抓出来：这证明的是
# 落到磁盘上的内容，而不是我自己声明想要的 tag。
set(_gtest_cml "${googletest_SOURCE_DIR}/CMakeLists.txt")
if(NOT EXISTS "${_gtest_cml}")
  message(FATAL_ERROR "FetchContent 声称成功但找不到 ${_gtest_cml}，无法核对 GoogleTest 版本。")
endif()
file(STRINGS "${_gtest_cml}" _gtest_version_line REGEX "set[ \t]*\\(GOOGLETEST_VERSION")
string(REGEX REPLACE ".*GOOGLETEST_VERSION[ \t]+([0-9a-zA-Z.\\-]+).*" "\\1"
       L7GATE_GTEST_VERSION "${_gtest_version_line}")
if(NOT L7GATE_GTEST_VERSION VERSION_EQUAL "1.14.0")
  message(FATAL_ERROR
    "GoogleTest 版本漂移：期望 1.14.0，实际 '${L7GATE_GTEST_VERSION}'（读自 ${_gtest_cml}）。"
    "按契约 8.2 禁令三，任何版本变更必须先写 ADR。")
endif()
message(STATUS "l7gate: GoogleTest ${L7GATE_GTEST_VERSION} (contract-locked v1.14.0, verified from source)")

# ---------------------------------------------------------------------------
# 目标
# ---------------------------------------------------------------------------
add_library(l7gate_core STATIC
  src/core/build_info.cc
  src/net/buffer.cc
  src/http/request_parser.cc
)
target_include_directories(l7gate_core PUBLIC src)
target_link_libraries(l7gate_core PUBLIC l7gate_warnings l7gate_sanitizers)
target_compile_definitions(l7gate_core PUBLIC
  L7GATE_VERSION="${PROJECT_VERSION}"
  L7GATE_GTEST_VERSION="${L7GATE_GTEST_VERSION}"
  L7GATE_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
  # 把"要求开哪些 sanitizer"传进编译期，让单测能断言它真的生效
  # （见 tests/build_info_test.cc 的 RequestedSanitizersAreActuallyCompiledIn）。
  L7GATE_SANITIZERS_EXPECTED="${L7GATE_SANITIZERS}"
)

add_executable(l7gate src/main.cc)
target_link_libraries(l7gate PRIVATE l7gate_core)

enable_testing()
add_subdirectory(tests)
