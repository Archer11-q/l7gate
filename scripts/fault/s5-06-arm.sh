#!/usr/bin/env bash
# S5-06 的一臂（由 s5-06-netem.sh 在 **一个全新的非特权 user+netns** 里调用）：
#   unshare -Un --map-root-user -- bash scripts/fault/s5-06-arm.sh '<netem 参数>' <业务条数> <落盘目录> <采样次数> <观察超时秒>
#
# 为什么注入在 netns 的回环上而不是宿主回环上（判据读数在 TASKS 的 S5-06 行与 ENVIRONMENT 5.4）：
# 宿主侧要 CAP_NET_ADMIN（uid=1000 加不上，sudo 要交互认证），而 `unshare -Un --map-root-user` 里
# uid=0、CapEff 全开，netem 加得上也生效——**作用域是这一个 ns 的 lo**，不碰同机任何真实流量。
#
# 这一臂量什么（都是能断言的数，不是"看起来通了"）：
#   一、注入确实发生过 —— qdisc 自己的 `Sent … pkt` / `dropped` 计数（它们在 `-s` 输出的**第二行**）；
#   二、摘除这一侧 —— `health_ejected_now` 峰值、逐目标 `health_t<i>_…_ejections` 之和、
#        `health_ejections_blocked_by_valve`、`health_all_denied_fallback`
#        （ADR-12 的安全阀这一格是应用层矩阵造不出来的形状）；
#   三、客户端这一侧 —— 每条请求的墙钟毫秒（min/p50/max）、状态码序列、被谁服务的序列；
#   四、退出账 —— live 归零、constructed==destructed。**fd 那一格按 ARCH 1.31 边界⑦ 走"分两档"**：
#        这一栈装着探活，fd_end 在析构之前采样，所以不相等是预期形状，由驱动按"有界 + 解释"读，
#        不当守恒类不变式判死。
# 摘除时刻按**注入之后**的墙钟算，与 §12 的"连续 20 条不见 128"不是同一个判据——
# 网络层注入不区分成员，那条按成员体长识别的判据在这里没有定义（驱动里写明这处差异）。
set -uo pipefail
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1

SPEC="${1:-}"                 # 空 = 不注入（对照臂）
REQUESTS="${2:-12}"
OUT="${3:-build/fault/s5-06/arm}"
# **观察者自己走的就是那条被注入的回环**：采样与业务 curl 的超时必须比注入的往返长，
# 否则 metric() 一律空读、`ejected_now_max` 停在 0——那是仪器失灵，不是被测方没摘除。
# 上一发拿固定 3 s 去读 delay 1500 ms 的观测面：三条判定空转、整臂跑了 5 分钟。
SAMPLES="${4:-20}"
MTIME="${5:-6}"
# 产物目录跟着驱动那一轮的 PRESET 走——写死 release 会让"我跑的 preset"与"实际被测的二进制"分家
# （口径⑪ 那一族：上一轮就撞过一次假 ✗）。
BIN="build/${PRESET:-release}"
[[ -x "$BIN/l7gate" ]] || { echo "FATAL no_binary ($BIN)"; exit 4; }
GW_PORT=18950
U_PORTS=(18951 18952 18953)
M_PORT=18955
mkdir -p "$OUT"

echo "arm_spec=${SPEC:-<clean>}  requests=$REQUESTS  samples=$SAMPLES  mtime=$MTIME  ns_uid=$(id -u)  CapEff=$(grep CapEff /proc/self/status | cut -c10-)"

ip link set dev lo up
if [[ -n "$SPEC" ]]; then
  # 新 netns 里 `ip link set lo up` 之后 **127.0.0.1/8 已经自带**（上一版我多写了一次 ip addr add，
  # 报 Error: ipv4: Address already assigned——那是我的假设与事实不符，不影响判定但值得记）。
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

# 三台上游各钉一段**同长不同前缀**的正文（`M1…`/`M2…`/`M3…`，长度一样），客户端读第一格就知道被谁服务了。
# 体长刻意相同——不同大小的响应会让 delay 那一档掺进"传输时间随成员不同"的无关变量。
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
# 累计摘除次数在运行期面上**没有单一键**：能读的只有 `health_ejected_now`（当下在册）、
# `health_ejections_blocked_by_valve`、`health_all_denied_fallback`，加上逐目标那一族
# `health_t<i>_<addr>_<port>_ejections`。所以"到底摘过几次"= 逐目标三格相加。
# 上一版我读了一个不存在的 `health_ejections`，拿到空串，于是每条判定都错位——
# **"键名不许改"这条纪律的另一面是：写判据之前必须先核对键名存在**。
# **并且要数"读到了几个键"**：退化 D3 实测过一次——把键名写坏之后，"该摘"那两臂红了，
# 而"不该摘"的 clean/d50 两臂**空判绿**（键名坏 = 读回空 = 求和 0，与"确实没摘"在数上不可区分）。
# 所以"上界（不该摘）"必须配一条"我读得动这个键"的下界，否则那条绿是空的。
t_ej_sum() { local s=0 r=0 v i
  for i in 0 1 2; do
    v=$(metric "health_t${i}_127_0_0_1_${U_PORTS[$i]}_ejections")
    [[ -n "$v" ]] && { s=$((s + v)); r=$((r + 1)); }
  done
  echo "$s $r"; }

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

valve=$(metric health_ejections_blocked_by_valve)
denied=$(metric health_all_denied_fallback)
ejmax=0; empties=0
for i in $(seq 1 "$SAMPLES"); do
  e=$(metric health_ejected_now)
  [[ -z "$e" ]] && empties=$((empties + 1))
  e=${e:-0}
  (( e > ejmax )) && ejmax=$e
  sleep 0.3
done
ej_res=$(t_ej_sum); ej_total=${ej_res% *}; ej_reads=${ej_res#* }
# `-s` 的统计在**第二行**（第一行只有 qdisc 定义），上一版 head -1 抓错行 → Sent 永远读空。
qdisc_stats=$(tc -s qdisc show dev lo 2>/dev/null | grep -o 'Sent.*' | head -1 | tr -s ' ')
cf=$(metric upstream_connect_failed); replies_cf=$(metric gateway_replies_connect_failed)
replies_ae=$(metric gateway_replies_all_ejected); done_c=$(metric responses_completed)

kill -TERM "$GW" 2>/dev/null; wait "$GW" 2>/dev/null
for i in $(seq 1 20); do grep -q '\[serve\]' "$OUT/gateway.log" && break; sleep 0.25; done
srv() { sed -n "s/^\[serve\] $1=\([0-9-]*\).*/\1/p" "$OUT/gateway.log" | head -1; }
live=$(srv live_connections); con=$(srv connections_constructed); des=$(srv connections_destructed)
fd0=$(srv fd_start); fd1=$(srv fd_end)
srv_valve=$(srv health_ejections_blocked_by_valve)

kill -TERM "${pids[@]}" 2>/dev/null; wait 2>/dev/null

# **一行一键**：上一版把好几个键挤在同一行，而取数器是 `sed s/^键=//p`——于是每个键读到的都是
# "我之后那一整行"，判定全线错位。§21.2 第 4 条那一族（退出账把 11 个键打在一行里）本轮我自己
# 在**写产出**这一侧撞了一次。
{
  echo "arm=${SPEC:-clean}"
  echo "requests=$REQUESTS"
  echo "samples=$SAMPLES"
  echo "mtime=$MTIME"
  echo "qdisc_first=$(tc qdisc show dev lo 2>/dev/null | head -1)"
  echo "qdisc_stats=$qdisc_stats"
  echo "http_codes=$http_codes"
  echo "served_sequence=$served"
  echo "lat_min_ms=$min_ms"
  echo "lat_p50_ms=$p50_ms"
  echo "lat_max_ms=$max_ms"
  echo "metric_ejected_now_max=$ejmax"
  echo "metric_ejections_sum=$ej_total"
  echo "metric_ejection_key_reads=$ej_reads"
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
  echo "inject_to_last_req_ms=$(( $(date +%s%N) / 1000000 - T0 ))"
} > "$OUT/summary.txt"
cat "$OUT/summary.txt"
