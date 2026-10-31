#!/usr/bin/env bash
# 三 preset 全量构建 + ctest，并统计我方代码警告与第三方噪声警告。
# 用途：任务收尾时的一次性验证，结论写进 TASKS.md 的「实测数字」列。
#
# S4-02（原待裁⑥结掉）：任一 preset 的 configure/build/ctest 失败 → 脚本退出码 1；
# build 失败时**不再**拿旧二进制跑 ctest。旧行为有两个失效形态，本轮都撞上了：
#   ① 构建失败仍跑 ctest，旧二进制冒充通过（clang 那两个 preset 曾这样被读成 164/164）；
#   ② ctest 失败被管道吞掉（tsan 全量 1 条失败，脚本仍报 rc=0）。
set -uo pipefail
cd "$(dirname "$(readlink -f "$0")")/.." || exit 1

fail=0
for p in debug asan-ubsan tsan; do
  cfg=/tmp/l7_${p}_cfg.log
  bld=/tmp/l7_${p}_build.log
  cmake --preset "$p" > "$cfg" 2>&1
  rc=$?
  echo "== $p configure exit=$rc"
  if (( rc != 0 )); then fail=1; continue; fi
  cmake --build --preset "$p" -j8 > "$bld" 2>&1
  rc=$?
  echo "   build exit=$rc  warning_total=$(grep -cE 'warning:' "$bld")  非gtest警告=$(grep -E 'warning:' "$bld" | grep -vc '_deps/')"
  if (( rc != 0 )); then fail=1; continue; fi
  out=$(ctest --preset "$p" 2>&1)
  rc=$?
  printf '%s\n' "$out" | grep -E 'tests passed|tests failed' | sed 's/^/   /'
  (( rc == 0 )) || fail=1
done

echo "== 按任务筛选（-R 用 gtest 套件名）"
ctest --test-dir build/debug -L s1 -R 'S1_02_Buffer' 2>&1 | grep -E 'tests passed|Test #' || fail=1

exit "$fail"
