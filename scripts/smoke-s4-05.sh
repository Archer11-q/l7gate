#!/usr/bin/env bash
# S4-05 冒烟：熔断的两个新输入在**真进程**上跑出来的时间线（ADR-12 追加二）。
# ctest 那 25 条证的是状态机与候选集（喂时钟、不建 socket；端到端四条建真 socket 但没有
# 独立进程），这里证的是三件事：
#   一 **错误率窗口判住的事，连续计数判不住**：一台后端 50% 的流量打它、每次都回 503，
#      成功夹在失败之间 → 连续计数永远清零。窗口必须替它摘，而且摘除之后它一条流量也接不到
#      （上界），摘除之前它确实接过（下界）。
#   二 **半开限流**：到期不再全量放行。每个周期放出去的探测条数由**后端自己数的请求数**
#      背书——`bad 收到的总条数 == 摘除前的份额 + 门放行的条数` 这一式对不上，
#      就是要么门没夹住、要么网关的计数在说谎。
#   三 **半开 → 全量恢复的时间线**：故障消退之后攒满 K 条业务成功就转正，转正之后
#      它接得到流量（"恢复了"与"恢复后接得到流量"是两件事——ADR-19 追加三那条）。
# 外加进程侧的两条矛盾拒绝（circuit 没 health、K>B）与生命周期三重检查。
#
# 用法：PRESET=debug scripts/smoke-s4-05.sh      输出：build/smoke-s4-05/，退出码 0 = 全过
# 三条臂都用 `probe_interval_ms: 0`（纯被动）：ADR-12 追加二第七节写明了主动探活仍是
# 一条"立即全量恢复"的出口，那条路会把半开绕过——取证必须在没有它的世界里做。
set -uo pipefail
cd "$(dirname "$(readlink -f "$0")")/.." || exit 1

trap '' PIPE
PRESET="${PRESET:-release}"
BIN="build/$PRESET"
GATEWAY="$BIN/l7gate"
UPSTREAM="$BIN/tests/l7gate_test_upstream"
OUT="build/smoke-s4-05"
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

GW_PORT=18500
GOOD_PORT=18501
BAD_PORT=18502

port_busy() { (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; }
for p in "$GW_PORT" "$GOOD_PORT" "$BAD_PORT"; do
  port_busy "$p" && die "端口 $p 已被占用：先清场"
done

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
# 不假设键在行首（口径⑭(b)：一行多键配行首锚 = 其余几格静默读成空串，而空串在判定里长得像 0）
get_stat() { # key logfile → 值
  local key=$1 log=$2
  grep -o "[[:space:]]$1=[0-9-]*" "$log" 2>/dev/null | head -1 | sed 's/.*=//'
}
# 一台上游自报"我收到几条"= 两条分支相加（`--path-status` 那条不进 `已服务请求数`，
# S4-02 的老口径；只读第一行会让回 503 的那台恒读 0，而 0 看起来正像"它没接到流量"）。
upstream_received() { # logfile → 总条数
  local log=$1 a b
  a=$(sed -n 's/^\[upstream\] 已服务请求数=\([0-9]*\).*/\1/p' "$log" | head -1)
  b=$(sed -n 's/^\[upstream\] 固定状态码响应数=\([0-9]*\).*/\1/p' "$log" | head -1)
  echo $(( ${a:-0} + ${b:-0} ))
}
upstream_normal() { # logfile → 走默认响应分支的条数（= 回 200 的那几条）
  sed -n 's/^\[upstream\] 已服务请求数=\([0-9]*\).*/\1/p' "$1" | head -1
}

note_pid() { echo "$1" >> "$PIDFILE"; }
start_upstream() { # 标签 端口 额外参数… → pid
  local tag=$1 port=$2; shift 2
  port_busy "$port" && die "端口 $port 上还压着进程（上一臂没退干净）——不带着它往下跑"
  "$UPSTREAM" --listen "127.0.0.1:$port" --body-bytes 64 "$@" > "$OUT/upstream-$tag.log" 2>&1 &
  local pid=$!
  note_pid "$pid"
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
stop_pid() { # pid → 真的等它退出（`wait` 对子 shell 起的进程当场返回"not a child"）
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
hits() { # <条数> <path> [间隔毫秒] → "200 503 …"
  local n=$1 path=$2 gap=${3:-0} out="" code
  for _ in $(seq 1 "$n"); do
    code=$(curl -s --max-time 5 -o /dev/null -w '%{http_code}' "http://127.0.0.1:$GW_PORT$path")
    out="$out ${code:-000}"
    [[ "$gap" != "0" ]] && sleep "$(awk "BEGIN{print $gap/1000}")"
  done
  echo "${out# }"
}

cleanup() {
  while read -r pid; do [[ -n "$pid" ]] && kill -9 "$pid" 2>/dev/null; done < "$PIDFILE"
  wait 2>/dev/null
}
trap cleanup EXIT

# 三份配置。窗口与半开的参数逐条写死，读数的分母就在这几行里。
write_config() { # 文件名 fail_timeout 半开预算 半开证据 样本下界
  local file=$1 fail_timeout=$2 b=$3 k=$4 min=$5
  cat > "$file" <<JSON
{
  "upstreams": [
    { "name": "api", "balance": "round_robin", "targets": [
        { "address": "127.0.0.1", "port": $GOOD_PORT },
        { "address": "127.0.0.1", "port": $BAD_PORT } ] }
  ],
  "routes": [ { "match": "prefix", "pattern": "", "upstream": "api" } ],
  "health": { "max_fails": 99, "fail_timeout_ms": $fail_timeout,
              "probe_interval_ms": 0, "max_ejection_percent": 100 },
  "circuit": { "window_ms": 10000, "minimum_requests": $min, "error_rate_percent": 50,
               "half_open_requests": $b, "half_open_successes": $k,
               "half_open_timeout_ms": 5000 }
}
JSON
}
# `max_fails: 99` 是刻意的：轮询下坏的那台每两条才吃一条失败，永远攒不到 99 连——
# 于是"摘了"只可能来自错误率窗口，连续计数那条判据在这三条臂里都够不着。
write_config "$OUT/long.json"   60000 2 2 6
write_config "$OUT/reopen.json"   600 2 2 4
write_config "$OUT/recover.json"  600 2 2 4

echo "== 阶段 1：错误率摘除（连续计数够不着的那一档）=="
G1=$(start_upstream p1-good "$GOOD_PORT")
B1=$(start_upstream p1-bad "$BAD_PORT" --path-status "/boom:503")
GW1=$(start_gateway phase1 "$OUT/long.json")
CODES1=$(hits 24 /boom)
stop_pid "$GW1"
TRIPS=$(get_stat "health_t1_127.0.0.1_${BAD_PORT}_trips_rate" "$OUT/gateway-phase1.log")
EJ=$(get_stat "health_t1_127.0.0.1_${BAD_PORT}_ejections" "$OUT/gateway-phase1.log")
EJGOOD=$(get_stat "health_t0_127.0.0.1_${GOOD_PORT}_ejections" "$OUT/gateway-phase1.log")
ADMITTED=$(get_stat "health_t1_127.0.0.1_${BAD_PORT}_half_open_admitted" "$OUT/gateway-phase1.log")
BLOCKED=$(get_stat health_ejections_blocked_by_valve "$OUT/gateway-phase1.log")
stop_pid "$G1"; stop_pid "$B1"
BAD_RECV=$(upstream_received "$OUT/upstream-p1-bad.log")
GOOD_RECV=$(upstream_received "$OUT/upstream-p1-good.log")
n_ok=$(echo "$CODES1" | tr ' ' '\n' | grep -c '^200$')
n_5xx=$(echo "$CODES1" | tr ' ' '\n' | grep -c '^503$')
if [[ "$TRIPS" == "1" && "$EJ" == "1" ]]; then
  ok "坏的那台被**窗口**摘掉：trips_rate=$TRIPS、ejections=$EJ（max_fails=99 那条够不着）"
else
  fail "trips_rate=$TRIPS / ejections=$EJ，期望 1/1——窗口判据在真进程上没生效"
fi
if [[ "$EJGOOD" == "0" ]]; then ok "全 200 的那台一条摘除都没记（ejections=$EJGOOD）"; else fail "健康的那台被牵连：ejections=$EJGOOD"; fi
# 下界：摘除之前它确实接过流量（"从不熔断"的实现这里就露出来）
if [[ "$BAD_RECV" -ge 5 && "$BAD_RECV" -le 8 ]]; then
  ok "摘除前它收到 $BAD_RECV 条（样本下界 6，前后各一条的时序容差）——判据是真的攒出来的"
else
  fail "坏后端自报收到 $BAD_RECV 条，期望 5..8：要么它一直没接到流量（装配错），要么没摘"
fi
# 上界：摘除之后（fail_timeout=60s，本臂内不会到期）它一条也接不到
if [[ "$ADMITTED" == "0" && "$BAD_RECV" -le 8 ]]; then
  ok "摘除期内没有放行：half_open_admitted=$ADMITTED，它总共只收到 $BAD_RECV 条（打了 24 条）"
else
  fail "被摘除的后端还在接流量：admitted=$ADMITTED、自报 $BAD_RECV 条"
fi
if [[ "$GOOD_RECV" -ge 16 ]]; then ok "健康的那台接走其余 $GOOD_RECV 条（200 共 $n_ok 条、503 共 $n_5xx 条）"; else fail "健康那台只接到 $GOOD_RECV 条，期望 ≥16"; fi
if [[ "$BLOCKED" == "0" ]]; then ok "阀没参与（percent=100、只有一台该摘）：blocked_by_valve=0"; else fail "blocked_by_valve=$BLOCKED，期望 0"; fi
if [[ "$n_ok" -lt 16 ]]; then fail "客户端侧 200 只有 $n_ok 条，期望 ≥16（摘除后流量该全走健康那台）"; fi

echo "== 阶段 2：到期进半开，只放 B 条；探测失败立即回摘 =="
G2=$(start_upstream p2-good "$GOOD_PORT")
B2=$(start_upstream p2-bad "$BAD_PORT" --path-status "/boom:503")
GW2=$(start_gateway phase2 "$OUT/reopen.json")
CODES2=$(hits 20 /boom 120)          # 20 条 × 120ms ≈ 2.4s，跨过 4 个 600ms 的摘除周期
stop_pid "$GW2"
ADMITTED2=$(get_stat "health_t1_127.0.0.1_${BAD_PORT}_half_open_admitted" "$OUT/gateway-phase2.log")
DENIED2=$(get_stat "health_t1_127.0.0.1_${BAD_PORT}_half_open_denied" "$OUT/gateway-phase2.log")
REOPENED2=$(get_stat "health_t1_127.0.0.1_${BAD_PORT}_half_open_reopened" "$OUT/gateway-phase2.log")
EJ2=$(get_stat "health_t1_127.0.0.1_${BAD_PORT}_ejections" "$OUT/gateway-phase2.log")
REC2=$(get_stat "health_t1_127.0.0.1_${BAD_PORT}_half_open_recovered" "$OUT/gateway-phase2.log")
stop_pid "$G2"; stop_pid "$B2"
BAD_RECV2=$(upstream_received "$OUT/upstream-p2-bad.log")
GOOD_RECV2=$(upstream_received "$OUT/upstream-p2-good.log")
if [[ "$REOPENED2" -ge 1 && "$EJ2" -ge 2 ]]; then
  ok "半开期内失败**立即回摘**：reopened=$REOPENED2、ejections=$EJ2（不等 max_fails，这里它够不着）"
else
  fail "reopened=$REOPENED2 / ejections=$EJ2，期望 ≥1 / ≥2——回摘那条路没走"
fi
if [[ "$ADMITTED2" -ge 1 ]]; then
  ok "门真的放过探测：half_open_admitted=$ADMITTED2（denied=$DENIED2）——'半开从不放行'的实现这一格会读 0"
else
  fail "half_open_admitted=$ADMITTED2，期望 ≥1：半开成了只进不出的门"
fi
# 仪器对账：它收到的总条数 = 摘除前的 4 条 + 门放行的条数。两边独立数，等式不成立就是
# 有一侧在说谎（门夹不住、或者计数不落到那条路径上）。
if [[ "$BAD_RECV2" == $((4 + ADMITTED2)) ]]; then
  ok "后端自报 $BAD_RECV2 条 == 摘除前 4 条 + 门放行 $ADMITTED2 条（逐条对得上）"
else
  fail "自报 $BAD_RECV2 条，期望 4+$ADMITTED2=$((4 + ADMITTED2))——放行条数与实际到达不吻合"
fi
# 上界：一个周期最多消耗 B=2 条；失败即回摘，所以每周期实际只花 1 条，
# 松一口气留最后一个还没闭合的周期（它可能正卡在第 2 条上）。
if [[ "$ADMITTED2" -le $((REOPENED2 + 2)) ]]; then
  ok "放行条数受半开周期数约束：admitted=$ADMITTED2 ≤ reopened($REOPENED2)+B(2)"
else
  fail "admitted=$ADMITTED2 > reopened($REOPENED2)+2：预算没夹住，半开等于全量放行"
fi
if [[ "$REC2" == "0" ]]; then ok "它一次都没成功过，所以没有假恢复：recovered=0"; else fail "recovered=$REC2，期望 0（全 503 的后端不该被转正）"; fi
if [[ "$GOOD_RECV2" -ge 12 ]]; then ok "其余流量由健康那台接走：$GOOD_RECV2 条"; else fail "健康那台只接到 $GOOD_RECV2 条，期望 ≥12"; fi
echo "  客户端状态序列（20 条）：$CODES2"

echo "== 阶段 3：故障消退 → 半开攒够 K 条 → 全量恢复的时间线 =="
G3=$(start_upstream p3-good "$GOOD_PORT")
B3=$(start_upstream p3-bad "$BAD_PORT" --path-status "/boom:503")
GW3=$(start_gateway phase3 "$OUT/recover.json")
CODES3A=$(hits 12 /boom)               # min=4 → 第 8 条就触发（两台各吃到 4 条失败）
sleep 0.8                              # 跨过 fail_timeout=600ms：到期只该进半开，不该全量
T_PROBE=$(date +%s%N | cut -c1-13)      # 墙钟毫秒。这台机器的 `date +%s%3N` 不认 `%3N`、
                                        # 会把 `%N` 整段九位纳秒打出来（本轮实测），拿它当毫秒
                                        # 减就得到一个 19 位的怪物——取前 13 位才是 ms。
CODES3B=$(hits 3 /ok2)                 # 前两条是门放行的探测，它回 200 → 攒满 K=2 → 转正
CODES3C=$(hits 6 /ok2)                 # 转正之后回到全量：轮询该把一半流量给它
stop_pid "$GW3"
TRIPS3=$(get_stat "health_t1_127.0.0.1_${BAD_PORT}_trips_rate" "$OUT/gateway-phase3.log")
EJ3=$(get_stat "health_t1_127.0.0.1_${BAD_PORT}_ejections" "$OUT/gateway-phase3.log")
ADMITTED3=$(get_stat "health_t1_127.0.0.1_${BAD_PORT}_half_open_admitted" "$OUT/gateway-phase3.log")
RECOVERED3=$(get_stat "health_t1_127.0.0.1_${BAD_PORT}_half_open_recovered" "$OUT/gateway-phase3.log")
REOPENED3=$(get_stat "health_t1_127.0.0.1_${BAD_PORT}_half_open_reopened" "$OUT/gateway-phase3.log")
HALFNOW3=$(get_stat circuit_half_open_now "$OUT/gateway-phase3.log")
EPOCH3=$(get_stat "health_t1_127.0.0.1_${BAD_PORT}_last_ejection_epoch_ms" "$OUT/gateway-phase3.log")
stop_pid "$G3"; stop_pid "$B3"
BAD503=$(sed -n 's/^\[upstream\] 固定状态码响应数=\([0-9]*\).*/\1/p' "$OUT/upstream-p3-bad.log" | head -1)
BAD200=$(upstream_normal "$OUT/upstream-p3-bad.log")
GOOD200=$(upstream_normal "$OUT/upstream-p3-good.log")
if [[ "$TRIPS3" == "1" && "$EJ3" == "1" ]]; then ok "摘除由窗口触发：trips_rate=$TRIPS3、ejections=$EJ3"; else fail "trips_rate=$TRIPS3 / ejections=$EJ3，期望 1/1"; fi
if [[ "$ADMITTED3" == "2" && "$RECOVERED3" == "1" ]]; then
  ok "半开放行 $ADMITTED3 条（=B）→ 攒满 K=2 → 全量恢复（recovered=$RECOVERED3、reopened=$REOPENED3）"
else
  fail "admitted=$ADMITTED3 / recovered=$RECOVERED3，期望 2/1——恢复那条路在真进程上没走通"
fi
if [[ "$REOPENED3" == "0" && "$HALFNOW3" == "0" ]]; then ok "没有回摘、退出时不留半开态：reopened=0、circuit_half_open_now=0"; else fail "reopened=$REOPENED3 / half_open_now=$HALFNOW3，期望 0/0"; fi
# 时间线的两端都由**后端自己数的**背书：它先吃了 4 条 503（摘除的成因），
# 恢复之后又真的接到 200 的流量（不是"状态位改了但没流量"那一格）。
if [[ "$BAD503" == "4" ]]; then ok "摘除前它自报收到 4 条 503（窗口分母就是这几条）"; else fail "它自报 503 条数=$BAD503，期望 4"; fi
if [[ "$BAD200" -ge 3 ]]; then ok "恢复后它真的接得到 200 流量：$BAD200 条（探测 2 条 + 转正后的份额）"; else fail "恢复后它只接到 $BAD200 条 200，期望 ≥3：'恢复了'不等于'接得到流量'"; fi
if [[ "$GOOD200" -ge 4 ]]; then ok "健康那台同期 $GOOD200 条"; else fail "健康那台只有 $GOOD200 条，期望 ≥4"; fi
if [[ "$CODES3B" == "200 200 200" ]]; then ok "到期后的三条探测全 200：$CODES3B"; else fail "到期后那三条给出 $CODES3B，期望 200 200 200"; fi
if [[ "$CODES3C" == "200 200 200 200 200 200" ]]; then ok "转正后 6 条全 200"; else fail "转正后给出 $CODES3C"; fi
if [[ "$(echo "$CODES3A" | tr ' ' '\n' | grep -c '^503$')" == "4" ]]; then
  ok "摘除之后它一条 503 也没再发出来（12 条里只有前 4 条是 503）"
else
  fail "摘除期内有超出 4 条的 503：$CODES3A"
fi
# 恢复时间线的两端钉在**同一根墙钟轴**上（B-014 那套读法）：摘除那一端是网关自报的
# `last_ejection_epoch_ms`，探测这一端是脚本发出探测前的 `date +%s%3N`。
# 这一段的意义不是"多少毫秒恢复"——那个数由配置的 fail_timeout 与流量间隔决定——
# 而是"到期之后第一次探测立刻发生、第 K 条成功立刻转正"，中间没有额外等待。
if [[ -n "$EPOCH3" && "$EPOCH3" != "0" ]]; then
  GAP=$((T_PROBE - EPOCH3))
  ok "时间线两端：摘除自报墙钟 ${EPOCH3}ms → 首次探测发出在其后 ${GAP}ms（fail_timeout=600 + 脚本等 800），转正在那 2 条探测之内"
  # 这根轴本身也要断言：差值必须落在"等待窗口 + 一拍调度"的量级里。读出一个负数或一个
  # 六位以上的数，说明两边的单位不一致（这台机器的 `date` 就给了我一次纳秒）。
  if [[ "$GAP" -lt 600 || "$GAP" -gt 20000 ]]; then
    fail "摘除到探测的间隔 $GAP ms 落在 fail_timeout(600) 与 20s 之外——时间轴或单位不对"
  fi
else
  fail "last_ejection_epoch_ms 读回 '$EPOCH3'——恢复时间线少了网关自报那一端"
fi

echo "== 阶段 4：进程侧的矛盾拒绝（退出码 2，话里点名是哪一项）=="
run_reject() { # 标签 内容 需要点名的关键词…
  local tag=$1 content=$2; shift 2
  printf '%s\n' "$content" > "$OUT/$tag.json"
  "$GATEWAY" --serve --listen "127.0.0.1:18509" --config "$OUT/$tag.json" > "$OUT/$tag.out" 2>&1
  local rc=$?
  if [[ $rc -ne 2 ]]; then fail "$tag：退出码 $rc，期望 2"; return; fi
  local word
  for word in "$@"; do
    if grep -q -- "$word" "$OUT/$tag.out"; then ok "$tag：拒绝且话里点了「$word」"; else fail "$tag：拒绝了但没点「$word」——$(tail -2 "$OUT/$tag.out" | tr '\n' '|')"; fi
  done
}
run_reject no-health '{"upstreams":[{"name":"api","balance":"round_robin","targets":[{"address":"127.0.0.1","port":18501}]}],"circuit":{}}' circuit health
run_reject k-b '{"upstreams":[{"name":"api","balance":"round_robin","targets":[{"address":"127.0.0.1","port":18501}]}],"health":{},"circuit":{"half_open_requests":2,"half_open_successes":3}}' half_open_successes half_open_requests
run_reject window '{"upstreams":[{"name":"api","balance":"round_robin","targets":[{"address":"127.0.0.1","port":18501}]}],"health":{},"circuit":{"window_ms":50}}' window_ms

echo "== 阶段 5：deploy/gateway-circuit.json 直接跑通（示例配置不是示意图）=="
# 这份示例故意用**生产形状**的参数（max_fails=3、探活 1s、minimum_requests=20），和上面
# 三条臂的隔离参数不同——它要证的是"照 README 抄的那份文件真的能起来、两条判据各管各的"。
sed -e "s/18081/$GOOD_PORT/" -e "s/18082/$BAD_PORT/" deploy/gateway-circuit.json > "$OUT/shipped.json"
G5=$(start_upstream p5-good "$GOOD_PORT")
B5=$(start_upstream p5-bad "$BAD_PORT" --path-status "/boom:503")
GW5=$(start_gateway shipped "$OUT/shipped.json")
if grep -q 'circuit=on window_ms=10000 minimum_requests=20 error_rate_percent=50 half_open_requests=3 half_open_successes=2 half_open_timeout_ms=3000' "$OUT/gateway-shipped.log"; then
  ok "示例配置那六个数原样进横幅"
else
  fail "横幅与示例配置不符：$(grep -o 'circuit=on.*' "$OUT/gateway-shipped.log" | head -1)"
fi
if grep -q 'health=on max_fails=3 fail_timeout_ms=10000 probe_interval_ms=1000' "$OUT/gateway-shipped.log"; then
  ok "health 段照常生效（探活开着也能起，两条判据共存）"
else
  fail "health 横幅与示例不符：$(grep -o 'health=on.*' "$OUT/gateway-shipped.log" | head -1)"
fi
hits 6 /boom >/dev/null
stop_pid "$GW5"
T5=$(get_stat "health_t1_127.0.0.1_${BAD_PORT}_trips_rate" "$OUT/gateway-shipped.log")
E5=$(get_stat "health_t1_127.0.0.1_${BAD_PORT}_ejections" "$OUT/gateway-shipped.log")
V5=$(get_stat health_ejections_blocked_by_valve "$OUT/gateway-shipped.log")
stop_pid "$G5"; stop_pid "$B5"
if [[ "$T5" == "0" ]]; then
  ok "6 条 503 里窗口只攒到 3 条样本 < minimum_requests=20 → 错误率那条**没判**（trips_rate=0）"
else
  fail "trips_rate=$T5，期望 0：体积下界在示例配置上没兜住"
fi
# 这份示例是**生产形状**：`max_ejection_percent: 10` 配 2 个成员 → floor(2×10/100)=0，
# 于是连续计数那条（max_fails=3）确实成立、却一只阀位都批不出来。这不是配置的毛病，
# 是 ADR-12 那条 floor 读法在两台组上的必然结果（S3-03 已裁："想要摘除能力就把比例调大，
# 不替配小集群的人偷偷放宽成 ceil"）——被挡下的这一次必须有账，不然就是静默失灵。
if [[ "$E5" == "0" && "$V5" -ge 1 ]]; then
  ok "判据成立但被阀挡下：ejections=0、blocked_by_valve=$V5（floor(2×10/100)=0，两台组想摘就得调比例）"
else
  fail "ejections=$E5 / blocked_by_valve=$V5，期望 0 / ≥1——安全阀的 floor 语义或它的计数有一条不对"
fi

echo "== 阶段 6：启动横幅与生命周期 =="
if grep -q 'circuit=on' "$OUT/gateway-phase3.log"; then
  ok "横幅 circuit=on 一行六项齐全：$(grep -o 'circuit=on.*' "$OUT/gateway-phase3.log" | head -1)"
else
  fail "横幅里没有 circuit=on（装配了熔断却不说，报告里就没法对账分母）"
fi
if grep -q 'health=on max_fails=99' "$OUT/gateway-phase3.log"; then ok "横幅 health=on 与配置一致（max_fails=99 是刻意的隔离）"; else fail "health 横幅不对"; fi
LIVE=$(get_stat live_connections "$OUT/gateway-phase3.log")
CONSTRUCTED=$(get_stat connections_constructed "$OUT/gateway-phase3.log")
DESTRUCTED=$(get_stat connections_destructed "$OUT/gateway-phase3.log")
FD_START=$(get_stat fd_start "$OUT/gateway-phase3.log")
FD_END=$(get_stat fd_end "$OUT/gateway-phase3.log")
if [[ "$LIVE" == "0" ]]; then ok "live_connections=0"; else fail "live_connections=$LIVE"; fi
if [[ -n "$CONSTRUCTED" && "$CONSTRUCTED" == "$DESTRUCTED" ]]; then ok "constructed==destructed==$CONSTRUCTED"; else fail "constructed=$CONSTRUCTED / destructed=$DESTRUCTED 不等"; fi
if [[ "$FD_START" == "$FD_END" ]]; then ok "fd $FD_START→$FD_END 回到基线"; else fail "fd $FD_START→$FD_END 不等"; fi

echo
if [[ $FAILED -eq 0 ]]; then
  echo "smoke-s4-05: 全部通过（日志在 $OUT/）"
  echo "DONE"
  exit 0
fi
echo "smoke-s4-05: 有失败项（见上面 ✗），日志在 $OUT/"
exit 1
