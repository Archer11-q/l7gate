#!/usr/bin/env bash
# S5-02 冒烟：配置热重载在**真进程**里成立到什么程度。
#
# ctest 那 17 条管的是机制（换代只有一次 store、旧世代被引用计数延命、拒载分类账、
# 世代随请求进链）。这个脚本管的是它们替代不了的三件事：
#   一、**生效延迟的毫秒数**（验收句③，B-015 那 406/536 ms 的形状：改一次、量一次，
#        两端分别是"脚本写完文件"与"客户端第一次拿到新后端才有的那个响应体长"，
#        网关自报的 `config_last_publish_epoch_ms` 必须落在同一根墙钟轴的那段区间里）；
#   二、**存量请求走完旧表、零截断**——用一台会压 500 ms 才回的上游把"在飞"造出来，
#        换代就发生在那三条挂着的当口，三条都要拿到完整的旧后端响应；
#   三、**关闭态真的什么都不动**：不给 `--config-reload-ms` 时改文件不生效，
#        退出账里一个 `config_` 键都没有（golden 那 60 格因此不用改）。
#
# 阶段：
#   0 起两台上游 + 主网关（arm 200 ms，同时开指标面当运行期的眼睛）
#   1 基线路由
#   2 生效延迟实测两轮（来回改指）
#   3 在飞零截断（另一台网关 + 一台慢上游）
#   4 语法错：拒绝有账、旧世代继续服务、随后好配置确实发布（上界配下界）
#   5 定义域外（限流参数）：拒绝并点名
#   6 命令行来源的键出现在文件里：拒绝并点名（验收句⑤）
#   7 touch 不改内容：查了一遍不等于发布了一次
#   8 启动期两条拒绝（退出码 2）：没 --config 就 arm、mysql 组叠文件重载
#   9 关闭态：改文件不生效 + 退出账没有 config_ 键
#  10 排水收尾：constructed==destructed、fd 回基线、旧世代全部释放
#
# 用法：PRESET=debug scripts/smoke-s5-02.sh
# 输出：build/smoke-s5-02/ 里的日志；退出码 0 = 全部通过。
set -uo pipefail
cd "$(dirname "$(readlink -f "$0")")/.." || exit 1
trap '' PIPE

PRESET="${PRESET:-debug}"
BIN="build/$PRESET"
GATEWAY="$BIN/l7gate"
UPSTREAM_BIN="$BIN/tests/l7gate_test_upstream"
OUT="build/smoke-s5-02"
mkdir -p "$OUT"

G_PORT=18700          # 主网关（阶段 0–7、10）
G2_PORT=18701         # 阶段 3 那台"在飞"网关
G3_PORT=18702         # 阶段 9 那台没 arm 的网关
U1_PORT=18703         # 64 字节体（组 a）
U2_PORT=18704         # 128 字节体（组 b）
U3_PORT=18705         # 192 字节体 + 压 500 ms（组 slow，只在阶段 3）
M_PORT=18706          # 主网关的指标端口
RELOAD_MS="${RELOAD_MS:-200}"

FAILED=0
ok()   { echo "  ✓ $*"; }
fail() { echo "  ✗ $*"; FAILED=1; }
die()  { echo "  fatal: $*" >&2; exit 1; }

[[ -x "$GATEWAY" ]] || die "找不到 $GATEWAY，先 cmake --build --preset $PRESET"
[[ -x "$UPSTREAM_BIN" ]] || die "找不到 $UPSTREAM_BIN，先 cmake --build --preset $PRESET"
command -v curl >/dev/null || die "本机没有 curl"

port_free() { ! (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; }
for p in "$G_PORT" "$G2_PORT" "$G3_PORT" "$U1_PORT" "$U2_PORT" "$U3_PORT" "$M_PORT"; do
  port_free "$p" || die "端口 $p 已被占用：先清场"
done

wait_for_line() { # pid logfile pattern [seconds]
  local pid=$1 log=$2 pat=$3 secs=${4:-15} i=0
  while (( i < secs * 20 )); do
    grep -q -- "$pat" "$log" 2>/dev/null && return 0
    if ! kill -0 "$pid" 2>/dev/null; then
      echo "  进程 $pid 已退出，日志尾部：" >&2; tail -5 "$log" >&2; return 1
    fi
    ((i++)); sleep 0.05
  done
  return 1
}

get_stat() { sed -n "s/^\[serve\] $1=\([0-9]*\).*/\1/p" "$2" | head -1; }

# 运行期读指标面的一格。**只在脚本里当眼睛用**：判定用等式，不用"看起来有数"。
metric() { # key → 值（没有就空）
  curl -s --max-time 3 "http://127.0.0.1:$M_PORT/metrics" 2>/dev/null |
    awk -v k="$1" '$1==k {print $2; exit}'
}

req() { # port path → "code bytes"
  local port=$1 path=$2 file="$OUT/body.$$"
  local code n
  code=$(curl -s --max-time 5 -o "$file" -w '%{http_code}' "http://127.0.0.1:$port$path" 2>/dev/null)
  n=$(wc -c < "$file" 2>/dev/null)
  rm -f "$file"
  echo "${code:-000} ${n:-0}"
}

# 配置模板：`$1` 是组 a 那条路由指向哪个组（"a" 或 "b"），限流段固定 1000/1000。
# 端口是编在本机回环上的三台上游；routes 三条：/a 可切换、/b 固定组 b、catch-all 组 a。
write_config() { # 文件 组a的指向 [附加段]
  local file=$1 where=$2 extra=${3:-}
  cat > "$file" <<JSON
{
  "upstreams": [
    { "name": "a", "balance": "round_robin", "targets": [
      { "address": "127.0.0.1", "port": $U1_PORT } ] },
    { "name": "b", "balance": "round_robin", "targets": [
      { "address": "127.0.0.1", "port": $U2_PORT } ] }
  ],
  "routes": [
    { "match": "prefix", "pattern": "/a", "upstream": "$where" },
    { "match": "prefix", "pattern": "/b", "upstream": "b" },
    { "match": "prefix", "pattern": "", "upstream": "a" }
  ],
  "health": { "max_fails": 3, "probe_interval_ms": 0 },
  "rate_limit": { "rate": 1000, "burst": 1000, "key": "path" }$extra
}
JSON
}

PIDS=()
# 退出码要在 trap 里保住：`cleanup` 末尾那句 `wait` 自己会返回 0，把脚本的判定盖掉
# （本轮实测：判定 FAIL 而 rc=0，就是这个形状——FAIL 必须让 rc 也红，否则判定等于没跑）。
cleanup() {
  local rc=$?
  for pid in "${PIDS[@]:-}"; do kill -TERM "$pid" 2>/dev/null; done
  wait 2>/dev/null
  exit "$rc"
}
trap cleanup EXIT

# 毫秒时间戳。`date +%s%3N` 在本机不截断（给出的是 19 位纳秒），所以自己除——
# 两端钉时间线用的是同一根轴，轴上的单位必须是毫秒，否则"生效延迟"会读成一个天文数字
# （本轮第一轮就是这么红的：LAT 读成 1.4 亿 ms，而差的是我的取数方式不是网关）。
now_ms() { echo $(( $(date +%s%N) / 1000000 )); }

echo "== 阶段 0：两台上游 + 主网关（arm ${RELOAD_MS} ms，指标端口 $M_PORT）=="
"$UPSTREAM_BIN" --listen "127.0.0.1:$U1_PORT" --body-bytes 64 > "$OUT/up1.log" 2>&1 & U1=$!
"$UPSTREAM_BIN" --listen "127.0.0.1:$U2_PORT" --body-bytes 128 > "$OUT/up2.log" 2>&1 & U2=$!
PIDS+=("$U1" "$U2")
for pair in "$U1:up1" "$U2:up2"; do
  wait_for_line "${pair%%:*}" "$OUT/${pair##*:}.log" "监听" 15 || die "上游没起来"
done
CFG="$OUT/gateway.json"
write_config "$CFG" a
"$GATEWAY" --serve --listen "127.0.0.1:$G_PORT" --config "$CFG" \
           --config-reload-ms "$RELOAD_MS" --metrics-port "$M_PORT" > "$OUT/gateway.log" 2>&1 & G=$!
PIDS+=("$G")
wait_for_line "$G" "$OUT/gateway.log" "监听" 15 || die "主网关没起来"
if grep -q 'config_reload=on' "$OUT/gateway.log"; then
  ok "启动横幅报告 config_reload=on（path/interval/gen 三格都在）"
else
  fail "横幅里没有 config_reload=on：arm 了却没报告，运维读不出这台为什么不动"
fi
banner_gen=$(sed -n 's/.*config_reload=on.*gen=\([0-9]*\).*/\1/p' "$OUT/gateway.log" | head -1)
[[ "${banner_gen:-0}" == "1" ]] && ok "首装就是第 1 代" || fail "横幅的 gen=${banner_gen:-?}，应当是 1"
[[ "$(metric config_gen)" == "1" ]] && ok "指标面运行期读到 config_gen=1" \
  || fail "指标面读不到 config_gen=1（读到的是 '$(metric config_gen)'）"

echo "== 阶段 1：基线路由 =="
read -r c1 n1 <<< "$(req "$G_PORT" /a/x)"
read -r c2 n2 <<< "$(req "$G_PORT" /b/x)"
if [[ "$c1" == "200" && "$n1" == "64" && "$c2" == "200" && "$n2" == "128" ]]; then
  ok "/a→组 a(64)、/b→组 b(128)"
else
  fail "基线不对：/a=${c1}/${n1} /b=${c2}/${n2}，期望 200/64 与 200/128"
fi

echo "== 阶段 2：生效延迟实测（验收句③，两轮来回改指）=="
LATS=()
for round in 1 2; do
  if (( round == 1 )); then want=128; write_config "$CFG" b; else want=64; write_config "$CFG" a; fi
  T0=$(now_ms)
  got=""; T1=""
  for _ in $(seq 1 200); do
    read -r _ n <<< "$(req "$G_PORT" /a/x)"
    if [[ "$n" == "$want" ]]; then got="$n"; T1=$(now_ms); break; fi
    sleep 0.02
  done
  if [[ -z "$got" ]]; then
    fail "第 $round 轮改指后 4 s 内没有一次读到新后端（期望体长 $want）"
    continue
  fi
  LAT=$((T1 - T0))
  LATS+=("$LAT")
  pub=$(metric config_last_publish_epoch_ms)
  if [[ -n "$pub" ]] && (( pub >= T0 && pub <= T1 )); then
    ok "第 $round 轮生效 ${LAT} ms（节拍 ${RELOAD_MS} ms），网关自报换代时刻 $pub 落在 [$T0,$T1] 内"
  else
    fail "第 $round 轮自报换代时刻 ${pub:-空} 不在 [$T0,$T1] 里——那 ${LAT} ms 就没被两端钉住"
  fi
  # 上界：一个检查节拍 + worker 那一拍的等待（缺省 200 ms）+ 取数与 curl 的余量。
  if (( LAT <= RELOAD_MS + 800 )); then
    ok "第 $round 轮 ${LAT} ms ≤ 节拍 + 余量（节拍 ${RELOAD_MS} ms 这一格就是粒度）"
  else
    fail "第 $round 轮 ${LAT} ms 超出节拍 + 800 ms——节拍决定的粒度不该这么差"
  fi
done
if (( ${#LATS[@]} == 2 )); then
  echo "  生效延迟读数：${LATS[0]} ms / ${LATS[1]} ms（节拍 ${RELOAD_MS} ms，粒度由节拍决定）"
  ok "两轮都取到数（报的是毫秒数，不是'秒级'）"
else
  fail "生效延迟只取到 ${#LATS[@]} 轮"
fi

echo "== 阶段 3：在飞请求走完旧表、零截断（另起一台网关 + 一台压 500 ms 的上游）=="
"$UPSTREAM_BIN" --listen "127.0.0.1:$U3_PORT" --body-bytes 192 --delay-ms 500 > "$OUT/up3.log" 2>&1 & U3=$!
PIDS+=("$U3")
wait_for_line "$U3" "$OUT/up3.log" "监听" 15 || die "慢上游没起来"
# 组 slow 的指向由 /s 这条路由决定；配置里额外加一个组（第三台后端）。
CFG2="$OUT/gateway2.json"
cat > "$CFG2" <<JSON
{
  "upstreams": [
    { "name": "a", "balance": "round_robin", "targets": [
      { "address": "127.0.0.1", "port": $U1_PORT } ] },
    { "name": "slow", "balance": "round_robin", "targets": [
      { "address": "127.0.0.1", "port": $U3_PORT } ] },
    { "name": "b", "balance": "round_robin", "targets": [
      { "address": "127.0.0.1", "port": $U2_PORT } ] }
  ],
  "routes": [
    { "match": "prefix", "pattern": "/s", "upstream": "slow" },
    { "match": "prefix", "pattern": "", "upstream": "a" }
  ],
  "health": { "max_fails": 3, "probe_interval_ms": 0 }
}
JSON
"$GATEWAY" --serve --listen "127.0.0.1:$G2_PORT" --config "$CFG2" --config-reload-ms "$RELOAD_MS" \
           > "$OUT/gateway2.log" 2>&1 & G2=$!
PIDS+=("$G2")
wait_for_line "$G2" "$OUT/gateway2.log" "监听" 15 || die "阶段 3 的网关没起来"

# 三条并发打到 /s：每条都被路由到那台 500 ms 才回的后端上——这就是"在飞"。
# **只等这三条 curl 的 pid**：`wait` 不带参数会把上游与网关一起等住（那是 S3-05 记过的
# 同一族"脚本自己挂死"的形状）。
CPIDS=()
for i in 1 2 3; do
  curl -s --max-time 8 -o "$OUT/inflight$i.body" -w '%{http_code}\n' \
       "http://127.0.0.1:$G2_PORT/s/x" > "$OUT/inflight$i.code" 2>/dev/null &
  CPIDS+=("$!")
done
sleep 0.2   # 让三条都过了链（下一条断言用慢上游的自报数复核，不靠这句 sleep）
# 换代就发生在这三条挂着的当口：/s 从慢后端改指组 b。
sed -i 's|"pattern": "/s", "upstream": "slow"|"pattern": "/s", "upstream": "b"|' "$CFG2"
wait_for_line "$G2" "$OUT/gateway2.log" "gen=2 生效" 10 || fail "换代没发生——那后面三条读的仍是旧表，这一格就不是证据"
for pid in "${CPIDS[@]}"; do wait "$pid" 2>/dev/null; done
trunc=0
for i in 1 2 3; do
  code=$(head -1 "$OUT/inflight$i.code" 2>/dev/null)
  n=$(wc -c < "$OUT/inflight$i.body" 2>/dev/null)
  [[ "$code" == "200" && "$n" == "192" ]] || { trunc=$((trunc+1)); echo "    第 $i 条：code=${code:-?} bytes=${n:-?}（期望 200/192）"; }
done
if (( trunc == 0 )); then
  ok "在飞三条逐条完整、且都是**旧表那台**后端的 192 字节体（换代没截断、没改道）"
else
  fail "在飞三条里有 $trunc 条没有拿到完整的旧后端响应"
fi
# 上游的"已服务请求数"是它**退出时**打的那一行，所以先停它再读（顺序反过来读到的就是空，
# 而空在判定里长得像 0）。
kill -TERM "$U3" 2>/dev/null; wait "$U3" 2>/dev/null
slow_served=$(sed -n 's/^\[upstream\] 已服务请求数=\([0-9]*\).*/\1/p' "$OUT/up3.log" | head -1)
if [[ "$slow_served" == "3" ]]; then
  ok "慢后端自报收到 3 条（与客户端那三条对上，不是拿网关总数除以后端数）"
else
  fail "慢后端自报 ${slow_served:-?} 条，期望 3"
fi
read -r c_new n_new <<< "$(req "$G2_PORT" /s/x)"
if [[ "$c_new" == "200" && "$n_new" == "128" ]]; then
  ok "换代之后新请求用新表（/s → 组 b，128 字节体）"
else
  fail "新请求是 ${c_new:-?}/${n_new:-?}，期望 200/128"
fi
kill -TERM "$G2" 2>/dev/null; wait "$G2" 2>/dev/null
if grep -q 'connections_constructed=' "$OUT/gateway2.log" &&
   [[ "$(get_stat connections_constructed "$OUT/gateway2.log")" == "$(get_stat connections_destructed "$OUT/gateway2.log")" ]]; then
  ok "阶段 3 那台退出账 constructed==destructed（重载没漏对象）"
else
  fail "阶段 3 退出账不相等：constructed=$(get_stat connections_constructed "$OUT/gateway2.log") destructed=$(get_stat connections_destructed "$OUT/gateway2.log")"
fi
kill -TERM "$U3" 2>/dev/null; wait "$U3" 2>/dev/null

echo "== 阶段 4：语法错——拒绝有账、旧世代继续服务 =="
gen_before=$(metric config_gen); pub_before=$(metric config_publishes); syn_before=$(metric config_rejects_syntax)
printf '{ "upstreams": [ oops\n' > "$CFG"
wait_for_line "$G" "$OUT/gateway.log" "配置重载被拒（语法）" 10 || fail "语法错没打点名的话"
sleep 0.3
[[ "$(metric config_rejects_syntax)" == "$((syn_before + 1))" ]] \
  && ok "config_rejects_syntax +1" || fail "语法账没进：$(metric config_rejects_syntax) vs $((syn_before+1))"
[[ "$(metric config_publishes)" == "$pub_before" ]] \
  && ok "config_publishes 一动不动（没有半套生效）" || fail "语法错却换发了世代：$pub_before → $(metric config_publishes)"
read -r c_keep n_keep <<< "$(req "$G_PORT" /a/x)"
[[ "$n_keep" == "64" ]] && ok "旧世代仍在服务（/a 还是 64 字节体）" \
  || fail "坏配置把在服的那一代也带跑了：/a=${n_keep} 字节"
# 下界：紧接着写一份好配置，同一节拍里必须真的发布一次。
write_config "$CFG" b
sleep 0.6
[[ "$(metric config_publishes)" == "$((pub_before + 1))" ]] \
  && ok "随后一份好配置确实发布（+1）——上一格那句'一动不动'不是仪器失灵" \
  || fail "好配置也没发布：publishes=$(metric config_publishes) 期望 $((pub_before+1))"

echo "== 阶段 5：定义域外（限流参数）——拒绝并点名 =="
pub_before=$(metric config_publishes); rej_before=$(metric config_rejects_scope)
write_config "$CFG" b; sed -i 's|"rate": 1000, "burst": 1000|"rate": 2000, "burst": 1000|' "$CFG"
wait_for_line "$G" "$OUT/gateway.log" "定义域外" 10 || fail "限流参数变了却没打点名的话"
sleep 0.3
[[ "$(metric config_rejects_scope)" == "$((rej_before + 1))" ]] \
  && ok "config_rejects_scope +1" || fail "越界账没进：$(metric config_rejects_scope)"
[[ "$(metric config_publishes)" == "$pub_before" ]] \
  && ok "限流参数改动没有换发世代" || fail "限流参数被重载生效了——那是追加节第一节第 5 条禁止的形状"
grep -q 'rate_limit 的 rate/burst' "$OUT/gateway.log" && ok "那句拒绝点到了 rate_limit 的具体字段" \
  || fail "拒绝的话里没点到 rate_limit"

echo "== 阶段 6：命令行来源的键出现在文件里（验收句⑤）=="
rej_before=$(metric config_rejects_scope); pub_before=$(metric config_publishes)
write_config "$CFG" b
sed -i 's|"health": { "max_fails": 3|"listen": "0.0.0.0", "workers": 4, "health": { "max_fails": 3|' "$CFG"
wait_for_line "$G" "$OUT/gateway.log" "不认识的字段 listen" 10 || fail "文件里写 listen 没被点名"
sleep 0.3
[[ "$(metric config_rejects_scope)" == "$((rej_before + 1))" ]] && ok "config_rejects_scope +1" \
  || fail "越界账没进"
[[ "$(metric config_publishes)" == "$pub_before" ]] && ok "命令行来源的参数没有经由重载生效（已裁② 的切分原样成立）" \
  || fail "重载绕过了命令行/文件的切分"

echo "== 阶段 7：touch 不改内容——查了一遍不等于发布了一次 =="
write_config "$CFG" b            # 先把文件恢复到好配置
sleep 0.4
checks_before=$(metric config_checks); pub_before=$(metric config_publishes)
touch "$CFG"
sleep $((RELOAD_MS / 1000 + 1))
[[ "$(metric config_checks)" -gt "$checks_before" ]] && ok "config_checks 前进了（到点真的查了）" \
  || fail "config_checks 没动——节拍可能根本没在跑，那上一格就是假绿"
[[ "$(metric config_publishes)" == "$pub_before" ]] \
  && ok "内容没变的 touch 没被记成一次换代" || fail "touch 被读成了换代（$pub_before → $(metric config_publishes)）"

echo "== 阶段 8：启动期两条拒绝（退出码 2、话里点名）=="
"$GATEWAY" --serve --listen "127.0.0.1:$G3_PORT" --upstream "127.0.0.1:$U1_PORT" --config-reload-ms 500 > "$OUT/nocfg.log" 2>&1
rc=$?
if [[ $rc == 2 ]] && grep -q '需要 --config' "$OUT/nocfg.log"; then
  ok "arm 了却没给 --config：退出码 2 且点名"
else
  fail "arm 无 --config 应退出 2 并点名，实得 rc=$rc：$(tail -1 "$OUT/nocfg.log")"
fi
CFGMySQL="$OUT/mysql.json"
cat > "$CFGMySQL" <<JSON
{
  "upstreams": [
    { "name": "a", "balance": "round_robin", "source": "mysql" }
  ]
}
JSON
"$GATEWAY" --serve --listen "127.0.0.1:$G3_PORT" --config "$CFGMySQL" --nodes-schema db \
            --config-reload-ms 500 > "$OUT/mysqlclash.log" 2>&1
rc=$?
if [[ $rc == 2 ]] && grep -q '两条发布路径不叠加' "$OUT/mysqlclash.log"; then
  ok "mysql 组 + arm 文件重载：退出码 2 且说的是同一条理由（不叠加成两份真相）"
else
  fail "mysql 组与文件重载该在启动时互斥，实得 rc=$rc：$(tail -1 "$OUT/mysqlclash.log")"
fi

echo "== 阶段 9：关闭态（没给 --config-reload-ms）改文件不生效、退出账没有 config_ 键 =="
CFG3="$OUT/off.json"
write_config "$CFG3" a
"$GATEWAY" --serve --listen "127.0.0.1:$G3_PORT" --config "$CFG3" > "$OUT/off.log" 2>&1 & G3=$!
PIDS+=("$G3")
wait_for_line "$G3" "$OUT/off.log" "监听" 15 || die "阶段 9 的网关没起来"
grep -q 'config_reload=on' "$OUT/off.log" && fail "没 arm 却报告了 config_reload=on" \
  || ok "横幅里没有 config_reload=on（缺省就是关）"
read -r c_off n_off <<< "$(req "$G3_PORT" /a/x)"
[[ "$n_off" == "64" ]] && ok "基线 /a=64" || fail "关闭态基线就不对：${c_off}/${n_off}"
write_config "$CFG3" b
sleep $((RELOAD_MS / 1000 + 1))
read -r c_off2 n_off2 <<< "$(req "$G3_PORT" /a/x)"
[[ "$n_off2" == "64" ]] && ok "没 arm 时改文件不生效（还是 64）——重载不是白送的行为差异" \
  || fail "没 arm 却生效了：${n_off2} 字节"
kill -TERM "$G3" 2>/dev/null; wait "$G3" 2>/dev/null
off_keys=$(grep -o 'config_[a-z_]*=' "$OUT/off.log" | sort -u | tr '\n' ',' )
if [[ -z "$off_keys" ]]; then
  ok "关闭态退出账里一个 config_ 键都没有（新增的 11 格只在 arm 时存在）"
else
  fail "关闭态多出了 config_ 键：$off_keys"
fi

echo "== 阶段 10：主网关优雅排水 =="
kill -TERM "$G" 2>/dev/null; wait "$G" 2>/dev/null
constructed=$(get_stat connections_constructed "$OUT/gateway.log")
destructed=$(get_stat connections_destructed "$OUT/gateway.log")
live=$(get_stat live_connections "$OUT/gateway.log")
fd_start=$(get_stat fd_start "$OUT/gateway.log"); fd_end=$(get_stat fd_end "$OUT/gateway.log")
if [[ "$constructed" == "$destructed" && -n "$constructed" ]]; then
  ok "退出账 constructed==destructed（$constructed）——重载没让配对账漂移"
else
  fail "退出账漂移：constructed=${constructed:-?} destructed=${destructed:-?}"
fi
[[ "$live" == "0" ]] && ok "live_connections=0" || fail "live_connections=${live:-?}"
if [[ "$fd_start" == "$fd_end" && -n "$fd_start" ]]; then
  ok "fd 回基线（$fd_start→$fd_end）"
else
  fail "fd 没回基线：${fd_start:-?}→${fd_end:-?}"
fi
snap_live=$(get_stat config_snapshots_live "$OUT/gateway.log")
if [[ "$snap_live" == "1" ]]; then
  ok "退出时旧世代全释放：config_snapshots_live=1（只剩在册那一代）"
else
  fail "config_snapshots_live=${snap_live:-?}，期望 1——世代泄漏或有东西抱着针不放"
fi
gen_end=$(get_stat config_gen "$OUT/gateway.log")
publishes=$(get_stat config_publishes "$OUT/gateway.log")
rejects=$(get_stat config_rejects "$OUT/gateway.log")
echo "  总账：config_gen=${gen_end:-?} config_publishes=${publishes:-?} config_rejects=${rejects:-?}（其中语法 $(get_stat config_rejects_syntax "$OUT/gateway.log") / 越界 $(get_stat config_rejects_scope "$OUT/gateway.log")）"
if [[ -n "$gen_end" && "$gen_end" -ge 2 && "$publishes" -ge 2 && "$rejects" -ge 3 ]]; then
  ok "换代至少 2 次、拒绝至少 3 次（阶段 4–6 各一次），三格账都对得上"
else
  fail "总账对不上：gen=${gen_end:-?} publishes=${publishes:-?} rejects=${rejects:-?}"
fi

echo "== 阶段 11：多 worker 臂（钩子只挂 0 号，四个 worker 各自对账）=="
# 上面所有阶段都是 workers=1（缺省），那里"谁 stat、谁发布、谁读世代"只有一条循环，
# 分不清两件事：**一次发布**与**每个 worker 都换到了世代**。四个 worker 跑同一台，
# 这两件事各自可断——发布数只该是 1（多 worker 各自 stat 会把同一个文件读四遍、
# 还会让"哪一代生效"有四个发起者），而这 8 条连接（各落在不同 worker 上）
# 都要在换代之后读到新表。
G4_PORT=18707
port_free "$G4_PORT" || die "端口 $G4_PORT 已被占用"
CFG4="$OUT/gateway4.json"
write_config "$CFG4" a
"$GATEWAY" --serve --listen "127.0.0.1:$G4_PORT" --config "$CFG4" --workers 4 \
           --config-reload-ms "$RELOAD_MS" > "$OUT/gateway4.log" 2>&1 & G4=$!
PIDS+=("$G4")
wait_for_line "$G4" "$OUT/gateway4.log" "监听" 15 || die "阶段 11 的网关没起来"
if grep -q "config_reload=on" "$OUT/gateway4.log" && grep -q "workers=4" "$OUT/gateway4.log"; then
  ok "多 worker 臂装好了重载（横幅同时报告 workers=4 与 config_reload=on）"
else
  fail "多 worker 臂的重载横幅不全：$(grep -m1 'config_reload' "$OUT/gateway4.log" || echo 无 config_reload)"
fi
base_ok=0
for i in 1 2 3 4 5 6 7 8; do
  read -r _ n <<< "$(req "$G4_PORT" /a/w$i)"
  [[ "$n" == "64" ]] && base_ok=$((base_ok+1))
done
[[ "$base_ok" == "8" ]] && ok "换代前八条连接都读到旧后端（64），四条以上落在不同 worker 上" \
  || fail "换代前有 $((8-base_ok)) 条不是 64 字节"
write_config "$CFG4" b
wait_for_line "$G4" "$OUT/gateway4.log" "gen=2 生效" 10 || fail "0 号 worker 没发布第二代"
sw=0
for i in 1 2 3 4 5 6 7 8; do
  for _ in $(seq 1 40); do
    read -r _ n <<< "$(req "$G4_PORT" /a/w$i)"
    [[ "$n" == "128" ]] && { sw=$((sw+1)); break; }
    sleep 0.02
  done
done
if [[ "$sw" == "8" ]]; then
  ok "八条连接最终都读到新后端（128）——每个 worker 都对账到了第二代"
else
  fail "只有 $sw/8 条读到新表：世代只进了 0 号那条循环，其余三条没对账"
fi
kill -TERM "$G4" 2>/dev/null; wait "$G4" 2>/dev/null
pub4=$(get_stat config_publishes "$OUT/gateway4.log")
if [[ "$pub4" == "1" ]]; then
  ok "四个 worker 只发布了一次（config_publishes=1）——写者是一个，不是四个"
else
  fail "config_publishes=${pub4:-?}，期望 1：多个 worker 都在 stat 并各自发布就是两份真相"
fi
c4=$(get_stat connections_constructed "$OUT/gateway4.log")
d4=$(get_stat connections_destructed "$OUT/gateway4.log")
[[ -n "$c4" && "$c4" == "$d4" ]] && ok "多 worker 臂退出账 constructed==destructed（$c4）" \
  || fail "多 worker 臂退出账漂移：${c4:-?}/${d4:-?}"

echo
if (( FAILED == 0 )); then
  echo "SMOKE-S5-02 PASS（日志在 $OUT）"
  exit 0
fi
echo "SMOKE-S5-02 FAIL"
exit 1
