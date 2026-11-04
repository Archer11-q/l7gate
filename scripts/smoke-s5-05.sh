#!/usr/bin/env bash
# S5-05 冒烟：`docker compose` 一键起全套，在真容器里成立到什么程度（ADR-27）。
#
# ctest 碰不到这里要证的东西：它没有 daemon、没有编排、也没有第二台容器的地址解析行为。
# 这个脚本对着 S5-05 的验收五句：
#   ① 一条 `up -d` 之后：网关健康、**两台上游各自自报接走过请求**（一台一格，和 == 我发的条数）、
#      Prometheus 两条 target 都 up、exporter 那一格的数与盘上逐数相等、Grafana 面板入库且六格非空；
#   ② 全部镜像走契约锁定 tag、`latest` 一条不许出现 —— 判的是 **compose 文件本身**，
#      不是 `docker images` 的输出（后者会把同机别人的 latest 也算进来，那就不是这一栈的事实）；
#   ③ 端口发布拓扑写成事实而不是假设 —— 编排里不许有 network_mode: host；被网关引用的地址必须是
#      静态 IPv4，并且**把服务名那一版真跑一次，它必须 502**：本轮 P-2 那条实测从「我记得
#      inet_pton 不认主机名」变成一条会红的断言；
#   ④ 停栈后没有残留，且**绝不碰同机别人的容器** —— 收尾按 `l7gate-` 前缀数残留与网络，
#      同时断 rgbt-*/knowstack-*/tinrpc-* 的条数与开工前的快照相等；
#   ⑤ Redis 按 B-017 复述 CPU 分配（**只绑 Redis**）—— 文件那一格在阶段 1，运行时两处生效证据
#      （HostConfig + 容器内 cpuset.cpus.effective）在阶段 9，两处都断。
#
# 另外三条是编排自己的守门人，不对应验收句：
#   六、同一件事写在三处（compose 的静态 IP、网关配置、入库抓取配置）必须一致——漂了就一起红；
#   七、抓取不进被测方在容器这一层再证一遍：requests_parsed 恰好等于我发的业务条数；
#   八、容器里的网关退出账仍然闭合（live 归零、配对相等、fd 回基线）。
#
# 用法：PRESET=debug scripts/smoke-s5-05.sh
# 输出：build/smoke-s5-05/ 里的日志，判定同时写进 **build/smoke-s5-05/verdict.txt**。
#   **读判定只认 verdict.txt 与日志里的 ✗ 计数**：本机实测「全跑 + 真容器」的形状下脚本打 FAIL
#   而 `echo $?` 读成 0（§22.2 第 5 条，归因没找到；S5-03/S5-04 同样如此）。不许把 rc=0 当成通过。
set -uo pipefail
cd "$(dirname "$(readlink -f "$0")")/.." || exit 1
trap '' PIPE

PRESET="${PRESET:-debug}"
BIN="build/$PRESET"
GATEWAY="$BIN/l7gate"
UPSTREAM_BIN="$BIN/tests/l7gate_test_upstream"
EXPORTER="$BIN/tools/l7gate_bench_exporter"
OUT="build/smoke-s5-05"
REPO_ABS="$(pwd)"
CF="deploy/compose/docker-compose.yml"
CFG_JSON="deploy/gateway-compose.json"
DASH="deploy/grafana/dashboards/gateway-overview.json"
mkdir -p "$OUT"

# 端口与地址：与 deploy/compose/docker-compose.yml、deploy/gateway-compose.json、
# deploy/prometheus/prometheus.yml 三份里写的**同一批数**（阶段 1 就把这三处对齐断死）。
NET_NAME=l7gate-obs
GW_PORT=18080
M_PORT=18522
E_PORT=18090
PROM_HOST=19090
GRAF_HOST=13000
GRAF_PASS="l7gate-demo"
DASH_UID="l7gate-gateway-overview"
U1_IP=172.31.250.21
U2_IP=172.31.250.22
U1_NAME=upstream-a
U2_NAME=upstream-b
CTRL=l7gate-s5-05-ctrl
CTRL_BIZ=18099          # 对照臂只在容器网络里，不发布到宿主
CTRL_MET=18599

PROM_IMG="prom/prometheus:v2.51.2"
GRAF_IMG="grafana/grafana:10.4.2"
REDIS_IMG="redis:7.2.5"
MYSQL_IMG="mysql:8.0.36"
BASE_IMG="ubuntu:26.04"
RUNTIME_IMG="l7gate/runtime:s5-05"

FAILED=0
FAILS=0
CHECKS=0
ok()   { echo "  ✓ $*"; CHECKS=$((CHECKS + 1)); }
fail() { echo "  ✗ $*"; FAILED=1; FAILS=$((FAILS + 1)); CHECKS=$((CHECKS + 1)); }
die()  { echo "  fatal: $*" >&2;
         echo "FAIL checks=$CHECKS fails=$((FAILS + 1)) die=1" > "$OUT/verdict.txt"; exit 1; }

# ---------- 取数（判定只认落盘，不认印象）----------
metric() { curl -s --max-time 5 "http://127.0.0.1:$M_PORT/metrics" | awk -v k="$1" '$1==k {print $2; exit}'; }
prom_query() { local n=${2:-prom}
  curl -s --max-time 20 --data-urlencode "query=$1" \
    "http://127.0.0.1:$PROM_HOST/api/v1/query" > "$OUT/$n.json" 2>/dev/null
  cat "$OUT/$n.json"; }
prom_series() { prom_query "$1" "$2" | grep -o '"metric":{' | wc -l | tr -d ' '; }
prom_value() { prom_query "$1" "$2" | grep -o '"value":\[[^]]*\]' | head -1 | sed 's/.*,"//; s/"\].*//'; }
# 容器里没有 curl/wget（壳镜像不装它们，见 Dockerfile.runtime），取数一律走 bash 的 /dev/tcp。
http_in_container() { # 容器 端口 路径 → 状态行
  docker exec "$1" bash -c "exec 3<>/dev/tcp/127.0.0.1/$2 || exit 1
    printf 'GET $3 HTTP/1.0\r\nHost: smoke\r\nConnection: close\r\n\r\n' >&3
    head -1 <&3" 2>/dev/null | tr -d '\r' | head -1; }
tcp_open() { docker exec "$1" bash -c "exec 3<>/dev/tcp/127.0.0.1/$2" 2>/dev/null; }

# ---------- 阶段 0：前置 ----------
echo "-- 阶段 0：前置"
for b in "$GATEWAY" "$UPSTREAM_BIN" "$EXPORTER"; do
  [[ -x "$b" ]] || die "找不到 $b —— 先 cmake --build --preset $PRESET（全目标，口径⑪）"
done
ok "三个产物在位（$BIN）：l7gate / l7gate_test_upstream / l7gate_bench_exporter"
command -v curl >/dev/null || die "本机没有 curl"
command -v docker >/dev/null || die "本机没有 docker"
if ! docker info >/dev/null 2>&1; then
  echo "  引擎不在位：Docker Desktop / WSL Integration 没起来。本条依赖真容器，**拒跑**（不是通过）。"
  echo "  与 smoke-s3-04 / smoke-s4-04 / smoke-s5-03 同一种处置：引擎恢复后单跑补绿。"
  echo "FAIL checks=$CHECKS fails=0 refused=1" > "$OUT/verdict.txt"
  exit 1
fi
ok "docker 引擎在位"

# B-017 那句「绑到 15 号核」的前提是引擎侧看得见 16 个核；不成立就是假事实，直接判死。
NCPU=$(docker info --format '{{.NCPU}}' 2>/dev/null)
if [[ "$NCPU" == "16" ]]; then ok "docker info 的 NCPU=16，与发行版 nproc 同一视野（B-017 的绑核那一格以此为前提）"
else fail "NCPU=$NCPU（不是 16）：编排里那句 cpuset=15 就成了假事实"; fi

for img in "$PROM_IMG" "$GRAF_IMG" "$REDIS_IMG" "$MYSQL_IMG" "$BASE_IMG"; do
  if docker image inspect "$img" >/dev/null 2>&1; then
    ok "镜像在位（以 docker image inspect 为准，不以拉取日志为准）：$img"
  else
    fail "镜像不在位：$img（$BASE_IMG 是 ADR-27 登记的 base，缺它就 build 不出壳镜像）"
  fi
done

# exporter 那一格的数据源：build/bench 下必须已有**带着 `增量 p999 =` 那一行**的真实落盘摘要
# （build/ 不入库，全新克隆里跑这条冒烟要先前跑一轮压测——与 smoke-s5-03 阶段 5 同一处置，
#  宁可 die 也不静默换成我手写的假数）。
BENCH_FIXTURE=""
for f in build/bench/*/s1-p999.summary.txt; do
  [[ -f "$f" ]] || continue
  grep -q '^增量 p999 = ' "$f" && { BENCH_FIXTURE="$f"; break; }
done
[[ -n "$BENCH_FIXTURE" ]] || die "build/bench 下没有一份带「增量 p999 =」的摘要可当 fixture（先跑一轮压测）"
BENCH_BATCH=$(basename "$(dirname "$BENCH_FIXTURE")")
ok "exporter 的 fixture 取已有落盘档：$BENCH_BATCH（分位那一格读的就是这一批）"

port_free() { ! (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; }
for p in "$GW_PORT" "$M_PORT" "$E_PORT" "$PROM_HOST" "$GRAF_HOST"; do
  port_free "$p" || die "宿主端口 $p 已被占用：先清场（这一栈发布的就是这几个端口）"
done
ok "宿主端口 $GW_PORT/$M_PORT/$E_PORT/$PROM_HOST/$GRAF_HOST 全空"

# 开工前把**别人**的容器数一次（阶段 10 拿它对照：碰了别人就红）。
OTHER_BEFORE=$(docker ps -a --format '{{.Names}}' | grep -Ec '^(rgbt-|knowstack-|tinrpc-)' || true)
ok "开工前别人的容器快照：rgbt-*/knowstack-*/tinrpc-* 共 ${OTHER_BEFORE:-?} 条（本脚本一条都不碰）"

if docker compose -f "$CF" config --quiet; then ok "compose 文件过了 config 校验（语法与插值）"; else fail "compose config 校验失败"; fi

# ---------- 阶段 1：编排文件本身的事实（句②③⑤ 的文件那一半 + 三处对齐） ----------
echo "-- 阶段 1：compose 文件里那些必须写成事实的东西"
grep -oE 'image: +[^ ]+' "$CF" | awk '{print $2}' | sort -u > "$OUT/images.txt"
for want in "$PROM_IMG" "$GRAF_IMG" "$REDIS_IMG" "$MYSQL_IMG"; do
  grep -qx "$want" "$OUT/images.txt" && ok "锁定 tag 逐字命中：$want" || fail "缺锁定 tag：$want"
done
grep -qx "$RUNTIME_IMG" "$OUT/images.txt" \
  && ok "自产物那三台走同一个壳镜像：$RUNTIME_IMG（它不 COPY 产物，理由在 Dockerfile.runtime 顶上那段）" \
  || fail "壳镜像那一条没有走 l7gate/runtime:s5-05"
LATEST=$(grep -cE 'image: +[^ ]*latest' "$CF")
[[ "$LATEST" == "0" ]] && ok "compose 文件里 latest 出现 0 次（判的是文件，不是 docker images）" \
                      || fail "compose 文件里出现 $LATEST 条 latest"
HOSTNET=$(grep -cE '^ *network_mode:' "$CF")
[[ "$HOSTNET" == "0" ]] && ok "没有 network_mode: host（本机实测不成立，ENVIRONMENT 5.1）" \
                       || fail "出现 network_mode 共 $HOSTNET 条"
CPUS=$(grep -cE '^ *cpuset:' "$CF")
CPUS_SVC=$(awk '/^  [a-z0-9-]+:$/{svc=$1} /^ *cpuset:/{print svc}' "$CF" | tr -d ':' | sort -u | tr '\n' ' ')
if [[ "$CPUS" == "1" && "$CPUS_SVC" == "redis " ]]; then
  ok "cpuset 全栈只有一条、落在 redis 那一段（B-017：只绑 Redis）"
else
  fail "cpuset 条数=$CPUS、落在[$CPUS_SVC]——「只绑 Redis」这句与文件不符"
fi
IP_COUNT=$(grep -cE '^ *ipv4_address:' "$CF")
SUBNET=$(grep -c 'subnet: 172.31.250.0/24' "$CF")
if [[ "$IP_COUNT" == "6" && "$SUBNET" == "1" ]]; then
  ok "被引用的地址全是静态 IPv4（$IP_COUNT 条 ipv4_address + 1 条显式子网）"
else
  fail "静态地址形状对不上：ipv4_address=$IP_COUNT（期望 6）、subnet=$SUBNET（期望 1）"
fi
# 只数 YAML 里那一条列表项，不数注释里的字面串（`network_mode` 那一处我已经踩过一次，这是同族第二次）：
EXH=$(grep -cE '^ +- "host\.docker\.internal:host-gateway"' "$CF")
[[ "$EXH" == "2" ]] && ok "extra_hosts 在 prometheus 与 grafana 两处都带上（显式声明那条抓取通路，不押在 daemon 的自动行为上）" \
                   || fail "extra_hosts 出现 $EXH 次（期望 2）
     注：这条判的是**文件里写没写**，不是「不写就抓不到」——C4 实测删掉它两条 target 仍全 up
         （Docker Desktop 的内置 DNS 自己给 host.docker.internal，本机读数 192.168.65.254）。"

grep -qF "\"address\": \"$U1_IP\", \"port\": 18081" "$CFG_JSON" \
  && grep -qF "\"address\": \"$U2_IP\", \"port\": 18082" "$CFG_JSON" \
  && ok "网关配置里那两台上游的地址 == 编排里钉死的那两个静态 IP" \
  || fail "网关配置与编排的静态 IP 不一致（同一件事写两处，漂了就一起红）"
grep -q "host.docker.internal:$M_PORT" deploy/prometheus/prometheus.yml \
  && grep -q "host.docker.internal:$E_PORT" deploy/prometheus/prometheus.yml \
  && ok "入库的 prometheus.yml 那两个 target 字面仍是 host.docker.internal:18522/18090（本任务只更正了它顶上那段注释的说法，端口与主机名一个字母没动）" \
  || fail "入库抓取配置的目标被动了——那 S5-03 测的就不是这一份"
grep -q 'host.docker.internal:19090' deploy/grafana/provisioning/datasources/l7gate.yml \
  && ok "Grafana 数据源的 URL 字面仍是 host.docker.internal:19090（同样只更正注释，值没动）" \
  || fail "Grafana 数据源那一份的值被动了"

# ---------- 阶段 2：一条 up -d ----------
echo "-- 阶段 2：docker compose up -d --build（PRESET=$PRESET）"
UP_T0=$(date +%s)
L7GATE_PRESET="$PRESET" docker compose -f "$CF" up -d --build > "$OUT/up.log" 2>&1
UP_RC=$?
if [[ "$UP_RC" == "0" ]]; then ok "up -d --build rc=0"; else fail "up 失败 rc=$UP_RC，看 $OUT/up.log"; tail -12 "$OUT/up.log"; fi
healthy=0
for i in $(seq 1 90); do
  docker ps -a --filter name=l7gate- --format '{{.Names}} {{.Status}}' > "$OUT/ps.txt" 2>/dev/null
  healthy=$(grep -c '(healthy)' "$OUT/ps.txt")
  [[ "$healthy" == "8" ]] && break
  sleep 2
done
if [[ "$healthy" == "8" ]]; then
  ok "八台全部 healthy（轮询 $i 次、$(( $(date +%s) - UP_T0 )) s）：gateway/两台上游/exporter/redis/mysql/prometheus/grafana"
else
  fail "只有 $healthy/8 台 healthy，超时——状态表在 $OUT/ps.txt"
  cat "$OUT/ps.txt"
fi
# 网关这一台的「健康」不是「端口 accept 了」：判据是观测面答得出 # HELP。
if docker exec l7gate-gateway-1 sh /checks/healthcheck-gateway.sh 2>/dev/null; then
  ok "网关容器的健康判据成立：观测面能答出 # HELP（healthcheck-gateway.sh 顶上写了为什么不用「accept 就算健康」）"
else
  fail "网关容器的健康判据不成立"
fi
# Redis 的持久化那格是配置里的一个事实，不是我以为：B-017 明写易失状态不快照。
# `redis-cli CONFIG GET save` 回**两行**（键名一行、值一行），第一行永远是 "save"——
# 上一版我把整个输出当值读，于是"关掉的那个格"读成了没关掉（仪器侧的错，不是被测方的）。
SAVEVAL=$(docker exec l7gate-redis-1 redis-cli CONFIG GET save 2>/dev/null | tail -n +2 | tr -d '\r\n')
APPO=$(docker exec l7gate-redis-1 redis-cli CONFIG GET appendonly 2>/dev/null | tail -n +2 | tr -d '\r\n')
if [[ -z "$SAVEVAL" && "$APPO" == "no" ]]; then
  ok "redis 的 save 值为空、appendonly=no（B-017：限流计数是易失状态，RDB 快照会把 CPU 抖进尾延迟）"
else
  fail "持久化没关掉：save 的值读成「${SAVEVAL:-空}」、appendonly 读成「${APPO:-空}」"
fi

# ---------- 阶段 3：句① 前半，业务面通 + 网关侧逐数对账 ----------
echo "-- 阶段 3：经 -p 发布的业务端口打 12 条"
PARSED_BEFORE=$(metric requests_parsed)
FAILED_BEFORE=$(metric upstream_connect_failed)
[[ -n "$PARSED_BEFORE" ]] && ok "观测面可抓：requests_parsed=$PARSED_BEFORE（容器里那一台，经宿主 18522）" \
                         || fail "观测面抓不到 requests_parsed"
HITS=0
BUSINESS_CURLS=0
for i in $(seq 1 12); do
  body=$(curl -s --max-time 5 "http://127.0.0.1:$GW_PORT/c$i")
  BUSINESS_CURLS=$((BUSINESS_CURLS + 1))
  case "$body" in seq=*) HITS=$((HITS + 1));; esac
done
if [[ "$HITS" == "$BUSINESS_CURLS" ]]; then
  ok "$BUSINESS_CURLS 条全部拿到带 seq= 的上游正文（经网关命中两台上游）"
else
  fail "只有 $HITS/$BUSINESS_CURLS 条拿到带 seq= 的正文"
fi
PARSED_AFTER=$(metric requests_parsed); FAILED_AFTER=$(metric upstream_connect_failed)
if [[ -n "$PARSED_BEFORE" && -n "$PARSED_AFTER" ]] && (( PARSED_AFTER - PARSED_BEFORE == BUSINESS_CURLS )); then
  ok "requests_parsed 增量恰好 $BUSINESS_CURLS（$PARSED_BEFORE → $PARSED_AFTER）——容器里的抓取与探活都没进业务计数"
else
  fail "requests_parsed 增量对不上：$PARSED_BEFORE → $PARSED_AFTER，期望 +$BUSINESS_CURLS"
fi
if [[ -n "$FAILED_BEFORE" && -n "$FAILED_AFTER" ]] && (( FAILED_AFTER - FAILED_BEFORE == 0 )); then
  ok "upstream_connect_failed 增量 = 0：网关按**静态 IPv4** 打到了两台上游（P-2/P-3 的形状，不靠主机名）"
else
  fail "upstream_connect_failed 动了：$FAILED_BEFORE → $FAILED_AFTER——静态 IP 那条路没走通"
fi
SETTLES=$(metric redis_settles_ok)
if [[ -n "$SETTLES" ]] && (( SETTLES >= 1 )); then
  ok "redis_settles_ok=$SETTLES（下界：Redis 在那台网关的账里真被用过，B-017 那句绑核因此有主体）"
else
  fail "redis_settles_ok 读不到或为 0：--redis-db 那一段其实是死的"
fi

# ---------- 阶段 4：句③ 的正身。**S5-09 把这一臂的形状改了**：服务名那一版现在在装配期就被拒，
# 不再走到运行期 502。判据跟着改，而且它同时是 S5-09 的一条外部证据——拿**没有那条校验的实现**跑这一臂，
# 红的是「对照容器居然起来了」那一格（不是"读不到 502"），所以这条判据有牙。
# 上界配下界：同一份 JSON 只把 address 换成合法 IPv4 字面量，那一台必须正常服务，
# 否则「恒拒载」那种退化在这里也会红。
echo "-- 阶段 4：对照臂，服务名（上界，装配期拒载）+ 合法 IPv4（下界，照常服务）"
cat > "$OUT/gw-byname.json" <<JSON
{
  "upstreams": [ { "name": "api", "balance": "round_robin", "targets": [
      { "address": "$U1_NAME", "port": 18081 } ] } ],
  "routes": [ { "match": "prefix", "pattern": "", "upstream": "api" } ]
}
JSON
cat > "$OUT/gw-byip.json" <<JSON
{
  "upstreams": [ { "name": "api", "balance": "round_robin", "targets": [
      { "address": "$U1_IP", "port": 18081 } ] } ],
  "routes": [ { "match": "prefix", "pattern": "", "upstream": "api" } ]
}
JSON
run_ctrl() { # 配置文件名 —— 不再把网关输出重定向到容器里的 /tmp：拒载那句话现在要从 docker logs 读
  docker rm -f "$CTRL" >/dev/null 2>&1
  docker run -d --name "$CTRL" --network "$NET_NAME" \
    -v "$REPO_ABS/$BIN:/app:ro" -v "$REPO_ABS/$OUT:/cfg:ro" \
    --entrypoint "" "$RUNTIME_IMG" \
    sh -c "/app/l7gate --serve --listen 0.0.0.0:$CTRL_BIZ --config /cfg/$1 --metrics-port $CTRL_MET" \
    > "$OUT/ctrl-run.log" 2>&1
}
run_ctrl gw-byname.json
CTRL_RC=""
for i in $(seq 1 40); do
  CTRL_RC=$(docker inspect -f '{{.State.Status}} {{.State.ExitCode}}' "$CTRL" 2>/dev/null)
  [[ "$CTRL_RC" == exited* ]] && break
  sleep 1
done
docker logs "$CTRL" > "$OUT/ctrl.bye.log" 2>&1
if [[ "$CTRL_RC" == exited* ]]; then
  if [[ "$CTRL_RC" == *"2" ]] && grep -qF '不是 IPv4 字面量' "$OUT/ctrl.bye.log"; then
    ok "服务名那一版在装配期就被拒：状态「$CTRL_RC」，话里点到 inet_pton 那条理由（旧实现到不了这一格）"
  else
    fail "服务名那一版退成「$CTRL_RC」或话里没点「不是 IPv4 字面量」——期望 rc=2 + 点名，原样读数在 $OUT/ctrl.bye.log"
  fi
  if grep -qF "\"$U1_NAME\"" "$OUT/ctrl.bye.log"; then
    ok "点名点到实际读到的值（话里有 \"$U1_NAME\"），不是一句泛泛的配置错"
  else
    fail "拒载的话里没带实际读到的值 $U1_NAME：那条断言要的是「运维看得懂是哪个字段」"
  fi
  if tcp_open "$CTRL" "$CTRL_MET"; then
    fail "拒载之后观测端口还开着：那不算拒载"
  else
    ok "端口根本没开（拒载发生在装配期，不是起了再来失败）"
  fi
else
  fail "对照容器起来了（状态「${CTRL_RC:-读不到}」）：服务名那一版的 address 本该在装配期就被拒（S5-09）"
fi
docker rm -f "$CTRL" >/dev/null 2>&1

run_ctrl gw-byip.json
CTRL_UP=0
for i in $(seq 1 40); do
  tcp_open "$CTRL" "$CTRL_MET" && { CTRL_UP=1; break; }
  sleep 1
done
if (( CTRL_UP == 1 )); then
  S=$(http_in_container "$CTRL" "$CTRL_BIZ" /byip)
  if [[ "$S" == *"200"* ]]; then
    ok "下界：同一份配置只把 address 换成合法字面量 $U1_IP 就正常服务（挡住「恒拒载」那种退化）"
  else
    fail "合法 IPv4 那一版的状态行是「$S」——期望 200：校验前移不该改变任何合法配置的行为"
  fi
  # **这一发是真打到上游去的**（旧那一臂走的是 502，一个字节都没到上游），所以阶段 8 那条
  # "上游自报之和 == 我发的业务条数"的等式必须把它算进去。数从**对照那台网关自己的观测面**取，
  # 不从我脑子里的"1"取——取不到就判红，不用空串冒充 0（§25.2 第 2 条那一族）。
  CTRL_COMPLETED=$(docker exec "$CTRL" bash -c "exec 3<>/dev/tcp/127.0.0.1/$CTRL_MET || exit 1
      printf 'GET /metrics HTTP/1.0\r\nHost: smoke\r\nConnection: close\r\n\r\n' >&3
      while read -r line; do case \"\$line\" in responses_completed\\ *) echo \"\${line#responses_completed }\"; break;; esac; done <&3" 2>/dev/null | tr -d '\r' | head -1)
  if [[ "$CTRL_COMPLETED" =~ ^[0-9]+$ ]]; then
    ok "对照臂自报 responses_completed=$CTRL_COMPLETED（这一发要进阶段 8 那条等式，数从被测方取、不从我脑子里取）"
  else
    fail "对照臂的 responses_completed 读不到（'${CTRL_COMPLETED:-空}'）——阶段 8 的等式因此不可判，先修取数"
    CTRL_COMPLETED=""
  fi
else
  fail "合法 IPv4 那一版起不来：$OUT/ctrl-run.log 与 docker logs"
fi
docker rm -f "$CTRL" >/dev/null 2>&1
ok "对照容器按名清掉（$CTRL）"

# ---------- 阶段 5：句① 后半，targets up + exporter 与盘上逐数相等 ----------
echo "-- 阶段 5：Prometheus 两条 target + exporter 那一格与盘上对得上"
for i in $(seq 1 30); do
  curl -s --max-time 10 "http://127.0.0.1:$PROM_HOST/api/v1/targets" > "$OUT/targets.json" 2>/dev/null
  UPN=$(grep -o '"health":"up"' "$OUT/targets.json" | wc -l | tr -d ' ')
  [[ "$UPN" == "2" ]] && break
  sleep 3
done
UPN=$(grep -o '"health":"up"' "$OUT/targets.json" 2>/dev/null | wc -l | tr -d ' ')
if [[ "$UPN" == "2" ]]; then ok "两条 target 都 up（网关观测面 + exporter，都走 host.docker.internal + 发布端口）"
else fail "target up 只有 $UPN 条，看 $OUT/targets.json"; fi
grep -o '"scrapeUrl":"http://host.docker.internal:[0-9]*' "$OUT/targets.json" | sort -u > "$OUT/scrape-urls.txt"
if grep -q ":$M_PORT" "$OUT/scrape-urls.txt" && grep -q ":$E_PORT" "$OUT/scrape-urls.txt"; then
  ok "抓取的就是入库那两个端口：$(tr '\n' ' ' < "$OUT/scrape-urls.txt")"
else
  fail "scrapeUrl 里没有 18522/18090 那两条，看 $OUT/scrape-urls.txt"
fi
DISK_MS=$(sed -n 's/^增量 p999 = \([0-9.]*\) ms.*/\1/p' "$BENCH_FIXTURE" | head -1)
DISK_US=$(awk -v m="$DISK_MS" 'BEGIN{printf "%d", m*1000}')
P_US=$(prom_value "l7gate_bench_increment_microseconds{batch=\"$BENCH_BATCH\",quantile=\"p999\"}" p5-bench)
if [[ -n "$DISK_US" && -n "$P_US" && "$DISK_US" == "${P_US%.*}" ]]; then
  ok "exporter 在容器里读挂载的 /bench：盘上 增量 p999 = ${DISK_MS} ms → Prometheus ${P_US} µs，逐数相等（批 $BENCH_BATCH）"
else
  fail "分位那一格与盘上对不上：盘上 '${DISK_MS:-空}' ms（=${DISK_US:-空} µs）vs Prometheus '${P_US:-空}'"
fi
CANON=$(prom_query 'l7gate_bench_increment_microseconds' p5-canonic | grep -o '"canonic"' | wc -l | tr -d ' ')
[[ "$CANON" != "0" ]] && ok "分位那一族带着 canonic 标签（$CANON 条查询结果里有它）" \
                     || fail "分位那一族没带 canonic 标签"

# ---------- 阶段 6：Grafana 面板入库 + 六格非空 ----------
echo "-- 阶段 6：Grafana 面板入库、六格逐格非空"
for i in $(seq 1 60); do
  curl -s --max-time 15 -u "admin:$GRAF_PASS" "http://127.0.0.1:$GRAF_HOST/api/dashboards/uid/$DASH_UID" \
    > "$OUT/grafana-dashboard.json" 2>/dev/null
  grep -q '"title"' "$OUT/grafana-dashboard.json" && break
  sleep 2
done
if grep -q '"title"' "$OUT/grafana-dashboard.json"; then
  ok "面板经 provisioning 入库（不是手工点的）：$DASH_UID，轮询 $i 次拿到"
else
  fail "Grafana 里没有这块面板，看 $OUT/grafana-dashboard.json"
fi
# rate/increase 那两条要窗口里至少两个采样点，所以按**栈起跑**计时而不是按 Prometheus 就绪计时。
while (( $(date +%s) - UP_T0 < 90 )); do sleep 3; done
ok "栈已运行 $(( $(date +%s) - UP_T0 )) s（≥90，1m 展示窗口有样本）"
awk '
  /"id": *[0-9]+/ { p=$0; sub(/.*"id": */, "", p); sub(/[^0-9].*/, "", p); panel=p }
  /"expr":/ {
    line=$0
    sub(/.*"expr": *"/, "", line)
    gsub(/[\\]"/, "\001", line)
    sub(/".*/, "", line)
    gsub(/\001/, "\"", line)
    print panel "\t" line
  }
' "$DASH" > "$OUT/panel-exprs.tsv"
EXPR_N=$(wc -l < "$OUT/panel-exprs.tsv" | tr -d ' ')
[[ "$EXPR_N" == "20" ]] && ok "从面板 JSON 里取出 20 条表达式（表达式与面板同源，不另写一份查询）" \
                       || fail "取到 $EXPR_N 条表达式（期望 20）——取数器漂了"
for want in 1 2 3 4 5 6; do
  total=0; hit=0; empties=""
  while IFS=$'\t' read -r pid expr; do
    [[ "$pid" == "$want" ]] || continue
    total=$((total + 1))
    n=$(prom_series "$expr" "p6-panel${want}-e${total}")
    if (( n > 0 )); then hit=$((hit + 1)); else empties="$empties [#$total]$expr"; fi
  done < "$OUT/panel-exprs.tsv"
  if (( total > 0 && hit == total )); then ok "第 $want 格：$total 条表达式全部查到 series"
  else fail "第 $want 格：$total 条里只有 $hit 条查到，空的是$empties"; fi
done

# ---------- 阶段 7：三句限定语不许在编排这一层丢 ----------
echo "-- 阶段 7：三句限定语（面板两格 + README 两句）"
grep -qF '不能读成「没有坏后端」' "$DASH" && ok "面板带 ARCH 1.31 边界⑥ 那句限定" || fail "面板缺那句限定"
grep -qF '只覆盖首装那批成员' "$DASH" && ok "面板写明逐目标健康键只覆盖首装那批成员" || fail "面板缺逐目标键那句边界"
grep -qF '含热重载的预留槽位容量' "$DASH" && ok "面板 RSS 那格写成「含热重载的预留槽位容量」（B-020）" || fail "面板 RSS 那格没写那句"
grep -qF '只服务演示面' README.md && ok "README 带「容器这一栈只服务演示面」那句（判定仍走裸进程与落盘摘要）" \
  || fail "README 没写「只服务演示面」——编排会被读成判定档"
grep -qF '先构建' README.md && ok "README 写了起栈前要**先构建**（产物是挂进去的，不是打包进去的）" \
  || fail "README 缺「先构建」那句：壳镜像会被读成 compose 自己会构建"

# ---------- 阶段 8：句① 的下界，两台上游各自自报 ----------
echo "-- 阶段 8：停两台上游读自报（每台都接走过 + 和 == 我发的条数）"
docker compose -f "$CF" stop "$U1_NAME" "$U2_NAME" > "$OUT/stop-upstreams.log" 2>&1
A1=$(docker compose -f "$CF" logs "$U1_NAME" 2>/dev/null | grep -o '已服务请求数=[0-9]*' | tail -1 | cut -d= -f2)
A2=$(docker compose -f "$CF" logs "$U2_NAME" 2>/dev/null | grep -o '已服务请求数=[0-9]*' | tail -1 | cut -d= -f2)
if [[ -n "$A1" && -n "$A2" ]]; then
  if (( A1 >= 1 && A2 >= 1 )); then ok "两台上游**各自**自报接走过请求：$U1_NAME=$A1、$U2_NAME=$A2（上界之外还要下界：不是只打到一台）"
  else fail "有一台一条都没接到：$U1_NAME=${A1:-空} $U2_NAME=${A2:-空}"; fi
  # 等式的右边 = 阶段 3 主路径发的 $BUSINESS_CURLS **加上** 阶段 4 那条下界臂的自报条数。
  # 加这一项不是把等式放松：S5-09 之前那一臂是 502、一个字节都不到上游，所以旧等式成立；
  # 现在它是一条真服务起来的请求，被测方自己报了数，就把它的数放进来。
  if [[ -z "$CTRL_COMPLETED" ]]; then
    fail "对照臂那一发的自报条数没取到（阶段 4 已判红）——这条等式因此不可判，不用 0 顶替"
  elif (( A1 + A2 == BUSINESS_CURLS + CTRL_COMPLETED )); then
    ok "$A1 + $A2 == $((BUSINESS_CURLS + CTRL_COMPLETED))（主路径 $BUSINESS_CURLS + 对照臂自报 $CTRL_COMPLETED）：上游自报之和 == 我发的条数 == 网关 requests_parsed 的增量（三方同轴）"
  else
    fail "上游自报之和 $((A1 + A2)) != 主路径 $BUSINESS_CURLS + 对照臂 $CTRL_COMPLETED"
  fi
else
  fail "读不到上游自报数（$U1_NAME='${A1:-空}' $U2_NAME='${A2:-空}'）"
fi

# ---------- 阶段 9：句⑤ 的运行时两处生效证据 ----------
echo "-- 阶段 9：Redis 绑核生效（B-017 那两处证据）"
HOST_CPUSET=$(docker inspect l7gate-redis-1 --format '{{.HostConfig.CpusetCpus}}' 2>/dev/null | tr -d '\r\n')
EFF=$(docker exec l7gate-redis-1 cat /sys/fs/cgroup/cpuset.cpus.effective 2>/dev/null | tr -d '\r\n')
if [[ "$HOST_CPUSET" == "15" && "$EFF" == "15" ]]; then
  ok "HostConfig.CpusetCpus=15 且容器内 cpuset.cpus.effective=15（两处生效证据，同 B-017 的形状）"
else
  fail "绑核没生效：HostConfig='${HOST_CPUSET:-空}' effective='${EFF:-空}'"
fi
GW_CPUSET=$(docker inspect l7gate-gateway-1 --format '{{.HostConfig.CpusetCpus}}' 2>/dev/null | tr -d '\r\n')
if [[ -z "$GW_CPUSET" ]]; then
  ok "网关那台**没有**绑核（B-017 的取舍：契约只要 Redis 不抢网关的核，网关仍可被调度到 15 号核，那条竞争没被消除）"
else
  fail "网关也被绑成 $GW_CPUSET——那就不是「只绑 Redis」"
fi

# ---------- 阶段 10：句④ 停栈无残留、没碰别人 ----------
echo "-- 阶段 10：容器里的退出账 + down 之后没有残留"
docker compose -f "$CF" stop gateway > "$OUT/stop-gateway.log" 2>&1
# `docker compose logs` 每行都带 `<service>-<n>  | ` 前缀，而退出账那些行的锚是行首的 `[serve]`——
# 带前缀读就会**全部落空**，而"落空"在 grep 判定里长得像"没发这些键"（上一版三条红 + 一条假绿）。
docker compose -f "$CF" logs --no-log-prefix gateway 2>/dev/null > "$OUT/gateway-exit.log"
if ! grep -q '^\[serve\] live_connections=' "$OUT/gateway-exit.log"; then
  fail "读不到容器里网关的退出账（$OUT/gateway-exit.log 里没有行首的 [serve]）——下面几条红的全是取数，不是被测方"
fi
LIVE=$(sed -n 's/^\[serve\] live_connections=\([0-9-]*\).*/\1/p' "$OUT/gateway-exit.log" | head -1)
CON=$(sed -n 's/^\[serve\] connections_constructed=\([0-9-]*\).*/\1/p' "$OUT/gateway-exit.log" | head -1)
DES=$(sed -n 's/^\[serve\] connections_destructed=\([0-9-]*\).*/\1/p' "$OUT/gateway-exit.log" | head -1)
FD0=$(sed -n 's/^\[serve\] fd_start=\([0-9-]*\).*/\1/p' "$OUT/gateway-exit.log" | head -1)
FD1=$(sed -n 's/^\[serve\] fd_end=\([0-9-]*\).*/\1/p' "$OUT/gateway-exit.log" | head -1)
if [[ "$LIVE" == "0" ]]; then ok "容器里的网关退出账 live_connections=0（排水收干净）"; else fail "live_connections=${LIVE:-空}"; fi
if [[ -n "$CON" && "$CON" == "$DES" ]]; then ok "constructed==destructed==$CON（配对账不因编排与抓取漂移）"; else fail "constructed=$CON destructed=${DES:-空}"; fi
if [[ -n "$FD0" && "$FD0" == "$FD1" ]]; then ok "fd $FD0 → $FD1 逐数相等（探活关着，走 1.31 边界⑦ 的相等那一档）"; else fail "fd 没回基线：${FD0:-空} → ${FD1:-空}"; fi
if grep -q "^\[serve\] config_" "$OUT/gateway-exit.log"; then
  fail "这一栈没 arm 文件重载却发了 config_ 键——ADR-10 那条「关闭态一个都不发」被编排破坏了"
else
  ok "编排里没有 --config-reload-ms：退出账一个 config_ 键都不发（ADR-10 追加节的定义域原样成立）"
fi

docker compose -f "$CF" down --remove-orphans > "$OUT/down.log" 2>&1
DOWN_RC=$?
RESIDUE=$(docker ps -a --filter name=l7gate- --format '{{.Names}}' | wc -l | tr -d ' ')
NET_RESIDUE=$(docker network ls --filter name=l7gate- --format '{{.Name}}' | wc -l | tr -d ' ')
if [[ "$DOWN_RC" == "0" && "$RESIDUE" == "0" ]]; then
  ok "down rc=0 且 --filter name=l7gate- 的残留容器 0 条（句④ 的判据原样）"
else
  fail "down rc=$DOWN_RC、残留 $RESIDUE 条：$(docker ps -a --filter name=l7gate- --format '{{.Names}}' | tr '\n' ' ')"
fi
[[ "$NET_RESIDUE" == "0" ]] && ok "$NET_NAME 也随 down 移除，网络不残留" || fail "还有 $NET_RESIDUE 条 l7gate- 前缀的网络"
OTHER_AFTER=$(docker ps -a --format '{{.Names}}' | grep -Ec '^(rgbt-|knowstack-|tinrpc-)' || true)
if [[ "$OTHER_BEFORE" == "$OTHER_AFTER" ]]; then
  ok "别人的容器前后同为 $OTHER_AFTER 条（rgbt-*/knowstack-*/tinrpc-* 一条都没碰）"
else
  fail "别人的容器从 $OTHER_BEFORE 变成 $OTHER_AFTER 条——本脚本绝不该动它们"
fi

echo "== 汇总：$CHECKS 项判定，✗ 计数 $FAILS =="
verdict=PASS; (( FAILED )) && verdict=FAIL
echo "$verdict checks=$CHECKS fails=$FAILS" > "$OUT/verdict.txt"
echo "SMOKE-S5-05 $verdict（同一句判定写在 $OUT/verdict.txt）"
exit "$FAILED"
