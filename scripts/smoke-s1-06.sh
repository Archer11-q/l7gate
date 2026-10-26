#!/usr/bin/env bash
# S1-06 冒烟：用**真 curl** 把「直连上游」和「经网关转发」两条路各走一遍，逐字节比。
#
# 为什么这条必须是 curl，而不是只靠 framing_test.cc + reactor_test.cc：
# 单元用例两侧（客户端与网关）都是我写的，chunked 的"一致"可能只是我对框架的
# 理解在两处一致地错。curl 是第三方的——它自己实现了一套 chunked/Content-Length/
# 截断判定，它说"这两次拿到的字节一模一样"才是外部证据。反过来说，这个脚本
# **不产出任何性能数字**：curl 的串行往返里混着它自己的开销，p99 归 scripts/bench/。
#
# 每一步都单独起一对上游+网关，为的是"网关自报的计数只属于这一步"。把六个
# 形态塞进一条循环，最后一个 total 我分不清是哪一步贡献的——那种账不叫证据。
#
# 用法：PRESET=debug scripts/smoke-s1-06.sh
# 输出：build/smoke-s1-06/ 里的响应原文 + 各步日志；退出码 0 = 全部通过。
set -uo pipefail
cd "$(dirname "$(readlink -f "$0")")/.." || exit 1

PRESET="${PRESET:-debug}"
BIN="build/$PRESET"
GATEWAY="$BIN/l7gate"
UPSTREAM_BIN="$BIN/tests/l7gate_test_upstream"
GATEWAY_PORT="${GATEWAY_PORT:-18080}"
OUT="build/smoke-s1-06"
mkdir -p "$OUT"

FAILED=0
fail() { echo "  ✗ $*"; FAILED=1; }
ok()   { echo "  ✓ $*"; }

[[ -x "$GATEWAY" ]] || { echo "找不到 $GATEWAY，先 cmake --build --preset $PRESET" >&2; exit 1; }
[[ -x "$UPSTREAM_BIN" ]] || { echo "找不到 $UPSTREAM_BIN，先 cmake --build --preset $PRESET" >&2; exit 1; }
command -v curl >/dev/null || { echo "本机没有 curl：这个脚本要的第三方证据不成立" >&2; exit 1; }
curl --version | head -1 | sed 's/^/  curl: /'

# 响应体每轮随机，只在**本次运行内**固定。理由是：直连与经网关两次取值来自同一个
# 上游进程，比的是"网关有没有改动字节"，随机串能顺带排掉"恰好模板内容让它混过去了"
# 这种运气——比如一个全是空格的体，被改写成立。含 NUL/换行的体反而会让这里的
# awk 计数不可读，所以用 base64 字母表：够乱，又是纯文本可核对。
# 注意长度：900 字节随机数经 base64 是 **1200** 个字符，下面所有断言都用 $BODY_LEN，
# 不写死任何数字——第一版我在提示语里写了"900 字节"，那是随机源的长度不是体的长度。
BODY="$(head -c 900 /dev/urandom | base64 | tr -d '\n')"
BODY_LEN=${#BODY}
echo "  响应体 $BODY_LEN 字节（本次运行的固定串，直连与经网关共用同一个上游）"

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

# 每步一对新进程。UP_EXTRA 传给上游（--fixed-body 是这一步的关键：钉死正文之后
# "逐字节一致"才是可比命题，带 seq 的两次取值本来就不同）。
start_pair() { # tag up_arg1 up_arg2 ...
  local tag=$1; shift
  local ulog="$OUT/up-$tag.log" glog="$OUT/gateway-$tag.log"
  "$UPSTREAM_BIN" --fixed-body "$BODY" "$@" > "$ulog" 2>&1 &
  UP_PID=$!
  wait_for_line "$UP_PID" "$ulog" '\[upstream\] 监听' || { fail "上游没能起来（$tag）"; return 1; }
  UP_PORT=$(sed -n 's/.*监听 127\.0\.0\.1:\([0-9]*\).*/\1/p' "$ulog" | head -1)
  [[ -n "$UP_PORT" ]] || { fail "读不到上游实际端口（$tag）"; return 1; }
  "$GATEWAY" --serve --listen "127.0.0.1:$GATEWAY_PORT" --upstream "127.0.0.1:$UP_PORT" \
    > "$glog" 2>&1 &
  G_PID=$!
  if ! wait_for_line "$G_PID" "$glog" '\[serve\] 监听'; then
    kill -TERM "$UP_PID" 2>/dev/null; wait "$UP_PID" 2>/dev/null
    fail "网关没能起来（$tag）"; return 1
  fi
  G_LOG="$glog"
  echo "  $tag: 上游 pid=$UP_PID port=$UP_PORT / 网关 pid=$G_PID port=$GATEWAY_PORT"
}

# 停的顺序就是这一步的结论之一：先收完 curl 再停网关（反过来等于把还在出向缓冲
# 里的响应一起关掉，那正是 S1-04 起 CloseWhy 那一步要防的事）。
stop_pair() { # tag
  local tag=$1
  kill -TERM "$G_PID" 2>/dev/null
  for _ in $(seq 1 200); do kill -0 "$G_PID" 2>/dev/null || break; sleep 0.1; done
  if kill -0 "$G_PID" 2>/dev/null; then
    fail "网关收到 SIGTERM 之后 20s 还没停（$tag）"; kill -KILL "$G_PID" 2>/dev/null
  fi
  wait "$G_PID" 2>/dev/null; G_RC=$?
  kill -TERM "$UP_PID" 2>/dev/null; wait "$UP_PID" 2>/dev/null
  (( G_RC == 0 )) || fail "网关退出码 $G_RC（$tag）：SIGTERM 的优雅停止没生效"
}

get_stat() { sed -n "s/^\[serve\] $1=\([0-9]*\).*/\1/p" "$G_LOG" | head -1; }
first_line() { head -1 "$1" | tr -d '\r'; }
header_of() { sed -n "s/^$2: *//p" "$1" | head -1 | tr -d '\r'; }
# 头部之后的字节数。curl -i 保留了线上的 CRLF，所以按 CRLFCRLF 切第一块。
# 只在**非 chunked** 的步用它：chunked 的体里本来就有 CRLF，切法不成立，
# 那一步的比对口径是整份文件 cmp。
bytes_after_head() {
  awk 'BEGIN{RS="\r\n\r\n"; v=0} NR==2 && !done {gsub(/\r/, ""); v=length($0); done=1} END{print v}' "$1"
}

# 每步收尾都要过的三道生命周期检查。响应语义再对，漏一条 fd 也算这一步没完成。
check_lifecycle() { # tag  期望的"存活连接应为 0"
  local tag=$1
  local live constructed destructed closed fd_from fd_to
  live=$(get_stat live_connections)
  constructed=$(get_stat connections_constructed)
  destructed=$(get_stat connections_destructed)
  fd_from=$(get_stat fd_start); fd_to=$(get_stat fd_end)
  if [[ "${live:-?}" == "0" ]]; then ok "$tag：live_connections=0，循环退出时注册表是空的"
  else fail "$tag：live_connections=${live:-?}，期望 0（有连接没走关闭路径）"; fi
  if [[ -n "$constructed" && "$constructed" == "$destructed" ]]; then
    ok "$tag：constructed=destructed=$constructed，引用计数确实归零"
  else
    fail "$tag：构造 $constructed / 析构 ${destructed:-?} 不等"
  fi
  if [[ -n "$fd_from" && "$fd_from" == "$fd_to" ]]; then ok "$tag：fd $fd_from → $fd_to 回到基线"
  else fail "$tag：fd ${fd_from:-?} → ${fd_to:-?} 不等，有 fd 没关"; fi
}

# ------------------------------------------------------------------ 一
# 「状态码、Content-Length、body 与直连上游逐字节一致」——这一句是验收原文。
# 口径：同一份 curl -i --raw 的输出（头 + 体，原样）两份文件 cmp，一个字节都不能差。
echo "== 阶段一：200 正常响应，直连 vs 经网关逐字节比对"
start_pair ok --body-bytes "$BODY_LEN" || exit 1
curl -sS -i --raw --max-time 10 "http://127.0.0.1:$UP_PORT/plain"   > "$OUT/direct-plain.txt" 2> "$OUT/direct-plain.err"; D_RC=$?
curl -sS -i --raw --max-time 10 "http://127.0.0.1:$GATEWAY_PORT/plain" > "$OUT/proxy-plain.txt" 2> "$OUT/proxy-plain.err"; P_RC=$?
if cmp -s "$OUT/direct-plain.txt" "$OUT/proxy-plain.txt"; then
  ok "两份 $(wc -c < "$OUT/proxy-plain.txt") 字节的原始响应逐字节相同（curl 退出码 直连=$D_RC 经网关=$P_RC）"
else
  fail "直连与经网关的响应不同：$(cmp "$OUT/direct-plain.txt" "$OUT/proxy-plain.txt" | head -1)"
fi
if [[ "$(first_line "$OUT/proxy-plain.txt")" == "HTTP/1.1 200 OK" ]]; then
  ok "状态行：$(first_line "$OUT/proxy-plain.txt")"
else
  fail "状态行不是 200：$(first_line "$OUT/proxy-plain.txt")"
fi
cl=$(header_of "$OUT/proxy-plain.txt" 'Content-Length')
after=$(bytes_after_head "$OUT/proxy-plain.txt")
if [[ "${cl:-}" == "$BODY_LEN" && "${after:-}" == "$BODY_LEN" ]]; then
  ok "Content-Length=$cl 与实际送达的 body 字节数 $after 相等"
else
  fail "Content-Length=${cl:-?} 而送达 body=${after:-?} 字节，期望都是 $BODY_LEN"
fi
if [[ "$(tail -c "$BODY_LEN" "$OUT/proxy-plain.txt")" == "$BODY" ]]; then
  ok "body 内容就是上游那 $BODY_LEN 字节，网关没改写一个字符"
else
  fail "body 内容与钉死的串不同"
fi
stop_pair ok
[[ "$(get_stat responses_completed)" == "1" ]] && ok "responses_completed=1：完结点由框架判定给出，一条" \
  || fail "responses_completed=$(get_stat responses_completed)，期望 1"
[[ "$(get_stat gateway_replies_sent)" == "0" ]] && ok "gateway_replies_sent=0：网关一条自产响应都没发（不该发）" \
  || fail "gateway_replies_sent=$(get_stat gateway_replies_sent)，期望 0"
check_lifecycle ok

# ------------------------------------------------------------------ 二
# 「上游返回 chunked 时转发结果与直连一致」。--raw 让 curl 别替我解码：
# 我要比的是**线上的字节**，解码之后的比对证明不了框架字节有没有被改写。
# 附加一条"网关没有替上游重编框架"的证据：响应里仍然没有 Content-Length。
echo
echo "== 阶段二：chunked 响应，直连 vs 经网关逐字节比对（--raw，不解码）"
start_pair chunked --body-bytes "$BODY_LEN" || exit 1
curl -sS -i --raw --max-time 10 "http://127.0.0.1:$UP_PORT/chunked"    > "$OUT/direct-chunked.txt" 2> "$OUT/direct-chunked.err"; D_RC=$?
curl -sS -i --raw --max-time 10 "http://127.0.0.1:$GATEWAY_PORT/chunked" > "$OUT/proxy-chunked.txt" 2> "$OUT/proxy-chunked.err"; P_RC=$?
if cmp -s "$OUT/direct-chunked.txt" "$OUT/proxy-chunked.txt"; then
  ok "chunked 的原始字节两份完全相同（$(wc -c < "$OUT/proxy-chunked.txt") 字节；直连=$D_RC 经网关=$P_RC）"
else
  fail "chunked 转发与直连不同：$(cmp "$OUT/direct-chunked.txt" "$OUT/proxy-chunked.txt" | head -1)"
fi
te=$(header_of "$OUT/proxy-chunked.txt" 'Transfer-Encoding')
[[ "${te:-}" == "chunked" ]] && ok "响应头里的 Transfer-Encoding: chunked 原样送达" \
  || fail "Transfer-Encoding=${te:-缺失}：框架声明被改动或吞掉了"
if [[ -z "$(header_of "$OUT/proxy-chunked.txt" 'Content-Length')" ]]; then
  ok "没有 Content-Length：网关没把 chunked 拆成 CL 重发（那是改写，不是转发）"
else
  fail "经网关的响应里多出了 Content-Length：网关动了框架"
fi
# last-chunk + 空行必须在，否则 curl 之所以"看起来成功"是因为它自己容错了。
# 比的是十六进制而不是 od -c 的可读形式：后者用空格对齐，grep 的模式一旦依赖
# 对齐宽度就是一条迟早自己骗自己的断言。"350d0a0d0a" = "0" CRLF CRLF。
if tail -c 5 "$OUT/proxy-chunked.txt" | od -An -tx1 | tr -d ' \n' | grep -q '300d0a0d0a'; then
  ok "结尾是 0 长块 + CRLFCRLF：chunked 流自己收了口"
else
  fail "结尾不是 last-chunk 终止串：$(tail -c 5 "$OUT/proxy-chunked.txt" | od -An -tx1 | head -2)"
fi
# 解码之后应当等于原体：这一条是"第三方客户端也认这套框架"的证据。
curl -sS --max-time 10 "http://127.0.0.1:$GATEWAY_PORT/chunked" -o "$OUT/proxy-decoded.txt" 2> "$OUT/proxy-decoded.err"
if [[ "$(cat "$OUT/proxy-decoded.txt")" == "$BODY" ]]; then
  ok "curl 默认解码后拿到的 body 与钉死的串相同：两段 chunk 之和正好是体长"
else
  fail "解码后的 body 不同（长度 $(wc -c < "$OUT/proxy-decoded.txt")，期望 $BODY_LEN）"
fi
stop_pair chunked
# 这一步经网关的请求是**两条**（--raw 一次、解码一次），所以期望值写 2 而不是 1。
# 期望值得从"我发了几次"推出来，不能从"我希望网关怎么表现"推出来——后者是
# 把断言写成愿望，我第一次跑就是挂在这里（2 vs 1 看着像网关多算了一次完结）。
[[ "$(get_stat responses_completed)" == "2" ]] && ok "responses_completed=2：两个 chunked 响应都识别到了 last-chunk 完结点" \
  || fail "responses_completed=$(get_stat responses_completed)，期望 2"
[[ "$(get_stat requests_parsed)" == "2" ]] && ok "requests_parsed=2（本步发了两条经网关的请求）" \
  || fail "requests_parsed=$(get_stat requests_parsed)，期望 2"
[[ "$(get_stat upstream_truncated_in_body)" == "0" ]] && ok "upstream_truncated_in_body=0：合法 chunked 不算截断" \
  || fail "upstream_truncated_in_body=$(get_stat upstream_truncated_in_body)，期望 0"
check_lifecycle chunked

# ------------------------------------------------------------------ 三
# 多条响应挤在同一条 keep-alive 上。这里混了 200 与 chunked 两种框架：
# chunked 的终止空行之后还跟着下一条请求的响应，**边界算错一个字节**就会被
# 下一条响应吃掉或吞掉上一条的尾巴——curl 会把解出来的东西拼起来，所以断言
# 用"总字节 == 3 倍体长"，不需要我相信网关自己的计数。
echo
echo "== 阶段三：一条 curl 连接上混发 200 / chunked / 200（框架边界不吃掉下一条）"
start_pair mixed --body-bytes "$BODY_LEN" || exit 1
# 三个 URL 作为**三个参数**交给同一个 curl 进程：它内部是一条连接顺序发三次，
# 这正是"混着框架的同一条 keep-alive"。不用 URL glob：`[plain,chunked,plain]`
# 被 curl 判成非法 URL（退出码 3），而退出码非 0 时文件是空的——如果我只断言
# "字节数不对就失败"，这条会失败；但如果我断言的是"curl 退出码 0"之外的东西，
# 它就可能悄悄什么都不测。所以现在连退出码一起断言，并且把 curl 的 stderr 落盘。
# 输出不带 -o，让 shell 一次重定向、curl 顺序写——用 -o 时 curl 会在每次传输
# 开头重开文件，三条就只剩最后一条，那是一个"看起来什么都测了"的空壳。
curl -sS --max-time 20 "http://127.0.0.1:$GATEWAY_PORT/plain" \
  "http://127.0.0.1:$GATEWAY_PORT/chunked" "http://127.0.0.1:$GATEWAY_PORT/plain" \
  > "$OUT/mixed-bodies.bin" 2> "$OUT/mixed-bodies.err"
M_RC=$?
echo "  curl 退出码=$M_RC 解码后总字节=$(wc -c < "$OUT/mixed-bodies.bin")（期望 $((3 * BODY_LEN))）"
[[ -s "$OUT/mixed-bodies.err" ]] && sed 's/^/    curl stderr: /' "$OUT/mixed-bodies.err"
if [[ "$M_RC" == "0" ]] && [[ "$(wc -c < "$OUT/mixed-bodies.bin")" == "$((3 * BODY_LEN))" ]]; then
  ok "三条响应各 $BODY_LEN 字节，一条都没少、一条都没串到下一条上"
else
  fail "混合序列的字节数或退出码不对：见 $OUT/mixed-bodies.bin"
fi
# 每段各自可核：把解码流按体长切三刀，三刀都应当等于钉死的串。
if [[ "$(cut -b 1-$BODY_LEN "$OUT/mixed-bodies.bin")" == "$BODY" ]] &&
   [[ "$(cut -b $((BODY_LEN + 1))-$((2 * BODY_LEN)) "$OUT/mixed-bodies.bin")" == "$BODY" ]] &&
   [[ "$(cut -b $((2 * BODY_LEN + 1))-$((3 * BODY_LEN)) "$OUT/mixed-bodies.bin")" == "$BODY" ]]; then
  ok "三段内容逐段相等（顺序没错、没有互相覆盖）"
else
  fail "三段里至少一段不是钉死的那个串：响应串号了"
fi
stop_pair mixed
[[ "$(get_stat requests_parsed)" == "3" ]] && ok "requests_parsed=3" || fail "requests_parsed=$(get_stat requests_parsed)，期望 3"
[[ "$(get_stat downstream_accepted)" == "1" ]] && ok "downstream_accepted=1：三种形态共用一条下游连接" \
  || fail "downstream_accepted=$(get_stat downstream_accepted)，期望 1"
[[ "$(get_stat upstream_opened)" == "1" ]] && ok "upstream_opened=1：没有为 chunked 另开一条回源连接" \
  || fail "upstream_opened=$(get_stat upstream_opened)，期望 1"
[[ "$(get_stat responses_completed)" == "3" ]] && ok "responses_completed=3：三条各自走到完结点" \
  || fail "responses_completed=$(get_stat responses_completed)，期望 3"
check_lifecycle mixed

# ------------------------------------------------------------------ 四
# 「上游中途断开时网关返回可定义的 5xx」——形态一：一个字节都没回就关。
# 最终头还没出现，网关仍然有自产表态的资格 → 502，且必须是**网关自己的那一条**：
# 所以这里同时打一次直连，让第三方客户端证明上游确实什么都没说。
echo
# curl 的 stderr **必须**分到另一个文件。第一版我用 2>&1 把它并进了响应文件，
# 于是"直连一个字节都没拿到"这条断言拿到的是 `curl: (52) Empty reply from server`
# 这 35 个字节——断言失败，而失败的原因是我的取数方式，不是网关。
echo "== 阶段四：上游不回话就关（/dies）→ 网关自产 502"
start_pair dies-before-head --body-bytes "$BODY_LEN" || exit 1
curl -sS -i --max-time 10 "http://127.0.0.1:$UP_PORT/dies" > "$OUT/direct-dies.txt" 2> "$OUT/direct-dies.err"; D_RC=$?
curl -sS -i --max-time 10 "http://127.0.0.1:$GATEWAY_PORT/dies" > "$OUT/proxy-dies.txt" 2> "$OUT/proxy-dies.err"; P_RC=$?
echo "  直连 curl 退出码=$D_RC（拿到 $(wc -c < "$OUT/direct-dies.txt") 字节，stderr：$(tr -d '\n' < "$OUT/direct-dies.err")）；经网关=$P_RC"
if [[ ! -s "$OUT/direct-dies.txt" ]] && (( D_RC == 52 )); then
  ok "直连确实一个字节都没拿到（curl 52 = Empty reply from server）：上游此刻无话可说，502 不可能是转发的产物"
else
  fail "直连拿到 $(wc -c < "$OUT/direct-dies.txt") 字节或退出码不是 52（实测 $D_RC）：上游并没有真的不回话"
fi
if [[ "$(first_line "$OUT/proxy-dies.txt")" == "HTTP/1.1 502 Bad Gateway" ]]; then
  ok "经网关拿到的是可定义的 5xx：$(first_line "$OUT/proxy-dies.txt")（curl 退出码 $P_RC）"
else
  fail "经网关的状态行不是 502：$(first_line "$OUT/proxy-dies.txt")"
fi
[[ "$(header_of "$OUT/proxy-dies.txt" 'Content-Length')" == "12" ]] \
  && ok "502 自带 Content-Length: 12，与 body 的实际字节数一致" \
  || fail "502 的 Content-Length=$(header_of "$OUT/proxy-dies.txt" 'Content-Length')，期望 12"
[[ "$(bytes_after_head "$OUT/proxy-dies.txt")" == "12" ]] \
  && ok "502 的 body 送达 12 字节，不是半包" || fail "502 的 body 字节数=$(bytes_after_head "$OUT/proxy-dies.txt")，期望 12"
[[ "$(header_of "$OUT/proxy-dies.txt" 'Connection')" == "close" ]] \
  && ok "502 带 Connection: close：表态之后这条连接就收尾，不留悬着的 keep-alive" \
  || fail "502 的 Connection=$(header_of "$OUT/proxy-dies.txt" 'Connection')，期望 close"
(( P_RC == 0 )) || fail "客户端没有完整收到这条 502（curl 退出码 $P_RC）"
stop_pair dies-before-head
[[ "$(get_stat upstream_truncated_before_head)" == "1" ]] && ok "upstream_truncated_before_head=1（口径：最终头之前断）" \
  || fail "upstream_truncated_before_head=$(get_stat upstream_truncated_before_head)，期望 1"
[[ "$(get_stat gateway_replies_sent)" == "1" ]] && ok "gateway_replies_sent=1：这条 5xx 确实是网关自产的" \
  || fail "gateway_replies_sent=$(get_stat gateway_replies_sent)，期望 1"
[[ "$(get_stat responses_completed)" == "0" ]] && ok "responses_completed=0：没有任何响应被当成完结" \
  || fail "responses_completed=$(get_stat responses_completed)，期望 0"
check_lifecycle dies-before-head

# ------------------------------------------------------------------ 五
# 形态二：头回全了、body 只回一半就关。此刻最终头**已经交给客户端**，再补一条
# 5xx 就是给同一条请求发两条响应。可定义的表态只剩下连接终止本身，
# 而"不残留半包"要落在这两点上：客户端拿到的是恰好一半（不多不少）、
# 网关没有把剩下的半包留在任何缓冲里（live=0、constructed==destructed）。
echo
echo "== 阶段五：上游回半条 body 就关（/dies-mid-body）→ 以 EOF 表态，不追加 5xx"
start_pair dies-in-body --body-bytes "$BODY_LEN" || exit 1
HALF=$((BODY_LEN / 2))
curl -sS -i --max-time 10 "http://127.0.0.1:$GATEWAY_PORT/dies-mid-body" > "$OUT/proxy-midbody.txt" 2> "$OUT/proxy-midbody.err"; P_RC=$?
curl -sS -i --max-time 10 "http://127.0.0.1:$UP_PORT/dies-mid-body" > "$OUT/direct-midbody.txt" 2> "$OUT/direct-midbody.err"; D_RC=$?
echo "  经网关 curl 退出码=$P_RC 拿到 $(wc -c < "$OUT/proxy-midbody.txt") 字节；直连退出码=$D_RC"
if [[ "$(first_line "$OUT/proxy-midbody.txt")" == "HTTP/1.1 200 OK" ]]; then
  ok "200 的头原样送达（网关没在头已发出之后再改口）"
else
  fail "头不是 200：$(first_line "$OUT/proxy-midbody.txt")"
fi
if [[ "$(bytes_after_head "$OUT/proxy-midbody.txt")" == "$HALF" ]]; then
  ok "body 恰好送达 $HALF 字节：既没多（伪造）也没少（丢包）"
else
  fail "body 送达 $(bytes_after_head "$OUT/proxy-midbody.txt") 字节，期望 $HALF"
fi
if grep -q '50[0-9] ' "$OUT/proxy-midbody.txt"; then
  fail "截断之后又追加了 5xx：同一条请求出现两条响应"
else
  ok "文件里没有第二条响应：截断的表态就是连接终止"
fi
if cmp -s "$OUT/direct-midbody.txt" "$OUT/proxy-midbody.txt"; then
  ok "这一种截断，直连与经网关的**可见部分**逐字节一致"
else
  fail "截断场景下两条路的字节不同：$(cmp "$OUT/direct-midbody.txt" "$OUT/proxy-midbody.txt" | head -1)"
fi
(( P_RC != 0 )) && ok "curl 自己就报了错（退出码 $P_RC）：第三方客户端认这是截断，不是我单方面的判定" \
  || fail "curl 退出码 0：Content-Length 声明的字节没到齐它却认为成功，说明网关补了东西"
stop_pair dies-in-body
[[ "$(get_stat upstream_truncated_in_body)" == "1" ]] && ok "upstream_truncated_in_body=1（口径：最终头之后断）" \
  || fail "upstream_truncated_in_body=$(get_stat upstream_truncated_in_body)，期望 1"
[[ "$(get_stat gateway_replies_sent)" == "0" ]] && ok "gateway_replies_sent=0：网关没有越权追加表态" \
  || fail "gateway_replies_sent=$(get_stat gateway_replies_sent)，期望 0"
check_lifecycle dies-in-body

# ------------------------------------------------------------------ 六
# 框架声明合法但框架字节是坏的：curl 自己会拒绝这套 chunked，所以这里不比直连，
# 比的是"网关在坏框架面前做了什么"——透传了已送达的字节、没有替上游圆场、
# 把这条连接按截断收掉（计数落在 in_body，因为最终头已经发出去了）。
echo
echo "== 阶段六：坏 chunk 框架（/bad-chunk）→ 断流，不伪造内容"
start_pair bad-chunk --body-bytes "$BODY_LEN" || exit 1
curl -sS -i --raw --max-time 10 "http://127.0.0.1:$GATEWAY_PORT/bad-chunk" > "$OUT/proxy-badchunk.txt" 2> "$OUT/proxy-badchunk.err"; P_RC=$?
echo "  经网关 curl 退出码=$P_RC 拿到 $(wc -c < "$OUT/proxy-badchunk.txt") 字节"
if [[ "$(bytes_after_head "$OUT/proxy-badchunk.txt")" == "0" ]]; then
  ok "头之后没有任何字节漏给客户端：非 hex 的 size 行没被当正文转发"
else
  fail "坏框架的字节出现在客户端面前（$(bytes_after_head "$OUT/proxy-badchunk.txt") 字节）"
fi
(( P_RC != 0 )) && ok "curl 判定这条响应没成功（退出码 $P_RC）：截断由客户端也看到了" \
  || fail "curl 退出码 0：它对坏框架无感，这条证据就不成立了"
stop_pair bad-chunk
[[ "$(get_stat upstream_truncated_in_body)" == "1" ]] && ok "upstream_truncated_in_body=1" \
  || fail "upstream_truncated_in_body=$(get_stat upstream_truncated_in_body)，期望 1"
[[ "$(get_stat gateway_replies_sent)" == "0" ]] && ok "gateway_replies_sent=0：头已发出，网关没有改口" \
  || fail "gateway_replies_sent=$(get_stat gateway_replies_sent)，期望 0"
check_lifecycle bad-chunk

# ------------------------------------------------------------------ 七
# 一条**永远不齐**的响应头（70 KiB，超过 64 KiB 的响应头预算）。
# 这一条防的是反方向的内存放大：半包本身不是错误，只有"大到不可能是合法头"
# 才允许判死。所以断言的是两件事：判死了（502 + 计数）、而且**没有把 70 KiB
# 一直攥在手里**（fd 回基线、连接全析构）。
echo
echo "== 阶段七：永不完结的响应头（/endless-head）→ 超预算判死"
start_pair endless-head --body-bytes "$BODY_LEN" || exit 1
curl -sS -i --max-time 20 "http://127.0.0.1:$GATEWAY_PORT/endless-head" > "$OUT/proxy-endless.txt" 2> "$OUT/proxy-endless.err"; P_RC=$?
if [[ "$(first_line "$OUT/proxy-endless.txt")" == "HTTP/1.1 502 Bad Gateway" ]]; then
  ok "拿到 502：坏上游在头预算之外没有换来无限缓冲"
else
  fail "状态行不是 502：$(first_line "$OUT/proxy-endless.txt")（curl 退出码 $P_RC）"
fi
stop_pair endless-head
[[ "$(get_stat upstream_truncated_before_head)" == "1" ]] && ok "upstream_truncated_before_head=1（超长未齐与 EOF 同一口径）" \
  || fail "upstream_truncated_before_head=$(get_stat upstream_truncated_before_head)，期望 1"
[[ "$(get_stat gateway_replies_sent)" == "1" ]] && ok "gateway_replies_sent=1" \
  || fail "gateway_replies_sent=$(get_stat gateway_replies_sent)，期望 1"
check_lifecycle endless-head

# ------------------------------------------------------------------ 八
# 请求侧的映射表也要外部证据。curl 发不出畸形请求（它会拒绝），所以这里用
# bash 的 /dev/tcp 手写三条：缺 Host → 400，CONNECT → 501，TRACE → 照常转发。
# 第三条是这个阶段真正想要的对照：**语法合法的方法不是畸形**。解析器不给 TRACE
# 判错误码，网关就把它当普通请求回源——"不认识的方法一律 501"那种写法会让上游
# 明明能服务的东西被网关自己拒掉，而那看起来像是安全默认值。
# 单独一步的原因：前两条走的是 forward_pending_requests 之外的 reject 分支，
# 与阶段四的 502 不是同一条代码路径，不能用同一个计数概括。
echo
echo "== 阶段八：畸形请求的 4xx 与 501 映射（/dev/tcp 手写，curl 不会替我发这些）"
start_pair reject --body-bytes "$BODY_LEN" || exit 1
send_raw() { # 请求文本 输出文件
  exec 3<>/dev/tcp/127.0.0.1/$GATEWAY_PORT || { fail "/dev/tcp 连不上网关"; return 1; }
  printf '%s' "$1" >&3
  timeout 5 cat <&3 > "$2" 2>/dev/null
  exec 3<&- 3>&-
}
NO_HOST=$'GET / HTTP/1.1\r\nAccept: */*\r\n\r\n'
CONNECT=$'CONNECT 10.0.0.1:80 HTTP/1.1\r\nHost: smoke.test\r\n\r\n'
TRACE=$'TRACE / HTTP/1.1\r\nHost: smoke.test\r\n\r\n'
echo "  三条手写请求的字节数：no-host=${#NO_HOST} CONNECT=${#CONNECT} TRACE=${#TRACE}"
send_raw "$NO_HOST" "$OUT/reject-no-host.txt"
if [[ "$(first_line "$OUT/reject-no-host.txt")" == "HTTP/1.1 400 Bad Request" ]]; then
  ok "缺 Host → $(first_line "$OUT/reject-no-host.txt")"
else
  fail "缺 Host 的状态行是：$(first_line "$OUT/reject-no-host.txt")"
fi
[[ "$(bytes_after_head "$OUT/reject-no-host.txt")" == "12" ]] \
  && ok "400 的 body 12 字节送达，不是半包" || fail "400 的 body=$(bytes_after_head "$OUT/reject-no-host.txt") 字节，期望 12"
send_raw "$CONNECT" "$OUT/reject-connect.txt"
if [[ "$(first_line "$OUT/reject-connect.txt")" == "HTTP/1.1 501 Not Implemented" ]]; then
  ok "CONNECT → $(first_line "$OUT/reject-connect.txt")（authority-form 要隧道语义，S1 不做，但它是 501 不是 400）"
else
  fail "CONNECT 的状态行是：$(first_line "$OUT/reject-connect.txt")"
fi
[[ "$(header_of "$OUT/reject-connect.txt" 'Content-Length')" == "16" ]] \
  && ok "501 的 Content-Length=16 与 body 字节数一致" \
  || fail "501 的 Content-Length=$(header_of "$OUT/reject-connect.txt" 'Content-Length')，期望 16"
send_raw "$TRACE" "$OUT/proxy-trace.txt"
if [[ "$(first_line "$OUT/proxy-trace.txt")" == "HTTP/1.1 200 OK" ]]; then
  ok "TRACE 换到的是上游的 200：合法方法照常回源，网关没有替上游拒绝自己能服务的东西"
else
  fail "TRACE 的状态行是：$(first_line "$OUT/proxy-trace.txt")，期望上游的 200"
fi
stop_pair reject
[[ "$(get_stat requests_rejected)" == "2" ]] && ok "requests_rejected=2：400 与 501 都是解析层判的，不是上游回的" \
  || fail "requests_rejected=$(get_stat requests_rejected)，期望 2"
[[ "$(get_stat gateway_replies_sent)" == "2" ]] && ok "gateway_replies_sent=2：两条自产表态都进了下游出向缓冲" \
  || fail "gateway_replies_sent=$(get_stat gateway_replies_sent)，期望 2"
# 这条是这个阶段的口径核心：**被拒的请求一条都不该回源**。用 opened==parsed
# 而不是 opened==0 来断言，是因为同一步里我还发了 TRACE——它是该回源的那条。
[[ "$(get_stat upstream_opened)" == "$(get_stat requests_parsed)" ]] \
  && ok "upstream_opened=$(get_stat upstream_opened) == requests_parsed=$(get_stat requests_parsed)：只有解析成功的请求才换来回源连接" \
  || fail "upstream_opened=$(get_stat upstream_opened) 与 requests_parsed=$(get_stat requests_parsed) 不等：为没解析成功的请求开了上游"
[[ "$(get_stat requests_parsed)" == "1" ]] && ok "requests_parsed=1（只有 TRACE 那一条）" \
  || fail "requests_parsed=$(get_stat requests_parsed)，期望 1"
[[ "$(get_stat responses_completed)" == "1" ]] && ok "responses_completed=1：回源那条走完了完结点" \
  || fail "responses_completed=$(get_stat responses_completed)，期望 1"
check_lifecycle reject

echo
echo "== 各步网关自报计数（全量留档）："
for f in "$OUT"/gateway-*.log; do
  echo "  --- $(basename "$f")"
  grep '^\[serve\]' "$f" | sed -e 's/^/      /' -e 's/\[serve\] //'
done

echo
if (( FAILED == 0 )); then
  echo "== S1-06 冒烟通过：preset=$PRESET body=${BODY_LEN}B 八个阶段全绿，证据在 $OUT/"
else
  echo "== S1-06 冒烟未通过：上面带 ✗ 的行每一项都是没达标的项目，不是警告"
fi
exit "$FAILED"
