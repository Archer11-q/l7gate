#!/usr/bin/env bash
# SLO #2「100k 长连接资源占用」+ SLO #1 的 p999 半边取数脚本。口径抄自 docs/SLO.md：
#
#   100k WebSocket 长连接 / 握手完成后进入空闲（不发业务帧）/ 稳态 = 连接数达标后静置 ≥60s
#   且 RSS 不再单调上升 / RSS 取 /proc/self/status 的 VmRSS（不是 ps 的混合口径）/
#   必须记录 malloc 实现 / 每连接内存 = (VmRSS − 空进程基线) / **连接对数**（卡点四裁决）/
#   空闲 CPU < 5% / 工具是自研 C100K 客户端（ADR-17，契约 1.3 唯一许可的自研场景）
#
# 三件事写死在这个脚本里，因为它们都是"数字能不能被引用"的前提：
#
# 一、**目标地址数与每地址的连接数一律进报告**。本机同一 (目的 IP, 目的端口) 元组的实测
#     天花板是 28,232 条（第 28,233 条 EADDRNOTAVAIL，2026-10-28 实测），所以 100k 只能
#     靠多目标地址开出来——不写进报告的 100k 会让人以为单机回环天生就能开那么多。
#
# 二、**网关自己的回源跳是同一堵墙**（一条隧道 1:1 配一条上游连接，ADR-09/ADR-15，隧道态
#     不入池也不可复用）。而 `--upstream` 今天只能配**一个**地址 → 这一脚本目前最多能测到
#     28,232 连接对。撞到这个天花板时它**不**改口径、不降 SLO、也不起 4 个网关进程拼数：
#     它按"未达标／测量条件不齐"照记，并把 ADR-17 最后一条待裁决项打出来。
#
# 三、**工具丢了负载就作废这一轮**。accept_errors / downstream_rejected_over_capacity /
#     upstream_connect_failed / 客户端 failed 任一非 0 → fatal。BENCHMARK 那条"报告里一片 0
#     比一片 ? 危险"在这里的反面是：一片"看着正常"的数如果来自被内核队列丢掉的连接，
#     它就不是每连接内存，是每连接残骸。
#
# 用法：
#   scripts/bench/c100k.sh                                   # 正式口径（100k 对 / 4 地址 / 静置 60s）
#   CONNECTIONS=2000 TARGETS=1 scripts/bench/c100k.sh        # 冒烟档位：**非标准口径**，只验脚本自己
# 只在 release 上跑（同 s1-p99.sh：sanitizer 下的 RSS/CPU 不是被测行为）。
set -uo pipefail
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1

BIN="build/release"
GATEWAY="$BIN/l7gate"
UPSTREAM="$BIN/tools/l7gate_bench_upstream"
CLIENT="$BIN/tools/l7gate_c100k"
[[ -x "$GATEWAY" ]] || { echo "找不到 $GATEWAY；先 cmake --build --preset release" >&2; exit 1; }
[[ -x "$UPSTREAM" ]] || { echo "找不到 $UPSTREAM；先 cmake --build --preset release" >&2; exit 1; }
[[ -x "$CLIENT" ]] || { echo "找不到 $CLIENT；先 cmake --build --preset release" >&2; exit 1; }

CONNECTIONS="${CONNECTIONS:-100000}"     # SLO #2 写死 100k 连接对
TARGETS="${TARGETS:-4}"                  # 目标地址数（实测单元组上限 28,232，见文件头）
HOLD_SECONDS="${HOLD_SECONDS:-60}"       # SLO #2：达标后静置 ≥60s
RAMP_MS="${RAMP_MS:-120000}"             # 建连窗口：somaxconn=4096，摊开才测得到网关不是内核队列
WORKERS="${WORKERS:-$(nproc)}"
GATEWAY_PORT="${GATEWAY_PORT:-18083}"
DRAIN_TIMEOUT_MS="${DRAIN_TIMEOUT_MS:-30000}"   # 100k 条空闲连接的收尾也要时间（S2-05 排水）
HANDSHAKE_TIMEOUT_MS="${HANDSHAKE_TIMEOUT_MS:-30000}"
OUT="${OUT_DIR:-build/bench/c100k-$(date +%Y%m%d-%H%M%S)}"
# 单元组实测天花板：任何"每地址连接数"超过它，这一轮就不是测量而是猜。
PER_TUPLE_CEILING=28232

CANONIC=1
[[ "$CONNECTIONS" == "100000" && "$TARGETS" == "4" && "$HOLD_SECONDS" == "60" ]] || CANONIC=0

mkdir -p "$OUT" || exit 1
FAILED=0
note() { echo "  $*"; }
die() { echo "  ✗ $*" >&2; FAILED=1; }
fatal() { echo "  ✗ $*" >&2; exit 1; }
ok() { echo "  ✓ $*"; }

if [[ "$CANONIC" == "1" ]]; then
  note "口径：SLO #2 写死的那一套（$CONNECTIONS 连接对 / $TARGETS 个目标地址 / 静置 ${HOLD_SECONDS}s）"
else
  note "口径：**非标准**（conns=$CONNECTIONS targets=$TARGETS hold=${HOLD_SECONDS}s）——这份只能验脚本，不能进 BENCHMARK.md"
fi

# --- 前提：端口预算 -------------------------------------------------------
# 100k 条连接需要 100k 个"客户端→网关"元组 + 100k 个"网关→上游"元组，两侧各自受
# ip_local_port_range 约束。客户端侧靠 $TARGETS 个地址摊开；网关回源侧只有一个
# --upstream 地址（见文件头第二条），所以这里就按目标地址数把**这一轮的容量**算出来。
NEED_PER_TARGET=$(( (CONNECTIONS + TARGETS - 1) / TARGETS ))
if (( NEED_PER_TARGET > PER_TUPLE_CEILING )); then
  die "每个目标地址要扛 $NEED_PER_TARGET 条 > 实测单元组上限 $PER_TUPLE_CEILING → 加 --targets 或降 CONNECTIONS"
fi
CAPACITY=$((TARGETS * PER_TUPLE_CEILING))
note "端口预算：$TARGETS 个目标地址 × $PER_TUPLE_CEILING = $CAPACITY 条客户端连接（计划 $CONNECTIONS）"
note "        网关回源侧：1 个 --upstream 端点 → 同一元组，容量上限 $PER_TUPLE_CEILING 条（ADR-17 未决项）"

# --- 前提：fd 软上限自抬（实测 hard=1048576，抬 soft 不需要 root） ----------
# 需要值 = 网关 2×连接对 + 客户端 + 上游各自持有的那一侧。
FD_NEEDED=$((CONNECTIONS * 2 + 4096))
# 只抬**软**上限：`ulimit -n N`（不带 -S）会把软硬一起设成 N，而 hard 一旦降下来就再也
# 升不回去（非 root 只能降 hard）。实测本机 hard=1048576、soft=10240，本轮要的是把
# soft 抬到 FD_NEEDED，hard 保持原样才留得住"工具自己再 setrlimit"这条路。
if ! ulimit -Sn "$FD_NEEDED" 2>/dev/null; then
  note "ulimit -Sn $FD_NEEDED 没抬上去（当前 $(ulimit -n)，hard $(ulimit -Hn)）——工具会自己 setrlimit，继续跑"
fi
note "fd：soft=$(ulimit -n) hard=$(ulimit -Hn)（本轮需要约 $FD_NEEDED）"

UP_PID=""; G_PID=""; C_PID=""; SAMPLE_PID=""
cleanup() {
  [[ -n "$SAMPLE_PID" ]] && kill -TERM "$SAMPLE_PID" 2>/dev/null
  [[ -n "$C_PID" ]] && kill -TERM "$C_PID" 2>/dev/null
  [[ -n "$G_PID" ]] && kill -TERM "$G_PID" 2>/dev/null
  [[ -n "$UP_PID" ]] && kill -TERM "$UP_PID" 2>/dev/null
  return 0
}
trap cleanup EXIT

wait_for_line() { # pid logfile pattern [seconds]
  local pid=$1 log=$2 pat=$3 secs=${4:-30} i=0
  while (( i < secs * 20 )); do
    grep -q -- "$pat" "$log" 2>/dev/null && return 0
    kill -0 "$pid" 2>/dev/null || { echo "  进程 $pid 提前退出，日志尾部：" >&2; tail -5 "$log" >&2; return 1; }
    sleep 0.05; ((i += 1))
  done
  return 1
}

# 网关/上游/客户端都从子 shell 起，fd 软上限靠继承（脚本上面的 ulimit）。
start_upstream() {
  "$UPSTREAM" --listen "0.0.0.0:0" --upgrade --max-clients $((CONNECTIONS + 4096)) \
              --backlog 4096 > "$OUT/upstream.log" 2>&1 &
  UP_PID=$!
  wait_for_line "$UP_PID" "$OUT/upstream.log" 'bench-upstream\] 监听' 30 || return 1
  UP_PORT=$(sed -n 's/.*监听 0\.0\.0\.0:\([0-9]*\).*/\1/p' "$OUT/upstream.log" | head -1)
  [[ -n "$UP_PORT" ]]
}

start_gateway() {
  # 每 worker 的 fd 预算：连接对 ÷ worker 数，再乘 2（1 下游 + 1 上游都在同一张注册表里），
  # 外加池与在飞的余量。
  local per_worker=$(( (CONNECTIONS / WORKERS) * 2 + 512 ))
  "$GATEWAY" --serve --listen "0.0.0.0:$GATEWAY_PORT" --upstream "127.0.0.1:$UP_PORT" \
             --workers "$WORKERS" --max-connections "$per_worker" \
             --drain-timeout-ms "$DRAIN_TIMEOUT_MS" > "$OUT/gateway.log" 2>&1 &
  G_PID=$!
  wait_for_line "$G_PID" "$OUT/gateway.log" '\[serve\] 监听' 30 || return 1
}

# /proc 的两行数：VmRSS 与 (utime+stime)。读不到一律 -1，绝不拿 0 冒充。
gw_rss_kib() { awk '/^VmRSS:/{print $2; found=1} END{if(!found) print "-1"}' "/proc/$1/status" 2>/dev/null || echo -1; }
gw_cpu_ticks() { awk '{print $14 + $15}' "/proc/$1/stat" 2>/dev/null || echo -1; }
empty_baseline_rss() { # 网关进程自己的空载 VmRSS（连接一条都没进来时）
  gw_rss_kib "$G_PID"
}

command -v awk >/dev/null || fatal "本机没有 awk：取数脚本没法算分位与增量"

start_upstream || fatal "压测上游起不来（$OUT/upstream.log）"
ok "上游：pid=$UP_PID port=$UP_PORT（--upgrade：回 101 后不再管帧）"
start_gateway || fatal "网关起不来（$OUT/gateway.log）"
ok "网关：pid=$G_PID port=$GATEWAY_PORT workers=$WORKERS max_connections_per_worker=$(( (CONNECTIONS / WORKERS) * 2 + 512 ))"

# 环境落盘（契约第 7 节矩阵：任何一次压测都要有组件版本/分配器/绑核声明）
if [[ -x scripts/bench/env-snapshot.sh ]]; then
  scripts/bench/env-snapshot.sh > "$OUT/env-snapshot.out" 2>&1 || note "env-snapshot 失败（不作废本轮，但报告里会缺一栏）"
fi

RSS_EMPTY=$(empty_baseline_rss)
CPU_EMPTY=$(gw_cpu_ticks "$G_PID")
FD_EMPTY=$(ls "/proc/$G_PID/fd" 2>/dev/null | wc -l | tr -d ' ')
note "空进程基线：VmRSS=${RSS_EMPTY}KiB fd=${FD_EMPTY}（每连接内存的分母之外一切都减掉这个数）"
[[ "$RSS_EMPTY" != "-1" ]] || fatal "读不到网关 VmRSS：每连接内存没有可断言的起点"

# --- 采样线程：每 2s 一行 (t, VmRSS, utime+stime, 网关 fd 数) ----------------
(
  while kill -0 "$G_PID" 2>/dev/null; do
    printf '%s %s %s %s\n' \
      "$(awk 'BEGIN{""} /^VmRSS:/{print $2}' "/proc/$G_PID/status" 2>/dev/null || echo -1)" \
      "$(awk '{print $14 + $15}' "/proc/$G_PID/stat" 2>/dev/null || echo -1)" \
      "$(ls "/proc/$G_PID/fd" 2>/dev/null | wc -l | tr -d ' ')" \
      "$(date +%s)" >> "$OUT/gw-samples.log"
    sleep 2
  done
) &
SAMPLE_PID=$!

# --- 客户端：握手 → 空闲保持 -------------------------------------------------
# 100k 条隧道需要 100k 个**上游**连接（1:1），而网关只有一个 --upstream 端点：
# 超过 PER_TUPLE_CEILING 之后上游侧 connect 会拿 EADDRNOTAVAIL。这脚本不去绕它，
# 而是把 upstream_connect_failed 原样读出来当结论（见文件头第二条）。
if (( CONNECTIONS > PER_TUPLE_CEILING )); then
  note "预期：本轮计划 $CONNECTIONS 对 > 网关回源侧单元组上限 $PER_TUPLE_CEILING → 会撞 EADDRNOTAVAIL，照记"
fi
TADDRS=""
for ((i = 0; i < TARGETS; i++)); do
  [[ -n "$TADDRS" ]] && TADDRS+=","
  TADDRS+="127.0.0.$((i + 1)):$GATEWAY_PORT"
done
note "客户端目标地址（$TARGETS 个）：$TADDRS"

"$CLIENT" --targets "$TADDRS" --connections "$CONNECTIONS" --ramp-ms "$RAMP_MS" \
          --hold-seconds "$HOLD_SECONDS" --handshake-timeout-ms "$HANDSHAKE_TIMEOUT_MS" \
          --report-every-seconds 10 > "$OUT/client.log" 2>&1 &
C_PID=$!

# 等客户端打印 established==planned（或失败）。总预算：ramp + hold + 握手超时 + 60s 余量。
BUDGET=$(( (RAMP_MS / 1000) + HOLD_SECONDS + HANDSHAKE_TIMEOUT_MS / 1000 + 60 ))
CLIENT_RC=0
if ! wait_for_line "$C_PID" "$OUT/client.log" 'tag=final' "$BUDGET"; then
  # tag=final 没等到：可能是客户端进程已经退出（失败），也可能是超时预算不够。
  kill -0 "$C_PID" 2>/dev/null && { kill -TERM "$C_PID" 2>/dev/null; sleep 1; }
  note "客户端没打印终局（等满 ${BUDGET}s）——按失败处理"
fi
wait "$C_PID" 2>/dev/null; CLIENT_RC=$?
kill -TERM "$SAMPLE_PID" 2>/dev/null; wait "$SAMPLE_PID" 2>/dev/null; SAMPLE_PID=""
note "客户端退出码=$CLIENT_RC（0 = 计划连接全部建立且零失败）"

# 键值取数有两个坑，都是这里独有的：
#   一、`established=` 在**每个 target 行**里也出现，所以 cval 必须锚在 tag=final 那一行，
#       不能"最后一行匹配"——那样取到的是最后一个地址的数，不是总数（会少四分之三）。
#   二、精确匹配键名：`handshake_p50_us` 和 `handshake_p50_halfwidth_pct` 是同一段前缀，
#       用 grep 的"包含"语义会把两个数读成同一个。
#   三、终局报告是**多行**的：tag=final 一行、每个 target 一行、handshake_* 各一行。
#       只锚 tag=final 会把分位数读成空（我第一次就跑成了空），所以这里取"tag=final
#       起到文件尾"整块，再把 target 行剔掉——剔掉是因为它们也带 established= 这个键。
FINAL_C100K=$(awk '/^\[c100k\] tag=final/{f=1} f && !/^\[c100k\] target=/{print}' "$OUT/client.log")
cval() { awk -v k="$1" '{for (i = 1; i <= NF; ++i) {p = index($i, "="); if (p > 0 && substr($i, 1, p - 1) == k) print substr($i, p + 1)}}' <<<"$FINAL_C100K" | tail -1; }
gval() { sed -n "s/^\[serve\] $1=\([0-9-]*\).*/\1/p" "$OUT/gateway.log" | head -1; }

ESTABLISHED=$(cval established); FAILED_CONNS=$(cval failed)
HANDSHAKE_P50=$(cval handshake_p50_us); HANDSHAKE_P99=$(cval handshake_p99_us)
HANDSHAKE_P999=$(cval handshake_p999_us); P999_HW=$(cval handshake_p999_halfwidth_pct)

# --- 网关侧账：先把 drain 走完，再读终局 ------------------------------------
# 这里顺带就是 S2-05 排水在 100k 规模下的第一次真实使用：SIGTERM → 摘接受 → 100k 条
# 空闲下游立即关 → 退出。超时给到 $DRAIN_TIMEOUT_MS。
T_KILL=$(date +%s.%N)
kill -TERM "$G_PID" 2>/dev/null
for _ in $(seq 1 600); do kill -0 "$G_PID" 2>/dev/null || break; sleep 0.5; done
if kill -0 "$G_PID" 2>/dev/null; then
  fatal "网关 SIGTERM 后 300s 还没停：100k 规模下排水没收敛（这一轮的数不作数）"
fi
wait "$G_PID" 2>/dev/null; GW_RC=$?
T_DONE=$(date +%s.%N)
DRAIN_SECONDS=$(awk -v a="$T_KILL" -v b="$T_DONE" 'BEGIN{printf "%.1f", b-a}')
kill -TERM "$UP_PID" 2>/dev/null; wait "$UP_PID" 2>/dev/null; UP_PID=""
note "网关自己退出：码=$GW_RC 排水用时 ${DRAIN_SECONDS}s（workers=$WORKERS，S2-05 路径）"

LIVE=$(gval live_connections)
CONSTRUCTED=$(gval connections_constructed); DESTRUCTED=$(gval connections_destructed)
FD_START=$(gval fd_start); FD_END=$(gval fd_end)
ACCEPT_ERRORS=$(gval accept_errors); REJECTED=$(gval downstream_rejected_over_capacity)
UP_CONNECT_FAILED=$(gval upstream_connect_failed); UP_OPENED=$(gval upstream_opened)
DOWN_ACCEPTED=$(gval downstream_accepted); UPGRADES=$(gval upgrades_101)
HIGH_WATER=$(gval high_water_hits); PAUSE=$(gval backpressure_pause_count)
RSS_END=$(gval rss_end_kib)
# S2-05 的排水账在这里第一次有规模可读：这一轮所有连接都是隧道态，所以"空闲立即关"
# 那条分支一条都不该走，收干净的应该是 drain_tunnels_hard_closed。
DRAIN_REQ=$(gval drain_requested); DRAIN_INFLIGHT=$(gval drain_in_flight_at_entry)
DRAIN_IDLE=$(gval drain_closed_idle); DRAIN_TUNNEL=$(gval drain_tunnels_hard_closed)
DRAIN_FORCED=$(gval drain_forced_timeout); DRAIN_REJ=$(gval drain_rejected_new_connections)
UPSTREAM_SERVED=$(sed -n 's/.*accepted=\([0-9]*\).*/\1/p' "$OUT/upstream.log" | tail -1)
UPSTREAM_PEAK=$(sed -n 's/.*peak_clients=\([0-9]*\).*/\1/p' "$OUT/upstream.log" | tail -1)

(( GW_RC == 0 )) || fatal "网关退出码 $GW_RC：优雅停止没生效"
[[ "$ACCEPT_ERRORS" == "0" ]] || fatal "accept_errors=$ACCEPT_ERRORS：网关接不过来，这一轮丢的是连接不是延迟"
[[ "$REJECTED" == "0" ]] || fatal "downstream_rejected_over_capacity=$REJECTED：fd 预算被打到，测出来的不是 100k 那一档"
[[ "$LIVE" == "0" ]] || fatal "live_connections=$LIVE：排水没收干净（S2-05 的判据）"
[[ -n "$CONSTRUCTED" && "$CONSTRUCTED" == "$DESTRUCTED" ]] \
  || fatal "constructed=$CONSTRUCTED / destructed=$DESTRUCTED：有对象没被释放"
[[ -n "$FD_START" && "$FD_START" == "$FD_END" ]] \
  || fatal "fd ${FD_START}→${FD_END} 没回基线"

# 上游侧连接被拒/失败这一格必须**单独看**：upstream_connect_failed 是 EADDRNOTAVAIL 的落点，
# 也是"第二跳端口天花板"从推断变成实测的那个证据。它非 0 时这一轮不进 SLO #2 的判定，
# 但要照记——这正是要报给项目所有人裁决的那一格。
if [[ "$UP_CONNECT_FAILED" != "0" ]]; then
  die "upstream_connect_failed=$UP_CONNECT_FAILED：网关回源跳撞单元组上限（$PER_TUPLE_CEILING/端点）。SLO #2 的 100k 判定这一轮**不作数**，按 ADR-17 最后一条待裁决处理"
fi
(( CLIENT_RC == 0 )) || die "客户端退出码 $CLIENT_RC：established=$ESTABLISHED planned=$CONNECTIONS failed=${FAILED_CONNS:-?}（丢负载的轮次不进文档）"

# --- 稳态与内存 ------------------------------------------------------------
# 稳态判据是"RSS 不再单调上升"，所以取**达标之后那段时间**的样本：这里用客户端打印
# 达标之后的采样窗口（最后 30s）。窗口内 max−min 就是散布；它在增长的话这份内存数不成立。
read -r STEADY_MIN STEADY_MAX STEADY_LAST STEADY_SPREAD <<EOF2
$(awk 'NR>1 {print}' "$OUT/gw-samples.log" 2>/dev/null | tail -15 | awk '
  BEGIN { min = -1; max = 0; n = 0 }
  $1+0 > 0 { v = $1 + 0; if (min < 0 || v < min) min = v; if (v > max) max = v; last = v; n++ }
  END { if (n == 0) { print "-1 -1 -1 -1"; exit } printf "%d %d %d %d\n", min, max, last, max - min }')
EOF2
note "稳态窗口（最后 15 个样本 ≈30s）：VmRSS min=${STEADY_MIN} max=${STEADY_MAX} 末值=${STEADY_LAST} 散布=${STEADY_SPREAD}KiB"
(( STEADY_SPREAD >= 0 )) || fatal "读不到稳态样本：'RSS 不再单调上升'这条判据没人验过"
# 散布门：绝对值 + 相对值都给，避免"100k 连接时 5MiB 抖动"被当成稳定。
if (( STEADY_SPREAD > 20480 )); then
  die "稳态 RSS 散布 ${STEADY_SPREAD}KiB > 20MiB：还在涨，这份每连接内存不成立（要么延长静置要么查泄漏）"
fi

PAIRS="$ESTABLISHED"
# 数字比较，不是字符串比较：`[[ "$PAIRS" > "$CONNECTIONS" ]]` 在这里会按字典序把
# "25000" 判成大于 "100000"，于是每一轮都误报"分母非法"——这种门失效是最坏的，
# 因为它看起来像在保护口径，实际在制造噪声。空值/非数字同样按非法处理。
[[ "$PAIRS" =~ ^[0-9]+$ ]] || fatal "established 取不到数字（值='$PAIRS'）：每连接内存没有分母"
if (( PAIRS == 0 || PAIRS > CONNECTIONS )); then
  fatal "established=${PAIRS}（计划 $CONNECTIONS）：每连接内存的分母没有合法值，这一轮不作数"
fi
# 分母是**已配对的连接对**。这里必须再问一句"下游是不是都配上上游了"：
# 第二跳撞墙那一轮，100,000 条下游里只有 28,231 条配到上游（实测），剩下 71,769 条
# 照样占内存、照样让网关在事件里重试回源，却不进分母——于是每连接内存被算成
# 34.41 KiB/对（真实档 17.47），空闲 CPU 被算成占单核 43.41%（真实档 0.17%）。
# 这两个数都不是"未达标"的证据，是**没有口径**。所以这里直接判本轮这两格不可引用。
UNPAIRED=$(( ${DOWN_ACCEPTED:-0} - ${PAIRS:-0} ))
MEASURABLE=1
if (( UNPAIRED > 0 )); then
  MEASURABLE=0
  die "downstream_accepted=${DOWN_ACCEPTED} − established=${PAIRS} = ${UNPAIRED} 条下游没配上上游：它们进分子不进分母 → 本轮的「每连接内存」与「空闲 CPU」都没有口径（归因见 upstream_connect_failed）"
fi
# 每连接内存 = 稳态末值 − 空进程基线，再除以**连接对数**（卡点四：1 客户端 + 1 回源 = 1 个计费单位）
PER_PAIR_KIB=$(awk -v a="$STEADY_LAST" -v b="$RSS_EMPTY" -v n="$PAIRS" \
  'BEGIN{ if (a < 0 || b < 0 || n+0 == 0) { print "-1"; exit } printf "%.2f", (a - b) / n }')
if (( MEASURABLE == 1 )); then
  note "每连接内存（**连接对**口径）= (${STEADY_LAST} − ${RSS_EMPTY}) / ${PAIRS} = ${PER_PAIR_KIB} KiB/对，目标 ≤ 20 KiB"
else
  note "每连接内存：算出来是 ${PER_PAIR_KIB} KiB/对，但**本轮不引用**（${UNPAIRED} 条未配对下游在分子里、不在分母里）"
fi

# 空闲 CPU：取达标之后的窗口（最后 15 个样本的 CPU tick 增量 / 墙上时间）。
# 两个分母都报：占单核%（tick/HZ/秒）与占整机%（再除 nproc）。契约只写"空闲 CPU < 5%"，
# 没写分母——所以我把两个都摆出来，而不是挑对自己有利的那个（那正是 SLO 里"和谁比"的坑）。
read -r CPU_ONE_CORE CPU_WHOLE MACHINE <<EOF3
$(tail -15 "$OUT/gw-samples.log" | awk -v hz="$(getconf CLK_TCK)" -v cores="$(nproc)" '
  { rss[NR] = $1; cpu[NR] = $2; ts[NR] = $4 }
  END {
    n = NR
    if (n < 3) { print "-1 -1 -1"; exit }
    dticks = cpu[n] - cpu[1]
    dsec = ts[n] - ts[1]
    if (dsec <= 0 || hz <= 0) { print "-1 -1 -1"; exit }
    printf "%.2f %.2f %s\n", dticks / hz * 100.0 / dsec, dticks / hz * 100.0 / dsec / cores, cores
  }')
EOF3
note "空闲 CPU（网关进程，稳态窗口内）：占单核 ${CPU_ONE_CORE}% / 占整机(${MACHINE} 核) ${CPU_WHOLE}% —— 目标 < 5%"
(( MEASURABLE == 1 )) || note "  ↑ 同样不引用：${UNPAIRED} 条没配上上游的下游让网关在整个窗口里重试回源（见 upstream_connect_failed），这不是「空闲」"
[[ "$CPU_ONE_CORE" != "-1" ]] || die "算不出空闲 CPU：稳态样本不足"

# 握手延迟（SLO #1 的 p999 半边第一次有取数手段；注意这是**握手**RTT，不是转发尾延迟）
note "握手延迟 HDR 分位（±是桶半宽相对误差）：p50=${HANDSHAKE_P50}µs p99=${HANDSHAKE_P99}µs p999=${HANDSHAKE_P999}µs"

# --- 落一份 summary --------------------------------------------------------
SUMMARY="$OUT/c100k.summary.txt"
{
  echo "# SLO #2 取数（c100k.sh）canonic=$CANONIC"
  echo "date=$(date '+%Y-%m-%d %H:%M:%S') host=$(uname -r) nproc=$(nproc) mem_total_mib=$(awk '/MemTotal/{printf "%d", $2/1024}' /proc/meminfo)"
  echo "malloc=$("$BIN/l7gate" --self-check | sed -n 's/.*malloc=\([^ ]*\).*/\1/p')"
  echo "planned_pairs=$CONNECTIONS targets=$TARGETS per_target=$NEED_PER_TARGET per_tuple_ceiling_measured=$PER_TUPLE_CEILING capacity=$CAPACITY"
  echo "workers=$WORKERS max_connections_per_worker=$(( (CONNECTIONS / WORKERS) * 2 + 512 )) ramp_ms=$RAMP_MS hold_seconds=$HOLD_SECONDS"
  echo "established=$ESTABLISHED client_failed=${FAILED_CONNS:-?} downstream_accepted=$DOWN_ACCEPTED upstream_opened=$UP_OPENED upgrades_101=$UPGRADES"
  echo "gateway: accept_errors=$ACCEPT_ERRORS rejected_over_capacity=$REJECTED upstream_connect_failed=$UP_CONNECT_FAILED high_water_hits=$HIGH_WATER backpressure_pause=$PAUSE"
  echo "empty_baseline_rss_kib=$RSS_EMPTY steady_last_rss_kib=$STEADY_LAST steady_spread_kib=$STEADY_SPREAD rss_end_kib=${RSS_END:-?}"
  # measurable=1 才允许引用下面这两格。=0 的成因只有一种：有下游没配上上游，它们进分子
  # 不进分母（100k 名义档实测 71,769 条未配对 → 每连接内存被算成 34.41、空闲 CPU 43.41%）。
  echo "unpaired_downstreams=$UNPAIRED measurable=$MEASURABLE"
  echo "per_pair_kib=$PER_PAIR_KIB target_per_pair_kib=20"
  echo "idle_cpu_one_core_pct=$CPU_ONE_CORE idle_cpu_whole_machine_pct=$CPU_WHOLE"
  echo "handshake_p50_us=$HANDSHAKE_P50 handshake_p99_us=$HANDSHAKE_P99 handshake_p999_us=$HANDSHAKE_P999 halfwidth_pct=${P999_HW:-?}"
  echo "drain_seconds=$DRAIN_SECONDS lifecycle: live=$LIVE constructed=$CONSTRUCTED destructed=$DESTRUCTED fd=${FD_START}->${FD_END}"
  echo "upstream: accepted=${UPSTREAM_SERVED:-?} peak_clients=${UPSTREAM_PEAK:-?}"
} > "$SUMMARY"
echo
note "原始输出：$OUT/（client.log、gateway.log、upstream.log、gw-samples.log、env-snapshot.out）"
note "汇总：$SUMMARY"

if (( FAILED == 0 )); then
  [[ "$CANONIC" == "1" ]] && echo "c100k：全绿（$PAIRS 连接对，每对 ${PER_PAIR_KIB}KiB）" \
                          || echo "c100k：全绿，但**非标准口径**（conns=$CONNECTIONS targets=$TARGETS）——这份不能进 BENCHMARK.md"
else
  echo "c100k：有 ✗，见上面"
fi
exit "$FAILED"
