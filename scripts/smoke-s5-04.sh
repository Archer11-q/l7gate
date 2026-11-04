#!/usr/bin/env bash
# S5-04 冒烟：WebSocket 调试页 `demo/ws-echo.html` 在真进程 + 真浏览器里成立到什么程度。
#
# 存在理由（ADR-26）：验收句⑤ 要「取数证据用 DOM 断言，不用人眼」，而 ctest 没有浏览器、
# curl 也不是浏览器——浏览器侧那三条事实（握手要按 key 现算的 accept、回帧要未掩码、
# 状态码只能从**同源**请求里读）只有真浏览器能给答案。
#
# 仪器形态三件，都是实测逼出来的：
#   一、无头 Chrome `--headless=new --dump-dom --virtual-time-budget=30000`（单轮实测 1.1 s）。
#       单次快照**不可靠**：同参数连跑三轮，第三轮只抓到 `connect` 那一行。
#       所以判定是「有限重试（≤8）+ 完成哨兵」——页面只在全串步骤跑完才写
#       `<output id="s504-verdict">`，每一轮的 DOM 都落盘（try-N.html），不删。
#   二、判定只读**哨兵元素与消息流元素**，不读整篇文档：`--dump-dom` 里含页面自己的
#       `<script>` 源码，源码里就写着 `429 rate limited`。阶段 7 把这件事做成对照判定，
#       不然"限定元素"只是我的习惯，不是被钉住的规矩。
#   三、退出码不可信（§22.2 第 5 条那一族）：判定写进 `build/smoke-s5-04/verdict.txt`。
#       **不许把 rc=0 当成通过。**
#
# 阶段：
#   0 前置（二进制 / demo 页 / chrome 可执行 / 端口空闲）——缺依赖就**拒跑**，不换浏览器顶替
#   1 起上游（--ws-echo --html）+ 网关（rate_limit key=path，--metrics-port）
#   2 页面经网关同源取回：盘上字节数 == 上游读到的 == 网关透出的 Content-Length
#   3 自动序列 → 只读哨兵必须 DONE
#   4 句① 逐条回显与发送值逐字相等 + Δupgrades_101 == 1
#   5 句② `429 rate limited` 条数 == 哨兵 got429 == Δrate_limit_rejected；且消息流无非可打印字节
#   6 句③ closed code=1005 + live 回 0 + constructed==destructed
#   7 仪器对照：整篇命中 > 限定命中
#   8 句④ same-connection 行（两条消息共用一条长连接）
#   9 三方同轴：**换新上游只跑两轮**，让上游自报数正好等于这两轮（重试次数不污染等式）
#  10 生命周期：fd_start==fd_end、live=0、constructed==destructed、upgrades_101 有下界
#
# 用法：PRESET=debug scripts/smoke-s5-04.sh
# 输出：build/smoke-s5-04/（每轮 DOM 快照 + up-N.log + attempts.log + verdict.txt）
set -uo pipefail
cd "$(dirname "$(readlink -f "$0")")/.." || exit 1
trap '' PIPE

PRESET="${PRESET:-debug}"
BIN="build/$PRESET"
GATEWAY="$BIN/l7gate"
UPSTREAM_BIN="$BIN/tests/l7gate_test_upstream"
PAGE_SRC="demo/ws-echo.html"
OUT="build/smoke-s5-04"
mkdir -p "$OUT"

G_PORT=18940          # 业务端口：页面、WS、状态码探针都走它——同源是句② 的前提
U_PORT=18941
M_PORT=18524          # 网关 --metrics-port

BUDGET=30000
MAX_TRIES=8
PROBE_TOTAL=12        # 页面自动序列里的探针条数（与 URL 的 burst 一致）

FAILED=0
FAILS=0
CHECKS=0
ROUNDS=0              # 无头浏览器实际跑了多少轮（每轮一次握手 → upgrades_101 的上界）
REACHED_END=0
PIDS=()
ok()   { echo "  ✓ $*"; CHECKS=$((CHECKS + 1)); }
fail() { echo "  ✗ $*"; FAILED=1; FAILS=$((FAILS + 1)); CHECKS=$((CHECKS + 1)); }
die()  { echo "  fatal: $*" >&2; echo "FAIL checks=$CHECKS fails=$((FAILS + 1)) refused=0" > "$OUT/verdict.txt"; exit 1; }

cleanup() {
  for pid in "${PIDS[@]:-}"; do kill -TERM "$pid" 2>/dev/null; done
  wait 2>/dev/null
}
trap cleanup EXIT

# ---------------- 阶段 0：前置 ----------------
echo "== S5-04 冒烟：WebSocket 调试页（PRESET=$PRESET）=="
echo "-- 阶段 0：前置"
for b in "$GATEWAY" "$UPSTREAM_BIN"; do
  [[ -x "$b" ]] || die "找不到 $b —— 先 cmake --build --preset $PRESET（全目标，口径⑪）"
done
[[ -f "$PAGE_SRC" ]] || die "找不到 $PAGE_SRC"
command -v curl >/dev/null || die "本机没有 curl"
port_free() { ! (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; }
for p in "$G_PORT" "$U_PORT" "$M_PORT"; do
  port_free "$p" || die "端口 $p 已被占用：先清场"
done

CHROME='/mnt/c/Program Files/Google/Chrome/Application/chrome.exe'
if [[ ! -x "$CHROME" ]]; then
  echo "  没有可经 WSL interop 执行的 chrome.exe（$CHROME）。"
  echo "  本条依赖真浏览器，**拒跑**——不是通过，也不换别的浏览器顶上去（照 s3-04/s4-04 对 Docker 的先例）。"
  echo "FAIL checks=$CHECKS fails=0 refused=1" > "$OUT/verdict.txt"
  trap - EXIT
  exit 1
fi
WIN_TEMP=$(cmd.exe /c 'echo %TEMP%' 2>/dev/null | tr -d '\r\n')
if [[ -z "$WIN_TEMP" ]]; then
  echo "  拿不到 Windows 侧 TEMP：无头浏览器没有专用 profile 可放，拒跑。"
  echo "FAIL checks=$CHECKS fails=0 refused=1" > "$OUT/verdict.txt"
  trap - EXIT
  exit 1
fi
PROFILE="${WIN_TEMP}\\l7gate-s5-04-profile"   # 专用 profile：绝不挂到用户自己的 Chrome 配置上
ok "依赖在位：chrome.exe + 专用 profile=$PROFILE"
if ! timeout 60 "$CHROME" --headless=new --disable-gpu --no-first-run --user-data-dir="$PROFILE" \
     --virtual-time-budget=1 --dump-dom 'about:blank' > "$OUT/boot.log" 2>&1; then
  echo "  chrome.exe 存在但跑不起来（见 $OUT/boot.log）：拒跑。"
  echo "FAIL checks=$CHECKS fails=0 refused=1" > "$OUT/verdict.txt"
  trap - EXIT
  exit 1
fi
ok "interop 实测能执行（about:blank 一次 dump 成功）"

metric() { curl -s --max-time 3 "http://127.0.0.1:$M_PORT/metrics" 2>/dev/null |
             awk -v k="$1" '$1==k {print $2; exit}'; }
# 取数器必须**行首锚定**：这两个标签的字面串在页面的注释里也出现过（第一版就从注释那一处
# 开始抓，把 `是**完成哨兵**` 当成了哨兵内容，八轮全被判成"没拿到"——红的是我的取数方式，
# 不是被测方；§20.1/§21.2 那一族在这里的第六次应验）。锚定成"行首就是该标签"之后，
# 注释里的中途出现不会再被抓住，而 dump-dom 保持了我入库时的行首缩进（实测）。
# 另一个坑：**Windows 侧的 chrome.exe 写出来的是 CRLF**。快照按原样落盘（那是证据），
# 取数时统一 `tr -d '\r'`——第一版没去这一层，`grep -x 'sent s504-a'` 整行匹配全部落空，
# 于是句①②④ 一起红，而红的原因是我的行尾处理。
sentinel_of() { tr -d '\r' < "$1" | awk 'index($0, "<output id=\"s504-verdict\">")==1 {
                    sub(/^<output id="s504-verdict">/, ""); sub(/<\/output>.*/, ""); print; exit }'; }
log_of() { tr -d '\r' < "$1" | awk '
    index($0, "<pre id=\"log\">")==1 { f=1; sub(/^<pre id="log">/, ""); print; next }
    f && /<\/pre>/ { sub(/<\/pre>.*/, ""); print; exit }
    f { print }'; }
tag_present() { tr -d '\r' < "$2" | grep -c "^$1" | tr -d ' '; }
field_of() { printf '%s' "$1" | grep -o "$2=[0-9]*" | head -1 | cut -d= -f2; }
run_headless() { # url outfile
  timeout 120 "$CHROME" --headless=new --disable-gpu --no-first-run --no-default-browser-check \
    --user-data-dir="$PROFILE" --virtual-time-budget="$BUDGET" --dump-dom "$1" \
    > "$2" 2>"$2.err"
}

# ---------------- 阶段 1：起拓扑 ----------------
echo "-- 阶段 1：上游（--ws-echo --html）+ 网关（rate_limit key=path）"
CFG="$OUT/gw.json"
cat > "$CFG" <<JSON
{
  "upstreams": [
    { "name": "web", "balance": "round_robin", "targets": [
      { "address": "127.0.0.1", "port": $U_PORT } ] }
  ],
  "routes": [ { "match": "prefix", "pattern": "", "upstream": "web" } ],
  "rate_limit": { "rate": 5, "burst": 5, "key": "path" }
}
JSON
"$UPSTREAM_BIN" --listen "127.0.0.1:$U_PORT" --ws-echo --html "$PAGE_SRC" > "$OUT/up-1.log" 2>&1 &
PIDS+=("$!"); U=$!
sleep 0.6
grep -q 'ws_echo=1' "$OUT/up-1.log" && ok "上游自报 ws_echo=1" \
  || fail "上游没开 --ws-echo：$(head -1 "$OUT/up-1.log")"
HTML_BYTES=$(grep -o 'html_bytes=[0-9]*' "$OUT/up-1.log" | head -1 | cut -d= -f2)
"$GATEWAY" --serve --listen "0.0.0.0:$G_PORT" --config "$CFG" --metrics-port "$M_PORT" \
  > "$OUT/gateway.log" 2>&1 &
PIDS+=("$!"); GW=$!
sleep 0.8
wait_gw=0
until curl -s --max-time 2 -o /dev/null "http://127.0.0.1:$G_PORT/keepalive-probe"; do
  (( ++wait_gw > 30 )) && die "网关没起来，看 $OUT/gateway.log"
  sleep 0.3
done
ok "网关在 $G_PORT（业务，页面同源）+ $M_PORT（观测）"

# ---------------- 阶段 2：页面经网关同源取回 ----------------
echo "-- 阶段 2：页面经网关取回"
FILE_BYTES=$(wc -c < "$PAGE_SRC" | tr -d ' ')
CL=$(curl -s --max-time 5 -o "$OUT/page-from-gateway.html" -D - \
      "http://127.0.0.1:$G_PORT/$PAGE_SRC" 2>/dev/null | tr -d '\r' |
      awk 'tolower($1)=="content-length:" {print $2; exit}')
GOT_BYTES=$(wc -c < "$OUT/page-from-gateway.html" | tr -d ' ')
if [[ "$CL" == "$FILE_BYTES" && "$GOT_BYTES" == "$FILE_BYTES" && "$HTML_BYTES" == "$FILE_BYTES" ]]; then
  ok "页面字节数三处逐数相等：盘上 $FILE_BYTES == 上游读到 $HTML_BYTES == 网关透出 $CL（body 实收 $GOT_BYTES）"
else
  fail "页面字节数对不上：盘上 $FILE_BYTES / 上游 $HTML_BYTES / 头 $CL / 实收 $GOT_BYTES"
fi
grep -q 's504-verdict' "$OUT/page-from-gateway.html" \
  && ok "取回的就是带哨兵那一份" || fail "透回的页面里没有哨兵元素"

URL="http://127.0.0.1:$G_PORT/$PAGE_SRC?auto=1&ws=/ws&probe=/limited&burst=$PROBE_TOTAL"

# ---------------- 阶段 3：自动序列 ----------------
echo "-- 阶段 3：无头自动序列（预算 $BUDGET，重试 ≤$MAX_TRIES）"
TRY=0
GOOD=""
while (( TRY < MAX_TRIES )); do
  TRY=$((TRY + 1))
  RB=$(metric rate_limit_rejected); UB=$(metric upgrades_101)
  run_headless "$URL" "$OUT/try-$TRY.html"; ROUNDS=$((ROUNDS + 1))
  RA=$(metric rate_limit_rejected); UA=$(metric upgrades_101)
  SENT=$(sentinel_of "$OUT/try-$TRY.html")
  if [[ -z "$SENT" ]]; then
    # 空哨兵有两种成因，必须当场分开：页面真没跑完 / 我的取数器锚错了。
    N=$(tag_present '<output id="s504-verdict">' "$OUT/try-$TRY.html")
    echo "    （行首就是哨兵标签的行有 $N 条；0 条=取数器问题，1 条空=页面没跑完）" | tee -a "$OUT/attempts.log"
  fi
  echo "  try=$TRY Δrejected=$((RA-RB)) Δupgrades=$((UA-UB)) sentinel=[$SENT]" | tee -a "$OUT/attempts.log"
  # 收货条件是"哨兵那格里有结论"（DONE 或 INCOMPLETE 都算结论），不是"必须 DONE"：
  # 第一版把 DONE 当门槛，结果退化 W4（收到第一条就断）让八轮全判 INCOMPLETE、
  # 后面每一格都被门挡住，红只落在门槛上而不是句④ 那一格——门槛吸收了证据，
  # 这不是好仪器。现在 DONE 自己是一条判定，其余判定直接读消息流那份证据。
  if [[ -n "$SENT" ]]; then
    GOOD="$OUT/try-$TRY.html"; GOOD_RB=$RB; GOOD_RA=$RA; GOOD_UB=$UB; GOOD_UA=$UA
    ok "第 $TRY 轮拿到哨兵结论（前 $((TRY-1)) 轮的快照留在 $OUT/try-*.html，没删）"
    break
  fi
done
if [[ -z "$GOOD" ]]; then
  fail "$MAX_TRIES 轮都没拿到完成哨兵——句①②③④ 无从判定"
  echo "-- 后续阶段依赖哨兵，跳过"
else
  SENT=$(sentinel_of "$GOOD")
  LOG=$(log_of "$GOOD")
  GOT429=$(field_of "$SENT" got429)
  PTOTAL=$(field_of "$SENT" probe_total)
  RRECV=$(field_of "$SENT" recv)

  # ---------------- 阶段 4：句① ----------------
  echo "-- 阶段 4：句① 逐条回显与发送值逐字相等"
  [[ "$SENT" == DONE* ]] && ok "哨兵自报 DONE（页面承认整串自动步骤跑完）" \
    || fail "哨兵不是 DONE 而是 [$SENT]——页面自己说这串没跑完"
  [[ "$RRECV" == "2" ]] && ok "哨兵 recv=2（两条消息都回到页面）" || fail "哨兵 recv=$RRECV，期望 2"
  [[ "$PTOTAL" == "$PROBE_TOTAL" ]] && ok "哨兵 probe_total=$PROBE_TOTAL" \
    || fail "probe_total=$PTOTAL，期望 $PROBE_TOTAL"
  pair_ok=1
  for pair in "1 s504-a" "2 s504-b"; do
    set -- $pair
    idx=$1; text=$2
    if printf '%s\n' "$LOG" | grep -qx "sent $text" &&
       printf '%s\n' "$LOG" | grep -qx "recv $idx len=${#text} val=$text"; then
      ok "第 $idx 条 $text：发出去的那一串与回来的那一串逐字相等（len=${#text}）"
    else
      fail "第 $idx 条 $text：消息流里没有那两行逐字相等的记录"
      pair_ok=0
    fi
  done
  (( pair_ok )) || echo "    （串号/丢帧在这一格会直接露出来：回来的 val 必须是发出去的那一串）"
  D_UPGRADES=$((GOOD_UA - GOOD_UB))
  [[ "$D_UPGRADES" == "1" ]] \
    && ok "Δupgrades_101 == 1：两条消息共用**一次**升级" \
    || fail "Δupgrades_101=$D_UPGRADES，期望 1"

  # ---------------- 阶段 5：句② ----------------
  echo "-- 阶段 5：句② 页面出现 429 rate limited 且与 rate_limit_rejected 逐数相等"
  N_TEXT=$(printf '%s\n' "$LOG" | grep -cx '429 rate limited')
  D_REJECTED=$((GOOD_RA - GOOD_RB))
  # 互背书的**形状**先钉住（照 ADR-25 第八节 E3 那条指纹做法）：等式两边必须一条来自 DOM 快照、
  # 一条来自网关观测面的两次读数，而且是"读来的"不是"我推算的"。
  # 没有这一格，把网关那一半换成页面自己的数会让等式变成**空洞的绿**（页面 == 页面恒真）。
  {
    echo "dom=$GOOD"
    echo "gateway=http://127.0.0.1:$M_PORT/metrics rate_limit_rejected $GOOD_RB->$GOOD_RA"
  } > "$OUT/prov-429.txt"
  GW_SRC=$(awk -F= '/^gateway=/{sub(/^gateway=/,""); sub(/ rate_limit_rejected.*/,""); print; exit}' "$OUT/prov-429.txt")
  DOM_SRC=$(awk -F= '/^dom=/{print $2; exit}' "$OUT/prov-429.txt")
  if [[ -n "$GW_SRC" && -n "$DOM_SRC" && "$GW_SRC" != "$DOM_SRC" &&
        "$GW_SRC" == http://* &&
        "$DOM_SRC" =~ ^build/smoke-s5-04/(try|axis)-[0-9]+\.html$ ]] &&
     grep -q '^gateway=.*rate_limit_rejected [0-9][0-9]*->[0-9][0-9]*$' "$OUT/prov-429.txt"; then
    ok "等式两边的通道指纹齐且**不同源**：被测方=$GW_SRC（两次读数差），页面=$DOM_SRC（落盘快照）"
  else
    fail "互背书形状不成立：两条通道同源、缺失，或被测方那一半不是从网关观测面读来的（gw=[$GW_SRC] dom=[$DOM_SRC]）"
  fi
  if (( N_TEXT > 0 )) && [[ "$N_TEXT" == "$GOT429" && "$N_TEXT" == "$D_REJECTED" ]]; then
    ok "文案条数 $N_TEXT == 哨兵 got429 $GOT429 == Δrate_limit_rejected $D_REJECTED（三处逐数相等）"
  else
    fail "429 对不上：文案 $N_TEXT / 哨兵 $GOT429 / 网关增量 $D_REJECTED"
  fi
  (( N_TEXT > 0 )) && ok "下界：429 确实发生过（不是把桶读空当成限流没生效）" \
    || fail "一条 429 都没出现：突发没打在同一条路径上？"
  if printf '%s\n' "$LOG" | grep -q '[^[:print:][:space:]]'; then
    fail "消息流里出现非可打印字节——那正是「把帧当文本误显示」的形状"
  else
    ok "消息流全是可打印字符（帧没被当文本误显示；畸形帧的实测表现是断连不是文本）"
  fi

  # ---------------- 阶段 6：句③ ----------------
  echo "-- 阶段 6：句③ 断开时配对两侧都收"
  printf '%s\n' "$LOG" | grep -qx 'closed code=1005' \
    && ok "页面侧 closed code=1005（实测：浏览器主动关的收尾码是 1005 而不是 1000）" \
    || fail "页面侧没读到 closed code=1005：$(printf '%s\n' "$LOG" | tail -2 | tr '\n' '|')"
  i=0
  until [[ "$(metric live_connections)" == "0" ]] || (( i > 16 )); do ((i++)); sleep 0.3; done
  LIVE=$(metric live_connections)
  CON_C=$(metric connections_constructed); CON_D=$(metric connections_destructed)
  [[ "$LIVE" == "0" ]] && ok "网关侧 live_connections 回到 0" \
    || fail "网关侧 live_connections=$LIVE（页面断开后仍有未收的连接）"
  [[ -n "$CON_C" && "$CON_C" == "$CON_D" ]] \
    && ok "constructed==destructed==$CON_C（配对两头都收）" \
    || fail "配对漂移：constructed=$CON_C destructed=$CON_D"

  # ---------------- 阶段 7：仪器对照 ----------------
  echo "-- 阶段 7：取数方式对照（限定元素 vs 整篇文档）"
  WHOLE=$(grep -o '429 rate limited' "$GOOD" | wc -l | tr -d ' ')
  if (( WHOLE > N_TEXT )); then
    ok "整篇命中 $WHOLE > 限定命中 $N_TEXT —— 断言必须限定元素，这条是对照实验不是习惯"
  else
    fail "整篇命中 $WHOLE 不大于限定 $N_TEXT：页面源码里那句文案去哪了？（取数范围本身没被钉住）"
  fi

  # ---------------- 阶段 8：句④ ----------------
  echo "-- 阶段 8：句④ 只有长连接算数"
  printf '%s\n' "$LOG" | grep -qx 'same-connection recv=2 readyState=1' \
    && ok "第二条消息发出前连接仍是 OPEN 且 recv 已累计到 2 —— 两条消息共用同一条长连接" \
    || fail "没有 same-connection 那一行：两条消息不在同一条连接上"
fi

# ---------------- 阶段 9：三方同轴（一台新进程只负责一轮）----------------
echo "-- 阶段 9：三方同轴——每轮换一台**全新上游进程**，自报数天然只等于成交的那一轮"
# 为什么一台进程一轮：第一版在这里连跑两轮共用一个进程，结果第一轮空哨兵、却已经回了 2 帧，
# 累计数成了 5 而不是预言的 4——重试次数污染了等式。既然"单次快照不可靠"是实测事实，
# 就别让不成交的那些轮进入账本：不成交就整台重启，成交那一轮的自报数是干净的。
kill -TERM "$U" 2>/dev/null; wait "$U" 2>/dev/null
AX_OK=0
for ax in 1 2 3 4 5 6; do
  sleep 0.3
  "$UPSTREAM_BIN" --listen "127.0.0.1:$U_PORT" --ws-echo --html "$PAGE_SRC" > "$OUT/up-axis-$ax.log" 2>&1 &
  UAX=$!
  PIDS=("$UAX" "$GW")
  sleep 0.6
  B_R=$(metric rate_limit_rejected); B_U=$(metric upgrades_101)
  run_headless "$URL" "$OUT/axis-$ax.html"; ROUNDS=$((ROUNDS + 1))
  A_R=$(metric rate_limit_rejected); A_U=$(metric upgrades_101)
  s=$(sentinel_of "$OUT/axis-$ax.html")
  echo "  axis=$ax Δupgrades=$((A_U-B_U)) Δrejected=$((A_R-B_R)) sentinel=[$s]" | tee -a "$OUT/attempts.log"
  kill -TERM "$UAX" 2>/dev/null; wait "$UAX" 2>/dev/null
  if [[ "$s" != DONE* ]]; then continue; fi
  HANDSHAKES=$(sed -n 's/.*握手101数=\([0-9]*\).*/\1/p' "$OUT/up-axis-$ax.log" | tail -1)
  FRAMES=$(sed -n 's/.*回显帧数=\([0-9]*\).*/\1/p' "$OUT/up-axis-$ax.log" | tail -1)
  AX_OK=1
  # 每轮三条帧：两条消息回显 + 一条 close 的回帧（RFC 6455 要求对 close 回一条，夹具把它也计入）。
  # 第一版按 2 预言，实测 3——那是我对机制的预判错，不是被测方错，按"照记 + 归因"改预言值。
  [[ "$HANDSHAKES" == "1" && "$FRAMES" == "3" ]] \
    && ok "成交轮（第 $ax 次尝试）的上游自报：握手101数=1、回显帧数=3（2 消息 + 1 close 回帧）" \
    || fail "上游自报不等于这一轮该有的数：握手101数=$HANDSHAKES 回显帧数=$FRAMES，期望 1/3"
  [[ "$((A_U - B_U))" == "1" ]] && ok "网关 Δupgrades_101 == 1（一轮一次升级）" \
    || fail "网关 Δupgrades_101=$((A_U - B_U))，期望 1"
  G429=$(field_of "$s" got429)
  [[ "$((A_R - B_R))" == "$G429" ]] \
    && ok "网关 Δrate_limit_rejected $((A_R - B_R)) == 页面数到的 429 条数 $G429" \
    || fail "429 对不上：网关 $((A_R - B_R)) vs 页面 $G429"
  break
done
(( AX_OK )) || fail "六次尝试都没有成交轮，同轴等式无法判定"

# ---------------- 阶段 10：生命周期 ----------------
echo "-- 阶段 10：网关排水与生命周期"
kill -TERM "$GW" 2>/dev/null; wait "$GW" 2>/dev/null
serv() { sed -n "s/^\[serve\] $1=\([0-9-]*\).*/\1/p" "$OUT/gateway.log" | tail -1; }
FD_S=$(serv fd_start); FD_E=$(serv fd_end)
LIVE_E=$(serv live_connections); CON_E=$(serv connections_constructed)
DESTR_E=$(serv connections_destructed); UPG_E=$(serv upgrades_101)
[[ -n "$FD_S" && "$FD_S" == "$FD_E" ]] && ok "fd $FD_S→$FD_E 逐数相等（生命周期不变式）" \
  || fail "fd 没回基线：start=$FD_S end=$FD_E"
[[ "$LIVE_E" == "0" ]] && ok "退出账 live_connections=0" || fail "退出账 live_connections=$LIVE_E"
[[ -n "$CON_E" && "$CON_E" == "$DESTR_E" ]] && ok "退出账 constructed==destructed==$CON_E" \
  || fail "退出账配对漂移：constructed=$CON_E destructed=$DESTR_E"
# upgrades_101 是**整个网关进程**的累计数，跨阶段 3 的每一次尝试与阶段 9 的每一次尝试，
# 所以我不能预言一个"猜出来的 ≥3"（第一版就是这么写的，实测读到 2 就红了——红的是预言）。
# 能推导的是双侧界：
#   下界 2 —— 阶段 3 与阶段 9 各有一轮拿到了完成哨兵，成交轮必然各计一次升级；
#   上界 ROUNDS —— 每一轮只发一次握手，累计数不可能超过跑过的轮数。
(( ${UPG_E:-0} >= 2 && ${UPG_E:-0} <= ROUNDS )) \
  && ok "退出账 upgrades_101=$UPG_E 落在可推导的界内：2 ≤ $UPG_E ≤ 跑过的轮数 $ROUNDS" \
  || fail "退出账 upgrades_101=$UPG_E 越界（期望 2..$ROUNDS）"

REACHED_END=1
echo "== 判定：checks=$CHECKS fails=$FAILS"
if (( FAILED == 0 )); then
  echo "PASS checks=$CHECKS fails=0" > "$OUT/verdict.txt"
  echo "S5-04 冒烟：全绿（$CHECKS 项判定、✗ 计数 0）"
else
  echo "FAIL checks=$CHECKS fails=$FAILS" > "$OUT/verdict.txt"
  echo "S5-04 冒烟：有 ✗，见上面"
fi
(( REACHED_END )) || echo "  ✗ 脚本没走到最后一行——这不是通过" >&2
echo "  判定落盘：$OUT/verdict.txt（以它为准，不以退出码为准）"
trap - EXIT
cleanup
exit "$FAILED"
