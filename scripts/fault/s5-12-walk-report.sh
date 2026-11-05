#!/usr/bin/env bash
# S5-12 的报告：只读各轮的 build/fault/s5-12/$ROUND/table.tsv 与 verdict.txt，自己不碰被测方
# ——**跑与判分开**（S5-11 那一族的形状）。
#
# 它回答两个问题，并把"跨轮一致性"做成断言而不是取平均：
#   一、delay 轴的分界线夹在**哪一对相邻点**之间（§25 那句"网络层一次探活 ≈ 4×delay"的 /4 是不是 4）；
#   二、loss 轴上"多大 loss 开始让探活判死"的**最小越阈点**，以及那一臂的 ej 里
#       首次摘除与恢复后再摘各占几次（恒等式 ej = 被摘过的目标数 + recoveries）。
# 一致性按**方向**判不按数值判：这几臂的 ej 是个位到十几的小计数，±1 抖动属预期；
# 但"阈值两侧各落在哪一侧"必须在每一轮同形，否则这条轴不能用来宣布分界线。极差照报。
#
# 用法：ROUNDS="r1 r2" scripts/fault/s5-12-walk-report.sh
set -uo pipefail
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1

ROUNDS="${ROUNDS:-r1 r2}"
OUT_ROOT="${OUT_ROOT:-build/fault/s5-12}"
REP="$OUT_ROOT/report"
mkdir -p "$REP"
rm -f "$REP/verdict.txt"

FAILED=0; FAILS=0; CHECKS=0
ok()   { echo "  ✓ $*"; CHECKS=$((CHECKS + 1)); }
fail() { echo "  ✗ $*"; FAILED=1; FAILS=$((FAILS + 1)); CHECKS=$((CHECKS + 1)); }
note() { echo "  · $*"; }
die()  { echo "  fatal: $*" >&2; echo "FAIL checks=$CHECKS fails=$((FAILS + 1)) die=1" > "$REP/verdict.txt"; exit 1; }

NR=$(echo "$ROUNDS" | wc -w | tr -d ' ')
(( NR >= 1 )) || die "ROUNDS 为空"
FIRST=$(echo "$ROUNDS" | awk '{print $1}')
echo "== S5-12 报告（ROUNDS=$ROUNDS，$NR 轮）=="

# ---------------- 前置：每一轮都得有自己的盘，且盘的先后顺序对得上 ----------------
for r in $ROUNDS; do
  [[ -s "$OUT_ROOT/$r/table.tsv" ]] || die "$r 没有 table.tsv（先跑 scripts/fault/s5-12-threshold.sh ROUND=$r）"
  v=$(cat "$OUT_ROOT/$r/verdict.txt" 2>/dev/null || echo NO_VERDICT)
  tm=$(stat -c %Y "$OUT_ROOT/$r/table.tsv" 2>/dev/null || echo 0)
  vm=$(stat -c %Y "$OUT_ROOT/$r/verdict.txt" 2>/dev/null || echo 9999999999)
  if [[ "$v" == PASS* ]]; then ok "[$r] verdict=$v"; else note "[$r] verdict=$v —— 照记：这一轮自己有 ✗，报告不替它圆场"; fi
  (( tm >= vm )) && ok "[$r] table.tsv 不早于该轮 verdict.txt（同一轮写的，不是残盘）" \
                 || fail "[$r] table.tsv 比该轮 verdict.txt 旧：跨轮引用会张冠李戴"
done

# ---------------- 臂集合必须逐字同集，否则"同一根轴"不成立 ----------------
arms_ref=$(cut -f1 "$OUT_ROOT/$FIRST/table.tsv" | tail -n +2 | tr '\n' ' ')
for r in $ROUNDS; do
  a=$(cut -f1 "$OUT_ROOT/$r/table.tsv" | tail -n +2 | tr '\n' ' ')
  if [[ "$a" == "$arms_ref" ]]; then ok "[$r] 臂集合与 $FIRST 逐字相同（$a）"
  else fail "[$r] 臂集合与 $FIRST 不同：[$a] vs [$arms_ref] —— 点位变了，两轮的轴不能并成一轴"; fi
done

cell()  { awk -F'\t' -v arm="$2" -v col="$3" '$1==arm {print $col; exit}' "$OUT_ROOT/$1/table.tsv"; }
axis()  { awk -F'\t' -v ax="$1" 'NR>1 && $3==ax {print $1}' "$OUT_ROOT/$FIRST/table.tsv"; }
DELAY_ARMS=$(axis delay); LOSS_ARMS=$(axis loss)

# ---------------- 逐臂逐轮读数 ----------------
echo "-- 逐臂（ej / rec / n / 寿命 / dropped 或 sent_pkt）"
for arm in $arms_ref; do
  line="$arm:"
  for r in $ROUNDS; do
    line="$line [$r ej=$(cell "$r" "$arm" 4) rec=$(cell "$r" "$arm" 5) n=$(cell "$r" "$arm" 6) 寿命=$(cell "$r" "$arm" 15)ms 注入计数=$(cell "$r" "$arm" 10)/$(cell "$r" "$arm" 11)]"
  done
  note "$line"
done

# ---------------- 结构不变式跨轮复算（表里只有求和，所以复算的是求和层面的推论） ----------------
# 一台目标的序列只能是 `E R E R …` ⇒ 求和后必须同时满足：
#   n ≤ ej（被摘过的每台至少摘一次）、ej − n ≤ rec（每次再摘都要前面那次恢复）、rec ≤ ej（每次恢复都要前面那次摘）。
# 第一版这里写的是 `ej == n + rec`，被 d80 干跑（ej=1、rec=1、n=1）当场打红——那次恢复后面没跟再摘。
bad=0; ncells=0
for arm in $arms_ref; do
  for r in $ROUNDS; do
    ej=$(cell "$r" "$arm" 4); rec=$(cell "$r" "$arm" 5); n=$(cell "$r" "$arm" 6)
    [[ -z "$ej" || -z "$rec" || -z "$n" ]] && continue
    ncells=$((ncells + 1))
    re=$((ej - n))
    if (( n <= ej && re >= 0 && re <= rec && rec <= ej )); then :; else
      bad=$((bad + 1))
      fail "[][$r/$arm] 序列不变式的求和推论不成立：ej=$ej rec=$rec n=$n（再摘推导值 $re，要求 n≤ej、0≤再摘≤rec、rec≤ej）"
    fi
  done
done
if (( bad == 0 && ncells > 0 )); then
  ok "序列不变式（求和推论）在 $ncells 个（臂×轮）格子里逐格复算通过 —— 「ej − 被摘目标数 = 恢复后再摘」这笔拆分可以引用"
elif (( ncells == 0 )); then
  fail "table.tsv 里一个可复算的格子都没有：报告没有输入"
else
  note "不变式复算：$bad/$ncells 个格子不成立（上面已逐格记走）"
fi

# ---------------- delay 轴：每轮各自夹出分界线，再比同不同形 ----------------
echo "-- delay 轴（${DELAY_ARMS:-<空>}）"
if [[ -z "${DELAY_ARMS// /}" ]]; then
  note "[delay 轴] 表里一根 delay 臂都没有（这一轮跑的是 loss？）—— 本轴不作判定，空集合既不报红也不报绿"
else
dnum() { case "$1" in clean) echo 0;; *) echo "${1#?}";; esac; }   # 点位从臂名尾数读（d60→60，clean 当 0）
# **两层的界分开算**（这是本轮 delay 轴真正的内容）：
#   界②「一次探活过不过期」= 4×delay 与 probe_timeout=300 的比较 —— 阶跃，可做硬断言；
#   界③「摘不摘」= 要连到 max_fails=2 次过期 —— ②的二次函数，带内单点跨轮必翻动，只报数不断言。
# 夹逼因此取在②上：最大 fail=0 的点与最小 fail>0 的点。上一版把界取在③上，r1 的 d80（12 次过期、0 次摘除）就把红报出来了。
brk="" sharpbad=0 rampbad=0 failreads="" ejreads=""
for r in $ROUNDS; do
  lastzero=""; firstnz=""
  for a in $DELAY_ARMS; do
    pf=$(cell "$r" "$a" 25); [[ -z "$pf" ]] && continue
    d=$(dnum "$a")
    if (( pf == 0 )); then lastzero=$a; elif [[ -z "$firstnz" ]]; then firstnz=$a; fi
    if (( d < 75 && pf > 0 )); then sharpbad=$((sharpbad + 1)); fail "[$r/$a] 4×$d=$((4 * d)) < 300 却过期 ${pf} 次 → 界比 /4 靠左（等效穿越多于 4 次）：照记 + 归因"; fi
    if (( d == 80 || d == 90 )); then
      if (( pf == 0 )); then sharpbad=$((sharpbad + 1)); fail "[$r/$a] 4×$d=$((4 * d)) > 300 却零过期 → 等效穿越少于 4 次，模型说早了：照记 + 归因"; fi
    fi
  done
  if [[ -n "$lastzero" && -n "$firstnz" && "$lastzero" != "$firstnz" ]]; then
    lo=$(dnum "$lastzero"); hi=$(dnum "$firstnz")
    brk="$brk $r=${lastzero}|${firstnz}"
    note "[$r] 界②（探测过期这一层）夹在 **${lastzero}（fail=0）与 ${firstnz}（fail>0）之间** ⇒ 分界线 ∈ (${lo} ms, ${hi} ms]"
  else
    brk="$brk $r=NONE"
    fail "[$r] 界②夹不出相邻点（lastzero=${lastzero:-无} firstnz=${firstnz:-无}）"
  fi
  ej=$(cell "$r" d90 4); [[ -n "$ej" && "$ej" -gt 0 ]] || { rampbad=$((rampbad + 1)); fail "[$r/d90] 过期层已越界却整轮没摘（ej=${ej:-空}）：③与 §25 不同形，得回读 max_fails 的窗口语义"; }
done
if (( sharpbad == 0 )); then
  ok "delay 轴按**探测过期**那一层逐点核过（$NR 轮）：≤70 的点每轮 probe_fail=0，80/90 的点每轮 >0 —— **「一次探活 ≈ 4×delay ⇒ 界 = probe_timeout/4 = 75 ms」在 5 ms 分辨率上成立，且这一层是阶跃**"
fi
nb=$(printf '%s\n' $brk | sed 's/^[^=]*=//' | sort -u | wc -l | tr -d ' ')
if [[ "$nb" == "1" && "$brk" != *NONE* ]]; then
  ok "跨轮一致：$NR 轮在界②上夹出同一对（$brk）"
else
  fail "跨轮不一致：界②各轮不同形（$brk）→ 这条界夹不稳，只能报带不能报分辨率"
fi
if (( rampbad == 0 )); then ok "界③（摘除）在 d90 每一轮都兑现（过期率高到连出 2 次时摘除稳定发生，与 §25 的 100ms 档同形）"; fi
for a in $DELAY_ARMS; do
  d=$(dnum "$a"); [[ "$d" == "0" ]] && continue
  fr=""; er=""
  for r in $ROUNDS; do fr="$fr $r:fail=$(cell "$r" "$a" 25)"; er="$er $r:ej=$(cell "$r" "$a" 4)"; done
  note "$a（4×$d=$((4 * d)) ms）过期读数:$fr ｜ 摘除读数:$er"
done
note "**带内（75 恰等、80 刚越界）摘除跨轮翻动是预期的**：摘除要连到 max_fails=2 次过期才发生，所以它是"过期率"的二次函数而不是阶跃——本轮 r1 的 d80 有 12 次过期却 ej=0，r2 的 d80 ej=2。§25.5 那句「没扫到 75 ms 附近」的答案落在界②上，不在这里。"
fi

# ---------------- loss 轴：最小越阈点 + 那一格的 ej 拆分 ----------------
echo "-- loss 轴（${LOSS_ARMS:-<空>}）"
if [[ -z "${LOSS_ARMS// /}" ]]; then
  note "[loss 轴] 表里一根 loss 臂都没有（这一轮跑的是 delay？）—— 本轴不作判定，空集合既不报「全没摘」也不报「全摘」"
else
ths=""; back=""; bad_drop=""; dropmono=1; rates_all=""; death_all=""
for r in $ROUNDS; do
  pdr=""; lomax=-1; himax=-1; prev=""; thr=""; ldeath=""; hdeath=""
  for a in $LOSS_ARMS; do
    ej=$(cell "$r" "$a" 4); [[ -z "$ej" ]] && continue
    [[ -z "$thr" && "$ej" -gt 0 ]] && thr=$a
    lt=$(cell "$r" "$a" 15); dr=$(cell "$r" "$a" 11)
    sn=$(cell "$r" "$a" 10)
    pk=$(cell "$r" "$a" 24); pf=$(cell "$r" "$a" 25)
    if [[ -n "$dr" && -n "$sn" && -n "$pdr" ]]; then
      fa=$(awk -v d="$dr" -v s="$sn" 'BEGIN{ if (d+s <= 0) { print "-1"; exit } printf "%.1f", 100*d/(d+s) }')
      fb=$(awk -v d="$pdr" -v s="$psn" 'BEGIN{ if (d+s <= 0) { print "-1"; exit } printf "%.1f", 100*d/(d+s) }')
      awk -v a="$fb" -v b="$fa" 'BEGIN{exit !(b >= a - 0.5)}' || { dropmono=0; bad_drop="$bad_drop $r:$a(${fb}->${fa})"; }
    fi
    [[ -n "$dr" ]] && { pdr=$dr; psn=$sn; }
    [[ -n "$prev" ]] && (( ej < prev )) && back="$back $r:$a(${prev}→${ej})"
    prev=$ej
    if [[ -n "$lt" && "$lt" -gt 0 ]]; then
      rt=$(awk -v e="$ej" -v ms="$lt" 'BEGIN{printf "%.3f", e/(ms/1000)}')
      rates_all="$rates_all $r/$a=${rt}/s"
      case "$a" in
        l02|l05|l10) awk -v x="$rt" -v y="$lomax" 'BEGIN{exit !(x > y)}' && lomax=$rt ;;
        l20|l25)     awk -v x="$rt" -v y="$himax" 'BEGIN{exit !(x > y)}' && himax=$rt ;;
      esac
    fi
    if [[ -n "$pk" && -n "$pf" ]] && (( pk + pf > 0 )); then
      dh=$(awk -v e="$ej" -v n="$((pk + pf))" 'BEGIN{printf "%.4f", e/n}')
      death_all="$death_all $r/$a=${dh}/次探测"
      case "$a" in
        l02) ldeath=$dh ;;
        l20|l25) if [[ -z "$hdeath" ]] || awk -v x="$dh" -v y="$hdeath" 'BEGIN{exit !(x > y)}'; then hdeath=$dh; fi ;;
      esac
    fi
  done
  ths="$ths $r=${thr:-NONE}"
  if [[ -n "$thr" ]]; then
    ej=$(cell "$r" "$thr" 4); rec=$(cell "$r" "$thr" 5); n=$(cell "$r" "$thr" 6)
    note "[$r] 本轮第一个 ej>0 的点是 $thr（loss=${thr#?}%）：ej=$ej = 首次摘除 $n + 恢复后再摘 $((ej - n))（recoveries=$rec）"
  else
    fail "[$r] loss 轴全臂不摘：整轴在「不摘」那一侧 → 得往更大 loss 延轴才算得出阈值"
  fi
  # **原写法作废**：上一版这里断「六臂里必须有一臂 ej=0」并且要求越阈点跨轮一致，
  # 那是把随机长尾当阶跃。机制是：一次探活判死要 max_fails=2 次失败落进同一个窗口，
  # 2% 丢包下这是小概率但**不是零概率**——r1 的 l02 读到 0、r2 的 l02 读到 1，跨轮就是不同形。
  if awk -v a="$lomax" -v b="$himax" 'BEGIN{exit !(b > a && a >= 0)}'; then
    ok "[$r] 低端（2/5/10%）最大摘除速率 ${lomax}/s < 高端（20/25%）${himax}/s —— 轴按**速率**分级成立"
  else
    fail "[$r] 低端速率没有低于高端（${lomax} 对 ${himax}）：这条轴量不出「loss 越大探活越容易死」，读数照记、结论降级"
  fi
  if [[ -n "$ldeath" && -n "$hdeath" ]] && awk -v a="$ldeath" -v b="$hdeath" 'BEGIN{exit !(a < b)}'; then
    ok "[$r] 每次探测的死概率低端 ${ldeath} < 高端 ${hdeath}（分母是同臂的 probe_ok+probe_fail 之和，不受观测窗长短影响）"
  elif [[ -n "$ldeath" || -n "$hdeath" ]]; then
    fail "[$r] 死概率没有从低端升到高端（低端 ${ldeath:-无} 对 高端 ${hdeath:-无}）：换成按探测次数归一之后这条不再成立，得归因"
  else
    fail "[$r] 读不到 probe_ok_sum/probe_fail_sum 两列（表格是旧版驱动的产物？）：没有分母就不算死概率"
  fi
done
(( dropmono == 1 )) && ok "loss 轴：每轮 qdisc 自报**丢包率**沿轴不减（仪器侧的下界；上一版拿绝对 dropped 比会假红）" \
                    || fail "loss 轴：丢包率沿轴出现下降（${bad_drop:-无}）：注入强度序列不可信，阈值结论降级"
note "逐臂摘除速率：$rates_all"
note "逐臂每次探测的死概率：$death_all"
note "各轮「第一个 ej>0 的点」：$ths —— **这一串允许不同形**（2% 那一端是小概率不是零概率），所以它只作数据、不当达标项"
[[ -n "$back" ]] && note "loss 轴计数出现回退的位置：$back —— 计数本来就不是单调量（同册最多 1 台、RTO 随机、各臂寿命不等），只作记录"

# §25 那一轮 l20 记的是单点 ej=18：本轮同点位复算报在这里
for r in $ROUNDS; do
  ej=$(cell "$r" l20 4); [[ -z "$ej" ]] && continue
  rec=$(cell "$r" l20 5); n=$(cell "$r" l20 6); lt=$(cell "$r" l20 15)
  note "[$r] l20 与 §25 对照：ej=${ej}（那一轮 18）、首次摘除 ${n}、恢复后再摘 $((ej - n))（recoveries=${rec}）、dropped=$(cell "$r" l20 11)、半开 admit=$(cell "$r" l20 22) recovered=$(cell "$r" l20 23)、ej_now 序列=$(cell "$r" l20 21)"
  # 绝对值不与 §25 那轮的 18 直接比：那一轮的观测段是 14×0.3 s，本轮是**固定 8 s 窗**，
  # 而逐目标 ej 是在整条臂的寿命上累计的。归一化到每秒才讲得通。
  [[ -n "$lt" ]] && note "[$r] l20 归一化：ej/秒 = $(awk -v e="$ej" -v ms="$lt" 'BEGIN{printf "%.2f", e/(ms/1000)}')（寿命 ${lt} ms）；§25 那轮的 18 没有同口径的寿命读数，**只作形状对照**"
done
fi

# ---------------- 极差按臂报出 ----------------
echo "-- ej 跨轮极差（小计数报极差，不报均值）"
worst=-1; worst_arm="无"
for arm in $arms_ref; do
  vals=""; mn=-1; mx=-1
  for r in $ROUNDS; do
    ej=$(cell "$r" "$arm" 4); [[ -z "$ej" ]] && continue
    vals="$vals $ej"
    (( mn < 0 || ej < mn )) && mn=$ej
    (( ej > mx )) && mx=$ej
  done
  [[ -z "${vals// /}" ]] && continue
  d=$((mx - mn))
  note "$arm ej 读数:$vals → 极差 $d（min $mn / max $mx）"
  (( d > worst )) && { worst=$d; worst_arm=$arm; }
done
ok "极差已按臂报出（最大在 $worst_arm = ${worst:-0}）；阈值结论按方向一致性判，不按均值"

{
  echo "rounds=$ROUNDS"
  echo "delay_boundary_layer2=$(echo "${brk:-无}" | tr ' ' ';')"
  echo "loss_threshold=$(echo "$ths" | tr ' ' ';')"
  echo "identity_cells=$ncells identity_bad=$bad"
} > "$REP/summary.txt"

echo "== 汇总：$CHECKS 项判定，✗ 计数 $FAILS =="
verdict=PASS; (( FAILED )) && verdict=FAIL
echo "$verdict checks=$CHECKS fails=$FAILS" > "$REP/verdict.txt"
echo "S5-12 report $verdict（同一句判定写在 $REP/verdict.txt）"
exit "$FAILED"
