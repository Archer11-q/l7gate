#!/usr/bin/env bash
# S1-04 冒烟：用**真 curl** 打 10000 个请求，看网关自己报出来的账。
#
# 为什么要有这个脚本，而不只靠 reactor_test.cc 里那条 10000 请求的用例：
# 那条用例的客户端是我自己写的，它和网关共享同一套我对 HTTP 的理解——它对，
# 可能只是我和我自己一致。curl 是第三方的，它说"我收到了 200 和 64 字节正文"
# 才算外部证据。代价是这里量不到 p99（curl 的串行往返里混着它自己的开销），
# 所以这个脚本的结论只有"通不通、关干净没有"，性能数字一律不从这里取。
#
# 用法：PRESET=debug scripts/smoke-s1-04.sh
# 输出：build/smoke-s1-04/ 里的日志 + fd 曲线；退出码 0 = 全部通过。
set -uo pipefail
cd /home/archer/l7gate || exit 1

PRESET="${PRESET:-debug}"
BIN="build/$PRESET"
GATEWAY="$BIN/l7gate"
# 上游工具在 tests/ 子目录下：它链的是 l7gate_core，但产物不是测试可执行文件，
# 位置跟着定义它的 CMake 目录走，不在 preset 的顶层输出目录里。
UPSTREAM_BIN="$BIN/tests/l7gate_test_upstream"
GATEWAY_PORT="${GATEWAY_PORT:-18080}"
REQUESTS="${REQUESTS:-10000}"
OUT="build/smoke-s1-04"
mkdir -p "$OUT"

FAILED=0
fail() { echo "  ✗ $*"; FAILED=1; }
ok()   { echo "  ✓ $*"; }

[[ -x "$GATEWAY" ]] || { echo "找不到 $GATEWAY，先 cmake --build --preset $PRESET" >&2; exit 1; }
[[ -x "$UPSTREAM_BIN" ]] || { echo "找不到 $UPSTREAM_BIN，先 cmake --build --preset $PRESET" >&2; exit 1; }
command -v curl >/dev/null || { echo "本机没有 curl：这个脚本要的第三方证据不成立" >&2; exit 1; }
curl --version | head -1 | sed 's/^/  curl: /'

# 等一个后台进程在它的日志里打出某一行。等不到就返回 1，并顺手区分两种病因：
# 进程还活着 = 真挂住；进程已退出 = 启动失败，日志尾部就是原因。
wait_for_line() { # pid logfile pattern [seconds]
  local pid=$1 log=$2 pat=$3 secs=${4:-15} i=0
  while (( i < secs * 20 )); do
    if grep -q -- "$pat" "$log" 2>/dev/null; then return 0; fi
    if ! kill -0 "$pid" 2>/dev/null; then
      echo "  进程 $pid 已退出，日志尾部：" >&2
      tail -5 "$log" >&2
      return 1
    fi
    sleep 0.05; ((i += 1))
  done
  return 1
}

start_upstream() { # tag -> 置 UP_PID / UP_PORT
  local log="$OUT/up-$1.log"
  "$UPSTREAM_BIN" --body-bytes 64 > "$log" 2>&1 &
  UP_PID=$!
  if ! wait_for_line "$UP_PID" "$log" '\[upstream\] 监听'; then
    fail "测试上游没能起来（见 $log）"; return 1
  fi
  UP_PORT=$(sed -n 's/.*监听 127\.0\.0\.1:\([0-9]*\).*/\1/p' "$log" | head -1)
  if [[ -z "$UP_PORT" ]]; then fail "读不到上游实际端口（见 $log）"; return 1; fi
  echo "  上游 pid=$UP_PID port=$UP_PORT"
}

# 上游的响应体长度和 seq 前缀都在 test_upstream.h 里，这里只核对"回来了多少字节"。
check_fd_curves() { # gateway_log  —— 用网关自己的自报数，口径和外部采样不同，两条都给
  local log=$1
  local from to
  from=$(sed -n 's/^\[serve\] fd_start=\([0-9]*\).*/\1/p' "$log")
  to=$(sed -n 's/^\[serve\] fd_end=\([0-9]*\).*/\1/p' "$log")
  if [[ -n "$from" && "$from" == "$to" ]]; then
    ok "网关自报 fd $from → $to：跑完之后 fd 回到基线（含它自己那 3 个常驻项）"
  else
    fail "网关自报 fd ${from:-?} → ${to:-?} 不等：有 fd 没关"
  fi
}

get_stat() { # key logfile
  sed -n "s/^\[serve\] $1=\([0-9]*\).*/\1/p" "$2" | head -1
}

# ---------------------------------------------------------------- 阶段一
# 一个请求，把响应原文留在磁盘上。这一步不看任何计数，只看字节：出问题时我要
# 第一眼就看到"到底是请求没过去，还是响应没回来"。
echo "== 阶段一：单请求字节可见性（curl -i 的完整响应）"
start_upstream one || exit 1
G1="$OUT/gateway-one.log"
"$GATEWAY" --serve --listen "127.0.0.1:$GATEWAY_PORT" --upstream "127.0.0.1:$UP_PORT" \
  > "$G1" 2>&1 &
G_PID=$!
if ! wait_for_line "$G_PID" "$G1" '\[serve\] 监听'; then
  kill -TERM "$UP_PID" 2>/dev/null; wait "$UP_PID" 2>/dev/null
  fail "网关没能起来"; exit 1
fi
curl -sS -i --max-time 10 "http://127.0.0.1:$GATEWAY_PORT/smoke" > "$OUT/response-one.txt" 2>&1
CURL_RC=$?
kill -TERM "$G_PID" 2>/dev/null
wait "$G_PID"; G_RC=$?
kill -TERM "$UP_PID" 2>/dev/null
wait "$UP_PID" 2>/dev/null
echo "  curl 退出码=$CURL_RC 网关退出码=$G_RC"
(( CURL_RC == 0 )) || fail "curl 在单请求阶段就失败了（退出码 $CURL_RC）"
(( G_RC == 0 )) || fail "网关不是自己退出的（退出码 $G_RC）—— SIGTERM 的优雅停止没生效"
if grep -q '^HTTP/1.1 200 OK' "$OUT/response-one.txt"; then
  ok "响应头回来了：$(head -1 "$OUT/response-one.txt")"
else
  fail "响应头没回来，见 $OUT/response-one.txt"
fi
body_len=$(awk 'BEGIN{RS="\r\n\r\n"} NR==2 {gsub(/\r/,""); print length($0)}' "$OUT/response-one.txt")
if [[ "${body_len:-0}" == "64" ]]; then
  ok "响应体 64 字节，一个不多一个不少（上游 --body-bytes 64）"
else
  fail "响应体长度 = ${body_len:-UNKNOWN}，期望 64"
fi
echo "  响应原文（响应体没有结尾换行，所以这里补一个空行，不然下面的输出会接在它尾巴上）："
sed 's/^/    /' "$OUT/response-one.txt" | head -8
echo
parsed_one=$(get_stat requests_parsed "$G1")
if [[ "${parsed_one:-0}" == "1" ]]; then
  ok "网关解析到 1 个请求"
else
  fail "网关报的解析数是 ${parsed_one:-UNKNOWN}，不是 1"
fi

# ---------------------------------------------------------------- 阶段二
# 10000 个请求，一个 curl 进程。
# 用 curl 的 URL glob（{1..N}）而不是 shell 循环 10000 次 curl：循环会让每次请求
# 都新开一个进程、新握一条连接，而 keep-alive 复用恰恰是这一步要盯的性质。
echo "== 阶段二：$REQUESTS 个请求（单进程 curl，keep-alive）"
start_upstream main || exit 1
G2="$OUT/gateway-main.log"
"$GATEWAY" --serve --listen "127.0.0.1:$GATEWAY_PORT" --upstream "127.0.0.1:$UP_PORT" \
  > "$G2" 2>&1 &
G_PID=$!
if ! wait_for_line "$G_PID" "$G2" '\[serve\] 监听'; then
  kill -TERM "$UP_PID" 2>/dev/null
  fail "网关没能起来（阶段二）"; exit 1
fi

# 基线和曲线都在循环外读 /proc：采样的是另一个进程的 fd 表，不掺网关自己的口径。
FD_BASE=$(ls "/proc/$G_PID/fd" 2>/dev/null | wc -l)
RSS_BASE=$(awk '/VmRSS/{print $2}' "/proc/$G_PID/status" 2>/dev/null)
CURVE="$OUT/fd-curve.txt"
: > "$CURVE"
# 采样器：0.2s 一次的 fd 曲线。要这条而不是"结束后一个数"，是因为泄漏的形态是
# **单调增长**，只看终点会把"跑了 5000 个请求才撞上限"这类问题洗掉。
( while kill -0 "$G_PID" 2>/dev/null; do
    printf '%s %s %s\n' "$(date +%s.%N)" \
      "$(ls "/proc/$G_PID/fd" 2>/dev/null | wc -l)" \
      "$(awk '/VmRSS/{print $2}' "/proc/$G_PID/status" 2>/dev/null)" >> "$CURVE"
    sleep 0.2
  done ) &
SAMPLER=$!

start_ts=$(date +%s.%N)
# curl 的区间写法是**方括号** [1-N]，不是 shell 的花括号 {1..N}。
# 我第一次写的是 '{1..10000}'：curl 不认识这种 set，把大括号吃掉当成普通字符，
# 于是只发出 1 个请求、退出码还是 0、耗时 0.02s——三个"看起来成功"的信号同时成立。
# 抓到它靠的就是下面那条 requests_parsed == $REQUESTS 的断言：脚本必须自己数,
# 不能信客户端的退出码。
curl -sS -o /dev/null --max-time 600 "http://127.0.0.1:$GATEWAY_PORT/r[1-$REQUESTS]"
CURL_RC=$?
end_ts=$(date +%s.%N)
elapsed=$(awk -v a="$start_ts" -v b="$end_ts" 'BEGIN{printf "%.2f", b-a}')
echo "  curl 退出码=$CURL_RC 耗时=${elapsed}s"
(( CURL_RC == 0 )) || fail "curl 退出码 $CURL_RC：$REQUESTS 个请求没有全部拿到响应"

# curl 收完了才停网关。**顺序就是这条用例**：反过来（先停再收）等于把还在
# 出向缓冲里的响应一起关掉，那正是 S1-04 里 CloseWhy 那一步要防的事。
kill -TERM "$G_PID" 2>/dev/null
for _ in $(seq 1 200); do kill -0 "$G_PID" 2>/dev/null || break; sleep 0.1; done
if kill -0 "$G_PID" 2>/dev/null; then
  fail "网关收到 SIGTERM 之后 20s 还没停：优雅停止没生效"
  kill -KILL "$G_PID" 2>/dev/null
fi
wait "$G_PID"; G_RC=$?
kill "$SAMPLER" 2>/dev/null; wait "$SAMPLER" 2>/dev/null
# 先停上游、等它把最后一行打出来，再去读那个数：反过来读到的是它还没写的日志。
kill -TERM "$UP_PID" 2>/dev/null; wait "$UP_PID" 2>/dev/null
UP_SERVED=$(sed -n 's/.*已服务请求数=\([0-9]*\).*/\1/p' "$OUT/up-main.log" | head -1)
echo "  网关退出码=$G_RC 上游侧自报已服务=${UP_SERVED:-UNKNOWN}"
if [[ "${UP_SERVED:-UNKNOWN}" == "$REQUESTS" ]]; then
  ok "上游侧收到 $REQUESTS 个请求：网关没有丢请求，也没有每请求新开一条回源连接"
else
  fail "上游侧收到 ${UP_SERVED:-UNKNOWN}，期望 $REQUESTS"
fi

echo "  网关统计（它自己打印的 key=value）："
grep '^\[serve\]' "$G2" | sed 's/^/    /'

(( G_RC == 0 )) || fail "网关退出码 $G_RC"
parsed=$(get_stat requests_parsed "$G2")
accepted=$(get_stat downstream_accepted "$G2")
up_opened=$(get_stat upstream_opened "$G2")
live=$(get_stat live_connections "$G2")
constructed=$(get_stat connections_constructed "$G2")
destructed=$(get_stat connections_destructed "$G2")
closed=$(get_stat connections_closed "$G2")
b_down=$(get_stat bytes_down_to_upstream "$G2")
b_up=$(get_stat bytes_up_to_downstream "$G2")
scan=$(get_stat header_scan_bytes "$G2")
reject=$(get_stat requests_rejected "$G2")

if [[ "${parsed:-UNKNOWN}" == "$REQUESTS" ]]; then
  ok "requests_parsed = $parsed（目标 $REQUESTS）"
else
  fail "requests_parsed = ${parsed:-UNKNOWN}，目标 $REQUESTS"
fi
if [[ "${reject:-0}" == "0" ]]; then
  ok "requests_rejected = 0：curl 的请求全部被判为合法（解析器没把第三方写法当畸形）"
else
  fail "requests_rejected = ${reject:-UNKNOWN}，期望 0：我的解析器不接受 curl 的某种写法"
fi
if [[ "${accepted:-UNKNOWN}" == "1" ]]; then
  ok "downstream_accepted = 1：curl 全程复用了一条 keep-alive 下游连接"
else
  fail "downstream_accepted = ${accepted:-UNKNOWN}，期望 1。归因：curl 没复用连接，或网关提前关了下游"
fi
if [[ "${up_opened:-UNKNOWN}" == "1" ]]; then
  ok "upstream_opened = 1：$REQUESTS 个请求只回源了一条连接，不是每请求一条"
else
  fail "upstream_opened = ${up_opened:-UNKNOWN}，期望 1"
fi
if [[ "${live:-UNKNOWN}" == "0" ]]; then
  ok "live_connections = 0：循环退出时没有一条连接还挂在注册表里"
else
  fail "live_connections = ${live:-UNKNOWN}，期望 0：关闭路径漏了某一步"
fi
if [[ -n "$constructed" && "$constructed" == "$destructed" ]]; then
  ok "connections_constructed = connections_destructed = $constructed：shared_ptr 引用计数确实归零"
else
  fail "构造 $constructed / 析构 ${destructed:-UNKNOWN} 不等：有对象没被释放"
fi
echo "  connections_closed=${closed:-?} bytes_down=${b_down:-?} bytes_up=${b_up:-?} header_scan=${scan:-?}"
check_fd_curves "$G2"

if [[ -s "$CURVE" ]]; then
  peak_fd=$(awk '{if($2>m)m=$2} END{print m}' "$CURVE")
  peak_rss=$(awk '{if($3>m)m=$3} END{print m+0}' "$CURVE")
  samples=$(wc -l < "$CURVE")
  last_fd=$(awk 'END{print $2}' "$CURVE")
  echo "    外部采样：fd 基线=$FD_BASE 末值=${last_fd:-?} 峰值=${peak_fd:-?}；RSS 基线=${RSS_BASE:-?}KiB 峰值=${peak_rss:-?}KiB；样本=${samples}个"
  # 峰值允许比基线高出 8：curl 侧的连接在同一个 netns 里，网关那半算得清，
  # 客户端那半不归我管；这个余量是给"我不知道的那几个"留的，不是给泄漏留的。
  if [[ -n "$peak_fd" ]] && (( peak_fd - FD_BASE <= 8 )); then
    ok "fd 峰值相对基线只涨 $((peak_fd - FD_BASE))（≤8）：曲线没有随请求数爬升"
  else
    fail "fd 峰值 ${peak_fd:-UNKNOWN} 相对基线 $FD_BASE 涨幅超出 8：fd 随请求数累积"
  fi
  if [[ -n "$peak_rss" && -n "$RSS_BASE" ]]; then
    echo "    RSS 增量 $((peak_rss - RSS_BASE)) KiB / $REQUESTS 请求（debug preset，含 curl 与分配器噪声，不作 SLO 依据）"
  fi
else
  fail "外部 fd 采样为空（读不到 /proc/$G_PID/fd）：曲线这条证据没拿到"
fi

echo
if (( FAILED == 0 )); then
  echo "== S1-04 冒烟通过：preset=$PRESET requests=$REQUESTS 耗时=${elapsed}s 证据在 $OUT/"
else
  echo "== S1-04 冒烟未通过：见上面带 ✗ 的行，逐项都是没达标的项目，不是警告"
fi
exit "$FAILED"
