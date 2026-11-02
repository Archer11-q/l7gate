#!/usr/bin/env bash
# S4-04 冒烟：真 Redis 容器（`redis:7.2.5`，cpuset 绑核）+ 真网关进程，证 ADR-05 追加节
# 的四句话（ctest 那 11 条用 RESP 夹具证的是**状态机**，这里证的是**真对端**）：
#   一 首次连通取证：`l7gate_redis_probe` 七步全通（客户端 1.2.0 ↔ 服务端 7.2.5，含产品
#      那份 Lua 的四格确定性读数）——契约 9.1 那句"未取证前不算既成事实"在这里还款；
#   二 **数字一致**：同一个键（同一个 path）、同一批六连击，接 Redis 与不接必须逐条相同
#      `200 200 200 429 429 429`，且 `rate_limit_capped_rejects=0`——分布式是聚合形态，
#      不是第二种限流语义；
#   三 **夹子真的咬人**：把 Redis 那一格冻成 0，本地桶明明还有余粮（rate/burst=1000）
#      也必须回 429 并计进 `rate_limit_capped_rejects`，解冻后自动恢复放行；
#   四 **降级三段**：容器停 → 本地桶顶住（六连击仍是 3+3，拒绝能力不消失）→ 容器起 →
#      自动恢复（结算继续涨、降级态自己摘掉、夹子重新咬得住）；外加进程侧的矛盾拒绝
#      与生命周期三重检查（结算线程不许留下 fd）。
#
# 用法：PRESET=debug scripts/smoke-s4-04.sh
#       REDIS_CONTAINER=l7gate-redis（默认；不在就按契约 tag 自起、退出时销毁）
#       REDIS_CPUS=15（自起容器时的 cpuset，方案见 ADR-05 追加节第七节）
# 输出：build/smoke-s4-04/ 下的日志；退出码 0 = 全部通过。
#
# 两条纪律上的说明：
#   · `redis_degraded` 与 `redis_settles_*` 是**退出账**（网关自报的计数只在退出时打），
#     所以降级态的实时判据只能是客户端实际收到的状态码，退出账负责事后对账——
#     这不是妥协，是把"仪器不进被测方"和"读数要能钉住"分开写。
#   · redis-cli 在这里只做旁证与故障注入（读写那一格 hash），判据本身不拿它的输出当达标证明。
set -uo pipefail
cd "$(dirname "$(readlink -f "$0")")/.." || exit 1

trap '' PIPE
PRESET="${PRESET:-release}"
BIN="build/$PRESET"
GATEWAY="$BIN/l7gate"
UPSTREAM="$BIN/tests/l7gate_test_upstream"
PROBE="$BIN/tools/l7gate_redis_probe"
OUT="build/smoke-s4-04"
rm -rf "$OUT"; mkdir -p "$OUT"
PIDFILE="$OUT/pids"
: > "$PIDFILE"

FAILED=0
fail() { echo "  ✗ $*"; FAILED=1; }
ok()   { echo "  ✓ $*"; }
die()  { echo "  fatal: $*" >&2; exit 1; }

[[ -x "$GATEWAY" ]] || die "找不到 $GATEWAY，先 cmake --build --preset $PRESET"
[[ -x "$UPSTREAM" ]] || die "找不到 $UPSTREAM，先 cmake --build --preset $PRESET"
[[ -x "$PROBE" ]] || die "找不到 $PROBE（l7gate_redis_probe），先 cmake --build --preset $PRESET"
command -v curl >/dev/null || die "本机没有 curl"
command -v docker >/dev/null || die "本机 WSL 里没有 docker（WSL Integration 未开）"
command -v redis-cli >/dev/null || die "本机没有 redis-cli（redis-tools）——故障注入没法做"

REDIS_CONTAINER="${REDIS_CONTAINER:-l7gate-redis}"
REDIS_CPUS="${REDIS_CPUS:-15}"
REDIS_PORT=16379
GW_PORT=18490
U_PORT=18491
FREE_PORT=18499

port_busy() { (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; }
for p in "$GW_PORT" "$U_PORT" "$FREE_PORT"; do
  port_busy "$p" && die "端口 $p 已被占用：先清场"
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
get_stat() { # key logfile → 值；一行几个键也读得到（旧写法按 `^\[serve\] key=` 锚，
  # 只命得中行首那一个，其余四格返回空串——空串在判定里长得像"0"，比报错危险）。
  local key=$1 log=$2
  grep -o "[[:space:]]$1=[0-9-]*" "$log" 2>/dev/null | head -1 | sed 's/.*=//'
}
wait_bucket_key() { # 等网关真的把某一格键写进 Redis（结算周期 200ms，这里最多等 3s）
  local i k
  for i in $(seq 1 60); do
    k=$(bucket_keys | grep -v ':ovf$' | head -1)
    [[ -n "$k" ]] && { echo "$k"; return 0; }
    sleep 0.05
  done
  echo ""
  return 1
}
rcli() { redis-cli -h 127.0.0.1 -p "$REDIS_PORT" "$@"; }
# 网关用过的 Redis 键（键名是被测方算出来的，我按它的口径去冻它，不硬编码哈希值）
bucket_keys() { rcli --scan --pattern 'l7gate:rl:*' 2>/dev/null | tr -d '\r'; }

# 进程一律按 pid 记账到文件：$(start_gateway …) 是子 shell，数组 add 传不出来
# （S3-05 那轮 `${PIDS[@]:1}` 切片漏发信号的坑是同一族——清场靠文件，不靠我的记性）。
note_pid() { echo "$1" >> "$PIDFILE"; }
start_upstream() { # 标签 → pid
  port_busy "$U_PORT" && die "端口 $U_PORT 上还压着进程（上一臂没退干净）——不带着它往下跑"
  "$UPSTREAM" --listen "127.0.0.1:$U_PORT" --body-bytes 64 > "$OUT/upstream-$1.log" 2>&1 &
  local pid=$!
  note_pid "$pid"
  echo "$pid"
}
start_gateway() { # 标签 配置文件 [额外参数...] → pid
  local tag=$1 cfg=$2; shift 2
  port_busy "$GW_PORT" && die "端口 $GW_PORT 上还压着进程（上一臂没退干净）——不带着它往下跑"
  "$GATEWAY" --serve --listen "127.0.0.1:$GW_PORT" --config "$cfg" "$@" \
    > "$OUT/gateway-$tag.log" 2>&1 &
  local pid=$!
  note_pid "$pid"
  if ! wait_for_line "$pid" "$OUT/gateway-$tag.log" "\[serve\] 监听" 20; then
    fail "网关（$tag）没起来，日志尾部：$(tail -3 "$OUT/gateway-$tag.log" | tr '\n' '|')"
  fi
  echo "$pid"
}
stop_pid() { # pid → 真的等它退出
  # 不能写成 `wait "$pid"`：`$(start_gateway …)` 是在**子 shell** 里起的进程，
  # 它不是本 shell 的孩子，`wait` 当场返回"not a child"——于是脚本在它还没退出时就去读
  # 退出账（空）、再拿同一个端口起下一臂（EADDRINUSE），后面每一臂测到的都是第一臂那个
  # 还活着的进程。本轮实测就是这么一路假绿的。
  local pid=$1 i=0
  kill -TERM "$pid" 2>/dev/null
  while (( i < 400 )); do
    kill -0 "$pid" 2>/dev/null || return 0
    ((i++)); sleep 0.05
  done
  fail "进程 $pid 20 秒还没退出，强杀"
  kill -9 "$pid" 2>/dev/null
  return 1
}
hits() { # <条数> <path> → "200 200 429 …"
  local n=$1 path=$2 out=""
  for _ in $(seq 1 "$n"); do
    out="$out $(curl -s --max-time 5 -o /dev/null -w '%{http_code}' "http://127.0.0.1:$GW_PORT$path")"
  done
  echo "${out# }"
}
# 冻住 / 解冻网关那一格授权（tokens=0 且把 last 推到将来 → 脚本里 elapsed<0 夹成 0 → 补货恒为 0）
freeze_key() { rcli HSET "$1" tokens 0 last 999999999999 >/dev/null; }
melt_key() { rcli HSET "$1" last 0 >/dev/null; }   # elapsed 巨大 → 补货夹回 burst

cleanup() {
  while read -r pid; do [[ -n "$pid" ]] && kill -9 "$pid" 2>/dev/null; done < "$PIDFILE"
  wait 2>/dev/null
  if [[ "${CONTAINER_OURS:-0}" == "1" ]]; then docker rm -f "$REDIS_CONTAINER" >/dev/null 2>&1; fi
}
trap cleanup EXIT

# 两份配置：一致阶段用 S4-03 的同参（rate=1 burst=3，六连击能对上历史读数）；
# 夹子阶段用 rate=1000 burst=1000——几条请求内本地桶不可能是空的，"少放"才只能归因于授权。
cat > "$OUT/rate-3.json" <<JSON
{
  "upstreams": [ { "name": "api", "balance": "round_robin", "targets": [ { "address": "127.0.0.1", "port": $U_PORT } ] } ],
  "rate_limit": { "rate": 1, "burst": 3, "key": "path" }
}
JSON
sed "s/\"rate\": 1, \"burst\": 3/\"rate\": 1000, \"burst\": 1000/" "$OUT/rate-3.json" > "$OUT/rate-1000.json"

echo "== 阶段 0：容器、镜像 tag 与 cpuset 绑核 =="
if docker inspect "$REDIS_CONTAINER" >/dev/null 2>&1; then
  ok "容器 $REDIS_CONTAINER 已存在，复用"
  docker start "$REDIS_CONTAINER" >/dev/null 2>&1 || die "启动已有容器失败"
else
  # 契约 9.1 锁 `redis:7.2.5`；本机另有 redis:8.0——我不拿它顶包，也不换 tag（换了历史断代）。
  docker image inspect redis:7.2.5 >/dev/null 2>&1 \
    || die "本机没有 redis:7.2.5（契约锁定的 tag，我不换；mirror 见 ENVIRONMENT 第 5 节）"
  docker run -d --name "$REDIS_CONTAINER" --cpuset-cpus "$REDIS_CPUS" \
    -p "127.0.0.1:$REDIS_PORT:6379" redis:7.2.5 --save "" --appendonly no >/dev/null \
    || die "起 redis 容器失败"
  CONTAINER_OURS=1
  ok "按契约 tag 自起容器（限流计数是易失状态，持久化关掉——RDB 快照会把 CPU 抖进尾延迟）"
fi
for _ in $(seq 1 60); do [[ "$(rcli PING 2>/dev/null)" == "PONG" ]] && break; sleep 0.2; done
[[ "$(rcli PING 2>/dev/null)" == "PONG" ]] || die "容器起来了但 16379 不回 PONG（端口发布没通？）"
ok "PING → PONG（端口发布形态，同 ENVIRONMENT 5.1 的拓扑结论：容器不共享本发行版网络命名空间）"
VERSION="$(rcli INFO server 2>/dev/null | tr -d '\r' | sed -n 's/^redis_version:\(.*\)/\1/p')"
if [[ "$VERSION" == "7.2.5" ]]; then ok "服务端版本 = $VERSION（契约锁定值）"; else fail "服务端版本是 $VERSION，不是 7.2.5"; fi
CPUS_CONF="$(docker inspect -f '{{.HostConfig.CpusetCpus}}' "$REDIS_CONTAINER" 2>/dev/null)"
CPUS_EFF="$(docker exec "$REDIS_CONTAINER" cat /sys/fs/cgroup/cpuset.cpus.effective 2>/dev/null | tr -d '\r')"
if [[ "$CPUS_CONF" == "$REDIS_CPUS" ]]; then ok "cpuset 配置 = $CPUS_CONF"; else fail "cpuset 配置是 '$CPUS_CONF'，期望 $REDIS_CPUS"; fi
if [[ "$CPUS_EFF" == "$REDIS_CPUS" ]]; then
  ok "容器内 cpuset.cpus.effective = $CPUS_EFF（绑核真的生效，不是只写了个参数）"
else
  fail "容器内 cpuset.cpus.effective 读到 '$CPUS_EFF'，期望 $REDIS_CPUS"
fi

echo "== 阶段 1：首次连通取证（探针七步）=="
"$PROBE" --host 127.0.0.1 --port "$REDIS_PORT" > "$OUT/probe.log" 2>&1
PROBE_RC=$?
if [[ $PROBE_RC -eq 0 ]] && grep -q '结论=七步全通' "$OUT/probe.log"; then
  ok "探针 rc=0、七步全通：$(grep -o 'client=hiredis-[0-9.]*' "$OUT/probe.log" | head -1) ↔ $(grep -o 'server=redis [0-9.]*' "$OUT/probe.log" | tail -1)"
else
  fail "探针 rc=$PROBE_RC，日志尾部：$(tail -3 "$OUT/probe.log" | tr '\n' '|')"
fi
if grep -q 'case=clamp_burst.*✓' "$OUT/probe.log" && grep -q 'case=refill_1s.*✓' "$OUT/probe.log"; then
  ok "Redis 上那份 Lua 的四格确定性读数与本地令牌桶对得上（满桶、扣消耗、按 rate 补、夹回 burst）"
else
  fail "探针没打出确定性的对账读数"
fi

echo "== 阶段 2：数字一致（同一个键，接与不接逐条相同）=="
rcli FLUSHDB >/dev/null 2>&1
UP=$(start_upstream same)
GW_A=$(start_gateway no-redis "$OUT/rate-3.json")
CODES_A="$(hits 6 /same)"
stop_pid "$GW_A"
stop_pid "$UP"
A_ALLOWED=$(get_stat rate_limit_allowed "$OUT/gateway-no-redis.log")
A_REJECTED=$(get_stat rate_limit_rejected "$OUT/gateway-no-redis.log")
if [[ "$CODES_A" == "200 200 200 429 429 429" ]]; then ok "不接 Redis：$CODES_A（S4-03 同参读数复现）"; else fail "不接 Redis 给出 $CODES_A，期望 200 200 200 429 429 429"; fi
if [[ "$A_ALLOWED" == "3" && "$A_REJECTED" == "3" ]]; then ok "退出账 allowed/rejected = 3/3"; else fail "退出账 $A_ALLOWED/$A_REJECTED，期望 3/3"; fi

UP=$(start_upstream same2)
GW_B=$(start_gateway redis "$OUT/rate-3.json" --redis-db "127.0.0.1:$REDIS_PORT" --redis-settle-ms 200)
if grep -q 'redis=on' "$OUT/gateway-redis.log"; then ok "启动横幅 redis=on（addr/settle/timeout/kind 齐全）"; else fail "启动横幅里没有 redis=on"; fi
sleep 0.5   # 至少一拍：让夹子先按授权收紧，六连击才是"分布式那一臂"的读数
CODES_B="$(hits 6 /same)"
stop_pid "$GW_B"
stop_pid "$UP"
B_ALLOWED=$(get_stat rate_limit_allowed "$OUT/gateway-redis.log")
B_REJECTED=$(get_stat rate_limit_rejected "$OUT/gateway-redis.log")
B_CAPPED=$(get_stat rate_limit_capped_rejects "$OUT/gateway-redis.log")
B_OK=$(get_stat redis_settles_ok "$OUT/gateway-redis.log")
B_CMDS=$(get_stat redis_settle_commands "$OUT/gateway-redis.log")
B_DEGRADED=$(get_stat redis_degraded "$OUT/gateway-redis.log")
if [[ "$CODES_B" == "$CODES_A" ]]; then ok "接 Redis 同一批：$CODES_B（与不接逐条相同）"; else fail "接 Redis 给出 $CODES_B，与不接的 $CODES_A 不一致"; fi
if [[ "$B_ALLOWED" == "$A_ALLOWED" && "$B_REJECTED" == "$A_REJECTED" ]]; then ok "退出账同为 $B_ALLOWED/$B_REJECTED"; else fail "退出账 $B_ALLOWED/$B_REJECTED 与不接那臂 $A_ALLOWED/$A_REJECTED 不等"; fi
if [[ "$B_CAPPED" == "0" ]]; then ok "capped_rejects=0：单实例下授权从没压过本地桶"; else fail "capped_rejects=$B_CAPPED，期望 0"; fi
if [[ -n "$B_OK" && "$B_OK" -ge 1 ]]; then ok "redis_settles_ok=$B_OK，redis_settle_commands=$B_CMDS（六连击只花了 $B_CMDS 条上报命令）"; else fail "redis_settles_ok 是 '$B_OK'，期望 ≥1"; fi
if [[ "$B_DEGRADED" == "0" ]]; then ok "redis_degraded=0"; else fail "redis_degraded=$B_DEGRADED，期望 0"; fi
# Redis 侧那一格必须真的被扣过：聚合发生在服务端，不是只有网关自己在数。
KEY=$(wait_bucket_key)
if [[ -z "$KEY" ]]; then
  fail "Redis 里没找到网关用过的键（scan 到：$(bucket_keys | tr '\n' ' ')）"
else
  TOKENS="$(rcli HGET "$KEY" tokens | tr -d '\r')"
  if awk -v t="$TOKENS" 'BEGIN{exit !(t+0 < 3000)}'; then
    ok "Redis 侧 $KEY 的 tokens=$TOKENS < 满额 3000（消耗真记在服务端账上）"
  else
    fail "Redis 侧 tokens=$TOKENS 没被扣过——结算等于没发生"
  fi
fi

echo "== 阶段 3：夹子真的咬人（本地桶有余粮也要少放）=="
# 节奏要说清楚：**冻格之后第一批仍可能放行**——授权是按结算周期更新的，那一批放行正好
# 制造出下一次上报的需求，第二拍才把夹子压下来。这不是脚本在等运气，这就是 ADR-05 追加节
# 第八节写的"跨窗偏差上界 = 一个结算周期的补货量"的实测形状，所以判据打在**第二批**上。
rcli FLUSHDB >/dev/null 2>&1
UP=$(start_upstream cap)
GW_C=$(start_gateway cap "$OUT/rate-1000.json" --redis-db "127.0.0.1:$REDIS_PORT" --redis-settle-ms 200)
sleep 0.5
CODES_OPEN="$(hits 5 /frozen)"
if [[ "$CODES_OPEN" == "200 200 200 200 200" ]]; then ok "授权宽裕时 5 条全放行：$CODES_OPEN"; else fail "阶段 3 开头就没放行（$CODES_OPEN），后面那格无从判断"; fi
FKEY=$(wait_bucket_key)
if [[ -z "$FKEY" ]]; then
  fail "冻不住：没找到那一格键"
else
  freeze_key "$FKEY"
  hits 5 /frozen >/dev/null        # 这批的作用是制造上报需求（放行与否都不参与判定）
  sleep 0.8                          # 四拍：夹子按冻住的授权收紧
  CODES_FROZEN="$(hits 5 /frozen)"
  if [[ "$CODES_FROZEN" == "429 429 429 429 429" ]]; then
    ok "冻住那一格后第二批 5 条全拒：$CODES_FROZEN（rate/burst=1000 的本地桶不可能空，只能是授权挡的）"
  else
    fail "冻住后给出 $CODES_FROZEN，期望 5 条 429——夹子没咬住"
  fi
  melt_key "$FKEY"
  sleep 0.8
  CODES_MELT="$(hits 5 /frozen)"
  if [[ "$CODES_MELT" == "200 200 200 200 200" ]]; then ok "解冻后自动恢复放行：$CODES_MELT"; else fail "解冻后给出 $CODES_MELT，期望 5 条 200"; fi
fi
stop_pid "$GW_C"
stop_pid "$UP"
C_CAPPED=$(get_stat rate_limit_capped_rejects "$OUT/gateway-cap.log")
C_CMDS=$(get_stat redis_settle_commands "$OUT/gateway-cap.log")
if [[ -n "$C_CAPPED" && "$C_CAPPED" -ge 5 ]]; then ok "退出账 rate_limit_capped_rejects=$C_CAPPED（被夹住的那一格在数）"; else fail "capped_rejects 是 '$C_CAPPED'，期望 ≥5"; fi
if [[ -n "$C_CMDS" && "$C_CMDS" -ge 3 ]]; then ok "redis_settle_commands=$C_CMDS（冻与解各逼出一次上报，不是每拍全表问）"; else fail "redis_settle_commands 是 '$C_CMDS'"; fi

echo "== 阶段 4：降级三段（停 → 本地兜底 → 起 → 自动恢复）=="
# 这一段全程用 rate=1/burst=3 那份配置：降级判据要的形式是"六连击仍是 3+3"——
# 这与阶段 2 不接 Redis 的读数同形，才是"兜底 = S4-03 语义"这句话的可执行形式。
rcli FLUSHDB >/dev/null 2>&1
UP=$(start_upstream degrade)
GW_D=$(start_gateway degrade "$OUT/rate-3.json" --redis-db "127.0.0.1:$REDIS_PORT" --redis-settle-ms 200)
sleep 0.5
docker stop "$REDIS_CONTAINER" >/dev/null || die "停容器失败"
ok "docker stop $REDIS_CONTAINER（Redis 挂）"
sleep 1.0   # 至少五拍失败：降级态与 release_all_caps 在这段时间兑现
CODES_DOWN="$(hits 6 /same)"
if [[ "$CODES_DOWN" == "200 200 200 429 429 429" ]]; then
  ok "挂着期间六连击仍是 $CODES_DOWN——**拒绝能力不消失**，兜底就是本地桶的语义（既不放到无限，也不全站 503）"
else
  fail "降级期给出 $CODES_DOWN，期望 200 200 200 429 429 429"
fi
docker start "$REDIS_CONTAINER" >/dev/null || die "起容器失败"
ok "docker start $REDIS_CONTAINER（Redis 回）"
for _ in $(seq 1 60); do [[ "$(rcli PING 2>/dev/null)" == "PONG" ]] && break; sleep 0.2; done
# 让这台网关**撑过**恢复：`redis_degraded` 是退出账，只有在它自己活着走出降级态时才会
# 打成 0——本轮第一版在这里立刻停网关，读到的自然是降级态那格的 1，那条 ✗ 是判据挂错了
# 对象，不是实现没恢复（恢复本身由后面 rate=1000 那一臂的"夹子重新咬住"独立证明）。
sleep 1.2   # 六拍：至少一次成功结算把降级态自己摘掉
stop_pid "$GW_D"          # 兜底那一段读的是这台 rate=3 网关的退出账，先收口再对账
stop_pid "$UP"
# 恢复后的聚合通路必须重新咬得住。这里换一台 rate=1000 的网关：本地桶在几条请求内不可能
# 是空的，所以"429"只能来自服务端授权；判据打在**冻格之后的第二批**上（节奏的理由见阶段 3
# 顶上那段——授权按结算周期更新，第一批放行正是制造第二次上报的需求）。
D_FAILED=$(get_stat redis_settles_failed "$OUT/gateway-degrade.log")
D_OK=$(get_stat redis_settles_ok "$OUT/gateway-degrade.log")
D_DEGRADED=$(get_stat redis_degraded "$OUT/gateway-degrade.log")
if [[ -n "$D_FAILED" && "$D_FAILED" -ge 1 ]]; then ok "退出账 redis_settles_failed=$D_FAILED（挂着期间每一拍都有账，不是静默降级）"; else fail "settles_failed 是 '$D_FAILED'，期望 ≥1"; fi
if [[ "$D_DEGRADED" == "0" ]]; then ok "恢复后 redis_degraded=0（没人重启网关，降级态自己摘掉）"; else fail "终局仍报 degraded=$D_DEGRADED"; fi
if [[ -n "$D_OK" && "$D_OK" -ge 1 ]]; then ok "redis_settles_ok=$D_OK（终局读数；挂着的 8 拍之后还能有成功结算，本身就有账在下面那条 degraded=0 里）"; else fail "settles_ok 是 '$D_OK'"; fi
rcli FLUSHDB >/dev/null 2>&1
UP=$(start_upstream recover)
GW_R=$(start_gateway recover "$OUT/rate-1000.json" --redis-db "127.0.0.1:$REDIS_PORT" --redis-settle-ms 200)
sleep 0.5
CODES_R0="$(hits 5 /recover)"
if [[ "$CODES_R0" == "200 200 200 200 200" ]]; then ok "Redis 回来后限流照常放行：$CODES_R0"; else fail "恢复后第一步就没放行（$CODES_R0）"; fi
RKEY=$(wait_bucket_key)
if [[ -z "$RKEY" ]]; then
  fail "恢复后没找到那一格键"
else
  freeze_key "$RKEY"
  hits 5 /recover >/dev/null
  sleep 0.8
  CODES_RECOVERED="$(hits 5 /recover)"
  if [[ "$CODES_RECOVERED" == "429 429 429 429 429" ]]; then
    ok "恢复后夹子重新咬住：$CODES_RECOVERED（聚合通路自己回来了）"
  else
    fail "恢复后给出 $CODES_RECOVERED，期望 5 条 429——聚合通路没回来"
  fi
fi
stop_pid "$GW_R"
stop_pid "$UP"
R_CAPPED=$(get_stat rate_limit_capped_rejects "$OUT/gateway-recover.log")
if [[ -n "$R_CAPPED" && "$R_CAPPED" -ge 5 ]]; then ok "恢复那臂的 rate_limit_capped_rejects=$R_CAPPED（429 是授权挡的，不是本地桶拒的）"; else fail "capped_rejects 是 '$R_CAPPED'，期望 ≥5"; fi

echo "== 阶段 5：进程侧矛盾与参数域（都该 rc=2 且话里点名）=="
cat > "$OUT/no-rate.json" <<JSON
{ "upstreams": [ { "name": "api", "balance": "round_robin", "targets": [ { "address": "127.0.0.1", "port": $U_PORT } ] } ] }
JSON
"$GATEWAY" --serve --listen "127.0.0.1:$FREE_PORT" --config "$OUT/no-rate.json" \
  --redis-db "127.0.0.1:$REDIS_PORT" > "$OUT/reject-no-rate.log" 2>&1
RC=$?
if [[ $RC -eq 2 ]]; then ok "给了 --redis-db 却没装限流：退出码 2"; else fail "退出码 $RC，期望 2"; fi
if grep -q -- '--redis-db' "$OUT/reject-no-rate.log" && grep -q 'rate_limit' "$OUT/reject-no-rate.log"; then
  ok "拒绝的话里同时点名 --redis-db 与 rate_limit"
else
  fail "拒绝的话没点名字：$(head -1 "$OUT/reject-no-rate.log")"
fi
"$GATEWAY" --serve --listen "127.0.0.1:$FREE_PORT" --config "$OUT/rate-3.json" --redis-settle-ms 50 \
  > "$OUT/reject-settle.log" 2>&1
RC=$?
if [[ $RC -eq 2 ]]; then ok "--redis-settle-ms 50（低于 100）：退出码 2"; else fail "退出码 $RC，期望 2"; fi
"$GATEWAY" --serve --listen "127.0.0.1:$FREE_PORT" --config "$OUT/rate-3.json" --redis-db "127.0.0.1:notaport" \
  > "$OUT/reject-badaddr.log" 2>&1
RC=$?
if [[ $RC -eq 2 ]]; then ok "--redis-db 的端口不是数字：退出码 2"; else fail "退出码 $RC，期望 2"; fi
# 合法组合仍然要起得来（否则上面那三条 rc=2 只是"什么都起不来"）
UP=$(start_upstream ok)
GW_OK=$(start_gateway ok-params "$OUT/rate-3.json" --redis-db "127.0.0.1:$REDIS_PORT" \
  --redis-settle-ms 100 --redis-timeout-ms 2000)
if grep -q 'redis=on .*settle_ms=100 timeout_ms=2000' "$OUT/gateway-ok-params.log"; then
  ok "参数域内合法组合正常启动（settle=100 timeout=2000）"
else
  fail "合法组合没能按参数启动：$(grep 'redis=on' "$OUT/gateway-ok-params.log" | head -1)"
fi
stop_pid "$GW_OK"
stop_pid "$UP"

echo "== 阶段 6：生命周期三重检查（结算线程不许留下任何东西）=="
UP=$(start_upstream life)
GW_L=$(start_gateway life "$OUT/rate-3.json" --redis-db "127.0.0.1:$REDIS_PORT" --redis-settle-ms 200)
sleep 0.4
hits 4 /life >/dev/null
stop_pid "$GW_L"
stop_pid "$UP"
LIVE=$(get_stat live_connections "$OUT/gateway-life.log")
CONSTRUCTED=$(get_stat connections_constructed "$OUT/gateway-life.log")
DESTRUCTED=$(get_stat connections_destructed "$OUT/gateway-life.log")
FD_START=$(get_stat fd_start "$OUT/gateway-life.log")
FD_END=$(get_stat fd_end "$OUT/gateway-life.log")
if [[ "$LIVE" == "0" ]]; then ok "live_connections=0"; else fail "live_connections=$LIVE"; fi
if [[ -n "$CONSTRUCTED" && "$CONSTRUCTED" == "$DESTRUCTED" ]]; then ok "constructed==destructed==$CONSTRUCTED"; else fail "constructed=$CONSTRUCTED / destructed=$DESTRUCTED 不等"; fi
if [[ "$FD_START" == "$FD_END" ]]; then ok "fd $FD_START→$FD_END 回到基线（Redis 连接没漏）"; else fail "fd $FD_START→$FD_END 不等——多出来的那个 fd 就是没关的 Redis 连接"; fi

echo
if [[ $FAILED -eq 0 ]]; then
  echo "smoke-s4-04: 全部通过（日志在 $OUT/）"
  echo "DONE"
  exit 0
fi
echo "smoke-s4-04: 有失败项（见上面 ✗），日志在 $OUT/"
exit 1
