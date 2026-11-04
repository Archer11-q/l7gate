#!/usr/bin/env bash
# S5-07 · SLO #3 的**终判档**：网关与 nginx **两侧都进容器**、同一种发布方式、打同一个上游进程。
#
# 这条脚本为什么必须存在（不是把 s2-qps-vs-nginx.sh 复制一份）：
#   s2 那条量的是「裸进程网关 vs 容器 nginx」，而两侧的**网络路径不一样**——nginx 的客户端腿多一次
#   端口发布（DNAT/userland proxy）、上游腿多一次容器出向，网关两侧都是原生回环（ENVIRONMENT 5.1
#   实测那张表）。B-005 那句「赢的是对照组不是实现」的批评，根据就是这一维。已裁第 ⑨ 条第 4 项裁的是：
#   **加一臂「网关也进容器」，与 nginx 同拓扑同发布方式，两档都报，终判取对称那一档**；
#   ADR-27 第二节第 6 条把形状钉成「必须按同一形态把两侧都放进容器」。
#   所以这条脚本只做一件事：把那一维**对齐**，其余不齐项照记（「同一分配器记录」仍然不齐——
#   nginx 用它镜像里的 libc/openssl，网关是宿主 glibc 2.43；那是 B-005 第四条理由，不是调参数能抹的）。
#
# 两档的关系（不许混池比较）：
#   档一 = s2-qps-vs-nginx.sh 的「裸进程网关 vs 容器 nginx」：只作对照，报出来并带 DNAT 那条声明；
#   档二 = 本脚本的「容器网关 vs 容器 nginx」：**终判取这一档**。
#
# 网关臂的形态：`--upstream`（空 filter 链，与 s2 的裸进程臂同形）、不装 `--config`、不 arm
# `--metrics-port`、不 arm 热重载。它量的是「转发本体在容器拓扑里的吞吐」，不是「S3+S4 全链的吞吐」。
# 上游走**默认 bridge**，与 s2 里 nginx 那一条完全相同的路径（ENVIRONMENT 5.1 实测：容器 → WSL eth0
# 地址 → 上游端口 是 200 + 1024 字节；`--network host` 在这台机器上不成立）。
#
# 用法：
#   scripts/bench/s5-07-qps-symmetric.sh                       # 正式口径（1KiB / 1000 并发 / 60s / 10s 预热）
#   ORDER=gw-first OUT_DIR=build/bench/s5-07-sym-r2 scripts/bench/s5-07-qps-symmetric.sh
#   DURATION=5s WARMUP=1s scripts/bench/s5-07-qps-symmetric.sh # 只验脚本本身，canonic=0，不进文档
# 输出：$OUT/{summary.txt, 每档 wrk 原始输出, 渲染的 nginx 配置, 容器日志与拓扑快照}
set -uo pipefail
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1

BIN="build/release"
GATEWAY="$BIN/l7gate"
UPSTREAM="$BIN/tools/l7gate_bench_upstream"
[[ -x "$GATEWAY" ]]  || { echo "✗ 找不到 $GATEWAY；先 cmake --build --preset release" >&2; exit 1; }
[[ -x "$UPSTREAM" ]] || { echo "✗ 找不到 $UPSTREAM；先 cmake --build --preset release" >&2; exit 1; }
command -v wrk >/dev/null || { echo "✗ 本机没有 wrk：#3 的判定工具" >&2; exit 1; }
command -v docker >/dev/null || { echo "✗ 本机没有 docker CLI" >&2; exit 1; }

IMAGE_GATEWAY="l7gate/runtime:s5-05"     # ADR-27 那个壳镜像：产物运行期只读挂进来，不 COPY
IMAGE_NGINX="nginx:1.25.4"               # 契约 9.1 锁定的基线 tag，中途不换
BODY_BYTES="${BODY_BYTES:-1024}"
CONNECTIONS="${CONNECTIONS:-1000}"       # 与 SLO #3 / s1-p99.sh 同一套 wrk 参数
DURATION="${DURATION:-60s}"
WARMUP="${WARMUP:-10s}"
WRK_THREADS="${WRK_THREADS:-8}"
WORKERS="${WORKERS:-16}"                 # 被测系统属性；取 16 是与 nginx 的 worker_processes auto 对齐（B-005 第二条批评的那一维）
UPSTREAM_PORT="${UPSTREAM_PORT:-18199}"
GW_PUB="${GW_PUB:-18183}"
NGX_PUB="${NGX_PUB:-18184}"
GW_CONTAINER_PORT=18080
ORDER="${ORDER:-nginx-first}"            # 另一轮排成 gw-first，让「谁先跑」本身成为对照变量（B-018 那条时段漂移的教训）
OUT="${OUT_DIR:-build/bench/s5-07-sym-$(date +%Y%m%d-%H%M%S)}"

CANONIC=1
[[ "$BODY_BYTES" == "1024" && "$CONNECTIONS" == "1000" && "$DURATION" == "60s" &&
   "$WARMUP" == "10s" ]] || CANONIC=0

mkdir -p "$OUT/conf" || exit 1
OUT=$(readlink -f "$OUT")
FAILED=0
note() { echo "  $*"; }
ok()   { echo "  ✓ $*"; }
die()  { echo "  ✗ $*" >&2; FAILED=1; }
fatal() { echo "  ✗ $*" >&2; exit 1; }

CONTAINERS=()
UP_PID=""
cleanup() {
  local c
  for c in ${CONTAINERS[@]+"${CONTAINERS[@]}"}; do
    [[ -n "$c" ]] && docker rm -f "$c" >/dev/null 2>&1
  done
  if [[ -n "$UP_PID" ]]; then kill -TERM "$UP_PID" 2>/dev/null; wait "$UP_PID" 2>/dev/null; fi
}
trap cleanup EXIT

residue_count() { docker ps -a --format '{{.Names}}' 2>/dev/null | grep -c '^l7gate-s5-07-' || true; }
others_count()  { docker ps -a --format '{{.Names}}' 2>/dev/null | grep -cE '^(rgbt-|knowstack-|tinrpc-)' || true; }

# ---------- 前置：引擎与镜像身份，判的是「在位且是那个东西」而不是「命令存在」 ----------
NCPU=$(docker info --format '{{.NCPU}}' 2>/dev/null)
[[ "$NCPU" == "16" ]] || fatal "docker info 的 NCPU=${NCPU:-读不到} ≠ 16：SLO #3 的前提（16 vCPU）不成立，这一档不作数"
docker image inspect "$IMAGE_NGINX" >/dev/null 2>&1 \
  || fatal "本机没有 $IMAGE_NGINX（契约锁定的基线 tag，我不换）；先 docker pull"
docker image inspect "$IMAGE_GATEWAY" >/dev/null 2>&1 \
  || fatal "本机没有 $IMAGE_GATEWAY；它是 ADR-27 的壳镜像，构建入口：docker build -f deploy/compose/Dockerfile.runtime -t $IMAGE_GATEWAY deploy/compose/"
NGX_IMAGE_ID=$(docker image inspect --format '{{.Id}}' "$IMAGE_NGINX")
GW_IMAGE_ID=$(docker image inspect --format '{{.Id}}' "$IMAGE_GATEWAY")
[[ -n "$NGX_IMAGE_ID" && -n "$GW_IMAGE_ID" ]] || fatal "读不到两个镜像的 id"

for p in "$UPSTREAM_PORT" "$GW_PUB" "$NGX_PUB"; do
  if ss -ltn 2>/dev/null | grep -q ":$p "; then
    fatal "端口 $p 已被占用：这一档需要它空着。只按 l7gate-s5-07- 前缀清自己的残留，别碰同机别人的"
  fi
done

OTHERS_BEFORE=$(others_count)
[[ "$CANONIC" == "1" ]] \
  && note "口径：与 SLO #3 / s1-p99.sh / s2-qps-vs-nginx.sh 同一套（1KiB / 1000 并发 / 60s 稳态 / 10s 预热 / wrk 8 线程）" \
  || note "口径：**非标准**（body=$BODY_BYTES conns=$CONNECTIONS duration=$DURATION warmup=$WARMUP）——这份只能验脚本，不进 BENCHMARK.md"

WSL_IP=$(hostname -I | awk '{print $1}')
[[ -n "$WSL_IP" ]] || fatal "拿不到 WSL 的 eth0 地址，两个容器都够不到上游"
HOSTGW_IP=$(getent hosts host.docker.internal 2>/dev/null | awk '{print $1}' | head -1)

wait_http_200() { # url 超时秒
  local url=$1 lim=$2 i=0
  while (( i < lim * 10 )); do
    [[ "$(curl -s -o /dev/null -w '%{http_code}' --max-time 2 "$url")" == "200" ]] && return 0
    sleep 0.1; ((i += 1))
  done
  return 1
}

# ---------- 上游：两臂共用同一个进程（与 s2 同一条理由：只差一个因子） ----------
"$UPSTREAM" --listen "0.0.0.0:$UPSTREAM_PORT" --body-bytes "$BODY_BYTES" \
  --max-clients $((CONNECTIONS * 3 + 256)) --backlog $((CONNECTIONS * 3 + 256)) \
  > "$OUT/upstream.log" 2>&1 &
UP_PID=$!
for i in $(seq 1 150); do
  curl -s --max-time 2 -o /dev/null "http://127.0.0.1:$UPSTREAM_PORT/bench" && break
  sleep 0.1
done
curl -s --max-time 5 -o /dev/null "http://127.0.0.1:$UPSTREAM_PORT/bench" \
  || fatal "上游没起来（看 $OUT/upstream.log）"
note "上游 pid=$UP_PID 端口 $UPSTREAM_PORT"

# 容器到底能不能打到这个地址，**在下游开跑之前从一个一次性容器里真打一次**，不拿"历史上通过"当本轮前提。
# 这条门是链 D 里那发 `sym-w16-r2` 逼出来的：两臂探测分别拿到 16 字节与 0 字节、上游整轮只服务了 3 条请求，
# 也就是容器到宿主上游那一条路当时不通——若不先核就开跑，那份数据已经带着 502 上了盘，
# 红的只是事后那条"负载形状不同"。
# **这道门自己错过两次，两次都记在这里**：① 第一版把它放在上游启动**之前**，两个候选当然都"打不到"，
# 门正确地关了但关错了理由（已挪到上游起来之后）；② 第二版用 `--entrypoint wget`，而实测
# `nginx:1.25.4` 里 **没有 wget**（`command -v wget=无`、`curl=/usr/bin/curl`）——于是链 F 的四臂全被这个
# 假阴性关掉。第三版还试过一次壳镜像 + bash `/dev/tcp`：第一个候选回了 1089 字节（通路本身没问题），
# 第二个候选没路由，`exec 3<>` 一直挂着我只能掐掉。**现在的形状**：用被测那台镜像里真的有的 `curl`，
# 超时交给 curl 自己的 `-m`，不交给 shell 的重定向。
probe_from_container() { # 地址 → 被测镜像里的 curl 能不能拿回整份 BODY_BYTES
  local addr=$1 bytes
  bytes=$(timeout 45 docker run --rm --entrypoint curl "$IMAGE_NGINX" -s -m 8 -o /dev/null -w '%{size_download}' \
            "http://$addr:$UPSTREAM_PORT/bench" 2>/dev/null)
  [[ "${bytes:-0}" -ge "$BODY_BYTES" ]]
}
UP_ADDR=""
for cand in "$WSL_IP" "$HOSTGW_IP"; do
  [[ -n "$cand" ]] || continue
  if probe_from_container "$cand"; then
    UP_ADDR="$cand"
    note "容器→宿主上游通路实测：用 $cand（被测镜像里的 curl 回包 ≥ $BODY_BYTES 字节）"
    break
  fi
  note "候选 $cand：一次性容器里的 curl 拿不到 $BODY_BYTES 字节，换下一个候选"
done
[[ -n "$UP_ADDR" ]] || fatal "两个候选地址（WSL eth0=$WSL_IP、host.docker.internal=${HOSTGW_IP:-读不到}）从容器里都打不到上游：这一档的对称拓扑在此刻不成立——不换手段、不开跑"
note "**两臂都写同一个 $UP_ADDR**：这就是这一档与 B-005 那两档的唯一差别所在"

# ---------- 负载形状核对：两侧必须回同样长度的体，否则比的是两个负载 ----------
check_workload() { # 标签 url
  local tag=$1 url=$2 bytes
  bytes=$(curl -s --max-time 10 -o "$OUT/probe-$tag.bin" -w '%{size_download}' "$url")
  if [[ "$bytes" != "$BODY_BYTES" ]]; then
    echo "  ✗ [$tag] 响应 $bytes 字节，期望 $BODY_BYTES：负载形状不同，比值无意义" >&2
    return 1
  fi
  ok "[$tag] 负载形状 $bytes == $BODY_BYTES 字节"
}

run_wrk() { # 标签 url —— QPS 到 stdout；失败 return 1（fatal 在命令替换的子 shell 里传不出来，B-005 那条教训）
  local tag=$1 url=$2
  wrk -t"$WRK_THREADS" -c"$CONNECTIONS" -d"$WARMUP" "$url" > "$OUT/$tag.warmup.txt" 2>&1
  wrk -t"$WRK_THREADS" -c"$CONNECTIONS" -d"$DURATION" --latency "$url" > "$OUT/$tag.wrk.txt" 2>&1
  local non2xx qps
  non2xx=$(sed -n 's/.*Non-2xx or 3xx responses: \([0-9]*\).*/\1/p' "$OUT/$tag.wrk.txt")
  qps=$(sed -n 's/^Requests\/sec:[[:space:]]*\([0-9.]*\).*/\1/p' "$OUT/$tag.wrk.txt" | head -1)
  [[ -n "$qps" ]] || { echo "  ✗ [$tag] Requests/sec 没解析出来：版式变了，看 $OUT/$tag.wrk.txt" >&2; return 1; }
  if [[ -n "$non2xx" ]]; then
    echo "  ✗ [$tag] wrk 报了 $non2xx 条非 2xx/3xx：这一档的负载形状不成立" >&2
    return 1
  fi
  echo "$qps"
}

# ---------- 归因取数：wrk 的 socket 超时与非 2xx 各自落一行 ----------
# 为什么加这一格（S5-08）：链 G/H 那几发里有三发 gw 臂报过 `timeout` 与非 2xx，而当时的
# summary 只留了「QPS=?」，条数与「有没有到达网关」这件事一起丢了，于是那笔账至今没归因
# （TASKS「已知待决事项」第三条 ①）。这里只**多记一行读数**，负载参数一字不动，
# 所以与已入表的那五发仍然同轴。
# 约定（§25.2 第 2 条那一族：读回空与"确实没发生"在数上不可区分）：
#   wrk 只在**有条数**时才打 `Socket errors:` / `Non-2xx` 这两行，所以"行不在"记 0；
#   而"文件根本是空的"（那一臂没跑到终局）记 `?`，绝不冒充 0。
wrk_count() { # 文件 行首样式 sed 脚本 → 一个数，或 0 / ?
  local f=$1 expr=$2 line v
  [[ -s "$f" ]] || { printf '?'; return; }
  line=$(grep -m1 -E 'Socket errors:|Non-2xx or 3xx responses:' "$f" 2>/dev/null)
  v=$(sed -n "$expr" "$f" | head -1)
  if [[ -n "$v" ]]; then
    printf '%s' "$v"
  elif [[ -n "$line" ]]; then
    printf '?'          # 有相关行但版式变了：这是取数失灵，不是"没发生"
  else
    printf '0'          # 两类行都不在 = wrk 一条都没报
  fi
}

record_wrk_errors() { # 标签 —— 把 warmup 与正式档各自的 timeout / non-2xx 写成一行一键
  local tag=$1 f w_t w_n r_t r_n r_req
  for f in "$OUT/$tag.warmup.txt" "$OUT/$tag.wrk.txt"; do [[ -e "$f" ]] || : > "$f"; done
  w_t=$(wrk_count "$OUT/$tag.warmup.txt" 's/.*timeout \([0-9]*\).*/\1/p')
  w_n=$(wrk_count "$OUT/$tag.warmup.txt" 's/.*Non-2xx or 3xx responses: \([0-9]*\).*/\1/p')
  r_t=$(wrk_count "$OUT/$tag.wrk.txt" 's/.*timeout \([0-9]*\).*/\1/p')
  r_n=$(wrk_count "$OUT/$tag.wrk.txt" 's/.*Non-2xx or 3xx responses: \([0-9]*\).*/\1/p')
  r_req=$(sed -n 's/^[[:space:]]*\([0-9]*\) requests in .*/\1/p' "$OUT/$tag.wrk.txt" | head -1)
  {
    echo "arm=$tag"
    echo "warmup_socket_timeout=$w_t"
    echo "warmup_non2xx=$w_n"
    echo "run_socket_timeout=$r_t"
    echo "run_non2xx=$r_n"
    echo "run_requests=${r_req:-?}"
  } > "$OUT/$tag.errors.txt"
}

gw_exit_keys() { # 退出账文件 → 把归因要用的那几个键原样抄成一行一键（键名先核过存在于 src/net/stats.h）
  local f=$OUT/gateway.exit.log k v
  [[ -s "$f" ]] || { echo "gateway_exit_log=EMPTY"; return; }
  for k in requests_parsed responses_completed upstream_head_timeout upstream_connect_timeout \
           upstream_body_idle_timeout gateway_replies_sent gateway_replies_connect_failed \
           gateway_replies_all_ejected gateway_replies_over_capacity \
           downstream_rejected_over_capacity live_connections connections_constructed connections_destructed; do
    v=$(sed -n "s/.*\[serve\] $k=\([0-9-]*\).*/\1/p" "$f" | head -1)
    echo "${k}=${v:-键不在退出账里}"
  done
}

p99_ms() {
  awk '
    /^[ \t]*Latency Distribution[ \t]*$/ { inblock = 1; next }
    inblock && /^[ \t]*99%[ \t]+([0-9.]+)(m|s)/ {
      v = $2 + 0; unit = $2; if (unit ~ /ms$/) print v; else if (unit ~ /s$/) print v * 1000; else print "0"
      exit
    }
  ' "$1" 2>/dev/null | head -1
}

# ---------- 拓扑证据：在容器**还活着**的时候取，否则读到的不是被压的那一台 ----------
assert_same_topology() { # 容器名 标签 发布端口
  local c=$1 tag=$2 pub=$3
  local mode ports
  mode=$(docker inspect -f '{{.HostConfig.NetworkMode}}' "$c" 2>/dev/null) || {
    echo "  ✗ [$tag] inspect 读不到 $c" >&2; return 1; }
  # `docker port` 打的是 "80/tcp -> 127.0.0.1:18184" 这种**已展平**的形状。
  # 我第一版拿 `inspect` 的 `NetworkSettings.Ports` JSON 去匹配字面串 "127.0.0.1:18184"——
  # 那段 JSON 里是 {"80/tcp":[{"HostIp":"127.0.0.1","HostPort":"18184"}]}，**两个值分居两个键**，
  # 那个串永远不出现，于是四臂全在两秒内被判"拓扑证据不成立"。锚要锚结构或锚展平后的形态，不锚我脑子里的文本。
  ports=$(docker port "$c" 2>/dev/null)
  printf 'NetworkMode=%s\n' "$mode" > "$OUT/topology-$tag.mode.txt"
  printf '%s\n' "$ports" > "$OUT/topology-$tag.ports.txt"
  if [[ "$mode" == "host" ]]; then
    echo "  ✗ [$tag] NetworkMode=host：这台机器上不成立（ENVIRONMENT 5.1），而且两侧就不对称了" >&2
    return 1
  fi
  if ! printf '%s\n' "$ports" | grep -q "127.0.0.1:$pub"; then
    echo "  ✗ [$tag] docker port 的发布行里没有 127.0.0.1:$pub（实得：$(echo "$ports" | tr '\n' ' ')）：wrk 打的不是这个容器" >&2
    return 1
  fi
  ok "[$tag] NetworkMode=$mode、发布 127.0.0.1:$pub（与另一臂同一种形态）"
}

NGX_CONTAINER=""
GW_CONTAINER=""

start_nginx() {
  local conf="$OUT/conf/nginx-default.conf"
  {
    echo "upstream bench { server $UP_ADDR:$UPSTREAM_PORT; keepalive 1024; }"
    echo "server {"
    echo "  listen 80;"
    echo "  location / {"
    echo "    proxy_pass http://bench;"
    echo "    proxy_http_version 1.1;"
    echo '    proxy_set_header Connection "";'
    echo "  }"
    echo "}"
  } > "$conf"
  local c="l7gate-s5-07-nginx"
  docker rm -f "$c" >/dev/null 2>&1
  docker run -d --name "$c" -p "127.0.0.1:$NGX_PUB:80" \
    -v "$conf:/etc/nginx/conf.d/default.conf:ro" "$IMAGE_NGINX" >/dev/null 2>&1 \
    || fatal "nginx 容器起不来（$IMAGE_NGINX）"
  CONTAINERS+=("$c")
  if ! wait_http_200 "http://127.0.0.1:$NGX_PUB/bench" 30; then
    docker logs "$c" > "$OUT/nginx.docker.log" 2>&1
    fatal "nginx 30s 内没答 200，看 $OUT/nginx.docker.log"
  fi
  NGX_CONTAINER="$c"
  assert_same_topology "$c" ngx "$NGX_PUB" || fatal "nginx 那一侧的拓扑证据不成立：这一档作废"
  note "nginx 容器 $c 就绪：http://127.0.0.1:$NGX_PUB/bench"
}

start_gateway() {
  local c="l7gate-s5-07-gateway"
  docker rm -f "$c" >/dev/null 2>&1
  # nofile 必须抬：1000 下游 + 1000 回源 + 每 worker 的 epoll/eventfd，而容器默认 soft=1024
  # 会把这一档打成「第 1025 个 fd 起全失败」——那不是网关的吞吐，是容器的 fd 上限（B-005 档 3 的同族坑）。
  local need_fd=$(( CONNECTIONS * 2 + 512 + WORKERS * 2 ))
  docker run -d --name "$c" -p "127.0.0.1:$GW_PUB:$GW_CONTAINER_PORT" \
    --ulimit "nofile=${need_fd}:${need_fd}" \
    -v "$PWD/build/release:/app:ro" \
    "$IMAGE_GATEWAY" /app/l7gate --serve \
      --listen "0.0.0.0:$GW_CONTAINER_PORT" \
      --workers "$WORKERS" \
      --upstream "$UP_ADDR:$UPSTREAM_PORT" \
    >/dev/null 2>&1 || fatal "网关容器起不来（$IMAGE_GATEWAY；产物是挂进去的，先 cmake --build --preset release）"
  CONTAINERS+=("$c")
  local i=0
  while (( i < 300 )); do
    docker logs "$c" > "$OUT/gateway.container.log" 2>&1
    grep -q '\[serve\] 监听' "$OUT/gateway.container.log" && break
    [[ "$(docker inspect -f '{{.State.Running}}' "$c" 2>/dev/null)" == "true" ]] \
      || fatal "网关容器退出了，看 $OUT/gateway.container.log"
    sleep 0.1; ((i += 1))
  done
  grep -q '\[serve\] 监听' "$OUT/gateway.container.log" \
    || fatal "网关容器 30s 内没打印监听行，看 $OUT/gateway.container.log"
  # 路径签名：多 Reactor 才打 workers=、单 Reactor 打 max_connections=——请求的是什么就必须看到什么（s1-p999 同一条门）
  if (( WORKERS >= 2 )); then
    grep -q "\[serve\] 监听 .*workers=$WORKERS " "$OUT/gateway.container.log" \
      || fatal "请求 --workers $WORKERS，但网关横幅不是多 Reactor 路径：这一臂作废"
  else
    grep -q "\[serve\] 监听 .*max_connections=" "$OUT/gateway.container.log" \
      || fatal "请求 --workers 1，但网关横幅不是单 Reactor 路径：这一臂作废"
  fi
  GW_CONTAINER="$c"
  assert_same_topology "$c" gw "$GW_PUB" || fatal "网关那一侧的拓扑证据不成立：这一档作废"
  note "网关容器 $c 就绪：http://127.0.0.1:$GW_PUB/bench（workers=$WORKERS，路径签名与拓扑都核过）"
}

stop_container_with_final_log() { # 容器名 落盘名 —— 退场**之前**先把退出账取走
  local c=$1 name=$2
  [[ -n "$c" ]] || return 0
  # `docker rm -f` 是硬杀，网关的 `[serve]` 退出账就不会打出来（链 H 的 gw-first 那发因此只有 307 字节
  # 的启动横幅）。要拿到退出账必须先 `docker stop` 走完排水——这一档 1000 条在飞，给 25 s。
  docker logs "$c" > "$OUT/$name.pre-stop.log" 2>&1
  timeout 60 docker stop -t 25 "$c" >/dev/null 2>&1
  docker logs "$c" > "$OUT/$name.exit.log" 2>&1
  docker rm -f "$c" >/dev/null 2>&1
}

QPS_gw=""; P99_gw=""; QPS_ngx=""; P99_ngx=""
measure() { # 标签 url
  local tag=$1 url=$2 qps p99 wrk_rc=0
  check_workload "$tag" "$url" || { die "[$tag] 负载形状核对没过：不取它的 QPS"; return 1; }
  qps=$(run_wrk "$tag" "$url") || wrk_rc=$?
  # **先记条数再判作废**：这一档最需要的正是"被判红那发到底报了多少条 timeout / 非 2xx"，
  # 反过来写就会把要归因的那一发丢掉（链 G/H 那三发就是这么丢的）。
  record_wrk_errors "$tag"
  (( wrk_rc == 0 )) || { die "[$tag] wrk 这一臂没跑到终局：比值不成立"; return 1; }
  p99=$(p99_ms "$OUT/$tag.wrk.txt")
  # 动态变量名必须走 printf -v / declare：写成 `QPS_$tag="$qps"` 会被 bash 当成一条命令执行，
  # 自验那一跑就在屏幕上打出 `QPS_ngx=10263.85: command not found`——数取到了但进不了表，
  # 而脚本仍然继续往下跑并打出一张两个问号的脸。这是"仪器不是看不见，是打的是别人的账"的同族。
  printf -v "QPS_$tag" '%s' "$qps"
  printf -v "P99_$tag" '%s' "${p99:-}"
  note "[$tag] QPS=$qps p99=${p99:-?}ms"
  return 0
}

echo "== SLO #3 · 终判档：两侧都进容器（同一上游进程 / 同一台机 / 同一时间窗口 / 顺序 $ORDER）"
if [[ "$ORDER" == "gw-first" ]]; then
  start_gateway
  measure gw "http://127.0.0.1:$GW_PUB/bench" || true
  stop_container_with_final_log "$GW_CONTAINER" gateway
  CONTAINERS=("${CONTAINERS[@]/l7gate-s5-07-gateway/}")
  start_nginx
  measure ngx "http://127.0.0.1:$NGX_PUB/bench" || true
  stop_container_with_final_log "$NGX_CONTAINER" nginx
  CONTAINERS=("${CONTAINERS[@]/l7gate-s5-07-nginx/}")
else
  start_nginx
  measure ngx "http://127.0.0.1:$NGX_PUB/bench" || true
  stop_container_with_final_log "$NGX_CONTAINER" nginx
  CONTAINERS=("${CONTAINERS[@]/l7gate-s5-07-nginx/}")
  start_gateway
  measure gw "http://127.0.0.1:$GW_PUB/bench" || true
  stop_container_with_final_log "$GW_CONTAINER" gateway
  CONTAINERS=("${CONTAINERS[@]/l7gate-s5-07-gateway/}")
fi

# 上游的账：负载没被仪器丢掉（refused 非 0 就让这一档作废）
sleep 2
kill -TERM "$UP_PID" 2>/dev/null; wait "$UP_PID" 2>/dev/null; UP_PID=""
# 容器必须在"数残留"之前真的退掉：清理挂在 EXIT trap 上，而残留判据跑在 trap 之前——
# 自验那一跑因此报出 `残留=1`，红的不是没清，是我在自己的清理还没执行的时候去数它。
# 容器在测量收尾时就已带着退出账退场（stop_container_with_final_log）；这里不再补删，
# 因为"先 docker logs 再 rm"这个顺序错过一次，退出账就永久没了——链 G 那两发正是这样丢的。
UP_REFUSED=$(grep -m1 -oE 'refused_over_capacity=[0-9]+' "$OUT/upstream.log" | cut -d= -f2)
UP_ADDFAIL=$(grep -m1 -oE 'refused_addfail=[0-9]+' "$OUT/upstream.log" | cut -d= -f2)
UP_REQUESTS=$(grep -m1 -oE 'requests=[0-9]+' "$OUT/upstream.log" | cut -d= -f2)
if [[ "${UP_REFUSED:-x}" != "0" || "${UP_ADDFAIL:-x}" != "0" ]]; then
  die "压测上游丢了负载：容量满 ${UP_REFUSED:-?}、注册失败 ${UP_ADDFAIL:-?}——这两个 QPS 是「排队等仪器」的数，作废"
else
  ok "上游终局：requests=${UP_REQUESTS:-?} refused_over_capacity=0 refused_addfail=0"
fi

RATIO=""
if [[ -n "$QPS_gw" && -n "$QPS_ngx" ]]; then
  RATIO=$(awk -v g="$QPS_gw" -v n="$QPS_ngx" 'BEGIN{ if (n+0>0) printf "%.2f", 100.0*g/n }')
fi

REPORT="$OUT/summary.txt"
{
  echo "# SLO #3 · 终判档（s5-07-qps-symmetric.sh）canonic=$CANONIC —— $(date -Is)"
  echo "参数: body=${BODY_BYTES}B conns=${CONNECTIONS} duration=${DURATION} warmup=${WARMUP} wrk_threads=${WRK_THREADS} 网关 workers=${WORKERS} 顺序=${ORDER}"
  echo "镜像: nginx=$IMAGE_NGINX id=${NGX_IMAGE_ID}；gateway=$IMAGE_GATEWAY id=${GW_IMAGE_ID}（壳镜像，产物只读挂 $PWD/build/release）"
  echo "拓扑: 两侧都进容器、都 -p 发布到 127.0.0.1、上游是同一个原生进程 $UP_ADDR:$UPSTREAM_PORT（该地址由一次性容器实测选出）"
  echo "      网关 ${GW_CONTAINER:-未起} 发布 127.0.0.1:$GW_PUB:$GW_CONTAINER_PORT；nginx ${NGX_CONTAINER:-未起} 发布 127.0.0.1:$NGX_PUB:80"
  echo "声明: 这一档里**每一次请求两侧都各多一次端口发布（DNAT/userland proxy）+ 一次容器出向**，"
  echo "      所以它与档一（裸进程网关 vs 容器 nginx）不可混池比较；它对齐的是 B-005 批评里的网络路径那一维。"
  echo "      仍然不齐的一条是契约 #3 明写的「同一分配器记录」：nginx 用它镜像里的 libc/openssl，网关是宿主 glibc 2.43。"
  echo "网关臂形态: --upstream（空 filter 链）、不装 --config、不 arm --metrics-port、不 arm 热重载"
  echo
  printf '%-10s %14s %12s\n' 档 QPS p99_ms
  printf '%-10s %14s %12s\n' nginx "${QPS_ngx:-?}" "${P99_ngx:-?}"
  printf '%-10s %14s %12s\n' gateway "${QPS_gw:-?}" "${P99_gw:-?}"
  echo
  if [[ -n "$RATIO" ]]; then
    echo "比值 QPS_网关/QPS_nginx = ${RATIO}%（判定式 ≥60%；目标值一个字不动，禁令二）"
  else
    echo "比值：**无法计算**（某一臂的 QPS 没取到）——绝不写 0.00，0 在这个语境里等于「达标」"
  fi
  echo "上游终局: requests=${UP_REQUESTS:-?} refused_over_capacity=${UP_REFUSED:-?} refused_addfail=${UP_ADDFAIL:-?}"
  echo
  echo "每臂 wrk 条数（wrk 只在有条数时打那两行；? = 取数失灵，不是没发生）："
  printf '%-8s %10s %10s %10s %10s %14s\n' 臂 warmup_timeout warmup_non2xx run_timeout run_non2xx run_requests
  for e in gw ngx; do
    [[ -s "$OUT/$e.errors.txt" ]] || { printf '%-8s %10s\n' "$e" "没跑到取数这一格"; continue; }
    printf '%-8s %10s %10s %10s %10s %14s\n' "$e" \
      "$(sed -n 's/^warmup_socket_timeout=//p' "$OUT/$e.errors.txt")" \
      "$(sed -n 's/^warmup_non2xx=//p' "$OUT/$e.errors.txt")" \
      "$(sed -n 's/^run_socket_timeout=//p' "$OUT/$e.errors.txt")" \
      "$(sed -n 's/^run_non2xx=//p' "$OUT/$e.errors.txt")" \
      "$(sed -n 's/^run_requests=//p' "$OUT/$e.errors.txt")"
  done
  echo
  echo "网关退出账（stop 之前取过一次、排水之后再取一次；键名逐个来自 src/net/stats.h 的 put(...)）："
  gw_exit_keys > "$OUT/gateway.exit.keys.txt" 2>/dev/null
  sed 's/^/  /' "$OUT/gateway.exit.keys.txt"
  echo
  echo "原始输出: <档>.warmup.txt / <档>.wrk.txt / <档>.errors.txt / probe-<档>.bin / gateway.container.log / gateway.pre-stop.log / gateway.exit.log / gateway.exit.keys.txt / upstream.log / topology-<档>.{mode.txt,ports.txt}"
} > "$REPORT"
cat "$REPORT"

OTHERS_AFTER=$(others_count)
RESIDUE=$(residue_count)
echo
echo "== 清场：l7gate-s5-07- 残留=$RESIDUE；同机别人的容器 before=$OTHERS_BEFORE after=$OTHERS_AFTER"
[[ "$RESIDUE" == "0" ]] || die "有残留容器没被清掉（$RESIDUE 条）：清理在这一档里是判据不是礼貌"
[[ "$OTHERS_AFTER" == "$OTHERS_BEFORE" ]] || die "别人的容器条数变了（$OTHERS_BEFORE → $OTHERS_AFTER）：碰到同机别的项目了"

echo "== 结果落在 $REPORT"
if (( FAILED != 0 )); then
  echo "== SLO #3 终判档本轮有 ✗，上面的数字不要用"
fi
exit "$FAILED"
