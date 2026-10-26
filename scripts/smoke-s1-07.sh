#!/usr/bin/env bash
# S1-07 冒烟：在**真进程 + 真 socket** 上把「撞停读线 → 摘 EPOLLIN → 回落 → 恢复」这条链
# 走一遍，并把 TASKS.md 那一行还空着的「高水位触发时 RSS」取出数来。
#
# 与 ctest 的分工先写死，不靠读者猜。平台期的**严格断言**（入向读取字节数两次采样相同）
# 只在 ctest 里做：它需要测试线程在循环跑着的时候切 stall 开关，还需要一个能一次推
# 3.17 MB 的客户端。这一步交的是另外四件事：
#   一、真 curl 当慢客户端（--limit-rate）时，响应方向的 pause/resume 确实发生，
#       1 MiB 的字节一条不少、且与直连逐字节相同；
#   二、/dev/tcp 在一条 keep-alive 上连打 400 条 8 KiB 的 POST（≈3.3 MB）打在
#       --stall-reads-ms 的慢上游上，stall 结束之后 400 条响应全部回来——
#       这才是"恢复"在进程级的形状。curl 做不到这一条：它不 pipeline，
#       一条请求必须等它的响应，压强永远堆不到停读线；
#   三、把停读线压到比一条请求还小（4 KiB 的线 + 100 KiB 的 POST），那条请求**停在
#       线上**：不丢、不报错、不 5xx，而且 `read_disarmed_connections=1` —— 这是
#       "网关不再消费下游可读事件"这句话在进程侧唯一能被直接读出来的形式；
#   四、上游 accept 之后**半条响应头**就关（切在中间、远不到响应头预算）——
#       FAULT-INJECTION §5 里归属本任务的那条缺口。
#
# 这个脚本不产出任何性能数字：--limit-rate 的节奏是 curl 自己的，混不进 p99（同 S1-06）。
#
# 用法：PRESET=debug scripts/smoke-s1-07.sh
# 输出：build/smoke-s1-07/ 里的响应原文、各步日志、RSS 采样；退出码 0 = 全部通过。
set -uo pipefail
cd /home/archer/l7gate || exit 1

PRESET="${PRESET:-debug}"
BIN="build/$PRESET"
GATEWAY="$BIN/l7gate"
UPSTREAM_BIN="$BIN/tests/l7gate_test_upstream"
GATEWAY_PORT="${GATEWAY_PORT:-18081}"
OUT="build/smoke-s1-07"
mkdir -p "$OUT"

FAILED=0
fail() { echo "  ✗ $*"; FAILED=1; }
ok()   { echo "  ✓ $*"; }

[[ -x "$GATEWAY" ]] || { echo "找不到 $GATEWAY，先 cmake --build --preset $PRESET" >&2; exit 1; }
[[ -x "$UPSTREAM_BIN" ]] || { echo "找不到 $UPSTREAM_BIN，先 cmake --build --preset $PRESET" >&2; exit 1; }
command -v curl >/dev/null || { echo "本机没有 curl：慢客户端这条证据不成立" >&2; exit 1; }
curl --version | head -1 | sed 's/^/  curl: /'

UP_PID=""; G_PID=""; RSS_PID=""; UP_PORT=""; UP_LOG=""; G_LOG=""
cleanup() {
  [[ -n "$RSS_PID" ]] && kill -TERM "$RSS_PID" 2>/dev/null
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

# RSS 采样：100 ms 一次，VmRSS 与 VmHWM 都抓。
# 「高水位触发时 RSS」这个数取的是**压力窗口内的峰值**，不是我某一瞬间看到的值：
# 停读动作在循环线程里完成，外部没有任何时刻能让我恰好醒在撞线那一刻。VmHWM 由内核
# 记账，不受采样间隔影响，所以两个都留下——前者是"我看到过的最大"，后者是"真实最大"。
start_rss_sampler() { # tag
  local tag=$1
  local log="$OUT/rss-$tag.log"
  : > "$log"
  # 用 grep 取字段而不是 awk 的正则+printf 程序：awk 在这里踩过一次语法坑（mawk 对
  # `/^A:|^B:/ {...}` 那一条报 syntax error），而采样失败是**静默**的（输出被 2>/dev/null
  # 吞掉、日志文件留 0 字节），第一次跑就是这样把 RSS 读成 "?" 的。grep 的输出形状上面
  # 直接验证过，取最大值交给只干一件事的 awk。
  ( while kill -0 "$G_PID" 2>/dev/null; do
      grep -E '^VmRSS:|^VmHWM:' "/proc/$G_PID/status" >> "$log" 2>/dev/null
      sleep 0.1
    done ) &
  RSS_PID=$!
}
stop_rss_sampler() {
  [[ -n "$RSS_PID" ]] || return 0
  kill -TERM "$RSS_PID" 2>/dev/null; wait "$RSS_PID" 2>/dev/null
  RSS_PID=""
}
rss_peak() { # tag field —— 该步采样窗口内这个字段的最大值（KiB）
  local field=${2:-VmRSS:}
  grep "^$field" "$OUT/rss-$1.log" 2>/dev/null | awk '{if ($2+0 > m) m = $2 + 0} END {print (m ? m : "?")}'
}

stop_pair() { # tag
  local tag=$1
  kill -TERM "$G_PID" 2>/dev/null
  for _ in $(seq 1 200); do kill -0 "$G_PID" 2>/dev/null || break; sleep 0.1; done
  if kill -0 "$G_PID" 2>/dev/null; then
    fail "网关收到 SIGTERM 之后 20s 还没停（$tag）"; kill -KILL "$G_PID" 2>/dev/null
  fi
  wait "$G_PID" 2>/dev/null; local g_rc=$?
  kill -TERM "$UP_PID" 2>/dev/null; wait "$UP_PID" 2>/dev/null
  (( g_rc == 0 )) || fail "网关退出码 $g_rc（$tag）：SIGTERM 的优雅停止没生效"
}

get_stat() { sed -n "s/^\[serve\] $1=\([0-9-]*\).*/\1/p" "$G_LOG" | head -1; }
stat_of() { # tag key —— 汇总段用，那时 $G_LOG 已经指向最后一步了
  sed -n "s/^\[serve\] $2=\([0-9-]*\).*/\1/p" "$OUT/gateway-$1.log" | head -1;
}
first_line() { head -1 "$1" | tr -d '\r'; }
header_of() { sed -n "s/^$2: *//p" "$1" | head -1 | tr -d '\r'; }
bytes_after_head() {
  awk 'BEGIN{RS="\r\n\r\n"; v=0} NR==2 && !done {gsub(/\r/, ""); v=length($0); done=1} END{print v}' "$1"
}
# 一份原始响应里有没有出现过"终止头"的字节序列（0d0a0d0a = CRLFCRLF）。
# 用十六进制而不是 grep 空行：grep 的模式一旦依赖行对齐，就是一条迟早自己骗自己的断言
# （S1-06 阶段二比 last-chunk 时踩过同一次）。
has_head_terminator() { od -An -tx1 "$1" | tr -d ' \n' | grep -q '0d0a0d0a'; }
# pause/resume/撞线 三个数一起看：只有 pause 没有 resume 是"停了没恢复"，两个都没有
# 就是这条链根本没跑。阶段三反过来——那里只许 pause，恢复没有资格发生。
expect_flow() { # tag
  local tag=$1
  local pause resume hits
  pause=$(get_stat backpressure_pause_count)
  resume=$(get_stat backpressure_resume_count)
  hits=$(get_stat high_water_hits)
  if [[ -n "$pause" && -n "$resume" ]] && (( pause >= 1 )) && (( resume >= 1 )); then
    ok "$tag：pause=$pause resume=$resume 撞线=$hits —— 摘过读，也挂回来过"
  else
    fail "$tag：pause=${pause:-?} resume=${resume:-?} 撞线=${hits:-?}，期望两者都 ≥1"
  fi
}
# 背压改动的是订阅掩码，掩码改动最容易留下的就是"连接带着摘掉的 EPOLLIN 被关掉"，
# 所以正常收尾这一步在 S1-06 那三条之外多加一条 read_disarmed_connections==0。
check_lifecycle() { # tag
  local tag=$1
  local live constructed destructed fd_from fd_to disarmed
  live=$(get_stat live_connections)
  constructed=$(get_stat connections_constructed)
  destructed=$(get_stat connections_destructed)
  fd_from=$(get_stat fd_start); fd_to=$(get_stat fd_end)
  disarmed=$(get_stat read_disarmed_connections)
  if [[ "${live:-?}" == "0" ]]; then ok "$tag：live_connections=0，循环退出时注册表是空的"
  else fail "$tag：live_connections=${live:-?}，期望 0（有连接没走关闭路径）"; fi
  if [[ -n "$constructed" && "$constructed" == "$destructed" ]]; then
    ok "$tag：constructed=destructed=$constructed，引用计数确实归零"
  else
    fail "$tag：构造 $constructed / 析构 ${destructed:-?} 不等"
  fi
  if [[ -n "$fd_from" && "$fd_from" == "$fd_to" ]]; then ok "$tag：fd $fd_from → $fd_to 回到基线"
  else fail "$tag：fd ${fd_from:-?} → ${fd_to:-?} 不等，有 fd 没关"; fi
  if [[ "${disarmed:-?}" == "0" ]]; then ok "$tag：read_disarmed_connections=0，没有连接被留在停读态"
  else fail "$tag：read_disarmed_connections=${disarmed:-?}，期望 0（有连接带着摘掉的 EPOLLIN 走到收尾）"; fi
}

# ------------------------------------------------------------------ 一
# 响应方向：真 curl 用 --limit-rate 当一个慢到看得见的客户端。停读线夹到 64 KiB
# （默认 256 KiB 在回环上要靠更大的真实流量才顶得动，取证不稳定）。
echo "== 阶段一：响应方向 —— curl --limit-rate 慢客户端 + 1 MiB 响应"
RESP_BYTES=1048576
start_upstream slow-client --body-bytes "$RESP_BYTES" || exit 1
start_gateway slow-client --high-water-mark 65536 || exit 1
start_rss_sampler slow-client
# 直连那趟**不限速**：它的作用只有一个——证明这些字节本来长什么样，网关没资格改。
curl -sS -i --raw --max-time 30 "http://127.0.0.1:$UP_PORT/plain" \
  > "$OUT/direct-1m.txt" 2> "$OUT/direct-1m.err"; D_RC=$?
# 经网关这趟 200 KiB/s：上游的产出速度远高于客户端的消费速度，网关往这条连接压的字节
# 就会先顶满它自己的出向缓冲，再顶到上游那一侧的入向停读线。
curl -sS -i --raw --limit-rate 200k --max-time 60 "http://127.0.0.1:$GATEWAY_PORT/plain" \
  > "$OUT/proxy-1m.txt" 2> "$OUT/proxy-1m.err"; P_RC=$?
stop_rss_sampler
echo "  curl 退出码 直连=$D_RC / 经网关=$P_RC；经网关收到 $(wc -c < "$OUT/proxy-1m.txt") 字节"
[[ "$(first_line "$OUT/proxy-1m.txt")" == "HTTP/1.1 200 OK" ]] \
  && ok "状态行 200：慢客户端没有让网关改口" || fail "状态行：$(first_line "$OUT/proxy-1m.txt")"
[[ "$(header_of "$OUT/proxy-1m.txt" 'Content-Length')" == "$RESP_BYTES" ]] \
  && ok "Content-Length=$RESP_BYTES 原样送达" \
  || fail "Content-Length=$(header_of "$OUT/proxy-1m.txt" 'Content-Length')，期望 $RESP_BYTES"
[[ "$(bytes_after_head "$OUT/proxy-1m.txt")" == "$RESP_BYTES" ]] \
  && ok "body 实到 $RESP_BYTES 字节：限速客户端没有被背压漏掉任何一块" \
  || fail "body 实到 $(bytes_after_head "$OUT/proxy-1m.txt") 字节，期望 $RESP_BYTES"
# 这一步比的是**长度**，不是字节。夹具的模板体里带 `seq=<累计条数>`，直连那趟和经网关
# 那趟取到的是两个不同的序号，所以逐字节比对在这里必然红——那不是我测出来的差异，
# 是我选的口径不可比。"逐字节一致"这条归 S1-06（那里用 --fixed-body 把体钉死之后比），
# 这里要的是另一件事：背压期间**一块都没少**。
if [[ "$(wc -c < "$OUT/direct-1m.txt")" == "$(wc -c < "$OUT/proxy-1m.txt")" ]]; then
  ok "直连与经网关的原始响应同为 $(wc -c < "$OUT/proxy-1m.txt") 字节：限速 + 摘读没有吃掉任何一块"
else
  fail "直连 $(wc -c < "$OUT/direct-1m.txt") 字节 / 经网关 $(wc -c < "$OUT/proxy-1m.txt") 字节，长度不等"
fi
(( P_RC == 0 )) || fail "curl 退出码 $P_RC：这条响应在客户端看来不完整"
stop_pair slow-client
expect_flow slow-client
[[ "$(get_stat responses_completed)" == "1" ]] && ok "responses_completed=1" \
  || fail "responses_completed=$(get_stat responses_completed)，期望 1"
[[ "$(get_stat gateway_replies_sent)" == "0" ]] && ok "gateway_replies_sent=0：慢客户端不该换来网关自产的表态" \
  || fail "gateway_replies_sent=$(get_stat gateway_replies_sent)，期望 0"
[[ "$(get_stat bytes_read_upstream)" == "$(get_stat bytes_up_to_downstream)" ]] \
  && ok "上游入向=$(get_stat bytes_read_upstream) == 交给下游=$(get_stat bytes_up_to_downstream)：搬进多少就送出多少" \
  || fail "上游入向 $(get_stat bytes_read_upstream) 与下游出向 $(get_stat bytes_up_to_downstream) 不等"
check_lifecycle slow-client

# ------------------------------------------------------------------ 二
# 请求方向：一条 keep-alive 上连打 400 条 8 KiB 的 POST，上游 --stall-reads-ms 8000
# 期间一个字节都不消费。压强必须明显超过网关→上游这段内核路径能吸收的量（ctest 实测
# ≈1.9 MB），否则"平台期"是写侧没货而不是网关摘读——同一个理由在 test_upstream.h 的
# 删除记录里。每条请求 8,252 字节 < 停读线 32 KiB，所以"整块才转"不被违反：
# 这条用例压的是**条数**，不是单条的尺寸。
# 读侧不能等 EOF：keep-alive 上不会来 EOF，而我在这一版里试过用最后一条带
# `Connection: close` 的请求去制造 EOF——它确实制造了，但是在**第一条响应完成时**就制造了
# （400 条请求挤在同一批里被解析，close 意图是"下一条响应之后收尾"，它兑现的正是字面意思）。
# 那是 pipeline 语义的边界、不是 S1 声明支持的东西（S1 只声明 keep-alive 复用），
# 所以这里把 close 摘掉，改成"数够条数就自己关连接"，并把那条边界记进文档。
echo
echo "== 阶段二：请求方向 —— 400 条 pipelined POST 打在 stall 中的上游，再看恢复"
UP_STALL_MS=8000
REQ_COUNT=400
REQ_BODY="$(head -c 8192 /dev/zero | tr '\0' 'x')"
PLAIN_REQ_FILE="$OUT/plain-request.bin"
printf 'POST /b HTTP/1.1\r\nHost: smoke.test\r\nContent-Length: 8192\r\n\r\n%s' "$REQ_BODY" \
  > "$PLAIN_REQ_FILE"
PLAIN_BYTES=$(wc -c < "$PLAIN_REQ_FILE")
EXPECT_DOWN=$(( REQ_COUNT * PLAIN_BYTES ))
echo "  单条 $PLAIN_BYTES 字节，共 $REQ_COUNT 条 = $EXPECT_DOWN 字节"
PIPELINE="$OUT/pipeline.bin"
PLAIN_REQ="$(cat "$PLAIN_REQ_FILE")"
start_upstream stall-req --stall-reads-ms "$UP_STALL_MS" --fixed-body "S107-FIXED-BODY" || exit 1
start_gateway stall-req --high-water-mark 32768 || exit 1
start_rss_sampler stall-req
exec 3<>/dev/tcp/127.0.0.1/$GATEWAY_PORT || { fail "/dev/tcp 连不上网关"; exit 1; }
# 写侧放进后台：stall 期间它一定会卡在 write() 上（网关摘了读、内核路径也满了），
# 这正是压强到位的形状。断言因此可以站在另一侧做。
{ for (( i = 1; i <= REQ_COUNT; ++i )); do printf '%s' "$PLAIN_REQ" >&3; done; } &
WRITER_PID=$!
echo "  写侧 pid=$WRITER_PID 已在打；上游 stall ${UP_STALL_MS}ms 期间采样 RSS"
: > "$PIPELINE"
( timeout 120 cat <&3 > "$PIPELINE" 2>/dev/null ) &
READ_PID=$!
sleep "$((UP_STALL_MS / 1000))"
echo "  stall 窗口内 VmRSS 峰值=$(rss_peak stall-req) KiB / VmHWM=$(rss_peak stall-req VmHWM:) KiB"
# 数到 400 条就收手。恢复链路如果只挂在"下一次事件"上，这个循环永远等不到 400，
# 用例就红在条数上——那正是边沿触发睡死在进程级的表现方式。
GOT_STATUS=0
for (( i = 0; i < 120; ++i )); do
  GOT_STATUS=$(grep -c 'HTTP/1.1 200 OK' "$PIPELINE" 2>/dev/null || true)
  (( GOT_STATUS >= REQ_COUNT )) && break
  sleep 0.5
done
kill -TERM "$READ_PID" 2>/dev/null; wait "$READ_PID" 2>/dev/null
exec 3<&- 3>&-
wait "$WRITER_PID" 2>/dev/null
stop_rss_sampler
echo "  收到 $(wc -c < "$PIPELINE") 字节，状态行 $GOT_STATUS 条（期望 $REQ_COUNT）；自己关掉这条下游连接"
if (( GOT_STATUS == REQ_COUNT )); then
  ok "恢复之后 $REQ_COUNT 条响应一条不少全部到达"
else
  fail "响应条数 $GOT_STATUS / 期望 $REQ_COUNT（差多少就是背压期间掉了多少）"
fi
stop_pair stall-req
expect_flow stall-req
[[ "$(get_stat requests_parsed)" == "$REQ_COUNT" ]] && ok "requests_parsed=$REQ_COUNT：每条请求都被解析" \
  || fail "requests_parsed=$(get_stat requests_parsed)，期望 $REQ_COUNT"
[[ "$(get_stat responses_completed)" == "$REQ_COUNT" ]] && ok "responses_completed=$REQ_COUNT" \
  || fail "responses_completed=$(get_stat responses_completed)，期望 $REQ_COUNT"
[[ "$(get_stat upstream_opened)" == "1" ]] \
  && ok "upstream_opened=1：$REQ_COUNT 条请求共用一条回源连接（stall 没有逼出重连）" \
  || fail "upstream_opened=$(get_stat upstream_opened)，期望 1"
[[ "$(get_stat bytes_down_to_upstream)" == "$EXPECT_DOWN" ]] \
  && ok "bytes_down_to_upstream=$EXPECT_DOWN：请求方向字节零丢失" \
  || fail "bytes_down_to_upstream=$(get_stat bytes_down_to_upstream)，期望 $EXPECT_DOWN"
[[ "$(get_stat bytes_read_downstream)" == "$EXPECT_DOWN" ]] \
  && ok "bytes_read_downstream=$EXPECT_DOWN：读进来的与转出去的同一本账（没有反复读）" \
  || fail "bytes_read_downstream=$(get_stat bytes_read_downstream)，期望 $EXPECT_DOWN"
[[ "$(get_stat gateway_replies_sent)" == "0" ]] && ok "gateway_replies_sent=0：全程没有一次自产表态" \
  || fail "gateway_replies_sent=$(get_stat gateway_replies_sent)，期望 0"
check_lifecycle stall-req

# ------------------------------------------------------------------ 三
# 停读线之下的边界：线夹到 4 KiB，再发一条 100 KiB 的 POST。
# 这条**不该被当成缺陷**：转发的最小单位是"整条请求"（`queue_bytes` 的整块判断），
# 线比一条请求还小，那条请求就永远装不进缓冲——它停在停读线上等人来处理，不丢、
# 不报错、不 5xx。1.15 里那句话需要一个阶段让它在真实进程里发生一次。
# 而 `read_disarmed_connections=1` 是这一步真正的收获：验收句"网关不再消费下游可读事件"
# 在进程侧只能这样读——循环已经停下，还带着没有 EPOLLIN 的掩码的那条连接就是被按住的那个。
echo
echo "== 阶段三：--high-water-mark 4096 + 100 KiB 请求 —— 停在停读线上，不丢不报错"
BIG_FILE="$OUT/big-body.bin"
head -c 102400 /dev/zero | tr '\0' 'y' > "$BIG_FILE"
start_upstream tiny-line --body-bytes 64 || exit 1
start_gateway tiny-line --high-water-mark 4096 || exit 1
start_rss_sampler tiny-line
# `--header 'Expect:'` 关掉 curl 的 100-continue：那条请求要**一次**压满 100 KiB，
# 中间不能停下来等网关说"可以继续"（网关此刻正在停读，那个等待就变成纯粹的 curl 行为）。
curl -sS -i --max-time 6 -X POST --data-binary "@$BIG_FILE" --header 'Expect:' \
  "http://127.0.0.1:$GATEWAY_PORT/plain" \
  > "$OUT/proxy-tiny.txt" 2> "$OUT/proxy-tiny.err"; T_RC=$?
stop_rss_sampler
echo "  请求体 102400 字节；curl 退出码=$T_RC（28=超时），收到 $(wc -c < "$OUT/proxy-tiny.txt") 字节"
if [[ "$T_RC" == "28" ]] && [[ ! -s "$OUT/proxy-tiny.txt" ]]; then
  ok "curl 超时且一个字节都没收到：网关没有丢弃它，也没有替它编一条错误出来"
else
  fail "期望退出码 28 + 零字节，实测退出码 $T_RC / $(wc -c < "$OUT/proxy-tiny.txt") 字节"
fi
stop_pair tiny-line
[[ "$(get_stat requests_parsed)" == "0" ]] \
  && ok "requests_parsed=0：100 KiB 的请求在 4 KiB 的线下面永远拼不完整" \
  || fail "requests_parsed=$(get_stat requests_parsed)，期望 0"
[[ "$(get_stat upstream_opened)" == "0" ]] && ok "upstream_opened=0：没解析出请求就不该回源" \
  || fail "upstream_opened=$(get_stat upstream_opened)，期望 0"
[[ "$(get_stat gateway_replies_sent)" == "0" ]] && ok "gateway_replies_sent=0：没有自产 5xx" \
  || fail "gateway_replies_sent=$(get_stat gateway_replies_sent)，期望 0"
# `high_water_hits` 在这里是 **≥1 而不是 ==1**：口径从 S1-02 起就是"limit_reached 这个
# 事件发生了几次"，缓冲顶满之后每一次读尝试都会再报一次"停在停读线上"（这一步实测 2 次：
# 一次真读满 4096，一次在满缓冲上的空读）。真正说明"停在原地"的是下面那个字节数等式，
# 不是这个计数——所以这里断字节数、不断 hits 的个数。
[[ "$(get_stat high_water_hits)" =~ ^[0-9]+$ ]] && (( $(get_stat high_water_hits) >= 1 )) \
  && ok "high_water_hits=$(get_stat high_water_hits) ≥1：入向缓冲顶到了 4 KiB 这条线" \
  || fail "high_water_hits=$(get_stat high_water_hits)，期望 ≥1"
[[ "$(get_stat backpressure_pause_count)" == "1" ]] \
  && ok "pause=1：那一次顶线换来一次 EPOLLIN 摘除" \
  || fail "backpressure_pause_count=$(get_stat backpressure_pause_count)，期望 1"
[[ "$(get_stat backpressure_resume_count)" == "0" ]] \
  && ok "resume=0：没有东西被转走，水位从未回落——恢复在这里没有资格发生" \
  || fail "backpressure_resume_count=$(get_stat backpressure_resume_count)，期望 0"
[[ "$(get_stat read_disarmed_connections)" == "1" ]] \
  && ok "read_disarmed_connections=1：这条连接**真的**停在没有 EPOLLIN 的状态上被观察到" \
  || fail "read_disarmed_connections=$(get_stat read_disarmed_connections)，期望 1"
READ_IN=$(get_stat bytes_read_downstream)
if [[ -n "$READ_IN" ]] && (( READ_IN >= 4096 )) && (( READ_IN <= 8192 )); then
  ok "bytes_read_downstream=$READ_IN：读进去的就贴在线附近，100 KiB 没有进内存"
else
  fail "bytes_read_downstream=${READ_IN:-?}，期望 4096..8192"
fi
# 这一步**不做** check_lifecycle：注入的就是一条永不完结的连接，而网关的停止语义是
# "不再接受新连接 + 退出循环"，不排水在飞连接（FAULT-INJECTION §3 记过这条，真排水在
# S2 之后）。所以这里改成断"停在那里是低成本的"：退出码 0、fd 没涨、RSS 有界。
if [[ "$(get_stat live_connections)" == "1" ]]; then
  ok "live_connections=1：这条连接按停止语义留在原地（不排水是**已知**边界，不是本步的结论）"
else
  fail "live_connections=$(get_stat live_connections)，期望 1（不是 0：无声关掉一条卡住的连接，就等于把它丢了）"
fi
echo "  本步内存形状：rss_start_kib=$(get_stat rss_start_kib) → rss_end_kib=$(get_stat rss_end_kib)，压力窗口峰值=$(rss_peak tiny-line) KiB / VmHWM=$(rss_peak tiny-line VmHWM:) KiB"
echo "  本步 fd：fd_start=$(get_stat fd_start) → fd_end=$(get_stat fd_end)"
TINY_FD=$(get_stat fd_end)
if [[ -n "$TINY_FD" ]] && (( TINY_FD <= 12 )); then ok "fd_end=$TINY_FD：一条卡住的连接没带出多余 fd"; else fail "fd_end=${TINY_FD:-?} 偏大"; fi

# ------------------------------------------------------------------ 四
# FAULT-INJECTION §5 归属本任务的一条：上游 accept 之后**半条响应头**就关。
# 它和 /dies（零字节）与 /endless-head（永不齐、超预算）都不是同一条路径：这半条头很小，
# 远不到响应头预算，判死只能靠 EOF。口径上仍落 before_head——最终头没出现，网关仍有
# 自产表态的资格，所以这里是 502；而 S1-06 阶段五（头之后断）只剩连接终止。
# 与直连的**不对称是本步的结论**（S1-06 阶段五那里不对称就是 bug，这里反过来）：直连看到
# 半条头然后断开，客户端看到完整一条 502——网关把"没说完的话"换成"一句说得完的话"，
# 这是表态，不是替上级改写内容。
echo
echo "== 阶段四：上游回半条响应头就关（/dies-mid-head）→ 502，半条头不外漏"
start_upstream mid-head --body-bytes 1200 --fixed-body "S107-MIDHEAD" || exit 1
start_gateway mid-head --high-water-mark 65536 || exit 1
curl -sS -i --max-time 10 "http://127.0.0.1:$UP_PORT/dies-mid-head" \
  > "$OUT/direct-midhead.txt" 2> "$OUT/direct-midhead.err"; D_RC=$?
curl -sS -i --max-time 10 "http://127.0.0.1:$GATEWAY_PORT/dies-mid-head" \
  > "$OUT/proxy-midhead.txt" 2> "$OUT/proxy-midhead.err"; P_RC=$?
echo "  直连：退出码=$D_RC 拿到 $(wc -c < "$OUT/direct-midhead.txt") 字节（首行\"$(first_line "$OUT/direct-midhead.txt")\"）"
echo "  经网关：退出码=$P_RC 拿到 $(wc -c < "$OUT/proxy-midhead.txt") 字节"
if [[ ! -s "$OUT/direct-midhead.txt" ]]; then
  fail "直连拿到 0 字节：这一刀切到了头之外，和 /dies 重合了，先查夹具"
elif has_head_terminator "$OUT/direct-midhead.txt"; then
  fail "直连拿到了**完整**的头（有 CRLFCRLF）：夹具这一刀没切在头中间"
else
  ok "直连确实拿到一条**没说完**的头：$(wc -c < "$OUT/direct-midhead.txt") 字节里没有 CRLFCRLF"
fi
[[ "$(first_line "$OUT/proxy-midhead.txt")" == "HTTP/1.1 502 Bad Gateway" ]] \
  && ok "经网关拿到可定义的 5xx：$(first_line "$OUT/proxy-midhead.txt")" \
  || fail "经网关的状态行不是 502：$(first_line "$OUT/proxy-midhead.txt")"
if grep -q 'HTTP/1.1 200' "$OUT/proxy-midhead.txt"; then
  fail "半条头漏给了客户端：网关把没说完的话原样转了出去"
else
  ok "文件里没有任何 200 的痕迹：那半条头没有外漏，客户端只看到网关自己那条 502"
fi
[[ "$(header_of "$OUT/proxy-midhead.txt" 'Content-Length')" == "12" ]] \
  && [[ "$(bytes_after_head "$OUT/proxy-midhead.txt")" == "12" ]] \
  && ok "502 自带 Content-Length: 12 且 body 实到 12 字节，不是半包" \
  || fail "502 的 CL=$(header_of "$OUT/proxy-midhead.txt" 'Content-Length') / body=$(bytes_after_head "$OUT/proxy-midhead.txt")，期望都是 12"
[[ "$(header_of "$OUT/proxy-midhead.txt" 'Connection')" == "close" ]] \
  && ok "502 带 Connection: close" \
  || fail "502 的 Connection=$(header_of "$OUT/proxy-midhead.txt" 'Connection')，期望 close"
stop_pair mid-head
[[ "$(get_stat upstream_truncated_before_head)" == "1" ]] \
  && ok "upstream_truncated_before_head=1：半条头断与零字节断同一口径（最终头之前）" \
  || fail "upstream_truncated_before_head=$(get_stat upstream_truncated_before_head)，期望 1"
[[ "$(get_stat upstream_truncated_in_body)" == "0" ]] \
  && ok "upstream_truncated_in_body=0：这一刀没落在 body 里，两个计数分得开" \
  || fail "upstream_truncated_in_body=$(get_stat upstream_truncated_in_body)，期望 0"
[[ "$(get_stat gateway_replies_sent)" == "1" ]] && ok "gateway_replies_sent=1" \
  || fail "gateway_replies_sent=$(get_stat gateway_replies_sent)，期望 1"
[[ "$(get_stat responses_completed)" == "0" ]] && ok "responses_completed=0：没有任何响应被当成完结" \
  || fail "responses_completed=$(get_stat responses_completed)，期望 0"
check_lifecycle mid-head

# ------------------------------------------------------------------ 汇总
echo
echo "== 压力窗口内的 RSS（TASKS.md 的 S1-07「高水位触发时 RSS」取这一档）"
# 只列三个**有压强**的阶段。阶段四不采样：它证的是截断表态，全程没有连接停在停读线上，
# 给它配一个采样窗口只会多一个读不出东西的数。
for tag in slow-client stall-req tiny-line; do
  printf '  %-12s VmRSS 峰值=%s KiB / VmHWM=%s KiB  rss_start=%s → rss_end=%s  hwm=%s\n' \
    "$tag" "$(rss_peak "$tag")" "$(rss_peak "$tag" VmHWM:)" \
    "$(stat_of "$tag" rss_start_kib)" "$(stat_of "$tag" rss_end_kib)" \
    "$(sed -n 's/.*high_water_mark=\([0-9]*\).*/\1/p' "$OUT/gateway-$tag.log" | head -1)"
done

echo
echo "== 各步网关自报计数（全量留档）："
for f in "$OUT"/gateway-*.log; do
  echo "  --- $(basename "$f")"
  grep '^\[serve\]' "$f" | sed -e 's/^/      /' -e 's/\[serve\] //'
done

echo
if (( FAILED == 0 )); then
  echo "== S1-07 冒烟通过：preset=$PRESET 四个阶段全绿，证据在 $OUT/"
else
  echo "== S1-07 冒烟未通过：上面带 ✗ 的行每一项都是没达标的项目，不是警告"
fi
exit "$FAILED"
