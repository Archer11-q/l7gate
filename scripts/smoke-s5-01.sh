#!/usr/bin/env bash
# S5-01 冒烟：指标暴露面在**真进程**上的四件事（ADR-24）。
#
#   阶段 1  关闭态（不给 --metrics-port）：退出账的键集合与**修前二进制**逐行相同
#           ——这一条是验收 ⑤「关掉时代码路径与今天逐字节相同」唯一能被机械判掉的形状。
#           对照对象是 `e229ce0`（ADR-24 那一笔、`src/` 未动）编出来的 l7gate，不是我的记忆。
#           golden 清单 `scripts/golden/serve-keys-off.txt` 的**来源就是那次修前二进制的实测输出**
#           （60 格；这一臂的装配里没有 health/nodes/redis/rate_limit，所以未装配的段不参与）。
#           以后新增键要先改 golden 才过得去——于是"关闭态多一个键"必须由人签字。
#   阶段 2  开启态：`/metrics` 的键集合与同一装配下的退出账互证（验收 ①「同名同口径」）——
#           差集必须**恰好**等于 ADR-24 第五节写死的那五格增/三格例外，多一格少一格都红。
#           外加 20 次抓取 × 100 条业务请求：`requests_parsed` 必须恰好等于 100（验收 ②
#           在进程侧的复证，ctest 那条证的是结构，这条证的是真 socket）。
#           外加 `fd_start == fd_end`——监听 fd 建在采样点之前那条硬前置的实测落点。
#   阶段 3  坏请求：404/405 各一次、`metrics_scrapes_failed` 跟上、随后一次 /metrics 仍 200。
#   阶段 4  参数校验：越界与非数字一律退出码 2 并点名（与其它数值旋钮同形）。
#   阶段 5  `--metrics-port 0`：内核给端口，且那个端口真的能 curl 通（"0 是功能不是便利"）。
#
# 用法：
#   PRESET=debug scripts/smoke-s5-01.sh
#   BASELINE_BIN=/home/archer/l7gate-base-e229ce0/build/debug/l7gate PRESET=debug scripts/smoke-s5-01.sh
#     ↑ 给了 BASELINE_BIN 才跑阶段 1 那条修前对照；不给就只跑 golden 清单（新加的那格会跳过并写明"未跑"）。
# 输出：build/smoke-s5-01/，退出码 0 = 全过。
set -uo pipefail
cd "$(dirname "$(readlink -f "$0")")/.." || exit 1

trap '' PIPE
PRESET="${PRESET:-release}"
BIN="build/$PRESET"
GATEWAY="$BIN/l7gate"
UPSTREAM="$BIN/tests/l7gate_test_upstream"
BASELINE_BIN="${BASELINE_BIN:-}"
GOLDEN="scripts/golden/serve-keys-off.txt"
OUT="build/smoke-s5-01"
rm -rf "$OUT"; mkdir -p "$OUT"
PIDFILE="$OUT/pids"
: > "$PIDFILE"

FAILED=0
SKIPPED=0
fail() { echo "  ✗ $*"; FAILED=1; }
ok()   { echo "  ✓ $*"; }
skip() { echo "  ～ 跳过：$*（跳过＝不判，不是通过）"; SKIPPED=$((SKIPPED+1)); }
die()  { echo "  fatal: $*" >&2; exit 1; }

[[ -x "$GATEWAY" ]] || die "找不到 $GATEWAY，先 cmake --build --preset $PRESET（全目标，口径⑪）"
[[ -x "$UPSTREAM" ]] || die "找不到 $UPSTREAM，先 cmake --build --preset $PRESET"
command -v curl >/dev/null || die "本机没有 curl"

GW_PORT=18520
UP_PORT=18521
METRICS_PORT=18522

port_busy() { (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; }
for p in "$GW_PORT" "$UP_PORT" "$METRICS_PORT"; do
  port_busy "$p" && die "端口 $p 已被占用：先清场"
done

wait_for_line() { # logfile pattern [seconds]
  local log=$1 pat=$2 secs=${3:-20} i=0
  while (( i < secs * 20 )); do
    grep -q -- "$pat" "$log" 2>/dev/null && return 0
    ((i++)); sleep 0.05
  done
  return 1
}

get_stat() { # key logfile → 值（不假设键在行首：口径⑭(b)）
  local key=$1 log=$2
  grep -o "[[:space:]]$1=[0-9-]*" "$log" 2>/dev/null | head -1 | sed 's/.*=//'
}

# 退出账的键名集合：把每行 `[serve] k=v` 里的 k 提出来，排序去重。
# 横幅行（`[serve] 监听 …`、`[serve] redis=on …`）不贡献键名——它们没有 `k=v` 形状。
serve_keys() { # logfile → 键名清单
  sed -n 's/^\[serve\] \([a-z0-9_]*\)=[0-9-].*/\1/p' "$1" | sort -u
}

# /metrics 的 metric 名集合：只取样本行（不以 # 开头），点号那一族在渲染时已换成下划线。
metric_names() { # body-file → 名字清单
  grep -v '^#' "$1" | sed -n 's/^\([a-z0-9_:]*\) .*/\1/p' | sort -u
}

start_upstream() {
  port_busy "$UP_PORT" && die "端口 $UP_PORT 上还压着进程（上一臂没退干净）"
  "$UPSTREAM" --listen "127.0.0.1:$UP_PORT" --body-bytes 64 > "$OUT/upstream.log" 2>&1 &
  local pid=$!; echo "$pid" >> "$PIDFILE"
  wait_for_line "$OUT/upstream.log" '\[upstream\] 监听' 10 || die "上游起不来"
  echo "$pid"
}

start_gateway() { # 标签 额外参数… → pid；日志在 $OUT/gateway-标签.log
  local tag=$1; shift
  port_busy "$GW_PORT" && die "端口 $GW_PORT 上还压着进程（上一臂没退干净）"
  "$GATEWAY" --serve --listen "127.0.0.1:$GW_PORT" --upstream "127.0.0.1:$UP_PORT" "$@" \
    > "$OUT/gateway-$tag.log" 2>&1 &
  local pid=$!; echo "$pid" >> "$PIDFILE"
  wait_for_line "$OUT/gateway-$tag.log" '\[serve\] 监听' 10 || die "网关起不来（$tag）：$(tail -3 "$OUT/gateway-$tag.log")"
  echo "$pid"
}

# 优雅停止：等退出账打完（`event_batches` 是最后一批键之一）。
stop_gateway() { # pid → 0/1
  local pid=$1
  kill -TERM "$pid" 2>/dev/null || return 1
  local i=0
  while (( i < 200 )); do
    kill -0 "$pid" 2>/dev/null || return 0
    ((i++)); sleep 0.05
  done
  return 1
}

# 打 N 条 keep-alive 请求（一条 curl 连接跑一串，进程侧计数才对得上）。
curl_many() { # 条数
  local n=$1 i
  for ((i = 0; i < n; i++)); do
    curl -s -o /dev/null "http://127.0.0.1:$GW_PORT/probe-$i" || return 1
  done
  return 0
}

echo "=== S5-01 冒烟：PRESET=$PRESET ==="
UP_PID=$(start_upstream)

# ---------------------------------------------------------------- 阶段 1：关闭态
echo
echo "--- 阶段 1：关闭态（不给 --metrics-port）"
GW1=$(start_gateway off)
curl_many 20 || fail "阶段 1：curl 没跑完"
stop_gateway "$GW1" || fail "阶段 1：网关没在期限内退出"
serve_keys "$OUT/gateway-off.log" > "$OUT/keys-off.txt"

# 与 golden 清单比。golden 是**手写在仓库里**的：新增键要先改 golden 才过得去，
# 于是"关掉时多一个键"这件事必须由人签字，不能由脚本默认接受。
if [[ -f "$GOLDEN" ]]; then
  if diff -u "$GOLDEN" "$OUT/keys-off.txt" > "$OUT/keys-off.diff"; then
    ok "关闭态键集合与 golden 逐行相同（$(wc -l < "$OUT/keys-off.txt") 个键）"
  else
    fail "关闭态键集合与 golden 不同：$(head -8 "$OUT/keys-off.diff" | tr '\n' ' ')"
  fi
else
  fail "缺 $GOLDEN：golden 清单必须入库，脚本不接受'第一次跑就是真值'"
fi

# 与**修前二进制**比：这条才是"逐字节相同"那句的硬证，golden 只是它的可复现替身。
if [[ -n "$BASELINE_BIN" && -x "$BASELINE_BIN" ]]; then
  # 直接调 $BASELINE_BIN，参数与本臂逐字同形（不重新指 GATEWAY，避免把两条臂的日志搞混）。
  port_busy "$GW_PORT" && die "端口 $GW_PORT 没空出来"
  "$BASELINE_BIN" --serve --listen "127.0.0.1:$GW_PORT" --upstream "127.0.0.1:$UP_PORT" \
    > "$OUT/gateway-baseline.log" 2>&1 &
  BASE_PID=$!; echo "$BASE_PID" >> "$PIDFILE"
  wait_for_line "$OUT/gateway-baseline.log" '\[serve\] 监听' 10 || die "修前二进制起不来"
  curl_many 20 || fail "阶段 1：修前对照臂 curl 没跑完"
  stop_gateway "$BASE_PID" || fail "阶段 1：修前对照臂没退出"
  serve_keys "$OUT/gateway-baseline.log" > "$OUT/keys-baseline.txt"
  if diff -u "$OUT/keys-baseline.txt" "$OUT/keys-off.txt" > "$OUT/keys-vs-baseline.diff"; then
    ok "关闭态键集合与修前二进制逐行相同（$BASELINE_BIN）"
  else
    fail "关闭态与修前二进制的键集合不同：$(head -8 "$OUT/keys-vs-baseline.diff" | tr '\n' ' ')"
  fi
else
  skip "阶段 1 的修前二进制对照（没给 BASELINE_BIN）"
fi

for k in metrics_scrapes_ok metrics_scrapes_failed metrics_render_bytes; do
  if grep -q "[[:space:]]$k=" "$OUT/gateway-off.log"; then
    fail "关闭态却发了 $k —— 仪器自己的键不该出现在没开的面板上"
  fi
done
grep -q '\[serve\] metrics=on' "$OUT/gateway-off.log" && fail "关闭态却打了 metrics=on 横幅"
if (exec 3<>/dev/tcp/127.0.0.1/$METRICS_PORT) 2>/dev/null; then
  fail "没给 --metrics-port，端口 $METRICS_PORT 上却有人应答"
else
  ok "关闭态：端口 $METRICS_PORT 无人监听（connect 直接被拒）"
fi
LIVE=$(get_stat live_connections "$OUT/gateway-off.log")
CONSTR=$(get_stat connections_constructed "$OUT/gateway-off.log")
DESTR=$(get_stat connections_destructed "$OUT/gateway-off.log")
PARSED=$(get_stat requests_parsed "$OUT/gateway-off.log")
FD_S=$(get_stat fd_start "$OUT/gateway-off.log")
FD_E=$(get_stat fd_end "$OUT/gateway-off.log")
[[ "$LIVE" == "0" ]] && ok "live_connections=0" || fail "live_connections=$LIVE"
[[ -n "$CONSTR" && "$CONSTR" == "$DESTR" ]] && ok "constructed==destructed==$CONSTR" \
  || fail "constructed=$CONSTR / destructed=$DESTR 不等"
[[ "$PARSED" == "20" ]] && ok "requests_parsed=20（20 条 curl，一格不多一格不少）" \
  || fail "requests_parsed=$PARSED，应为 20"
[[ "$FD_S" == "$FD_E" ]] && ok "关闭态 fd $FD_S→$FD_E 回到基线" || fail "关闭态 fd $FD_S→$FD_E 不等"

# ---------------------------------------------------------------- 阶段 2：开启态
echo
echo "--- 阶段 2：开启态（--metrics-port $METRICS_PORT）"
GW2=$(start_gateway on --metrics-port "$METRICS_PORT")
grep -q '\[serve\] metrics=on' "$OUT/gateway-on.log" && ok "横幅里有 metrics=on" \
  || fail "开了端口却没有横幅：$(head -3 "$OUT/gateway-on.log")"
# 抓 20 次，每次都要拿到完整的 exposition。
MOK=0
for ((i = 0; i < 20; i++)); do
  if curl -sf "http://127.0.0.1:$METRICS_PORT/metrics" > "$OUT/metrics-$i.txt"; then
    grep -q '^# TYPE requests_parsed counter$' "$OUT/metrics-$i.txt" && MOK=$((MOK+1))
  fi
  sleep 0.05
done
[[ "$MOK" == "20" ]] && ok "20 次抓取全部拿到 200 + 完整 # TYPE 段" \
  || fail "只有 $MOK/20 次抓取拿到合法的 exposition"
curl -sf "http://127.0.0.1:$METRICS_PORT/metrics" > "$OUT/metrics-body.txt" || fail "最后一次抓取失败"
metric_names "$OUT/metrics-body.txt" > "$OUT/keys-metrics.txt"

# 业务侧照跑 100 条，然后互证两个键集合。
curl_many 100 || fail "阶段 2：业务 curl 没跑完"
stop_gateway "$GW2" || fail "阶段 2：网关没退出"
serve_keys "$OUT/gateway-on.log" > "$OUT/keys-on.txt"

PARSED=$(get_stat requests_parsed "$OUT/gateway-on.log")
[[ "$PARSED" == "100" ]] && ok "开启态下 requests_parsed 恰好 100（20 次抓取一条都没多计）" \
  || fail "requests_parsed=$PARSED，应为 100——抓取混进了业务账"
[[ "$(get_stat responses_completed "$OUT/gateway-on.log")" == "100" ]] \
  && ok "responses_completed 恰好 100" || fail "responses_completed 不是 100"
[[ "$(get_stat metrics_scrapes_ok "$OUT/gateway-on.log")" == "21" ]] \
  && ok "metrics_scrapes_ok=21（20 次循环 + 1 次留档）" \
  || fail "metrics_scrapes_ok=$(get_stat metrics_scrapes_ok "$OUT/gateway-on.log")，应为 21"
FD_S=$(get_stat fd_start "$OUT/gateway-on.log")
FD_E=$(get_stat fd_end "$OUT/gateway-on.log")
[[ "$FD_S" == "$FD_E" ]] && ok "开启态 fd $FD_S→$FD_E 仍回到基线（监听 fd 建在采样点之前那条硬前置）" \
  || fail "开启态 fd $FD_S→$FD_E 不等——指标监听 fd 建晚了，或抓取连接漏了"

# 一、开启 vs 关闭：退出账的差集必须**恰好**是那三个仪器自己的键。
comm -13 "$OUT/keys-off.txt" "$OUT/keys-on.txt" > "$OUT/on-minus-off.txt"
comm -23 "$OUT/keys-off.txt" "$OUT/keys-on.txt" > "$OUT/off-minus-on.txt"
if [[ "$(tr '\n' ' ' < "$OUT/on-minus-off.txt")" == "metrics_render_bytes metrics_scrapes_failed metrics_scrapes_ok " ]] \
   && [[ ! -s "$OUT/off-minus-on.txt" ]]; then
  ok "开启只多三格（metrics_scrapes_ok/failed/render_bytes），关闭的一格没少"
else
  fail "开启/关闭键集合差对不上：多 $(tr '\n' ',' < "$OUT/on-minus-off.txt") / 少 $(tr '\n' ',' < "$OUT/off-minus-on.txt")"
fi

# 二、`/metrics` vs 同一装配的退出账：差集必须恰好等于 ADR-24 第五节那五格增、三格例外。
comm -13 "$OUT/keys-on.txt" "$OUT/keys-metrics.txt" > "$OUT/metrics-minus-exit.txt"
comm -23 "$OUT/keys-on.txt" "$OUT/keys-metrics.txt" > "$OUT/exit-minus-metrics.txt"
if [[ "$(tr '\n' ' ' < "$OUT/metrics-minus-exit.txt")" == "fd_open_now rss_kib_now " ]] \
   && [[ "$(tr '\n' ' ' < "$OUT/exit-minus-metrics.txt")" == "event_batches fd_end rss_end_kib " ]]; then
  ok "/metrics 与退出账同名同口径：多的两格（fd_open_now/rss_kib_now）、少的三格（fd_end/rss_end_kib/event_batches）与 ADR-24 第五节逐字一致"
else
  fail "/metrics 与退出账的差集不对：多 $(tr '\n' ',' < "$OUT/metrics-minus-exit.txt") / 少 $(tr '\n' ',' < "$OUT/exit-minus-metrics.txt")"
fi
# 同名的那些键必须**同名**：把退出账的点号地址换成下划线后逐行比对，一条不许改名字。
# 两侧各自剥掉"只在一侧存在"的那几格——剥的清单就是上面那条差集，两处必须一致。
grep -vE '^(fd_open_now|rss_kib_now|metrics_)' "$OUT/keys-metrics.txt" > "$OUT/metrics-keys-core.txt"
sed 's/[.:]/_/g' "$OUT/keys-on.txt" | grep -vE '^(metrics_|fd_end$|rss_end_kib$|event_batches$)' \
  > "$OUT/exit-keys-normalized.txt"
if diff -u "$OUT/metrics-keys-core.txt" "$OUT/exit-keys-normalized.txt" > "$OUT/keys-name.diff"; then
  ok "两集合规范化后逐行相同：运行期没有任何一格被改过名字（$(wc -l < "$OUT/metrics-keys-core.txt") 格）"
else
  fail "键名有出入：$(head -8 "$OUT/keys-name.diff" | tr '\n' ' ')"
fi
# 装配里没有 health 时这一格本来就不发（与退出账同一条 if），所以只能条件判。
if grep -q '^health_ejected_now ' "$OUT/metrics-body.txt"; then
  grep -q '0 不等于没有坏后端' "$OUT/metrics-body.txt" \
    && ok "health_ejected_now 的 HELP 带上 1.31 边界⑥ 那句限定（面板与 README 有出处可指）" \
    || fail "health_ejected_now 的限定语没出现在 exposition 里"
else
  skip "本臂没装 health，`health_ejected_now` 不该出现；那句限定的断言在 ctest 的 EjectedNowHelpCarriesTheBoundaryQualifier 里"
fi

# ---------------------------------------------------------------- 阶段 3：坏请求
echo
echo "--- 阶段 3：坏请求（上界配下界）"
# 阶段 2 那台已经打完账退出了，这里必须重新起一台——否则三条 curl 全打在空端口上，
# "000" 会被读成"网关回了 000"（本轮第一次跑就是这么红的：脚本自己的错，不是实现的）。
GW_BAD=$(start_gateway bad --metrics-port "$METRICS_PORT")
CODE=$(curl -s -o "$OUT/bad-404.txt" -w '%{http_code}' "http://127.0.0.1:$METRICS_PORT/nope" 2>/dev/null || echo "000")
[[ "$CODE" == "404" ]] && ok "GET /nope → 404（只有 /metrics 这一条路径）" || fail "GET /nope 回了 $CODE，应为 404"
CODE=$(curl -s -o "$OUT/bad-405.txt" -X POST -w '%{http_code}' "http://127.0.0.1:$METRICS_PORT/metrics" 2>/dev/null || echo "000")
[[ "$CODE" == "405" ]] && ok "POST /metrics → 405" || fail "POST /metrics 回了 $CODE，应为 405"
# 下界：失败账要跟上，而**随后一次正路抓取仍必须通**——否则"失败计数在涨"
# 可能只是因为这条路从来没通过。
CODE=$(curl -s -o "$OUT/good-after-bad.txt" -w '%{http_code}' "http://127.0.0.1:$METRICS_PORT/metrics" 2>/dev/null || echo "000")
[[ "$CODE" == "200" ]] && ok "两次坏请求之后 /metrics 仍返回 200（坏请求没把面板弄死）" \
  || fail "坏请求之后 /metrics 回了 $CODE"
grep -q '^# TYPE' "$OUT/good-after-bad.txt" || fail "这次 200 的正文里没有 # TYPE 行"
stop_gateway "$GW_BAD" || fail "阶段 3：网关没退出"
# 进程侧的下界：失败账要跟上，而且是在响应真的发完之后再进（不是"决定了要回 4xx"就算）。
FAILED_SCRAPE=$(get_stat metrics_scrapes_failed "$OUT/gateway-bad.log")
OK_SCRAPE=$(get_stat metrics_scrapes_ok "$OUT/gateway-bad.log")
[[ "$FAILED_SCRAPE" == "2" ]] && ok "metrics_scrapes_failed=2（404 与 405 各一笔，进程侧对上）" \
  || fail "metrics_scrapes_failed=$FAILED_SCRAPE，应为 2"
[[ "$OK_SCRAPE" == "1" ]] && ok "metrics_scrapes_ok=1（那笔 200 是坏请求之后仍能通的下界）" \
  || fail "metrics_scrapes_ok=$OK_SCRAPE，应为 1"
FD_B_S=$(get_stat fd_start "$OUT/gateway-bad.log")
FD_B_E=$(get_stat fd_end "$OUT/gateway-bad.log")
[[ "$FD_B_S" == "$FD_B_E" ]] && ok "阶段 3 fd $FD_B_S→$FD_B_E 回到基线（三条 curl 都没漏 fd）" \
  || fail "阶段 3 fd $FD_B_S→$FD_B_E 不等——坏请求那条路径上漏了 fd"

# ---------------------------------------------------------------- 阶段 4：参数校验
echo
echo "--- 阶段 4：参数校验"
for bad in 65536 abc -1; do
  "$GATEWAY" --serve --listen "127.0.0.1:$GW_PORT" --upstream "127.0.0.1:$UP_PORT" \
    --metrics-port "$bad" > "$OUT/reject-$bad.log" 2>&1
  rc=$?
  if [[ $rc -eq 2 ]] && grep -q -- '--metrics-port' "$OUT/reject-$bad.log"; then
    ok "--metrics-port $bad 被拒（退出码 2 且点名）"
  else
    fail "--metrics-port $bad 的拒绝形态不对：rc=$rc"
  fi
done

# ---------------------------------------------------------------- 阶段 5：端口 0
echo
echo "--- 阶段 5：--metrics-port 0（内核选端口）"
port_busy "$GW_PORT" && die "阶段 5：端口 $GW_PORT 没空出来"
"$GATEWAY" --serve --listen "127.0.0.1:$GW_PORT" --upstream "127.0.0.1:$UP_PORT" \
  --metrics-port 0 > "$OUT/gateway-zero.log" 2>&1 &
GW3=$!; echo "$GW3" >> "$PIDFILE"
wait_for_line "$OUT/gateway-zero.log" '\[serve\] metrics=on' 10 || fail "端口 0 那一臂没打横幅"
ZERO_PORT=$(sed -n 's/.*\[serve\] metrics=on addr=[0-9.]*:\([0-9]*\).*/\1/p' "$OUT/gateway-zero.log" | head -1)
if [[ -n "$ZERO_PORT" && "$ZERO_PORT" != "0" ]]; then
  CODE=$(curl -s -o "$OUT/zero-scrape.txt" -w '%{http_code}' "http://127.0.0.1:$ZERO_PORT/metrics" 2>/dev/null || echo "000")
  [[ "$CODE" == "200" ]] && ok "--metrics-port 0 拿到内核端口 $ZERO_PORT 且能抓（测试靠它避免端口撞车）" \
    || fail "端口 0 那一臂拿到了 $ZERO_PORT 但抓取回了 $CODE"
else
  fail "横幅里没读到实际端口"
fi
stop_gateway "$GW3" || fail "阶段 5：网关没退出"

kill "$(cat "$PIDFILE")" 2>/dev/null
wait 2>/dev/null

echo
echo "统计：跳过 $SKIPPED 项"
if [[ $FAILED -eq 0 ]]; then
  echo "smoke-s5-01: 全部通过（日志在 $OUT/）"
  echo "DONE"
  exit 0
fi
echo "smoke-s5-01: 有失败项（见上面 ✗），日志在 $OUT/"
exit 1
