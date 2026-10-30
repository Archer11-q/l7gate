#!/usr/bin/env bash
# S3-02 冒烟：拿仓库里那份 **`deploy/gateway.json`** 起真进程，用**真 curl** 打，证明
# 契约第 7 节对"配置文件字段变化"的完成判据——**示例配置能直接跑通**——是真的。
#
# 为什么 ctest 里那条 `TheShippedExampleConfigAssembles` 不够：它只证明这份文件**装配得过**
# （解析 + 校验 + 建环），不证明按它跑起来的网关真的会把请求派到文件里写的那些后端。
# "能解析"和"能跑通"是两个命题，中间差的正是路由与均衡这两环在真 socket 上的行为。
#
# 五个阶段：
#   一 策略在真进程里生效：/api 走轮询（两次落到两个不同后端，体长 64 → 128）、
#     /v2/orders 走一致哈希（两次**同一个**后端）、catch-all 兜住其余路径；
#   二 无匹配回 404 且**一条上游都不开**（用一份没有 catch-all 的临时配置，另起一个网关）；
#   三 配置面的进程侧拒绝全部退出码 2 且**话里点名**：`--config` 与 `--upstream` 互斥、
#     S3-01 那两个临时开关已删（不留兼容层）、未知字段、文件读不到；
#   四 排水收尾：SIGTERM 之后 live_connections=0、constructed==destructed、fd 回基线；
#   五 **每个后端各收多少条由被连的那一侧自报**（上游退出时打的"已服务请求数"），
#     不拿网关的总数除以后端数算——沿用 S3-01/B-007 那条可见性立场。
#
# 用法：PRESET=debug scripts/smoke-s3-02.sh
# 输出：build/smoke-s3-02/ 里的日志；退出码 0 = 全部通过。
set -uo pipefail
cd "$(dirname "$(readlink -f "$0")")/.." || exit 1

# 往"已经被关闭的对端 socket"写一次就会吃到 SIGPIPE，而非交互 bash 的默认处置是**直接打死脚本**，
# 而且日志里一行 ✗ 都不会留（S2-05 实测过：同一份脚本，有这行连跑 6 次全绿、没它 3 次里死 2 次）。
trap '' PIPE

PRESET="${PRESET:-debug}"
BIN="build/$PRESET"
GATEWAY="$BIN/l7gate"
UPSTREAM_BIN="$BIN/tests/l7gate_test_upstream"
EXAMPLE_CONFIG="deploy/gateway.json"
OUT="build/smoke-s3-02"
mkdir -p "$OUT"

G1_PORT="${GATEWAY_PORT:-18080}"   # 阶段一的主网关
G2_PORT="${GATEWAY2_PORT:-18091}"  # 阶段二的"没有 catch-all"网关
U1_PORT=18081
U2_PORT=18082
U3_PORT=18083

FAILED=0
fail() { echo "  ✗ $*"; FAILED=1; }
ok()   { echo "  ✓ $*"; }
die()  { echo "  fatal: $*" >&2; echo "SMOKE-INCOMPLETE" >> "$OUT/marker" 2>/dev/null; exit 1; }

[[ -x "$GATEWAY" ]] || die "找不到 $GATEWAY，先 cmake --build --preset $PRESET"
[[ -x "$UPSTREAM_BIN" ]] || die "找不到 $UPSTREAM_BIN，先 cmake --build --preset $PRESET"
[[ -f "$EXAMPLE_CONFIG" ]] || die "找不到 $EXAMPLE_CONFIG：契约第 7 节的完成判据没有载体"
command -v curl >/dev/null || die "本机没有 curl：这个脚本要的第三方证据不成立"

# 端口占用检查用 bash 的 /dev/tcp，不用 ss（不同发行版字段位置不一样，抓不到就是漏判）。
# 端口被上一个没清干净的会话占着 → 后面每一格都红在"体长不对"上，那种红要花半天才认出病因。
port_free() { ! (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; }
for p in "$G1_PORT" "$G2_PORT" "$U1_PORT" "$U2_PORT" "$U3_PORT"; do
  port_free "$p" || die "端口 $p 已被占用：先清场（本脚本假定这四个端口都空）"
done

# 等某个后台进程在它的日志里打出某一行。等不到就区分两种病因：进程还活着 = 真挂住，
# 已退出 = 启动失败，日志尾部就是原因。
wait_for_line() { # pid logfile pattern [seconds]
  local pid=$1 log=$2 pat=$3 secs=${4:-15} i=0
  while (( i < secs * 20 )); do
    if grep -q -- "$pat" "$log" 2>/dev/null; then return 0; fi
    if ! kill -0 "$pid" 2>/dev/null; then
      echo "  进程 $pid 已退出，日志尾部：" >&2
      tail -5 "$log" >&2
      return 1
    fi
    ((i++)); sleep 0.05
  done
  return 1
}

get_stat() { # key logfile
  sed -n "s/^\[serve\] $1=\([0-9]*\).*/\1/p" "$2" | head -1
}

upstream_served() { # logfile → 已服务请求数
  sed -n 's/^\[upstream\] 已服务请求数=\([0-9]*\).*/\1/p' "$1" | head -1
}

# 发一条请求，打出「状态码 体长」。体长是"落到哪一台后端"的外部证据（64/128/192 各代表一台）。
# **两个值一起打、不做全局变量**：命令替换在子 shell 里跑，函数里写的全局出不到父 shell——
# 上一版就是这么把 `LAST_CODE` 丢掉的，那种"变量看着设了、读出来是空"的坑不响。
req() { # port path → "code bytes"
  local port=$1 path=$2 file="$OUT/body.$$"
  local code
  code=$(curl -s --max-time 5 -o "$file" -w '%{http_code}' "http://127.0.0.1:$port$path" 2>/dev/null)
  local n
  n=$(wc -c < "$file" 2>/dev/null)
  rm -f "$file"
  echo "${code:-000} ${n:-0}"
}

PIDS=()
cleanup() {
  for pid in "${PIDS[@]}"; do kill -TERM "$pid" 2>/dev/null; done
  wait 2>/dev/null
}
trap cleanup EXIT

echo "== 阶段 0：起三台上游与主网关（$EXAMPLE_CONFIG）=="
# 体长 64 / 128 / 192 分别代表示例配置里的 18081 / 18082 / 18083。
"$UPSTREAM_BIN" --listen "127.0.0.1:$U1_PORT" --body-bytes 64 > "$OUT/up1.log" 2>&1 & U1=$!
"$UPSTREAM_BIN" --listen "127.0.0.1:$U2_PORT" --body-bytes 128 > "$OUT/up2.log" 2>&1 & U2=$!
"$UPSTREAM_BIN" --listen "127.0.0.1:$U3_PORT" --body-bytes 192 > "$OUT/up3.log" 2>&1 & U3=$!
PIDS+=("$U1" "$U2" "$U3")
for pair in "$U1:up1" "$U2:up2" "$U3:up3"; do
  pid=${pair%%:*}
  log="$OUT/${pair##*:}.log"
  wait_for_line "$pid" "$log" "监听" 15 || die "上游 $pid（$log）没打出监听行"
done

"$GATEWAY" --serve --listen "127.0.0.1:$G1_PORT" --config "$EXAMPLE_CONFIG" > "$OUT/gateway1.log" 2>&1 & G1=$!
PIDS+=("$G1")
wait_for_line "$G1" "$OUT/gateway1.log" "监听" 15 || die "网关没起来"
grep -q 'policy=on' "$OUT/gateway1.log" || fail "横幅里没有 policy=on：装了配置却没报告策略"
banner=$(sed -n 's/.*policy=on config=\(.*\) upstream_groups=\([0-9]*\) route_rules=\([0-9]*\).*/\2 \3/p' "$OUT/gateway1.log" | head -1)
groups=$(echo "$banner" | cut -d' ' -f1); rules=$(echo "$banner" | cut -d' ' -f2)
if [[ "$groups" == "2" && "$rules" == "3" ]]; then
  ok "横幅报告 upstream_groups=2 route_rules=3（与示例配置一致）"
else
  fail "横幅是 groups=${groups:-?} rules=${rules:-?}，示例配置应当是 2 组 3 条路由"
fi

echo "== 阶段 1：三条策略在真进程里各走一次 =="
# --workers 缺省是 1，所以游标只有一份、顺序由 curl 的串行往返决定 → 期望序列是确定的。
read -r code1 n1 <<< "$(req "$G1_PORT" /api/users)"
read -r code2 n2 <<< "$(req "$G1_PORT" /api/users)"
if [[ "$code1" == "200" && "$code2" == "200" && "$n1" == "64" && "$n2" == "128" ]]; then
  ok "轮询：/api 两次落到两个不同后端（体长 64 → 128）"
else
  fail "轮询：/api 两次拿到的是 code=${code1}/${code2} 体长 ${n1}/${n2}，期望 200 与 64→128"
fi

read -r c_h1 h1 <<< "$(req "$G1_PORT" /v2/orders)"
read -r c_h2 h2 <<< "$(req "$G1_PORT" /v2/orders)"
if [[ "$c_h1" == "200" && "$c_h2" == "200" && "$h1" == "$h2" && ( "$h1" == "128" || "$h1" == "192" ) ]]; then
  ok "一致哈希：同一个 key 两次都落在同一个后端（体长 $h1）"
else
  fail "一致哈希：/v2/orders 两次是 ${h1}/${h2}（code ${c_h1}/${c_h2}），应当相等且属于 128|192"
fi

read -r code5 n5 <<< "$(req "$G1_PORT" /nothing/else)"
if [[ "$code5" == "200" && "$n5" == "64" ]]; then
  ok "catch-all：空前缀兜住了 /nothing/else（轮询游标回到 18081，体长 64）"
else
  fail "catch-all：/nothing/else 是 code=$code5 体长=$n5，期望 200 与 64"
fi

echo "== 阶段 2：没有 catch-all 的那份配置，无匹配必须 404 且不回源 =="
# 临时配置：只有一条 /api 前缀，**故意不带兜底**。它不是示例配置的一部分，
# 这一格要的是"无匹配 = 请求侧的确定性结果"这句话在真进程里成立。
cat > "$OUT/no-catchall.json" <<'JSON'
{
  "upstreams": [
    { "name": "api", "balance": "round_robin",
      "targets": [ { "address": "127.0.0.1", "port": 18081 } ] }
  ],
  "routes": [
    { "match": "prefix", "pattern": "/api", "upstream": "api" }
  ]
}
JSON
"$GATEWAY" --serve --listen "127.0.0.1:$G2_PORT" --config "$OUT/no-catchall.json" > "$OUT/gateway2.log" 2>&1 & G2=$!
PIDS+=("$G2")
wait_for_line "$G2" "$OUT/gateway2.log" "监听" 15 || die "第二个网关没起来"

code404=$(curl -s --max-time 5 -o /dev/null -w '%{http_code}' "http://127.0.0.1:$G2_PORT/zzz" 2>/dev/null)
if [[ "$code404" == "404" ]]; then
  ok "无匹配 → 404"
else
  fail "无匹配给的是 ${code404:-无响应}，期望 404"
fi
kill -TERM "$G2" 2>/dev/null; wait "$G2" 2>/dev/null
parsed2=$(get_stat requests_parsed "$OUT/gateway2.log")
replies2=$(get_stat gateway_replies_sent "$OUT/gateway2.log")
opened2=$(get_stat upstream_opened "$OUT/gateway2.log")
if [[ "${parsed2:-0}" == "1" && "${replies2:-0}" == "1" && "${opened2:-1}" == "0" ]]; then
  ok "拦下来就真的没回源：requests_parsed=1、gateway_replies_sent=1、upstream_opened=0"
else
  fail "404 那一格的账是 parsed=${parsed2:-?} replies=${replies2:-?} opened=${opened2:-?}，期望 1/1/0"
fi

echo "== 阶段 3：进程侧的拒绝面（全部退出码 2，话里要点名）=="
check_reject() { # 名称 期望码 期望话里的词 -- 参数...
  local name=$1 want_code=$2 want_word=$3; shift 3
  local log="$OUT/reject-${name// /_}.log"
  "$GATEWAY" "$@" > "$log" 2>&1
  local rc=$?
  if [[ "$rc" == "$want_code" ]] && grep -q -- "$want_word" "$log"; then
    ok "$name：退出码 $rc，话里点了「$want_word」"
  else
    fail "$name：退出码 ${rc}（期望 $want_code），话里没找到「$want_word」。日志尾部：$(tail -2 "$log" | tr '\n' ' ')"
  fi
}
check_reject "config与upstream同时给" 2 "只能给一个" --serve --listen "127.0.0.1:$G2_PORT" \
  --config "$EXAMPLE_CONFIG" --upstream "127.0.0.1:$U1_PORT"
check_reject "旧开关 --route 已删" 2 "无法识别的选项" --serve --listen "127.0.0.1:$G2_PORT" \
  --upstream "127.0.0.1:$U1_PORT" --route "/a=0"
check_reject "配置文件读不到" 2 "不存在.json" --serve --listen "127.0.0.1:$G2_PORT" \
  --config "$OUT/这一份配置文件不存在.json"

cat > "$OUT/bad-field.json" <<'JSON'
{ "upstreams": [ { "name": "api", "balance": "round_robin", "weight": 3,
                    "targets": [ { "address": "127.0.0.1", "port": 18081 } ] } ] }
JSON
check_reject "未知字段 weight" 2 "weight" --serve --listen "127.0.0.1:$G2_PORT" \
  --config "$OUT/bad-field.json"

cat > "$OUT/bad-balance.json" <<'JSON'
{ "upstreams": [ { "name": "api", "balance": "round-robin",
                    "targets": [ { "address": "127.0.0.1", "port": 18081 } ] } ] }
JSON
check_reject "策略名拼错 round-robin" 2 "balance" --serve --listen "127.0.0.1:$G2_PORT" \
  --config "$OUT/bad-balance.json"

echo "== 阶段 4：主网关排水收尾 =="
# `fd_start` 与 `fd_end` 都是**退出时**才打的（report_stats 在最后一段），所以在发信号之前读
# 只能读到空——上一版就是这么假红成"fd 泄漏"的。先停、再读。
kill -TERM "$G1" 2>/dev/null
wait "$G1" 2>/dev/null
fd_line=$(get_stat fd_start "$OUT/gateway1.log")
live=$(get_stat live_connections "$OUT/gateway1.log")
constructed=$(get_stat connections_constructed "$OUT/gateway1.log")
destructed=$(get_stat connections_destructed "$OUT/gateway1.log")
fd_end=$(get_stat fd_end "$OUT/gateway1.log")
parsed=$(get_stat requests_parsed "$OUT/gateway1.log")
completed=$(get_stat responses_completed "$OUT/gateway1.log")
if [[ "${live:-1}" == "0" ]]; then ok "live_connections = 0"; else fail "live_connections = ${live:-?}，期望 0"; fi
if [[ -n "$constructed" && "$constructed" == "$destructed" ]]; then
  ok "constructed == destructed == $constructed"
else
  fail "constructed=${constructed:-?} / destructed=${destructed:-?} 不相等：有连接没走完六步关闭"
fi
if [[ -n "$fd_line" && "$fd_line" == "$fd_end" ]]; then
  ok "fd 回到基线（$fd_line → $fd_end）"
else
  fail "fd_start=${fd_line:-?} 与 fd_end=${fd_end:-?} 不等：有 fd 泄漏"
fi
if [[ "${parsed:-0}" == "5" && "${completed:-0}" == "5" ]]; then
  ok "requests_parsed == responses_completed == 5"
else
  fail "requests_parsed=${parsed:-?} responses_completed=${completed:-?}，期望 5/5"
fi

echo "== 阶段 5：每个后端收到几条，由被连的那一侧自报 =="
for pid in "$U1" "$U2" "$U3"; do kill -TERM "$pid" 2>/dev/null; done
wait "$U1" 2>/dev/null; wait "$U2" 2>/dev/null; wait "$U3" 2>/dev/null
s1=$(upstream_served "$OUT/up1.log")
s2=$(upstream_served "$OUT/up2.log")
s3=$(upstream_served "$OUT/up3.log")
echo "  上游自报：18081=${s1:-?} 18082=${s2:-?} 18083=${s3:-?}"
total=$(( ${s1:-0} + ${s2:-0} + ${s3:-0} ))
# 阶段一发了 5 条请求（/api ×2、/v2/orders ×2、catch-all ×1），阶段二那条 404 不回源，
# 而且它连的是**另一个**网关、用的是只指向 18081 的临时配置——那条路径上一条上游都没开。
if [[ "$total" == "5" && "${s1:-0}" == "2" ]]; then
  ok "总数 5、18081 恰 2 条（轮询第一次 + catch-all 那一格游标回到它）"
else
  fail "上游计数是 ${s1:-?}/${s2:-?}/${s3:-?}（总 $total），期望总数 5 且 18081 == 2"
fi
# 一致哈希那两条必须落在**同一台**上：所以 18082 与 18083 的读数只能是 (1,2) 或 (3,0)。
if [[ ( "${s2:-0}" == "1" && "${s3:-0}" == "2" ) || ( "${s2:-0}" == "3" && "${s3:-0}" == "0" ) ]]; then
  ok "哈希那两条落在同一台后端上（18082=${s2}、18083=${s3}）"
else
  fail "哈希那两条散开了：18082=${s2:-?}、18083=${s3:-?}，期望 1/2 或 3/0"
fi

trap - EXIT
echo "DONE"
if (( FAILED )); then
  echo "✗ 有失败格，看 $OUT/"
  exit 1
fi
echo "✓ S3-02 冒烟五阶段全过"
exit 0
