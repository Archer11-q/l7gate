#!/usr/bin/env bash
# S2-05 冒烟：在**真进程 + 真 socket + 真 SIGTERM** 上把优雅停止（排水）走一遍。
#
# 与 ctest 的分工先写死，免得读者以为这里重复了一遍单测：
#   ctest 那 6 条 `S2_05_GracefulStop` 钉的是**机制**（在飞等完 / 空闲立即关 / 半包走兜底 /
#   隧道走硬关分支 / accept 层拒新 / 多 worker 协调），它们能在循环跑着的时候原子切夹具的
#   stall 开关，也能在排水中途 join 线程——这些进程外做不到。
#   本脚本交的是**只有真进程才能证明**的四件事：
#   一、信号链路（SIGTERM → handler → request_drain → 排水 → 自己退出 → 打印统计）真的通；
#   二、`--drain-timeout-ms` 是命令行可配的，而且它**守时限**（早关和拖太久都算红）；
#   三、排水之后生命周期三项归零（live=0、constructed==destructed、fd_start==fd_end）——
#       这三条是全项目停止语义的地基，S2-05 改的是"怎么走到这一步"，不是这一步的形状；
#   四、多 worker（--workers 3）下排水是每条 worker 各排的，而排水期间的新连接不被服务。
#
# 每阶段单独起一对上下游+网关，网关自报的计数只属于这一步（同 smoke-s1-06/07 的理由：
# 把五个形态塞进一条循环，最后一个 total 分不清是哪一步贡献的，那种账不叫证据）。
# 本脚本不产出任何性能数字：/dev/tcp 的节奏是我自己写的，混不进 p99。
#
# 用法：PRESET=debug scripts/smoke-s2-05.sh
# 输出：build/smoke-s2-05/ 里的响应原文 + 各步日志；退出码 0 = 全部通过。
set -uo pipefail
# 忽略 SIGPIPE。这条是**实测出来的**，不是防御性摆设：
# 阶段五会把一条连接写给"正在排水的网关刚 accept 完就关掉"的那个 socket（ARCHITECTURE
# 1.20：监听 fd 留到析构，拒发生在连接对象这一层），对已关 socket 的写在下一次写时
# 拿到 EPIPE → SIGPIPE → **直接把这条 shell 打死**，而且日志里一行 ✗ 都没有
# （看起来像"跑到一半没输出"，不像"失败了"）。
# A/B（同一份脚本，唯一差别就是下面这行 trap 在不在）：
#   有 trap '' PIPE：连跑 6 次 rc=0、✗ 计数 0
#   没有它：连跑 3 次里 2 次 rc=13、死在阶段五开头，1 次侥幸绿
# 归因到 SIGPIPE 的依据是这次 A/B，不是我构造的最小复现——两写的最小样本里 shell
# 活下来了，所以"第二次写才 EPIPE"这个形状我只有 A/B 一层证据，写在这里按这个强度读。
# 第二层兜底是下面的 DONE 哨兵：任何没走到最后一行的终止都会变成一句响的失败。
trap '' PIPE
cd "$(dirname "$(readlink -f "$0")")/.." || exit 1

PRESET="${PRESET:-debug}"
BIN="build/$PRESET"
GATEWAY="$BIN/l7gate"
UPSTREAM_BIN="$BIN/tests/l7gate_test_upstream"
GATEWAY_PORT="${GATEWAY_PORT:-18082}"
OUT="build/smoke-s2-05"
mkdir -p "$OUT"

FAILED=0
fail() { echo "  ✗ $*"; FAILED=1; }
ok()   { echo "  ✓ $*"; }

[[ -x "$GATEWAY" ]] || { echo "找不到 $GATEWAY，先 cmake --build --preset $PRESET" >&2; exit 1; }
[[ -x "$UPSTREAM_BIN" ]] || { echo "找不到 $UPSTREAM_BIN，先 cmake --build --preset $PRESET" >&2; exit 1; }

UP_PID=""; G_PID=""; UP_PORT=""; UP_LOG=""; G_LOG=""; G_RC=""; REACHED_END=0
cleanup() {
  # DONE 哨兵：没走到脚本最后一行就是**提前终止**。这类死亡是静悄悄的（`exec` 重定向
  # 失败会直接带走非交互 shell，第一版就是这样：rc=127、日志里一行 ✗ 都没有，
  # 看起来像"跑到一半没输出"而不是"失败了"）。所以在这里把它变成一句响的。
  if (( REACHED_END == 0 )); then
    echo "  ✗ 脚本提前终止（没走到最后一行）——这不是通过" >&2
    FAILED=1
  fi
  [[ -n "$G_PID" ]] && kill -TERM "$G_PID" 2>/dev/null
  [[ -n "$UP_PID" ]] && kill -TERM "$UP_PID" 2>/dev/null
  return 0
}
trap cleanup EXIT

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

start_upstream() { # tag  夹具参数...
  local tag=$1; shift
  UP_LOG="$OUT/up-$tag.log"
  "$UPSTREAM_BIN" "$@" > "$UP_LOG" 2>&1 &
  UP_PID=$!
  wait_for_line "$UP_PID" "$UP_LOG" '\[upstream\] 监听' || { fail "上游没能起来（$tag）"; return 1; }
  UP_PORT=$(sed -n 's/.*监听 127\.0\.0\.1:\([0-9]*\).*/\1/p' "$UP_LOG" | head -1)
  [[ -n "$UP_PORT" ]] || { fail "读不到上游实际端口（$tag）"; return 1; }
}

start_gateway() { # tag  网关额外参数...
  local tag=$1; shift
  G_LOG="$OUT/gateway-$tag.log"
  "$GATEWAY" --serve --listen "127.0.0.1:$GATEWAY_PORT" --upstream "127.0.0.1:$UP_PORT" \
    "$@" > "$G_LOG" 2>&1 &
  G_PID=$!
  if ! wait_for_line "$G_PID" "$G_LOG" '\[serve\] 监听'; then
    kill -TERM "$UP_PID" 2>/dev/null; wait "$UP_PID" 2>/dev/null
    fail "网关没能起来（$tag）"; return 1
  fi
  echo "  $tag: 上游 pid=$UP_PID port=$UP_PORT / 网关 pid=$G_PID port=$GATEWAY_PORT"
}

get_stat() { sed -n "s/^\[serve\] $1=\([0-9-]*\).*/\1/p" "$G_LOG" | head -1; }

# 等网关**自己**退出并取退出码。这里是本脚本最硬的一条断言的位置：优雅停止的定义是
# "它自己走完收尾并把统计打印出来"，所以 kill -KILL 只允许出现在**失败分支**里
# ——不这样写的脚本会把"被我 Timeout 掉"读成"它停下来了"（BENCHMARK 规矩：一片 0 比
# 一片 ? 危险，这里同理：一个被 KILL 的退出码比一个 ? 更容易被抄进文档）。
wait_gateway_exit() { # tag seconds
  local tag=$1 deadline=$2 waited=0
  while (( waited < deadline * 10 )); do
    if ! kill -0 "$G_PID" 2>/dev/null; then
      wait "$G_PID" 2>/dev/null; G_RC=$?
      (( G_RC == 0 )) || fail "网关 $tag 退出码 $G_RC（不是 0：SIGTERM 的优雅停止没生效）"
      return 0
    fi
    sleep 0.1; ((waited += 1))
  done
  fail "网关 $tag：收到 SIGTERM 之后 ${deadline}s 还没自己停（排水挂住了）"
  kill -KILL "$G_PID" 2>/dev/null; wait "$G_PID" 2>/dev/null
  G_RC="KILLED"
  return 1
}

stop_upstream() { kill -TERM "$UP_PID" 2>/dev/null; wait "$UP_PID" 2>/dev/null; UP_PID=""; }

# 生命周期三重检查。注意 fd_start==fd_end 这里**不加分支**：排水按设计不关监听 fd
# （理由见 ARCHITECTURE 1.20 的 accept_all 那段），所以常驻项两边都在、等式照旧。
check_lifecycle() { # tag
  local tag=$1 live constructed destructed fd_from fd_to
  live=$(get_stat live_connections)
  constructed=$(get_stat connections_constructed)
  destructed=$(get_stat connections_destructed)
  fd_from=$(get_stat fd_start); fd_to=$(get_stat fd_end)
  [[ "${live:-?}" == "0" ]] || { fail "$tag：live_connections=${live:-?}，期望 0（排水没排干）"; return; }
  if [[ -n "$constructed" && "$constructed" == "$destructed" ]]; then
    ok "$tag：live=0、constructed==destructed==$constructed、fd ${fd_from}→${fd_to}"
  else
    fail "$tag：构造 $constructed / 析构 ${destructed:-?} 不等——有对象没被释放"
  fi
  [[ -n "$fd_from" && "$fd_from" == "$fd_to" ]] \
    || fail "$tag：fd ${fd_from:-?}→${fd_to:-?} 没回基线"
}

# --- 客户端：一条能"发完先不收"的 /dev/tcp 双向 fd -------------------------
# curl 做不到这件事（它发完就等响应，等不到就自己超时走了），而排水要测的恰恰是
# "请求已经在网关手上、响应还没回来"的那一刻。
open_conn() { # var_name
  local name=$1
  # 不额外探测可达性：向网关多连一条就会多一个 downstream_accepted / 多一次关闭，
  # 而阶段二要断言的正是 `downstream_accepted == 3`——探针会把它自己数进账里。
  # "连不上"在这里不是需要兜的分支：start_gateway 已经等到 [serve] 监听 那一行。
  # 真失败了 `exec` 的重定向错误会让非交互 shell 直接退出，那种早退由下面的 DONE 哨兵
  # 兜成一句响亮的失败（第一版就是这样静悄悄 rc=127、日志里一行 ✗ 都没有）。
  exec {fd}<>/dev/tcp/127.0.0.1/$GATEWAY_PORT || { fail "连不上网关 127.0.0.1:$GATEWAY_PORT"; return 1; }
  printf -v "$name" '%s' "$fd"
}
# 关 fd 只能走 eval：`exec {1}>&-` 里的 {1} 不是变量名，bash 把它当**文件名**去重定向，
# 而 exec 的重定向失败会**直接终止这条非交互 shell**（第一版就是这样在阶段一中间静悄悄
# 退出、rc=127、日志里一行 ✗ 都没有）。所以先校验它是纯数字再展开。
close_conn() { # fd
  local fd=${1:-}
  [[ "$fd" =~ ^[0-9]+$ ]] || return 0
  eval "exec $fd>&-" 2>/dev/null
}
send_get() { # fd target
  printf 'GET /%s HTTP/1.1\r\nHost: smoke.test\r\n\r\n' "$2" >&"$1"
}
# 读到 EOF 为止，把全部字节落进文件。打印收到的字节数；读超时打印 -1（不打印 0：
# 0 会让"网关既没发完也没关"看起来像"它发了一个空响应"）。
read_to_eof() { # fd outfile seconds
  local fd=$1 out=$2 secs=$3
  timeout "$secs" cat <&"$fd" > "$out" 2>/dev/null
  local rc=$?
  (( rc == 124 )) && { echo "-1"; return; }
  wc -c < "$out" | tr -d ' '
}
# 头部区长度 = 第一个 CRLFCRLF 的偏移 + 4。**整档读成一条记录**再找：第一版这里用
# `grep -abo $'\r\n\r\n'`，它给了一个看着合理的偏移（32），于是阶段一/四都红在
# "字节数对不上"上——grep 是按行工作的，模式里的 \n 让它永远跨不了行，报出来的是
# 行内位置而不是文件偏移。找不到一律返回 -1（一片 0 比一片 ? 危险）。
head_len_of() { # file
  awk 'BEGIN{RS="@@__no_such_record_separator__@@"}
       { i = index($0, "\r\n\r\n"); print (i ? i + 3 : -1) }' < "$1" 2>/dev/null
}
# 「响应完整」写成等式而不是"看着够长"：总字节 == 头长 + Content-Length。
# >= 会容纳"多发了别的字节"这一类错，而阶段四恰恰要拒绝那一种错（硬关不许伪造
# close 握手）——同一条判据不该两个阶段两个松紧。
check_response_complete() { # tag file
  local tag=$1 f=$2 total headend cl
  total=$(wc -c < "$f" 2>/dev/null | tr -d ' ')
  headend=$(head_len_of "$f")
  cl=$(sed -n 's/^Content-Length: \([0-9]*\).*/\1/p' "$f" 2>/dev/null | head -1)
  if [[ -z "$total" || "$headend" == "-1" || -z "$cl" ]]; then
    fail "$tag：读不到响应形状（总=${total:-?} 头=${headend:-?} CL=${cl:-?}）——没资格算送达"
    return 1
  fi
  if (( total == headend + cl )); then
    ok "$tag：完整响应 $total 字节 = 头 $headend + 体 $cl"
    return 0
  fi
  fail "$tag：响应不完整或多了字节（总 $total，头 $headend，声明体 $cl）"
  return 1
}

# ------------------------------------------------------------------ 一
# 裁决第一条在进程里的形态：SIGTERM 到达那一刻，有一条请求**已经转发给上游、响应还
# 没回来**（上游停读 4s 造出这一格）。排水必须把它答完再退出，而不是把它当成在飞泄漏。
echo
echo "== 阶段一：在飞请求被逐条答完（上游停读 4s + 兜底 60000ms）"
start_upstream inflight --stall-reads-ms 4000 --fixed-body "S205-INFLIGHT" || exit 1
start_gateway inflight --drain-timeout-ms 60000 || exit 1
C1=""; open_conn C1 || exit 1
send_get "$C1" inflight
sleep 1  # 请求转出去了，上游一个字节都不读 → 它正卡在"已接受、没响应"那一格
kill -TERM "$G_PID" 2>/dev/null
sleep 5  # stall 到点 → 上游读出请求 → 响应回来，必须落在排水窗口里
N=$(read_to_eof "$C1" "$OUT/proxy-inflight.txt" 10)
close_conn "$C1"
wait_gateway_exit inflight 60 || { stop_upstream; exit 1; }
stop_upstream
echo "  客户端收到 ${N} 字节，网关退出码=$G_RC"
check_response_complete 阶段一 "$OUT/proxy-inflight.txt"
grep -q "S205-INFLIGHT" "$OUT/proxy-inflight.txt" \
  && ok "响应体就是这一步钉死的那条内容（没串号、没被改写）" \
  || fail "响应体不是钉死的那条内容"
[[ "$(get_stat drain_requested)" == "1" ]] \
  && ok "drain_requested=1：SIGTERM 真的走到了排水（不是硬停）" \
  || fail "drain_requested=$(get_stat drain_requested)，期望 1——信号链路没通"
[[ "$(get_stat drain_in_flight_at_entry)" == "1" ]] \
  && ok "drain_in_flight_at_entry=1：第一次扫描确实抓到了那条在飞请求" \
  || fail "drain_in_flight_at_entry=$(get_stat drain_in_flight_at_entry)，期望 1"
[[ "$(get_stat responses_completed)" == "1" ]] \
  && ok "responses_completed=1" || fail "responses_completed=$(get_stat responses_completed)，期望 1"
[[ "$(get_stat drain_forced_timeout)" == "0" ]] \
  && ok "drain_forced_timeout=0：走的是「等到了」而不是兜底（兜底给的是 60000ms）" \
  || fail "drain_forced_timeout=$(get_stat drain_forced_timeout)，期望 0"
[[ "$(get_stat drain_closed_idle)" == "1" ]] \
  && ok "drain_closed_idle=1：答完之后它成空闲下游被立即关" \
  || fail "drain_closed_idle=$(get_stat drain_closed_idle)，期望 1"
check_lifecycle 阶段一

# ------------------------------------------------------------------ 二
# 裁决第一条的后半句：空闲连接（在飞==0）**立即关**，不进兜底队列。
# 判据是时间差而不是"等 100ms 看关没关"：兜底写着 60000ms，若空闲连接也在等兜底，
# 网关就要整整一分钟才退出——这个差别一眼可见，而且不随机器快慢变形。
echo
echo "== 阶段二：空闲连接立即关（3 条连上不发请求，兜底 60000ms）"
start_upstream idle --fixed-body "S205-IDLE" || exit 1
start_gateway idle --drain-timeout-ms 60000 || exit 1
FDS=()
for _ in 1 2 3; do
  F=""; open_conn F || { for X in "${FDS[@]}"; do close_conn "$X"; done; exit 1; }
  FDS+=("$F")
done
sleep 1
t0=$(date +%s.%N)
kill -TERM "$G_PID" 2>/dev/null
wait_gateway_exit idle 20 || { for X in "${FDS[@]}"; do close_conn "$X"; done; stop_upstream; exit 1; }
t1=$(date +%s.%N)
for X in "${FDS[@]}"; do close_conn "$X"; done
stop_upstream
elapsed=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.2f", b-a}')
echo "  从 SIGTERM 到自己退出=${elapsed}s（兜底 60s），网关退出码=$G_RC"
awk -v e="$elapsed" 'BEGIN{exit !(e < 5.0)}' \
  && ok "空闲连接没在等兜底：elapsed=${elapsed}s << 60s" \
  || fail "elapsed=${elapsed}s：空闲连接走了兜底，没做到「立即关」"
[[ "$(get_stat drain_closed_idle)" == "3" ]] \
  && ok "drain_closed_idle=3：三条空闲下游各计一次" \
  || fail "drain_closed_idle=$(get_stat drain_closed_idle)，期望 3"
[[ "$(get_stat drain_forced_timeout)" == "0" ]] \
  && ok "drain_forced_timeout=0：一条都没落到兜底格" \
  || fail "drain_forced_timeout=$(get_stat drain_forced_timeout)，期望 0"
[[ "$(get_stat downstream_accepted)" == "3" ]] \
  && ok "downstream_accepted=3：三条连接都被正常 accept 过（不是没连上）" \
  || fail "downstream_accepted=$(get_stat downstream_accepted)，期望 3"
check_lifecycle 阶段二

# ------------------------------------------------------------------ 三
# 兜底是**时限**不是"尽快"：半条请求既不算空闲（缓冲里有字节）、也没有在飞（从没被接受），
# 按口径只能交给兜底。这条同时钉两端：不许早于 deadline 关（那等于无声丢掉还在路上的
# 连接），也不许拖过 deadline 太多（那正是"卡死客户端无限拖住下线"要防的形状）。
echo
echo "== 阶段三：半条请求走超时兜底（--drain-timeout-ms 800）"
start_upstream backstop --fixed-body "S205-BACKSTOP" || exit 1
start_gateway backstop --drain-timeout-ms 800 || exit 1
C3=""; open_conn C3 || exit 1
printf 'GET /x HTTP/1.1\r\nHost: smoke.test\r\nX: half' >&"$C3"   # 故意不写空行收尾
sleep 0.5
t0=$(date +%s.%N)
kill -TERM "$G_PID" 2>/dev/null
N=$(read_to_eof "$C3" "$OUT/proxy-backstop.txt" 15)
close_conn "$C3"
wait_gateway_exit backstop 20 || { stop_upstream; exit 1; }
t1=$(date +%s.%N)
stop_upstream
elapsed=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.2f", b-a}')
echo "  elapsed=${elapsed}s（deadline=0.8s），客户端收到 ${N} 字节，网关退出码=$G_RC"
awk -v e="$elapsed" 'BEGIN{exit !(e >= 0.7)}' \
  && ok "没有提前强关：elapsed=${elapsed}s ≥ deadline" \
  || fail "elapsed=${elapsed}s < 0.7s：兜底没守时限，等于无声丢掉还在路上的连接"
awk -v e="$elapsed" 'BEGIN{exit !(e < 8.0)}' \
  && ok "也没被无限拖住：elapsed=${elapsed}s（卡死的客户端不能决定下线时间）" \
  || fail "elapsed=${elapsed}s：兜底没生效"
[[ "$N" == "0" ]] && ok "客户端收到 0 字节：兜底强关没有替它编一条错误响应" \
  || fail "客户端收到 ${N} 字节，期望 0（半包不该换来任何伪造内容）"
[[ "$(get_stat drain_forced_timeout)" == "1" ]] \
  && ok "drain_forced_timeout=1：兜底确实动过一次" \
  || fail "drain_forced_timeout=$(get_stat drain_forced_timeout)，期望 1"
[[ "$(get_stat drain_closed_idle)" == "0" ]] \
  && ok "drain_closed_idle=0：这条不空闲（缓冲里有半包），没被误记成空闲格" \
  || fail "drain_closed_idle=$(get_stat drain_closed_idle)，期望 0"
[[ "$(get_stat requests_parsed)" == "0" ]] \
  && ok "requests_parsed=0：半包不算一条请求" \
  || fail "requests_parsed=$(get_stat requests_parsed)，期望 0"
check_lifecycle 阶段三

# ------------------------------------------------------------------ 四
# 隧道只能硬关（ADR-09 不解析帧 → 发不出 WS close 握手）。判据写成**字节数恰好相等**：
# 总量必须正好 = 101 头 + 我发出去的那 2048 帧字节。多一个字节就是网关在我从没解析过的
# 东西上编了内容（伪造 close 握手），少一个字节就是已缓冲的帧被排水丢掉。
echo
echo "== 阶段四：隧道硬关（不伪造 close 握手，已缓冲帧字节不丢）"
start_upstream tunnel --fixed-body "S205-TUNNEL" || exit 1
start_gateway tunnel --drain-timeout-ms 60000 || exit 1
C4=""; open_conn C4 || exit 1
printf 'GET /ws HTTP/1.1\r\nHost: smoke.test\r\nConnection: keep-alive, Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n' >&"$C4"
FRAME=$(head -c 2048 /dev/zero | tr '\0' '\xaa')
printf '%s' "$FRAME" >&"$C4"
# **刻意不先读**：先读就把 101 头和回显分成两次收了，那样"尾巴上有没有多出来的字节"
# 就没有判据。排水之后一次性读到 EOF，文件里应当正好是 [101 头][2048 帧回显]。
sleep 1  # 让这一帧走完 客户端→网关→上游→网关→客户端 的透传
kill -TERM "$G_PID" 2>/dev/null
N=$(read_to_eof "$C4" "$OUT/proxy-tunnel.bin" 15)
close_conn "$C4"
wait_gateway_exit tunnel 60 || { stop_upstream; exit 1; }
stop_upstream
HEADLEN=$(head_len_of "$OUT/proxy-tunnel.bin")
echo "  客户端共收到 ${N} 字节，101 头=${HEADLEN} 字节，网关退出码=$G_RC"
if [[ "$HEADLEN" == "-1" ]]; then
  fail "读不到 101 头的终止串：这一阶段的前置条件（握手成立）就没满足"
else
  [[ "$(head -c 12 "$OUT/proxy-tunnel.bin")" == "HTTP/1.1 101" ]] \
    && ok "开头就是 HTTP/1.1 101：升级握手成立了" \
    || fail "101 没回来，开头是 \"$(head -c 12 "$OUT/proxy-tunnel.bin")\""
  if [[ "$N" != "-1" ]] && (( N == HEADLEN + 2048 )); then
    ok "收到的字节数**恰好** = 101 头 + 2048 帧：没丢、也没伪造 close 握手"
  else
    fail "字节数对不上：收到 ${N}，期望 $((HEADLEN + 2048))（头 $HEADLEN + 帧 2048）"
  fi
  if tail -c 2048 "$OUT/proxy-tunnel.bin" | cmp -s - <(printf '%s' "$FRAME"); then
    ok "尾部 2048 字节与发出的帧逐字节相同（透传不改写，ADR-09）"
  else
    fail "帧字节与发出不一致——被改写了"
  fi
fi
[[ "$(get_stat drain_tunnels_hard_closed)" == "1" ]] \
  && ok "drain_tunnels_hard_closed=1：配对走的是硬关分支（按下游侧计一次）" \
  || fail "drain_tunnels_hard_closed=$(get_stat drain_tunnels_hard_closed)，期望 1"
[[ "$(get_stat drain_forced_timeout)" == "0" ]] \
  && ok "drain_forced_timeout=0：隧道是被硬关收掉的，不是挂到兜底（101 不走 kComplete，在飞永不为零）" \
  || fail "drain_forced_timeout=$(get_stat drain_forced_timeout)，期望 0"
[[ "$(get_stat upgrades_101)" == "1" ]] && ok "upgrades_101=1" \
  || fail "upgrades_101=$(get_stat upgrades_101)，期望 1"
[[ "$(get_stat requests_parsed)" == "1" ]] \
  && ok "requests_parsed=1：帧字节一次都没进解析器（ADR-09 复查）" \
  || fail "requests_parsed=$(get_stat requests_parsed)，期望 1"
check_lifecycle 阶段四

# ------------------------------------------------------------------ 五
# 多 worker 协调（"停接受 → 排水 → 超时强关 → join" 里最容易被写错的那一半）：
# 三条连接 round-robin 落到三条 worker，每条 worker 各自排干、各自置停止标志，
# accept 循环读到"全都在停止状态"才 join——这条只有真进程 + 真线程能证。
# 顺带钉住"排水期间不再接受新请求"：窗口里连上来的那条不该被服务。
echo
echo "== 阶段五：3 worker 各自排干 + 排水期间新连接不被服务"
start_upstream group --stall-reads-ms 4000 --fixed-body "S205-GROUP" || exit 1
start_gateway group --workers 3 --drain-timeout-ms 60000 || exit 1
GF=()
for i in 1 2 3; do
  F=""; open_conn F || { for X in "${GF[@]}"; do close_conn "$X"; done; exit 1; }
  send_get "$F" "g$i"
  GF+=("$F")
done
sleep 1  # 三条请求都转出去了（上游停读，响应回不来）
kill -TERM "$G_PID" 2>/dev/null
sleep 1  # 排水窗口开着的时候试一条新连接
CL=""; open_conn CL || { for X in "${GF[@]}"; do close_conn "$X"; done; exit 1; }
send_get "$CL" late
NLATE=$(read_to_eof "$CL" "$OUT/proxy-late.txt" 6)
close_conn "$CL"
sleep 4  # stall 到点：三条在飞请求的响应必须都回来
for idx in "${!GF[@]}"; do
  F="${GF[$idx]}"
  read_to_eof "$F" "$OUT/proxy-group-$F.txt" 15 >/dev/null
done
for X in "${GF[@]}"; do close_conn "$X"; done
wait_gateway_exit group 60 || { stop_upstream; exit 1; }
stop_upstream
echo "  排水期间那条新连接收到 ${NLATE} 字节；网关退出码=$G_RC"
groupok=1
for F in "${GF[@]}"; do
  check_response_complete "阶段五 fd=$F" "$OUT/proxy-group-$F.txt" || groupok=0
done
(( groupok == 1 )) && ok "三条 worker 各自把**它自己那条**在飞请求答完了（排水是按 worker 各排的）" \
  || fail "有 worker 没把它的在飞请求答完"
[[ "$NLATE" == "0" ]] \
  && ok "排水期间的新连接一条响应都没收到就被关了（没被服务、也没挂着没人理）" \
  || fail "排水期间的新连接收到了 ${NLATE} 字节——「不再接受新请求」没落实"
[[ "$(get_stat requests_parsed)" == "3" ]] \
  && ok "requests_parsed=3：被拒的那条连解析都没进" \
  || fail "requests_parsed=$(get_stat requests_parsed)，期望 3"
[[ "$(get_stat drain_requested)" == "3" ]] \
  && ok "drain_requested=3：每条事件循环各计一次（accept 循环不持有连接，不计数）" \
  || fail "drain_requested=$(get_stat drain_requested)，期望 3（等于 worker 数）"
[[ "$(get_stat drain_in_flight_at_entry)" == "3" ]] \
  && ok "drain_in_flight_at_entry=3：三条 worker 的第一次扫描求和对得上（跨 worker 用 fetch_add）" \
  || fail "drain_in_flight_at_entry=$(get_stat drain_in_flight_at_entry)，期望 3"
[[ "$(get_stat drain_rejected_new_connections)" =~ ^[0-9]+$ ]] && (( $(get_stat drain_rejected_new_connections) >= 1 )) \
  && ok "drain_rejected_new_connections=$(get_stat drain_rejected_new_connections) ≥1：「停止接受新连接」有可断言的形态" \
  || fail "drain_rejected_new_connections=$(get_stat drain_rejected_new_connections)，期望 ≥1"
[[ "$(get_stat responses_completed)" == "3" ]] \
  && ok "responses_completed=3" || fail "responses_completed=$(get_stat responses_completed)，期望 3"
[[ "$(get_stat drain_forced_timeout)" == "0" ]] \
  && ok "drain_forced_timeout=0：没有一条走兜底" \
  || fail "drain_forced_timeout=$(get_stat drain_forced_timeout)，期望 0"
[[ "$(get_stat workers)" == "3" ]] && ok "workers=3：本次确实走的是多 Reactor 路径" \
  || fail "workers=$(get_stat workers)，期望 3"
check_lifecycle 阶段五

REACHED_END=1
echo
if (( FAILED == 0 )); then
  echo "S2-05 冒烟：五个阶段全绿（fail 计数 0，退出码 0）"
else
  echo "S2-05 冒烟：有 ✗，见上面"
fi
exit "$FAILED"
