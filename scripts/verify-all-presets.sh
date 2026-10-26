#!/usr/bin/env bash
# 三 preset 全量构建 + ctest，并统计我方代码警告与第三方噪声警告。
# 用途：任务收尾时的一次性验证，结论写进 TASKS.md 的「实测数字」列。
set -uo pipefail
cd /home/archer/l7gate || exit 1

for p in debug asan-ubsan tsan; do
  cfg=/tmp/l7_${p}_cfg.log
  bld=/tmp/l7_${p}_build.log
  cmake --preset "$p" > "$cfg" 2>&1
  echo "== $p configure exit=$?"
  cmake --build --preset "$p" -j8 > "$bld" 2>&1
  echo "   build exit=$?  warning_total=$(grep -cE 'warning:' "$bld")  非gtest警告=$(grep -E 'warning:' "$bld" | grep -vc '_deps/')"
  ctest --preset "$p" 2>&1 | grep -E 'tests passed|tests failed' | sed 's/^/   /'
done

echo "== 按任务筛选（-R 用 gtest 套件名）"
ctest --test-dir build/debug -L s1 -R 'S1_02_Buffer' 2>&1 | grep -E 'tests passed|Test #'
