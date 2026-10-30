#!/usr/bin/env bash
# S3-05 故障注入矩阵第一轮（ADR-13：全部应用层注入；tc netem 交叉验证仍是未验证项，不在本轮）。
#
# 五类故障 × 每类的摘除耗时 / 恢复耗时，真进程（release 网关 + 三台测试上游 + curl）：
#   1 connect refused  —— kill 上游进程（探活 connect 失败）
#   2 后端 5xx         —— 上游对所有路径回 503（探活死 + 业务被动失败同源）
#   3 半开             —— accept 照旧、一个字节不读不回（探活超时判死），恢复 = stall 到期
#   4 慢响应           —— 两档：慢过探活超时（600ms > 300ms → 摘）与慢在超时内（100ms < 300ms → 不摘）
#   5 探活失败业务正常 —— 只有探活路径（/probe）回 503，业务路径 200：探活是成员资格的唯一真源
#
# 摘除耗时 = 故障注入（T0）→ 该成员连续 20 条请求不出现（探活失败 + 重路由生效）；
# 恢复耗时 = 故障清除（T0）→ 该成员的响应体长首次再现。预期 vs 实测的对账在 FAULT-INJECTION §12。
#
# PID 管理的一条硬规矩（第一轮踩的坑）：受害成员 2 的 PID 走专用变量 UP2_PID，
# **不许从 PIDS 数组里"按行号"取**——here-string 会把数组拼成一行，sed 按行取到的是
# 空或整行，kill 落空、症状是"成员杀不死 + 重启撞 EADDRINUSE"。
#
# 用法：PRESET=release scripts/fault/s3-05-matrix.sh
# 输出：build/fault/s3-05/ 下的日志 + 本脚本 stdout 的读数；退出码 0 = 全部断言过。
set -uo pipefail
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1

trap '' PIPE
PRESET="${PRESET:-release}"
BIN="build/$PRESET"
GATEWAY="$BIN/l7gate"
UPSTREAM="$BIN/tests/l7gate_test_upstream"
OUT="build/fault/s3-05"
mkdir -p "$OUT"

FAILED=0
fail() { echo "  ✗ $*"; FAILED=1; }
ok()   { echo "  ✓ $*"; }
die()  { echo "  fatal: $*" >&2; exit 1; }

[[ -x "$GATEWAY" ]] || die "找不到 $GATEWAY，先 cmake --build --preset $PRESET"
[[ -x "$UPSTREAM" ]] || die "找不到 $UPSTREAM，先 cmake --build --preset $PRESET"
command -v curl >/dev/null || die "本机没有 curl"

GW_PORT=18580
U1=18581; U2=18582; U3=18583

port_free() { ! (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; }
for p in "$GW_PORT" "$U1" "$U2" "$U3"; do
  port_free "$p" || die "端口 $p 已被占用：先清场"
done

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
GW_PID=""
UP2_PID=""
start_u2() { # log [extra flags...]
  # kill -9 之后立刻重绑同一个端口可能撞 EADDRINUSE：被杀进程的 socket 由内核代收，
  # FIN_WAIT 系状态（非 TIME_WAIT）连 SO_REUSEADDR 都绕不开，要等对端（网关）把
  # EOF 事件处理完、FIN 交互完——毫秒到百毫秒级。所以这里退避重试，不当成故障。
  local log=$1; shift
  local try=0
  while (( try < 50 )); do
    "$UPSTREAM" --listen "127.0.0.1:$U2" --body-bytes 128 "$@" > "$log" 2>&1 &
    UP2_PID=$!
    if wait_for_line "$UP2_PID" "$log" "监听" 2; then
      PIDS+=("$UP2_PID")
      return 0
    fi
    kill -9 "$UP2_PID" 2>/dev/null
    wait "$UP2_PID" 2>/dev/null
    echo "try $try bind failed; ss snapshot:" >> "$OUT/bind-fail.log"
    ss -tlnp | grep "$U2" >> "$OUT/bind-fail.log" 2>&1
    ((try++))
    sleep 0.1
  done
  return 1
}
stop_u2() {
  kill -9 "$UP2_PID" 2>/dev/null
  wait "$UP2_PID" 2>/dev/null
  local rest=() p
  for p in "${PIDS[@]}"; do [[ "$p" == "$UP2_PID" ]] || rest+=("$p"); done
  PIDS=("${rest[@]}")
  UP2_PID=""
}
cleanup() {
  [[ -n "$GW_PID" ]] && kill -9 "$GW_PID" 2>/dev/null
  [[ -n "$UP2_PID" ]] && kill -9 "$UP2_PID" 2>/dev/null
  for pid in "${PIDS[@]}"; do kill -9 "$pid" 2>/dev/null; done
  wait 2>/dev/null
}
trap cleanup EXIT

cat > "$OUT/matrix.json" <<'JSON'
{
  "upstreams": [
    { "name": "api", "balance": "round_robin",
      "targets": [ { "address": "127.0.0.1", "port": 18581 },
                    { "address": "127.0.0.1", "port": 18582, "health_path": "/probe" },
                    { "address": "127.0.0.1", "port": 18583 } ] }
  ],
  "routes": [ { "match": "prefix", "pattern": "", "upstream": "api" } ],
  "health": { "max_fails": 2, "fail_timeout_ms": 60000,
              "probe_interval_ms": 100, "probe_timeout_ms": 300,
              "max_ejection_percent": 50 }
}
JSON

"$UPSTREAM" --listen "127.0.0.1:$U1" --body-bytes 64 > "$OUT/up1.log" 2>&1 &
PIDS+=($!)
wait_for_line "${PIDS[-1]}" "$OUT/up1.log" "监听" 15 || die "上游 1 没起来"
start_u2 "$OUT/up2.log" || die "上游 2 没起来"
"$UPSTREAM" --listen "127.0.0.1:$U3" --body-bytes 192 > "$OUT/up3.log" 2>&1 &
PIDS+=($!)
wait_for_line "${PIDS[-1]}" "$OUT/up3.log" "监听" 15 || die "上游 3 没起来"
"$GATEWAY" --serve --listen "127.0.0.1:$GW_PORT" --config "$OUT/matrix.json" --workers 1 \
  > "$OUT/gateway.log" 2>&1 & GW=$!
GW_PID=$GW
wait_for_line "$GW" "$OUT/gateway.log" "health=on" 15 || die "网关没打出 health=on"
wait_for_line "$GW" "$OUT/gateway.log" "监听" 15 || die "网关没起来"

# 成员 2 的响应体长（128）是"它在接流量"的标志；摘除判定 = 连续 20 条不见 128。
all_healthy() { # 30 条里三台都出现
  local seen64=0 seen128=0 seen192=0 i c n
  for i in $(seq 1 30); do
    read -r c n <<< "$(req "$GW_PORT" /settle)"
    [[ "$c" == "200" ]] || return 1
    case "$n" in 64) seen64=1;; 128) seen128=1;; 192) seen192=1;; esac
  done
  (( seen64 && seen128 && seen192 ))
}

measure_eject() { # 依赖调用方已设 EJECT_T0；每条响应追加进 trace 文件
  local streak=0 t_end i c n
  for i in $(seq 1 400); do
    read -r c n <<< "$(req "$GW_PORT" /probe-fault)"
    echo "$i $c $n" >> "$OUT/trace-eject-${TRACE_TAG:-x}.log"
    if [[ "$n" == "128" ]]; then
      streak=0
    else
      streak=$((streak + 1))
      if (( streak >= 20 )); then t_end=$(epoch_ms); echo $(( t_end - EJECT_T0 )); return 0; fi
    fi
  done
  echo -1
}

measure_recover() { # 依赖调用方已设 RECOVER_T0
  local t i c n
  for i in $(seq 1 400); do
    read -r c n <<< "$(req "$GW_PORT" /recover)"
    echo "$i $c $n" >> "$OUT/trace-recover-${TRACE_TAG:-x}.log"
    if [[ "$n" == "128" ]]; then t=$(epoch_ms); echo $(( t - RECOVER_T0 )); return 0; fi
  done
  echo -1
}

echo "== 类 1：connect refused（kill 上游进程）=="
TRACE_TAG=c1
all_healthy || die "类 1 前置不满足：三台没有全健康"
EJECT_T0=$(epoch_ms)
stop_u2
measured_eject_ms=$(measure_eject)
if (( measured_eject_ms > 0 && measured_eject_ms < 10000 )); then
  echo "  摘除耗时 = ${measured_eject_ms} ms（预期 ≈ max_fails×探活间隔 ≈ 200–600ms；含 pre-ejection 请求的 5s 排空）"
  ok "类 1 摘除"
else
  fail "类 1 摘除耗时 = ${measured_eject_ms} ms"
fi
RECOVER_T0=$(epoch_ms)
start_u2 "$OUT/up2.log" || die "上游 2 重启失败"
measured_recover_ms=$(measure_recover)
if (( measured_recover_ms > 0 && measured_recover_ms < 5000 )); then
  echo "  恢复耗时 = ${measured_recover_ms} ms（预期 ≤ 探活间隔 + 调度 ≈ 100–500ms）"
  ok "类 1 恢复"
else
  fail "类 1 恢复耗时 = ${measured_recover_ms} ms"
fi

echo "== 类 2：后端 5xx（所有路径回 503）=="
TRACE_TAG=c2
all_healthy || die "类 2 前置不满足"
EJECT_T0=$(epoch_ms)
stop_u2
start_u2 "$OUT/up2-5xx.log" --path-status "/:503" || die "5xx 上游没起来"
measured_eject_ms=$(measure_eject)
if (( measured_eject_ms > 0 && measured_eject_ms < 5000 )); then
  echo "  摘除耗时 = ${measured_eject_ms} ms（探活 503 + 业务被动 503 同源记账）"
  ok "类 2 摘除"
else
  fail "类 2 摘除耗时 = ${measured_eject_ms} ms"
fi
stop_u2
RECOVER_T0=$(epoch_ms)
start_u2 "$OUT/up2.log" || die "上游 2 重启失败"
measured_recover_ms=$(measure_recover)
if (( measured_recover_ms > 0 && measured_recover_ms < 5000 )); then
  echo "  恢复耗时 = ${measured_recover_ms} ms"
  ok "类 2 恢复"
else
  fail "类 2 恢复耗时 = ${measured_recover_ms} ms"
fi

echo "== 类 3：半开（accept 照旧、不读不回，探活超时判死）=="
TRACE_TAG=c3
all_healthy || die "类 3 前置不满足"
EJECT_T0=$(epoch_ms)
stop_u2
start_u2 "$OUT/up2-stall.log" --stall-reads-ms 15000 || die "半开上游没起来"
measured_eject_ms=$(measure_eject)
if (( measured_eject_ms > 0 && measured_eject_ms < 10000 )); then
  echo "  摘除耗时 = ${measured_eject_ms} ms（每次探活吃满 300ms 超时：≈2×(300+100)=800ms 起步）"
  ok "类 3 摘除"
else
  fail "类 3 摘除耗时 = ${measured_eject_ms} ms"
fi
# 恢复 = stall 到期（15s 后探活自然成功），不重启：恢复耗时从名义到期时刻起量。
STALL_DEADLINE=$(( $(epoch_ms) + 15000 ))
while (( $(epoch_ms) < STALL_DEADLINE )); do sleep 0.1; done
RECOVER_T0=$STALL_DEADLINE
measured_recover_ms=$(measure_recover)
if (( measured_recover_ms > 0 && measured_recover_ms < 5000 )); then
  echo "  恢复耗时 = ${measured_recover_ms} ms（自 stall 名义到期起量）"
  ok "类 3 恢复"
else
  fail "类 3 恢复耗时 = ${measured_recover_ms} ms"
fi
stop_u2
start_u2 "$OUT/up2.log" || die "上游 2 重置失败"

echo "== 类 4a：慢响应、慢过探活超时（600ms > 300ms → 摘）=="
TRACE_TAG=c4a
all_healthy || die "类 4a 前置不满足"
EJECT_T0=$(epoch_ms)
stop_u2
start_u2 "$OUT/up2-slow600.log" --delay-ms 600 || die "慢上游没起来"
measured_eject_ms=$(measure_eject)
if (( measured_eject_ms > 0 && measured_eject_ms < 10000 )); then
  echo "  摘除耗时 = ${measured_eject_ms} ms（探活超时判死；业务其实能通——timeout 判据的固有取舍）"
  ok "类 4a 摘除"
else
  fail "类 4a 摘除耗时 = ${measured_eject_ms} ms"
fi
stop_u2

echo "== 类 4b：慢响应、慢在探活超时内（100ms < 300ms → 不摘）=="
TRACE_TAG=c4b
start_u2 "$OUT/up2-slow100.log" --delay-ms 100 || die "慢上游没起来"
saw128=0
for i in $(seq 1 60); do
  read -r c n <<< "$(req "$GW_PORT" /slow-but-alive)"
  [[ "$n" == "128" ]] && saw128=1
  sleep 0.05
done
if (( saw128 )); then ok "类 4b 慢在超时内：3 秒内 128 仍在轮换里（预期不摘）"
else fail "类 4b 慢在超时内却被摘：分界线画错了"; fi
stop_u2
start_u2 "$OUT/up2.log" || die "上游 2 重置失败"

echo "== 类 5：探活失败业务正常（/probe 回 503，业务路径 200）=="
TRACE_TAG=c5
all_healthy || die "类 5 前置不满足"
EJECT_T0=$(epoch_ms)
stop_u2
start_u2 "$OUT/up2-probe503.log" --path-status "/probe:503" || die "探活 503 上游没起来"
measured_eject_ms=$(measure_eject)
if (( measured_eject_ms > 0 && measured_eject_ms < 5000 )); then
  echo "  摘除耗时 = ${measured_eject_ms} ms（业务 200 也留不住它：探活是成员资格的唯一真源）"
  ok "类 5 摘除"
else
  fail "类 5 摘除耗时 = ${measured_eject_ms} ms"
fi
# 摘除后直接 curl 成员 2 的业务路径：它自己还是好的——这一格就是"探活判死 ≠ 机器死了"。
direct=$(curl -s --max-time 3 -o /dev/null -w '%{http_code}' "http://127.0.0.1:$U2/biz")
if [[ "$direct" == "200" ]]; then ok "被摘成员直连业务路径仍 200（判死的是探活，不是机器）"
else fail "被摘成员直连业务路径返回 $direct"; fi
stop_u2
RECOVER_T0=$(epoch_ms)
start_u2 "$OUT/up2.log" || die "上游 2 重启失败"
measured_recover_ms=$(measure_recover)
if (( measured_recover_ms > 0 && measured_recover_ms < 5000 )); then
  echo "  恢复耗时 = ${measured_recover_ms} ms（探活路径修好后第一拍回岗）"
  ok "类 5 恢复"
else
  fail "类 5 恢复耗时 = ${measured_recover_ms} ms"
fi

echo "== 收尾：网关自报的账 =="
kill -TERM "$GW" 2>/dev/null; wait "$GW" 2>/dev/null; GW_PID=""
EJ2=$(get_stat "health_t1_127.0.0.1_${U2}_ejections" "$OUT/gateway.log")
PF2=$(get_stat "health_t1_127.0.0.1_${U2}_probe_fail" "$OUT/gateway.log")
LIVE=$(get_stat live_connections "$OUT/gateway.log")
if [[ "${EJ2:-0}" -ge 4 ]]; then ok "成员 2 被摘总数 = ${EJ2}（类 1/2/3/4a/5 五次注入）"
else fail "成员 2 被摘总数 = ${EJ2:-?}，期望 ≥ 4"; fi
if [[ "${PF2:-0}" -ge 4 ]]; then ok "成员 2 探活失败总数 = ${PF2}"
else fail "成员 2 探活失败总数 = ${PF2:-?}"; fi
if [[ "${LIVE:-1}" == "0" ]]; then ok "live_connections = 0"; else fail "live = ${LIVE:-?}"; fi

trap - EXIT
echo "DONE"
if (( FAILED )); then
  echo "✗ S3-05 矩阵有失败格，看 $OUT/"
  exit 1
fi
echo "✓ S3-05 五类故障矩阵全过（读数见上面各行，预期 vs 实测对账进 FAULT-INJECTION §12）"
exit 0
