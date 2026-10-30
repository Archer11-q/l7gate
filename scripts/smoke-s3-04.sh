#!/usr/bin/env bash
# S3-04 冒烟：真 MySQL 容器（`mysql:8.0.36`）+ 真网关进程，证 ADR-20 的四句话：
#   一 source=mysql 的组真的从 `l7gate_nodes` 拿到成员（由被连的那一侧自报）；
#   二 **reload 生效耗时**（B-015）：INSERT 一行（T0，脚本墙钟）→ 网关第一次把请求
#      发到新成员（T1，首次观测到它的响应体长），两端钉住；DELETE 对称验证；
#   三 **降级路径**：mysqld 停掉后业务零中断（全部 200、自产错误响应数 0），
#      恢复后 reload 自动接上、被删的成员重新到岗；
#   四 探活/节点表都不进业务计数：requests_parsed == responses_completed == 发出的请求数。
#
# 用法：PRESET=release scripts/smoke-s3-04.sh
#       MYSQL_CONTAINER=l7gate-mysql（默认；容器不在就按镜像自起、退出时销毁）
# 输出：build/smoke-s3-04/ 下的日志；退出码 0 = 全部通过。
set -uo pipefail
cd "$(dirname "$(readlink -f "$0")")/.." || exit 1

trap '' PIPE
PRESET="${PRESET:-release}"
BIN="build/$PRESET"
GATEWAY="$BIN/l7gate"
UPSTREAM="$BIN/tests/l7gate_test_upstream"
PROBE="$BIN/tools/l7gate_mysql_probe"
OUT="build/smoke-s3-04"
mkdir -p "$OUT"

FAILED=0
fail() { echo "  ✗ $*"; FAILED=1; }
ok()   { echo "  ✓ $*"; }
die()  { echo "  fatal: $*" >&2; exit 1; }

[[ -x "$GATEWAY" ]] || die "找不到 $GATEWAY，先 cmake --build --preset $PRESET"
[[ -x "$UPSTREAM" ]] || die "找不到 $UPSTREAM，先 cmake --build --preset $PRESET"
[[ -x "$PROBE" ]] || die "找不到 $PROBE（l7gate_mysql_probe），先 cmake --build --preset $PRESET"
command -v curl >/dev/null || die "本机没有 curl"
command -v docker >/dev/null || die "本机 WSL 里没有 docker（WSL Integration 未开）"

MYSQL_CONTAINER="${MYSQL_CONTAINER:-l7gate-mysql}"
GW_PORT=18480
U1=18481; U2=18482; U3=18483
DB_PORT=13306

port_free() { ! (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; }
for p in "$GW_PORT" "$U1" "$U2" "$U3"; do
  port_free "$p" || die "端口 $p 已被占用：先清场"
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
get_stat() { sed -n "s/^\[serve\] $1=\([0-9-]*\).*/\1/p" "$2" | head -1; }
epoch_ms() { echo $(( ${EPOCHREALTIME/./} / 1000 )); }
req() { # port path → "code bytes"
  local port=$1 path=$2 file="$OUT/body.$$"
  local code
  code=$(curl -s --max-time 5 -o "$file" -w '%{http_code}' "http://127.0.0.1:$port$path" 2>/dev/null)
  local n; n=$(wc -c < "$file" 2>/dev/null); rm -f "$file"
  echo "${code:-000} ${n:-0}"
}
mysql_exec() { # SQL...
  docker exec l7gate-mysql sh -c "mysql -ul7gate -pl7gate-pw l7gate -e \"$1\"" 2>/dev/null
}

PIDS=()
cleanup() {
  for pid in "${PIDS[@]}"; do kill -9 "$pid" 2>/dev/null; done
  wait 2>/dev/null
  if [[ "${CONTAINER_OURS:-0}" == "1" ]]; then docker rm -f "$MYSQL_CONTAINER" >/dev/null 2>&1; fi
}
trap cleanup EXIT

echo "== 阶段 0：容器与表 =="
if docker inspect "$MYSQL_CONTAINER" >/dev/null 2>&1; then
  CONTAINER_OURS=0
  docker start "$MYSQL_CONTAINER" >/dev/null 2>&1 || true
else
  docker run -d --name "$MYSQL_CONTAINER" -p "127.0.0.1:$DB_PORT:3306" \
    -e MYSQL_ROOT_PASSWORD=l7gate-root -e MYSQL_DATABASE=l7gate \
    -e MYSQL_USER=l7gate -e MYSQL_PASSWORD=l7gate-pw mysql:8.0.36 >/dev/null || die "起容器失败"
  CONTAINER_OURS=1
fi
# 等 mysqld 就绪（首次初始化要几十秒）。
ready=0
for i in $(seq 1 120); do
  if "$PROBE" --port "$DB_PORT" > "$OUT/probe.log" 2>&1; then ready=1; break; fi
  sleep 1
done
(( ready )) || die "MySQL 60 秒内没就绪，probe 日志：$(tail -3 "$OUT/probe.log")"
ok "首次连通取证复跑通过（$(sed -n 's/^\[probe\] server=//p' "$OUT/probe.log")）"
mysql_exec "DELETE FROM l7gate_nodes" || die "清表失败"

echo "== 阶段 1：source=mysql 的组从表里拿到成员 =="
start_upstream() { # port body_bytes log
  "$UPSTREAM" --listen "127.0.0.1:$1" --body-bytes "$2" > "$3" 2>&1 &
  local pid=$!
  PIDS+=("$pid")
  wait_for_line "$pid" "$3" "监听" 15 || return 1
}
start_upstream "$U1" 64 "$OUT/up1.log" || die "上游 1 没起来"
start_upstream "$U2" 128 "$OUT/up2.log" || die "上游 2 没起来"
start_upstream "$U3" 192 "$OUT/up3.log" || die "上游 3 没起来"

mysql_exec "INSERT INTO l7gate_nodes VALUES ('api','127.0.0.1',$U1,'/'),('api','127.0.0.1',$U2,'/')" \
  || die "写成员失败"

cat > "$OUT/gateway.json" <<'JSON'
{
  "upstreams": [ { "name": "api", "balance": "round_robin", "source": "mysql" } ],
  "routes": [ { "match": "prefix", "pattern": "", "upstream": "api" } ]
}
JSON
"$GATEWAY" --serve --listen "127.0.0.1:$GW_PORT" --config "$OUT/gateway.json" --workers 1 \
  --nodes-db "127.0.0.1:$DB_PORT" --nodes-user l7gate --nodes-password l7gate-pw \
  --nodes-schema l7gate --nodes-reload-ms 500 --nodes-timeout-ms 1000 \
  > "$OUT/gateway.log" 2>&1 & GW=$!
PIDS+=("$GW")
wait_for_line "$GW" "$OUT/gateway.log" "nodes=on" 20 || die "网关没打出 nodes=on 横幅"
wait_for_line "$GW" "$OUT/gateway.log" "监听" 20 || die "网关没起来"

saw64=0; saw128=0
for i in $(seq 1 30); do
  read -r c n <<< "$(req "$GW_PORT" /warm)"
  [[ "$c" == "200" ]] || die "预热请求失败 code=$c"
  [[ "$n" == "64" ]] && saw64=1
  [[ "$n" == "128" ]] && saw128=1
  (( saw64 && saw128 )) && break
  sleep 0.1
done
if (( saw64 && saw128 )); then ok "两个成员都接到了流量（体长 64 与 128）"
else fail "30 轮内没见到两台成员：64=$saw64 128=$saw128"; fi

echo "== 阶段 2：reload 生效耗时（B-015）——INSERT 后网关把请求发到新成员 =="
T0=$(epoch_ms)
mysql_exec "INSERT INTO l7gate_nodes VALUES ('api','127.0.0.1',$U3,'/')" || die "INSERT 失败"
saw192=0
for i in $(seq 1 200); do
  read -r c n <<< "$(req "$GW_PORT" /grow)"
  [[ "$c" == "200" ]] || die "reload 期间请求失败 code=$c"
  if [[ "$n" == "192" ]]; then saw192=1; break; fi
  sleep 0.05
done
T1=$(epoch_ms)
if (( saw192 )); then
  RELOAD_MS=$(( T1 - T0 ))
  echo "  reload 生效耗时（INSERT → 新成员首次接到请求）= ${RELOAD_MS} ms（reload 间隔 500ms）"
  if (( RELOAD_MS < 5000 )); then ok "reload 在 5s 预算内生效"; else fail "reload 生效耗时 ${RELOAD_MS} ms 异常"; fi
else
  fail "10 秒内新成员没接到流量：reload 链路断了"
fi

echo "== 阶段 3：DELETE 后新成员不再接流量 =="
mysql_exec "DELETE FROM l7gate_nodes WHERE port=$U3" || die "DELETE 失败"
# 给 reload 生效留一个完整间隔的宽限（500ms + 余量）：宽限期内看到 192 是旧快照的尾巴，
# 不算失败；宽限之后再看到 192 才是"被删的成员还在接流量"。
sleep 1.5
BAD=0
for i in $(seq 1 60); do
  read -r c n <<< "$(req "$GW_PORT" /shrink)"
  [[ "$c" == "200" ]] || die "收缩期请求失败 code=$c"
  [[ "$n" == "192" ]] && BAD=1 && break
  sleep 0.05
done
if (( BAD )); then fail "被删的成员还在接流量"; else ok "被删成员不再接流量（30 轮内未见 192）"; fi

echo "== 阶段 4：降级路径——mysqld 停掉，业务零中断 =="
docker stop "$MYSQL_CONTAINER" >/dev/null || die "停容器失败"
FAILED_BEFORE=0
for i in $(seq 1 20); do
  read -r c n <<< "$(req "$GW_PORT" /degraded)"
  [[ "$c" == "200" ]] || FAILED_BEFORE=1
  sleep 0.1
done
if (( FAILED_BEFORE == 0 )); then ok "MySQL 停机期间 20 条请求全部 200（按已加载成员继续服务）"
else fail "MySQL 停机期间出现非 200：降级路径没接住"; fi

echo "== 阶段 5：恢复——reload 自动接上，成员重新到岗 =="
docker start "$MYSQL_CONTAINER" >/dev/null || die "起容器失败"
for i in $(seq 1 60); do
  docker exec l7gate-mysql sh -c "mysqladmin ping -ul7gate -pl7gate-pw 2>/dev/null" >/dev/null 2>&1 && break
  sleep 1
done
mysql_exec "INSERT INTO l7gate_nodes VALUES ('api','127.0.0.1',$U3,'/')" || die "恢复后 INSERT 失败"
saw192=0
for i in $(seq 1 200); do
  read -r c n <<< "$(req "$GW_PORT" /recover)"
  [[ "$c" == "200" ]] || die "恢复期请求失败 code=$c"
  [[ "$n" == "192" ]] && saw192=1 && break
  sleep 0.05
done
if (( saw192 )); then ok "恢复后 reload 自动接上，成员重新到岗"
else fail "恢复后 10 秒内成员没回来"; fi

echo "== 阶段 6：收尾——网关自报的账 =="
kill -TERM "$GW" 2>/dev/null; wait "$GW" 2>/dev/null
OKR=$(get_stat nodes_reloads_ok "$OUT/gateway.log")
FAILR=$(get_stat nodes_reloads_failed "$OUT/gateway.log")
PARSED=$(get_stat requests_parsed "$OUT/gateway.log")
COMPLETED=$(get_stat responses_completed "$OUT/gateway.log")
LIVE=$(get_stat live_connections "$OUT/gateway.log")
PUB=$(get_stat nodes_last_publish_epoch_ms "$OUT/gateway.log")
if [[ "${OKR:-0}" -ge 3 ]]; then ok "nodes_reloads_ok = ${OKR}（初始 + 增员 + 恢复）"
else fail "nodes_reloads_ok = ${OKR:-?}，期望 ≥ 3"; fi
if [[ "${FAILR:-0}" -ge 1 ]]; then ok "nodes_reloads_failed = ${FAILR}（停机期间那几拍有账）"
else fail "nodes_reloads_failed = ${FAILR:-?}，降级期间没记账"; fi
if [[ -n "$PUB" && "$PUB" != "0" ]]; then
  ok "last_publish_epoch_ms = $PUB（与脚本时间线同一根墙钟轴）"
else
  fail "网关没自报最后发布时刻"
fi
if [[ "${PARSED:-0}" == "${COMPLETED:-1}" && "${PARSED:-0}" != "0" ]]; then
  ok "requests_parsed == responses_completed == $PARSED（节点表查询不进业务计数）"
else
  fail "parsed=${PARSED:-?} completed=${COMPLETED:-?}：业务计数被仪器污染或丢了响应"
fi
if [[ "${LIVE:-1}" == "0" ]]; then ok "live_connections = 0"; else fail "live = ${LIVE:-?}"; fi

for pid in "${PIDS[@]}"; do kill -TERM "$pid" 2>/dev/null; done
wait 2>/dev/null
s1=$(sed -n 's/^\[upstream\] 已服务请求数=\([0-9]*\).*/\1/p' "$OUT/up1.log" | head -1)
s2=$(sed -n 's/^\[upstream\] 已服务请求数=\([0-9]*\).*/\1/p' "$OUT/up2.log" | head -1)
s3=$(sed -n 's/^\[upstream\] 已服务请求数=\([0-9]*\).*/\1/p' "$OUT/up3.log" | head -1)
echo "  上游自报：$U1=${s1:-?} $U2=${s2:-?} $U3=${s3:-?}"

trap - EXIT
if [[ "${CONTAINER_OURS:-0}" == "1" ]]; then docker rm -f "$MYSQL_CONTAINER" >/dev/null 2>&1; fi
echo "DONE"
if (( FAILED )); then
  echo "✗ S3-04 冒烟有失败格，看 $OUT/"
  exit 1
fi
echo "✓ S3-04 冒烟六阶段全过（B-015 reload 生效耗时 = ${RELOAD_MS:-?} ms）"
exit 0
