#!/usr/bin/env bash
# S5-12：netem 的 **loss / delay 阈值扫描**——结掉 FAULT-INJECTION §25.5 白纸黑字欠着的那两格：
#   ① 「d50 是模型的单点证伪：`probe_timeout/4` 这条界只用 50 与 100 两点夹住，没扫到 75 ms 附近」
#      → delay 轴 60/70/75/80/90 五个点，把分界线夹进一对相邻点之间；
#   ② 「loss 与摘除之间没做阈值扫描：l20 臂 ej=18 是单点读数，没量多大 loss 开始让探活判死，
#      也没取 recoveries 那一族来拆那 18 次里有几次是恢复后再摘的」
#      → loss 轴 2/5/10/15/20/25 六个点 + 逐目标 recoveries 读数，用恒等式
#        `ej_total == 被摘过的目标数 + rec_total` 把首次摘除与恢复后再摘拆开。
#
# 判据里带**可证伪的预测**（不是"扫一遍看看"）：4×delay 那条模型说 60/70 该 ej=0、80/90 该 ej>0
# （75 是 4×75=300 与 probe_timeout 恰等，模型本身不表态，只作夹逼点）。
# 如果实测是"60/70/80 全不摘、要到 150 才摘"，那说明探活走的是**池化连接**、单向穿越是 2 次不是 4 次
# ——那是要照记 + 归因的形状，不是把预测改一改了事。
#
# 用法：PRESET=release ROUND=r1 scripts/fault/s5-12-threshold.sh
#   ARMS="d60 d90" 只跑某几臂（阳性验证与复跑用这个旋钮）；OUT_ROOT 换落盘根。
# 判定落盘 **build/fault/s5-12/$ROUND/verdict.txt**（§22.2 第 5 条那一族：真子进程 + 真 ns 的形状下 rc 不可信）。
set -uo pipefail
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1
trap '' PIPE

PRESET="${PRESET:-release}"
ROUND="${ROUND:-r1}"
[[ -n "$ROUND" ]] || { echo "ROUND 不许为空（它决定落盘目录，空值会让 rm -rf 把两轮一起吃掉）"; exit 1; }
OUT_ROOT="${OUT_ROOT:-build/fault/s5-12}"
OUT="$OUT_ROOT/$ROUND"
ARM="scripts/fault/s5-12-arm.sh"
NS='unshare -Un --map-root-user --propagation private --'
OBS_MS="${OBS_MS:-8000}"
MTIME="${MTIME:-6}"
REQUESTS="${REQUESTS:-12}"

rm -rf "$OUT"; mkdir -p "$OUT"
FAILED=0; FAILS=0; CHECKS=0
ok()   { echo "  ✓ $*"; CHECKS=$((CHECKS + 1)); }
fail() { echo "  ✗ $*"; FAILED=1; FAILS=$((FAILS + 1)); CHECKS=$((CHECKS + 1)); }
note() { echo "  · $*"; }
die()  { echo "  fatal: $*" >&2; echo "FAIL checks=$CHECKS fails=$((FAILS + 1)) die=1" > "$OUT/verdict.txt"; exit 1; }
sum() { sed -n "s/^$1=//p" "$OUT/$2/summary.txt" 2>/dev/null | head -1; }

echo "== S5-12 netem 阈值扫描（PRESET=$PRESET ROUND=$ROUND requests=$REQUESTS obs=${OBS_MS}ms mtime=${MTIME}s）=="
echo "START $(date +%F_%T)" > "$OUT/run.log"

# ---------------- 阶段 0：前置 ----------------
echo "-- 阶段 0：前置"
for b in "build/$PRESET/l7gate" "build/$PRESET/tests/l7gate_test_upstream"; do
  [[ -x "$b" ]] || die "找不到 $b —— 先 cmake --build --preset $PRESET"
done
# **陈旧二进制门**（2026-11-05 我自己撞出来的那一族：`build exit=0` 在没活可干时也是 0，
# 所以"三 preset 全绿"与"这一臂的读数"都可能是在引用旧产物）。这里不靠 make 的自觉，直接问一次：
STALE=$(find src tests -type f -newer "build/$PRESET/l7gate" 2>/dev/null | head -3 | tr '\n' ' ')
if [[ -n "$STALE" ]]; then
  die "二进制比源码旧：$PRESET 的产物早于这些文件（$STALE）—— 先 cmake --build --preset $PRESET 再扫，否则整轮读的是旧行为"
fi
ok "产物不早于任何 src/tests 文件（$PRESET 的 l7gate 可以引用为当前行为）"
command -v tc >/dev/null || die "本机没有 tc（iproute2）"
$NS bash -c 'ip link set dev lo up; tc qdisc add dev lo root netem delay 10ms 2>&1 | head -1;
             tc qdisc show dev lo | head -1; id -u; grep CapEff /proc/self/status' \
   > "$OUT/availability.txt" 2>&1
if grep -q 'netem' "$OUT/availability.txt" && grep -q '^0$' "$OUT/availability.txt"; then
  ok "非特权 user+netns 里 netem 能加、能读回：$(grep '^qdisc' "$OUT/availability.txt" | head -1)"
else
  echo "  这条路不通（原样读数在 $OUT/availability.txt）。照 S5-06 验收句②：**先写 ADR 再换手段，不许悄悄换成没做**。"
  echo "FAIL checks=$CHECKS fails=0 refused=1" > "$OUT/verdict.txt"
  exit 1
fi
HOST_QDISC0=$(tc qdisc show dev lo 2>/dev/null | head -1)
echo "$HOST_QDISC0" | grep -q 'noqueue' && ok "宿主回环是 $HOST_QDISC0（注入只活在每臂自己的 ns 里）" \
                                        || fail "宿主回环不是 noqueue（$HOST_QDISC0）：先查是谁挂过 qdisc，本轮不该在这种地基上跑"

# ---------------- 两轴 ----------------
ARMS="${ARMS:-clean d60 d70 d75 d80 d90 l02 l05 l10 l15 l20 l25}"
declare -A SPEC=( [clean]="" [d60]="delay 60ms" [d70]="delay 70ms" [d75]="delay 75ms"
                  [d80]="delay 80ms" [d90]="delay 90ms"
                  [l02]="loss 2% limit 1000" [l05]="loss 5% limit 1000" [l10]="loss 10% limit 1000"
                  [l15]="loss 15% limit 1000" [l20]="loss 20% limit 1000" [l25]="loss 25% limit 1000" )
declare -A AXIS=( [clean]=delay [d60]=delay [d70]=delay [d75]=delay [d80]=delay [d90]=delay
                  [l02]=loss [l05]=loss [l10]=loss [l15]=loss [l20]=loss [l25]=loss )
declare -A DELAY_MS=( [clean]=0 [d60]=60 [d70]=70 [d75]=75 [d80]=80 [d90]=90 )
declare -A LOSS_PCT=( [l02]=2 [l05]=5 [l10]=10 [l15]=15 [l20]=20 [l25]=25 )

for arm in $ARMS; do
  [[ -n "${SPEC[$arm]+x}" ]] || die "不认识的臂名 $arm（可选：${!SPEC[*]}）"
  echo "-- 臂 $arm：netem '${SPEC[$arm]:-<不加>}'"
  PRESET="$PRESET" $NS bash "$ARM" "${SPEC[$arm]}" "$REQUESTS" "$OUT/$arm" "$OBS_MS" "$MTIME" \
      > "$OUT/$arm.out" 2>&1
  rc=$?
  if (( rc != 0 )); then fail "臂 $arm 跑失败 rc=$rc（$OUT/$arm.out）"; continue; fi
  [[ -s "$OUT/$arm/summary.txt" ]] || { fail "臂 $arm 没落 summary.txt"; continue; }
  ok "臂 $arm 跑完（ej=$(sum metric_ejections_sum $arm) rec=$(sum metric_recoveries_sum $arm) 寿命=$(sum arm_lifetime_ms $arm)ms）"
done

# ---------------- 判定 A：每一臂都要过的四道通用门 ----------------
echo "-- 判定 A：通用门（键名下界、注入下界、观测窗、生命周期闭合）"
for arm in $ARMS; do
  [[ -s "$OUT/$arm/summary.txt" ]] || continue
  # A1 键名下界：**上界（不该摘）必须配一条"我读得动这个键"**。recoveries 是本轮新引的族，
  #    所以它的守卫与 ejections 同规格——读回空串与"确实没恢复"在数上不可区分（§25.3 的 D3 就是这一格）。
  # 探测求和对**每一臂**都算（界②的分母就在这两格里；上一版只在 loss 循环里写，delay 臂读不到）
  pksum=$(awk -F/ '{s=0; for(i=1;i<=NF;i++) s+=$i+0; print s}' <<< "$(sum target_probe_ok $arm)")
  pfsum=$(awk -F/ '{s=0; for(i=1;i<=NF;i++) s+=$i+0; print s}' <<< "$(sum target_probe_fail $arm)")
  { echo "probe_ok_sum=${pksum:-0}"; echo "probe_fail_sum=${pfsum:-0}"; } >> "$OUT/$arm/summary.txt"
  kre=$(sum metric_ejection_key_reads $arm); krr=$(sum metric_recovery_key_reads $arm)
  sc=$(sum final_scrape_complete $arm); sct=$(sum final_scrape_tries $arm)
  if [[ "${kre:-?}" == "3" && "${krr:-?}" == "3" && "${sc:-0}" == "1" ]]; then
    ok "[$arm] 终局是**一次抓取**读齐 6 个键（ej=${kre}/3 rec=${krr}/3，抓了 ${sct} 次成）——ej 与 rec 取自同一份文本，跨事件错读的形状被排除"
  else
    fail "[$arm] 终局抓取不合格（ej=${kre:-?}/3 rec=${krr:-?}/3 complete=${sc:-0} tries=${sct:-?}）：这一臂的 ej/rec 全部不作数——键读空会把求和算小，跨抓取会把 rec 读到 ej 前面去"
  fi
  # A2 注入下界：qdisc 自己的计数（`-s` 的统计在第二行，S5-06 §25.2 那一格）。
  #    **对照臂没有 qdisc，这一格不该套在它头上**（干跑时 `Sent 0 bytes 0 pkt` 把 clean 判红了）。
  st=$(sum qdisc_stats $arm)
  sent=$(printf '%s' "$st" | awk '{print $4}'); drop=$(printf '%s' "$st" | sed -n 's/.*dropped \([0-9]*\).*/\1/p')
  if [[ -z "${SPEC[$arm]}" ]]; then
    ok "[$arm] 对照臂不注入，注入下界这一格按定义跳过（它的作用是证明别的臂真的注入了）"
  elif [[ "${AXIS[$arm]}" == "loss" ]]; then
    [[ -n "$drop" && "$drop" -gt 0 ]] && ok "[$arm] qdisc 自数 dropped=$drop（丢包真发生过）" \
      || fail "[$arm] loss 臂 dropped 读不到或为 0（原始串「$st」）：这一臂没注入成功，它的 ej 不能当阈值点用"
  else
    [[ -n "$sent" && "$sent" -gt 0 ]] && ok "[$arm] qdisc 自数 Sent=${sent} pkt（注入真发生过；取的是第 4 格 pkt，第 2 格是 bytes——§25.2 那一格我这次没重犯）" \
      || fail "[$arm] qdisc 没有发包计数（原始串「$st」）"
  fi
  # A3 观测窗：跨臂比 ej 必须先比暴露时长，否则读的是"谁跑得久"。
  #    窗口的**超量最多等于一次卡住的抓取**（采样循环每轮先发一条 curl，上限就是 MTIME），
  #    所以可解释的区间是 `OBS_MS ≤ 窗 < OBS_MS + MTIME + 0.4s`；上一版写成 ±25%，
  #    于是 25% 丢包那一臂因为一次抓取慢了就判红——那是仪器的形状，不是被测方的。
  oa=$(sum obs_ms_actual $arm); os=$(sum obs_samples $arm)
  lo=$((OBS_MS)); hi=$((OBS_MS + MTIME * 1000 + 400))
  if [[ -n "$oa" ]] && (( oa >= lo && oa < hi )); then
    ok "[$arm] 观测窗 ${oa} ms 落在可解释区间 [$lo, $hi)（采样 ${os} 次）——这一臂的 ej 与同轴其他臂可比"
  else
    fail "[$arm] 观测窗 ${oa:-?} ms 越出可解释区间 [$lo, $hi)：它的 ej 不进阈值判定（超量说明不止一次抓取被卡住，得先查仪器）"
  fi
  # A4 生命周期闭合（与 S5-06 同一把尺：fd 那一格按 ARCH 1.31 边界⑦ 分两档）
  live=$(sum live $arm); con=$(sum constructed $arm); des=$(sum destructed $arm)
  fd0=$(sum fd_start $arm); fd1=$(sum fd_end $arm)
  if [[ "$live" == "0" ]]; then ok "[$arm] 退出账 live_connections=0"; else fail "[$arm] live=${live:-空}"; fi
  if [[ -n "$con" && "$con" == "$des" ]]; then ok "[$arm] constructed==destructed==$con"
  else fail "[$arm] constructed=${con:-空} destructed=${des:-空}"; fi
  gap=$(( ${fd1:-0} - ${fd0:-0} ))
  if (( gap >= 0 && gap <= 4 )); then ok "[$arm] fd $fd0 → $fd1（差 $gap，边界⑦ 的「不相等那一档」上界 4）"
  else fail "[$arm] fd $fd0 → $fd1（差 $gap）超出可解释范围：得归因"; fi
  # A5 摘除/恢复的**结构不变式**。一台目标的事件序列只能是 `E R E R …`（先摘、恢复、才可能再摘），
  # 所以对每台 `ej_i − rec_i ∈ {0,1}`：0 = 退出时已经恢复，1 = 退出时仍在册。
  # 于是「恢复后再摘」的次数 = `ej_total − 被摘过的目标数`，**不是** `recoveries` 本身——
  # 这一格我第一版写成 `ej == 被摘目标数 + recoveries`，干跑的 d80（ej=1、rec=1、被摘 1 台）当场把它打红了：
  # 那一次恢复后面并没有跟着再摘。语义假设错了要改判据，不是改读数（照记在 §29）。
  ej=$(sum metric_ejections_sum $arm); rec=$(sum metric_recoveries_sum $arm); ne=$(sum metric_targets_with_ejections $arm)
  IFS='/' read -r -a A_EJ <<< "$(sum target_ejections $arm)"
  IFS='/' read -r -a A_REC <<< "$(sum target_recoveries $arm)"
  if [[ "$kre" == "3" && "$krr" == "3" && -n "$ej" && -n "$rec" && -n "$ne" ]]; then
    bad=0; detail=""
    for i in 0 1 2; do
      d=$(( ${A_EJ[i]:-0} - ${A_REC[i]:-0} ))
      detail="$detail t$i:ej=${A_EJ[i]:-?}/rec=${A_REC[i]:-?}"
      (( d == 0 || d == 1 )) || bad=$((bad + 1))
    done
    re_ej=$(( ej - ne )); stayed=$(( rec - re_ej ))
    if (( bad == 0 && re_ej >= 0 && stayed >= 0 )); then
      ok "[$arm] 序列不变式成立（逐台 ej−rec ∈ {0,1}：$detail）→ ej=${ej} 里首次摘除 ${ne} 次、**恢复后再摘 ${re_ej} 次**、恢复后没再摘（退出时在册外）${stayed} 次"
    else
      fail "[$arm] 序列不变式不成立或拆分算不通（$detail，ej=${ej} 被摘 ${ne} 台 rec=${rec}）：bad=${bad} 再摘=${re_ej} 未再摘=${stayed} —— 说明 recoveries/ejections 的语义不是我假设的"那个目标摘↔恢复交替"，得去读 src/route 那两侧的实现再谈拆分，别硬套"
    fi
  fi
done

# ---------------- 判定 B：delay 轴（分界线落在"探测过期"那一层，不是"摘除"那一层） ----------------
echo "-- 判定 B：delay 轴（4×delay 与 probe_timeout=300 的分界 + 摘除的二次放大）"
has() { [[ -s "$OUT/$1/summary.txt" ]]; }
# 三个层次分清楚，这一格才讲得通：
#   ① 单条请求的往返 = 4×delay + 基线（业务与探活共用那条被注入的回环）；
#   ② **一次探活过不过期** = 4×delay 与 probe_timeout=300 的比较 —— 这一层是**阶跃**；
#   ③ 摘不摘除 = 要连到 `max_fails=2` 次过期才发生 —— 这一层是 ② 的**二次函数**，
#      带内（4×delay≈300）单点跨轮必然翻动：r1 的 d80 有 12 次过期却 ej=0，r2 的 d80 是 ej=2。
# 上一版把断言写在 ③ 上（"4×80=320>300 却没摘"因此报红），是把③当阶跃；本轮改成②做硬断言、③只记录，
# 并在 ③ 上只断言 d90（6 次读数里每轮都摘）。这不是迁就读数：② 的两条在两轮里都干净成立。
if has clean; then
  ej=$(sum metric_ejections_sum clean); pk=$(sum probe_ok_sum clean); pf=$(sum probe_fail_sum clean)
  if [[ "${ej:-1}" == "0" && "${pf:-?}" == "0" ]]; then
    ok "[delay 轴对照 clean] ej=0 且 probe_fail=0（探测 ok=${pk:-?} 次）：不注入就没有过期也没有摘除，注入臂的红因此不能赖配置本身"
  else
    fail "[delay 轴对照 clean] ej=${ej:-?} 而 probe_fail=${pf:-?}：对照臂就坏了，两轴的对照作废"
  fi
fi
for arm in d60 d70; do
  has "$arm" || continue
  d=${DELAY_MS[$arm]}; pf=$(sum probe_fail_sum "$arm"); pk=$(sum probe_ok_sum "$arm")
  ej=$(sum metric_ejections_sum "$arm"); p50=$(sum lat_p50_ms "$arm")
  if [[ "${pf:-?}" == "0" ]]; then
    ok "[${arm}] 4×${d}=$((4 * d)) ms < 300 → **一次探活都不该过期**，实测 probe_fail=0（探测 ok=${pk:-?} 次）——分界线这一层是阶跃，这一臂站在界内"
  else
    fail "[${arm}] 4×${d}=$((4 * d)) ms < 300 却过期了 ${pf} 次：等效单向穿越多于 4 次（或延迟不止作用在探活那条路上）——界比 /4 更靠左，照记 + 归因"
  fi
  if [[ "${ej:-0}" == "0" ]]; then ok "[${arm}] 过期 0 次 ⇒ 摘除必为 0（实测 ej=0）"; else fail "[${arm}] 没过期却摘了 ej=${ej}：摘除另有来源，得回读 src/route/health.cc"; fi
  (( ${p50:-0} >= 4 * d )) && ok "[${arm}] 单条 p50=${p50} ms ≥ 4×${d}（与 §25 同一把尺）" \
                           || fail "[${arm}] p50=${p50} ms 连 4×${d} 都不到：注入没落到这条路径上，或计时取错了"
done
# 带内两点（75 恰等、80 略过 300）：只断言"有过期"，摘除作数据
for arm in d75 d80; do
  has "$arm" || continue
  d=${DELAY_MS[$arm]}; pf=$(sum probe_fail_sum "$arm"); pk=$(sum probe_ok_sum "$arm")
  ej=$(sum metric_ejections_sum "$arm"); rec=$(sum metric_recoveries_sum "$arm"); ne=$(sum metric_targets_with_ejections "$arm")
  if [[ "${arm}" == "d80" ]]; then
    if [[ -n "$pf" ]] && (( pf > 0 )); then ok "[d80] 4×80=320 ms > 300 → 探活该过期，实测 probe_fail=${pf} 次（ok=${pk}）——②这一层与模型同侧"
    else fail "[d80] 4×80=320 > 300 却零过期（probe_fail=${pf:-空}）：等效穿越少于 4 次，模型说早了，照记 + 归因"; fi
  else
    note "[d75] 4×75=300 与 probe_timeout **恰等**：等号归属没定义，只记录（实测 probe_fail=${pf:-?} 次 / ok=${pk:-?} 次，ej=${ej:-?}）"
  fi
  note "[${arm}] 摘除那一层（③）：ej=${ej:-?}、被摘过的目标 ${ne:-?}/3、recoveries=${rec:-?} —— **带内跨轮翻动是预期的**（③是②的二次函数：要连到 max_fails=2 次过期才摘），本条不拿它做达标判据"
done
for arm in d90; do
  has "$arm" || continue
  d=${DELAY_MS[$arm]}; pf=$(sum probe_fail_sum "$arm"); pk=$(sum probe_ok_sum "$arm")
  ej=$(sum metric_ejections_sum "$arm"); valve=$(sum metric_valve "$arm")
  if [[ -n "$pf" ]] && (( pf > 0 )); then ok "[d90] 4×90=360 ms > 300 → 探活过期 ${pf} 次（ok=${pk}）——②这一层与模型同侧"
  else fail "[d90] 4×90=360 > 300 却零过期：模型错了，界在 90 之上，照记 + 归因"; fi
  if [[ "${ej:-0}" != "0" ]]; then
    ok "[d90] 摘除那一层也兑现：ej=${ej}（valve=${valve}）——过期率高到连出 2 次时，摘除就稳定发生（§25 在 100ms 档读到 ej=1/valve=27 是同一件事）"
  else
    fail "[d90] 过期 ${pf} 次却没摘（ej=0，valve=${valve}）：③这一层与 §25 不同形，得回读 max_fails 的窗口语义"
  fi
done

# ---------------- 判定 C：loss 轴的阈值与拆分 ----------------
echo "-- 判定 C：loss 轴（多大 loss 开始让探活判死 + ej 里恢复后再摘占几次）"
LOSS_ORDER="l02 l05 l10 l15 l20 l25"
prev=""; nz=0; any_ej=0; ran=0; prev_drop=-1; drop_mono=1; bad_drop=""; rates=""; drops=""; nonmono=""
prev_ej=-1; prev_loss=0
for arm in $LOSS_ORDER; do
  has "$arm" || continue
  ej=$(sum metric_ejections_sum "$arm")
  [[ -z "$ej" ]] && continue
  ran=$((ran + 1))
  lt=$(sum arm_lifetime_ms "$arm")
  rate=$(awk -v e="$ej" -v ms="${lt:-1}" 'BEGIN{printf "%.3f", e/(ms/1000)}')
  dr=$(printf '%s' "$(sum qdisc_stats $arm)" | sed -n 's/.*dropped \([0-9]*\).*/\1/p')
  pksum=$(awk -F/ '{s=0; for(i=1;i<=NF;i++) s+=$i+0; print s}' <<< "$(sum target_probe_ok $arm)")
  pfsum=$(awk -F/ '{s=0; for(i=1;i<=NF;i++) s+=$i+0; print s}' <<< "$(sum target_probe_fail $arm)")
  death=$(awk -v e="$ej" -v n="$(( ${pksum:-0} + ${pfsum:-0} ))" 'BEGIN{ if (n+0 <= 0) { print "-1"; exit } printf "%.4f", e/n }')
  rates="$rates ${arm}=${rate}/s"
  drops="$drops ${arm}=${dr:-?}"
  if [[ -n "$dr" ]] && (( prev_drop >= 0 && dr < prev_drop )); then drop_mono=0; bad_drop="$bad_drop ${arm}(${prev_drop}→${dr})"; fi
  [[ -n "$dr" ]] && prev_drop=$dr
  if (( prev_ej >= 0 && ej < prev_ej )); then nonmono="$nonmono ${prev_loss}%→${LOSS_PCT[$arm]}%(${prev_ej}→${ej})"; fi
  prev_ej=$ej; prev_loss=${LOSS_PCT[$arm]}
  (( ej > 0 )) && any_ej=1
  (( ej == 0 )) && nz=$((nz + 1))
    sent=$(printf '%s' "$(sum qdisc_stats $arm)" | awk '{print $4}')
    frac=$(awk -v d="${dr:-0}" -v s="${sent:-0}" 'BEGIN{ if (d+s <= 0) { print "-1"; exit } printf "%.1f", 100*d/(d+s) }')
  note "[$arm] loss=${LOSS_PCT[$arm]}% ej=${ej}（逐目标 $(sum target_ejections $arm)）rec=$(sum metric_recoveries_sum $arm)（逐目标 $(sum target_recoveries $arm)）被摘过的目标 $(sum metric_targets_with_ejections $arm)/3 速率=${rate}/s 丢包率=${frac}% 探测次数=$(( ${pksum:-0} + ${pfsum:-0} ))（ok=${pksum:-?} fail=${pfsum:-?}）每次探测的死概率=${death} dropped=${dr:-?} valve=$(sum metric_valve $arm) 半开 admit/recov=$(sum target_half_open_admitted $arm)/$(sum target_half_open_recovered $arm) ej_now 序列=$(sum ejected_now_series $arm)"
  # 把两个求和键补写回 summary，表格与报告都按**数字**读，不再解析 a/b/c 串
done
# **`ran>0` 守卫**（§25.2 那一族，也是 `--gtest_filter` 那个坑的同族：0 条跑出来的「全绿」与「全红」都不能信）。
# 上一版没有它，干跑时「六臂全没摘」和「六臂全摘」两条互相矛盾的判定同时报红——因为两把尺都在数空集合。
if (( ran < 6 )); then
  note "[loss 轴] 本轮只有 $ran/6 臂有读数：**不宣布阈值、也不报「整轴同侧」**——轴不完整时这两句都没有意义（要整轴就 ARMS 不带限制重跑）"
else
  (( any_ej == 1 )) && ok "[loss 轴] 至少一臂摘了（ej>0 存在）——高强度那一侧有读数" \
                    || fail "[loss 轴] 六臂全没摘：整轴都在「不摘」那一侧 → **阈值没被夹住**，本轮不能宣布界在哪，得往更大的 loss 延轴"
  # **原写法作废**（r2 实测打红的就是这条）：上一版断「六臂里必须有一臂 ej=0」，
  # 也就是假定轴的低端**恰好**落在那个零上。机制不支持这个假定：一次探活被判死要 `max_fails=2` 次失败
  # 落进同一个窗口，2% 丢包下这是「小概率但不是零概率」——r1 的 l02 是 0、r2 的 l02 是 1。
  # 换成按速率分级：低端（2%/5%）的最大速率必须低于高端（20%/25%）的最大速率。
  read -r lo_max hi_max <<<"$(awk -v s="$rates" 'BEGIN{
      n = split(s, w, " "); lomax = -1; himax = -1
      for (i = 1; i <= n; i++) { if (w[i] == "") continue
        split(w[i], kv, "="); split(kv[2], v, "/"); val = v[1] + 0
        if (kv[1] == "l02" || kv[1] == "l05" || kv[1] == "l10") { if (val > lomax) lomax = val }
        if (kv[1] == "l20" || kv[1] == "l25") { if (val > himax) himax = val } }
      printf "%.3f %.3f\n", lomax, himax }')"
  if awk -v a="$lo_max" -v b="$hi_max" 'BEGIN{exit !(b > a && a >= 0)}'; then
    ok "[loss 轴] 低端（2/5/10%）最大摘除速率 ${lo_max}/s < 高端（20/25%）${hi_max}/s（逐臂速率：$rates）——轴是按**速率**分级的，不是按某个点恰好为零"
  else
    fail "[loss 轴] 低端速率没有低于高端（${lo_max} 对 ${hi_max}，逐臂：$rates）：这条轴量不出「loss 越大探活越容易死」，读数照记、结论降级"
  fi
  # **原写法作废**：上一版断的是「qdisc 自数 dropped 沿轴不减」。dropped 是**绝对数**，
  # 它跟这条臂跑了多少包绑在一起（r2 的 l20 `sent=2973`、l25 `sent=2008`），点位更高反而可能数更少，
  # r2 就因此报了一条假红。机制量是**丢包率** `dropped/(sent+dropped)`：它必须对得上配置点位，且沿轴不减。
  badfrac=0; detail=""; prevfrac=""; fracmono=1
  for arm in $LOSS_ORDER; do
    [[ -s "$OUT/$arm/summary.txt" ]] || continue
    dr=$(printf '%s' "$(sum qdisc_stats $arm)" | sed -n 's/.*dropped \([0-9]*\).*/\1/p')
    sn=$(printf '%s' "$(sum qdisc_stats $arm)" | awk '{print $4}')
    [[ -z "$dr" || -z "$sn" ]] && continue
    p=${LOSS_PCT[$arm]}
    # 一次判定要同时给出实测率与该样本量下的 3σ 容差：`dropped/(sent+dropped)` 是个二项比例，
    # n 只有几千，σ 本身就接近 1 个百分点。**上一版写死 ±2 个百分点**，25% 那点在 n=3079 时
    # σ≈0.80pp，实测偏离 2.1pp 就被判成"注入没按点位落"——那是容差不讲样本量，不是注入坏。
    read -r f tol <<EOF_FRAC
$(awk -v d="$dr" -v s="$sn" -v p="$p" 'BEGIN{
      n = d + s;
      if (n <= 0) { print "-1 0"; exit }
      ph = 100 * d / n;
      p0 = p / 100;
      sig = 100 * sqrt(p0 * (1 - p0) / n);
      printf "%.1f %.2f\n", ph, 3 * sig + 0.3
    }')
EOF_FRAC
    detail="$detail ${p}%->${f}%(±${tol:-0})"
    if [[ -z "$f" || "$f" == "-1" ]]; then badfrac=$((badfrac + 1)); continue; fi
    if ! awk -v x="$f" -v p="$p" -v t="$tol" 'BEGIN{d = x - p; if (d < 0) d = -d; exit !(d <= t)}'; then
      badfrac=$((badfrac + 1))
    fi
    if [[ -n "$prevfrac" ]]; then
      awk -v a="$prevfrac" -v b="$f" 'BEGIN{exit !(b >= a - 0.5)}' || fracmono=0
    fi
    prevfrac=$f
  done
  (( badfrac == 0 )) && ok "[loss 轴] 每臂 qdisc 自报丢包率都落在配置点位的 ±3σ 内（n 只有几千，σ 本来就接近 1 个百分点）：$detail" \
                     || fail "[loss 轴] 有臂的实测丢包率越出自身样本量的 ±3σ（$detail）：注入没按点位落到这条路上，先查仪器"
  (( fracmono == 1 )) && ok "[loss 轴] 丢包率沿轴不减（$detail）——仪器侧的下界站住了（上一版拿绝对 dropped 比，被 l20/l25 的假红打过）" \
                      || fail "[loss 轴] 丢包率沿轴出现下降（$detail）：这条轴的强度序列不可信，阈值结论降级"
  [[ -n "$nonmono" ]] && note "[loss 轴] 逐臂计数出现回退的位置：$nonmono —— 计数本来就不是单调量（同册最多 1 台、RTO 随机、各臂寿命不等），只作记录不报红"
fi

# ---------------- 判定 D：本轮的 ej=18 那一格拆完是什么形状 ----------------
if has l20; then
  ej=$(sum metric_ejections_sum l20); rec=$(sum metric_recoveries_sum l20); ne=$(sum metric_targets_with_ejections l20)
  kre20=$(sum metric_ejection_key_reads l20); krr20=$(sum metric_recovery_key_reads l20)
  if [[ -n "$ej" && -n "$rec" && -n "$ne" && "${kre20:-0}" == "3" && "${krr20:-0}" == "3" ]]; then
    if (( ej > 0 )); then
      ok "[l20 拆分] ej=${ej}（§25 那一轮记的是 18）：首次摘除 ${ne} 次（每台至多一次）、**恢复后再摘 $((ej - ne)) 次**；recoveries=${rec} 里 $((rec - (ej - ne))) 次后面没跟再摘 —— §25.5 欠的那格现在有数"
    else
      note "[l20 拆分] 本轮 l20 的 ej=0（与 §25 记的 18 不同形）：拆分对「零」没有意义，两数之差交给报告脚本判极差"
    fi
  else
    fail "[l20 拆分] 键名 reads=${kre20:-?}/3 与 ${krr20:-?}/3 或读数缺（ej=${ej:-空} rec=${rec:-空} n=${ne:-空}）：不拿这一臂做拆分"
  fi
fi

# ---------------- 收尾：宿主没被动过 ----------------
echo "-- 收尾"
HOST_QDISC1=$(tc qdisc show dev lo 2>/dev/null | head -1)
if [[ "$HOST_QDISC1" == "$HOST_QDISC0" ]]; then ok "宿主回环仍是 $HOST_QDISC1（与开工前逐字相同）"
else fail "宿主回环从 $HOST_QDISC0 变成 $HOST_QDISC1 —— 本脚本绝不该动它"; fi
LEFT=$(ss -ltn 2>/dev/null | grep -c ':1896' || true)
(( LEFT == 0 )) && ok "宿主上没有 1896x 残留监听" || fail "1896x 还有 $LEFT 个监听没退"
# **清场门要按本轮目录限定**：数"系统上有几个网关进程"在同机并发时必然假红（上一版就是这么红的，其实那是另一条作业的臂）。
# **不能用 `ps | grep -- "$OUT/"` 数进程**：那个 `grep` 自己的命令行里就带着 `$OUT`，而副本的落盘路径里含 `l7gate` 字样 ⇒ 它数到了自己（本轮的 d1/d3 臂就这么假红过一次）。`pgrep -f` 排除自己，且按网关独有的 `gw.json` 锁定，不误数别人的作业。
GWC=$(pgrep -f -- "$OUT/[^ ]*gw\.json" 2>/dev/null | wc -l | tr -d " ")
(( ${GWC:-0} == 0 )) && ok "本轮目录 $OUT 下没有漏下的网关进程（按 gw.json 路径锁定，pgrep 不自匹配，同机并发也不互相顶红）" \
                     || fail "还有 $GWC 个网关进程没退：本轮的 ns 是随进程死的，别把残留当清场干净"

# ---------------- 表格（报告脚本只读这一份，不再碰 summary） ----------------
{
  printf 'arm\tspec\taxis\tej\trec\tn_ej_targets\tejmax_now\tvalve\tdenied\tsent_pkt\tdropped\tp50\tmin\tmax\tlifetime_ms\tobs_actual_ms\tobs_samples\tempty_reads\thttp_codes\tserved\tseries\thalf_open_admitted\thalf_open_recovered\tprobe_ok_sum\tprobe_fail_sum\n'
  for arm in $ARMS; do
    has "$arm" || continue
    st=$(sum qdisc_stats $arm)
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
      "$arm" "${SPEC[$arm]:-<clean>}" "${AXIS[$arm]}" \
      "$(sum metric_ejections_sum $arm)" "$(sum metric_recoveries_sum $arm)" \
      "$(sum metric_targets_with_ejections $arm)" "$(sum metric_ejected_now_max $arm)" \
      "$(sum metric_valve $arm)" "$(sum metric_denied $arm)" \
      "$(printf '%s' "$st" | awk '{print $4}')" "$(printf '%s' "$st" | sed -n 's/.*dropped \([0-9]*\).*/\1/p')" \
      "$(sum lat_p50_ms $arm)" "$(sum lat_min_ms $arm)" "$(sum lat_max_ms $arm)" \
      "$(sum arm_lifetime_ms $arm)" "$(sum obs_ms_actual $arm)" "$(sum obs_samples $arm)" \
      "$(sum observer_empty_reads $arm)" "$(sum http_codes $arm)" "$(sum served_sequence $arm)" \
      "$(sum ejected_now_series $arm)" "$(sum target_half_open_admitted $arm)" "$(sum target_half_open_recovered $arm)" "$(sum probe_ok_sum $arm)" "$(sum probe_fail_sum $arm)"
  done
} > "$OUT/table.tsv"
echo "END $(date +%F_%T)" >> "$OUT/run.log"
echo "round=$ROUND preset=$PRESET arms='$ARMS' requests=$REQUESTS obs_ms=$OBS_MS mtime=$MTIME" > "$OUT/run.meta"

echo "== 汇总：$CHECKS 项判定，✗ 计数 $FAILS =="
verdict=PASS; (( FAILED )) && verdict=FAIL
echo "$verdict checks=$CHECKS fails=$FAILS" > "$OUT/verdict.txt"
echo "S5-12 $ROUND $verdict（同一句判定写在 $OUT/verdict.txt）"
exit "$FAILED"
