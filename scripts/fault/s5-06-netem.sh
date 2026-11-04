#!/usr/bin/env bash
# S5-06：`tc netem` 现场验证（ADR-13 那条"网络层注入仅作交叉验证"的落地）。
#
# 判据（验收句①，已在认领那一轮量完，本脚本把它做成一条每次重跑都会复述的自检）：
# netem 能加、能生效——**但加不上的不是内核，是权限**：宿主回环要 CAP_NET_ADMIN（uid=1000 不行，
# sudo 要交互认证），而 `unshare -Un --map-root-user` 出来的非特权 user+netns 里 uid=0、CapEff 全开，
# netem 加上就生效。所以每一臂都在**一个全新的 ns** 里跑，注入的作用域是那一个 ns 的 lo，
# 宿主回环与同机流量一根毛都不碰（阶段 0 与收尾各查一次宿主 qdisc 必须是 `noqueue`）。
#
# 对照的对象（验收句③）：§12 的类 4a/4b（"慢与死的分界线就是 probe_timeout"）与类 3（连上不回字节）。
# **两处定义差异先写死，不许把对照读成等价**：
#   ① 网络层注入**不区分成员**——整条 ns 回环一起慢，健康的两台也超 probe_timeout，
#      因此会出现应用层矩阵从没造出的形状：三台一起被判死 → **ADR-12 的安全阀挡住摘除**
#      （`health_ejections_blocked_by_valve` 跳数）；
#   ② 应用层 `--delay-ms` 占住那台上游的单线程 poll（它所有客户端一起排队），
#      网络层 delay **不占被测方进程**——"在途请求排空"那一半成分因此不同，
#      §12 那条"连续 20 条不见 128"的成员识别判据在这里没有定义。
#
# 用法：PRESET=release scripts/fault/s5-06-netem.sh
# 判定落盘 **build/fault/s5-06/verdict.txt**（§22.2 第 5 条那一族：真子进程 + 真 ns 的形状下 rc 不可信）。
set -uo pipefail
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1
trap '' PIPE

PRESET="${PRESET:-release}"
OUT="build/fault/s5-06"
ARM="scripts/fault/s5-06-arm.sh"
rm -rf "$OUT"; mkdir -p "$OUT"

FAILED=0; FAILS=0; CHECKS=0
ok()   { echo "  ✓ $*"; CHECKS=$((CHECKS + 1)); }
fail() { echo "  ✗ $*"; FAILED=1; FAILS=$((FAILS + 1)); CHECKS=$((CHECKS + 1)); }
note() { echo "  · $*"; }
die()  { echo "  fatal: $*" >&2; echo "FAIL checks=$CHECKS fails=$((FAILS + 1)) die=1" > "$OUT/verdict.txt"; exit 1; }

NS='unshare -Un --map-root-user --propagation private --'
sum() { sed -n "s/^$1=//p" "$OUT/$2/summary.txt" 2>/dev/null | head -1; }

echo "== S5-06 netem 现场验证（PRESET=$PRESET）=="

# ---------------- 阶段 0：可用性判据（自检，每次都复述一遍） ----------------
echo "-- 阶段 0：可用性判据"
for b in "build/$PRESET/l7gate" "build/$PRESET/tests/l7gate_test_upstream"; do
  [[ -x "$b" ]] || die "找不到 $b —— 先 cmake --build --preset $PRESET"
done
command -v tc >/dev/null || die "本机没有 tc（iproute2）"
note "tc = $(tc -Version 2>&1 | head -1)；uname -r = $(uname -r)"

$NS bash -c 'ip link set dev lo up; tc qdisc add dev lo root netem delay 10ms 2>&1 | head -1;
              tc qdisc show dev lo | head -1; id -u; grep CapEff /proc/self/status' \
   > "$OUT/availability.txt" 2>&1
if grep -q 'netem' "$OUT/availability.txt" && grep -q '^0$' "$OUT/availability.txt"; then
  ok "非特权 user+netns 里 netem 能加、能读回（ns 内 uid=0、CapEff 全开）：$(grep '^qdisc' "$OUT/availability.txt" | head -1)"
else
  echo "  这条路不通（原样读数在 $OUT/availability.txt）。按 S5-06 验收句②：**先写 ADR 再换手段，不许悄悄换成没做**。"
  echo "FAIL checks=$CHECKS fails=0 refused=1" > "$OUT/verdict.txt"
  exit 1
fi
HOST_QDISC0=$(tc qdisc show dev lo 2>/dev/null | head -1)
echo "$HOST_QDISC0" | grep -q 'noqueue' && ok "宿主回环当前是 $HOST_QDISC0（本脚本不碰它，收尾再核一次）" \
                                        || fail "宿主回环不是 noqueue（$HOST_QDISC0）：先查是谁挂过 qdisc"

# ---------------- 六臂：每臂一个全新的 ns ----------------
# ARMS 是复跑旋钮（阳性验证要能只重跑一臂）：默认全跑。
ARMS="${ARMS:-clean d50 d100 d400 l20 d1500}"
declare -A SPEC=( [clean]="" [d50]="delay 50ms" [d100]="delay 100ms" [d400]="delay 400ms"
                  [l20]="loss 20% limit 1000" [d1500]="delay 1500ms" )
declare -A NREQ=( [clean]=12 [d50]=12 [d100]=12 [d400]=12 [l20]=12 [d1500]=8 )
# 采样次数与观察者超时：都跟着这一臂的注入量级走（上一发拿固定 3 s 去读 1500 ms 注入的观测面，
# 三条判定空转，那一臂的"没摘除"其实是"没读到"）。
# d50 是**为了证伪我自己那条修正后的模型**才加的：网络层 delay 把建连往返也算进去，
# 一次探活的等效耗时 ≈ 4×delay，所以分界线应从"delay<300ms"移到"delay<75ms"——
# 预测具体是 d50 不摘、d100 摘。哪一条不成立都要红出来。
declare -A NSAMP=( [clean]=14 [d50]=16 [d100]=16 [d400]=20 [l20]=14 [d1500]=6 )
declare -A NMTIME=( [clean]=4 [d50]=4 [d100]=4 [d400]=6 [l20]=6 [d1500]=20 )

for arm in $ARMS; do
  echo "-- 臂 $arm：netem '${SPEC[$arm]:-<不加>}'"
  PRESET="$PRESET" $NS bash "$ARM" "${SPEC[$arm]}" "${NREQ[$arm]}" "$OUT/$arm" \
      "${NSAMP[$arm]}" "${NMTIME[$arm]}" > "$OUT/$arm.out" 2>&1
  rc=$?
  if (( rc != 0 )); then fail "臂 $arm 跑失败 rc=$rc（$OUT/$arm.out）"; continue; fi
  ok "臂 $arm 跑完，读数落 $OUT/$arm/summary.txt"
done

# ---------------- 判定 ----------------
echo "-- 判定：每臂的『注入确实发生过』下界 + 摘除/安全阀 + 生命周期闭合"

# 通用：每一臂共同的退出账与观察者守卫
for arm in $ARMS; do
  [[ -s "$OUT/$arm/summary.txt" ]] || continue
  live=$(sum live "$arm"); con=$(sum constructed "$arm"); des=$(sum destructed "$arm")
  fd0=$(sum fd_start "$arm"); fd1=$(sum fd_end "$arm")
  if [[ "$live" == "0" ]]; then ok "[$arm] 退出账 live_connections=0"; else fail "[$arm] live=${live:-空}"; fi
  if [[ -n "$con" && "$con" == "$des" ]]; then ok "[$arm] constructed==destructed==$con（netem 与 ns 都不许让配对账漂移）"
  else fail "[$arm] constructed=${con:-空} destructed=${des:-空}"; fi
  # **fd 这一格按 ARCH 1.31 边界⑦ 走"分两档"**：这一栈装着探活，fd_end 在析构之前采样，
  # 所以不相等是预期形状（有几条在飞探活 socket 还没关），不拿它当守恒类不变式判死；
  # 但也不许放任——差值超过 4 就说不通（探活节拍 100 ms、三台成员，同时在飞的量级是个位数）。
  gap=$(( ${fd1:-0} - ${fd0:-0} ))
  if (( gap >= 0 && gap <= 4 )); then ok "[$arm] fd $fd0 → $fd1（差 $gap）——装探活的臂走边界⑦ 的「不相等那一档」，有界成立；守恒判定交给上面那两条"
  else fail "[$arm] fd $fd0 → $fd1（差 $gap）超出可解释的范围：这不是边界⑦ 的形状，得归因"; fi
  # 键名可读性守卫（D3 打出来的那条）：逐目标 ejections 三个键必须**三个都读到手**，
  # 否则"之和=0"这条绿是空的——键名写坏与"确实没摘"在数上不可区分。
  kr=$(sum metric_ejection_key_reads "$arm")
  kr=$(sum metric_ejection_key_reads "$arm"); ejsx=$(sum metric_ejections_sum "$arm")
  if [[ "${kr:-?}" == "3" ]]; then ok "[$arm] 逐目标 ejections 三个键都读到手（reads=$kr/3）——这一臂的 ej 求和不是空判"
  else fail "[$arm] 逐目标 ejections 只读到 ${kr:-?}/3 个键 —— 那个 ej=${ejsx:-空} 不作数：先查键名，别把读不到当成没发生"; fi
  # 观察者守卫。两种失灵必须分开，不许混成一个 ✗：
  #   ①**系统性读空**（超时给短了，整段采样全空）→ 这一臂的摘除/阀读数作废，判死；
  #   ②**丢包打到观察者自己**（loss 臂）→ 观测面走的也是那条被注入的回环，偶发几次超时是**预期**，
  #     只要收尾那次累计读成功，逐目标 ejections 与 valve 的数仍然可用。
  # 上一版一律要求零空读，于是 l20 臂被 ① 的措辞判死，其实它是 ②。
  emp=$(sum observer_empty_reads "$arm"); nsamp=$(sum samples "$arm")
  final=$(sum responses_completed "$arm")
  lim=$(( nsamp / 4 ))
  if [[ "${emp:-1}" == "0" ]]; then
    ok "[$arm] 观察者 $nsamp 次采样零空读（注入下观测面仍读得到，摘除那一侧的数才有资格进判定）"
  elif [[ -n "$final" ]] && (( ${emp:-99} <= lim )); then
    ok "[$arm] 观察者 $nsamp 次里空读 $emp 次（界内 $lim）且收尾累计读成功（responses_completed=$final）——这是 ②：丢包也打在观察面自己走的回环上，属预期，不按失灵处理"
    note "  （这一格本身就是 S5-06 的一条产出：**观测面不在注入的作用域之外**，它和被测方共用那条路）"
  else
    fail "[$arm] 观察者 $nsamp 次里空读 ${emp:-?} 次、收尾累计读=${final:-空} —— 属 ①：这一臂的 ejected/valve 读数作废，先修 --max-time 再谈结论"
  fi
done

# clean 臂：不注入就不该摘，且三台都在服务（round_robin 的配置下界）
if [[ -s "$OUT/clean/summary.txt" ]]; then
  ej=$(sum metric_ejections_sum clean); valve=$(sum metric_valve clean); seq1=$(sum served_sequence clean)
  p50=$(sum lat_p50_ms clean)
  if [[ "${ej:-0}" == "0" && "${valve:-0}" == "0" ]]; then
    ok "[clean] 逐目标 ejections 之和=0、valve=0（不注入就没有摘除，注入臂的红因此不能赖配置本身）"
  else
    fail "[clean] 对照臂就摘了 ej=${ej:-?}、valve=${valve:-?}：配置或机器有问题，注入臂的对照作废"
  fi
  n1=$(printf '%s' "$seq1" | tr -cd '1' | wc -c); n2=$(printf '%s' "$seq1" | tr -cd '2' | wc -c); n3=$(printf '%s' "$seq1" | tr -cd '3' | wc -c)
  if (( n1 > 0 && n2 > 0 && n3 > 0 )); then ok "[clean] 三台都被服务过（$n1/$n2/$n3 条，序列 $seq1）——round_robin 在 ns 里正常轮换"
  else fail "[clean] 有成员一条都没被服务：序列 $seq1"; fi
  (( p50 <= 50 )) && ok "[clean] 单条 p50=${p50} ms（回环无注入的量级，是注入臂那几格的分母）" \
                   || fail "[clean] p50=${p50} ms 高得不像没注入"
fi

# delay 50ms：**修正后模型的证伪臂**（4×50=200 < probe_timeout 300 → 该不摘）
if [[ -s "$OUT/d50/summary.txt" ]]; then
  ej=$(sum metric_ejections_sum d50); valve=$(sum metric_valve d50); p50=$(sum lat_p50_ms d50)
  if [[ "${ej:-0}" == "0" && "${valve:-0}" == "0" ]]; then
    ok "[d50] ejections 之和=0、valve=0 —— 修正后的模型过了这一格：**网络层 delay 的分界线在 ≈probe_timeout/4（75ms），不是 300ms**"
  else
    fail "[d50] 50ms 档（4×50=200 < 300）也摘了 ej=${ej:-?} valve=${valve:-?} —— 连 /4 这个模型也不对，得回去看等效耗时到底怎么算的"
  fi
  (( p50 >= 200 )) && ok "[d50] 单条 p50=${p50} ms ≥ 4×50" || fail "[d50] p50=${p50} ms 连 4×50 都不到，读不出注入"
fi

# delay 100ms：**这里我原先的预测被实测否掉了，照记 + 归因**
# 原预测（认领那一轮写下的）：100ms < probe_timeout 300ms → 与 §12 类 4b 同形，不摘。
# 实测：ej=1、valve=27 —— **摘了**。归因：应用层 `--delay-ms` 只加在"上游回话"那一段，
# 探活的等效耗时 ≈ 2×delay；而网络层 delay 加在**每一次单向穿越**上，一次探活要走
# 建连（SYN/SYN-ACK）+ 请求 + 响应 = 4 次穿越 ≈ 4×delay，所以 400ms > 300ms 判死。
# 也就是说"慢与死的分界线就是 probe_timeout"这条判据**仍然成立**，但**横轴换了单位**：
# 网络层档的分界线在 delay ≈ probe_timeout/4，而不是 probe_timeout。d50 臂就是为证伪这句话加的。
if [[ -s "$OUT/d100/summary.txt" ]]; then
  ej=$(sum metric_ejections_sum d100); valve=$(sum metric_valve d100)
  stats=$(sum qdisc_stats d100); p50=$(sum lat_p50_ms d100)
  sent=$(printf '%s' "$stats" | awk '{print $4}')
  if [[ -n "$sent" ]] && (( sent > 0 )); then ok "[d100] qdisc 自数 Sent=${sent} pkt（下界：注入真的发生过，不是空挂一个 qdisc）"
  else fail "[d100] qdisc 没有发包计数：'$stats'"; fi
  if [[ "${ej:-0}" != "0" ]]; then
    ok "[d100] ejections 之和=$ej（valve=$valve）—— **与原预测相反，摘了**；按上面的归因读：判据没坏，是横轴的单位变了（4×100=400 已越过 300）"
  else
    fail "[d100] 这一臂没摘（ej=${ej:-空}）——与本轮第一次实测（ej=1、valve=27）不同，形状变了要重查，别把一次读数当永恒事实"
  fi
  (( p50 >= 400 )) && ok "[d100] 单条 p50=${p50} ms ≥ 4×100（经网关一条请求至少 4 次单向穿越；直连只 2 次——**网络层注入对「经网关」的放大至少是 2 倍**，S5-07 容器对照臂要带着这个口径）" \
                   || fail "[d100] p50=${p50} ms 连 4×delay 都不到：注入没落到这条路径上，或计时取错了"
fi

# delay 400ms：类 4a 的网络层对照——三台一起超 probe_timeout → 安全阀这一格
if [[ -s "$OUT/d400/summary.txt" ]]; then
  ej=$(sum metric_ejections_sum d400); valve=$(sum metric_valve d400)
  ejmax=$(sum metric_ejected_now_max d400); denied=$(sum metric_denied d400)
  cf=$(sum upstream_connect_failed d400); codes=$(sum http_codes d400); p50=$(sum lat_p50_ms d400)
  if [[ "${valve:-0}" != "0" ]]; then ok "[d400] health_ejections_blocked_by_valve=$valve（退出账读回 $valve→$(sum exit_valve d400)）—— 应用层矩阵从没造出的形状：**整条回环一起慢 → 健康的两台也超 probe_timeout → ADR-12 的阀在挡摘除**"
  else fail "[d400] 400ms 档（>300ms）却没触发安全阀（valve=${valve:-空}）：预期落空，需要归因"; fi
  if [[ "${ej:-0}" != "0" ]]; then ok "[d400] 逐目标 ejections 之和=$ej、ejected_now 峰值 $ejmax、all_denied_fallback=${denied:-0}（摘除与阀同时有数：不是『全都摘干净』而是『摘到上限为止』）"
  else fail "[d400] 一次都没摘（ej=${ej:-空}）而阀也没挡（valve=${valve:-空}）：这一臂等于什么都没发生"; fi
  printf '%s' "$codes" | grep -q '504' && fail "[d400] 出现了 504：400ms 延迟把响应头限期（10s）打穿了，得归因" \
                                      || ok "[d400] 状态码序列里没有 504（$codes）——慢不等于超时"
  (( p50 >= 1600 )) && ok "[d400] 单条 p50=${p50} ms ≥ 4×400（与 d100 同一把尺，线性可外推：注入是逐跳加的不是打包加的）" \
                    || fail "[d400] p50=${p50} ms 连 4×400 都不到，与 d100 臂不同尺"
  note "[d400] upstream_connect_failed=${cf:-?}（网络层给的是延迟不是拒绝，这一格应当为 0；不为 0 就说明 SYN 也被拖过了 connect 超时）"
fi

# loss 20%：TCP 重传把丢包藏成延迟
if [[ -s "$OUT/l20/summary.txt" ]]; then
  stats=$(sum qdisc_stats l20); codes=$(sum http_codes l20)
  drop=$(printf '%s' "$stats" | sed -n 's/.*dropped \([0-9]*\).*/\1/p')
  ej=$(sum metric_ejections_sum l20)
  if [[ -n "$drop" ]] && (( drop > 0 )); then ok "[l20] qdisc 自数 dropped=$drop（下界：丢包真的发生了；原始串「$stats」）"
  else fail "[l20] dropped 读不到或为 0：'$stats' —— 这一臂没注入成功"; fi
  n200=$(printf '%s' "$codes" | grep -o '200' | wc -l | tr -d ' ')
  if (( n200 > 0 )); then ok "[l20] 丢包之下客户端仍拿到 $n200 个 200（TCP 重传把 loss 藏成了延迟，这就是它与应用层『不回字节』的区别）"
  else fail "[l20] 一个 200 都没拿到：$codes"; fi
  # 机制句按实测改过：原先写"重传后响应仍在 probe_timeout 内（所以不摘）"，
  # 而这一臂实测 ejections 之和=18 —— **说反了**。丢包经 TCP 重传（Linux RTO 起算 ~200 ms 且翻倍）
  # 把探活的等效耗时推过 300 ms，于是探活判死；业务侧却几乎全是 200（重传对应用透明）。
  # 这一对反差本身是 S5-06 的产出：**探活这条路比业务更怕丢包**。
  note "[l20] 逐目标 ejections 之和=${ej:-?}（丢包经重传把探活推过 probe_timeout → 摘；同一臂业务侧仍拿到若干 200，见上一条。这句在 2026-11-04 被实测改写过，原句见 §25.2）"
fi

# delay 1500ms：类 3 的网络层对照（连得上、字节迟迟不到）
if [[ -s "$OUT/d1500/summary.txt" ]]; then
  p50=$(sum lat_p50_ms d1500); max=$(sum lat_max_ms d1500); codes=$(sum http_codes d1500)
  ej=$(sum metric_ejections_sum d1500); valve=$(sum metric_valve d1500)
  (( p50 >= 6000 )) && ok "[d1500] 单条 p50=${p50} ms、max=${max} ms（≥4×1500——**连得上但字节迟迟不到**，这是 §12 类 3『连上不回字节』的网络层版本：客户端视角同形，机制完全不同）" \
                    || fail "[d1500] p50=${p50} ms 连 4×1500 都不到，读不出注入"
  printf '%s' "$codes" | grep -q '504' && note "[d1500] 出现 504：注入把响应头限期（10s）打穿了——照记不掩盖，这一档的 504 来自限期不是来自上游不回话" \
                                      || ok "[d1500] 状态码里没有 504（$codes）——默认 10s 限期在这一档仍站得住"
  note "[d1500] ejections 之和=${ej:-?}、valve=${valve:-?}（这一档探活必然超时，摘除与阀的读数与 d400 同形）"
fi

# ---------------- 收尾：宿主没被动过 ----------------
echo "-- 收尾"
HOST_QDISC1=$(tc qdisc show dev lo 2>/dev/null | head -1)
if [[ "$HOST_QDISC1" == "$HOST_QDISC0" ]]; then ok "宿主回环仍是 $HOST_QDISC1（与开工前逐字相同，注入只活在那些 ns 里）"
else fail "宿主回环从 $HOST_QDISC0 变成 $HOST_QDISC1 —— 本脚本绝不该动它"; fi
LEFT=$(ss -ltn 2>/dev/null | grep -c ':1895' || true)
(( LEFT == 0 )) && ok "宿主上没有 1895x 残留监听" || fail "1895x 还有 $LEFT 个监听没退"

echo "== 汇总：$CHECKS 项判定，✗ 计数 $FAILS =="
verdict=PASS; (( FAILED )) && verdict=FAIL
echo "$verdict checks=$CHECKS fails=$FAILS" > "$OUT/verdict.txt"
echo "S5-06 $verdict（同一句判定写在 $OUT/verdict.txt）"
exit "$FAILED"
