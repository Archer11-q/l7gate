#!/usr/bin/env bash
# S5-03 冒烟：Prometheus 抓取 + Grafana「网关总览」六格，在真进程 + 真容器里成立到什么程度。
#
# ctest 那一条都碰不到这里要证的东西：它没有 Prometheus、没有 Grafana、也没有一台真被摘除的后端。
# 这个脚本管四件事，正好对着 S5-03 的验收四条：
#   一、六格**逐格非空**——而且判据用的表达式是从 `deploy/grafana/dashboards/gateway-overview.json`
#        里 grep 出来的**同一串**，不另写一份查询（另写一份 = 面板与断言分家，ADR-25 第八节 E2 那个形状）；
#   二、429 那一格与网关自报的 `rate_limit_rejected` **逐数对得上**（仪器与被测方互背书，同 §15.1 那条
#        等式的形状）——等式两边一个来自 Prometheus、一个来自我直接抓网关的 /metrics，取数路径互相独立；
#   三、熔断把后端摘掉这件事**面板看得见**：`health_ejected_now` 与 `circuit_half_open_now` 的跳变
#        由网关自己的观测面先证「确实发生过」，再要求 Prometheus 里也留有那一次跳变的样本；
#   四、分位那一格的口径标签齐（`canonic`/`conns`/`observer_cores` + 面板文案里那句 ≈128 µs/请求）。
#
# 另外两条是这个脚本自己的守门人，不是验收条：
#   五、**抓取不进被测方**（ADR-24 那条立场在容器这一层再证一遍）：Prometheus 抓了 2 分钟之后，
#         `requests_parsed` 的增量恰好等于我自己发的 curl 条数，而 `metrics_scrapes_ok` 自己在涨；
#   六、**缺字段就不发那一格**（ADR-25 第三节第 1 条）：一份故意截断的摘要，它那一族 series 必须不出现。
#
# 阶段：
#   0 前置（二进制 / Docker 引擎 / 两个契约 tag 以 inspect 为准 / 端口空闲）
#   1 起两台上游 + 网关（health+circuit+rate_limit 低阈值，`--metrics-port`，**探活关**：fd 判据走逐数相等那一档）
#   2 基线一批请求 → `responses_completed` 有下界
#   3 超速率突发 → `rate_limit_rejected` ≥1（429 确实发生过）
#   4 杀一台上游 → 摘除与半开的跳变（网关侧逐秒采样，先有 0 再有 ≥1）
#   5 exporter：--once 与盘上摘要逐数对得上 + 截断摘要那一族不发 + 起服务
#        （fixture 取 `build/bench/` 下**已有的真实落盘摘要**——`build/` 不入库，全新克隆里
#         没有它，这一阶段会当场 `die` 而不是静默换成我手写的假数：要跑这条冒烟，先跑一轮压测。）
#   6 Prometheus：两个 target 都 up
#   7 Grafana：数据源与面板都入库
#   8 六格逐格非空（表达式取自 dashboard.json 本身）
#   9 429 与 `rate_limit_rejected` 逐数对得上（静置一次 + 扰动一次，两轮都对）
#  10 面板文案里那四句措辞限定（入库靠断言，不靠我记得）
#  11 抓取不进被测方（requests_parsed 的增量恰好等于我发的条数）
#  12 排水与生命周期：fd 回基线、live 归零、constructed==destructed、关闭态没有 config_ 键
#
# 用法：PRESET=debug scripts/smoke-s5-03.sh
# 输出：build/smoke-s5-03/ 里的日志，判定同时写进 **build/smoke-s5-03/verdict.txt**
#   （`PASS|FAIL checks=<n> fails=<n>`）。读判定以 verdict.txt 与日志里的 ✗ 计数为准：
#   本脚本在「全跑 + 真容器 + 真子进程」的形状下退出码读成 0 而 FAILED=1（十一组最小复现全部 rc=1，
#   我没能归因到具体那一句；差异本身记在 FAULT-INJECTION §22）。**不许把 rc=0 当成通过**。
set -uo pipefail
cd "$(dirname "$(readlink -f "$0")")/.." || exit 1
trap '' PIPE

PRESET="${PRESET:-debug}"
BIN="build/$PRESET"
GATEWAY="$BIN/l7gate"
UPSTREAM_BIN="$BIN/tests/l7gate_test_upstream"
EXPORTER="$BIN/tools/l7gate_bench_exporter"
OUT="build/smoke-s5-03"
REPO_ABS="$(pwd)"
mkdir -p "$OUT"

PROM_IMG="prom/prometheus:v2.51.2"    # 契约 9.1 锁定，禁 latest
GRAF_IMG="grafana/grafana:10.4.2"
CTR_PROM="l7gate-s5-03-prometheus"    # 只碰这两个名字；同机 rgbt-*/knowstack-* 与本项目无关
CTR_GRAF="l7gate-s5-03-grafana"

G_PORT=18930          # 业务端口
U1_PORT=18931
U2_PORT=18932
M_PORT=18522          # 网关 --metrics-port —— **必须与 deploy/prometheus/prometheus.yml 里写死的那个端口一致**，
                      # 否则冒烟测的是我自己临时改出来的那份抓取配置，不是入库的那份（第一轮就栽在这上面：
                      # 脚本用 18933、配置写 18522，两条 target 全 down，第 6–9 阶段一起连锁红）
E_PORT=18090          # exporter —— 同理，跟着入库配置走
PROM_HOST=19090       # -p 发布到 127.0.0.1
GRAF_HOST=13000
GRAF_PASS="l7gate-demo"
DASH_UID="l7gate-gateway-overview"

FAILED=0
FAILS=0
CHECKS=0
ok()   { echo "  ✓ $*"; CHECKS=$((CHECKS + 1)); }
fail() { echo "  ✗ $*"; FAILED=1; FAILS=$((FAILS + 1)); CHECKS=$((CHECKS + 1)); }
die()  { echo "  fatal: $*" >&2; exit 1; }

for b in "$GATEWAY" "$UPSTREAM_BIN" "$EXPORTER"; do
  [[ -x "$b" ]] || die "找不到 $b —— 先 cmake --build --preset $PRESET（全目标，口径⑪）"
done
command -v curl   >/dev/null || die "本机没有 curl"
command -v docker >/dev/null || die "本机没有 docker"

port_free() { ! (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; }
for p in "$G_PORT" "$U1_PORT" "$U2_PORT" "$M_PORT" "$E_PORT" "$PROM_HOST" "$GRAF_HOST"; do
  port_free "$p" || die "端口 $p 已被占用：先清场"
done

# ---------- 取数小工具（判定只认落盘，不认印象）----------
metric() { # key → 值（网关观测面直接抓；没有就空）
  # 留下取数通道的痕迹：阶段 9 的互背书等式要求两边来自**两条不同的通道**，
  # 否则「拿面板读数替代网关自报」这种改法在脚本里看不出来（ADR-25 第八节 E3）。
  echo "http://127.0.0.1:$M_PORT/metrics" > "$OUT/prov-gateway.txt"
  curl -s --max-time 3 "http://127.0.0.1:$M_PORT/metrics" 2>/dev/null |
    awk -v k="$1" '$1==k {print $2; exit}'
}
prom_query() { # expr → JSON（落盘一份，失败时能翻）
  local n=${2:-prom}
  echo "http://127.0.0.1:$PROM_HOST/api/v1/query" > "$OUT/prov-prometheus.txt"
  curl -s --max-time 15 --data-urlencode "query=$1" "http://127.0.0.1:$PROM_HOST/api/v1/query" \
    > "$OUT/$n.json" 2>/dev/null
  cat "$OUT/$n.json"
}
prom_series() { prom_query "$1" "$2" | grep -o '"metric":{' | wc -l | tr -d ' '; }
prom_value() { # 单 series 的 instant 值
  prom_query "$1" "$2" | grep -o '"value":\[[^]]*\]' | head -1 |
    sed 's/.*,"//; s/"\].*//'
}
wait_http() { # url [seconds]
  local url=$1 secs=${2:-60} i=0
  while (( i < secs * 2 )); do
    curl -s --max-time 2 -o /dev/null "$url" && return 0
    ((i++)); sleep 0.5
  done
  return 1
}
now_ms() { echo $(( $(date +%s%N) / 1000000 )); }   # 轴上的单位必须是毫秒（§21.2 第 2 条）
get_serve() { sed -n "s/^\[serve\] $1=\([0-9]*\).*/\1/p" "$OUT/gateway.log" | head -1; }

# ---------- 清理：只收自己建的东西 ----------
PIDS=()
cleanup() {
  for pid in "${PIDS[@]:-}"; do kill -TERM "$pid" 2>/dev/null; done
  docker rm -f "$CTR_PROM" "$CTR_GRAF" >/dev/null 2>&1
  wait 2>/dev/null
}
# 异常退出（die、被信号打断）时由 trap 兜底清场；正常路径在主体末尾**显式调 cleanup 并撤销 trap**，
# 这样 `exit "$FAILED"` 不再和 EXIT trap 的传染路径纠缠——本机实测这一版在全跑（有真子进程、真容器）
# 的形状下，无论把 exit 写在主体还是写在 trap 里，rc 都读成 0，而四个截取版与九个最小样本都是 1。
# 归因没找到，所以门禁不只读 rc：脚本同时把判定写进 build/smoke-s5-03/verdict.txt（只认落盘输出）。
trap cleanup EXIT

echo "== S5-03 冒烟：Prometheus 抓取 + Grafana 六格（PRESET=$PRESET）=="

# ---------------- 阶段 0：前置 ----------------
echo "-- 阶段 0：前置"
if ! docker info >/dev/null 2>&1; then
  echo "  引擎不在位：Docker Desktop / WSL Integration 没起来。本条依赖真容器，**拒跑**（不是通过）。"
  echo "  ✗ 计数 0、rc 非 0 —— 与 smoke-s3-04 / smoke-s4-04 同一种处置：引擎恢复后单跑补绿。"
  exit 1
fi
ok "docker 引擎在位"
for img in "$PROM_IMG" "$GRAF_IMG"; do
  if docker image inspect "$img" >/dev/null 2>&1; then
    ok "镜像在位（以 docker image inspect 为准，不以拉取日志为准）：$img"
  else
    fail "契约 9.1 锁的镜像不在位：$img"
  fi
done

# ---------------- 阶段 1：网关与两台上游 ----------------
echo "-- 阶段 1：起网关（health+circuit+rate_limit，探活关，--metrics-port $M_PORT）"
CFG="$OUT/gw.json"
cat > "$CFG" <<JSON
{
  "upstreams": [
    { "name": "api", "balance": "round_robin", "targets": [
      { "address": "127.0.0.1", "port": $U1_PORT },
      { "address": "127.0.0.1", "port": $U2_PORT } ] }
  ],
  "routes": [ { "match": "prefix", "pattern": "", "upstream": "api" } ],
  "health": { "max_fails": 2, "fail_timeout_ms": 1500, "probe_interval_ms": 0, "max_ejection_percent": 50 },
  "circuit": { "window_ms": 2000, "minimum_requests": 5, "error_rate_percent": 50,
               "half_open_requests": 2, "half_open_successes": 2, "half_open_timeout_ms": 8000 },
  "rate_limit": { "rate": 5, "burst": 5, "key": "path" }
}
JSON

"$UPSTREAM_BIN" --listen "127.0.0.1:$U1_PORT" --body-bytes 64 > "$OUT/up1.log" 2>&1 & U1=$!
"$UPSTREAM_BIN" --listen "127.0.0.1:$U2_PORT" --body-bytes 64 > "$OUT/up2.log" 2>&1 & U2=$!
PIDS+=("$U1" "$U2")
sleep 0.6

# 网关绑 0.0.0.0：观测端口要能被容器抓到（本会话实测的唯一通路是宿主端口 + host.docker.internal）。
"$GATEWAY" --serve --listen "0.0.0.0:$G_PORT" --config "$CFG" --metrics-port "$M_PORT" \
  > "$OUT/gateway.log" 2>&1 & GW=$!
PIDS+=("$GW")
wait_http "http://127.0.0.1:$G_PORT/healthz-probe" 15 >/dev/null || die "网关没起来，看 $OUT/gateway.log"
ok "网关在 $G_PORT（业务）+ $M_PORT（观测）"

if [[ -n "$(metric requests_parsed)" ]]; then
  ok "观测面可抓：requests_parsed=$(metric requests_parsed)"
else
  fail "观测面抓不到键 requests_parsed"
fi

# ---------------- 阶段 2：基线一批请求 ----------------
echo "-- 阶段 2：基线"
BUSINESS_CURLS=0
PARSED_BEFORE_CURLS=$(metric requests_parsed)   # 必须在第一批 curl 之前读（第一轮在之后读，把 8 条算了两遍）
base_before=$(metric responses_completed)
for i in $(seq 1 8); do curl -s --max-time 2 -o /dev/null "http://127.0.0.1:$G_PORT/a$i"; done
BUSINESS_CURLS=$((BUSINESS_CURLS + 8))
base_after=$(metric responses_completed)
if [[ -n "$base_before" && -n "$base_after" ]] && (( base_after > base_before )); then
  ok "responses_completed $base_before → $base_after（上界之外还要下界：确实有响应走完）"
else
  fail "responses_completed 没动：$base_before → $base_after"
fi
REJECTED_BASE=$(metric rate_limit_rejected)

# 限流键是 path（`rate_limit.key`），每条新路径是一只新桶（burst=5）。所以 429 必须打**同一条路径**：
# 40 条各用 unique path 的话每只桶都还没空，`rate_limit_rejected` 恒 0——第一轮就读了个 0 出来当"没发生"。
LIMITED="127.0.0.1:$G_PORT/limited"
echo "-- 阶段 3：超速率突发（配置 rate=5 burst=5，同一条路径 $LIMITED）"
burst_429=0
for i in $(seq 1 40); do
  code=$(curl -s --max-time 2 -o /dev/null -w '%{http_code}' "http://$LIMITED" 2>/dev/null)
  [[ "$code" == "429" ]] && burst_429=$((burst_429 + 1))
done
BUSINESS_CURLS=$((BUSINESS_CURLS + 40))
REJECTED_NOW=$(metric rate_limit_rejected)
if (( burst_429 > 0 )) && [[ -n "$REJECTED_NOW" ]] && (( REJECTED_NOW > REJECTED_BASE )); then
  ok "客户端侧数到 $burst_429 条 429，网关自报 rate_limit_rejected $REJECTED_BASE → $REJECTED_NOW"
else
  fail "429 没发生：客户端 $burst_429 条、网关 $REJECTED_BASE → $REJECTED_NOW"
fi

# ---------------- 阶段 4：把一台后端摘掉 ----------------
echo "-- 阶段 4：杀 U2($U2_PORT) → 摘除与半开的跳变"
e_pre=$(metric health_ejected_now)
kill -TERM "$U2" 2>/dev/null; wait "$U2" 2>/dev/null
for i in $(seq 1 12); do curl -s --max-time 2 -o /dev/null "http://127.0.0.1:$G_PORT/e$i" >/dev/null 2>&1; done
BUSINESS_CURLS=$((BUSINESS_CURLS + 12))
# 半开是**瞬时态**：一旦有业务请求撞上它，"一次失败立即回摘"会当场把它关回去（1.31 第三节）。
# 所以要看到 `circuit_half_open_now` 的跳变，得在摘除之后**停止发业务请求**、只读观测面：
# 到期由循环那一拍的 `sweep_expired` 推进，不需要流量；`half_open_timeout_ms=8000` 给它留出窗口。
ejected_max=0; halfopen_max=0
for i in $(seq 1 60); do
  e=$(metric health_ejected_now); h=$(metric circuit_half_open_now)
  e=${e:-0}; h=${h:-0}
  (( e > ejected_max )) && ejected_max=$e
  (( h > halfopen_max )) && halfopen_max=$h
  sleep 0.1
done
if [[ "$e_pre" == "0" ]]; then ok "杀之前 health_ejected_now=0（跳变的起点，跳变前的那一格先钉住）"
else fail "杀之前就读到 health_ejected_now=$e_pre，跳变的起点不成立"; fi
if (( ejected_max >= 1 )); then
  ok "health_ejected_now 跳到过 $ejected_max（后端真被摘掉，网关自己的观测面先证）"
else
  fail "health_ejected_now 全程 0：摘除没发生（U2 已 kill）"
fi
if (( halfopen_max >= 1 )); then
  ok "circuit_half_open_now 跳到过 $halfopen_max（到期→半开那条路走通了）"
else
  fail "circuit_half_open_now 全程 0：半开没被观测到（half_open_timeout_ms=8000 应至少跨过一个抓取节拍）"
fi
# 逐目标那一族键名里地址与端口都被换成下划线（ADR-24 第五节例外 3），这里按同一个变换取键。
EJECTIONS=$(metric "health_t1_127_0_0_1_${U2_PORT}_ejections")
if [[ -n "$EJECTIONS" ]] && (( EJECTIONS >= 1 )); then
  ok "逐目标键 health_t1_127_0_0_1_${U2_PORT}_ejections=$EJECTIONS（面板第 5 格读的就是这一族）"
else
  fail "逐目标摘除键读不到或为 0：$EJECTIONS"
fi

# ---------------- 阶段 5：exporter ----------------
echo "-- 阶段 5：l7gate_bench_exporter（只读落盘摘要）"
BROOT="$OUT/bench"
mkdir -p "$BROOT/fixture-full" "$BROOT/fixture-truncated"
SRC_HDR="build/bench/s5-01-metrics-cost-p999-cal20k/on-r1/s1-p999.summary.txt"
[[ -f "$SRC_HDR" ]] || SRC_HDR=$(ls build/bench/*/s1-p999.summary.txt 2>/dev/null | head -1)
[[ -n "${SRC_HDR:-}" && -f "$SRC_HDR" ]] || die "没有任何 s1-p999.summary.txt 可当 fixture（先跑一轮压测或从盘上找）"
cp "$SRC_HDR" "$BROOT/fixture-full/s1-p999.summary.txt"
# 故意截断：删掉那两行 `增量 pXXX =` —— 这一族对这批必须整个不出现（ADR-25 第三节第 1 条）。
grep -v "^增量 p" "$SRC_HDR" > "$BROOT/fixture-truncated/s1-p999.summary.txt" || true

"$EXPORTER" --root "$BROOT" --once > "$OUT/exporter-once.txt" 2>&1 || fail "exporter --once 退出码非 0"

# 逐数对盘上：从 fixture 里取 `增量 p999 = X ms`，换成 µs 后必须与 exporter 发的同一个数。
disk_ms=$(sed -n 's/^增量 p999 = \([0-9.]*\) ms.*/\1/p' "$BROOT/fixture-full/s1-p999.summary.txt" | head -1)
disk_us=$(awk -v m="$disk_ms" 'BEGIN{printf "%d", m*1000}')
exp_us=$(awk '$1 ~ /^l7gate_bench_increment_microseconds/ && $0 ~ /batch="fixture-full"/ && $0 ~ /quantile="p999"/ {print $2; exit}' "$OUT/exporter-once.txt")
if [[ -n "$disk_us" && "$disk_us" == "$exp_us" ]]; then
  ok "exporter 与盘上逐数对得上：增量 p999 = ${disk_ms} ms → ${disk_us} µs"
else
  fail "exporter 与盘上对不上：盘上 ${disk_ms} ms（=${disk_us} µs），exporter 发的是 '${exp_us:-空}'"
fi
if grep -q 'batch="fixture-truncated"' "$OUT/exporter-once.txt" &&
   ! awk '$1 ~ /^l7gate_bench_increment_microseconds/ && $0 ~ /batch="fixture-truncated"/ {found=1} END{exit found?0:1}' "$OUT/exporter-once.txt"; then
  ok "截断摘要：那批的其他格照发，增量那一族对它整个不出现（没有用 0 冒充）"
else
  fail "截断摘要那一族出现了，或者整批被丢掉——两种都是把「没读到」读成别的"
fi
"$EXPORTER" --root "$BROOT" --listen "0.0.0.0:$E_PORT" > "$OUT/exporter.log" 2>&1 & EX=$!
PIDS+=("$EX")
wait_http "http://127.0.0.1:$E_PORT/metrics" 10 >/dev/null || fail "exporter 没起来"
ok "exporter 在 $E_PORT（root=$BROOT，只有两批 fixture）"

# ---------------- 阶段 6：Prometheus ----------------
echo "-- 阶段 6：Prometheus（$PROM_IMG，发布 127.0.0.1:$PROM_HOST）"
docker rm -f "$CTR_PROM" >/dev/null 2>&1
PROM_START=$(date +%s)
docker run -d --name "$CTR_PROM" --add-host=host.docker.internal:host-gateway \
  -p "127.0.0.1:$PROM_HOST:9090" \
  -v "$REPO_ABS/deploy/prometheus/prometheus.yml:/etc/prometheus/prometheus.yml:ro" \
  "$PROM_IMG" --config.file=/etc/prometheus/prometheus.yml --storage.tsdb.path=/prometheus \
  > "$OUT/prometheus.run.log" 2>&1 || fail "起 Prometheus 失败"
wait_http "http://127.0.0.1:$PROM_HOST/-/ready" 90 >/dev/null || fail "Prometheus 没 ready"
# 给它两个抓取节拍，让两条 target 都变 up。
for i in $(seq 1 30); do
  up=$(curl -s --max-time 5 "http://127.0.0.1:$PROM_HOST/api/v1/targets" 2>/dev/null | grep -o '"health":"up"' | wc -l)
  (( up >= 2 )) && break
  sleep 2
done
up=$(curl -s --max-time 5 "http://127.0.0.1:$PROM_HOST/api/v1/targets" > "$OUT/targets.json" 2>/dev/null; grep -o '"health":"up"' "$OUT/targets.json" | wc -l)
if (( up >= 2 )); then
  ok "两条 target 都 up（网关观测面 + exporter）"
else
  fail "target up 只有 $up 条，看 $OUT/targets.json"
fi

# ---------------- 阶段 7：Grafana ----------------
echo "-- 阶段 7：Grafana（$GRAF_IMG，发布 127.0.0.1:$GRAF_HOST）"
docker rm -f "$CTR_GRAF" >/dev/null 2>&1
docker run -d --name "$CTR_GRAF" --add-host=host.docker.internal:host-gateway \
  -p "127.0.0.1:$GRAF_HOST:3000" \
  -e GF_SECURITY_ADMIN_PASSWORD="$GRAF_PASS" \
  -e GF_AUTH_ANONYMOUS_ENABLED=true -e GF_AUTH_ANONYMOUS_ORG_ROLE=Viewer \
  -e GF_ANALYTICS_REPORTING_ENABLED=false \
  -v "$REPO_ABS/deploy/grafana/provisioning:/etc/grafana/provisioning:ro" \
  -v "$REPO_ABS/deploy/grafana/dashboards:/var/lib/grafana/dashboards:ro" \
  "$GRAF_IMG" > "$OUT/grafana.run.log" 2>&1 || fail "起 Grafana 失败"
wait_http "http://127.0.0.1:$GRAF_HOST/api/health" 120 >/dev/null || fail "Grafana 没起来"
# provisioning 落在 /api/health 之后（第一轮就是栽在这：health 已经 200 而面板还没扫进来，
# 单发一次 API 读到 404 → 明明入库成功却判了红）。所以轮询到拿到 title 为止。
for i in $(seq 1 60); do
  curl -s --max-time 10 -u "admin:$GRAF_PASS" "http://127.0.0.1:$GRAF_HOST/api/dashboards/uid/$DASH_UID" \
    > "$OUT/grafana-dashboard.json" 2>/dev/null
  grep -q '"title"' "$OUT/grafana-dashboard.json" && break
  sleep 2
done
if grep -q '"title"' "$OUT/grafana-dashboard.json"; then
  ok "面板入库（provisioning 出来的，不是我手工点的）：$DASH_UID，轮询 $i 次拿到"
else
  fail "Grafana 里没有这块面板，看 $OUT/grafana-dashboard.json"
fi

# ---------------- 阶段 8：六格逐格非空 ----------------
echo "-- 阶段 8：六格逐格非空（表达式从 dashboard.json 里取，不另写一份）"
# 从面板 JSON 里取 (格号, 表达式)。两处踩过的手感：
#   `"id": 1,` 的值没有引号（上一版按 `"id": "` 找，格号全空 → 六格一起假红）；
#   表达式里的 label 是 `\"` 转义的，这里的 PromQL 不含合法反斜杠，所以**一律去掉反斜杠**
#   比在 awk 里写四重转义可靠（上一版留着 `\"` 发给 Prometheus，查询直接 invalid）。
awk '
  /"id": *[0-9]+/ { p=$0; sub(/.*"id": */, "", p); sub(/[^0-9].*/, "", p); panel=p }
  /"expr":/ {
    line=$0
    sub(/.*"expr": *"/, "", line)          # 只有一处 "expr":，贪婪也无妨
    gsub(/[\\]"/, "\001", line)            # 先把 label 里那两个转义引号换成占位符
    sub(/".*/, "", line)                   # 再在第一个真引号处截断
    gsub(/\001/, "\"", line)               # 还原成 PromQL 自己的引号
    print panel "\t" line
  }
' deploy/grafana/dashboards/gateway-overview.json > "$OUT/panel-exprs.tsv"

# 第 1、3 格里有 rate(...[1m]) 与 increase(...[1m])：窗口里没有两个采样点时 Prometheus 返回**空**，
# 那是仪器还没攒够历史，不是被测方没数。Prometheus 2.51 没有 /-/uptime 这个口，所以自己记起跑秒。
while (( $(date +%s) - PROM_START < 75 )); do sleep 3; done
ok "Prometheus 已运行 $(( $(date +%s) - PROM_START )) s（≥75，rate/increase 的 1m 窗口有样本）"

for want in 1 2 3 4 5 6; do
  total=0; hit=0; empties=""
  while IFS=$'\t' read -r pid expr; do
    [[ "$pid" == "$want" ]] || continue
    total=$((total + 1))
    n=$(prom_series "$expr" "p8-panel${want}-expr${total}")
    if (( n > 0 )); then hit=$((hit + 1)); else empties="$empties [#$total]$expr"; fi
  done < "$OUT/panel-exprs.tsv"
  if (( total > 0 && hit == total )); then
    ok "第 $want 格：$total 条表达式全部查到 series（键名与面板同源）"
  else
    fail "第 $want 格：$total 条里只有 $hit 条查到 series，空的是$empties"
  fi
done
# 分位那一族的口径标签必须在**查询结果**里，不是只在文案里（E4：抹掉 canonic 标签要能红）。
if prom_query 'l7gate_bench_increment_microseconds' p8-labels | grep -q '"canonic"' &&
   prom_query 'l7gate_bench_increment_microseconds' p8-labels | grep -q '"conns"'; then
  ok "分位那一族带着 canonic/conns 标签（哪一批、什么档，读图的人能自己核）"
else
  fail "分位那一族少了 canonic 或 conns 标签——非标准档就没法和 SLO 写死的那一档区分开"
fi

# ---------------- 阶段 9：429 与 rate_limit_rejected 逐数对得上 ----------------
echo "-- 阶段 9：429 互背书（Prometheus 读数 == 网关自报，取数路径互相独立）"
rm -f "$OUT/prov-gateway.txt" "$OUT/prov-prometheus.txt"   # 这一节自己负责留下两边的痕迹
sleep 8   # 让至少一个抓取节拍落在静置之后
p_val=$(prom_value "rate_limit_rejected" p9-429)
g_val=$(metric rate_limit_rejected)
if [[ -s "$OUT/prov-gateway.txt" && -s "$OUT/prov-prometheus.txt" ]] &&
   ! cmp -s "$OUT/prov-gateway.txt" "$OUT/prov-prometheus.txt"; then
  ok "等式两边确实是两条通道：网关自报 $(cat "$OUT/prov-gateway.txt") ≠ 仪器 $(cat "$OUT/prov-prometheus.txt")"
else
  fail "这一轮没留下两条不同的取数通道——「互背书」退化成了仪器自己对自己（E3 要抓的就是这个）"
fi
if [[ -n "$p_val" && -n "$g_val" ]] && [[ "${p_val%.*}" == "${g_val%.*}" ]]; then
  ok "静置臂：Prometheus=${p_val} 与网关自报 rate_limit_rejected=${g_val} 逐数相等"
else
  fail "静置臂对不上：Prometheus='${p_val:-空}' vs 网关自报 '${g_val:-空}'"
fi
for i in $(seq 1 20); do curl -s --max-time 2 -o /dev/null "http://$LIMITED" >/dev/null; done
BUSINESS_CURLS=$((BUSINESS_CURLS + 20))
g_val2=$(metric rate_limit_rejected)
if (( g_val2 > g_val )); then
  ok "扰动臂：网关自报确实前进了（$g_val → $g_val2），等式不是两个静止的 0 相等"
else
  fail "扰动臂没前进：$g_val → $g_val2"
fi
sleep 8
p_val2=$(prom_value "rate_limit_rejected" p9-429-2)
g_val3=$(metric rate_limit_rejected)
if [[ "${p_val2%.*}" == "${g_val3%.*}" ]]; then
  ok "扰动后再对一次：Prometheus=${p_val2} == 网关自报=${g_val3}"
else
  fail "扰动后对不上：Prometheus='${p_val2:-空}' vs 网关自报='${g_val3:-空}'"
fi
other=$(prom_value "responses_completed" p9-other)
if [[ -n "$other" && "$other" != "$p_val2" ]]; then
  ok "反向对照：另一条键（responses_completed=$other）与 429 那条不相等——等式取错键时不会也绿"
else
  fail "反向对照没建立：两条键读成同一个数，这条等式挡不住取错键"
fi

# ---------------- 阶段 10：措辞限定入库 ----------------
echo "-- 阶段 10：面板文案里那几句限定（入库靠断言）"
DASH=deploy/grafana/dashboards/gateway-overview.json
grep -qF '不能读成「没有坏后端」' "$DASH" \
  && ok "第 5 格带 ARCH 1.31 边界⑥ 那句限定" \
  || fail "第 5 格缺 health_ejected_now=0 那句限定"
grep -qF '只覆盖首装那批成员' "$DASH" \
  && ok "第 5 格写明逐目标健康键只覆盖首装那批成员（重载带进来的要重启才进这些键）" \
  || fail "第 5 格缺「只覆盖首装那批成员」这条 S5-02 边界"
grep -qF '含热重载的预留槽位容量' "$DASH" \
  && ok "RSS 那一格写成「含热重载的预留槽位容量」（B-020 的 +19.1 MiB）" \
  || fail "RSS 那一格没写预留槽位容量那句（会被读成网关自己吃内存）"
grep -qF '≈128 µs/请求' "$DASH" \
  && ok "分位那一格带本档分辨率限定 ≈128 µs/请求" \
  || fail "分位那一格缺分辨率限定"
grep -qF '取不到' "$DASH" && grep -qF 'B-009' "$DASH" \
  && ok "分位那一格写了 1000 并发档 p999 判「取不到」与观察者饱和（B-009）" \
  || fail "分位那一格没带 B-009 那句（面板会读成连续监测）"

# ---------------- 阶段 11：抓取不进被测方 ----------------
echo "-- 阶段 11：抓取不进被测方"
parsed_now=$(metric requests_parsed)
scrapes=$(metric metrics_scrapes_ok)
if [[ -n "$scrapes" ]] && (( scrapes > 0 )); then
  ok "metrics_scrapes_ok=$scrapes（抓取确实发生过——不然这一格是空的仪器）"
else
  fail "metrics_scrapes_ok 读不到或为 0：Prometheus 到底抓没抓？"
fi
expected=$(( PARSED_BEFORE_CURLS + BUSINESS_CURLS ))
if [[ "$parsed_now" == "$expected" ]]; then
  ok "requests_parsed 恰好等于我自己发的条数：$PARSED_BEFORE_CURLS + $BUSINESS_CURLS = $expected（Prometheus 抓了 $scrapes 次，一条都没进业务计数）"
else
  fail "requests_parsed=$parsed_now 而我只发了 $expected 条——抓取进被测方了，或者我的计数错了"
fi

# ---------------- 阶段 12：排水与生命周期 ----------------
echo "-- 阶段 12：停网关，读退出账"
kill -TERM "$GW" 2>/dev/null; wait "$GW" 2>/dev/null
for i in $(seq 1 20); do [[ -s "$OUT/gateway.log" ]] && grep -q "\[serve\]" "$OUT/gateway.log" && break; sleep 0.5; done
live=$(get_serve live_connections); constructed=$(get_serve connections_constructed)
destructed=$(get_serve connections_destructed); fd0=$(get_serve fd_start); fd1=$(get_serve fd_end)
if [[ -n "$live" && "$live" == "0" ]]; then ok "live_connections=0（排水收干净）"; else fail "live_connections=$live"; fi
if [[ -n "$constructed" && "$constructed" == "$destructed" ]]; then
  ok "constructed==destructed==$constructed（配对账不因抓取与摘除漂移）"
else
  fail "constructed=$constructed destructed=$destructed"
fi
if [[ "$fd0" == "$fd1" ]]; then ok "fd $fd0 → $fd1 逐数相等（探活关着，走的是相等那一档，1.31 边界⑦）"
else fail "fd 没回基线：$fd0 → $fd1"; fi
if grep -q "^\[serve\] config_" "$OUT/gateway.log"; then
  fail "没 arm 文件重载却发了 config_ 键——S5-02 那条「关闭态一个都不发」被我破坏了"
else
  ok "关闭态没有 config_ 键（S5-02 的 60 格 golden 口径原样成立）"
fi

cleanup          # 正常路径自己清场，然后撤销 trap，让最后那句 exit 不再与 EXIT trap 纠缠
trap - EXIT
verdict=PASS; (( FAILED )) && verdict=FAIL
echo "$verdict checks=$CHECKS fails=$FAILS" > "$OUT/verdict.txt"
echo "== 汇总：$CHECKS 项判定，✗ 计数 $FAILS =="
echo "SMOKE-S5-03 $verdict（同一句判定写在 $OUT/verdict.txt）"
exit "$FAILED"
