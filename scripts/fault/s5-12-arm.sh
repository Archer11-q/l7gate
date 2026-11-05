#!/usr/bin/env bash
# S5-12 的一臂（由 scripts/fault/s5-12-threshold.sh 在**一个全新的非特权 user+netns** 里调用）：
#   unshare -Un --map-root-user -- bash scripts/fault/s5-12-arm.sh '<netem 参数>' <业务条数> <落盘目录> <观测窗毫秒> <curl 超时秒>
#
# 与 s5-06-arm.sh 的分工（为什么是两份而不是一份加旋钮）：
#   s5-06-arm.sh 是 S5-06 六臂对照的夹具，已经被 FAULT-INJECTION §25 的判定和 §26 的两条退化实验钉住；
#   把它拖进本轮就等于让一条已关门那轮的判定面跟着改。这一份只做两件 S5-06 没做的事：
#     ① 读**逐目标 recoveries / half_open 那一族**——§25.5 欠的那格（l20 的 ej=18 里有几次是恢复后再摘）；
#     ② 把注入之后的观测段钉成**固定墙上时间窗**——跨臂比 ej 必须先比暴露时长，否则阈值读的是"谁跑得久"。
#   被测配置与 S5-06 逐字同参（3 成员 round_robin + catch-all、max_fails=2、fail_timeout=60s、
#   probe_interval=100ms、probe_timeout=300ms、阀 50%）：换了参数就不是同一条分界线。
#
# 键名先核过再引用（§25.2 那条纪律：读回空串与「确实没发生」在数上不可区分）：
#   逐目标那一族的后缀表在 src/metrics/metrics_render.cc:136-148 ——
#   ejections / probe_ok / probe_fail / **recoveries** / half_open_admitted / half_open_denied /
#   half_open_recovered / half_open_reopened；地址里的点号由 sanitize_metric_name()（同文件 :152-160）
#   换成下划线，所以键形如 health_t0_127_0_0_1_18961_recoveries。
set -uo pipefail
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1

SPEC="${1:-}"
REQUESTS="${2:-12}"
OUT="${3:-build/fault/s5-12/arm}"
OBS_MS="${4:-8000}"
MTIME="${5:-4}"
BIN="build/${PRESET:-release}"
[[ -x "$BIN/l7gate" ]] || { echo "FATAL no_binary ($BIN)"; exit 4; }
[[ -x "$BIN/tests/l7gate_test_upstream" ]] || { echo "FATAL no_upstream_binary ($BIN)"; exit 4; }

# 端口带与 S5-06 的 1895x 错开（臂内监听其实活在各臂自己的 netns 里，
# 但错开能让"两条驱动同时跑"与"宿主残留"这两种情况一眼可辨）。
GW_PORT=18960
U_PORTS=(18961 18962 18963)
M_PORT=18965
mkdir -p "$OUT"

echo "arm_spec=${SPEC:-<clean>}  requests=$REQUESTS  obs_ms=$OBS_MS  mtime=$MTIME  ns_uid=$(id -u)  CapEff=$(grep CapEff /proc/self/status | cut -c10-)"

ip link set dev lo up
T_INJECT=$(( $(date +%s%N) / 1000000 ))
if [[ -n "$SPEC" ]]; then
  tc qdisc add dev lo root netem $SPEC || { echo "FATAL qdisc_add_failed"; exit 3; }
fi
tc qdisc show dev lo | head -1 | sed 's/^/qdisc: /'

cat > "$OUT/gw.json" <<JSON
{
  "upstreams": [
    { "name": "api", "balance": "round_robin", "targets": [
        { "address": "127.0.0.1", "port": ${U_PORTS[0]}, "health_path": "/probe" },
        { "address": "127.0.0.1", "port": ${U_PORTS[1]}, "health_path": "/probe" },
        { "address": "127.0.0.1", "port": ${U_PORTS[2]}, "health_path": "/probe" } ] }
  ],
  "routes": [ { "match": "prefix", "pattern": "", "upstream": "api" } ],
  "health": { "max_fails": 2, "fail_timeout_ms": 60000, "probe_interval_ms": 100,
              "probe_timeout_ms": 300, "max_ejection_percent": 50 }
}
JSON

pids=()
for i in 0 1 2; do
  mark=$(printf 'M%d--------------------------------------------------------' "$((i+1))")
  "$BIN/tests/l7gate_test_upstream" --listen "127.0.0.1:${U_PORTS[$i]}" \
      --fixed-body "$mark" > "$OUT/up$((i+1)).log" 2>&1 &
  pids+=("$!")
done
sleep 0.5

"$BIN/l7gate" --serve --listen "127.0.0.1:$GW_PORT" --config "$OUT/gw.json" \
    --metrics-port "$M_PORT" > "$OUT/gateway.log" 2>&1 &
GW=$!

metric() { curl -s --max-time "$MTIME" "http://127.0.0.1:$M_PORT/metrics" 2>/dev/null |
           awk -v k="$1" '$1==k {print $2; exit}'; }
t_key() { echo "health_t${1}_127_0_0_1_${U_PORTS[$1]}_${2}"; }

for i in $(seq 1 40); do
  curl -s --max-time "$MTIME" -o /dev/null "http://127.0.0.1:$GW_PORT/warm" && break
  sleep 0.25
done

T0=$(( $(date +%s%N) / 1000000 ))
served=""; lat=""; http_codes=""
for i in $(seq 1 "$REQUESTS"); do
  a=$(( $(date +%s%N) / 1000000 ))
  code=$(curl -s --max-time "$MTIME" -o "$OUT/lastbody" -w '%{http_code}' "http://127.0.0.1:$GW_PORT/r$i")
  b=$(( $(date +%s%N) / 1000000 ))
  http_codes="$http_codes$code"
  m=$(sed -n 's/^M\([123]\).*/\1/p' "$OUT/lastbody" 2>/dev/null | head -1)
  served="$served${m:-?}"
  lat="$lat $((b-a))"
done
min_ms=$(printf '%s\n' $lat | sort -n | head -1)
p50_ms=$(printf '%s\n' $lat | sort -n | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}')
max_ms=$(printf '%s\n' $lat | sort -n | tail -1)

# --- 固定墙上时间窗的观测段：每 0.3 s 抄一次 health_ejected_now，直到累计 OBS_MS ---
# 逐目标 ej 是在**整条臂的寿命**上累计的，所以跨臂比大小必须先比观测窗；
# 这里按墙上时间而不是按采样次数收口，采样次数就退化成读到的样本数（诊断用，不进判定）。
OBS_T0=$(( $(date +%s%N) / 1000000 ))
series=""; ejmax=0; empties=0; obs_samples=0
while :; do
  now=$(( $(date +%s%N) / 1000000 ))
  (( now - OBS_T0 >= OBS_MS )) && break
  e=$(metric health_ejected_now)
  [[ -z "$e" ]] && empties=$((empties + 1))
  e=${e:-0}
  series="$series$e"
  (( e > ejmax )) && ejmax=$e
  obs_samples=$((obs_samples + 1))
  sleep 0.3
done
obs_ms_actual=$(( $(date +%s%N) / 1000000 - OBS_T0 ))

# --- 终局读表：**一次抓取、一份文本、六个键都读齐才算数** ---
# 上一版是每个键发一条 curl：丢包臂里观测面自己走的就是那条被注入的回环（§25 记过一次），
# 于是一个键读空就把 ej 求和算小（l25 实测 `target_ejections=9//16` → ej=25 而 rec=38），
# 而且 ej 与 rec 取自**不同次抓取**，中间正好越过一次"摘↔恢复"就会出现 rec>ej（l15 实测 19/20）。
# 这两条都是仪器的错，不是被测方的形状，所以改成单次抓取 + 抓不齐重试。
EXP="$OUT/final-exposition.txt"
scrape_ok=0; scrape_tries=0
for attempt in $(seq 1 6); do
  scrape_tries=$attempt
  curl -s --max-time "$MTIME" "http://127.0.0.1:$M_PORT/metrics" > "$EXP" 2>/dev/null
  got=0
  for i in 0 1 2; do
    for suf in ejections recoveries; do
      grep -q "^$(t_key $i ${suf}) " "$EXP" 2>/dev/null && got=$((got + 1))
    done
  done
  # valve 与 denied 也必须在同一份抓取里
  grep -q '^health_ejections_blocked_by_valve ' "$EXP" && got=$((got + 1))
  grep -q '^health_all_denied_fallback ' "$EXP" && got=$((got + 1))
  (( got == 8 )) && { scrape_ok=1; break; }
  sleep 0.3
done
k() { awk -v k="$1" '$1==k {print $2; exit}' "$EXP" 2>/dev/null; }

valve=$(k health_ejections_blocked_by_valve)
denied=$(k health_all_denied_fallback)
# `-s` 的统计在第二行（第一行只有 qdisc 定义）——§25.2 那一格。
qdisc_stats=$(tc -s qdisc show dev lo 2>/dev/null | grep -o 'Sent.*' | head -1 | tr -s ' ')
cf=$(k upstream_connect_failed); replies_cf=$(k gateway_replies_connect_failed)
replies_ae=$(k gateway_replies_all_ejected); done_c=$(k responses_completed)

# --- 逐目标那一族：全部来自上面那**同一份**抓取 ---
declare -a TEJ TREC TPOK TPFAIL THA THRE
ej_reads=0; rec_reads=0; ej_total=0; rec_total=0; n_ejected=0
for i in 0 1 2; do
  TEJ[i]=$(k "$(t_key $i ejections)");   TREC[i]=$(k "$(t_key $i recoveries)")
  TPOK[i]=$(k "$(t_key $i probe_ok)");   TPFAIL[i]=$(k "$(t_key $i probe_fail)")
  THA[i]=$(k "$(t_key $i half_open_admitted)"); THRE[i]=$(k "$(t_key $i half_open_recovered)")
  [[ -n "${TEJ[i]}" ]] && { ej_total=$((ej_total + TEJ[i])); ej_reads=$((ej_reads + 1)); (( TEJ[i] > 0 )) && n_ejected=$((n_ejected + 1)); }
  [[ -n "${TREC[i]}" ]] && { rec_total=$((rec_total + TREC[i])); rec_reads=$((rec_reads + 1)); }
done

kill -TERM "$GW" 2>/dev/null; wait "$GW" 2>/dev/null
for i in $(seq 1 20); do grep -q '\[serve\]' "$OUT/gateway.log" && break; sleep 0.25; done
srv() { sed -n "s/^\[serve\] $1=\([0-9-]*\).*/\1/p" "$OUT/gateway.log" | head -1; }
live=$(srv live_connections); con=$(srv connections_constructed); des=$(srv connections_destructed)
fd0=$(srv fd_start); fd1=$(srv fd_end)
srv_valve=$(srv health_ejections_blocked_by_valve)

kill -TERM "${pids[@]}" 2>/dev/null; wait 2>/dev/null
T_END=$(( $(date +%s%N) / 1000000 ))

# **一行一键**（§25.2 那一族：把几个键挤在同一行，取数器就把"我之后那一整行"全读回来）。
{
  echo "arm=${SPEC:-clean}"
  echo "requests=$REQUESTS"
  echo "obs_ms_target=$OBS_MS"
  echo "obs_ms_actual=$obs_ms_actual"
  echo "obs_samples=$obs_samples"
  echo "arm_lifetime_ms=$((T_END - T_INJECT))"
  echo "qdisc_first=$(tc qdisc show dev lo 2>/dev/null | head -1)"
  echo "qdisc_stats=$qdisc_stats"
  echo "http_codes=$http_codes"
  echo "served_sequence=$served"
  echo "lat_min_ms=$min_ms"
  echo "lat_p50_ms=$p50_ms"
  echo "lat_max_ms=$max_ms"
  echo "metric_ejected_now_max=$ejmax"
  echo "ejected_now_series=$series"
  echo "metric_ejections_sum=$ej_total"
  echo "metric_ejection_key_reads=$ej_reads"
  echo "metric_recoveries_sum=$rec_total"
  echo "metric_recovery_key_reads=$rec_reads"
  echo "final_scrape_complete=$scrape_ok"
  echo "final_scrape_tries=$scrape_tries"
  echo "metric_targets_with_ejections=$n_ejected"
  echo "target_ejections=${TEJ[0]}/${TEJ[1]}/${TEJ[2]}"
  echo "target_recoveries=${TREC[0]}/${TREC[1]}/${TREC[2]}"
  echo "target_probe_ok=${TPOK[0]}/${TPOK[1]}/${TPOK[2]}"
  echo "target_probe_fail=${TPFAIL[0]}/${TPFAIL[1]}/${TPFAIL[2]}"
  echo "target_half_open_admitted=${THA[0]}/${THA[1]}/${THA[2]}"
  echo "target_half_open_recovered=${THRE[0]}/${THRE[1]}/${THRE[2]}"
  echo "metric_valve=$valve"
  echo "metric_denied=$denied"
  echo "exit_valve=$srv_valve"
  echo "upstream_connect_failed=$cf"
  echo "gateway_replies_connect_failed=$replies_cf"
  echo "gateway_replies_all_ejected=$replies_ae"
  echo "responses_completed=$done_c"
  echo "live=$live"
  echo "constructed=$con"
  echo "destructed=$des"
  echo "fd_start=$fd0"
  echo "fd_end=$fd1"
  echo "observer_empty_reads=$empties"
} > "$OUT/summary.txt"
cat "$OUT/summary.txt"
