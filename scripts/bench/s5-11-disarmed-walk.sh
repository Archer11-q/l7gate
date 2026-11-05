#!/usr/bin/env bash
# S5-11：`read_disarmed_connections` 那一格 O(连接数) 遍历，在 SLO #2 那套 100k 连接对拓扑下值多少 CPU。
#
# 一条链只回答一件事：**开启指标面**（属主 worker 每秒走一遍注册表，`reactor.cc:362` →
# `connections_with_read_disarmed()`，本体 `reactor.cc:441-452`）与**关闭指标面**之间，
# 静置窗口里网关的 CPU 差是多少。两臂的参数逐字相同，只差 `--metrics-port` 这一个标志。
#
# 三件事在这个脚本里是前提，不是修饰：
#
# 一、**off/on 之差 ≠ 遍历成本**。开启那一臂同时带来：每拍 4 次 relaxed store、一根采集线程、
#     （若给抓取臂）抓取本身的解析。所以这里报的是「指标面开启的总成本」，遍历那一格由
#     `PERF=1` 那一种形态（ENVIRONMENT 4.1：由 perf 直接拉起网关）与逐线程表去归因，不许把总差值
#     直接读成"遍历值多少钱"。
#
# 二、**配对门先过，数才有口径**。`established == planned`、客户端 rc=0、`failed/accept_errors/
#     upstream_connect_failed == 0`、`downstream_accepted − established == 0`、窗口内 fd 不下降——
#     任何一条不成立就判这一轮作废（`c100k.sh` 里那三扇门同款；第二跳撞墙那一轮把空闲 CPU 读成
#     43.41% 的教训就写在那份脚本的注释里）。
#
# 三、**窗口必须落在客户端的 hold 里面**。客户端一到 `--hold-seconds` 就整体退出，那一刻是 FIN 风暴，
#     落进窗口的 CPU 是"关门"不是"空闲"。所以窗口 = 达标之后先静置 SETTLE 秒、再取 WINDOW 秒，
#     并且要求 SETTLE + WINDOW + 余量 < HOLD_SECONDS（下面按这个不等式拒绝参数）。
#
# 用法（一臂一轮）：
#   ARM=off CONNECTIONS=100000 WORKERS=1 scripts/bench/s5-11-disarmed-walk.sh
#   ARM=on  CONNECTIONS=100000 WORKERS=1 scripts/bench/s5-11-disarmed-walk.sh
#   ARM=on  SCRAPE_EVERY=5     …            # 抓取那一臂：每 5 s 真抓一次 /metrics
#   ARM=on  PERF=1             …            # 归因那一臂：由 perf 直接拉起网关
#   只在 release 上跑（sanitizer 下的 CPU/RSS 不是被测行为）。
set -uo pipefail
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1

BIN="build/release"
GATEWAY="$BIN/l7gate"
UPSTREAM="$BIN/tools/l7gate_bench_upstream"
CLIENT="$BIN/tools/l7gate_c100k"
[[ -x "$GATEWAY" ]] || { echo "找不到 $GATEWAY；先 cmake --build --preset release" >&2; exit 1; }
[[ -x "$UPSTREAM" ]] || { echo "找不到 $UPSTREAM；先 cmake --build --preset release" >&2; exit 1; }
[[ -x "$CLIENT" ]]   || { echo "找不到 $CLIENT；先 cmake --build --preset release" >&2; exit 1; }

ARM="${ARM:-on}"                        # off | on —— 唯一被差出来的那一面
CONNECTIONS="${CONNECTIONS:-100000}"
TARGETS="${TARGETS:-4}"                 # 客户端→网关的目的地址数
UPSTREAM_ENDPOINTS="${UPSTREAM_ENDPOINTS:-4}"
WORKERS="${WORKERS:-1}"                 # shipped 缺省是 1：一秒走满 10 万个桶
HOLD_SECONDS="${HOLD_SECONDS:-60}"      # 客户端达标后的空闲保持
SETTLE_SECONDS="${SETTLE_SECONDS:-10}"  # 达标之后先静置，再开窗
WINDOW_SECONDS="${WINDOW_SECONDS:-40}"  # 采样窗（CPU 差的分母）
RAMP_MS="${RAMP_MS:-120000}"
HANDSHAKE_TIMEOUT_MS="${HANDSHAKE_TIMEOUT_MS:-30000}"
DRAIN_TIMEOUT_MS="${DRAIN_TIMEOUT_MS:-30000}"
SCRAPE_EVERY="${SCRAPE_EVERY:-0}"       # 0=不抓；>0=每 N 秒 curl 一次 /metrics
PERF="${PERF:-0}"                       # 0|1
GATEWAY_PORT="${GATEWAY_PORT:-18203}"   # ENVIRONMENT 6.1 的 s5-11 那一格
METRICS_PORT="${METRICS_PORT:-18204}"
OUT="${OUT_DIR:-build/bench/s5-11-${ARM}-c${CONNECTIONS}-w${WORKERS}-$(date +%Y%m%d-%H%M%S)}"
PER_TUPLE_CEILING=28232
HZ=$(getconf CLK_TCK)

FAILED=0; CHECKS=0
UP_PID=""; G_PID=""; C_PID=""
mkdir -p "$OUT" || exit 1
note() { echo "  $*"; }
ok()   { echo "  ✓ $*"; CHECKS=$((CHECKS+1)); }
fail() { echo "  ✗ $*" >&2; CHECKS=$((CHECKS+1)); FAILED=$((FAILED+1)); }
fatal() { echo "  ✗ $*" >&2; echo "FAIL checks=$CHECKS fails=$((FAILED+1)) fatal=1" > "$OUT/verdict.txt"; exit 1; }

# --- 清场：每一条退出路径都要把自己起的三根进程收掉 --------------------------------
# 为什么这条是门而不是礼貌：一臂漏下的网关会占住 ~28k 个临时端口与 18213，
# 下一臂的夹具连 `bind :0` 都拿不到端口（实测 errno=98），于是一臂的失败会连锁污染后面全部臂。
# 顺序：客户端 → 网关 → 上游；先 TERM，20 秒不退才 KILL（KILL 会让网关退出账没了，
# 所以只在它已经不肯自己退的时候用，并在日志里点名是哪一根）。
cleanup() {
  local pid i
  for pid in "$C_PID" "$G_PID" "$UP_PID"; do
    [[ -n "$pid" ]] || continue
    kill -0 "$pid" 2>/dev/null || continue
    kill -TERM "$pid" 2>/dev/null
  done
  for pid in "$C_PID" "$G_PID" "$UP_PID"; do
    [[ -n "$pid" ]] || continue
    for (( i = 0; i < 40; ++i )); do kill -0 "$pid" 2>/dev/null || break; sleep 0.5; done
    if kill -0 "$pid" 2>/dev/null; then
      echo "  ! pid=$pid 20 秒内没退，发 KILL（残留会把下一臂的临时端口吃掉）" >&2
      kill -KILL "$pid" 2>/dev/null
    fi
  done
}
trap cleanup EXIT

# --- 参数门（全部排在 fatal 定义之后，否则门是虚的）--------------------------------
[[ "$ARM" == "off" || "$ARM" == "on" ]] || fatal "ARM 只能是 off 或 on，收到 '$ARM'"
[[ "$PERF" == "0" || "$PERF" == "1" ]] || fatal "PERF 只能是 0 或 1，收到 '$PERF'"
[[ "$SCRAPE_EVERY" =~ ^[0-9]+$ ]] || fatal "SCRAPE_EVERY 要非负整数秒，收到 '$SCRAPE_EVERY'"
[[ "$CONNECTIONS" =~ ^[1-9][0-9]*$ ]] || fatal "CONNECTIONS 要正整数，收到 '$CONNECTIONS'"
[[ "$WORKERS" =~ ^[1-9][0-9]*$ ]] || fatal "WORKERS 要正整数，收到 '$WORKERS'"
if [[ "$ARM" == "off" ]]; then
  (( SCRAPE_EVERY == 0 )) || fatal "ARM=off 时 SCRAPE_EVERY 必须是 0：没有观测端口就没处可抓，那一臂量的不是抓取成本"
  # ARM=off + PERF=1 是**对照臂**：符号级归因只能靠"同一形态两臂只差一个标志"的差额读，
  # off 臂不同法取样，on 臂榜上那一格到底是观测块还是建连阶段，就读不出来。
  (( PERF == 0 || PERF == 1 )) || fatal "PERF 只能是 0 或 1，收到 '$PERF'"
fi
if (( PERF == 1 )); then
  command -v perf >/dev/null || fatal "PERF=1 需要 perf（ENVIRONMENT 4.1 已验证只有'由 perf 直接拉起被测进程'这一种形态能用）"
fi
(( SETTLE_SECONDS + WINDOW_SECONDS + 10 <= HOLD_SECONDS )) \
  || fatal "窗口必须整个落在客户端 hold 之内：SETTLE($SETTLE_SECONDS)+WINDOW($WINDOW_SECONDS)+10 要 ≤ HOLD($HOLD_SECONDS)，否则尾部的 FIN 风暴会落进采样窗"
NEED_PER_TARGET=$(( (CONNECTIONS + TARGETS - 1) / TARGETS ))
(( NEED_PER_TARGET <= PER_TUPLE_CEILING )) || fatal "每个目标地址要扛 $NEED_PER_TARGET 条 > 实测单元组上限 $PER_TUPLE_CEILING → 加 TARGETS 或降 CONNECTIONS"
USABLE_ENDPOINTS=$WORKERS
(( UPSTREAM_ENDPOINTS < USABLE_ENDPOINTS )) && USABLE_ENDPOINTS=$UPSTREAM_ENDPOINTS
CLIENT_CAPACITY=$(( TARGETS * PER_TUPLE_CEILING ))
UPSTREAM_CAPACITY=$(( USABLE_ENDPOINTS * PER_TUPLE_CEILING ))
CAPACITY=$CLIENT_CAPACITY
(( UPSTREAM_CAPACITY < CAPACITY )) && CAPACITY=$UPSTREAM_CAPACITY
(( CONNECTIONS <= CAPACITY )) || fatal "计划 $CONNECTIONS 条 > 本轮容量 $CAPACITY（客户端侧 $CLIENT_CAPACITY / 回源侧 min(workers=$WORKERS,E=$UPSTREAM_ENDPOINTS)=$USABLE_ENDPOINTS × $PER_TUPLE_CEILING=$UPSTREAM_CAPACITY）"
FD_NEEDED=$(( CONNECTIONS * 2 + 4096 ))
ulimit -Sn "$FD_NEEDED" 2>/dev/null || note "ulimit -Sn $FD_NEEDED 没抬上去（soft=$(ulimit -n) hard=$(ulimit -Hn)）——工具自己 setrlimit，继续"
# 端口探活：被占就 die，不带着它往下跑
for P in "$GATEWAY_PORT" "$METRICS_PORT"; do
  if (exec 3<>"/dev/tcp/127.0.0.1/$P") 2>/dev/null; then exec 3>&- 3<&-; fatal "端口 $P 已被占用（s5-11 用 18203/18204，见 ENVIRONMENT 6.1）"; fi
done

SUM="$OUT/summary.txt"
: > "$SUM"
k() { echo "$1=$2" >> "$SUM"; }   # 一行一键：两个键写同一行，取数器就会把后一个读成"我之后那一整行"
k arm "$ARM"; k connections "$CONNECTIONS"; k targets "$TARGETS"
k endpoints "$UPSTREAM_ENDPOINTS"; k usable_endpoints "$USABLE_ENDPOINTS"; k workers "$WORKERS"
k hold_seconds "$HOLD_SECONDS"; k settle_seconds "$SETTLE_SECONDS"; k window_request "$WINDOW_SECONDS"
k ramp_ms "$RAMP_MS"; k scrape_every "$SCRAPE_EVERY"; k perf "$PERF"
k gateway_port "$GATEWAY_PORT"; k metrics_port "$METRICS_PORT"; k hz "$HZ"
k kernel "$(uname -r)"; k nproc "$(nproc)"
# 分配器按契约要"必须记录"：这一格从被测产物自己的 `--self-check` 取（B-007/B-021 同一口径），
# 不从我脑子里取；取不到就写读不到，不许拿"上一轮是 glibc-malloc"顶这一轮。
timeout 60 "$GATEWAY" --self-check > "$OUT/self-check.txt" 2>&1
SELF_CHECK_RC=$?
MALLOC_REPORT=$(tr ' ' '\n' < "$OUT/self-check.txt" | sed -n 's/^malloc=//p' | head -1)
k malloc "${MALLOC_REPORT:-读不到（--self-check 没打 malloc=）}"
k self_check_rc "$SELF_CHECK_RC"
k binary_gateway_sha256 "$(sha256sum "$GATEWAY" | cut -c1-16)"
k binary_client_sha256 "$(sha256sum "$CLIENT" | cut -c1-16)"
k binary_upstream_sha256 "$(sha256sum "$UPSTREAM" | cut -c1-16)"
note "本轮：ARM=$ARM n=$CONNECTIONS workers=$WORKERS 窗=${WINDOW_SECONDS}s 容量=$CAPACITY 目录=$OUT"

# --- 取数函数：读不到一律 -1，绝不拿 0 冒充"没发生" --------------------------------
gw_cpu_ticks()   { awk '{print $14 + $15}' "/proc/$1/stat" 2>/dev/null || echo -1; }
gw_rss_kib()     { awk '/^VmRSS:/{print $2; found=1} END{if(!found) print -1}' "/proc/$1/status" 2>/dev/null || echo -1; }
gw_fd_count()    { ls "/proc/$1/fd" 2>/dev/null | wc -l | tr -d ' '; }
threads_table()  { # 逐线程一行：tid comm ticks
  for d in "/proc/$1"/task/*; do
    [[ -r "$d/stat" ]] || continue
    awk '{gsub(/[()]/,"",$2); print $1, $2, $14 + $15}' "$d/stat" 2>/dev/null
  done
}

# --- 起上游（夹具）：一台进程、四个环回别名都收 -------------------------------------
wait_for_line() { local pid=$1 log=$2 pat=$3 secs=${4:-30} i=0
  while (( i < secs * 20 )); do
    grep -q -- "$pat" "$log" 2>/dev/null && return 0
    kill -0 "$pid" 2>/dev/null || { note "进程 $pid 提前退出，日志尾部：$(tail -3 "$log" 2>/dev/null)"; return 1; }
    sleep 0.05; ((i += 1))
  done; return 1; }

"$UPSTREAM" --listen "0.0.0.0:0" --upgrade --max-clients $((CONNECTIONS + 4096)) --backlog 4096 \
            > "$OUT/upstream.log" 2>&1 &
UP_PID=$!
wait_for_line "$UP_PID" "$OUT/upstream.log" 'bench-upstream\] 监听' 30 || fatal "压测上游起不来（$OUT/upstream.log）"
UP_PORT=$(sed -n 's/.*监听 0\.0\.0\.0:\([0-9]*\).*/\1/p' "$OUT/upstream.log" | head -1)
[[ -n "$UP_PORT" ]] || fatal "读不到上游监听端口"
ok "上游夹具 pid=$UP_PID port=$UP_PORT（--upgrade：回 101 后不再管帧）"

SPEC=""
for (( i = 1; i <= UPSTREAM_ENDPOINTS; ++i )); do SPEC="${SPEC:+$SPEC,}127.0.0.$i:$UP_PORT"; done
TADDRS=""
for (( i = 0; i < TARGETS; ++i )); do TADDRS="${TADDRS:+$TADDRS,}127.0.0.$((i + 1)):$GATEWAY_PORT"; done

# --- 起网关：两臂只差 --metrics-port（PERF=1 时由 perf 直接拉起）-------------------
PER_T=$( (date +%s%N) 2>/dev/null || echo 0 )
GW_ARGS=(--serve --listen "0.0.0.0:$GATEWAY_PORT" --upstream "$SPEC" --workers "$WORKERS"
         --max-connections $(( (CONNECTIONS / WORKERS) * 2 + 512 )) --drain-timeout-ms "$DRAIN_TIMEOUT_MS")
[[ "$ARM" == "on" ]] && GW_ARGS+=(--metrics-port "$METRICS_PORT")
if (( PERF == 1 )); then
  PERF_BIN=$(command -v perf)
  ( "$PERF_BIN" record -e cpu-clock -F 99 -g -o "$OUT/perf.data" -- "$GATEWAY" "${GW_ARGS[@]}" ) \
      > "$OUT/gateway.log" 2>&1 &
else
  "$GATEWAY" "${GW_ARGS[@]}" > "$OUT/gateway.log" 2>&1 &
fi
G_PID=$!
PERF_WRAP=""
wait_for_line "$G_PID" "$OUT/gateway.log" '\[serve\] 监听' 30 || fatal "网关起不来（$OUT/gateway.log）"
if (( PERF == 1 )); then
  # `perf record -- 被测进程` 这个形状下 `$!` 是 **perf 自己**。上一版我就这么拿 /proc/<$!>/stat 的
  # utime+stime 当网关的读数，报出 cpu_us_per_second=0.0——**仪器打的是别人的账**（§26.4 第 3/4 条那一族）。
  # 被测方是 perf 的子进程：按父子关系取那一根 pid；取不到就判这一档作废，不交一份空数。
  REAL=$(pgrep -P "$G_PID" 2>/dev/null | head -1)
  [[ -n "$REAL" ]] || fatal "PERF=1 但读不到 perf 的子进程 pid：这一档的 CPU 归不了被测方，作废"
  PERF_WRAP=$G_PID; G_PID=$REAL
  ok "PERF 臂：perf pid=$PERF_WRAP 之下的被测网关 pid=$G_PID（CPU 与退出账都从这一根读）"
fi
ok "网关 pid=$G_PID workers=$WORKERS arm=$ARM 每 worker 预算 $(( (CONNECTIONS / WORKERS) * 2 + 512 )) 个连接位"

RSS_EMPTY=$(gw_rss_kib "$G_PID")
note "空进程基线 VmRSS=${RSS_EMPTY}KiB"

# --- 客户端：建满 → 静置 → 开窗 ----------------------------------------------------
"$CLIENT" --targets "$TADDRS" --connections "$CONNECTIONS" --ramp-ms "$RAMP_MS" \
          --hold-seconds "$HOLD_SECONDS" --handshake-timeout-ms "$HANDSHAKE_TIMEOUT_MS" \
          --report-every-seconds 10 > "$OUT/client.log" 2>&1 &
C_PID=$!
pval() { local lp; lp=$(grep '^\[c100k\] tag=progress' "$OUT/client.log" | tail -1)
  awk -v k="$1" '{for (i = 1; i <= NF; ++i) {p = index($i, "="); if (p > 0 && substr($i, 1, p - 1) == k) print substr($i, p + 1)}}' <<<"$lp" | tail -1; }
DEADLINE=$(( RAMP_MS / 1000 + HANDSHAKE_TIMEOUT_MS / 1000 + 120 ))
EST=""
for (( i = 0; i < DEADLINE * 2; ++i )); do
  kill -0 "$C_PID" 2>/dev/null || break
  E=$(pval established); F=$(pval failed)
  [[ "$E" == "$CONNECTIONS" && "$F" == "0" ]] && { EST="$E"; break; }
  sleep 0.5
done
[[ -n "$EST" ]] || fatal "没等到 established==$CONNECTIONS 且 failed=0（最后一条进度行：$(grep 'tag=progress' "$OUT/client.log" | tail -1)）→ 这一轮量的不是空闲拓扑"
ok "连接对达标：established=$EST / planned=$CONNECTIONS"

sleep "$SETTLE_SECONDS"
kill -0 "$C_PID" 2>/dev/null || fatal "静置期间客户端就退出了（$OUT/client.log）"
T0_TICKS=$(gw_cpu_ticks "$G_PID"); T0_TS=$(date +%s)
threads_table "$G_PID" > "$OUT/threads.t0.txt"
[[ "$T0_TICKS" != "-1" ]] || fatal "读不到网关 CPU tick（窗口起点）"
note "开窗：ticks=$T0_TICKS ts=$T0_TS fd=$(gw_fd_count "$G_PID") rss=$(gw_rss_kib "$G_PID")KiB"

SCRAPES_OK=0; SCRAPES_FAIL=0
: > "$OUT/scrape.log"
# 观测端口的存活只在窗口两端各查一次，不在窗内每 2 s 查一次——窗口里每一次 connect
# 都是仪器在被测方身上做事，off 臂没有对应的动作，那样会把探针成本混进差值。
if [[ "$ARM" == "on" ]]; then
  (exec 3<>"/dev/tcp/127.0.0.1/$METRICS_PORT") 2>/dev/null || fatal "开窗之前观测端口 $METRICS_PORT 就连不上：这一臂的仪器前提不成立"
  exec 3>&- 3<&-
  ok "窗口起点：观测端口 $METRICS_PORT 可连"
fi
END_TS=$(( T0_TS + WINDOW_SECONDS ))
while :; do
  NOW=$(date +%s)
  (( NOW >= END_TS )) && break
  if [[ "$ARM" == "on" ]] && (( SCRAPE_EVERY > 0 )); then
    if curl -s -m 5 -o "$OUT/last-scrape.txt" "http://127.0.0.1:$METRICS_PORT/metrics"; then
      SCRAPES_OK=$((SCRAPES_OK+1)); echo "$NOW ok" >> "$OUT/scrape.log"
    else SCRAPES_FAIL=$((SCRAPES_FAIL+1)); echo "$NOW fail" >> "$OUT/scrape.log"; fi
    SAMP=$(awk '/^read_disarmed_connections /{print $2; found=1} END{if(!found) print "KEY_ABSENT"}' "$OUT/last-scrape.txt" 2>/dev/null || echo NA)
    echo "scrape_last_read_disarmed=$SAMP" >> "$SUM"
    REMAIN=$(( END_TS - $(date +%s) ))
    sleep "$(( SCRAPE_EVERY < REMAIN ? SCRAPE_EVERY : (REMAIN > 0 ? REMAIN : 1) ))"
  else
    sleep 2
  fi
done
if [[ "$ARM" == "on" ]]; then
  (exec 3<>"/dev/tcp/127.0.0.1/$METRICS_PORT") 2>/dev/null && { exec 3>&- 3<&-; ok "窗口终点：观测端口仍可连（静置窗口里网关没被自己的仪器拖死）"; } \
    || fail "窗口终点观测端口 $METRICS_PORT 连不上：这一轮的 on 臂中途塌了，读数不可用"
fi
T1_TICKS=$(gw_cpu_ticks "$G_PID"); T1_TS=$(date +%s)
threads_table "$G_PID" > "$OUT/threads.t1.txt"
[[ "$T1_TICKS" != "-1" ]] || fatal "读不到网关 CPU tick（窗口终点）"
WALL=$(( T1_TS - T0_TS ))
kill -0 "$C_PID" 2>/dev/null || fail "客户端在窗口结束之前就退出了：尾部的 FIN 风暴可能落进窗口"
(( WALL >= WINDOW_SECONDS - 5 )) || fail "窗口只有 ${WALL}s < ${WINDOW_SECONDS}s（差 5s 容差）：分母不成立，这一轮不进结论"

# --- 逐线程归因（哪一根线程在动）---------------------------------------------------
# 不用 join：join 要求两边按同一字典序排过，而我这里第一列是数字 tid，`sort -k1,1n` 的数值序
# 会让 join 报 "not in order"。awk 两遍读，键取 tid，缺哪一边都记下来而不是丢掉。
awk -v hz="$HZ" '
  NR == FNR { t0[$1] = $3 + 0; name[$1] = $2; seen[$1] = 1; next }
  { d = ($3 + 0) - (seen[$1] ? t0[$1] : 0); if (!seen[$1]) name[$1] = $2 "(窗口内新建)"
    if (d != 0) printf "tid=%s comm=%s dticks=%d dseconds=%.4f\n", $1, name[$1], d, d / hz }
  { t1[$1] = $3 + 0 }
  END { for (k in t0) if (!(k in t1)) printf "tid=%s comm=%s dticks=?（窗口内消失，终点读不到）\n", k, name[k] }
' "$OUT/threads.t0.txt" "$OUT/threads.t1.txt" > "$OUT/threads.delta.txt"
DT_SUM=$(( T1_TICKS - T0_TICKS ))
note "窗口 ${WALL}s：整进程 ticks $T0_TICKS→$T1_TICKS（Δ=$DT_SUM，Δs=$(awk -v d="$DT_SUM" -v h="$HZ" 'BEGIN{printf "%.4f", d/h}')）；逐线程非零差值 $(wc -l < "$OUT/threads.delta.txt" | tr -d ' ') 根"
k window_seconds "$WALL"
k cpu_ticks_delta "$DT_SUM"
k cpu_seconds_delta "$(awk -v d="$DT_SUM" -v h="$HZ" 'BEGIN{printf "%.4f", d/h}')"
k cpu_one_core_pct "$(awk -v d="$DT_SUM" -v h="$HZ" -v w="$WALL" 'BEGIN{if(w>0) printf "%.4f", d*100.0/(h*w); else print -1}')"
k cpu_us_per_second "$(awk -v d="$DT_SUM" -v h="$HZ" -v w="$WALL" 'BEGIN{if(w>0) printf "%.1f", d*1000000.0/(h*w); else print -1}')"
k cpu_ns_per_pair_per_second "$(awk -v d="$DT_SUM" -v h="$HZ" -v w="$WALL" -v n="$CONNECTIONS" 'BEGIN{if(w>0 && n>0) printf "%.1f", d*1000000000.0/(h*w*n); else print -1}')"
k scrapes_ok "$SCRAPES_OK"; k scrapes_fail "$SCRAPES_FAIL"
k threads_with_nonzero_delta "$(wc -l < "$OUT/threads.delta.txt" | tr -d " ")"

# --- 收尾：客户端先退、再 SIGTERM 网关，退出账从日志取（不硬杀）---------------------
wait "$C_PID" 2>/dev/null; CLIENT_RC=$?
EST=$(pval established); FAILED_CONNS=$(pval failed); CLIENT_END_TS=$(date +%s)
k client_rc "$CLIENT_RC"; k established "$EST"; k client_failed "$FAILED_CONNS"
kill -TERM "$G_PID" 2>/dev/null
for (( i = 0; i < 120; ++i )); do kill -0 "$G_PID" 2>/dev/null || break; sleep 0.5; done
if kill -0 "$G_PID" 2>/dev/null; then note "网关 60s 内没退出，记录后再看（不硬杀，退出账要留着）"; else ok "网关自己退出（rc 见 gateway.log 末尾）"; fi
gval() { sed -n "s/^\[serve\] $1=\([0-9-]*\).*/\1/p" "$OUT/gateway.log" | head -1; }
LIVE=$(gval live_connections); CONSTRUCTED=$(gval connections_constructed); DESTRUCTED=$(gval connections_destructed)
DOWN_ACCEPTED=$(gval downstream_accepted); UP_CONNECT_FAILED=$(gval upstream_connect_failed)
ACCEPT_ERRORS=$(gval accept_errors); OVER_CAPACITY=$(gval downstream_rejected_over_capacity)
DISARMED_EXIT=$(gval read_disarmed_connections); FD_START=$(gval fd_start); FD_END=$(gval fd_end)
for kv in live_connections connections_constructed connections_destructed downstream_accepted \
          upstream_connect_failed accept_errors downstream_rejected_over_capacity \
          read_disarmed_connections fd_start fd_end requests_parsed responses_completed; do
  echo "$kv=$(gval "$kv")" >> "$SUM"
done
kill -TERM "$UP_PID" 2>/dev/null; wait "$UP_PID" 2>/dev/null
k upstream_accepted_by_addr "$(grep -o "accepted_by_addr=[^ ]*" "$OUT/upstream.log" | tail -1 | cut -d= -f2)"
k upstream_requests_last "$(grep -o "requests=[0-9]*" "$OUT/upstream.log" | tail -1 | cut -d= -f2)"

# --- 判定（可断言的句子，一条一次计数）---------------------------------------------
[[ "$EST" == "$CONNECTIONS" ]] && ok "established=$EST 等于计划 $CONNECTIONS" || fail "established=$EST ≠ 计划 $CONNECTIONS"
(( CLIENT_RC == 0 )) && ok "客户端 rc=0" || fail "客户端 rc=$CLIENT_RC"
[[ "$FAILED_CONNS" == "0" ]] && ok "客户端 failed=0" || fail "客户端 failed=$FAILED_CONNS"
[[ "${ACCEPT_ERRORS:-x}" == "0" ]] && ok "accept_errors=0" || fail "accept_errors=${ACCEPT_ERRORS:-读不到}"
[[ "${OVER_CAPACITY:-x}" == "0" ]] && ok "downstream_rejected_over_capacity=0" || fail "downstream_rejected_over_capacity=${OVER_CAPACITY:-读不到}"
[[ "${UP_CONNECT_FAILED:-x}" == "0" ]] && ok "upstream_connect_failed=0" || fail "upstream_connect_failed=${UP_CONNECT_FAILED:-读不到}"
UNPAIRED=$(( ${DOWN_ACCEPTED:-0} - ${EST:-0} ))
(( UNPAIRED == 0 )) && ok "downstream_accepted − established = 0（每对下游都配上了上游）" \
                   || fail "未配对下游 $UNPAIRED 条：进分子不进分母，本轮的每连接成本没有口径"
[[ "${CONSTRUCTED:-x}" == "${DESTRUCTED:-x}" && "${CONSTRUCTED:-x}" != "x" ]] && ok "配对账 constructed==destructed=$CONSTRUCTED" \
  || fail "配对账不等（constructed=${CONSTRUCTED:-?} destructed=${DESTRUCTED:-?}）"
[[ "${LIVE:-x}" == "0" ]] && ok "live_connections=0（收尾干净）" || fail "live_connections=${LIVE:-读不到}"
if [[ "$ARM" == "on" ]]; then
  [[ "${DISARMED_EXIT:-x}" =~ ^[0-9]+$ ]] && ok "退出账里 read_disarmed_connections 键存在且取值 $DISARMED_EXIT（空闲态应为 0，但遍历照样付——0 不等于没走）" \
    || fail "读不到 read_disarmed_connections 那一格"
  if [[ "$(grep -c '^read_disarmed_connections ' "$OUT/last-scrape.txt" 2>/dev/null)" == "0" ]] && [[ "$SCRAPE_EVERY" == "0" ]]; then
    note "抓取臂关着（SCRAPE_EVERY=0）：运行期那一格只在退出账里读"
  fi
else
  ok "ARM=off：没给 --metrics-port，运行期一个指标键都不发（退出照旧有 read_disarmed_connections 那一行）"
fi
if (( PERF == 1 )); then
  if [[ -s "$OUT/perf.data" ]]; then
    ok "perf.data 落盘 $(wc -c < "$OUT/perf.data" | tr -d ' ') 字节"
  else
    fail "perf.data 空/没落盘"
  fi
  # 榜要在这里就落盘：报告脚本不该依赖"谁后来手动跑一次 perf report"
  if command -v perf >/dev/null; then
    timeout 600 perf report -i "$OUT/perf.data" --stdio --no-children -g none > "$OUT/perf.report.txt" 2>/dev/null
    [[ -s "$OUT/perf.report.txt" ]] || fail "perf.report.txt 空：这一臂没有榜"
  fi
  # perf 打的样本是 "1K of event" 这种带单位的写法。上一版 sed 只吃数字，把 "1K" 读成 1，
  # 于是那一臂（榜上原样写 Samples: 1K）被我自己判成"样本不足只作旁证"——取数器把单位吞了（§25.2 那一族）。
  SAMPLES_RAW=$(sed -n 's/.*Samples: \([0-9][0-9]*[kKmM]?\).*/\1/p' "$OUT/perf.report.txt" 2>/dev/null | head -1)
  SAMPLES=$(awk -v v="${SAMPLES_RAW:-0}" 'BEGIN{ u=""; if (v ~ /[kK]$/) u="k"; if (v ~ /[mM]$/) u="m"; gsub(/[kKmM]/,"",v); n=v+0; if (u=="k") n*=1000; if (u=="m") n*=1000000; printf "%.0f", n }')
  # 符号级那一层：`connections_with_read_disarmed` 在 release 里是**真符号**（nm 两条 `T`），
  # 但循环里那一次调用被内联进 `Reactor::run()`，所以它自己不上榜——归因读的是 run() 那一格与对照臂的差。
  DIS=$(grep -m1 -o '[0-9.]*%.*connections_with_read_disarmed' "$OUT/perf.report.txt" 2>/dev/null | sed 's/%.*//')
  RUNP=$(sed -n 's/^ *\([0-9.]*\)%.*l7gate::net::Reactor::run().*/\1/p' "$OUT/perf.report.txt" 2>/dev/null | head -1)
  k perf_samples_raw "${SAMPLES_RAW:-读不到}"
  k perf_samples "${SAMPLES:-NA}"
  k perf_run_self_pct "${RUNP:-没上榜}"
  k perf_disarmed_self_pct "${DIS:-没上榜}"
  k perf_top "$(grep -m1 -E '^ +[0-9.]+%' "$OUT/perf.report.txt" 2>/dev/null | tr -s ' ' | cut -c1-100)"
  note "perf 榜：样本 ${SAMPLES_RAW:-读不到}（换算 ${SAMPLES:-?}）· run() self=${RUNP:-没上榜}% · disarmed 符号 self=${DIS:-没上榜}%"
  [[ -n "$RUNP" ]] || fail "榜上没有 Reactor::run() 那一格：这一臂归不了因"
fi

VERDICT=PASS; (( FAILED == 0 )) || VERDICT=FAIL
echo "$VERDICT checks=$CHECKS fails=$FAILED" > "$OUT/verdict.txt"
note "判定落盘 $OUT/verdict.txt（$VERDICT checks=$CHECKS fails=$FAILED）"
[[ "$VERDICT" == "PASS" ]] || exit 1
exit 0
