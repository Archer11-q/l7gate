#!/usr/bin/env bash
# S5-07 验收句④ · 火焰图取数。只按 docs/ENVIRONMENT.md 4.1 那一种**已验证形态**：
#   ① 由 perf **直接拉起**被测进程（`perf record … -- <被测命令行>`），不事后 attach（paranoid=2 下不通）；
#   ② 只有**用户态**栈（`-a` 系统级与内核帧这台机器拿不到）；
#   ③ nginx 对照臂**同法**——所以它也得由容器里的 perf 拉起 nginx 自己那个进程，
#      这条能力由本脚本的 ARM=nginx 分支现测：拿不到就**拒跑并说明**，不换成"只画网关那一侧然后当证据齐了"。
# `kernel.perf_event_paranoid` 保持 2，不抬（已裁第 ⑥ 条），所以内核时间去哪了这一格今天没有直接证据，
# 侧面证据是每请求 `epoll_modifications` 那套整数等式（B-013/B-016/B-018/B-019）。
#
# 用法：
#   scripts/bench/s5-07-flamegraph.sh                      # ARM=gw：网关原生 + wrk 满负荷 60s
#   ARM=nginx scripts/bench/s5-07-flamegraph.sh            # ARM=nginx：容器里由 perf 拉起 nginx（先测能力）
#   WORKERS=16 CONNECTIONS=1000 DURATION=60s scripts/bench/s5-07-flamegraph.sh
set -uo pipefail
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1

BIN="build/release"
ARM="${ARM:-gw}"
PERF="${PERF:-perf}"
WORKERS="${WORKERS:-16}"
CONNECTIONS="${CONNECTIONS:-1000}"
DURATION="${DURATION:-60s}"
WARMUP="${WARMUP:-10s}"
WRK_THREADS="${WRK_THREADS:-8}"
GW_PORT="${GW_PORT:-18188}"
UP_PORT="${UP_PORT:-18189}"
FREQ="${FREQ:-99}"
IMAGE_GATEWAY="l7gate/runtime:s5-05"
IMAGE_NGINX="nginx:1.25.4"
OUT="${OUT_DIR:-build/bench/s5-07-flame-$ARM-$(date +%Y%m%d-%H%M%S)}"

mkdir -p "$OUT" || exit 1
OUT=$(readlink -f "$OUT")
FAILED=0
note() { echo "  $*"; }
ok()   { echo "  ✓ $*"; }
die()  { echo "  ✗ $*" >&2; FAILED=1; }
fatal() { echo "  ✗ $*" >&2; exit 1; }

PARANOID=$(sysctl -n kernel.perf_event_paranoid 2>/dev/null || echo unknown)
command -v "$PERF" >/dev/null || fatal "本机没有 perf：这一格的证据手段不在（不换手段顶替，照 ADR-13 的姿势先记录）"

UP_PID=""; G_PID=""; NGX_C=""
cleanup() {
  [[ -n "$G_PID" ]] && { kill -TERM "$G_PID" 2>/dev/null; wait "$G_PID" 2>/dev/null; }
  [[ -n "$UP_PID" ]] && { kill -TERM "$UP_PID" 2>/dev/null; wait "$UP_PID" 2>/dev/null; }
  [[ -n "$NGX_C" ]] && docker rm -f "$NGX_C" >/dev/null 2>&1
}
trap cleanup EXIT

# ---------- 上游：两臂共用同一个原生夹具（绑 0.0.0.0，容器也够得到） ----------
[[ -x "$BIN/tools/l7gate_bench_upstream" ]] || fatal "找不到 $BIN/tools/l7gate_bench_upstream；先 cmake --build --preset release"
command -v wrk >/dev/null || fatal "本机没有 wrk：这一档要在满负荷下采样，空转的火焰图不算证据"
"$BIN/tools/l7gate_bench_upstream" --listen "0.0.0.0:$UP_PORT" --body-bytes 1024 \
  --max-clients $((CONNECTIONS * 3 + 256)) --backlog $((CONNECTIONS * 3 + 256)) > "$OUT/upstream.log" 2>&1 &
UP_PID=$!
for i in $(seq 1 150); do
  curl -s --max-time 2 -o /dev/null "http://127.0.0.1:$UP_PORT/bench" && break
  sleep 0.1
done
curl -s --max-time 5 -o /dev/null "http://127.0.0.1:$UP_PORT/bench" || fatal "上游没起来（$OUT/upstream.log）"
WSL_IP=$(hostname -I | awk '{print $1}')

# ---------- 采样：由 perf 直接拉起被测进程 ----------
if [[ "$ARM" == "gw" ]]; then
  [[ -x "$BIN/l7gate" ]] || fatal "找不到 $BIN/l7gate"
  WSL_RLIMIT="$(ulimit -Hn)"
  NEED_FD=$(( CONNECTIONS * 2 + 512 + WORKERS * 2 ))
  [[ "$WSL_RLIMIT" -ge "$NEED_FD" ]] || fatal "hard nofile=$WSL_RLIMIT < $NEED_FD：这一档会被 fd 上限打死，采到的不是网关的形状"
  ulimit -n "$NEED_FD"
  note "ARM=gw：perf 直接拉起 $BIN/l7gate（workers=$WORKERS，负载 wrk -c$CONNECTIONS -d$DURATION）"
  "$PERF" record -e cpu-clock -F "$FREQ" -g -o "$OUT/perf.data" -- \
    "$BIN/l7gate" --serve --listen "127.0.0.1:$GW_PORT" --workers "$WORKERS" \
      --upstream "127.0.0.1:$UP_PORT" > "$OUT/perf.stdout.txt" 2>&1 &
  G_PID=$!
  i=0
  while (( i < 300 )); do
    grep -q '\[serve\] 监听' "$OUT/perf.stdout.txt" 2>/dev/null && break
    kill -0 "$G_PID" 2>/dev/null || fatal "perf/网关起的瞬间就退了，看 $OUT/perf.stdout.txt"
    sleep 0.1; ((i += 1))
  done
  grep -q '\[serve\] 监听' "$OUT/perf.stdout.txt" || fatal "网关 30s 内没打印监听行"
  TARGET_URL="http://127.0.0.1:$GW_PORT/bench"
  # 路径签名要**按档分叉**：单 Reactor 那条横幅打的是 `max_connections=`，多 Reactor 才打 `workers=N`。
  # 第一版我没分叉，无条件找 `workers=1 `，于是 workers=1 那一发在自己的门上作废（s1-p99/s1-p999 里
  # 这条门从一开始就是两档写的，我抄漏了另一半）。
  if (( WORKERS >= 2 )); then
    grep -q "\[serve\] 监听 .*workers=$WORKERS " "$OUT/perf.stdout.txt" \
      || fatal "请求 --workers $WORKERS，但网关横幅不是多 Reactor 路径：这一档作废"
  else
    grep -q "\[serve\] 监听 .*max_connections=" "$OUT/perf.stdout.txt" \
      || fatal "请求 --workers 1，但网关横幅不是单 Reactor 路径：这一档作废"
  fi
elif [[ "$ARM" == "nginx" ]]; then
  # 能力现测：容器里没有 perf 就**拒跑**，不换手段、不省这一格
  docker image inspect "$IMAGE_NGINX" >/dev/null 2>&1 || fatal "本机没有 $IMAGE_NGINX"
  note "ARM=nginx：先测容器里能不能由 perf 拉起 nginx 自己（4.1 那条「同法」今天到底成不成立）"
  # perf 与其依赖库挂进容器：壳镜像与宿主 glibc 逐字同代才走得通这条路；nginx 那台是 Debian 12（glibc 2.36）
  PERFLIB="$OUT/perflib"; mkdir -p "$PERFLIB"
  cp -f "$(command -v perf)" "$OUT/perf" 2>/dev/null || fatal "取不到 perf 二进制"
  for lib in $(ldd "$(command -v perf)" 2>/dev/null | sed -n 's/.*=> \(\/[^ ]*\).*/\1/p'); do
    cp -n "$lib" "$PERFLIB/" 2>/dev/null
  done
  # 与档二同一个 nginx 配置形状（默认态：worker_processes auto + access_log 默认开）
  cat > "$OUT/nginx-default.conf" <<EOF
upstream bench { server $WSL_IP:$UP_PORT; keepalive 1024; }
server {
  listen 80;
  location / {
    proxy_pass http://bench;
    proxy_http_version 1.1;
    proxy_set_header Connection "";
  }
}
EOF
  NGX_C="l7gate-s5-07-flame-nginx"
  docker rm -f "$NGX_C" >/dev/null 2>&1
  docker run -d --name "$NGX_C" --cap-add=CAP_PERFMON --security-opt seccomp=unconfined \
    -p "127.0.0.1:18198:80" \
    -v "$OUT/nginx-default.conf:/etc/nginx/conf.d/default.conf:ro" \
    -v "$OUT:/probe:ro" \
    --entrypoint sh "$IMAGE_NGINX" -c \
      'LD_LIBRARY_PATH=/probe/perflib /probe/perf record -e cpu-clock -F 99 -g -o /tmp/perf.data -- nginx -g "daemon off;" > /probe/nginx-perf.stdout.txt 2>&1; sleep 3600' \
    >/dev/null 2>&1 || fatal "nginx+perf 容器起不来（看 docker logs $NGX_C）"
  for i in $(seq 1 300); do
    [[ "$(curl -s -o /dev/null -w '%{http_code}' --max-time 2 http://127.0.0.1:18198/bench)" == "200" ]] && break
    sleep 0.1
  done
  [[ "$(curl -s -o /dev/null -w '%{http_code}' --max-time 3 http://127.0.0.1:18198/bench)" == "200" ]] \
    || { docker logs "$NGX_C" > "$OUT/nginx.docker.log" 2>&1; fatal "nginx 容器 30s 内没答 200：这一臂取不到，看 $OUT/nginx.docker.log"; }
  TARGET_URL="http://127.0.0.1:18198/bench"
else
  fatal "ARM 只认 gw 或 nginx，收到 $ARM"
fi

# ---------- 满负荷 ----------
note "预热 $WARMUP（结果丢掉）再正式 $DURATION"
wrk -t"$WRK_THREADS" -c"$CONNECTIONS" -d"$WARMUP" "$TARGET_URL" > "$OUT/wrk.warmup.txt" 2>&1
wrk -t"$WRK_THREADS" -c"$CONNECTIONS" -d"$DURATION" --latency "$TARGET_URL" > "$OUT/wrk.txt" 2>&1
NON2XX=$(sed -n 's/.*Non-2xx or 3xx responses: \([0-9]*\).*/\1/p' "$OUT/wrk.txt")
QPS=$(sed -n 's/^Requests\/sec:[[:space:]]*\([0-9.]*\).*/\1/p' "$OUT/wrk.txt" | head -1)
[[ -n "$QPS" ]] || fatal "wrk 的 Requests/sec 没解析出来"
[[ -z "$NON2XX" ]] || die "wrk 报了 $NON2XX 条非 2xx：这一档的负载形状不成立，采到的栈属于失败路径"
ok "负载：QPS=$QPS"

# ---------- 收尾：SIGTERM 被测进程，perf 落盘 ----------
if [[ "$ARM" == "gw" ]]; then
  sleep 2
  kill -TERM "$G_PID" 2>/dev/null
  for _ in $(seq 1 400); do kill -0 "$G_PID" 2>/dev/null || break; sleep 0.1; done
  wait "$G_PID" 2>/dev/null
  G_RC=$?
  [[ "$G_RC" == "0" || "$G_RC" == "143" ]] || die "网关经 perf 退出码 $G_RC：优雅停止没生效"
  cp -f "$OUT/perf.stdout.txt" "$OUT/gateway.log" 2>/dev/null
else
  docker stop -t 20 "$NGX_C" >/dev/null 2>&1
  docker cp "$NGX_C:/tmp/perf.data" "$OUT/perf.data" >/dev/null 2>&1 || die "取不到容器里的 perf.data：这一臂没有证据"
fi
[[ -s "$OUT/perf.data" ]] || fatal "perf.data 不存在或为空：这一臂取不到（不画空图、不写空结论）"

# ---------- 折叠与报告 ----------
"$PERF" report -i "$OUT/perf.data" --stdio --percent-limit 0.05 > "$OUT/perf.report.txt" 2>&1
RC=$?
[[ "$RC" == "0" ]] || die "perf report 退出码 $RC"
# 另取一张**自占比榜**（`--no-children -g none`）：上面那张带调用图的表里，一屏可以只装得下一个符号的
# 十六进制碎片，读不出"谁最贵"。归因段要的是名次，所以两份都留，榜在这份里。
"$PERF" report -i "$OUT/perf.data" --stdio --no-children -g none --percent-limit 0.2 > "$OUT/perf.self-rank.txt" 2>&1
"$PERF" script -i "$OUT/perf.data" 2>/dev/null > "$OUT/perf.script.txt"
# 标准折叠：每个采样的调用链按 根;…;叶 拼一行，同链计数相加（用户态栈；内核帧这台机器根本没有）。
# **这里的取数器错过一次，形状记着**：第一版认帧行的正则是 `^[ \t]+[a-zA-Z_]`，而 `perf script` 的帧行是
# 「空白 + 十六进制地址 + 符号+0x偏移 + (dso)」——地址打头，所以一帧都没匹配上，折叠出来全是
# `[unknown] ([unknown])`，被我自己那条"栈里没有被测方符号"的门判红（门是对的，取数器是坏的）。
# 现在按真实版式取第二个字段，并把符号尾部的 `+0x…` 削掉；`[unknown]` 保留成它自己，不伪装成有名字。
awk '
  /^[^ \t]/ {
    if (n > 0) { s = stack[n]; for (i = n - 1; i >= 1; i--) s = s ";" stack[i];
                 if (s != "") folds[s]++ }
    n = 0; delete stack; next
  }
  /^[ \t]+[0-9a-fA-F]+[ \t]/ {
    sym = $2
    sub(/\+0x[0-9a-fA-F]+$/, "", sym)
    if (sym == "") next
    stack[++n] = sym
  }
  END {
    if (n > 0) { s = stack[n]; for (i = n - 1; i >= 1; i--) s = s ";" stack[i];
                 if (s != "") folds[s]++ }
    for (f in folds) printf "%s %d\n", f, folds[f]
  }
' "$OUT/perf.script.txt" | sort -k2,2nr > "$OUT/folded.txt"
SAMPLES=$(grep -c '^' "$OUT/folded.txt" 2>/dev/null || echo 0)
TOTAL=$(awk '{ t += $NF } END { print t + 0 }' "$OUT/folded.txt")
[[ "$TOTAL" -gt 0 ]] || die "折叠后一条采样都没有：perf.data 非空但取不出栈，这一臂的证据不成立"
# 下界：栈里必须真的出现被测方的用户态符号——否则"有图"不等于"画的是被测方"
if [[ "$ARM" == "gw" ]]; then
  grep -qE 'l7gate|net::|Http::|Reactor' "$OUT/folded.txt" \
    || die "折叠栈里没有被测方的符号：采到的可能只是 wrk 或壳层，这一格不算证据"
else
  grep -qE 'nginx' "$OUT/folded.txt" \
    || die "折叠栈里没有 nginx 的符号：这一臂采的不是被测方"
fi
KERN=$(grep -c 'vmlinux\|\[k\]' "$OUT/folded.txt" 2>/dev/null || echo 0)

{
  echo "# S5-07 火焰图取数（s5-07-flamegraph.sh）ARM=$ARM —— $(date -Is)"
  echo "形态: 由 perf 直接拉起被测进程（不 attach）；事件 cpu-clock -F $FREQ -g；kernel.perf_event_paranoid=$PARANOID（不抬，已裁第 ⑥ 条）"
  echo "负载: wrk -t$WRK_THREADS -c$CONNECTIONS -d$DURATION（预热 $WARMUP 丢掉）QPS=$QPS 非2xx=${NON2XX:-0}"
  echo "被测: ARM=gw → $BIN/l7gate workers=$WORKERS ；ARM=nginx → $IMAGE_NGINX 容器内由 perf 拉起 nginx 自己"
  echo "采样: 折叠后唯一栈 $SAMPLES 条 / 总采样 $TOTAL 个；内核帧 $KERN 条（这台机器拿不到，所以「内核时间去哪了」只能由 syscall 等式侧面读）"
  echo "声明: 只有用户态栈 = 4.1 那一种已验证形态；本档不做任何 SLO 判定，只作 #3 差值的归因证据"
  echo "产物: perf.data / perf.report.txt（perf 自己的 top-down）/ folded.txt / flame.svg（由 scripts/report/perf-fold-to-svg.awk 自渲，无第三方工具）"
} > "$OUT/summary.txt"
awk -f scripts/report/perf-fold-to-svg.awk -v title="l7gate · ARM=$ARM" "$OUT/folded.txt" > "$OUT/flame.svg" 2>/dev/null
[[ -s "$OUT/flame.svg" ]] || note "（SVG 渲染没产出，证据仍以 perf.report.txt + folded.txt 为准）"
cat "$OUT/summary.txt"
head -20 "$OUT/perf.report.txt"

kill -TERM "$UP_PID" 2>/dev/null; wait "$UP_PID" 2>/dev/null; UP_PID=""
echo
echo "== 结果落在 $OUT"
if (( FAILED != 0 )); then echo "== 本轮有 ✗，上面的证据不要单独引用"; fi
exit "$FAILED"
