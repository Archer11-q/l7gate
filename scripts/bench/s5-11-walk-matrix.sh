#!/usr/bin/env bash
# S5-11 的矩阵驱动（第二版，按容量事实拆两组主尺）。
#
# 为什么要拆：回源侧容量 = **min(workers, E) × 28,232**（S2-06a 候选 A 的形状，ADR-17 末条）。
#   · n=100,000 这一档只有 workers ≥ 4 才开得出 → 主尺 A 用 workers=16（SLO #2 与 B-007/B-021 的档）。
#   · workers=1 是 shipped 缺省，也是"一次走满整张注册表"那一档，但它最多只能开到 28,232 对，取 28,000 留余量 → 主尺 B 用它。
#   两组不能互比绝对值（连接数不同），能比的是"每秒各花多少 µs"与"每连接对每秒多少 ns"。
#
# 用法：
#   bash scripts/bench/s5-11-walk-matrix.sh                      # 全矩阵（跑很久，见每条臂的 ETA）
#   ONLY="A" bash ...                                            # 只跑主尺 A
#   ONLY="sweep" bash ...                                        # 只跑 n 扫描
#   ONLY="perf" bash ...                                         # 只跑归因臂
#   ROUNDS=1 bash ...                                            # 每臂只一轮（**取样规程不齐，不进结论**）
# 判定与差值一律由 s5-11-walk-report.sh 从各臂落盘的 summary.txt / verdict.txt 算，本脚本只管跑与记账。
set -uo pipefail
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1

ROUNDS="${ROUNDS:-2}"
ONLY="${ONLY:-A B sweep perf}"
BASE="${OUT_DIR:-build/bench/s5-11-matrix}"
TABLE="$BASE/table.tsv"
CEILING=28232
mkdir -p "$BASE" || exit 1
[[ -f "$TABLE" ]] || printf 'arm\tn\tworkers\tround\tkind\tcpu_us_per_second\tcpu_ticks_delta\twindow_seconds\tverdict\tclient_rc\tdir\n' > "$TABLE"

note() { echo "  $*"; }
fatal() { echo "  ✗ $*" >&2; exit 1; }
field() { awk -v k="$1" -F= '$1==k {print $2; exit}' "$2"; }

# 一臂一轮。参数：arm n workers round kind [perf]
GATEWAY_PORT="${GATEWAY_PORT:-18213}"
METRICS_PORT="${METRICS_PORT:-18214}"

# 起臂之前的清场门：上一臂漏下来的进程会占住业务端口**并吃掉 ~28k 个临时端口**，
# 于是后面每一臂的夹具连 `bind :0` 都失败（本轮实测 errno=98）。这不是礼貌问题，
# 一臂的失败会连锁伪造成"整组都不成立"——宁可中止也不带着残留继续跑。
precheck() {
  local left
  left=$(pgrep -f "[l]7gate --serve|[l]7gate_bench_upstream|[l]7gate_c100k" 2>/dev/null | tr '\n' ' ')
  if [[ -n "${left// /}" ]]; then
    note "✗ 起臂之前还有残留进程：$left —— 矩阵中止（不清场不继续）"; return 1
  fi
  if ss -ltn 2>/dev/null | grep -qE ":($GATEWAY_PORT|$METRICS_PORT)[[:space:]]"; then
    note "✗ 起臂之前 $GATEWAY_PORT/$METRICS_PORT 仍被监听 —— 矩阵中止"; return 1
  fi
  return 0
}

one_round() {
  local arm="$1" n="$2" w="$3" r="$4" kind="$5" perf="${6:-0}" hold_ov="${7:-}" win_ov="${8:-}" settle_ov="${9:-}"
  local targets ep ramp hs hold settle window dir rc us td ws v est
  if (( n > CEILING )); then targets=4; ep=4; else targets=2; ep=1; fi
  ramp=$(( n <= 1000 ? 5000 : (n <= 10000 ? 30000 : (n <= 30000 ? 120000 : 300000)) ))
  hs=$(( n <= 10000 ? 15000 : (n <= 30000 ? 60000 : 120000) ))
  hold=$(( n <= 10000 ? 60 : 110 ))
  settle=$(( n <= 10000 ? 8 : 20 ))
  window=$(( n <= 10000 ? 40 : 60 ))
  [[ -n "$win_ov" ]] && { hold="$hold_ov"; window="$win_ov"; settle="$settle_ov"; }
  dir="$BASE/${kind}-c${n}-w${w}-${arm}-r${r}$([[ "$perf" == "1" ]] && echo "-perf")"
  if ! precheck; then
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
      "$arm" "$n" "$w" "$r" "$kind" NA NA NA PRECHECK_FAIL NA "$dir" >> "$TABLE"
    return 1
  fi
  mkdir -p "$dir" || return 1
  note "ETA≈$(( (ramp/1000 + hold + 90) / 60 )) 分钟：$kind ARM=$arm n=$n workers=$w targets=$targets E=$ep ramp=${ramp}ms hold=${hold}s 窗=${window}s perf=$perf → $dir"
  ARM="$arm" CONNECTIONS="$n" TARGETS="$targets" UPSTREAM_ENDPOINTS="$ep" WORKERS="$w" \
  RAMP_MS="$ramp" HANDSHAKE_TIMEOUT_MS="$hs" HOLD_SECONDS="$hold" SETTLE_SECONDS="$settle" \
  WINDOW_SECONDS="$window" DRAIN_TIMEOUT_MS=30000 PERF="$perf" OUT_DIR="$dir" \
  GATEWAY_PORT="$GATEWAY_PORT" METRICS_PORT="$METRICS_PORT" \
    timeout 2400 bash scripts/bench/s5-11-disarmed-walk.sh > "$dir/driver.log" 2>&1
  rc=$?
  v=$(cat "$dir/verdict.txt" 2>/dev/null || echo NO_VERDICT)
  us=$(field cpu_us_per_second "$dir/summary.txt")
  td=$(field cpu_ticks_delta "$dir/summary.txt")
  ws=$(field window_seconds "$dir/summary.txt")
  est=$(field established "$dir/summary.txt")
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$arm" "$n" "$w" "$r" "$kind" "${us:-NA}" "${td:-NA}" "${ws:-NA}" "$v" "${est:-NA}" "$dir" >> "$TABLE"
  [[ "$v" == PASS* ]] || note "!! $dir 判定不是 PASS：$v（驱动 rc=$rc）"
  return 0
}

for KIND in $ONLY; do
  case "$KIND" in
    A) # 主尺 A：SLO #2 那一档（100k / workers=16），off→on→on→off 交替
      note "=== 主尺 A：n=100000 workers=16（SLO #2 档）==="
      for (( r = 1; r <= ROUNDS; ++r )); do
        if (( r % 2 == 1 )); then one_round off 100000 16 "$r" A; one_round on 100000 16 "$r" A
        else one_round on 100000 16 "$r" A; one_round off 100000 16 "$r" A; fi
      done ;;
    B) # 主尺 B：workers=1（shipped 缺省，一次走满整张表），容量顶 ≈ 28,232（取 28,000 留余量）
      note "=== 主尺 B：n=28000 workers=1（shipped 缺省那一臂）==="
      for (( r = 1; r <= ROUNDS; ++r )); do
        if (( r % 2 == 1 )); then one_round off 28000 1 "$r" B; one_round on 28000 1 "$r" B
        else one_round on 28000 1 "$r" B; one_round off 28000 1 "$r" B; fi
      done ;;
    sweep) # 下界：workers=1 下 n=1,000 / 10,000（28,232 那点由主尺 B 提供）
      note "=== 规模扫描：workers=1，n=1000 / 10000 ==="
      for N in 1000 10000; do one_round off "$N" 1 s sweep; one_round on "$N" 1 s sweep; done ;;
    perf) # 归因：perf 直接拉起网关
      note "=== 归因臂：perf 直接拉起（workers=1 / n=28000 与 workers=16 / n=100000 各一发）==="
      # 为什么窗要拉到 840 s：perf 只能"由 perf 直接拉起被测进程"（ENVIRONMENT 4.1），取不到"只采静置窗"，
      # profile 是整臂的。ramp 120 s 是重活，而 w1/28k 那一臂的遍历只有 5 ms/s——窗不够长时 run() 那一格
      # 被建连阶段的样本盖住，读出来的份额不是静置窗的份额。HOLD 900 / 窗 840 让静置占住 profile。
      note "   长窗理由：perf 只能整臂取样，静置必须盖过建连阶段（ramp 120 s vs 窗 840 s）"
      one_round on 28000 1 p perf 1 900 840 20
      one_round off 28000 1 p perf 1 900 840 20 ;;
    *) fatal "ONLY 里有不认识的臂：$KIND（可用 A B sweep perf）" ;;
  esac
done

note "表落在 $TABLE"
column -t -s "$(printf '\t')" "$TABLE" 2>/dev/null || cat "$TABLE"
