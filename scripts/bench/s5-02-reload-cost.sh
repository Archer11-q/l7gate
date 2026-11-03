#!/usr/bin/env bash
# S5-02 成本与常驻两臂对照（ADR-10 追加节第七节那三条代价里的第 1、3 条）。
#
# 主尺是**每请求整数等式**（已裁第 ⑨ 条第 5 项）：`epoll_modifications ÷ requests_parsed`
# ——这条不依赖任何分辨率。p50/QPS 只当带外报警器，RSS 那一格量的是"arm 了要常驻多少"。
#
# 为什么不用 `s1-p999.sh` / `s1-p99.sh` 那两个既有取数脚本：它们把 `--upstream` 写死在命令行里，
# 而 `--config` 与 `--upstream` 互斥（已裁②），所以"带着配置文件的臂"在那两个脚本里跑不出来。
# **不改它们**——动它们的命令行形状会让 B-001…B-019 那些历史档逐字同形那条不作废。
# 本脚本因此自带一套两臂：两臂之间**只差 `--config-reload-ms` 这一个参数**，其余逐字相同，
# 所以可比性是这条脚本内部的性质，不跨档宣称。
#
# 用法：PRESET=release ROUNDS=2 bash scripts/bench/s5-02-reload-cost.sh          # 全新三轮/两轮
#       ONLY_ROUNDS=3 ROUNDS=3 bash scripts/bench/s5-02-reload-cost.sh           # 只补跑第 3 轮
#       ARMS=reload ROUNDS=3 ONLY_ROUNDS=3 bash scripts/bench/s5-02-reload-cost.sh  # 只补某一臂某轮
#       SKIP_RUNS=1 bash scripts/bench/s5-02-reload-cost.sh                       # 不跑，只对落盘重算判定
# 输出：build/bench/s5-02-reload-cost/（每臂每轮一个目录 + summary.tsv）
# **补跑路径一律不删目录**（见 OUT 那格守卫）。
set -uo pipefail
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1

PRESET="${PRESET:-release}"
ROUNDS="${ROUNDS:-2}"
WORKERS="${WORKERS:-1}"
GW_PORT="${GW_PORT:-18710}"
UP_PORT="${UP_PORT:-18711}"
DURATION="${DURATION:-45}"
CONNS="${CONNS:-100}"
THREADS="${THREADS:-2}"
OUT="build/bench/s5-02-reload-cost"
# 只在**起跑全新的一轮**时清空。给了 `ONLY_ROUNDS` / `ARMS`（补跑某一臂某一轮）或 `SKIP_RUNS=1`
# （只对已落盘重算）时绝不删——删一次就把已经合格的臂一起抹掉，判定只剩补跑那一臂可用
# （`s5-01-metrics-cost.sh` 那轮的 §20.1 第 3 条就是这个形状，本轮一开始还是无条件 `rm -rf` 写的）。
if [[ -z "${ONLY_ROUNDS:-}" && -z "${ARMS:-}" && "${SKIP_RUNS:-0}" != "1" ]]; then
  rm -rf "$OUT"
fi
mkdir -p "$OUT"

GATEWAY="build/$PRESET/l7gate"
UPSTREAM="build/$PRESET/tests/l7gate_test_upstream"
[[ -x "$GATEWAY" && -x "$UPSTREAM" ]] || { echo "缺 $PRESET 二进制"; exit 1; }
command -v wrk >/dev/null || { echo "缺 wrk"; exit 1; }

port_free() { ! (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; }
for p in "$GW_PORT" "$UP_PORT"; do port_free "$p" || { echo "端口 $p 被占"; exit 1; }; done

CFG="$OUT/gateway.json"
cat > "$CFG" <<JSON
{
  "upstreams": [
    { "name": "api", "balance": "round_robin", "targets": [
      { "address": "127.0.0.1", "port": $UP_PORT } ] }
  ],
  "routes": [ { "match": "prefix", "pattern": "", "upstream": "api" } ],
  "health": { "max_fails": 3, "probe_interval_ms": 0 },
  "rate_limit": { "rate": 1000000, "burst": 1000000, "key": "path" }
}
JSON
# 两臂共用**同一份文件**且全程不改它：这一档量的是"arm 之后闲着不动"的稳态成本，
# 不是重载那一下的成本（那格由 `config_last_publish_duration_ms` 自报，冒烟阶段 2 已经量到）。

"$UPSTREAM" --listen "127.0.0.1:$UP_PORT" --body-bytes 1024 > "$OUT/upstream.log" 2>&1 & UP=$!
trap 'kill "$UP" 2>/dev/null' EXIT
sleep 0.5
grep -q '监听' "$OUT/upstream.log" || { echo "上游没起来"; exit 1; }

gval() { sed -n "s/^\[serve\] $1=\([0-9-]*\).*/\1/p" "$2" | head -1; }

echo "=== S5-02 成本两臂：PRESET=$PRESET workers=$WORKERS rounds=$ROUNDS wrk -t$THREADS -c$CONNS -d${DURATION}s ==="
printf 'arm\tround\tepoll_mod\trequests_parsed\tper_request\tp50_ms\tqps\trss_end_kib\trc\n' > "$OUT/summary.tsv"

ARMS="${ARMS:-off reload}"
for ((r = 1; r <= ROUNDS; r++)); do
  # 补跑某一轮就只跑那一轮（`ONLY_ROUNDS=3`）。这三个旋钮本轮**先写进了注释、后来才发现循环里
  # 没实现**——我按 `ONLY_ROUNDS=3` 起跑，实际把三轮全跑了一遍（后果不致命：那一跑反而产出了
  # 一份 QPS 解析正确的完整三轮数据，而旧的两轮被覆盖前已经落进日志）。记在 FAULT-INJECTION
  # §21.2 第 6 条：脚本里写了旋钮就必须有人读它，与"配置写了、代码没读"是同一条雷。
  if [[ -n "${ONLY_ROUNDS:-}" ]] && [[ " $ONLY_ROUNDS " != *" $r "* ]]; then continue; fi
  if [[ "${SKIP_RUNS:-0}" == "1" ]]; then continue; fi
  for arm in $ARMS; do
    dir="$OUT/$arm-r$r"
    mkdir -p "$dir"
    extra=""
    [[ "$arm" == "reload" ]] && extra="--config-reload-ms 200"
    # 同机交替（取样规程）：两臂在同一轮里前后相接，跨轮再重复一次
    # shellcheck disable=SC2086
    "$GATEWAY" --serve --listen "127.0.0.1:$GW_PORT" --config "$CFG" --workers "$WORKERS" \
               $extra > "$dir/gateway.log" 2>&1 &
    G=$!
    for _ in $(seq 1 60); do grep -q '\[serve\] 监听' "$dir/gateway.log" 2>/dev/null && break; sleep 0.1; done
    if ! kill -0 "$G" 2>/dev/null; then
      echo "  ✗ $arm-r$r 网关起不来"; echo 1 > "$dir.rc"; continue
    fi
    wrk -t"$THREADS" -c"$CONNS" -d"${DURATION}s" --latency \
        "http://127.0.0.1:$GW_PORT/cost" > "$dir.wrk.txt" 2>&1
    wrk_rc=$?
    kill -TERM "$G" 2>/dev/null; wait "$G" 2>/dev/null
    echo "$wrk_rc" > "$dir.rc"          # 驱动自己写 rc，判定读这个文件（不在别人的输出里找自己的数）
    ep=$(gval epoll_modifications "$dir/gateway.log")
    pa=$(gval requests_parsed "$dir/gateway.log")
    rss=$(gval rss_end_kib "$dir/gateway.log")
    p50=$(awk '/^[ ]*50%/ {print $2; exit}' "$dir.wrk.txt")
    # `Requests/sec:` 后面 wrk 打的是**两个**空格：`sed -n 's/^Requests\/sec: \([0-9.]*\).*/'`
    # 一条都不匹配，四行 QPS 全读成 "?"（§20.1 第 4 条同族：取数的行没读错，是模式没匹配上，
    # 而空值在判定里长得像 0）。按字段取就不依赖空格数。
    qps=$(awk '/^Requests\/sec:/ {print $2; exit}' "$dir.wrk.txt")
    ratio=$(awk -v a="${ep:-0}" -v b="${pa:-0}" 'BEGIN{ if (b+0>0) printf "%.6f", a/b; else print "?" }')
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$arm" "$r" "${ep:-?}" "${pa:-?}" "$ratio" \
      "${p50:-?}" "${qps:-?}" "${rss:-?}" "$wrk_rc" >> "$OUT/summary.tsv"
    echo "  $arm-r$r: parsed=${pa:-?} epoll_MOD=${ep:-?} 每请求=$ratio p50=${p50:-?} QPS=${qps:-?} rss=${rss:-?}KiB rc=$wrk_rc"
  done
done

echo
echo "== 判定（主尺：每请求整数等式；p50/QPS 只当带外报警器）=="
awk -F'\t' 'NR>1 {
  if ($1=="off")   { o_n++;   o_ratio+=$5+0; o_q+=$7+0;   o_r+=$8+0;   o_p50=$6; o_rmin=($8+0<o_rmin||o_rmin=="")?$8+0:o_rmin; o_rmax=($8+0>o_rmax)?$8+0:o_rmax }
  if ($1=="reload"){ r_n++;   r_ratio+=$5+0; r_q+=$7+0;   r_r+=$8+0;   r_p50=$6; r_rmin=($8+0<r_rmin||r_rmin=="")?$8+0:r_rmin; r_rmax=($8+0>r_rmax)?$8+0:r_rmax }
}
END {
  if (o_n==0 || r_n==0) { print "  ✗ 有一臂一个合格读数都没有（off=" o_n " reload=" r_n "），判定不成立"; exit 1 }
  printf "  off    臂：每请求等式均值 %.6f，RSS 均值 %.0f KiB\n", o_ratio/o_n, o_r/o_n
  printf "  reload 臂：每请求等式均值 %.6f，RSS 均值 %.0f KiB\n", r_ratio/r_n, r_r/r_n
  printf "  跨臂差：等式 %+.6f，RSS %+.0f KiB（%.1f%%）\n", r_ratio/r_n - o_ratio/o_n, r_r/r_n - o_r/o_n, (r_r/r_n - o_r/o_n)*100.0/(o_r/o_n)
  printf "  p50：off=%s reload=%s（wrk 那一档的分辨率由组内极差自己说，见下面原始行）\n", o_p50, r_p50
}' "$OUT/summary.tsv"
echo
echo "  合格性看的是每臂自己那格的 rc（$OUT/*-r*.rc），不看渲染好的表。"
bad=$(awk -F'\t' 'NR>1 && $9+0!=0 {c++} END{print c+0}' "$OUT/summary.tsv")
echo "  rc≠0 的行数：$bad"
(( bad == 0 )) || exit 1
echo "S5-02-COST DONE（原始数据在 $OUT）"
