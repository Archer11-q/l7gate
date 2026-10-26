#!/usr/bin/env bash
# SLO #1「转发尾延迟」的第一份取数脚本。口径逐条抄自 docs/SLO.md 的「#1 测量条件」，
# 写死在这里而不是每次手调——手调一次的口径就漂移一次，三个数字也就作废一次。
#
#   同机回环 / 响应体 1 KiB / HTTP/1.1 keep-alive / 1000 并发连接 / 稳态 60 s /
#   wrk --latency 为主、hey 交叉验证 / **直连与经网关各一组**
#
# 为什么必须有"直连那一组"：SLO 判的是**增量**，而增量的分母是我自己的上游。
# 只报"经网关 p99 = X ms"是把上游和 wrk 的开销一起算在我头上，那个数既不好看
# 也不能归因。差值才是网关引入的，两组的原始数字也都必须落盘（SLO 里那句
# "最容易造假的点在于和谁比"就是这条）。
#
# 上游用 tools/bench_upstream_main.cc，不用 tests/ 那个：后者 poll 单线程、
# 上限 32 条连接，1000 并发下它拒绝的是连接而不是延迟问题。
#
# 只在 release 上跑。debug 与 sanitizer 下的延迟数字不是延迟数字（ENVIRONMENT 2.2：
# 插桩会改变被测行为本身），这里对 PRESET 直接拒绝。
#
# 用法：scripts/bench/s1-p99.sh                    # 正式口径：预热 10s + 稳态 60s × 两组
#       DURATION=5s WARMUP=1s scripts/bench/s1-p99.sh   # 只用来验证脚本本身，
#       # 输出会在文件头写「非标准口径」，别把它抄进 BENCHMARK.md
set -uo pipefail
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1

BIN="build/release"
GATEWAY="$BIN/l7gate"
UPSTREAM="$BIN/tools/l7gate_bench_upstream"
GATEWAY_PORT="${GATEWAY_PORT:-18081}"
BODY_BYTES="${BODY_BYTES:-1024}"          # SLO 写死 1 KiB
CONNECTIONS="${CONNECTIONS:-1000}"        # SLO 写死 1000 并发
DURATION="${DURATION:-60s}"               # SLO 写死稳态 60 s
WARMUP="${WARMUP:-10s}"                   # 预热不计入
WRK_THREADS="${WRK_THREADS:-8}"
HEY_DURATION="${HEY_DURATION:-30s}"
HEY_CONNS="${HEY_CONNS:-$CONNECTIONS}"
SETTLE="${SETTLE:-5}"                      # 工具退场后给网关收尾连接的静置时间（秒）
MAX_CLIENTS_NOTE=$((CONNECTIONS * 3 + 256))  # 压测上游容量：留跨轮回收的余量，见 start_upstream
OUT="${OUT_DIR:-build/bench/s1-p99-$(date +%Y%m%d-%H%M%S)}"

# 是不是标准口径，由这几个值自己决定，不由我嘴上说。
CANONIC=1
[[ "$BODY_BYTES" == "1024" && "$CONNECTIONS" == "1000" && "$DURATION" == "60s" &&
   "$WARMUP" == "10s" ]] || CANONIC=0

mkdir -p "$OUT" || exit 1
FAILED=0
die() { echo "✗ $*" >&2; FAILED=1; }
# fatal：断到"这轮数字没有资格进文档"级别的错。和 die 的区别是它真的停——
# 一个 fd 没回基线的 p99 报告继续往下生成，就会有人把这份报告抄进 BENCHMARK.md。
fatal() { echo "✗ $*" >&2; exit 1; }
note() { echo "  $*"; }

[[ "$CANONIC" == "1" ]] && note "口径：SLO #1 写死的那一套（1KiB / 1000 并发 / 60s 稳态 / 10s 预热）" \
  || note "口径：**非标准**（body=$BODY_BYTES conns=$CONNECTIONS duration=$DURATION warmup=$WARMUP）——这份结果不能进 BENCHMARK.md"

[[ -x "$GATEWAY" ]] || { echo "找不到 $GATEWAY；先 cmake --build --preset release" >&2; exit 1; }
[[ -x "$UPSTREAM" ]] || { echo "找不到 $UPSTREAM；先 cmake --build --preset release" >&2; exit 1; }
command -v wrk >/dev/null || { echo "本机没有 wrk：SLO #1 的主测量工具" >&2; exit 1; }
command -v hey >/dev/null || { echo "本机没有 hey：SLO #1 的交叉验证工具" >&2; exit 1; }

# 经网关那一组要 2×并发 个 fd（一条客户端连接配一条回源连接），加上监听/epoll/eventfd。
NEED_FD=$(( CONNECTIONS * 2 + 64 ))
HAVE_FD=$(ulimit -n)
note "wrk: $(wrk --version 2>&1 | head -1)"
note "hey: $(hey 2>&1 | grep -m1 -E 'hey version|^Usage' || true)"
note "ulimit -n = ${HAVE_FD}，网关需要 ≥ ${NEED_FD}"
if (( HAVE_FD < NEED_FD )); then
  echo "文件描述符上限不够：经网关那一组会在第 ${HAVE_FD} 条连接上失败，跑出来的不是延迟数字。" >&2
  echo "先 ulimit -n ${NEED_FD}（本机默认 10240，够 1000 并发；不够时别硬跑）" >&2
  exit 1
fi

UP_PID=0; G_PID=0
cleanup() {
  [[ "$G_PID" != 0 ]] && kill -TERM "$G_PID" 2>/dev/null
  [[ "$UP_PID" != 0 ]] && kill -TERM "$UP_PID" 2>/dev/null
  return 0
}
trap cleanup EXIT INT TERM

wait_for_line() { # pid logfile pattern [seconds]
  local pid=$1 log=$2 pat=$3 secs=${4:-20} i=0
  while (( i < secs * 20 )); do
    if grep -q -- "$pat" "$log" 2>/dev/null; then return 0; fi
    if ! kill -0 "$pid" 2>/dev/null; then
      echo "  进程 $pid 已退出，日志尾部：" >&2; tail -5 "$log" >&2; return 1
    fi
    sleep 0.05; ((i += 1))
  done
  return 1
}

start_upstream() {
  # 容量要留**跨轮的余量**，不是刚好等于并发数：上一轮（wrk 预热）的连接在工具
  # 退出后才被事件批逐个回收，下一轮（wrk 稳态）可能已经带着 1000 条新连接进来了。
  # 首轮 refused_over_capacity=20 就是这么来的：--max-clients 恰好写了 CONNECTIONS+64。
  # 上游按定义不能成为瓶颈，否则量到的 p99 是"排队等仪器"的延迟。
  "$UPSTREAM" --body-bytes "$BODY_BYTES" --max-clients $((CONNECTIONS * 3 + 256)) \
    --backlog $((CONNECTIONS * 3 + 256)) > "$OUT/upstream.log" 2>&1 &
  UP_PID=$!
  wait_for_line "$UP_PID" "$OUT/upstream.log" '\[bench-upstream\] 监听' || { die "压测上游起不来"; exit 1; }
  UP_PORT=$(sed -n 's/.*监听 127\.0\.0\.1:\([0-9]*\).*/\1/p' "$OUT/upstream.log" | head -1)
  note "压测上游 pid=$UP_PID port=$UP_PORT body=$BODY_BYTES"
}

start_gateway() {
  "$GATEWAY" --serve --listen "127.0.0.1:$GATEWAY_PORT" --upstream "127.0.0.1:$UP_PORT" \
    > "$OUT/gateway.log" 2>&1 &
  G_PID=$!
  if ! wait_for_line "$G_PID" "$OUT/gateway.log" '\[serve\] 监听'; then
    die "网关起不来"; exit 1
  fi
  note "网关 pid=$G_PID port=$GATEWAY_PORT"
}

# wrk 跑一轮：预热一遍（结果丢掉）+ 正式一遍（存盘）。
# 预热的理由不是"让数字好看"：第一批连接要走 TCP 建连、网关要现建连接对象、
# 分配器要向内核要页——那些是一次性成本，混进 60s 的分布里，p99 测的就是启动。
run_wrk() { # 标签 url  —— 只把 QPS 写到 stdout，其它一律走 stderr：
  local tag=$1 url=$2     # 这个函数要被 $( ) 捕获，多一行输出就等于把进度日志
                         # 当成数字抄进报告，那是最像"成功"的一种错。
  echo "  [$tag] 预热 $WARMUP ..." >&2
  wrk -t"$WRK_THREADS" -c"$CONNECTIONS" -d"$WARMUP" "$url" > "$OUT/$tag.warmup.txt" 2>&1
  echo "  [$tag] 稳态 $DURATION ..." >&2
  wrk -t"$WRK_THREADS" -c"$CONNECTIONS" -d"$DURATION" --latency "$url" > "$OUT/$tag.wrk.txt" 2>&1
  # 非 2xx 与 socket 错误必须为 0：那说明这条数字量的是"失败请求的耗时"，不是延迟。
  local non2xx errs
  non2xx=$(sed -n 's/.*Non-2xx or 3xx responses: \([0-9]*\).*/\1/p' "$OUT/$tag.wrk.txt")
  errs=$(sed -n 's/^Socket errors: \(.*\)$/\1/p' "$OUT/$tag.wrk.txt")
  [[ -z "$non2xx" ]] && non2xx=0
  if (( non2xx != 0 )) || [[ -n "$errs" ]]; then
    die "[$tag] 有非 2xx（${non2xx}）或 socket 错误（${errs:-无}）：这轮的延迟数字作废"
  fi
  local qps
  qps=$(sed -n 's/^Requests\/sec:[[:space:]]*\([0-9.]*\).*/\1/p' "$OUT/$tag.wrk.txt" | head -1)
  [[ -z "$qps" ]] && die "[$tag] wrk 的 Requests/sec 没解析出来：版式变了，看 $OUT/$tag.wrk.txt"
  echo "$qps"
}

# 从 wrk 的 Latency Distribution 里取一个分位，统一换算成**毫秒**。
# 不换算的话 "900us" 与 "1.20ms" 会被字符串比较当成同一个量级——那正是
# 一个能把 10 倍差异读成"基本持平"的错。
wrk_percentile_ms() { # 文件 标签(50%|75%|90%|99%)
  # 版式（本机 wrk debian/4.1.0-4build3 实测，照抄 raw 输出）：
  #   "  Latency Distribution"          ← **整行缩进两个空格**
  #   "     50%   26.06ms"              ← 5 空格 + 百分比 + 值
  #   "  188553 requests in 5.07s, ..." ← 同样以空格开头：只靠"有缩进"认行不够，
  #                                       必须认 $1 长成 "数字%" 的样子。
  # 第一版在这里返回空——锚写成 /^Latency Distribution/，而真实那行前面有两个空格，
  # 于是 p50…p99 全取不到、增量被算成 0.000ms。报告里一片 0 比一片 ? 危险：
  # 0 看起来像"网关没引入延迟"。
  awk -v want="$2" '
    /^[ \t]*Latency Distribution[ \t]*$/ { inblock = 1; next }
    inblock {
      if ($1 !~ /^[0-9]+(\.[0-9]+)?%$/) next
      if ($1 != want) next
      val = $2
      unit = "ms"
      if (val ~ /us$/) { unit = "us"; sub(/us$/, "", val) }
      else if (val ~ /ms$/) { unit = "ms"; sub(/ms$/, "", val) }
      else if (val ~ /s$/)  { unit = "s";  sub(/s$/, "", val) }
      v = val + 0
      if (unit == "us") v = v / 1000.0
      else if (unit == "s") v = v * 1000.0
      printf "%.3f\n", v
      exit
    }
  ' "$1"
}

run_hey() { # 标签 url —— hey 只做交叉验证：它的连接模型与 wrk 不同（它按请求数采样），
           # 两个工具给出的 p99 落在同一量级，才说明那个量级不是某个工具的形状。
  local tag=$1 url=$2
  echo "  [$tag] hey 交叉验证 $HEY_DURATION × ${HEY_CONNS} 连接 ..." >&2
  hey -z "$HEY_DURATION" -c "$HEY_CONNS" "$url" > "$OUT/$tag.hey.txt" 2>&1
  local errs non2xx
  # hey 的版式（本机 0.1.4 实测）是两段：
  #   Status code distribution:
  #     [200] 621432 responses
  #   Error distribution:
  #     [523086]  Get "http://..." : dial tcp: connect: connection refused
  # 我要的是"非 2xx 的总数"与"错误总数"。按段名匹配而不是按某一行文本匹配：
  # 抄一行样例进去，hey 改版式的下一次就是静默读空（读空 = 0 = "通过"），
  # 那是最容易骗到自己的一种失败形状。
  non2xx=$(awk '
    /^Status code distribution:/ { codes = 1; errs = 0; next }
    /^Error distribution:/ { codes = 0; errs = 1; next }
    /^[A-Za-z]/ { codes = 0; errs = 0 }
    {
      gsub(/[\[\]]/, "", $1);           # "[200]" / "[523086]" 去掉方括号才好换算数
      if (codes && $1 !~ /^2/) bad += $2;
      if (errs) errsum += $1;
    }
    END { printf "%d %d\n", bad + 0, errsum + 0 }' "$OUT/$tag.hey.txt")
  non2xx=${non2xx%% *}
  errs=${non2xx##* }
  [[ -z "$non2xx" ]] && non2xx=0
  [[ -z "$errs" ]] && errs=0
  if [[ "$errs" != "0" || "$non2xx" != "0" ]]; then
    die "[$tag] hey 有失败（错误=${errs} 非2xx=${non2xx}）：交叉验证这一轮作废"
  fi
  # hey 的这行是 **tab** 分隔（"  Requests/sec:\t20287.0084"），不是空格：
  # sed 里只写 [ ]* 会得到空串，报告里就是一行 QPS=? ——而空串永远"不算失败"。
  # [[:space:]] 同时吃 tab 和空格，读不到时报错而不是静默留空。
  local qps
  qps=$(sed -n 's/^[[:space:]]*Requests\/sec:[[:space:]]*\([0-9.]*\).*/\1/p' "$OUT/$tag.hey.txt" | head -1)
  [[ -z "$qps" ]] && die "[$tag] hey 的 Requests/sec 没解析出来：版式变了，看 $OUT/$tag.hey.txt"
  echo "$qps"
}

hey_percentile_ms() { # 文件 标签(50%|90%|99%)
  awk -v want="$2" '
    /^Latency distribution/ { inblock = 1; next }
    inblock {
      # 形如 "  99% in 1.23 millis" 或 "  99% in 0.004567 secs"
      if ($1 != want) next
      v = $3; unit = $4
      if (unit ~ /^mil/) v = v + 0
      else if (unit ~ /^sec/) v = (v + 0) * 1000.0
      else if (unit ~ /^micro/) v = (v + 0) / 1000.0
      printf "%.3f\n", v
      exit
    }
  ' "$1"
}

echo "== SLO #1 转发尾延迟（release；preset 不是可选项，是这条数字的前提）"
start_upstream

echo "-- 组 A：直连上游（这是分母，不是对照组）"
QPS_DIRECT=$(run_wrk direct "http://127.0.0.1:$UP_PORT/bench")
QPS_DIRECT_HEY=$(run_hey direct "http://127.0.0.1:$UP_PORT/bench")

echo "-- 组 B：经网关转发（同一个上游、同一台机器、同样的连接数）"
start_gateway
QPS_PROXY=$(run_wrk proxy "http://127.0.0.1:$GATEWAY_PORT/bench")
QPS_PROXY_HEY=$(run_hey proxy "http://127.0.0.1:$GATEWAY_PORT/bench")

# 静置一会儿再停网关。理由：SIGTERM 一到，事件循环立刻退出，**在飞连接不会被收尾**
# ——上一轮就是这么被记成"fd_end=351、live_connections=342、unregistrations 比
# registrations 少 344"的：那不是泄漏，是我们在 1000 并发正跑着的时候把进程杀了。
# 静置把这两种形状分开：工具都退了、也给了网关时间，剩下的每一条才是真的没人管。
# 分开之后下面的断言才有资格是断言（见 gateway_account_checks）。
echo "-- 静置 ${SETTLE}s：让网关把客户端已关的连接收完"
sleep "$SETTLE"

kill -TERM "$G_PID" 2>/dev/null
for _ in $(seq 1 200); do kill -0 "$G_PID" 2>/dev/null || break; sleep 0.1; done
wait "$G_PID" 2>/dev/null; G_RC=$?
kill -TERM "$UP_PID" 2>/dev/null; wait "$UP_PID" 2>/dev/null
(( G_RC == 0 )) || die "网关退出码 $G_RC：优雅停止没生效，这一轮的账不完整"

# 三个数字是这一轮"是不是在量延迟"的资格证明，不是锦上添花：
#   live_connections 必须为 0（S1-04 定的不变量，静置后仍非 0 就是真漏）
#   fd_end - fd_start 必须为 0（同上，从内核侧再证一次）
#   压测上游的 refused_over_capacity / refused_addfail 必须为 0（仪器没丢负载）
gval() { sed -n "s/^\[serve\] $1=\(.*\)$/\1/p" "$OUT/gateway.log" | head -1; }
uval() { sed -n "s/^\[bench-upstream\].*$1=\([0-9]*\).*/\1/p" "$OUT/upstream.log" | head -1; }
LIVE=$(gval live_connections); FD0=$(gval fd_start); FD1=$(gval fd_end)
REFUSED=$(uval refused_over_capacity); ADDFAIL=$(uval refused_addfail)
PEAK=$(uval peak_clients)
[[ "${LIVE:-x}" == "0" ]] || die "静置 ${SETTLE}s 后仍有存活连接 ${LIVE:-?}：这是真漏，不是杀太早"
[[ "${FD0:-x}" == "${FD1:-y}" ]] || die "fd 没回基线：${FD0:-?} → ${FD1:-?}（差 $((${FD1:-0} - ${FD0:-0}))）"
[[ "${REFUSED:-x}" == "0" && "${ADDFAIL:-x}" == "0" ]] || \
  die "压测上游丢了负载：容量满 ${REFUSED:-?} 条、epoll 注册失败 ${ADDFAIL:-?} 条——这轮的 p99 是'排队等仪器'的延迟，作废"
note "生命周期收尾：live_connections=0、fd ${FD0}→${FD1}、上游 refused=0（峰值并发 ${PEAK:-?}/${MAX_CLIENTS_NOTE}）"

echo "-- 网关自报的账（它自己打印的，与 wrk 的口径独立）："
grep '^\[serve\]' "$OUT/gateway.log" | sed -e 's/^/    /' -e 's/\[serve\] //'
grep '^\[bench-upstream\]' "$OUT/upstream.log" | sed -e 's/^/    /'

pctl() { wrk_percentile_ms "$OUT/$1.wrk.txt" "$2"; }
d50=$(pctl direct 50%); d75=$(pctl direct 75%); d90=$(pctl direct 90%); d99=$(pctl direct 99%)
p50=$(pctl proxy 50%); p75=$(pctl proxy 75%); p90=$(pctl proxy 90%); p99=$(pctl proxy 99%)
# p999 在这里**必须是空**，不是解析失败：wrk 4.1.0 的 Latency Distribution 只有
# 50/75/90/99 四行，hey 0.1.4 只到 99%。两个声明过的工具都不产出 99.9% 分位，
# 所以 SLO #1 的 p999 半边当前无法判定。下面那段报告把这件事写成一句人话，
# 而不是留个 0.000ms 让人以为"增量达标"。
d999=$(pctl direct '99.9%'); p999=$(pctl proxy '99.9%')
h50=$(hey_percentile_ms "$OUT/direct.hey.txt" 50%); h90=$(hey_percentile_ms "$OUT/direct.hey.txt" 90%); h99=$(hey_percentile_ms "$OUT/direct.hey.txt" 99%)
hp50=$(hey_percentile_ms "$OUT/proxy.hey.txt" 50%); hp90=$(hey_percentile_ms "$OUT/proxy.hey.txt" 90%); hp99=$(hey_percentile_ms "$OUT/proxy.hey.txt" 99%)

REPORT="$OUT/s1-p99.summary.txt"
{
  echo "# SLO #1 转发尾延迟 —— $(date -Is)"
  echo "口径 canonic=$CANONIC  preset=release  body=${BODY_BYTES}B  conns=${CONNECTIONS}  duration=${DURATION}  warmup=${WARMUP}  wrk_threads=${WRK_THREADS}"
  echo "工具: $(wrk --version 2>&1 | head -1) / $(hey 2>&1 | grep -m1 'hey version' || echo hey)"
  echo "ulimit -n=${HAVE_FD}  nproc=$(nproc)  CPU=$(sed -n 's/^model name[[:space:]]*:[[:space:]]*//p' /proc/cpuinfo | head -1)"
  echo "绑核：未做（cpuset 方案按 ENVIRONMENT.md 第 7 节留到 S4 定稿；这条不是口径变更，是从未声明过绑核）"
  echo
  echo "## wrk（主）"
  printf '%-8s %10s %10s %10s %10s %12s\n' 组 p50ms p75ms p90ms p99ms QPS
  printf '%-8s %10s %10s %10s %10s %12s\n' 直连 "${d50:-?}" "${d75:-?}" "${d90:-?}" "${d99:-?}" "${QPS_DIRECT:-?}"
  printf '%-8s %10s %10s %10s %10s %12s\n' 经网关 "${p50:-?}" "${p75:-?}" "${p90:-?}" "${p99:-?}" "${QPS_PROXY:-?}"
  # 增量只在**两个数都取到**时才打印。缺一个就写"无法计算"，绝不写 0.000：
  # 0.000ms 在 SLO #1 的语境里等于"达标"，而它其实等于"没解析出来"。
  awk -v a="${d99:-}" -v b="${p99:-}" 'BEGIN{
    if (a == "" || b == "") { print "增量 p99：无法计算（分位没解析出来，见 *.wrk.txt 原始版式）"; }
    else printf "增量 p99=%.3fms（经网关 − 直连，SLO 判的就是这个差值）\n", b - a }'
  awk -v c="${d999:-}" -v d="${p999:-}" 'BEGIN{
    if (c != "" && d != "") { printf "增量 p999=%.3fms\n", d - c; }
    else print "增量 p999：无法计算 —— wrk 4.1.0 / hey 0.1.4 的分位表都只到 99%，SLO #1 的 p999 半边当前判不了（要产出任意分位的工具 = 新依赖，需先裁决 + ADR）"}'
  echo
  echo "## hey（交叉验证，连接模型不同，只看量级）"
  printf '%-8s %10s %10s %10s %12s\n' 组 p50ms p90ms p99ms QPS
  printf '%-8s %10s %10s %10s %12s\n' 直连 "${h50:-?}" "${h90:-?}" "${h99:-?}" "${QPS_DIRECT_HEY:-?}"
  printf '%-8s %10s %10s %10s %12s\n' 经网关 "${hp50:-?}" "${hp90:-?}" "${hp99:-?}" "${QPS_PROXY_HEY:-?}"
  echo
  echo "原始输出：direct.wrk.txt / proxy.wrk.txt / direct.hey.txt / proxy.hey.txt / gateway.log / upstream.log"
} > "$REPORT"

cat "$REPORT"
echo
echo "== 结果落在 $REPORT（连同原始 wrk/hey 输出都在 $OUT/）"
if (( FAILED != 0 )); then
  echo "== SLO #1 本轮有 ✗，上面的数字不要用"
fi
exit "$FAILED"
