#!/usr/bin/env bash
# SLO #1 的 **p999 半边**：1000 并发闭环下「直连 vs 经网关」的每请求尾延迟**增量**。
#
# 为什么单独一个脚本，而不是塞进 s1-p99.sh 或 c100k.sh：
#   - s1-p99.sh 的主工具是 wrk，而 wrk 4.1.0 / hey 0.1.4 的分位表都只到 99%——p999 这一格
#     在那条线上**永远取不到数**，不是没跑，是仪器不产出那个分位（SLO.md #1「工具」条）。
#   - c100k.sh 是 SLO #2（100k 长连接持有成本）的脚本，它的口径是「握手后不再消费任何字节」。
#     本脚本用的是同一个客户端（`l7gate_c100k`）的**另一个形态**（`--requests-per-conn`，
#     闭环、逐条记每请求延迟），量的是另一个物理量。ADR-17 把这句话写死过：拿握手延迟填
#     #1 那一格就是换口径。
#   - 所以：一个脚本一个口径。SLO.md 的脚本索引里 #1 的 p999 那一行指到这里。
#
# 口径逐条抄自 docs/SLO.md 的「#1 测量条件」：同机回环 / 响应体 1 KiB / HTTP/1.1 keep-alive /
# **1000 并发连接** / 稳态 60 s / 判的是**增量**（直连与经网关各一组，两组的原始数字都落盘）。
#
# 与 wrk 那一条线的差别只有一处，而且是必须写出来的：wrk 的 1000 并发是它自己的线程模型，
# 本脚本的 1000 并发是**单线程观察者**的 epoll 循环。所以观察者自己的成本在这里是一等栏目，
# 不是脚注——两个 arm 用同一个观察者，增量才读得出「网关引入多少」；观察者的 CPU 顶到单核
# 上限时，这一轮的 p999 只能当下界（签名与判别办法写在下面的告警里，也写在 ADR-17 后果段）。
#
# 用法：
#   scripts/bench/s1-p999.sh                       # 标准口径（1000 并发 / 1 KiB / workers=1）
#   WORKERS=16 scripts/bench/s1-p999.sh            # 同一口径，只换网关的线程模型（S2-07 的 A/B）
#   CONNECTIONS=50 REQUESTS_PER_CONN=200 scripts/bench/s1-p999.sh   # 冒烟档，canonic=0，不进文档
set -uo pipefail
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1

BIN="build/release"
GATEWAY="$BIN/l7gate"
UPSTREAM="$BIN/tools/l7gate_bench_upstream"
CLIENT="$BIN/tools/l7gate_c100k"
for b in "$GATEWAY" "$UPSTREAM" "$CLIENT"; do
  [[ -x "$b" ]] || { echo "找不到 $b；先 cmake --build --preset release" >&2; exit 1; }
done

BODY_BYTES="${BODY_BYTES:-1024}"           # SLO 写死 1 KiB
CONNECTIONS="${CONNECTIONS:-1000}"         # SLO 写死 1000 并发
# 两臂各自的请求数（理由写在 run_arm 上面那段：闭环下窗口 = N ÷ 单请求延迟，而两臂的单请求延迟
# 差一个数量级）。默认值来自 2026-10-29 的标定：直连 714,285 QPS → 45,000 条 ≈ 63 s；
# 经网关 workers=1 是 23,249 QPS → 1,500 条 ≈ 65 s。workers=16 那臂更快，所以跑它时要抬 N，
# 抬到多少由窗口门（STEADY_MIN_MS）说了算，不由我觉得差不多。
REQUESTS_DIRECT="${REQUESTS_DIRECT:-45000}"
REQUESTS_PROXY="${REQUESTS_PROXY:-1500}"
# 兼容旧的单参数写法：给了 REQUESTS_PER_CONN 就两臂同用这个数（标定与冒烟用）。
REQUESTS_PER_CONN="${REQUESTS_PER_CONN:-}"
[[ -z "$REQUESTS_PER_CONN" ]] || { REQUESTS_DIRECT="$REQUESTS_PER_CONN"; REQUESTS_PROXY="$REQUESTS_PER_CONN"; }
RAMP_MS="${RAMP_MS:-10000}"                # 1000 条摊到 10 s：somaxconn=4096，别一次砸满内核队列
HANDSHAKE_TIMEOUT_MS="${HANDSHAKE_TIMEOUT_MS:-30000}"
WORKERS="${WORKERS:-1}"                    # 被测系统的属性，不是 SLO 条件（与 s1-p99.sh 同一个理由）
GATEWAY_PORT="${GATEWAY_PORT:-18084}"      # 避开 18081（s1-p99）与 18083（c100k）
# S3-01 成本 A/B 的因子位：**只给经网关那一臂**追加网关参数（例如 "--route /=0"）。
# 默认空 -> 下面 argv 的构造与 S2 时代逐字相同，所以 B-008/B-009 的可比性不作废。
# 这里做的是词拆分而不是整串一个参数：值里的空格本来就该是多个 argv 项，
# 打包成一项会被网关读成"无法识别的选项"，退化成"那一臂其实没配路由"那种安静错误。
# A/B 的两臂必须只有这一个因子不同，所以它只作用在经网关那一臂，直连臂碰不到它。
GATEWAY_EXTRA="${GATEWAY_EXTRA:-}"
# S3-02 的成本 A/B（B-013）要的两个旋钮。**两个都取缺省值时，本脚本拼出来的 argv 与
# S2/S3-01 时代逐字节相同**，所以 B-008/B-009/B-012 那批读数不作废——这条不是形式主义：
# B-012 的判据是"跨臂差不超过本臂组内极差"，只要两臂的环境被动过，那个极差就不是同一把尺了。
#   `UPSTREAM_COUNT=2`：多起一台压测上游（缺省 1）。
#   `GATEWAY_CONFIG_TEMPLATE=<路径>`：经网关那一臂不再用 `--upstream`，改成读一份由本脚本现场
#     渲染出来的 `--config`。为什么是模板而不是仓库里一份现成文件：压测上游的端口是内核选的，
#     写死在配置里的那台根本没在监听，那一臂测的就不是"转发"而是"连不上"。
#     注意 `--config` 与 `--upstream` 是互斥的（ADR-19 第一节），所以给了模板就必须**替换**掉
#     `--upstream` 而不是追加——追加的那一臂会直接退出码 2，`wait_for_line 监听` 把它判成
#     "网关起不来"，那种红至少还响；真正危险的是脚本将来改得允许两个都给，那就变成两份真相。
UPSTREAM_COUNT="${UPSTREAM_COUNT:-1}"
GATEWAY_CONFIG_TEMPLATE="${GATEWAY_CONFIG_TEMPLATE:-}"
# S3-06 的成本对照（B-016）要的旋钮：配置模板带 `health` 段时探活会往上游打进请求，
# 「direct + proxy == upstream served」的严格等式必须放宽成**有界账**——差值落在
# [probe_ok, probe_ok+probe_fail] 之内（失败的探活里 connect 没成的那些不会到上游）。
# 缺省 0：B-008/B-009/B-012/B-013 的等式语义一个字不动。
HEALTH_PROBES="${HEALTH_PROBES:-0}"
ARM_LABEL="${ARM_LABEL:-（未命名）}"
[[ "$UPSTREAM_COUNT" == "1" || "$UPSTREAM_COUNT" == "2" ]] \
  || fatal "UPSTREAM_COUNT 只支持 1 或 2：这条线要的是两臂只差一个因子，不是压上游容量"
if [[ -n "$GATEWAY_CONFIG_TEMPLATE" ]]; then
  [[ -r "$GATEWAY_CONFIG_TEMPLATE" ]] || fatal "配置模板读不到：$GATEWAY_CONFIG_TEMPLATE"
  [[ "$UPSTREAM_COUNT" == "2" ]] || note "⚠ 给了模板但 UPSTREAM_COUNT=1：模板里的 __UP2_PORT__ 会被填成 up1 的值"
fi
SETTLE="${SETTLE:-5}"                      # 工具退场后给网关收连接的时间（S2-05 起这是排水，不是杀进程）
STEADY_MIN_MS="${STEADY_MIN_MS:-60000}"    # SLO 写死「稳态采样 60 s」：达标看**实际跑出来的窗口**
REPORT_EVERY_SECONDS="${REPORT_EVERY_SECONDS:-5}"
OUT="${OUT_DIR:-build/bench/s1-p999-$(date +%Y%m%d-%H%M%S)}"

# canonic 由参数自己判，不由我嘴上说（同 s1-p99.sh 的设计）。它只覆盖「参数是不是那五条」；
# 60 s 稳态那一条由下面的 traffic_ms 门负责——两者都过，这一轮才有资格进 BENCHMARK.md。
CANONIC=1
[[ "$BODY_BYTES" == "1024" && "$CONNECTIONS" == "1000" ]] || CANONIC=0
[[ "$WORKERS" =~ ^[1-9][0-9]*$ ]] || { echo "WORKERS 要是正整数，收到 '$WORKERS'" >&2; exit 1; }

mkdir -p "$OUT" || exit 1
FAILED=0
note() { echo "  $*"; }
die() { echo "  ✗ $*" >&2; FAILED=1; }
fatal() { echo "  ✗ $*" >&2; exit 1; }
ok() { echo "  ✓ $*"; }

if [[ "$CANONIC" == "1" ]]; then
  note "口径：SLO #1 写死的那一套（${BODY_BYTES}B / ${CONNECTIONS} 并发 / 闭环每请求 / workers=$WORKERS 不是口径项）"
else
  note "口径：**非标准**（body=$BODY_BYTES conns=$CONNECTIONS）——这份只能验脚本，不能进 BENCHMARK.md"
fi

NEED_FD=$(( CONNECTIONS * 2 + 64 + WORKERS * 2 ))
HAVE_FD=$(ulimit -Sn)
if (( HAVE_FD < NEED_FD )); then
  if ! ulimit -Sn "$NEED_FD" 2>/dev/null; then
    fatal "fd 软上限 ${HAVE_FD} < 需要 ${NEED_FD} 且抬不上去：经网关那一组会在中途失败，量出来的不是延迟"
  fi
  note "fd 软上限已抬到 $(ulimit -Sn)（需要 ${NEED_FD}，hard $(ulimit -Hn)）"
fi

UP_PID=0; UP2_PID=0; G_PID=0; C_PID=0
cleanup() {
  [[ "$C_PID" != 0 ]] && kill -TERM "$C_PID" 2>/dev/null
  [[ "$G_PID" != 0 ]] && kill -TERM "$G_PID" 2>/dev/null
  [[ "$UP2_PID" != 0 ]] && kill -TERM "$UP2_PID" 2>/dev/null
  [[ "$UP_PID" != 0 ]] && kill -TERM "$UP_PID" 2>/dev/null
  return 0
}
trap cleanup EXIT INT TERM

wait_for_line() { # pid logfile pattern seconds
  local pid=$1 log=$2 pat=$3 secs=${4:-30} i=0
  while (( i < secs * 20 )); do
    grep -q -- "$pat" "$log" 2>/dev/null && return 0
    kill -0 "$pid" 2>/dev/null || { echo "  进程 $pid 提前退出，日志尾部：" >&2; tail -5 "$log" >&2; return 1; }
    sleep 0.05; ((i += 1))
  done
  return 1
}

gval() { sed -n "s/^\[serve\] $1=\([0-9-]*\).*/\1/p" "$OUT/gateway.log" | head -1; }
# 上游的账要按**键名**取：横幅与终局行都带 `accepted=`，用「最后一行匹配」会取到横幅。
# 这里取最后一个匹配（终局行在横幅之后），与 s1-p99.sh 里那个 uval 同一形状。
uval() { sed -n "s/^\[bench-upstream\].*$1=\([0-9]*\).*/\1/p" "${2:-$OUT/upstream.log}" | tail -1; }
# 两台上游时，等式与"上游有没有丢负载"都得按**两台之和**算：夹具只在退出时打终局，
# 所以每台报的是自己那一份，漏加一台就变成"等式不闭合"那种假红（B-009 那次是按单臂比
# 累计值被骗过一轮，同一个形状的错换个方向还会再犯）。
uval_sum() { # key → 所有上游日志里该键的和
  local key=$1 total=0 value log
  for log in "$OUT/upstream.log" "$OUT/upstream2.log"; do
    [[ -f "$log" ]] || continue
    value=$(uval "$key" "$log")
    total=$(( total + ${value:-0} ))
  done
  echo "$total"
}

start_upstream() {
  "$UPSTREAM" --body-bytes "$BODY_BYTES" --max-clients $((CONNECTIONS * 3 + 256)) \
    --backlog $((CONNECTIONS * 3 + 256)) > "$OUT/upstream.log" 2>&1 &
  UP_PID=$!
  wait_for_line "$UP_PID" "$OUT/upstream.log" '\[bench-upstream\] 监听' 30 || fatal "压测上游起不来（$OUT/upstream.log）"
  UP_PORT=$(sed -n 's/.*监听 127\.0\.0\.1:\([0-9]*\).*/\1/p' "$OUT/upstream.log" | head -1)
  [[ -n "$UP_PORT" ]] || fatal "读不到上游监听端口"
  note "压测上游 pid=$UP_PID port=$UP_PORT body=${BODY_BYTES}B mode=normal（不是 --upgrade：这条线量的是每请求转发）"
  if [[ "$UPSTREAM_COUNT" == "2" ]]; then
    "$UPSTREAM" --body-bytes "$BODY_BYTES" --max-clients $((CONNECTIONS * 3 + 256)) \
      --backlog $((CONNECTIONS * 3 + 256)) > "$OUT/upstream2.log" 2>&1 &
    UP2_PID=$!
    wait_for_line "$UP2_PID" "$OUT/upstream2.log" '\[bench-upstream\] 监听' 30 \
      || fatal "第二台压测上游起不来（$OUT/upstream2.log）"
    UP2_PORT=$(sed -n 's/.*监听 127\.0\.0\.1:\([0-9]*\).*/\1/p' "$OUT/upstream2.log" | head -1)
    [[ -n "$UP2_PORT" ]] || fatal "读不到第二台上游的监听端口"
    note "第二台压测上游 pid=$UP2_PID port=$UP2_PORT（两臂的上游**进程数相同**，差的只有组里有几个成员）"
  fi
}

# 跑一个 arm：$1=标签（direct|proxy） $2=targets 列表 $3=这一臂的请求数。
# 两个 arm 的**请求数不同是设计，不是偷懒**：闭环形态下稳态窗口 = N ÷ (QPS ÷ 并发)，
# 而这两臂的 QPS 差一个数量级（标定：直连 714,285、经网关 workers=1 是 23,249）。
# 用同一个 N 就让"稳态 60 s"这条 SLO 条件只能满足其中一个——所以按臂配 N，
# 让**两臂各自的窗口都 ≥60 s**，判据（分位、并发、体长、同一台机）一条都不动。
# 分位数比的是分布，不是样本条数；两臂样本各 ≥1.5M，p999 都不稀疏。
run_arm() {
  local tag=$1 targs=$2 nreq=$3
  local log="$OUT/$tag.client.log"
  note "[$tag] l7gate_c100k：$CONNECTIONS 条连接 × $nreq 条请求，ramp ${RAMP_MS}ms"
  "$CLIENT" --targets "$targs" --connections "$CONNECTIONS" --ramp-ms "$RAMP_MS" \
            --requests-per-conn "$nreq" --expect-body-bytes "$BODY_BYTES" \
            --handshake-timeout-ms "$HANDSHAKE_TIMEOUT_MS" \
            --report-every-seconds "$REPORT_EVERY_SECONDS" > "$log" 2>&1 &
  C_PID=$!
  local budget=$(( RAMP_MS / 1000 + nreq / 20 + 180 ))
  # 上限的算法要能解释：闭环下每条连接串行发 N 条，所以这一 arm 的墙上时间 ≈ ramp + N × 单请求
  # 延迟。N=1200、单请求中位 50 ms 时是 70 s；给 N/2 秒（= 单请求平均到 0.5 s）再加 120 s 余量，
  # 是「卡死了要响」而不是「慢一点就误杀」。要改就改 ARM_BUDGET 这一个念头，别改实测参数。
  wait_for_line "$C_PID" "$log" 'tag=final' "$budget" || { die "[$tag] 客户端没跑到终局（等满 ${budget}s）"; return 1; }
  wait "$C_PID"; local rc=$?; C_PID=0
  # 终局块：tag=final 起到文件尾，剔掉 target= 那些行（它们也带 established=）——同一个坑同一条修法。
  local block
  block=$(awk '/^\[c100k\] tag=final/{f=1} f && !/^\[c100k\] target=/{print}' "$log")
  aval() { awk -v k="$1" '{for (i = 1; i <= NF; ++i) {p = index($i, "="); if (p > 0 && substr($i, 1, p - 1) == k) print substr($i, p + 1)}}' <<<"$block" | tail -1; }
  R_RC=$rc
  R_ESTABLISHED=$(aval established); R_FAILED=$(aval failed)
  R_DONE=$(aval requests_done); R_PLANNED=$(aval requests_planned)
  R_TRAFFIC_MS=$(aval traffic_ms); R_RPS=$(aval requests_per_second)
  R_CPU_TICKS=$(aval self_cpu_ticks); R_ELAPSED=$(aval elapsed_ms)
  R_P50=$(aval request_p50_us); R_P99=$(aval request_p99_us)
  R_P999=$(aval request_p999_us); R_P9999=$(aval request_p9999_us)
  R_SAMPLES=$(aval request_samples); R_MEAN=$(aval request_mean_us); R_MAX=$(aval request_max_us)
  R_HALF_OK=$(aval request_p999_halfwidth_pct)
  [[ -n "$R_P999" && "$R_P999" != "missing" ]] || die "[$tag] p999 取不到（值='${R_P999:-空}'）：这一格没有读数，别用 0 冒充"
  # 观察者的自证：闭环形态下 elapsed − traffic ≈ ramp；单核占比顶到 1.0 就是「观察者算不过来」
  # 的形状（B-007 限定③那个 39% 是 100k 条握手的排队，这一档的连接数不同，但同一个读法）。
  R_OBS_CORES=$(awk -v t="${R_CPU_TICKS:-0}" -v e="${R_ELAPSED:-0}" 'BEGIN{ if (e+0 <= 0) { print "-1"; exit } printf "%.2f", t / 100.0 / (e / 1000.0) }')
  note "[$tag] rc=$R_RC established=${R_ESTABLISHED:-?}/${CONNECTIONS} failed=${R_FAILED:-?} requests=${R_DONE:-?}/${R_PLANNED:-?} 窗口=${R_TRAFFIC_MS:-?}ms QPS=${R_RPS:-?} 样本=${R_SAMPLES:-?}"
  note "[$tag] 观察者自己：self_cpu_ticks=${R_CPU_TICKS:-?} elapsed=${R_ELAPSED:-?}ms ≈ 占单核 ${R_OBS_CORES}（顶到 1.0 就别把这一轮的 p999 读成网关的上界）"
  (( R_RC == 0 )) || die "[$tag] 客户端退出码 $R_RC：这一 arm 的连接/请求没跑满，数不作数"
  [[ "${R_FAILED:-?}" == "0" ]] || die "[$tag] 客户端 failed=$R_FAILED：有请求没拿到完整响应，延迟分布量的是失败"
  [[ "${R_DONE}" == "${R_PLANNED}" ]] || die "[$tag] requests_done=$R_DONE ≠ planned=$R_PLANNED：配额没跑完，稳态窗口是假的"
  [[ "${R_TRAFFIC_MS:-0}" -ge "$STEADY_MIN_MS" ]] \
    || die "[$tag] 实际稳态窗口 ${R_TRAFFIC_MS:-?}ms < ${STEADY_MIN_MS}ms：SLO #1 写死稳态 60s，这一份不够格（抬这一臂的 N：direct 用 REQUESTS_DIRECT、proxy 用 REQUESTS_PROXY；被门判红的是取数，不是被测方）"
  if awk -v c="${R_OBS_CORES:-1}" 'BEGIN{ exit !(c+0 >= 0.9) }'; then
    note "[$tag] ⚠ 观察者占单核 ≥ 90%：p999 可能受观察者限制。读法——同一 arm 的直连/经网关若都贴到观察者上限，增量会被**压缩**，这一轮只能报下界"
  fi
}

echo "== SLO #1 · p999（release / 闭环每请求 / 直连与经网关各一组）"
start_upstream

echo "-- 组 A：直连上游（这是分母，不是对照组）"
run_arm direct "127.0.0.1:$UP_PORT" "$REQUESTS_DIRECT" || fatal "直连 arm 没跑到终局：这一轮没有分母，p999 增量不成立"
D_RC=$R_RC; D_EST=$R_ESTABLISHED; D_DONE=$R_DONE; D_TRAFFIC=$R_TRAFFIC_MS; D_RPS=$R_RPS
D_P50=$R_P50; D_P99=$R_P99; D_P999=$R_P999; D_P9999=$R_P9999; D_HW=$R_HALF_OK
D_SAMPLES=$R_SAMPLES; D_MEAN=$R_MEAN; D_OBS=$R_OBS_CORES

echo "-- 组 B：经网关转发（同一个上游、同一台机器、同样的连接数与请求数）"
GW_ARGV=("$GATEWAY" --serve --listen "127.0.0.1:$GATEWAY_PORT" --workers "$WORKERS")
if [[ -n "$GATEWAY_CONFIG_TEMPLATE" ]]; then
  # 端口现场填进模板：这一步保证"配置里那台后端"就是"脚本刚起来的那台"。
  GW_CONFIG="$OUT/gateway-config.json"
  sed -e "s/__UP1_PORT__/$UP_PORT/g" -e "s/__UP2_PORT__/${UP2_PORT:-$UP_PORT}/g" \
      "$GATEWAY_CONFIG_TEMPLATE" > "$GW_CONFIG" || fatal "渲染配置模板失败：$GATEWAY_CONFIG_TEMPLATE"
  grep -q '__UP[12]_PORT__' "$GW_CONFIG" && fatal "渲染后模板里还剩占位符：这份配置的端口是假的"
  GW_ARGV+=(--config "$GW_CONFIG")
  note "经网关那一臂：--config $GW_CONFIG（模板 $GATEWAY_CONFIG_TEMPLATE，up1=$UP_PORT up2=${UP2_PORT:-同 up1}）"
else
  GW_ARGV+=(--upstream "127.0.0.1:$UP_PORT")
fi
if [[ -n "$GATEWAY_EXTRA" ]]; then
  read -r -a GW_EXTRA_ARGS <<< "$GATEWAY_EXTRA"
  GW_ARGV+=("${GW_EXTRA_ARGS[@]}")
fi
"${GW_ARGV[@]}" > "$OUT/gateway.log" 2>&1 &
G_PID=$!
wait_for_line "$G_PID" "$OUT/gateway.log" '\[serve\] 监听' 30 || fatal "网关起不来（$OUT/gateway.log）"
# 路径签名第二道：给了配置就必须看见 `policy=on`。**没有这道门，一臂"装了配置但配置没生效"
# 和"根本没装配置"在延迟上长得一模一样**——而 B-013 比的恰恰就是这两臂的差。
if [[ -n "$GATEWAY_CONFIG_TEMPLATE" ]]; then
  grep -q '^\[serve\] policy=on' "$OUT/gateway.log" \
    || fatal "给了 --config 但横幅里没有 policy=on：这一臂其实一环都没装，两臂差的是空气"
  note "网关策略横幅：$(grep -m1 '\[serve\] policy=on' "$OUT/gateway.log")"
fi
# 路径签名当场核对：A/B 的两臂如果其实都是单 Reactor，报告里那两个 workers= 就是假话。
if (( WORKERS >= 2 )); then
  grep -q "^\[serve\] 监听 .*（workers=$WORKERS " "$OUT/gateway.log" \
    || fatal "请求 --workers $WORKERS，但网关横幅不是多 Reactor 路径：这一臂作废"
else
  grep -q "^\[serve\] 监听 .*max_connections=" "$OUT/gateway.log" \
    || fatal "请求 --workers 1，但网关横幅不是单 Reactor 路径：这一臂作废"
fi
note "网关 pid=$G_PID port=$GATEWAY_PORT workers=$WORKERS（路径签名已核对）"
run_arm proxy "127.0.0.1:$GATEWAY_PORT" "$REQUESTS_PROXY" || fatal "经网关 arm 没跑到终局：网关那一侧没有读数，这一轮作废（trap 会把两个进程都停掉）"
P_RC=$R_RC; P_EST=$R_ESTABLISHED; P_DONE=$R_DONE; P_TRAFFIC=$R_TRAFFIC_MS; P_RPS=$R_RPS
P_P50=$R_P50; P_P99=$R_P99; P_P999=$R_P999; P_P9999=$R_P9999; P_HW=$R_HALF_OK
P_SAMPLES=$R_SAMPLES; P_MEAN=$R_MEAN; P_OBS=$R_OBS_CORES

# 静置再停：同 s1-p99.sh 那条（S2-05 之后 SIGTERM 走排水，所以这三行现在验的是「排水收干净」，
# 不是「绕开一个没做的功能」——纪律五的硬核部分一个字没改）。
note "静置 ${SETTLE}s 后 SIGTERM 网关"
sleep "$SETTLE"
kill -TERM "$G_PID" 2>/dev/null
for _ in $(seq 1 200); do kill -0 "$G_PID" 2>/dev/null || break; sleep 0.1; done
wait "$G_PID" 2>/dev/null; GW_RC=$?
G_PID=0
kill -TERM "$UP_PID" 2>/dev/null; wait "$UP_PID" 2>/dev/null; UP_PID=0
if [[ "$UP2_PID" != 0 ]]; then
  kill -TERM "$UP2_PID" 2>/dev/null; wait "$UP2_PID" 2>/dev/null; UP2_PID=0
fi

LIVE=$(gval live_connections); FD0=$(gval fd_start); FD1=$(gval fd_end)
CONSTRUCTED=$(gval connections_constructed); DESTRUCTED=$(gval connections_destructed)
GW_PARSED=$(gval requests_parsed); GW_COMPLETED=$(gval responses_completed)
GW_OPENED=$(gval upstream_opened); GW_BORROWS=$(gval pool_borrows)
UP_REFUSED=$(uval_sum refused_over_capacity); UP_ADDFAIL=$(uval_sum refused_addfail)
UP_SERVED=$(uval_sum requests)
GW_WORKERS=$(gval workers); [[ -n "$GW_WORKERS" ]] || GW_WORKERS=1
(( GW_RC == 0 )) || die "网关退出码 $GW_RC：优雅停止没生效，这一轮的账不完整"
[[ "${LIVE:-x}" == "0" ]] || die "live_connections=${LIVE:-?}：排水没收干净"
[[ -n "$FD0" && "$FD0" == "$FD1" ]] || die "fd 没回基线：${FD0:-?} → ${FD1:-?}"
[[ -n "$CONSTRUCTED" && "$CONSTRUCTED" == "$DESTRUCTED" ]] || die "constructed=${CONSTRUCTED:-?} / destructed=${DESTRUCTED:-?}"
[[ "${GW_WORKERS}" == "$WORKERS" ]] || die "网关自报 workers=$GW_WORKERS ≠ 请求 $WORKERS：这一臂的线程模型不是我要的那个"
[[ "${UP_REFUSED:-x}" == "0" && "${UP_ADDFAIL:-x}" == "0" ]] \
  || die "压测上游丢了负载：容量满 ${UP_REFUSED:-?}、注册失败 ${UP_ADDFAIL:-?}——这轮的 p999 是「排队等仪器」的延迟，作废"
# ADR-17 的等式自证，但**按两臂之和**比，不是按经网关那一臂单独比：两个 arm 共用同一个上游
# 进程，而那个夹具只在退出时打印终局（跑的过程中不报进度），所以它报的 `requests=` 天然是
# direct + proxy 的累计。标定那一轮就是被这里骗到的：客户端 2,000,000、网关 2,000,000、
# 上游 4,000,000 —— 看着像"上游在重复计数"，其实是两臂各 2M。等式的正确形式是**总和**。
if [[ "${HEALTH_PROBES:-0}" == "1" ]]; then
  # B-016 的探活口径：上游 served 里混着探活打进来的请求，严格等式放宽成有界账。
  # 探活成功一定到上游（1 条请求）；探活失败只有连上了的才到上游——所以差值
  # 落在 [probe_ok, probe_ok+probe_fail]，越界即账本有问题。
  PROBES_OK=0; PROBES_FAIL=0
  while read -r k; do
    [[ -n "$k" ]] || continue
    PROBES_OK=$(( PROBES_OK + $(gval "$k") ))
  done < <(grep -o 'health_t[0-9]*_[^ ]*_probe_ok' "$OUT/gateway.log" | sort -u)
  while read -r k; do
    [[ -n "$k" ]] || continue
    PROBES_FAIL=$(( PROBES_FAIL + $(gval "$k") ))
  done < <(grep -o 'health_t[0-9]*_[^ ]*_probe_fail' "$OUT/gateway.log" | sort -u)
  EXTRA=$(( ${UP_SERVED} - ${D_DONE} - ${P_DONE} ))
  [[ ${EXTRA} -ge ${PROBES_OK} && ${EXTRA} -le $(( PROBES_OK + PROBES_FAIL )) ]] \
    || die "探活账不闭合：upstream served ${UP_SERVED} − direct ${D_DONE} − proxy ${P_DONE} = ${EXTRA}，不在 [probe_ok=${PROBES_OK}, +probe_fail=$(( PROBES_OK + PROBES_FAIL ))] 内"
  ok "等式（探活口径）：direct ${D_DONE} + proxy ${P_DONE} + 探活打进上游 ${EXTRA}（probe_ok=${PROBES_OK} / probe_fail=${PROBES_FAIL}）== upstream ${UP_SERVED}"
  [[ "${P_DONE}" == "${GW_PARSED}" && "${GW_PARSED}" == "${GW_COMPLETED}" ]] \
    || die "网关那一侧不闭合：requests_done=${P_DONE} / parsed=${GW_PARSED} / completed=${GW_COMPLETED}"
else
  [[ $(( ${D_DONE} + ${P_DONE} )) -eq ${UP_SERVED} ]] \
    || die "等式不闭合：客户端 direct=${D_DONE} + proxy=${P_DONE} = $(( ${D_DONE} + ${P_DONE} )) ≠ 上游 served=${UP_SERVED}"
  [[ "${P_DONE}" == "${GW_PARSED}" && "${GW_PARSED}" == "${GW_COMPLETED}" ]] \
    || die "网关那一侧不闭合：requests_done=${P_DONE} / parsed=${GW_PARSED} / completed=${GW_COMPLETED}"
fi
ok "生命周期与等式：live=0、fd ${FD0}→${FD1}、constructed==destructed==${CONSTRUCTED}、上游 refused=0"
ok "等式：direct ${D_DONE} + proxy ${P_DONE} == upstream ${UP_SERVED}；proxy 一侧 == 网关 parsed/completed ${GW_PARSED}/${GW_COMPLETED}"

# 增量：µs 相减再换算成 ms，只在两个读数都取到时才算。取不到就写文字，绝不写 0.000
# （「一片 0 比一片 ? 危险」是 BENCHMARK 的第一条规矩，s1-p99.sh 里那条die也是）。
INC_MS=$(awk -v a="${D_P999:-}" -v b="${P_P999:-}" 'BEGIN{ if (a=="" || b=="" || a=="missing" || b=="missing") { print ""; exit } printf "%.3f", (b - a) / 1000.0 }')
INC_P99_MS=$(awk -v a="${D_P99:-}" -v b="${P_P99:-}" 'BEGIN{ if (a=="" || b=="" ) { print ""; exit } printf "%.3f", (b - a) / 1000.0 }')

REPORT="$OUT/s1-p999.summary.txt"
{
  echo "# SLO #1 · p999 增量（s1-p999.sh）canonic=$CANONIC —— $(date -Is)"
  echo "参数: body=${BODY_BYTES}B conns=${CONNECTIONS} requests_direct=${REQUESTS_DIRECT} requests_proxy=${REQUESTS_PROXY} ramp_ms=${RAMP_MS} workers=${WORKERS}(自报 ${GW_WORKERS}) steady_min_ms=${STEADY_MIN_MS} gateway_extra=${GATEWAY_EXTRA:-（空）}"
  # B-013 这一族要能看出"两臂只差一个因子"：臂名、上游进程数、以及经网关那一臂到底是 --upstream
  # 还是 --config（连渲染出来的文件一起写进来，红了可以当场把那份文件cat一遍）。
  echo "臂: label=${ARM_LABEL} upstream_count=${UPSTREAM_COUNT} 后端来源=$([[ -n "$GATEWAY_CONFIG_TEMPLATE" ]] && echo "config(${GATEWAY_CONFIG_TEMPLATE})" || echo "upstream(单端点)") 渲染=$OUT/gateway-config.json"
  echo "网关那一侧: upstream_opened=${GW_OPENED:-?} pool_borrows=${GW_BORROWS:-?} parsed=${GW_PARSED} completed=${GW_COMPLETED}"
  echo "工具: l7gate_c100k 闭环形态（HDR 分位带误差棒）；malloc=$("$BIN/l7gate" --self-check | sed -n 's/.*malloc=\([^ ]*\).*/\1/p')；绑核：未做（同 s1-p99.sh）"
  echo "构建: 只在 release 上跑；sanitizer 下的延迟是检查开销的延迟，不是网关的延迟"
  echo
  printf '%-8s %10s %12s %12s %12s %12s %14s %10s\n' 组 p50_us p99_us p999_us p9999_us 窗口ms QPS 观察者占单核 样本
  printf '%-8s %10s %12s %12s %12s %12s %14s %10s\n' 直连 "${D_P50:-?}" "${D_P99:-?}" "${D_P999:-?}" "${D_P9999:-?}" "${D_TRAFFIC:-?}" "${D_RPS:-?}" "${D_OBS:-?}" "${D_SAMPLES:-?}"
  printf '%-8s %10s %12s %12s %12s %12s %14s %10s\n' 经网关 "${P_P50:-?}" "${P_P99:-?}" "${P_P999:-?}" "${P_P9999:-?}" "${P_TRAFFIC:-?}" "${P_RPS:-?}" "${P_OBS:-?}" "${P_SAMPLES:-?}"
  echo
  # 误差棒：两个分位各自有桶半宽，相减之后的不确定度不是简单相加——这里报的是两者半宽的
  # 平均（保守读法写在文字里），增量的绝对不确定度 = 半宽% × 各自值，读者要拿 p999 的原值换算。
  echo "p999 半宽：直连 ${D_HW:-?}% / 经网关 ${P_HW:-?}%（写在一起是因为增量两个数是不同桶宽里取的，别当成同一个精度）"
  if [[ -n "$INC_MS" ]]; then
    echo "增量 p999 = ${INC_MS} ms（经网关 − 直连，SLO 判的就是这个差值；目标 ≤ 5.0 ms）"
  else
    echo "增量 p999：**无法计算**（某一侧的 p999 没取到，见上面的 missing/?）——绝不写 0.000，0 在这个语境里等于「达标」"
  fi
  echo "增量 p99  = ${INC_P99_MS:-无法计算} ms（同一份分布的副产物，只作参照；#1 的 p99 半边仍以 wrk 那条线为准）"
  echo
  echo "等式自证: requests_done=${P_DONE} == gateway requests_parsed=${GW_PARSED} == responses_completed=${GW_COMPLETED} == upstream requests=${UP_SERVED}"
  echo "生命周期: live=${LIVE:-?} constructed=${CONSTRUCTED:-?} destructed=${DESTRUCTED:-?} fd=${FD0:-?}->${FD1:-?} gw_rc=${GW_RC} upstream refused_over_capacity=${UP_REFUSED:-?} refused_addfail=${UP_ADDFAIL:-?}"
  echo "原始输出: direct.client.log / proxy.client.log / gateway.log / upstream.log"
} > "$REPORT"

cat "$REPORT"
echo
echo "== 结果落在 $REPORT"
if (( FAILED != 0 )); then
  echo "== SLO #1 p999 本轮有 ✗，上面的数字不要用"
fi
exit "$FAILED"
