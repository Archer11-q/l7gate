#!/usr/bin/env bash
# SLO #3（吞吐）的第一份 Nginx 基线。口径逐项抄自 docs/SLO.md 的「#3 测量条件」：
#   16 vCPU / 1 KB 响应体 / HTTP/1.1 keep-alive / 回显上游 / 同机 / 同绑核方案（未绑核）/
#   同一上游 / 同一时间窗口 / 同一分配器记录，判定式 QPS_网关 / QPS_nginx ≥ 60%。
# 与 s1-p99.sh 共用同一套 wrk 参数（BODY_BYTES/CONNECTIONS/DURATION/WARMUP/WRK_THREADS），
# 这样两份记录至少能对上"同样的负载形状"，不是各调各的口径。
#
# **分母取哪一个 nginx，我不挑对自己有利的那一档**：判定只用「原始态」（镜像默认
# worker_processes=auto → 本机 16 worker，access_log 按镜像默认开着）。另外两档
# （关 access_log、单 worker）是**归因用的对照组**，它们不进判定式——理由写在下面。
# 三档之间每次只差一个变量，所以 A→B  isolate 日志、B→C isolate worker 数：
#   A 原始态          worker auto(16) + access_log on
#   B 关日志          worker auto(16) + access_log off
#   C 单 worker 关日志  worker 1        + access_log off   ← 与我的单 Reactor 结构最近的一档
# 契约 #3 的附加产出要求把差值定位到「HTTP 解析 / 内存分配 / 连接管理 / 日志写盘 / 系统调用次数」
# 中的哪几项，这三档就是给「日志写盘」和「连接管理/并发分摊」两项准备的数；
# 「HTTP 解析」「内存分配」「syscall 次数」的证据不在这里，在 S5 的 perf 面（且 perf 未验证）。
#
# 一条必须写在前面的测量条件偏差：**nginx 在容器里，它到上游要走两跳 NAT**。
# 实测拓扑（本机取证，见 summary 的环境段）：
#   wrk(WSL) → 127.0.0.1:发布端口 → [Docker 端口发布/DNAT] → nginx 容器(bridge 172.17.0.x)
#            → [容器→WSL eth0] → 上游(l7gate_bench_upstream，绑 0.0.0.0)
#   wrk(WSL) → 127.0.0.1:网关端口 → l7gate(原生 WSL 进程) → 127.0.0.1 → 同一个上游
# 两侧的客户端都是回环，但 nginx 侧多了端口发布与容器出向这两跳，而这两跳**方向上是拖累 nginx 的**
# ——也就是说按这份数据算出来的比值对**我**偏乐观。我不靠调参数把它抹平（那等于改测量条件），
# 只把它记进 summary 并在这里写明方向。真正能消掉它的办法是同版本的原生 nginx，而契约 9.1/9.3
# 锁的是 `nginx:1.25.4` 这个**镜像**，apt 装的 nginx 版本不同、换它就是换基线口径（要 ADR），
# 所以这一条留给项目所有人裁决，我不动。
#
# 用法：scripts/bench/s2-qps-vs-nginx.sh
#       DURATION=5s WARMUP=1s scripts/bench/s2-qps-vs-nginx.sh   # 只验脚本本身，canonic=0，不进文档
# 输出：build/bench/s2-nginx-<stamp>/（每档 wrk 原始输出 + nginx 配置 + summary.txt）
# 退出码：0 = 五档全部跑完且每档 non-2xx=0；任何一档塌了就是非 0（判定不成立）。
set -uo pipefail
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1

BIN="build/release"
GATEWAY="$BIN/l7gate"
UPSTREAM="$BIN/tools/l7gate_bench_upstream"
BODY_BYTES="${BODY_BYTES:-1024}"
CONNECTIONS="${CONNECTIONS:-1000}"
DURATION="${DURATION:-60s}"
WARMUP="${WARMUP:-10s}"
WRK_THREADS="${WRK_THREADS:-8}"
GATEWAY_PORT="${GATEWAY_PORT:-18083}"
NGINX_PORT="${NGINX_PORT:-18084}"
UPSTREAM_PORT="${UPSTREAM_PORT:-18099}"
SETTLE="${SETTLE:-5}"
MAX_CLIENTS_NOTE=$((CONNECTIONS * 3 + 256))
IMAGE="nginx:1.25.4"                    # 契约 9.1/9.3 锁定的基线镜像 tag，中途不换
OUT="${OUT_DIR:-build/bench/s2-nginx-$(date +%Y%m%d-%H%M%S)}"
CONTAINER=""

CANONIC=1
[[ "$BODY_BYTES" == "1024" && "$CONNECTIONS" == "1000" && "$DURATION" == "60s" &&
   "$WARMUP" == "10s" && "$WRK_THREADS" == "8" ]] || CANONIC=0

mkdir -p "$OUT/conf" || exit 1
# 必须是绝对路径：`-v` 挂 host 路径时 Docker 不接受相对的（第一次跑就是栽在这里，
# 报的是"容器起不来"这种指向错误地方的错——真正的原因是 $OUT 是 build/… 相对路径）。
OUT=$(readlink -f "$OUT")
FAILED=0
die() { echo "✗ $*" >&2; FAILED=1; }
fatal() { echo "✗ $*" >&2; exit 1; }
note() { echo "  $*"; }

[[ "$CANONIC" == "1" ]] && note "口径：与 SLO #3 / s1-p99.sh 同一套（1KiB / 1000 并发 / 60s 稳态 / 10s 预热 / wrk 8 线程）" \
  || note "口径：**非标准**（body=$BODY_BYTES conns=$CONNECTIONS duration=$DURATION warmup=$WARMUP threads=$WRK_THREADS）——这份不能进 BENCHMARK.md 的判定"

[[ -x "$GATEWAY" ]] || fatal "找不到 $GATEWAY；先 cmake --build --preset release"
[[ -x "$UPSTREAM" ]] || fatal "找不到 $UPSTREAM；先 cmake --build --preset release"
command -v wrk >/dev/null || fatal "本机没有 wrk"
command -v docker >/dev/null || fatal "本机没有 docker：#3 的分母起不来"
docker image inspect "$IMAGE" >/dev/null 2>&1 || fatal "本机没有 $IMAGE 镜像（契约锁定的基线 tag，我不换）"

cleanup() {
  [[ -n "$CONTAINER" ]] && docker rm -f "$CONTAINER" >/dev/null 2>&1
  [[ -n "${UP_PID:-}" ]] && kill -TERM "$UP_PID" 2>/dev/null
  [[ -n "${G_PID:-}" ]] && kill -TERM "$G_PID" 2>/dev/null
  return 0
}
trap cleanup EXIT

wait_http_200() { # url [seconds]
  local url=$1 secs=${2:-25} i=0
  while (( i < secs * 5 )); do
    [[ "$(curl -s -o /dev/null -w '%{http_code}' --max-time 3 "$url" 2>/dev/null)" == "200" ]] && return 0
    sleep 0.2; ((i += 1))
  done
  return 1
}

start_upstream() { # 标签
  "$UPSTREAM" --listen "0.0.0.0:$UPSTREAM_PORT" --body-bytes "$BODY_BYTES" \
    --max-clients "$MAX_CLIENTS_NOTE" > "$OUT/upstream-$1.log" 2>&1 &
  UP_PID=$!
  wait_http_200 "http://127.0.0.1:$UPSTREAM_PORT/bench" 15 \
    || fatal "上游没起来（$1），看 $OUT/upstream-$1.log"
  note "$1: 上游 pid=$UP_PID port=$UPSTREAM_PORT（绑 0.0.0.0，容器与网关打的是同一个进程）"
}
stop_upstream() { kill -TERM "$UP_PID" 2>/dev/null; wait "$UP_PID" 2>/dev/null; UP_PID=""; }

# nginx 的一份 server 配置。三种变体只差一个变量，所以配置文本共用一个模板。
#
# `keepalive 128` 是我第一版的写法，它把档 3 毁了（两遍都毁：run1 报 5622 条非 2xx、
# run2 抬了 worker_connections 之后反而 11795 条 + 10326 次 timeout）。原因是这个池
# **按 worker 计**：16 worker 时总池 16×128=2048 ≥ 1000 并发，看不出问题；降到 1 worker
# 只剩 128 个上游空闲连接，1000 条在飞请求只能疯狂建断上游连接，测出来的就不是
# "单 worker 的吞吐"而是"上游连接池被我自己掐死"。所以抬到 1024（≥ CONNECTIONS），
# 并且**所有档共用同一份文本**——档 1/2 的总池因此从 2048 变成 16384，那是超配但无害，
# 而"只差一个变量"这条纪律只有靠共用文本才真的守得住。
write_nginx_conf() { # 文件名 是否关日志(yes/no)
  local file="$OUT/conf/$1.conf" log_off=$2
  {
    echo "upstream bench { server \$(WSL_IP):$UPSTREAM_PORT; keepalive 1024; }"
    echo "server {"
    echo "  listen 80;"
    [[ "$log_off" == "yes" ]] && echo "  access_log off;"
    echo "  location / {"
    echo "    proxy_pass http://bench;"
    echo "    proxy_http_version 1.1;"
    echo '    proxy_set_header Connection "";'
    echo "  }"
    echo "}"
  } > "$file"
}

start_nginx() { # 标签 配置 [单worker=yes]
  local tag=$1 conf=$2 single=${3:-no}
  local wsl_ip
  wsl_ip=$(hostname -I | awk '{print $1}')
  [[ -n "$wsl_ip" ]] || fatal "拿不到 WSL 的 eth0 地址，容器够不到上游"
  local runtime_conf="$OUT/conf/$tag.runtime.conf"
  sed "s/\$(WSL_IP)/$wsl_ip/" "$conf" > "$runtime_conf"
  CONTAINER="l7gate-bench-nginx-$tag"
  docker rm -f "$CONTAINER" >/dev/null 2>&1
  if [[ "$single" == "yes" ]]; then
    # 为什么不是 `nginx -g "worker_processes 1;"`：第一版就是这么写的，容器直接起不来——
    # 镜像自带的 /etc/nginx/nginx.conf 里已经有 `worker_processes auto;`，`-g` 注入的
    # 同一条指令是**重复指令**，nginx 以 `[emerg] "worker_processes" directive is duplicate`
    # 退出。所以这里改成把镜像那份 main conf 原样取出来、只 sed 掉一行再挂回去：
    # 这样档 3 与档 2 的差**字面上就是一行**，而我不用手写一份 main conf（手写的会悄悄
    # 改掉别的默认值，比如 keepalive_timeout 与日志格式，那就不再是"只差一个变量"）。
    local main_conf="$OUT/conf/$tag.main.conf"
    docker run --rm --entrypoint cat "$IMAGE" /etc/nginx/nginx.conf > "$main_conf" \
      || fatal "取镜像自带的 nginx.conf 失败（$tag）"
    grep -q "^worker_processes" "$main_conf" || fatal "$main_conf 里没有 worker_processes 那一行，sed 前提不成立"
    # 第二处改动是被连接上限逼出来的，不是我想顺手调参：镜像默认 events{ worker_connections 1024 }，
    # 16 worker 时总接受容量 = 16×1024 = 16384，而 1 worker 只有 1024。第一版档 3 只改 worker 数，
    # 结果 wrk 报了 5622 条非 2xx、p99 1860 ms——它测到的是"nginx 被自己的连接上限打死"，
    # 不是"单 worker 的吞吐"，那个数拿来跟我的网关比毫无意义。所以把这一格抬到 16384，
    # 让**总接受容量与档 1/2 相等**，差的那一维就只剩 worker 数本身。两处改动都印出来。
    local wc_default
    wc_default=$(sed -n 's/.*worker_connections[[:space:]]\+\([0-9]\+\).*/\1/p' "$main_conf" | head -1)
    sed -i 's/^worker_processes.*/worker_processes 1;/' "$main_conf"
    sed -i 's/worker_connections.*/worker_connections 16384;/' "$main_conf"
    note "$tag: 镜像默认 worker_connections=$wc_default → 抬到 16384（对齐 16×$wc_default 的总容量）；worker_processes → 1"
    note "$tag: 主配置副本与镜像原文件的差异 = $(diff <(docker run --rm --entrypoint cat "$IMAGE" /etc/nginx/nginx.conf) "$main_conf" | grep -c '^[<>]') 行（两条指令）"
    docker run -d --name "$CONTAINER" -p "$NGINX_PORT:80" \
      -v "$runtime_conf:/etc/nginx/conf.d/default.conf:ro" \
      -v "$main_conf:/etc/nginx/nginx.conf:ro" "$IMAGE" >/dev/null 2>&1 \
      || fatal "nginx 容器起不来（$tag）"
  else
    docker run -d --name "$CONTAINER" -p "$NGINX_PORT:80" \
      -v "$runtime_conf:/etc/nginx/conf.d/default.conf:ro" "$IMAGE" >/dev/null 2>&1 \
      || fatal "nginx 容器起不来（$tag）"
  fi
  wait_http_200 "http://127.0.0.1:$NGINX_PORT/bench" 30 || {
    docker logs "$CONTAINER" 2>&1 | tail -5 > "$OUT/$tag.docker.log"
    fatal "nginx（$tag）在 30s 内没答 200，看 $OUT/$tag.docker.log"
  }
  note "$tag: 容器 $CONTAINER 就绪，wrk 打 http://127.0.0.1:$NGINX_PORT/bench"
}
stop_nginx() { [[ -n "$CONTAINER" ]] && docker rm -f "$CONTAINER" >/dev/null 2>&1; CONTAINER=""; }

start_gateway() { # 标签
  "$GATEWAY" --serve --listen "127.0.0.1:$GATEWAY_PORT" --upstream "127.0.0.1:$UPSTREAM_PORT" \
    > "$OUT/gateway-$1.log" 2>&1 &
  G_PID=$!
  wait_for_line() { local pid=$1 log=$2 pat=$3 i=0
    while (( i < 300 )); do grep -q -- "$pat" "$log" 2>/dev/null && return 0
      kill -0 "$pid" 2>/dev/null || return 1; sleep 0.1; ((i += 1)); done; return 1; }
  wait_for_line "$G_PID" "$OUT/gateway-$1.log" '\[serve\] 监听' || fatal "网关没起来（$1）"
  note "$1: 网关 pid=$G_PID port=$GATEWAY_PORT"
}

# 每档跑一遍：预热一遍（结果丢掉）+ 正式一遍（存盘）。取数方式与 s1-p99.sh 完全一致
# （wrk 的 Latency Distribution 整行缩进两个空格那次教训已经固化在锚里）。
#
# **失败必须能从函数里跑出来**：第一版这里直接 fatal()，而调用形式是 `QPS=$(run_wrk …)`——
# 命令替换跑在子 shell 里，`exit` 只结束子 shell，主脚本照旧往下跑，最后退出码还是 0。
# 于是"档 3 崩了 5622 条非 2xx"这件事在一份 exit 0 的报告里过去了，那正是
# 契约说的"一片 0 比一片 ? 危险"的同一种假绿。现在改成：函数只 return 1 并把原因写 stderr，
# 每个调用点自己 `|| fatal`，abort 发生真正需要它发生的那一层。
run_wrk() { # 标签 url  —— 成功时把 QPS 写到 stdout，失败 return 1
  local tag=$1 url=$2
  wrk -t"$WRK_THREADS" -c"$CONNECTIONS" -d"$WARMUP" "$url" > "$OUT/$tag.warmup.txt" 2>&1
  wrk -t"$WRK_THREADS" -c"$CONNECTIONS" -d"$DURATION" --latency "$url" > "$OUT/$tag.wrk.txt" 2>&1
  local non2xx qps
  non2xx=$(sed -n 's/.*Non-2xx or 3xx responses: \([0-9]*\).*/\1/p' "$OUT/$tag.wrk.txt")
  qps=$(sed -n 's/^Requests\/sec:[[:space:]]*\([0-9.]*\).*/\1/p' "$OUT/$tag.wrk.txt" | head -1)
  if [[ -z "$qps" ]]; then
    echo "✗ [$tag] wrk 的 Requests/sec 没解析出来：版式变了，看 $OUT/$tag.wrk.txt" >&2
    return 1
  fi
  # 每档都必须 non-2xx = 0：任何一档在报错，它的 QPS 就是"更快的失败"，不能进比。
  if [[ -n "$non2xx" ]]; then
    echo "✗ [$tag] wrk 报了 $non2xx 条非 2xx/3xx：这一档的负载形状不成立" >&2
    return 1
  fi
  echo "$qps"
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

# 负载形状核对：三条路径必须回同样长度的体。这一步不是形式主义——如果 nginx 的
# proxy 配置把响应改了（比如换成它自己的 404 页），我比的就不是同一个负载。
check_workload() { # 标签 url
  local tag=$1 url=$2 bytes
  bytes=$(curl -s --max-time 10 -o "$OUT/probe-$tag.bin" -w '%{size_download}' "$url")
  [[ "$bytes" == "$BODY_BYTES" ]] || fatal "[$tag] 响应 $bytes 字节，期望 $BODY_BYTES：负载形状不同，比值无意义"
  note "$tag: 单请求核对通过（$bytes 字节）"
}

note "环境：nproc=$(nproc)  docker_ncpu=$(docker info --format '{{.NCPU}}')  docker_mem=$(docker info --format '{{.MemTotal}}')"
note "镜像：$IMAGE  id=$(docker image inspect "$IMAGE" --format '{{.Id}}' | cut -c1-19)…"
note "nginx 版本：$(docker run --rm --entrypoint nginx "$IMAGE" -v 2>&1 | head -1)"

# ---------------------------------------------------------------- 档 1：nginx 原始态（判定分母）
echo "== 档 1：nginx 原始态（worker auto=16、access_log 按镜像默认开着）—— 判定式用的就是这个数"
write_nginx_conf proxy-log no
start_upstream nginx-stock
start_nginx nginx-stock "$OUT/conf/proxy-log.conf" ""
check_workload nginx-stock "http://127.0.0.1:$NGINX_PORT/bench"
QPS_STOCK=$(run_wrk nginx-stock "http://127.0.0.1:$NGINX_PORT/bench") \
  || fatal "档 1（nginx 原始态）没跑成：判定分母不存在，这一份没有资格进文档"
P99_STOCK=$(p99_ms "$OUT/nginx-stock.wrk.txt")
note "nginx 原始态：QPS=$QPS_STOCK  p99=${P99_STOCK:-?} ms"
stop_nginx; stop_upstream

# ---------------------------------------------------------------- 档 2：关 access_log
echo "== 档 2：nginx 关掉 access_log（与档 1 只差这一个变量 → isolate「日志写盘」）"
write_nginx_conf proxy-nolog yes
start_upstream nginx-nolog
start_nginx nginx-nolog "$OUT/conf/proxy-nolog.conf" ""
check_workload nginx-nolog "http://127.0.0.1:$NGINX_PORT/bench"
QPS_NOLOG=$(run_wrk nginx-nolog "http://127.0.0.1:$NGINX_PORT/bench") \
  || fatal "档 2（nginx 关 access_log）没跑成"
P99_NOLOG=$(p99_ms "$OUT/nginx-nolog.wrk.txt")
note "nginx 关日志：QPS=$QPS_NOLOG  p99=${P99_NOLOG:-?} ms"
stop_nginx; stop_upstream

# ---------------------------------------------------------------- 档 3：单 worker + 关日志
echo "== 档 3：nginx 单 worker + 关日志（与档 2 只差 worker 数 → isolate「并发分摊」，也是与我单 Reactor 结构最近的一档）"
start_upstream nginx-1w
start_nginx nginx-1w "$OUT/conf/proxy-nolog.conf" yes
check_workload nginx-1w "http://127.0.0.1:$NGINX_PORT/bench"
QPS_1W=$(run_wrk nginx-1w "http://127.0.0.1:$NGINX_PORT/bench") \
  || fatal "档 3（nginx 单 worker）没跑成"
P99_1W=$(p99_ms "$OUT/nginx-1w.wrk.txt")
note "nginx 单 worker：QPS=$QPS_1W  p99=${P99_1W:-?} ms"
stop_nginx; stop_upstream

# ---------------------------------------------------------------- 档 4：l7gate
echo "== 档 4：l7gate（被测者本人，同一个上游、同一套 wrk 参数）"
start_upstream gateway
start_gateway gateway
check_workload gateway "http://127.0.0.1:$GATEWAY_PORT/bench"
QPS_GATEWAY=$(run_wrk gateway "http://127.0.0.1:$GATEWAY_PORT/bench") \
  || fatal "档 4（l7gate）没跑成：被测者本人没数，比值无从谈起"
P99_GATEWAY=$(p99_ms "$OUT/gateway.wrk.txt")
note "l7gate：QPS=$QPS_GATEWAY  p99=${P99_GATEWAY:-?} ms"
# 工具退场后静置再停：网关的停止语义不排水在飞连接，静置就是让生命周期读数反映网关自己
# （同 s1-p99.sh 那次的教训：不静置的话报告里是"我杀进程的时机"）。
note "静置 ${SETTLE}s 后 SIGTERM"
sleep "$SETTLE"
kill -TERM "$G_PID" 2>/dev/null
for _ in $(seq 1 200); do kill -0 "$G_PID" 2>/dev/null || break; sleep 0.1; done
if kill -0 "$G_PID" 2>/dev/null; then kill -KILL "$G_PID" 2>/dev/null; fatal "网关 SIGTERM 后 20s 没停"; fi
wait "$G_PID" 2>/dev/null; G_RC=$?
(( G_RC == 0 )) || fatal "网关退出码 $G_RC：优雅停止没生效"
G_LOG="$OUT/gateway-gateway.log"
get_stat() { sed -n "s/^\[serve\] $1=\([0-9-]*\).*/\1/p" "$G_LOG" | head -1; }
[[ "$(get_stat live_connections)" == "0" ]] || fatal "live_connections=$(get_stat live_connections) 不为 0：生命周期没闭合，这一档没有资格进文档"
[[ "$(get_stat fd_start)" == "$(get_stat fd_end)" ]] || fatal "fd $(get_stat fd_start)→$(get_stat fd_end) 不等：有 fd 没关"
[[ "$(get_stat connections_constructed)" == "$(get_stat connections_destructed)" ]] \
  || fatal "构造 $(get_stat connections_constructed) / 析构 $(get_stat connections_destructed) 不等"
note "网关生命周期：live=0、fd $(get_stat fd_start)→$(get_stat fd_end)、constructed==destructed=$(get_stat connections_constructed)、requests_parsed=$(get_stat requests_parsed)"
stop_upstream

# ---------------------------------------------------------------- 档 5：直连上游（天花板对照）
echo "== 档 5：直连同一个上游（不是判定项，只用来说明 nginx 与网关各自离上游天花板多远）"
start_upstream direct
check_workload direct "http://127.0.0.1:$UPSTREAM_PORT/bench"
QPS_DIRECT=$(run_wrk direct "http://127.0.0.1:$UPSTREAM_PORT/bench") \
  || fatal "档 5（直连上游天花板对照）没跑成"
P99_DIRECT=$(p99_ms "$OUT/direct.wrk.txt")
note "直连上游：QPS=$QPS_DIRECT  p99=${P99_DIRECT:-?} ms"
stop_upstream

# ---------------------------------------------------------------- 汇总
RATIO_STOCK=$(awk -v a="$QPS_GATEWAY" -v b="$QPS_STOCK" 'BEGIN{if (b>0) printf "%.2f", 100*a/b; else print "?"}')
RATIO_1W=$(awk -v a="$QPS_GATEWAY" -v b="$QPS_1W" 'BEGIN{if (b>0) printf "%.2f", 100*a/b; else print "?"}')
JUDGE=$(awk -v r="$RATIO_STOCK" 'BEGIN{print (r+0 >= 60) ? "达标" : "未达标"}')

{
  echo "# SLO #3 吞吐 vs Nginx —— $(date -Iseconds)"
  echo "口径 canonic=$CANONIC  preset=release  body=${BODY_BYTES}B  conns=${CONNECTIONS}  duration=${DURATION}  warmup=${WARMUP}  wrk_threads=${WRK_THREADS}"
  echo "工具: $(wrk --version 2>&1 | head -1)"
  echo "基线镜像: $IMAGE  id=$(docker image inspect "$IMAGE" --format '{{.Id}}')"
  echo "机器: nproc=$(nproc)  docker_ncpu=$(docker info --format '{{.NCPU}}')  docker_mem=$(docker info --format '{{.MemTotal}}')  kernel=$(uname -r)"
  echo "绑核：未做（与 s1-p99.sh 一致；cpuset 方案按 ENVIRONMENT.md 第 7 节留到 S4）"
  echo "分配器：$(ldd "$GATEWAY" | grep -m1 'libc.so' | awk '{print $1}')（nginx 容器内是它自己的 libc，见下面偏差声明）"
  echo
  echo "## 结果（QPS 越高越好；p99 只是同场记录，判定只看 QPS 比值）"
  echo "档                                        QPS          p99ms"
  printf '  1 nginx 原始态（判定分母）            %-12s %s\n' "$QPS_STOCK"   "${P99_STOCK:-?}"
  printf '  2 nginx 关 access_log（对照）         %-12s %s\n' "$QPS_NOLOG"   "${P99_NOLOG:-?}"
  printf '  3 nginx 单 worker + 关日志（对照）    %-12s %s\n' "$QPS_1W"      "${P99_1W:-?}"
  printf '  4 l7gate（被测）                      %-12s %s\n' "$QPS_GATEWAY" "${P99_GATEWAY:-?}"
  printf '  5 直连同一个上游（天花板对照）        %-12s %s\n' "$QPS_DIRECT"  "${P99_DIRECT:-?}"
  echo
  echo "判定：QPS_网关 / QPS_nginx(原始态) = ${RATIO_STOCK}% ≥ 60% ? → **$JUDGE**"
  echo "     （只按契约写死的分母判。档 2/3/5 是对照组，不参与判定式。）"
  echo "参考比值：网关 / nginx 单 worker = ${RATIO_1W}%"
  echo "网关自报：requests_parsed=$(get_stat requests_parsed)  responses_completed=$(get_stat responses_completed)  epoll_modifications=$(get_stat epoll_modifications)"
  echo
  echo "## 必须一起读的三条偏差（少任何一条，上面那个比值就会被读成它没说的意思）"
  echo "一、nginx 在容器里：客户端侧同样是回环，但它的上游 leg 要经端口发布与容器出向两跳 NAT，"
  echo "    方向上**拖累 nginx**，所以这个比值对我偏乐观。消掉它的唯一办法是同版本原生 nginx，"
  echo "    而契约锁的是 $IMAGE 这个镜像 tag，apt 的版本不同 → 属换基线口径，待裁决，我不动。"
  echo "二、worker_processes：档 1/2 是镜像默认的 auto（本机 16 worker），而 S1 的网关按 ADR-03"
  echo "    就是**单 Reactor**——多线程属 S2。所以档 1 与档 4 比的不是「同样的连接模型」，"
  echo "    是「单线程 S1 现状 vs 多线程 nginx 现状」。档 3（单 worker）才是把这一维对齐后的读数。"
  echo "三、分配器与 libc：nginx 容器用的是它自己镜像里的 libc/openssl，网关是 glibc 2.43。"
  echo "    SLO #3 要求「同一分配器记录」这一条**没有满足**，已按实际情形记在这里而不是假装一致。"
  echo
  echo "原始输出：nginx-stock.wrk.txt / nginx-nolog.wrk.txt / nginx-1w.wrk.txt / gateway.wrk.txt / direct.wrk.txt"
  echo "          conf/*.runtime.conf（实际挂进容器的配置）/ gateway-gateway.log / upstream-*.log"
} | tee "$OUT/s2-qps-vs-nginx.summary.txt"

echo
if (( FAILED == 0 )); then
  echo "== SLO #3 第一份基线取完：判定 $JUDGE（比值 ${RATIO_STOCK}%），证据在 $OUT/"
else
  echo "== SLO #3 基线未取完：上面带 ✗ 的每一项都是没达标的项目，不是警告"
fi
exit "$FAILED"
