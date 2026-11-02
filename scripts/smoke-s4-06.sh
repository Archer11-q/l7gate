#!/usr/bin/env bash
# S4-06 冒烟：connect 失败的重试在**真进程**上的放大账（ADR-23）。
#
# ctest 那 13 条证的是机制与归因；这里证的是验收句里那个**实测比**：
#   客户端请求数 : 网关发起的上游尝试数 ≤ 1 + 预算(=1)，且 后端实收请求数 ≤ 客户端请求数。
# 两条读数来自三处互相背书，不是网关自己数自己：
#   ① 客户端侧 curl 的状态行条数与状态码；
#   ② 网关退出账 `upstream_retries` / `gateway_replies_*` / `upstream_connect_failed`；
#   ③ **上游进程自报**的"我收到几条请求"（放大这件事的主语是后端执行了几次，
#      只有它自己数的才算——仪器与被测方各数一遍，S4-05 那条等式的形状）。
#
# 四臂：
#   1 一台死一台活 —— 下界（重试真的发生过、且落到了另一台）+ 上界（后端实收 == 客户端请求数，
#     一条都没被复制）+ 这一臂的价值主张：客户端**一条 502 都看不到**。
#   2 两台全死 —— 预算顶格：`retries == 请求数`（不是 2 倍：级联会在那里露出来），
#     而一次表态仍然只有一条（`after_retry == replies_sent == 请求数`）。
#   3 两台都活 —— 零误重：`retries == 0`（"看见失败就换一台"的实现过不了这一臂）。
#   4 半开 + 一台死 —— 被钉进半开的那几条探测请求**不许换台**：它们的表态条数 == 门放行的
#     条数（换一台救活了就会是 200、那一格就空了），且 `half_open_recovered == 0`。
#
# 用法：PRESET=debug scripts/smoke-s4-06.sh      输出：build/smoke-s4-06/，退出码 0 = 全过
# 不需要 Docker。重试没有配置项（ADR-23 第七节），所以这四臂里没有 `retry` 段可写——
# 装配条件就是"这个组有 ≥2 个成员"这一件事本身。
set -uo pipefail
cd "$(dirname "$(readlink -f "$0")")/.." || exit 1

trap '' PIPE
PRESET="${PRESET:-release}"
BIN="build/$PRESET"
GATEWAY="$BIN/l7gate"
UPSTREAM="$BIN/tests/l7gate_test_upstream"
OUT="build/smoke-s4-06"
rm -rf "$OUT"; mkdir -p "$OUT"
PIDFILE="$OUT/pids"
: > "$PIDFILE"

FAILED=0
fail() { echo "  ✗ $*"; FAILED=1; }
ok()   { echo "  ✓ $*"; }
die()  { echo "  fatal: $*" >&2; exit 1; }

[[ -x "$GATEWAY" ]] || die "找不到 $GATEWAY，先 cmake --build --preset $PRESET"
[[ -x "$UPSTREAM" ]] || die "找不到 $UPSTREAM，先 cmake --build --preset $PRESET"
command -v curl >/dev/null || die "本机没有 curl"

GW_PORT=18600
ALIVE_A=18601
ALIVE_B=18602
DEAD_A=18603   # 刻意不绑：connect 必然被拒（ECONNREFUSED）
DEAD_B=18604

port_busy() { (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; }
# 要活的端口必须**空**（上一臂没退干净就会顶着旧进程跑，S4-04 那条假绿的形状）；
# 要死的端口也必须**空**——它"没人监听"这件事就是这一臂的注入本身。
for p in "$GW_PORT" "$ALIVE_A" "$ALIVE_B" "$DEAD_A" "$DEAD_B"; do
  port_busy "$p" && die "端口 $p 已被占用：先清场（这台注入的成立条件就是它没人监听）"
done

now_ms() { date +%s%N | cut -c1-13; }   # 本机 `date +%s%3N` 不认 %3N（口径⑮那条）

wait_for_line() { # pid logfile pattern [seconds]
  local pid=$1 log=$2 pat=$3 secs=${4:-20} i=0
  while (( i < secs * 20 )); do
    if grep -q -- "$pat" "$log" 2>/dev/null; then return 0; fi
    if ! kill -0 "$pid" 2>/dev/null; then
      echo "  进程 $pid 已退出，日志尾部：" >&2; tail -5 "$log" >&2; return 1
    fi
    ((i++)); sleep 0.05
  done
  return 1
}
# 不假设键在行首（口径⑭(b)）
get_stat() { # key logfile → 值
  local key=$1 log=$2
  grep -o "[[:space:]]$1=[0-9-]*" "$log" 2>/dev/null | head -1 | sed 's/.*=//'
}
upstream_received() { # logfile → 这台自报收到几条（两条分支相加，S4-05 同一条理由）
  local log=$1 a b
  a=$(sed -n 's/^\[upstream\] 已服务请求数=\([0-9]*\).*/\1/p' "$log" | head -1)
  b=$(sed -n 's/^\[upstream\] 固定状态码响应数=\([0-9]*\).*/\1/p' "$log" | head -1)
  echo $(( ${a:-0} + ${b:-0} ))
}

note_pid() { echo "$1" >> "$PIDFILE"; }
start_upstream() { # 标签 端口 → pid
  local tag=$1 port=$2
  port_busy "$port" && die "端口 $port 上还压着进程（上一臂没退干净）——不带着它往下跑"
  "$UPSTREAM" --listen "127.0.0.1:$port" --body-bytes 64 > "$OUT/upstream-$tag.log" 2>&1 &
  local pid=$!
  note_pid "$pid"
  if ! wait_for_line "$pid" "$OUT/upstream-$tag.log" "\[upstream\] 监听" 20; then
    fail "上游（$tag）没起来：$(tail -3 "$OUT/upstream-$tag.log" | tr '\n' '|')"
  fi
  echo "$pid"
}
start_gateway() { # 标签 配置文件 → pid
  local tag=$1 cfg=$2
  port_busy "$GW_PORT" && die "端口 $GW_PORT 上还压着进程（上一臂没退干净）——不带着它往下跑"
  "$GATEWAY" --serve --listen "127.0.0.1:$GW_PORT" --config "$cfg" \
    > "$OUT/gateway-$tag.log" 2>&1 &
  local pid=$!
  note_pid "$pid"
  if ! wait_for_line "$pid" "$OUT/gateway-$tag.log" "\[serve\] 监听" 20; then
    fail "网关（$tag）没起来，日志尾部：$(tail -3 "$OUT/gateway-$tag.log" | tr '\n' '|')"
  fi
  echo "$pid"
}
stop_pid() { # 真的等它退出（`wait` 对子 shell 起的进程当场返回"not a child"＝S4-04 那条假绿）
  local pid=$1 i=0
  kill -TERM "$pid" 2>/dev/null
  while (( i < 400 )); do
    kill -0 "$pid" 2>/dev/null || return 0
    ((i++)); sleep 0.05
  done
  fail "进程 $pid 20 秒还没退出，强杀"
  kill -9 "$pid" 2>/dev/null
  return 1
}
hits() { # <条数> <path> → "200 502 …"（每条一次独立连接：回源失败的表态会关掉配对下游）
  local n=$1 path=$2 out="" code
  for _ in $(seq 1 "$n"); do
    code=$(curl -s --max-time 5 -o /dev/null -w '%{http_code}' "http://127.0.0.1:$GW_PORT$path")
    out="$out ${code:-000}"
  done
  echo "${out# }"
}
count_of() { echo "$1" | tr ' ' '\n' | grep -c "^$2$"; }

cleanup() {
  while read -r pid; do [[ -n "$pid" ]] && kill -9 "$pid" 2>/dev/null; done < "$PIDFILE"
  wait 2>/dev/null
}
trap cleanup EXIT

# 四份配置。成员顺序 = 轮询顺序，所以下标 0 就是"第一个被试到的那台"。
# 阶段 1~3 **不带 health 段**：这一批要数的是重试自己的账，摘除机构掺进来会把上界读成 1。
# 阶段 1~3 的配置用一条 here-doc 写死（读数分母就在这几行里）
make_plain_config() { # 文件名 端口1 端口2
  cat > "$1" <<JSON
{
  "upstreams": [
    { "name": "api", "balance": "round_robin", "targets": [
        { "address": "127.0.0.1", "port": $2 },
        { "address": "127.0.0.1", "port": $3 } ] }
  ],
  "routes": [ { "match": "prefix", "pattern": "", "upstream": "api" } ]
}
JSON
}
make_half_open_config() { # 文件名 端口1(死) 端口2(活)
  cat > "$1" <<JSON
{
  "upstreams": [
    { "name": "api", "balance": "round_robin", "targets": [
        { "address": "127.0.0.1", "port": $2 },
        { "address": "127.0.0.1", "port": $3 } ] }
  ],
  "routes": [ { "match": "prefix", "pattern": "", "upstream": "api" } ],
  "health": { "max_fails": 1, "fail_timeout_ms": 600,
              "probe_interval_ms": 0, "max_ejection_percent": 100 },
  "circuit": { "window_ms": 10000, "minimum_requests": 1000, "error_rate_percent": 50,
               "half_open_requests": 2, "half_open_successes": 2,
               "half_open_timeout_ms": 3000 }
}
JSON
}

make_plain_config "$OUT/one-dead.json"    "$DEAD_A" "$ALIVE_A"
make_plain_config "$OUT/both-dead.json"   "$DEAD_A" "$DEAD_B"
make_plain_config "$OUT/both-alive.json"  "$ALIVE_A" "$ALIVE_B"
make_half_open_config "$OUT/half-open.json" "$DEAD_A" "$ALIVE_A"

echo "== 阶段 1：一台死一台活（下界 + 后端实收这条上界）=="
A1=$(start_upstream p1-alive "$ALIVE_A")
GW1=$(start_gateway phase1 "$OUT/one-dead.json")
T0=$(now_ms)
CODES1=$(hits 20 /retry)
T1=$(now_ms)
stop_pid "$GW1"
RETRY1=$(get_stat upstream_retries "$OUT/gateway-phase1.log")
REPLY1=$(get_stat gateway_replies_sent "$OUT/gateway-phase1.log")
FAILED_CONNECT1=$(get_stat upstream_connect_failed "$OUT/gateway-phase1.log")
AFTER1=$(get_stat gateway_replies_after_retry "$OUT/gateway-phase1.log")
stop_pid "$A1"
ALIVE_RECV1=$(upstream_received "$OUT/upstream-p1-alive.log")
n200=$(count_of "$CODES1" 200)
echo "  状态行：$CODES1"
echo "  时间差 $((T1 - T0)) ms（20 条本机请求，区间断言用）"
if [[ "$n200" == "20" ]]; then
  ok "客户端 20 条全 200：首试被拒的那些被另一台救回来了（一条 502 都没露出来）"
else
  fail "期望 20 条 200，实到 200×$n200（其余：$(echo "$CODES1" | tr ' ' '\n' | sort | uniq -c | tr '\n' ' ')）"
fi
# 下界：重试发生过，而且落到了**另一台**——另一台自己数的条数就是它的收据。
if [[ "${RETRY1:-0}" -ge 1 ]]; then
  ok "下界：upstream_retries=$RETRY1（'从不重试'的实现这一格读 0）"
else
  fail "upstream_retries=${RETRY1:-空}：重试没发生，而后端却接住了全部流量——两件事必须同时成立才算真"
fi
if [[ "$ALIVE_RECV1" == "20" ]]; then
  ok "落点：活的那台自报收到 20 条 == 客户端 20 条（**后端实收 ≤ 客户端请求数**，一条都没被复制）"
else
  fail "活的那台自报 $ALIVE_RECV1 条，客户端发了 20 条——放大倍数就是这一格的差额"
fi
# 上界：尝试数 = 请求数 + retries ≤ 2 × 请求数
if [[ "${RETRY1:-99}" -le 20 ]]; then
  ok "上界：尝试数 $((20 + RETRY1)) ≤ 2×20（预算 1 的顶格）"
else
  fail "retries=$RETRY1 > 20 → 尝试数超过 2×请求数：预算失守（级联重试）"
fi
if [[ "${REPLY1:-0}" == "0" && "${AFTER1:-0}" == "0" ]]; then
  ok "救回来的请求不该有自产表态：replies_sent=0、after_retry=0"
else
  fail "replies_sent=${REPLY1:-空} / after_retry=${AFTER1:-空}，期望 0/0"
fi
if [[ "${FAILED_CONNECT1:-0}" == "$RETRY1" ]]; then
  ok "首试被拒的次数与重试次数一一对上：upstream_connect_failed=$FAILED_CONNECT1"
else
  fail "connect_failed=${FAILED_CONNECT1:-空} 而 retries=${RETRY1:-空}——两把尺数的是同一批事件才对"
fi

echo "== 阶段 2：两台全死（预算顶格 + 一次表态仍只有一条）=="
GW2=$(start_gateway phase2 "$OUT/both-dead.json")
CODES2=$(hits 10 /both-dead)
stop_pid "$GW2"
RETRY2=$(get_stat upstream_retries "$OUT/gateway-phase2.log")
CONNECT2=$(get_stat upstream_connect_failed "$OUT/gateway-phase2.log")
REPLY2=$(get_stat gateway_replies_sent "$OUT/gateway-phase2.log")
CFAIL2=$(get_stat gateway_replies_connect_failed "$OUT/gateway-phase2.log")
AFTER2=$(get_stat gateway_replies_after_retry "$OUT/gateway-phase2.log")
EJECT2=$(get_stat gateway_replies_all_ejected "$OUT/gateway-phase2.log")
n502=$(count_of "$CODES2" 502)
echo "  状态行：$CODES2"
if [[ "$n502" == "10" ]]; then ok "10 条全部拿到完整 502（没有一条挂在缓冲里）"; else fail "期望 10 条 502，实到 $n502"; fi
if [[ "${RETRY2:-0}" == "10" ]]; then
  ok "顶格：retries=10 == 请求数（级联会把它读到 20 以上，'从不重试'读到 0）"
else
  fail "retries=${RETRY2:-空}，期望 10（每条恰好花掉一次预算）"
fi
if [[ "${CONNECT2:-0}" == "20" ]]; then
  ok "两次尝试都有账：upstream_connect_failed=20 == 请求数 ×(1+预算)（ADR-23 第六节那条翻倍）"
else
  fail "upstream_connect_failed=${CONNECT2:-空}，期望 20"
fi
if [[ "${REPLY2:-0}" == "10" && "${CFAIL2:-0}" == "10" && "${AFTER2:-0}" == "10" ]]; then
  ok "重试用掉之后仍然只有一条表态：replies_sent=10、connect_failed=10、after_retry=10（子集关系成立）"
else
  fail "replies_sent=${REPLY2:-空} / connect_failed=${CFAIL2:-空} / after_retry=${AFTER2:-空}，期望 10/10/10"
fi
if [[ "${EJECT2:-0}" == "0" ]]; then ok "这一臂没装 health，all_ejected 那格应当空着（=0）：$EJECT2"; else fail "all_ejected=$EJECT2，不该有数"; fi

echo "== 阶段 3：两台都活（零误重）=="
A3=$(start_upstream p3-a "$ALIVE_A")
B3=$(start_upstream p3-b "$ALIVE_B")
GW3=$(start_gateway phase3 "$OUT/both-alive.json")
CODES3=$(hits 12 /alive)
stop_pid "$GW3"
RETRY3=$(get_stat upstream_retries "$OUT/gateway-phase3.log")
CONNECT3=$(get_stat upstream_connect_failed "$OUT/gateway-phase3.log")
stop_pid "$A3"; stop_pid "$B3"
RECV3A=$(upstream_received "$OUT/upstream-p3-a.log")
RECV3B=$(upstream_received "$OUT/upstream-p3-b.log")
n200=$(count_of "$CODES3" 200)
echo "  状态行：$CODES3"
if [[ "$n200" == "12" ]]; then ok "12 条全 200"; else fail "期望 12 条 200，实到 $n200"; fi
if [[ "${RETRY3:-0}" == "0" ]]; then
  ok "健康的路径上一条都不重试：upstream_retries=0（'看见什么都换一台'的实现这里露出来）"
else
  fail "upstream_retries=$RETRY3，期望 0：正常流量被重放就是纯粹的成本"
fi
if [[ "${CONNECT3:-0}" == "0" ]]; then ok "没有任何 connect 失败：$CONNECT3"; else fail "connect_failed=$CONNECT3，期望 0"; fi
if [[ $((RECV3A + RECV3B)) == "12" ]]; then
  ok "两台合计自报 12 条 == 客户端 12 条（轮询各 $RECV3A/$RECV3B）"
else
  fail "两台合计 $((RECV3A + RECV3B))，期望 12"
fi

echo "== 阶段 4：半开被钉的探测请求不换台 =="
A4=$(start_upstream p4-alive "$ALIVE_A")
GW4=$(start_gateway phase4 "$OUT/half-open.json")
CODES4=$(hits 16 /probe-pin)
sleep 1.0
CODES4B=$(hits 8 /probe-pin)
stop_pid "$GW4"
RETRY4=$(get_stat upstream_retries "$OUT/gateway-phase4.log")
ADMIT4=$(get_stat "health_t0_127.0.0.1_${DEAD_A}_half_open_admitted" "$OUT/gateway-phase4.log")
RECOVER4=$(get_stat "health_t0_127.0.0.1_${DEAD_A}_half_open_recovered" "$OUT/gateway-phase4.log")
REOPEN4=$(get_stat "health_t0_127.0.0.1_${DEAD_A}_half_open_reopened" "$OUT/gateway-phase4.log")
EJECT4=$(get_stat "health_t0_127.0.0.1_${DEAD_A}_ejections" "$OUT/gateway-phase4.log")
CFAIL4=$(get_stat gateway_replies_connect_failed "$OUT/gateway-phase4.log")
AFTER4=$(get_stat gateway_replies_after_retry "$OUT/gateway-phase4.log")
stop_pid "$A4"
ALIVE_RECV4=$(upstream_received "$OUT/upstream-p4-alive.log")
n502=$(count_of "$CODES4 $CODES4B" 502)
n200=$(count_of "$CODES4 $CODES4B" 200)
echo "  状态行：$CODES4 / $CODES4B"
if [[ "${ADMIT4:-0}" -ge 1 ]]; then
  ok "半开门确实放过过请求（admitted=$ADMIT4、reopened=$REOPEN4、ejections=$EJECT4）"
else
  fail "half_open_admitted=${ADMIT4:-空}：这台从没进过半开，阶段 4 的判据无从谈起"
fi
if [[ "${RECOVER4:-1}" == "0" ]]; then
  ok "那台死成员从没出过半开（recovered=0）——被钉进去的探测请求没有替它攒够证据"
else
  fail "half_open_recovered=$RECOVER4，期望 0"
fi
# 这一格是阶段 4 的全部：**被钉的请求都当场 502**，一条都没被换台救活。
# 如果重放抢了半开的名额或把探测请求改派给别的成员，`connect_failed` 会小于 `admitted`、
# 而 `after_retry` 会大于 0。
if [[ "${CFAIL4:-x}" == "${ADMIT4:-y}" && "${AFTER4:-0}" == "0" ]]; then
  ok "探测条数 == 502 条数（$ADMIT4）、after_retry=0：被钉住的那条**没换台**（换了就是 200，这格就空了）"
else
  fail "connect_failed=${CFAIL4:-空} vs admitted=${ADMIT4:-空}、after_retry=${AFTER4:-空}：期望相等且 0"
fi
if [[ "${RETRY4:-0}" == "1" ]]; then
  ok "摘除之前只有第 1 条请求用掉过一次重试（此后候选集只剩一台）：retries=$RETRY4"
else
  fail "upstream_retries=${RETRY4:-空}，期望 1"
fi
if [[ "$((n502 + n200))" == "24" ]]; then
  ok "24 条客户端请求全部拿到完整表态：200×$n200 + 502×$n502"
else
  fail "表态条数 $((n502 + n200)) != 24（客户端有请求没收到答复）"
fi

echo "== 阶段 5：生命周期三重（重试多开的那条上游也得关干净）=="
GW5=$(start_gateway phase5 "$OUT/one-dead.json")
A5=$(start_upstream p5-alive "$ALIVE_A")
hits 8 /lifecycle > /dev/null
stop_pid "$GW5"
stop_pid "$A5"
FD_BEFORE=$(sed -n 's/^\[serve\] fd_start=\([0-9]*\).*/\1/p' "$OUT/gateway-phase5.log" | head -1)
FD_AFTER=$(sed -n 's/^\[serve\] fd_end=\([0-9]*\).*/\1/p' "$OUT/gateway-phase5.log" | head -1)
LIVE=$(get_stat live_connections "$OUT/gateway-phase5.log")
CONSTRUCTED=$(get_stat connections_constructed "$OUT/gateway-phase5.log")
DESTRUCTED=$(get_stat connections_destructed "$OUT/gateway-phase5.log")
if [[ -n "$FD_BEFORE" && "$FD_BEFORE" == "$FD_AFTER" ]]; then
  ok "fd 收支平衡：$FD_BEFORE → $FD_AFTER"
else
  fail "fd ${FD_BEFORE:-空} → ${FD_AFTER:-空}：重播放弃的那条上游没关掉"
fi
if [[ "${LIVE:-x}" == "0" ]]; then ok "live_connections=0"; else fail "live=${LIVE:-空}"; fi
if [[ -n "$CONSTRUCTED" && "$CONSTRUCTED" == "$DESTRUCTED" ]]; then
  ok "构造数 == 析构数（$CONSTRUCTED）：被丢弃的重放配对没有留下对象"
else
  fail "constructed=${CONSTRUCTED:-空} vs destructed=${DESTRUCTED:-空}"
fi

echo
if [[ "$FAILED" == "0" ]]; then
  echo "== smoke-s4-06：全绿（四臂 + 生命周期；证据在 $OUT/）=="
  exit 0
fi
echo "== smoke-s4-06：有判定失败（见上面的 ✗），证据留在 $OUT/ =="
exit 1
