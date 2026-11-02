#!/usr/bin/env bash
# S4-07 —— S4 故障注入矩阵第二轮（ADR-13：全部应用层注入；tc netem 仍是未验证项，不在本轮）。
#
# 这一节在问什么：S3-05 那五类（§12）量的是"会选人、会摘人"的网关；S4 之后它还会**拒绝**
# （限流 429）、会**限期**（504）、会**问 Redis**、会把恢复**收进半开这道门**、还会**换一台重试**。
# 所以本轮不是在五类之外另加五个功能测试，而是把新机制**叠在旧故障上**，量两件旧工具量不到的事：
#   一 叠加后的**账对不对**（谁的计数该涨、谁的一格都不许动）；
#   二 恢复出口（到期→半开 vs 探活判活→？）在**真进程**上到底走哪条 —— ADR-12 追加二第七节
#     与 ARCHITECTURE 1.31 边界① 写的是"探活判活→立即全量，本轮没动"，而 tests/health_test.cc
#     的 `ProbeSuccessWakesIntoHalfOpenNotFullTraffic` 断的是"叫醒到半开"。**两份说法只能有一份是真的**，
#     类 9 那三条臂就是去读它是哪一条（S4-07 裁定的取数处，不靠读代码结案）。
#
# 臂：
#   类 0  基线五类复跑 = 另一条命令（FAULT_OUT_DIR=build/fault/s4-07-base scripts/fault/s3-05-matrix.sh）
#   类 6  限流触发 × connect refused（叠加重试）
#   类 7  超时撞线（响应头限期 800ms） × 连上不回字节 —— 把 §12 欠的"探活侧摘除 vs 客户端侧读数"分开报
#   类 8  Redis 降级 × 后端 5xx + 熔断窗口（两臂只差 Redis 在不在）
#   类 9  熔断半开 × "探活活、业务死"（9a 探活+circuit / 9b 纯被动+circuit / 9c 探活、**不装 circuit**）
#   类 10 重试放大 × 熔断错误率窗口（失败样本翻倍落在计数与摘除节奏上）
#
# 三条纪律的落点（TASKS S4-07 行）：上界必配"确实发生过"的下界；每格"某某没发生"的断言都必须
# 指出另一臂读数非零（口径⑮：仪器看不见那个事件时，读数永远支持你的断言）；判定一律从落盘 trace
# 离线算，不在循环里"看到条件成立就立刻读"（口径⑤）。
#
# 用法：PRESET=release scripts/fault/s4-07-matrix.sh
# 输出：build/fault/s4-07/（每臂网关日志、退出账、上游自报日志、逐请求 trace）；退出码 0 = 全部断言过。
set -uo pipefail
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1

trap '' PIPE
PRESET="${PRESET:-release}"
BIN="build/$PRESET"
GATEWAY="$BIN/l7gate"
UPSTREAM="$BIN/tests/l7gate_test_upstream"
OUT="build/fault/s4-07"
rm -rf "$OUT"; mkdir -p "$OUT"
PIDFILE="$OUT/pids"; : > "$PIDFILE"
BODYTMP="$OUT/body.tmp"

FAILED=0
fail() { echo "  ✗ $*"; FAILED=1; }
ok()   { echo "  ✓ $*"; }
# 中间档：**机制预期与实测不符**的格走这里——读数照记、不算失败也不算通过。
# 理由是 §12 先例（类 3/4a 那两格当年也是"读数与预期不同 + 归因"），而这一档不能升格成
# 硬断言：那等于用文档里那句过期的话去判二进制。反过来，守恒类不变式一律走 fail()。
note() { echo "  ◦ $*"; }
die()  { echo "  fatal: $*" >&2; exit 1; }

[[ -x "$GATEWAY" ]]   || die "找不到 $GATEWAY，先 cmake --build --preset $PRESET"
[[ -x "$UPSTREAM" ]]  || die "找不到 $UPSTREAM，先 cmake --build --preset $PRESET"
command -v curl  >/dev/null || die "本机没有 curl"

# 本机 `date +%s%3N` 不认 %3N（§15.3 第 3 条），毫秒一律这一条
now_ms() { date +%s%N | cut -c1-13; }

port_busy() { (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; }
# 要活的端口必须空（旧进程顶着的假绿，§14.3 第 1 条）；要死的端口也必须空——
# "没人监听"这件事就是那一臂的注入本身。
PORTS_ALL=(18700 18701 18703 18710 18711 18712 18713 18720 18721 18722 \
           18730 18731 18732 18740 18741 18742)
for p in "${PORTS_ALL[@]}"; do
  port_busy "$p" && die "端口 $p 已被占用：先清场（带着脏状态往下测，后面每一臂测的都是同一台进程）"
done

wait_for_line() { # pid logfile pattern [seconds]
  local pid=$1 log=$2 pat=$3 secs=${4:-20} i=0
  while (( i < secs * 20 )); do
    if grep -q -- "$pat" "$log" 2>/dev/null; then return 0; fi
    if ! kill -0 "$pid" 2>/dev/null; then
      echo "  进程 $pid 已退出，日志尾部：" >&2; tail -5 "$log" >&2; return 1
    fi
    ((i++)); sleep 0.05
  done
  return 1
}
# 不假设键在行首（§14.3 第 2 条：一行多键 + 行首锚 = 其余几格静默读空，而空串在判定里长得像 0）
get_stat() { grep -o "[[:space:]]$1=[0-9-]*" "$2" 2>/dev/null | head -1 | sed 's/.*=//'; }
# 逐目标那组键的完整名字里有地址与端口，拼错就是空串——空串参与判定是本项目最贵的那种错
tstat() { # gwlog target_index port key
  get_stat "health_t$2_127.0.0.1_${3}_$4" "$1"; }
upstream_served() { # log → 这台自报收到几条（两条分支相加，§15.3 第 2 条的理由）
  local a b
  a=$(sed -n 's/^\[upstream\] 已服务请求数=\([0-9]*\).*/\1/p' "$1" | head -1)
  b=$(sed -n 's/^\[upstream\] 固定状态码响应数=\([0-9]*\).*/\1/p' "$1" | head -1)
  echo $(( ${a:-0} + ${b:-0} )); }
upstream_path_status() { # log → 固定状态码那条分支回了几条（= 业务侧被这台接走几条）
  sed -n 's/^\[upstream\] 固定状态码响应数=\([0-9]*\).*/\1/p' "$1" | head -1; }

note_pid() { echo "$1" >> "$PIDFILE"; }
start_upstream() { # 标签 端口 额外参数... → pid
  # kill/TERM 之后立刻重绑同一端口可能撞 FIN_WAIT 系 EADDRINUSE（§12.2：被杀方的 socket 由内核
  # 代收，要等对端把 EOF 处理完）。类 3 那一臂要在同一个端口上把"不回字节"换成"正常"，所以这里
  # 退避重试，不当成故障——s3-05-matrix.sh 里 start_u2 那条经验原样搬过来。
  local tag=$1 port=$2; shift 2
  local try=0 pid
  while (( try < 50 )); do
    "$UPSTREAM" --listen "127.0.0.1:$port" "$@" > "$OUT/upstream-$tag.log" 2>&1 &
    pid=$!
    note_pid "$pid"
    if wait_for_line "$pid" "$OUT/upstream-$tag.log" "\[upstream\] 监听" 3; then
      echo "$pid"
      return 0
    fi
    kill -9 "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
    echo "try $try: 上游 $tag 绑 $port 失败" >> "$OUT/bind-fail.log"
    ss -tlnp 2>/dev/null | grep "$port" >> "$OUT/bind-fail.log"
    ((try++))
    sleep 0.1
  done
  die "上游 $tag 在端口 $port 上重试 50 次都起不来（见 $OUT/bind-fail.log）"
}
start_gateway() { # 标签 配置文件 额外参数... → pid
  local tag=$1 cfg=$2; shift 2
  port_busy "$GW_PORT" && die "端口 $GW_PORT 上还压着进程（上一臂没退干净）——不带着它往下跑"
  "$GATEWAY" --serve --listen "127.0.0.1:$GW_PORT" --config "$cfg" "$@" \
    > "$OUT/gateway-$tag.log" 2>&1 &
  local pid=$!
  note_pid "$pid"
  wait_for_line "$pid" "$OUT/gateway-$tag.log" "\[serve\] 监听" 20 \
    || die "网关 $tag 没起来：$(tail -3 "$OUT/gateway-$tag.log" | tr '\n' '|')"
  echo "$pid"
}
# `wait` 对子 shell 起的进程当场返回"not a child"（§14.3 第 1 条：脚本于是抢在退出之前读退出账）
stop_pid() {
  local pid=$1 i=0
  kill -TERM "$pid" 2>/dev/null
  while (( i < 600 )); do
    kill -0 "$pid" 2>/dev/null || return 0
    ((i++)); sleep 0.05
  done
  fail "进程 $pid 30 秒还没退出，强杀"
  kill -9 "$pid" 2>/dev/null
  return 1
}
req() { # path → "code bytes"（bytes 用 curl 自己数的 size_download）
  local path=$1 code n
  code=$(curl -s --max-time 6 -o "$BODYTMP" -w '%{http_code} %{size_download}' \
         "http://127.0.0.1:$GW_PORT$path" 2>/dev/null)
  read -r code n <<< "$code"
  echo "${code:-000} ${n:-0}"
}
count_code() { echo "$1" | tr ' ' '\n' | grep -c "^$2$"; }
# 打 n 条并把每条落进 trace（idx code bytes 相对毫秒）；interval 给 0 就是"能多快多快"
blast() { # trace文件 n path interval_ms → "code code ..."
  local tf=$1 n=$2 path=$3 iv=$4 t0 out="" i c b
  t0=$(now_ms)
  : > "$tf"
  for ((i = 1; i <= n; i++)); do
    read -r c b <<< "$(req "$path")"
    echo "$i $c $b $(( $(now_ms) - t0 ))" >> "$tf"
    out="$out $c"
    (( iv > 0 )) && sleep "$(awk -v ms="$iv" 'BEGIN{printf "%.3f", ms/1000}')"
  done
  echo "${out# }"
}
cleanup() {
  while read -r pid; do [[ -n "$pid" ]] && kill -9 "$pid" 2>/dev/null; done < "$PIDFILE"
  wait 2>/dev/null
  [[ "${CONTAINER_OURS:-0}" == "1" ]] && docker rm -f "$REDIS_CONTAINER" >/dev/null 2>&1
}
trap cleanup EXIT

# ───────────────────────── 类 6：限流触发 × connect refused（叠加重试） ─────────────────────────
GW_PORT=18700
A6=18701; D6=18703          # D6 刻意不绑：connect 必然被拒
cat > "$OUT/c6.json" <<JSON
{
  "upstreams": [ { "name": "api", "balance": "round_robin", "targets": [
      { "address": "127.0.0.1", "port": $D6 },
      { "address": "127.0.0.1", "port": $A6 } ] } ],
  "routes": [ { "match": "prefix", "pattern": "", "upstream": "api" } ],
  "rate_limit": { "rate": 1, "burst": 5, "key": "path" }
}
JSON
echo "== 类 6：限流触发 × connect refused（一死一活、rate=1 burst=5、12 条同 path）=="
U6=$(start_upstream a6 "$A6" --body-bytes 64)
G6=$(start_gateway c6 "$OUT/c6.json")
C6=$(blast "$OUT/trace-c6.log" 12 /six 0)
stop_pid "$G6"
ALLOWED6=$(get_stat rate_limit_allowed  "$OUT/gateway-c6.log")
REJECTED6=$(get_stat rate_limit_rejected "$OUT/gateway-c6.log")
RETRY6=$(get_stat upstream_retries        "$OUT/gateway-c6.log")
CF6=$(get_stat upstream_connect_failed    "$OUT/gateway-c6.log")
OPEN6=$(get_stat upstream_opened          "$OUT/gateway-c6.log")
REPLY6=$(get_stat gateway_replies_sent    "$OUT/gateway-c6.log")
REPLYCF6=$(get_stat gateway_replies_connect_failed "$OUT/gateway-c6.log")
RP6=$(get_stat requests_parsed            "$OUT/gateway-c6.log")
stop_pid "$U6"
RECV6=$(upstream_served "$OUT/upstream-a6.log")
N200_6=$(count_code "$C6" 200); N429_6=$(count_code "$C6" 429)
echo "  客户端：200×$N200_6 429×$N429_6（状态行 $C6）"
echo "  退出账：allowed=$ALLOWED6 rejected=$REJECTED6 retries=$RETRY6 connect_failed=$CF6 opened=$OPEN6 replies_sent=$REPLY6 replies_connect_failed=$REPLYCF6 requests_parsed=$RP6"
echo "  活的那台自报：$RECV6 条"
# 上界：令牌守恒 + 被限流的请求一个上游字节都不发
if [[ $(( ${ALLOWED6:-0} + ${REJECTED6:-0} )) == "12" ]]; then
  ok "上界：allowed+rejected=$ALLOWED6+$REJECTED6 == 客户端 12 条（一条请求最多消耗一个令牌）"
else
  fail "allowed+rejected=${ALLOWED6:-空}+${REJECTED6:-空} != 12"
fi
if [[ "${ALLOWED6:-0}" == "$N200_6" && "${REJECTED6:-0}" == "$N429_6" ]]; then
  ok "上界：客户端拿到的 200 条数 == allowed、429 条数 == rejected（表态与计数同一口径）"
else
  fail "客户端 200×$N200_6/429×$N429_6 与退出账 allowed=$ALLOWED6/rejected=$REJECTED6 不吻合"
fi
if [[ "${RECV6:-0}" == "${ALLOWED6:-1}" ]]; then
  ok "上界：**后端实收 $RECV6 == allowed $ALLOWED6** —— 重试不多耗令牌、也不复制请求"
else
  fail "后端实收 ${RECV6:-?} != allowed ${ALLOWED6:-?}（要么重试多花了令牌，要么重放复制了）"
fi
if [[ "${OPEN6:-9}" -le $(( ${ALLOWED6:-0} + ${RETRY6:-0} )) ]]; then
  ok "上界：upstream_opened=$OPEN6 ≤ allowed+retries=$(( ${ALLOWED6:-0} + ${RETRY6:-0} ))（被 429 的那几条没造连接）"
else
  fail "upstream_opened=$OPEN6 超过 allowed+retries=$(( ${ALLOWED6:-0} + ${RETRY6:-0} ))"
fi
# 下界：两件事都真的发生了
if [[ "${RETRY6:-0}" -ge 1 && "${REJECTED6:-0}" -ge 1 ]]; then
  ok "下界：retries=$RETRY6 ≥ 1 且 rejected=$REJECTED6 ≥ 1（限流与重试在同一臂里都真发生过）"
else
  fail "retries=${RETRY6:-空} / rejected=${REJECTED6:-空}：这一臂没东西可测，上界那几条全是恒真"
fi
if [[ "${CF6:-0}" == "${RETRY6:-1}" ]]; then
  ok "归因对上：upstream_connect_failed=$CF6 == retries=$RETRY6（每次被拒都换了一台且只换一次）"
else
  fail "connect_failed=${CF6:-?} != retries=${RETRY6:-?}"
fi
if [[ "${REPLYCF6:-1}" == "0" && "${REPLY6:-?}" == "${REJECTED6:-x}" ]]; then
  ok "重试兜住了回源失败：gateway_replies_connect_failed=0，而 replies_sent=$REPLY6 == rejected（**429 本身就是自产响应**，这一格我第一版读错了口径：见 §18 工具侧第 1 条）"
else
  fail "replies_connect_failed=${REPLYCF6:-空} / replies_sent=${REPLY6:-空} / rejected=${REJECTED6:-空}，期望 0 与相等"
fi

# ───────────────────────── 类 7：超时撞线 × 连上不回字节 ─────────────────────────
GW_PORT=18710
U71=18711; U72=18712; U73=18713
cat > "$OUT/c7.json" <<JSON
{
  "upstreams": [
    { "name": "api", "balance": "round_robin",
      "targets": [ { "address": "127.0.0.1", "port": $U71 },
                   { "address": "127.0.0.1", "port": $U72, "health_path": "/probe" },
                   { "address": "127.0.0.1", "port": $U73 } ] }
  ],
  "routes": [ { "match": "prefix", "pattern": "", "upstream": "api" } ],
  "health": { "max_fails": 2, "fail_timeout_ms": 60000,
              "probe_interval_ms": 100, "probe_timeout_ms": 300,
              "max_ejection_percent": 50 }
}
JSON
echo "== 类 7：超时撞线 × 连上不回字节（三成员同 §12 配置，响应头限期压到 800ms）=="
U71P=$(start_upstream c7-1 "$U71" --body-bytes 64)
U72P=$(start_upstream c7-2 "$U72" --body-bytes 128)
U73P=$(start_upstream c7-3 "$U73" --body-bytes 192)
G7=$(start_gateway c7 "$OUT/c7.json" --upstream-head-timeout-ms 800)
# 先确认三台都在轮换（§12 的 all_healthy 前置）
pre_ok=1
for i in 1 2 3; do read -r c b <<< "$(req /settle)"; [[ "$c" == "200" ]] || pre_ok=0; done
(( pre_ok )) || die "类 7 前置不满足：网关没在正常转发"
EJECT_T0=$(now_ms)
stop_pid "$U72P"
U72P=$(start_upstream c7-2-stall "$U72" --body-bytes 128 --stall-reads-ms 15000)
# 客户端侧判定沿用 §12：连续 20 条不见 128
streak=0; eject_ms=-1
: > "$OUT/trace-c7.log"
for i in $(seq 1 400); do
  read -r c b <<< "$(req /probe-fault)"
  echo "$i $c $b $(( $(now_ms) - EJECT_T0 ))" >> "$OUT/trace-c7.log"
  if [[ "$b" == "128" ]]; then streak=0; else
    streak=$((streak + 1))
    if (( streak >= 20 )); then eject_ms=$(( $(now_ms) - EJECT_T0 )); break; fi
  fi
done
stop_pid "$G7"
HEAD7=$(get_stat upstream_head_timeout     "$OUT/gateway-c7.log")
CT7=$(get_stat  upstream_connect_timeout   "$OUT/gateway-c7.log")
BI7=$(get_stat  upstream_body_idle_timeout "$OUT/gateway-c7.log")
RETRY7=$(get_stat upstream_retries         "$OUT/gateway-c7.log")
S504=$(grep -c "^[0-9]* 504 " "$OUT/trace-c7.log")
EJ7=$(tstat "$OUT/gateway-c7.log" 1 "$U72" ejections)
EPOCH7=$(tstat "$OUT/gateway-c7.log" 1 "$U72" last_ejection_epoch_ms)
GWEJECT_MS=$(( ${EPOCH7:-0} - EJECT_T0 ))
echo "  客户端侧摘除读数 = ${eject_ms} ms（§16.5 同格是 5349 ms）"
echo "  网关自报摘除时刻 = 注入后 ${GWEJECT_MS} ms（last_ejection_epoch_ms − 脚本 T0，同一根墙钟轴）"
echo "  退出账：head_timeout=$HEAD7 connect_timeout=$CT7 body_idle=$BI7 retries=$RETRY7 ejections=$EJ7；客户端 504 条数=$S504"
# 分开报：S4-01 之后这两笔计时是两件事，§12 那条归因要的就是这个
if [[ $eject_ms -gt 0 && $eject_ms -lt 3000 ]]; then
  ok "客户端侧读数离开 5s 排空主导那一带：${eject_ms} ms < 3000（限期 800ms 顶掉了原来那截）"
else
  fail "客户端侧摘除读数 = ${eject_ms} ms，没离开 §16.5 的 5349 ms 那一带（限期没起作用？）"
fi
if [[ $GWEJECT_MS -ge 0 && $GWEJECT_MS -le 3000 ]]; then
  ok "探活/判据侧摘除时刻单独可取：注入后 ${GWEJECT_MS} ms（这一格 §12 当年只能推断，现在有自报值）"
else
  fail "last_ejection_epoch_ms 给的是 ${EPOCH7:-空}，相对 T0 = ${GWEJECT_MS} ms，不落在合理区间"
fi
if [[ "${HEAD7:-0}" -ge 1 && "$S504" -ge 1 ]]; then
  ok "下界：upstream_head_timeout=$HEAD7 ≥ 1 且客户端确实收到 $S504 条 **504**（不是 curl 自己 5s 放弃）"
else
  fail "head_timeout=${HEAD7:-空} / 客户端 504 条数=$S504 —— 撞线那一格没打到"
fi
if [[ "${CT7:-1}" == "0" && "${BI7:-1}" == "0" ]]; then
  ok "撞的是哪一格说清了：connect 超时与 body 空闲超时都是 0，只有响应头限期在响"
else
  fail "connect_timeout=${CT7:-?} / body_idle=${BI7:-?}，期望两个都 0"
fi
if [[ "${RETRY7:-1}" == "0" ]]; then
  ok "越界探针：这一臂 upstream_retries=0（超时不在重试定义域内，ADR-23 第一节第三行）"
else
  fail "upstream_retries=${RETRY7:-?} != 0 —— 实现越界去重试超时了（改实现，不改文档）"
fi
echo "  （retries 这一格「看得见非零」由类 6 的 $RETRY6 与类 10 背书，不是恒真）"
if [[ "${EJ7:-0}" -ge 1 ]]; then ok "超时计入健康判据：成员 2 ejections=$EJ7（ADR-21 规则 1）"; else fail "ejections=${EJ7:-空}"; fi
stop_pid "$U71P"; stop_pid "$U72P"; stop_pid "$U73P"

# ───────────────────────── 类 8：Redis 降级 × 后端 5xx + 熔断窗口 ─────────────────────────
GW_PORT=18720
GOOD8=18721; BAD8=18722
REDIS_CONTAINER="${REDIS_CONTAINER:-l7gate-redis}"
REDIS_CPUS="${REDIS_CPUS:-15}"
REDIS_PORT=16379
rcli() { redis-cli -h 127.0.0.1 -p "$REDIS_PORT" "$@" 2>/dev/null; }
command -v docker >/dev/null || die "本机 WSL 里没有 docker（WSL Integration 未开）—— 类 8 没法跑"
if command -v redis-cli >/dev/null; then HAVE_REDIS_CLI=1; else HAVE_REDIS_CLI=0; fi
echo "== 类 8 前置：契约 tag 的 redis:7.2.5 容器 =="
if docker inspect "$REDIS_CONTAINER" >/dev/null 2>&1; then
  docker start "$REDIS_CONTAINER" >/dev/null 2>&1 || die "启动已有容器失败"
  ok "复用已有容器 $REDIS_CONTAINER"
else
  docker image inspect redis:7.2.5 >/dev/null 2>&1 \
    || die "本机没有 redis:7.2.5（契约锁定的 tag，我不换 tag 也不拿 redis:8.0 顶包）"
  docker run -d --name "$REDIS_CONTAINER" --cpuset-cpus "$REDIS_CPUS" \
    -p "127.0.0.1:$REDIS_PORT:6379" redis:7.2.5 --save "" --appendonly no >/dev/null \
    || die "起 redis 容器失败"
  CONTAINER_OURS=1
  ok "自起容器（cpuset=$REDIS_CPUS，端口发布 127.0.0.1:$REDIS_PORT；容器不共享本发行版网络命名空间）"
fi
for _ in $(seq 1 60); do [[ "$(rcli PING)" == "PONG" ]] && break; sleep 0.2; done
if [[ "$HAVE_REDIS_CLI" == "1" ]]; then
  [[ "$(rcli PING)" == "PONG" ]] || die "容器起来了但 $REDIS_PORT 不回 PONG"
  ok "PING → PONG（Redis 在位这件事由对端回答，不由我猜）"
fi

cat > "$OUT/c8.json" <<JSON
{
  "upstreams": [ { "name": "api", "balance": "round_robin", "targets": [
      { "address": "127.0.0.1", "port": $GOOD8 },
      { "address": "127.0.0.1", "port": $BAD8 } ] } ],
  "routes": [ { "match": "prefix", "pattern": "", "upstream": "api" } ],
  "rate_limit": { "rate": 1, "burst": 8, "key": "path" },
  "health": { "max_fails": 99, "fail_timeout_ms": 60000,
              "probe_interval_ms": 0, "max_ejection_percent": 100 },
  "circuit": { "window_ms": 20000, "minimum_requests": 6, "error_rate_percent": 50,
               "half_open_requests": 2, "half_open_successes": 2, "half_open_timeout_ms": 3000 }
}
JSON
# 两臂只差 Redis 在不在：**8a 全程在位**（对照），**8b 网关在没有它的世界里起来**、跑完降级段
# 再把容器起回来（同一个进程内看降级态自己摘掉——S4-04 证的是状态机，这一臂证的是它叠在熔断上）。
run_c8() { # 标签 起网关前停容器(1/0)
  local tag=$1 stop_first=$2
  local gp bp gw started_here=0
  gp=$(start_upstream "c8-good-$tag" "$GOOD8" --body-bytes 64)
  bp=$(start_upstream "c8-bad-$tag"  "$BAD8"  --body-bytes 128 --path-status "/:503")
  if [[ "$stop_first" == "1" ]]; then
    docker stop "$REDIS_CONTAINER" >/dev/null || die "停容器失败"
    ok "[$tag] docker stop $REDIS_CONTAINER —— 这一臂的注入就是 Redis 不在"
  fi
  gw=$(start_gateway "c8-$tag" "$OUT/c8.json" --redis-db "127.0.0.1:$REDIS_PORT" --redis-settle-ms 200)
  T0=$(now_ms); echo "$T0" > "$OUT/c8-$tag-t0"
  # 段 A：涓流 16 条（900ms 一条，令牌补得回来 → 这一段只该有"零误拒"，同时把 5xx 喂进窗口）
  CODESA=$(blast "$OUT/trace-c8-$tag-A.log" 16 /eight 900)
  # 段 B：突发 12 条。臂 b 跑到这里 Redis 还挂着——这一段的 429 就是"降级期间拒绝能力不消失"
  CODESB=$(blast "$OUT/trace-c8-$tag-B.log" 12 /eight 0)
  if [[ "$stop_first" == "1" ]]; then
    docker start "$REDIS_CONTAINER" >/dev/null || die "起容器失败"
    started_here=1
    ok "[$tag] docker start $REDIS_CONTAINER —— 不重启网关，把通路叫回来"
    sleep 1
    # 段 C：Redis 回来之后再突发一次
    CODESC=$(blast "$OUT/trace-c8-$tag-C.log" 12 /eight 0)
  else
    CODESC=""
  fi
  stop_pid "$gw"
  # 夹具的汇总行是**退出时**打的：在它还活着的时候读这两个数会读回空串，而空串在判定里
  # 长得像 0（§14.3 同族，本轮我在类 10c 上就是这么撞上一条恒真断言的，见 §18 工具侧第 2 条）
  stop_pid "$gp"; stop_pid "$bp"
  local lg="$OUT/gateway-c8-$tag.log" rec503
  rec503=$(upstream_path_status "$OUT/upstream-c8-bad-$tag.log")
  echo "  [$tag] 段 A 16 条：503×$(count_code "$CODESA" 503) 200×$(count_code "$CODESA" 200)｜段 B 12 条：503×$(count_code "$CODESB" 503) 429×$(count_code "$CODESB" 429) 200×$(count_code "$CODESB" 200)｜段 C $([[ -n "$CODESC" ]] && echo "12 条：429×$(count_code "$CODESC" 429)" || echo "（此臂 Redis 全程在位，没有'回来'这一段）")"
  echo "  [$tag] 退出账 allowed=$(get_stat rate_limit_allowed "$lg") rejected=$(get_stat rate_limit_rejected "$lg") capped=$(get_stat rate_limit_capped_rejects "$lg") settles_ok=$(get_stat redis_settles_ok "$lg") settles_failed=$(get_stat redis_settles_failed "$lg") degraded=$(get_stat redis_degraded "$lg")"
  echo "  [$tag] 熔断与摘除：trips_rate=$(tstat "$lg" 1 "$BAD8" trips_rate) ejections=$(tstat "$lg" 1 "$BAD8" ejections) 坏台自报 503=$rec503 blocked_by_valve=$(get_stat health_ejections_blocked_by_valve "$lg")"
  local ep; ep=$(tstat "$lg" 1 "$BAD8" last_ejection_epoch_ms)
  echo "  [$tag] 网关自报摘除时刻 = 注入后 $(( ${ep:-0} - T0 )) ms（成员 1 = 那台 503；epoch 空格读作 0，差值会是个大负数，别当读数用）"
}
echo "== 类 8a：Redis 在位 × 后端 5xx + 熔断窗口（对照臂）=="
run_c8 a 0
echo "== 类 8b：Redis 挂着开局 × 后端 5xx + 熔断窗口，段中把容器起回来 =="
run_c8 b 1
LG_A="$OUT/gateway-c8-a.log"; LG_B="$OUT/gateway-c8-b.log"
EJ_A8=$(tstat "$LG_A" 1 "$BAD8" ejections); EJ_B8=$(tstat "$LG_B" 1 "$BAD8" ejections)
FA=$(get_stat redis_settles_failed "$LG_A"); FB=$(get_stat redis_settles_failed "$LG_B")
DEG_B=$(get_stat redis_degraded "$LG_B"); SOK_B=$(get_stat redis_settles_ok "$LG_B")
REJ_B=$(get_stat rate_limit_rejected "$LG_B")
N429_B=$(grep -c ' 429 ' "$OUT/trace-c8-b-B.log")
EPOCH_A8=$(tstat "$LG_A" 1 "$BAD8" last_ejection_epoch_ms)
EPOCH_B8=$(tstat "$LG_B" 1 "$BAD8" last_ejection_epoch_ms)
if [[ "${EJ_A8:-0}" -ge 1 && "${EJ_B8:-0}" -ge 1 ]]; then
  ok "上界：Redis 在不在，窗口摘除都发生（a=$EJ_A8 b=$EJ_B8）—— 辅助依赖挂掉不会把拒绝能力一起带走"
else
  fail "ejections a=${EJ_A8:-空} b=${EJ_B8:-空}，期望两边都 ≥ 1"
fi
if [[ "${FB:-0}" -ge 1 && "${FA:-1}" == "0" ]]; then
  ok "下界 + 仪器可见性：Redis 挂那臂 settles_failed=$FB ≥ 1、在位那臂是 $FA（这一格既不恒 0 也不恒非 0）"
else
  fail "settles_failed a=${FA:-空} b=${FB:-空}，期望 0 与 ≥1 这对读数"
fi
if [[ "${DEG_B:-1}" == "0" && "${SOK_B:-0}" -ge 1 ]]; then
  ok "通路活着回来：b 臂终局 redis_degraded=0 且 settles_ok=$SOK_B ≥ 1（同一个进程，没人重启它）"
else
  fail "b 臂 degraded=${DEG_B:-空} settles_ok=${SOK_B:-空}，期望 0 与 ≥1"
fi
if [[ "${REJ_B:-0}" -ge 1 && "$N429_B" -ge 1 ]]; then
  ok "降级期间拒绝能力不消失：退出账 rejected=$REJ_B，突发段（Redis 还挂着）客户端实到 429×$N429_B"
else
  fail "Redis 挂着那臂突发段没有 429（rejected=${REJ_B:-空}）—— 限流不该因为问不到 Redis 就变成放行"
fi
T0_A=$(cat "$OUT/c8-a-t0"); T0_B=$(cat "$OUT/c8-b-t0")
echo "  摘除耗时对照（网关自报 last_ejection_epoch_ms − 各自 T0，同一根墙钟轴）：Redis 在位 = $(( ${EPOCH_A8:-0} - T0_A )) ms ｜ Redis 挂着 = $(( ${EPOCH_B8:-0} - T0_B )) ms"

# ───────────────────────── 类 9：熔断半开 × "探活活、业务死" ─────────────────────────
GW_PORT=18730
GOOD9=18731; BAD9=18732
make_c9() { # 文件名 probe_interval 装不装circuit(1/0) max_fails
  local f=$1 pi=$2 with_circuit=$3 mf=$4 circuit=""
  if [[ "$with_circuit" == "1" ]]; then
    circuit=',
  "circuit": { "window_ms": 20000, "minimum_requests": 6, "error_rate_percent": 50,
               "half_open_requests": 2, "half_open_successes": 2, "half_open_timeout_ms": 3000 }'
  fi
  cat > "$f" <<JSON
{
  "upstreams": [ { "name": "api", "balance": "round_robin", "targets": [
      { "address": "127.0.0.1", "port": $GOOD9 },
      { "address": "127.0.0.1", "port": $BAD9, "health_path": "/probe" } ] } ],
  "routes": [ { "match": "prefix", "pattern": "", "upstream": "api" } ],
  "health": { "max_fails": $mf, "fail_timeout_ms": 800,
              "probe_interval_ms": $pi, "probe_timeout_ms": 300,
              "max_ejection_percent": 100 }$circuit
}
JSON
}
# 每条臂两段：段 1 故障在（/biz9 → 坏台 503，/probe → 200）；段 2 故障撤（换 /ok9 → 坏台回 200/128B）。
# max_fails=99 时连续判据够不着，摘除只能来自错误率窗口（S4-05 同一个手法）；
# 不装 circuit 的那两臂用 max_fails=2 —— 那是 S3-03 唯一有的判据，也是"第二条出口"真正住的地方。
run_c9() { # 标签 probe_interval with_circuit max_fails 条数 间隔ms
  local tag=$1 pi=$2 wc=$3 mf=$4 n=$5 iv=$6
  make_c9 "$OUT/c9-$tag.json" "$pi" "$wc" "$mf"
  local gp bp gwp recv503 codes1 codes2
  gp=$(start_upstream "c9-good-$tag" "$GOOD9" --body-bytes 64)
  bp=$(start_upstream "c9-bad-$tag"  "$BAD9"  --body-bytes 128 --path-status "/biz9:503")
  gwp=$(start_gateway "c9-$tag" "$OUT/c9-$tag.json")
  codes1=$(blast "$OUT/trace-c9-$tag-1.log" "$n" /biz9 "$iv")
  codes2=$(blast "$OUT/trace-c9-$tag-2.log" "$n" /ok9 "$iv")
  stop_pid "$gwp"
  stop_pid "$gp"; stop_pid "$bp"     # 汇总行是退出时打的，先停再读（见 §18 工具侧第 2 条）
  recv503=$(upstream_path_status "$OUT/upstream-c9-bad-$tag.log")
  local lg="$OUT/gateway-c9-$tag.log"
  echo "  [$tag] 段 1（故障在，$n 条 / 间隔 ${iv}ms）503×$(count_code "$codes1" 503) 200×$(count_code "$codes1" 200)；段 2（故障撤，同量）128B×$(grep -cE ' 200 128 ' "$OUT/trace-c9-$tag-2.log")"
  echo "  [$tag] 坏台自报：固定状态码分支（=业务侧被它接走的 503 条数）=$recv503 默认分支（含探活）=$(sed -n 's/^\[upstream\] 已服务请求数=\([0-9]*\).*/\1/p' "$OUT/upstream-c9-bad-$tag.log" | head -1)"
  echo "  [$tag] 退出账 ejections=$(tstat "$lg" 1 "$BAD9" ejections) admitted=$(tstat "$lg" 1 "$BAD9" half_open_admitted) recovered=$(tstat "$lg" 1 "$BAD9" half_open_recovered) reopened=$(tstat "$lg" 1 "$BAD9" half_open_reopened) trips_rate=$(tstat "$lg" 1 "$BAD9" trips_rate) recoveries=$(tstat "$lg" 1 "$BAD9" recoveries) probe_ok=$(tstat "$lg" 1 "$BAD9" probe_ok)"
  # 互背书：客户端数的 503 必须等于坏台自己数的 503——不等就是有一侧在说谎
  local cli503; cli503=$(count_code "$codes1" 503)
  if [[ "${recv503:-x}" == "$cli503" ]]; then
    ok "[$tag] 暴露面两把尺逐数相等：客户端 503×$cli503 == 坏台自报固定状态码×$recv503"
  else
    fail "[$tag] 客户端 503×$cli503 与坏台自报 $recv503 不吻合"
  fi
  # 段 2 里"第一次拿到坏台的 200"落在哪一条 → 恢复耗时（自故障撤掉起算，同 §12 的判据口径）
  local first
  first=$(grep -nE ' 200 128 ' "$OUT/trace-c9-$tag-2.log" | head -1 | cut -d: -f1)
  if [[ -n "$first" ]]; then
    local rel; rel=$(sed -n "${first}p" "$OUT/trace-c9-$tag-2.log" | awk '{print $4}')
    echo "$rel" > "$OUT/c9-$tag-recover_ms"
    echo "  [$tag] 恢复耗时（故障撤 → 客户端再次从它拿到 200）= ${rel} ms（第 $first 条）"
  else
    echo none > "$OUT/c9-$tag-recover_ms"
    echo "  [$tag] 恢复耗时 = 没恢复（段 2 里从没出现该成员的 200/128B）"
  fi
}
echo "== 类 9a：探活开（100ms）+ 装 circuit、涓流 60ms —— 文档与用例对这一格的说法正好相反 =="
run_c9 a 100 1 99 40 60
echo "== 类 9b：探活关（纯被动）+ 装 circuit、涓流（§15 的形状）=="
run_c9 b 0 1 99 40 60
echo "== 类 9c：探活开 + **不装 circuit**、max_fails=2（S3-03 那一档，第二条出口真正住的地方）=="
run_c9 c 100 0 2 40 60
echo "== 类 9d：同 9a 但**突发**（40 条尽快）—— 到达率远高于门预算，看这道门到底夹不夹得住 =="
run_c9 d 100 1 99 40 0
echo "== 类 9e：同 9c 但突发（对照：全量放行那一档）=="
run_c9 e 100 0 2 40 0
# 判定：五条臂的差别只能落在"恢复走到哪一步"上，不能在别的机制上
c9()  { tstat "$OUT/gateway-c9-$1.log" 1 "$BAD9" "$2"; }
exp9(){ upstream_path_status "$OUT/upstream-c9-bad-$1.log"; }
ADM_A=$(c9 a half_open_admitted); ADM_B=$(c9 b half_open_admitted); ADM_D=$(c9 d half_open_admitted)
ADM_C=$(c9 c half_open_admitted); ADM_E=$(c9 e half_open_admitted)
RCL_A=$(c9 a half_open_recovered); RCL_B=$(c9 b half_open_recovered); RCL_D=$(c9 d half_open_recovered)
RCV_A=$(c9 a recoveries); RCV_C=$(c9 c recoveries); RCV_E=$(c9 e recoveries)
TRX_C=$(c9 c trips_rate); TRX_E=$(c9 e trips_rate); PRB_C=$(c9 c probe_ok); PRB_A=$(c9 a probe_ok)
EJ_A=$(c9 a ejections); EJ_B=$(c9 b ejections); EJ_C=$(c9 c ejections)
EJ_D=$(c9 d ejections); EJ_E=$(c9 e ejections)
EXP_A=$(exp9 a); EXP_B=$(exp9 b); EXP_C=$(exp9 c); EXP_D=$(exp9 d); EXP_E=$(exp9 e)
RKA=$(cat "$OUT/c9-a-recover_ms"); RKB=$(cat "$OUT/c9-b-recover_ms"); RKC=$(cat "$OUT/c9-c-recover_ms")
RKD=$(cat "$OUT/c9-d-recover_ms"); RKE=$(cat "$OUT/c9-e-recover_ms")
echo "  裁决格（段 1 的暴露面 = 坏台自己数的 503 条数；每臂段 1 都是 40 条）："
echo "    涓流 60ms：a=circuit+探活 $EXP_A ｜ b=circuit 纯被动 $EXP_B ｜ c=无 circuit（S3 原语义）$EXP_C"
echo "    突发 0ms： d=circuit+探活 $EXP_D ｜ e=无 circuit（S3 原语义）$EXP_E"
echo "    半开放行 a=$ADM_A b=$ADM_B d=$ADM_D ｜ 无 circuit 两臂 c=$ADM_C e=$ADM_E"
echo "    全量次数：a=$RCV_A（其中经半开转正 $RCL_A 次）e=$RCV_E c=$RCV_C ｜ 摘除 a=$EJ_A b=$EJ_B c=$EJ_C d=$EJ_D e=$EJ_E"
echo "    恢复耗时（故障撤→客户端再次拿到它的 200）：a=${RKA}ms b=${RKB}ms c=${RKC}ms d=${RKD}ms e=${RKE}ms"
if [[ -z "$ADM_A$ADM_B$ADM_D$EXP_C$EXP_E" ]]; then
  fail "类 9 有格子读回空串，判定不成立（空串在判定里长得像 0）"
else
  ok "五条臂都读到了自己的账（键名没写错、夹具汇总行是在它退出之后才读的）"
fi
# ① 结构性不变式：半开与窗口都长在 circuit 这个段上，不装就一个都不该有
if [[ "${ADM_C:-1}" == "0" && "${ADM_E:-1}" == "0" && "${TRX_C:-1}" == "0" && "${TRX_E:-1}" == "0" ]]; then
  ok "不变式：不装 circuit 的两臂 half_open_admitted=0 且 trips_rate=0（半开门与错误率窗口都由那一段装配决定）"
else
  fail "无 circuit 的臂出现了半开/窗口读数：admitted c=${ADM_C:-?} e=${ADM_E:-?}，trips c=${TRX_C:-?} e=${TRX_E:-?}"
fi
# ② 装 circuit 的三臂都得走过门（否则"门在位却从不放行"这种退化读不出来）
if [[ "${ADM_A:-0}" -ge 1 && "${ADM_B:-0}" -ge 1 && "${ADM_D:-0}" -ge 1 ]]; then
  ok "下界：三条装了 circuit 的臂都从半开门放过行（a=$ADM_A b=$ADM_B d=$ADM_D）"
else
  fail "有装 circuit 的臂从不放行：a=${ADM_A:-空} b=${ADM_B:-空} d=${ADM_D:-空}"
fi
# ③ 9a：解除的次数与"经半开转正"的次数成对读——探活一次都没有绕过门
if [[ "${RCV_A:-?}" == "${RCL_A:-?}" && "${EJ_A:-0}" -gt "${RCL_A:-999}" && "${ADM_A:-0}" -ge "${EJ_A:-0}" ]]; then
  ok "9a 成对读数：摘除 $EJ_A 次、全量恢复 $RCV_A 次且**逐数等于**经半开转正的次数 $RCL_A —— 探活判活 $PRB_A 次（probe_ok）没有一次直接放回全量；ADR-12 追加二第七节 bullet 2「立即全量」在装了 circuit 的配置上实测为假"
else
  fail "9a 对不上：recoveries=${RCV_A:-空} vs 半开转正=${RCL_A:-空}，ejections=${EJ_A:-空}，admitted=${ADM_A:-空}"
fi
# ④ 9e：不装 circuit 时，每次摘除都由探活解除为全量（第二条出口实测在跑）
if [[ "${RCV_E:-?}" == "${EJ_E:-?}" && "${EJ_E:-0}" -ge 1 && "${ADM_E:-1}" == "0" ]]; then
  ok "9e 成对读数：摘除 $EJ_E 次 == 探活→全量 $RCV_E 次，且没有半开 —— **「两条出口并存」的真实适用面是没装 circuit 这一档**"
else
  fail "9e 对不上：ejections=${EJ_E:-空} recoveries=${RCV_E:-空} admitted=${ADM_E:-空}"
fi
# ⑤ 9c 与认领时的预期相反（涓流档根本没摘），这是新发现的已知边界，照记不升格成断言
if [[ "${EJ_C:-?}" == "0" && "${PRB_C:-0}" -ge 1 && "${EXP_C:-0}" -ge 1 ]]; then
  note "9c（只装 health、探活开、max_fails=2、涓流 60ms）**摘除 0 次**：探活每 100ms 清一次连续计数，而落到这台的业务失败约每 120ms 才一条 → 判据永远够不着 max_fails。它自报接了 $EXP_C/40 条 503，**永久全量**。认领时我预期这一臂会横跳（摘除 ≥1），实测不是——归因与处置写进 §18 与 ADR-12 追加三"
else
  note "9c 读数与本轮的解释不同（ejections=${EJ_C:-?} probe_ok=${PRB_C:-?} 暴露面=${EXP_C:-?}），要重新归因"
fi
# ⑥ 暴露面：门在低到达率与"连续判据本来就只放 max_fails 条"这两档都不改变结论
note "暴露面对照：突发档 9d=$EXP_D vs 9e=$EXP_E（几乎等效——这一档 S3 的连续判据本身一次只放 max_fails=2 条，与 B=2 同量级）；涓流档 9a=$EXP_A vs 9c=$EXP_C（也等效——到达率低于门预算）。**半开门夹住暴露面那一格要在连续判据够不着的档才可见**，那是 §15.1 阶段 1 用 max_fails=99 量到的 24→6"
if [[ "${EXP_B:-9}" -le "${EXP_A:-0}" ]]; then
  ok "9b（$EXP_B）≤ 9a（$EXP_A）—— 同一份故障同一份流量，探活开着会让半开门更早、更频繁地打开"
else
  fail "9b=$EXP_B 大于 9a=$EXP_A，节奏归因要重看"
fi
# ⑦ 恢复耗时：五臂都要有读数，且合并这一档（a）与健康档（c）之差要在"无可分辨"那一侧
for pair in a:"$RKA" b:"$RKB" c:"$RKC" d:"$RKD" e:"$RKE"; do
  v="${pair#*:}"
  if ! [[ "$v" =~ ^[0-9]+$ ]] || (( v < 0 || v > 2000 )); then
    fail "恢复耗时那一格不成立：${pair%%:*} 臂读到 $v"
  fi
done
note "恢复耗时：9a（探活→半开→2 条业务成功即全量）= ${RKA}ms 与 9c（探活→立即全量）= ${RKC}ms 同带；9b（纯被动等到期）= ${RKB}ms。合并这条口的代价在本轮这个量级上读不出来"

# ───────────────────────── 类 10：重试放大 × 熔断错误率窗口 ─────────────────────────
GW_PORT=18740
D10A=18741; D10B=18742       # 两个端口都刻意不绑
cat > "$OUT/c10.json" <<JSON
{
  "upstreams": [ { "name": "api", "balance": "round_robin", "targets": [
      { "address": "127.0.0.1", "port": $D10A },
      { "address": "127.0.0.1", "port": $D10B } ] } ],
  "routes": [ { "match": "prefix", "pattern": "", "upstream": "api" } ],
  "health": { "max_fails": 99, "fail_timeout_ms": 60000,
              "probe_interval_ms": 0, "max_ejection_percent": 100 },
  "circuit": { "window_ms": 20000, "minimum_requests": 20, "error_rate_percent": 50,
               "half_open_requests": 2, "half_open_successes": 2, "half_open_timeout_ms": 3000 }
}
JSON
echo "== 类 10：两台全死 + 错误率窗口（minimum_requests=20，打 24 条 → 第 20 条前后达标，之后摘除生效）=="
G10=$(start_gateway c10 "$OUT/c10.json")
CODES10=$(blast "$OUT/trace-c10.log" 24 /ten 60)
stop_pid "$G10"
CF10=$(get_stat upstream_connect_failed "$OUT/gateway-c10.log")
RT10=$(get_stat upstream_retries        "$OUT/gateway-c10.log")
AR10=$(get_stat gateway_replies_after_retry "$OUT/gateway-c10.log")
EJ10A=$(tstat "$OUT/gateway-c10.log" 0 "$D10A" ejections)
EJ10B=$(tstat "$OUT/gateway-c10.log" 1 "$D10B" ejections)
TR10A=$(tstat "$OUT/gateway-c10.log" 0 "$D10A" trips_rate)
TR10B=$(tstat "$OUT/gateway-c10.log" 1 "$D10B" trips_rate)
echo "  退出账：connect_failed=$CF10 retries=$RT10 after_retry=$AR10 请求数=24；ejections a=$EJ10A b=$EJ10B；trips_rate a=$TR10A b=$TR10B；客户端 502×$(count_code "$CODES10" 502) 503×$(count_code "$CODES10" 503)（503=gateway_replies_all_ejected=$(get_stat gateway_replies_all_ejected "$OUT/gateway-c10.log")）"
# 上界：放大被预算夹住（connect 失败样本 ≤ 请求数 ×(1+预算)）
if [[ "${CF10:-999}" -le 48 ]]; then
  ok "上界：connect_failed=$CF10 ≤ 请求数 24 ×(1+预算 1)=48（失败样本翻倍的上界就是预算本身）"
else
  fail "connect_failed=${CF10:-空} 超过 48 —— 放大越界了"
fi
# 下界：翻倍确实发生（>请求数 就说明有一条请求注入了两笔样本）
if [[ "${CF10:-0}" -gt 24 && "${RT10:-0}" -ge 1 ]]; then
  ok "下界：connect_failed=$CF10 > 24 且 retries=$RT10 ≥ 1 —— ADR-23 第六节那句「一条请求注入 2 个失败样本」是实测，不是推演"
else
  fail "connect_failed=${CF10:-空} retries=${RT10:-空}：翻倍没发生，这一臂只剩上界是恒真"
fi
# 摘除生效后重试自然停：retries 停在窗口达标那一刻，不会一路涨到请求数
if [[ "${RT10:-99}" -lt 24 ]]; then
  ok "重试停在摘除那一刻：retries=$RT10 < 请求数 24（两台都被摘之后没有 kAllow 的落点，资格判据把放大掐住了）"
else
  fail "retries=${RT10:-?} == 请求数，说明摘除没有让重试停下来"
fi
if [[ "${EJ10A:-0}" -ge 1 && "${EJ10B:-0}" -ge 1 ]]; then
  ok "下界：两台都被窗口摘掉（a=$EJ10A b=$EJ10B，trips_rate a=$TR10A b=$TR10B）—— 翻倍落在摘除节奏上"
else
  fail "ejections a=${EJ10A:-空} b=${EJ10B:-空}，期望都 ≥ 1（minimum_requests=20 与每请求 1 笔/台的关系要按实际读数重算）"
fi
echo "== 类 10c：一死一活 + 错误率窗口（落点那台不该背别人的失败）=="
cat > "$OUT/c10c.json" <<JSON
{
  "upstreams": [ { "name": "api", "balance": "round_robin", "targets": [
      { "address": "127.0.0.1", "port": $D10A },
      { "address": "127.0.0.1", "port": $A6 } ] } ],
  "routes": [ { "match": "prefix", "pattern": "", "upstream": "api" } ],
  "health": { "max_fails": 99, "fail_timeout_ms": 60000,
              "probe_interval_ms": 0, "max_ejection_percent": 100 },
  "circuit": { "window_ms": 20000, "minimum_requests": 12, "error_rate_percent": 50,
               "half_open_requests": 2, "half_open_successes": 2, "half_open_timeout_ms": 3000 }
}
JSON
U10C=$(start_upstream c10c-alive "$A6" --body-bytes 64)
G10C=$(start_gateway c10c "$OUT/c10c.json")
CODES10C=$(blast "$OUT/trace-c10c.log" 24 /ten 60)
stop_pid "$G10C"
EJ10D=$(tstat "$OUT/gateway-c10c.log" 0 "$D10A" ejections)
TR10D=$(tstat "$OUT/gateway-c10c.log" 1 "$A6" trips_rate)
EJ10E=$(tstat "$OUT/gateway-c10c.log" 1 "$A6" ejections)
RT10C=$(get_stat upstream_retries "$OUT/gateway-c10c.log")
stop_pid "$U10C"                                 # 先停夹具再读它的汇总行（否则读回空串＝恒真的 0）
RECV10C=$(upstream_served "$OUT/upstream-c10c-alive.log")
echo "  退出账：死台 ejections=$EJ10D；活台 trips_rate=$TR10D ejections=$EJ10E；retries=$RT10C；活台自报实收=$RECV10C；客户端 200×$(count_code "$CODES10C" 200) 502×$(count_code "$CODES10C" 502) 503×$(count_code "$CODES10C" 503)"
if [[ "${TR10D:-1}" == "0" && "${EJ10E:-1}" == "0" ]]; then
  ok "落点那台不背别人的失败：活台 trips_rate=0 且 ejections=0（重试成功的那一条给它的是一笔**成功**样本）"
else
  fail "活台 trips_rate=${TR10D:-空} ejections=${EJ10E:-空}，期望两个都 0"
fi
if [[ "${EJ10D:-0}" -ge 1 ]]; then
  ok "下界：死台被窗口摘掉（ejections=$EJ10D），摘掉之后 24 条全部由活台接走"
else
  fail "死台 ejections=${EJ10D:-空}，窗口没达标（minimum_requests=12 与每请求 1 笔样本的关系要重算）"
fi
# 这一格必须是**等号**：上一版我写成 ≤ 24，而那时夹具还没退出、自报读回来是空串，
# 于是"0 ≤ 24"当场绿——一条恒真断言（§18 工具侧第 2 条）。
if [[ "${RECV10C:-x}" == "24" ]]; then
  ok "不复制：活台自报实收 == 客户端 24 条（死台被试了 12 次、每条都被换台救走，一条没多）"
else
  fail "活台自报 $RECV10C != 24 —— 要么复制了，要么这条读数的口径变了"
fi

echo "== 收尾：生命周期（每臂的网关日志里逐条看）=="
LIVE_BAD=0
for f in "$OUT"/gateway-c*.log; do
  cfg=$(basename "$f" .log); cfg="${cfg#gateway-}"
  case "$cfg" in c8-*) jf="$OUT/c8.json";; *) jf="$OUT/$cfg.json";; esac
  lv=$(get_stat live_connections "$f")
  cd_=$(get_stat connections_constructed "$f"); dd_=$(get_stat connections_destructed "$f")
  fd0=$(get_stat fd_start "$f"); fd1=$(get_stat fd_end "$f")
  if [[ "${lv:-1}" != "0" || "${cd_:-?}" != "${dd_:-?}" ]]; then
    fail "$(basename "$f")：live=${lv:-空} constructed=${cd_:-?}/destructed=${dd_:-?}"
    LIVE_BAD=1
    continue
  fi
  # fd 这一格分两种判法，因为 **fd_end 是在析构之前采样的**（main.cc:879 那条注释），
  # 而在飞的探活 fd 要到 ~Reactor 才关（reactor.cc:167-173）：装探活的臂在退出瞬间必然
  # 带着 1..槽位数 条探测 fd，这不是泄漏，是采样点。S4-04/05/06 那三份"fd 9→9"都出自
  # 没开探活的臂，本轮第一次把这条写成分档判据（§18 工具侧第 3 条）。
  if grep -q '"probe_interval_ms": *[1-9]' "$jf" 2>/dev/null; then
    delta=$(( ${fd1:-0} - ${fd0:-0} ))
    if (( delta >= 0 && delta <= 4 )); then
      ok "$(basename "$f")：live=0、constructed==destructed=$cd_、fd ${fd0}→${fd1}（Δ=$delta，装探活的臂：在飞探测 fd 在采样点之后才关）"
    else
      fail "$(basename "$f")：fd Δ=$delta 超出「在飞探活槽位数」能解释的范围，按泄漏查"
      LIVE_BAD=1
    fi
  elif [[ "${fd0:-?}" == "${fd1:-?}" ]]; then
    ok "$(basename "$f")：live=0、constructed==destructed=$cd_、fd ${fd0}→${fd1}（没开探活 → 应当逐数相等）"
  else
    fail "$(basename "$f")：fd=${fd0:-?}→${fd1:-?} 而这一臂没开探活，应当相等"
    LIVE_BAD=1
  fi
done
(( LIVE_BAD )) || ok "每臂都收了尾：live_connections=0 且 connections_constructed==connections_destructed"

echo "DONE"
if (( FAILED )); then
  echo "✗ S4-07 矩阵有失败格，落盘在 $OUT/"
  exit 1
fi
echo "✓ S4-07 叠加矩阵跑完（读数进 FAULT-INJECTION §18；对照臂另有 FAULT_OUT_DIR=build/fault/s4-07-base 那一跑）"
exit 0
