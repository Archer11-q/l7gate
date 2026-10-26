#!/usr/bin/env bash
# SLO 脚本索引里的「前置」那一项：每次压测前把**环境本身**落盘一份。
#
# 为什么这不是可选步骤：契约第 7 节对 BENCHMARK.md 的完成判据写的是
# 「含 CPU 绑核、分配器、各组件版本号」——一份没有环境快照的压测数字，
# 半年后连我自己都无法判断它和今天这份是否可比。可比性这件事不是靠
# "我记得我什么都没改"来保证的，是靠两次的快照能逐行 diff 来保证的。
#
# 输出是一份 key=value 的纯文本（grep 友好、diff 友好），不做排版：
# 排版给人看的部分归 BENCHMARK.md，这份是给机器核对的。
set -uo pipefail

cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1
OUT_DIR="${OUT_DIR:-build/bench/env}"
mkdir -p "$OUT_DIR" || exit 1
STAMP="${STAMP:-$(date +%Y%m%d-%H%M%S)}"
SNAP="$OUT_DIR/env-$STAMP.txt"

k() { printf '%-28s = %s\n' "$1" "$2"; }

{
  k captured_at "$(date -Is)"
  k hostname "$(uname -n)"
  k uname "$(uname -sr)"
  k distro "$(sed -n 's/^PRETTY_NAME="\(.*\)"/\1/p' /etc/os-release | head -1)"

  # --- CPU：型号、核数、当前频率、调频策略、绑核状态 -----------------------
  # model name 那行是 **tab** 分隔（"model name\t: AMD Ryzen..."），用 [ ] 匹配
  # 会读成空串——s1-p99.sh 的第一版就踩了这个，报告里 CPU= 后面什么都没有。
  k cpu_model "$(sed -n 's/^model name[[:space:]]*:[[:space:]]*\(.*\)$/\1/p' /proc/cpuinfo | head -1)"
  k nproc "$(nproc)"
  k cpu_mhz "$(sed -n 's/^cpu MHz[[:space:]]*:[[:space:]]*\(.*\)$/\1/p' /proc/cpuinfo | head -1)"
  k cpufreq_governor "$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo '不可读（WSL 常无 cpufreq 接口）')"
  k taskset "未绑核（脚本未使用 taskset/cpuset；绑核方案按 ENVIRONMENT.md 第 7 节留到 S4）"

  # --- 内存与 huge page：每连接内存那条口径的干扰项 -----------------------
  k mem_total_kb "$(sed -n 's/^MemTotal:[[:space:]]*\([0-9]*\).*/\1/p' /proc/meminfo)"
  k thp_enabled "$(cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || echo 不可读)"

  # --- 文件描述符与监听队列上限 -------------------------------------------
  k ulimit_n "$(ulimit -n)"
  k file_max "$(cat /proc/sys/fs/file-max 2>/dev/null || echo 不可读)"
  k somaxconn "$(cat /proc/sys/net/core/somaxconn 2>/dev/null || echo 不可读)"
  k ip_local_port_range "$(cat /proc/sys/net/ipv4/ip_local_port_range 2>/dev/null | tr '\t' ' ')"
  k tcp_tw_reuse "$(cat /proc/sys/net/ipv4/tcp_tw_reuse 2>/dev/null || echo 不可读)"

  # --- 被测程序与它的依赖：版本号一次取全 ---------------------------------
  for t in gcc clang cmake wrk curl; do
    if command -v "$t" >/dev/null 2>&1; then
      k "${t}_version" "$("$t" --version 2>&1 | head -1)"
    else
      k "${t}_version" "未安装"
    fi
  done
  # hey 没有 --version，也没有 -v：唯一可靠的版本来源是 dpkg（ENVIRONMENT.md 第 1 节）。
  k hey_version "$(dpkg-query -W -f='${Version}' hey 2>/dev/null || echo 未安装/非 dpkg 安装)"
  k glibc_version "$(ldd --version 2>&1 | head -1)"

  # --- 分配器：SLO #2/#3 都写明必须记录，换分配器等于换指标定义 -----------
  # 判据不是"系统里装了 jemalloc"，而是**这个可执行文件实际链的是谁**：
  # LD_PRELOAD 能覆盖 ldd 的结论，所以两处都要看，缺一处就可能把 glibc 的数
  # 记成 jemalloc 的数。
  for exe in build/release/l7gate build/release/tools/l7gate_bench_upstream; do
    if [[ -x "$exe" ]]; then
      k "allocator_of[$(basename "$exe")]" "$(ldd "$exe" 2>/dev/null | awk '/libjemalloc|libtcmalloc|libc\.so/ {print $1; exit}')"
    fi
  done
  k ld_preload "${LD_PRELOAD:-（空）}"
  k git_commit "$(git rev-parse --short HEAD 2>/dev/null || echo 非 git)"
  k git_dirty "$(git status --porcelain 2>/dev/null | wc -l | tr -d ' ')"

  # --- 仓库所在文件系统：契约 9.4 的硬约束（9P 抖动会直接进 p99） ---------
  k repo_device "$(findmnt -no FSTYPE -T . 2>/dev/null || echo 不可读)"
  k repo_note "必须是 ext4/WSL 原生；9p/drvfs 上的数字既不属于网关也不属于 Nginx（契约 9.4）"

  # --- 构建产物：release 才是 BENCHMARK 允许的那一个 ----------------------
  k preset "release"
  k l7gate_sha256 "$(sha256sum build/release/l7gate 2>/dev/null | cut -c1-16 || echo 缺失)"
} | tee "$SNAP"

echo "== 环境快照 → $SNAP" >&2
