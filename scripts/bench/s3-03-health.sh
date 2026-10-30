#!/usr/bin/env bash
# S3-03 实测（门禁第 1、2 个数 + 安全阀）：真进程、真 socket、release 构建。
#
#   阶段 1  摘除耗时：杀掉一台后端，从「进程死亡时刻」（脚本墙钟）到「网关自报的
#           摘除时刻」（`health_t<i>_..._last_ejection_epoch_ms`，同一根墙钟轴）
#           两端钉住。摘除之后 10 条业务请求必须全部由活着的两台服务。
#   阶段 2  误摘率（**必须带分母**）：三台全健康的后端跑 ≥1000 轮探活，
#           分母 = 探活轮数 × 健康后端数，分子 = 健康后端被摘次数（期望 0）。
#   阶段 3  安全阀（ADR-12 第一次真正落地）：10 台后端全死、上限 10% →
#           被摘除恰好停在 1 台，其余的摘除请求被挡且有可观测计数。
#
# 判据成对的来历（ADR-12 落地追加节）：只看"误摘率 ≤ X"挡不住"从不摘除"的退化实现，
# 所以阶段 1/3 同时交付"摘除确实发生过"的下界。
#
# 用法：PRESET=release scripts/bench/s3-03-health.sh
# 输出：build/bench/s3-03/ 下的日志 + 本脚本 stdout 的 B-014 读数；退出码 0 = 三段全过。
set -uo pipefail
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1

trap '' PIPE
PRESET="${PRESET:-release}"
BIN="build/$PRESET"
GATEWAY="$BIN/l7gate"
UPSTREAM="$BIN/tests/l7gate_test_upstream"
OUT="build/bench/s3-03"
mkdir -p "$OUT"

FAILED=0
fail() { echo "  ✗ $*"; FAILED=1; }
ok()   { echo "  ✓ $*"; }
die()  { echo "  fatal: $*" >&2; exit 1; }

[[ -x "$GATEWAY" ]] || die "找不到 $GATEWAY，先 cmake --build --preset $PRESET"
[[ -x "$UPSTREAM" ]] || die "找不到 $UPSTREAM，先 cmake --build --preset $PRESET"
command -v curl >/dev/null || die "本机没有 curl"

port_free() { ! (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; }
wait_for_line() { # pid logfile pattern [seconds]
  local pid=$1 log=$2 pat=$3 secs=${4:-15} i=0
  while (( i < secs * 20 )); do
    if grep -q -- "$pat" "$log" 2>/dev/null; then return 0; fi
    if ! kill -0 "$pid" 2>/dev/null; then
      echo "  进程 $pid 已退出，日志尾部：" >&2; tail -5 "$log" >&2; return 1
    fi
    ((i++)); sleep 0.05
  done
  return 1
}
get_stat() { sed -n "s/^\[serve\] $1=\([0-9]*\).*/\1/p" "$2" | head -1; }
epoch_ms() { echo $(( ${EPOCHREALTIME/./} / 1000 )); }
req() { # port path → "code bytes"
  local port=$1 path=$2 file="$OUT/body.$$"
  local code
  code=$(curl -s --max-time 5 -o "$file" -w '%{http_code}' "http://127.0.0.1:$port$path" 2>/dev/null)
  local n; n=$(wc -c < "$file" 2>/dev/null); rm -f "$file"
  echo "${code:-000} ${n:-0}"
}

PIDS=()
cleanup() {
  for pid in "${PIDS[@]}"; do kill -9 "$pid" 2>/dev/null; done
  wait 2>/dev/null
}
trap cleanup EXIT

# 起一台上游并等监听行。成功把 PID 追加进 PIDS、端口写进 $RET_PORT。
start_upstream() { # port body_bytes log [extra-flags...]
  local port=$1 bytes=$2 log=$3; shift 3
  "$UPSTREAM" --listen "127.0.0.1:$port" --body-bytes "$bytes" "$@" > "$log" 2>&1 &
  local pid=$!
  PIDS+=("$pid")
  wait_for_line "$pid" "$log" "监听" 15 || return 1
  return 0
}

# ============================ 阶段 1：摘除耗时 ==================================
echo "== B-014 阶段 1：摘除耗时（3 台、round_robin、探活 100ms、max_fails=2）=="
U1=18181; U2=18182; U3=18183; GW1=18180
for p in $GW1 $U1 $U2 $U3; do port_free "$p" || die "端口 $p 被占用：先清场"; done

cat > "$OUT/eject.json" <<'JSON'
{
  "upstreams": [
    { "name": "api", "balance": "round_robin",
      "targets": [ { "address": "127.0.0.1", "port": 18181 },
                    { "address": "127.0.0.1", "port": 18182 },
                    { "address": "127.0.0.1", "port": 18183 } ] }
  ],
  "routes": [ { "match": "prefix", "pattern": "", "upstream": "api" } ],
  "health": { "max_fails": 2, "fail_timeout_ms": 60000,
              "probe_interval_ms": 100, "probe_timeout_ms": 500,
              "max_ejection_percent": 50 }
}
JSON

start_upstream "$U1" 64 "$OUT/eject-up1.log" || die "上游 1 没起来"
start_upstream "$U2" 128 "$OUT/eject-up2.log" || die "上游 2 没起来"
start_upstream "$U3" 192 "$OUT/eject-up3.log" || die "上游 3 没起来"
"$GATEWAY" --serve --listen "127.0.0.1:$GW1" --config "$OUT/eject.json" --workers 1 \
  > "$OUT/eject-gw.log" 2>&1 & GW=$!
PIDS+=("$GW")
wait_for_line "$GW" "$OUT/eject-gw.log" "health=on" 15 || die "网关没打出 health=on 横幅"
wait_for_line "$GW" "$OUT/eject-gw.log" "监听" 15 || die "网关没起来"

# 预热：6 条把三台都配过一次（RR），摘除发生时三台的池状态是同构的。
for i in 1 2 3 4 5 6; do
  read -r c n <<< "$(req "$GW1" /warm)"
  [[ "$c" == "200" ]] || die "预热请求失败：code=$c"
done

KILL_MS=$(epoch_ms)
kill -9 "${PIDS[1]}" 2>/dev/null || die "杀上游 2 失败（PIDS[1] = ${PIDS[1]:-空}）"
# 探活间隔 100ms × max_fails 2 → 摘除应在几百毫秒内；留 1.5s 再验"不再收请求"。
sleep 1.5
BAD=0
for i in $(seq 1 10); do
  read -r c n <<< "$(req "$GW1" /after-eject)"
  if [[ "$c" != "200" || "$n" == "128" ]]; then BAD=1; fi
done
if (( BAD )); then fail "摘除后 10 条请求里有失败或落到了死后端（128 B）"; else ok "摘除后 10 条全部 200 且无一落到 128 B"; fi

kill -TERM "$GW" 2>/dev/null; wait "$GW" 2>/dev/null
EJECT_MS_LINE=$(get_stat "health_t1_127.0.0.1_${U2}_last_ejection_epoch_ms" "$OUT/eject-gw.log")
EJ2=$(get_stat "health_t1_127.0.0.1_${U2}_ejections" "$OUT/eject-gw.log")
EJ1=$(get_stat "health_t0_127.0.0.1_${U1}_ejections" "$OUT/eject-gw.log")
EJ3=$(get_stat "health_t2_127.0.0.1_${U3}_ejections" "$OUT/eject-gw.log")
if [[ -z "$EJECT_MS_LINE" ]]; then
  fail "网关没自报 2 号目标的摘除时刻（health_t1_..._last_ejection_epoch_ms）"
else
  EJECT_MS=$(( EJECT_MS_LINE - KILL_MS ))
  echo "  摘除耗时（kill → 网关自报摘除）= ${EJECT_MS} ms（含至多一个 100ms 探活间隔）"
  if (( EJECT_MS >= 0 && EJECT_MS < 3000 )); then ok "摘除在 3s 预算内发生"; else fail "摘除耗时 ${EJECT_MS} ms 异常"; fi
fi
if [[ "${EJ2:-0}" == "1" ]]; then ok "死后端恰好被摘 1 次（下界：摘除确实发生过）"
else fail "死后端被摘次数 = ${EJ2:-?}，期望 1"; fi
if [[ "${EJ1:-1}" == "0" && "${EJ3:-1}" == "0" ]]; then
  ok "两台健康后端零误摘"
else
  fail "健康后端被摘：18181=${EJ1:-?} 18183=${EJ3:-?}"
fi

# ============================ 阶段 2：误摘率（带分母）============================
echo "== B-014 阶段 2：误摘率（3 台全健康、探活 20ms、跑 25s ≈ 1250 轮）=="
V1=18281; V2=18282; V3=18283; GW2=18280
for p in $GW2 $V1 $V2 $V3; do port_free "$p" || die "端口 $p 被占用：先清场"; done

cat > "$OUT/misrate.json" <<'JSON'
{
  "upstreams": [
    { "name": "api", "balance": "round_robin",
      "targets": [ { "address": "127.0.0.1", "port": 18281 },
                    { "address": "127.0.0.1", "port": 18282 },
                    { "address": "127.0.0.1", "port": 18283 } ] }
  ],
  "routes": [ { "match": "prefix", "pattern": "", "upstream": "api" } ],
  "health": { "max_fails": 3, "fail_timeout_ms": 10000,
              "probe_interval_ms": 20, "probe_timeout_ms": 500,
              "max_ejection_percent": 10 }
}
JSON

start_upstream "$V1" 64 "$OUT/mis-up1.log" || die "上游 1 没起来"
start_upstream "$V2" 128 "$OUT/mis-up2.log" || die "上游 2 没起来"
start_upstream "$V3" 192 "$OUT/mis-up3.log" || die "上游 3 没起来"
"$GATEWAY" --serve --listen "127.0.0.1:$GW2" --config "$OUT/misrate.json" --workers 1 \
  > "$OUT/mis-gw.log" 2>&1 & GW2PID=$!
PIDS+=("$GW2PID")
wait_for_line "$GW2PID" "$OUT/mis-gw.log" "监听" 15 || die "网关没起来"

# 轻业务负载：25 s 内每 250 ms 一条，全部应为 200。探活 20ms × 25s ≈ 1250 轮/台。
( end=$((SECONDS+25)); while (( SECONDS < end )); do
    curl -s --max-time 3 -o /dev/null "http://127.0.0.1:$GW2/traffic" 2>/dev/null
    sleep 0.25
  done ) &
LOAD_PID=$!
PIDS+=("$LOAD_PID")
sleep 25
kill -TERM "$GW2PID" 2>/dev/null; wait "$GW2PID" 2>/dev/null

P0=$(get_stat "health_t0_127.0.0.1_${V1}_probe_ok" "$OUT/mis-gw.log")
P1=$(get_stat "health_t1_127.0.0.1_${V2}_probe_ok" "$OUT/mis-gw.log")
P2=$(get_stat "health_t2_127.0.0.1_${V3}_probe_ok" "$OUT/mis-gw.log")
M0=$(get_stat "health_t0_127.0.0.1_${V1}_ejections" "$OUT/mis-gw.log")
M1=$(get_stat "health_t1_127.0.0.1_${V2}_ejections" "$OUT/mis-gw.log")
M2=$(get_stat "health_t2_127.0.0.1_${V3}_ejections" "$OUT/mis-gw.log")
ROUNDS=$(( ${P0:-0} < ${P1:-0} ? ${P0:-0} : ${P1:-0} ))
(( ${P2:-0} < ROUNDS )) && ROUNDS=${P2:-0}
MISSED=$(( ${M0:-0} + ${M1:-0} + ${M2:-0} ))
DENOM=$(( ROUNDS * 3 ))
if (( ROUNDS >= 1000 )); then
  ok "探活轮数下限达标：min=${ROUNDS} 轮（分母 = ${ROUNDS} × 3 = ${DENOM}）"
else
  fail "探活轮数只有 ${ROUNDS}，不到 1000：25s × 20ms 不该是这个数"
fi
if (( MISSED == 0 )); then
  echo "  误摘率 = 0 / ${DENOM} = 0.00%（三台全健康，零误摘）"
  ok "误摘率 0/${DENOM}"
else
  echo "  误摘率 = ${MISSED} / ${DENOM}"
  fail "健康后端被摘了 ${MISSED} 次：误摘率 ${MISSED}/${DENOM}"
fi

# ============================ 阶段 3：安全阀 ====================================
echo "== B-014 阶段 3：安全阀（10 台全死、上限 10% → 恰好停 1 台）=="
GW3=18380
declare -a WPORTS=()
for i in $(seq 0 9); do WPORTS+=($((18381 + i))); done
for p in $GW3 "${WPORTS[@]}"; do port_free "$p" || die "端口 $p 被占用：先清场"; done

{
  echo '{'
  echo '  "upstreams": [ { "name": "api", "balance": "round_robin", "targets": ['
  first=1
  for p in "${WPORTS[@]}"; do
    (( first )) || echo ','
    first=0
    echo "      { \"address\": \"127.0.0.1\", \"port\": $p }"
  done
  echo '  ] } ],'
  echo '  "routes": [ { "match": "prefix", "pattern": "", "upstream": "api" } ],'
  echo '  "health": { "max_fails": 1, "fail_timeout_ms": 60000,'
  echo '              "probe_interval_ms": 50, "probe_timeout_ms": 300,'
  echo '              "max_ejection_percent": 10 }'
  echo '}'
} > "$OUT/valve.json"

# 先起 10 台拿到"确定监听过"的端口，再全部杀掉：探活必然失败，且不与任何服务撞车。
WPIDS=()
for i in "${!WPORTS[@]}"; do
  "$UPSTREAM" --listen "127.0.0.1:${WPORTS[$i]}" --body-bytes 64 > "$OUT/valve-up${i}.log" 2>&1 &
  WPIDS+=($!)
  PIDS+=("${WPIDS[-1]}")
done
for i in "${!WPIDS[@]}"; do
  wait_for_line "${WPIDS[$i]}" "$OUT/valve-up${i}.log" "监听" 15 || die "阀用上游 $i 没起来"
done
for pid in "${WPIDS[@]}"; do kill -9 "$pid" 2>/dev/null; done
sleep 0.3

"$GATEWAY" --serve --listen "127.0.0.1:$GW3" --config "$OUT/valve.json" --workers 1 \
  > "$OUT/valve-gw.log" 2>&1 & GW3PID=$!
PIDS+=("$GW3PID")
wait_for_line "$GW3PID" "$OUT/valve-gw.log" "监听" 15 || die "网关没起来"
# floor(10 × 10%) = 1 台可摘；其余 9 台的摘除请求必须被挡且有账。探活 50ms，3s 足够多拍。
sleep 3
kill -TERM "$GW3PID" 2>/dev/null; wait "$GW3PID" 2>/dev/null

NOW=$(get_stat health_ejected_now "$OUT/valve-gw.log")
BLK=$(get_stat health_ejections_blocked_by_valve "$OUT/valve-gw.log")
TOTAL=0
for i in $(seq 0 9); do
  v=$(get_stat "health_t${i}_127.0.0.1_${WPORTS[$i]}_ejections" "$OUT/valve-gw.log")
  TOTAL=$(( TOTAL + ${v:-0} ))
done
if [[ "${NOW:-9}" == "1" ]]; then ok "被摘除停在 1/10（≤ 10% 上限）"; else fail "被摘除 ${NOW:-?} 台，期望恰好 1"; fi
if (( TOTAL == 1 )); then ok "摘除总次数恰好 1（下界：阀不是靠'从不摘除'过关的）"
else fail "摘除总次数 ${TOTAL}，期望 1"; fi
if (( ${BLK:-0} >= 1 )); then ok "被阀挡下的摘除有账：${BLK} 次"
else fail "blocked_by_valve = ${BLK:-?}，阀挡人却没记账"; fi

trap - EXIT
echo "DONE"
if (( FAILED )); then
  echo "✗ B-014 有失败格，看 $OUT/"
  exit 1
fi
echo "✓ B-014 三段全过（摘除耗时见上面阶段 1 的读数；误摘率 0/${DENOM}；阀 1/10）"
exit 0
