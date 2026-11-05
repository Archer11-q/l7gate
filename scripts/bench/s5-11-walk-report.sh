#!/usr/bin/env bash
# S5-11 的判定：只从各臂落盘的 summary.txt / verdict.txt 取数，算 off/on 差、极差、单调性与分辨率门。
# 用法：bash scripts/bench/s5-11-walk-report.sh [矩阵目录]      （缺省 build/bench/s5-11-matrix）
set -uo pipefail
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1
BASE="${1:-build/bench/s5-11-matrix}"
TABLE="$BASE/table.tsv"
OUT="$BASE/report.txt"
VERD="$BASE/verdict.txt"
HZ=$(getconf CLK_TCK)
[[ -f "$TABLE" ]] || { echo "找不到 $TABLE：先跑 scripts/bench/s5-11-walk-matrix.sh"; exit 1; }

FAILED=0; CHECKS=0
ok()   { echo "  ✓ $*" | tee -a "$OUT"; CHECKS=$((CHECKS+1)); }
fail() { echo "  ✗ $*" | tee -a "$OUT" >&2; CHECKS=$((CHECKS+1)); FAILED=$((FAILED+1)); }
note() { echo "  $*" | tee -a "$OUT"; }

: > "$OUT"
note "口径声明：CPU 取数来自 /proc/<pid>/stat 的 utime+stime，本机 HZ=$HZ → 一格 = 10 ms。"
note "因此「差值 = 0 tick」只允许判成「这一档在本仪器分辨率之下」，不允许判成「没有成本」。"

# --- 1. 每臂合格性 ---------------------------------------------------------------
BAD=0; TOT=0
while IFS=$'\t' read -r arm n w r kind us td ws v est dir _rest; do
  [[ "$arm" == "arm" ]] && continue
  TOT=$((TOT+1))
  [[ "$v" == PASS* ]] || { BAD=$((BAD+1)); fail "$dir 判定=$v"
    continue; }
  [[ "$est" == "$n" ]] || { BAD=$((BAD+1)); fail "$dir established=$est ≠ 计划 $n（这一臂没有分母）"; }
  [[ -f "$dir/threads.delta.txt" ]] || { BAD=$((BAD+1)); fail "$dir 缺逐线程差值表（归因那一栏没人验过）"; }
  [[ "$arm" == "off" ]] || grep -q "^read_disarmed_connections=" "$dir/summary.txt" \
    || { BAD=$((BAD+1)); fail "$dir 的退出账读不到 read_disarmed_connections 那一格"; }
  # 臂身份门：两臂**必须**在退出账里留下不同形的指纹，否则 off/on 之差是空的
  if [[ "$arm" == "on" ]]; then
    grep -q "metrics_scrapes_ok=" "$dir/gateway.log" \
      || { BAD=$((BAD+1)); fail "$dir 标的是 on 臂，但退出账里没有 metrics_scrapes_ok —— 这一臂其实没开观测面"; }
  else
    grep -q "metrics_" "$dir/gateway.log" \
      && { BAD=$((BAD+1)); fail "$dir 标的是 off 臂，退出账里却出现 metrics_ 键 —— 两臂同形，差值没有对照"; }
  fi
done < "$TABLE"
(( TOT > 0 )) || fail "表里一条臂都没有"
(( BAD == 0 )) && ok "全部 $TOT 臂：verdict=PASS、established 达标、逐线程表在、退出账那格在"

# --- 2. 差值表 -------------------------------------------------------------------
note ""
note "== off/on 差值（µs CPU/秒，静置窗内；ns/对/秒 = 差 ÷ 连接对）=="
awk -F'\t' 'NR > 1 && $9 ~ /^PASS/ {
    key = $5 "|" $2 "|" $3 "|" $4
    if ($1 == "off") o[key] = $6 + 0
    if ($1 == "on")  p[key] = $6 + 0
  }
  END { for (k in p) { if (!(k in o)) continue; split(k, a, "|")
      d = p[k] - o[k]
      printf "%-24s r=%-3s on=%9.1f off=%9.1f 差=%9.1f µs/s  ns/对/秒=%8.1f\n",
             a[1] " n=" a[2] " w=" a[3], a[4], p[k], o[k], d, d * 1000.0 / a[2] } }' "$TABLE" | sort | tee -a "$OUT"

# --- 2b. 下界门：整组里至少一档的差值必须 > 0 -------------------------------------
note ""
note "== 下界门：至少要有一档 on−off > 0（全是 0 正是「没有成本」这种误读的入口）=="
MAXD=$(awk -F'\t' 'NR > 1 && $9 ~ /^PASS/ {
    key = $5 "|" $2 "|" $3 "|" $4
    if ($1 == "off") o[key] = $6 + 0
    if ($1 == "on")  p[key] = $6 + 0
  }
  END { best = -1; for (k in p) if (k in o) { d = p[k] - o[k]; if (d > best) best = d }
        printf "%.1f\n", best }' "$TABLE")
if [[ -z "${MAXD:-}" ]]; then
  fail "算不出最大差值（表里没有配对的 off/on）"
elif (( $(awk -v v="$MAXD" 'BEGIN{print (v > 0) ? 1 : 0}') == 1 )); then
  ok "最大差值 = ${MAXD} µs/s > 0 → 「确实有成本」这一格有读数，不是空判"
else
  fail "全部配对差值 ≤ 0（最大 ${MAXD}）→ 这一组只能判「在本机 10 ms 一格之下」，**不许**写成「没有成本」"
fi

# --- 3. 带与极差（≥2 轮才允许报带）------------------------------------------------
note ""
note "== 每档的配对轮数、差值带与极差 =="
awk -F'\t' 'NR > 1 && $9 ~ /^PASS/ {
    key = $5 "|" $2 "|" $3 "|" $4
    if ($1 == "off") o[key] = $6 + 0
    if ($1 == "on")  p[key] = $6 + 0
  }
  END { for (k in p) { if (!(k in o)) continue; split(k, a, "|"); g = a[1] "|" a[2] "|" a[3]
      d = p[k] - o[k]; cnt[g]++; sum[g] += d
      if (cnt[g] == 1 || d < lo[g]) lo[g] = d
      if (cnt[g] == 1 || d > hi[g]) hi[g] = d }
    for (g in cnt) { split(g, b, "|")
      tag = (cnt[g] >= 2) ? "带" : "只1轮·不报带"
      printf "%-24s 配对=%d %-12s %.1f..%.1f µs/s 极差=%.1f 均值=%.1f ns/对/秒均值=%.1f\n",
             b[1] " n=" b[2] " w=" b[3], cnt[g], tag, lo[g], hi[g], hi[g] - lo[g],
             sum[g] / cnt[g], sum[g] / cnt[g] * 1000.0 / b[2] } }' "$TABLE" | sort | tee -a "$OUT"

# --- 4. 单调性门（下界：证明遍历确实在花钱）---------------------------------------
note ""
note "== 下界：workers=1 那条线上，差值必须随 n 上升（1000 / 10000 / 28000）=="
awk -F'\t' 'NR > 1 && $9 ~ /^PASS/ && $3 + 0 == 1 {
    key = $2 "|" $4
    if ($1 == "off") o[key] = $6 + 0
    if ($1 == "on")  p[key] = $6 + 0
  }
  END { split("1000 10000 28000", ns, " "); miss = 0
    for (i = 1; i <= 3; ++i) { n = ns[i]; d = "NA"
      for (k in p) { split(k, a, "|"); if (a[1] == n && (k in o)) { d = p[k] - o[k]; break } }
      val[i] = d; if (d == "NA") miss++
      printf "  n=%-6s 差=%s\n", n, d }
    if (miss > 0) { print "MONO missing"; exit }
    print ((val[1]+0 < val[2]+0) && (val[2]+0 < val[3]+0)) ? "MONO ok" : "MONO FAIL" }' "$TABLE" | tee -a "$OUT"
grep -q "MONO ok" "$OUT"   && ok "差值随 n 单调上升 → 这一格 O(n) 遍历确实在花钱（实测，不是我的推断）"
grep -q "MONO missing" "$OUT" && fail "扫描缺档（三点不齐）→ 下界没立住，这一组不进结论"
grep -q "MONO FAIL" "$OUT"  && fail "单调性门红：三点差不随 n 上升 → 判这一组作废（要么分辨率不够，要么我取数错）"

# --- 5. 结构归因：差值随 n 的标度（O(n) 走查的签名）--------------------------------
note ""
note "== 结构归因：同一 workers 线上，Δ 应近似随 n 线性上升（O(1) 的那些 store 不会随 n 动）=="
awk -F'\t' 'NR > 1 && $9 ~ /^PASS/ {
    key = $5 "|" $2 "|" $3 "|" $4
    if ($1 == "off") o[key] = $6 + 0
    if ($1 == "on")  p[key] = $6 + 0
  }
  END { for (k in p) { if (!(k in o)) continue; split(k, a, "|")
      if (a[1] == "perf") continue
      d = p[k] - o[k]; g = a[3]; n = a[2] + 0
      if (!(g in seen) || d / n < lo_ns[g]) lo_ns[g] = d / n
      if (!(g in seen) || d / n > hi_ns[g]) hi_ns[g] = d / n
      seen[g] = 1; tot[g]++
      printf "  w=%s n=%-7s Δ=%9.1f µs/s → 每连接对每秒 %.1f ns\n", g, a[2], d, d * 1000.0 / n } }' "$TABLE" | sort | tee -a "$OUT"
awk -F'\t' 'NR > 1 && $9 ~ /^PASS/ {
    key = $5 "|" $2 "|" $3 "|" $4
    if ($1 == "off") o[key] = $6 + 0
    if ($1 == "on")  p[key] = $6 + 0
  }
  END { for (k in p) { if (!(k in o)) continue; split(k, a, "|"); if (a[1] == "perf") continue
      d = p[k] - o[k]; g = a[3]; ns = d * 1000.0 / (a[2] + 0)
      if (!(g in mn) || ns < mn[g]) mn[g] = ns
      if (!(g in mx) || ns > mx[g]) mx[g] = ns }
    for (g in mn) printf "LINE %s %.1f %.1f %.2f\n", g, mn[g], mx[g], mx[g] / (mn[g] > 0 ? mn[g] : 1) }' "$TABLE" > "$BASE/scale.txt"
while read -r TAG G MN MX RAT; do
  # 比值单独不成门：全是 0 时比值也是 0（V1 那发就把这道门骗过去了）。必须同时要求上界 > 0。
  if awk -v r="$RAT" -v m="$MX" 'BEGIN{exit !(r <= 4.0 && m > 0)}'; then
    ok "workers=$G 这条线上每连接对的每秒成本散布 ${MN}~${MX} ns（比值 ${RAT} ≤ 4）→ 总 Δ 随 n 近似线性，这是 O(n) 走查的签名"
  else
    fail "workers=$G 这条线上 ns/对 散布 ${MN}~${MX}（比值 ${RAT}，或上界本就是 0）→ 线性不成立/没有正读数，Δ 不能全记在那一格走查头上"
  fi
done < "$BASE/scale.txt"
note "对照说明：如果这笔成本来自每拍那四次 relaxed store（O(1)），Δ 应当**不随 n 动**；"
note "实测是 237.8 → 2250 → 5000 → 27710 µs/s 随 n 一路上升，而 store 的次数一个都没变。"

# --- 6. perf 符号级归因：这一格按实测记「取不到」，不冒充完成 ------------------------
note ""
note "== 符号级归因（perf）：本轮**取不到**，两条实测理由 =="
note "理由一：那一格被内联。「nm」 证明 「l7gate::net::Reactor::connections_with_read_disarmed」 在 release 里是真符号"
note "（「Reactor」 与 「ReactorGroup」 各一条 「T」），但循环里那一次调用没有单独的榜上条目——"
note "两发 perf 臂的 self 榜头名都是 「Reactor::run()」（62.18% 与 60.15%），「read_disarmed」 一个样本都没有。"
note "理由二：静置窗太便宜。perf 只能「由 perf 直接拉起被测进程」（ENVIRONMENT 4.1），profile 是**整臂**的，"
note "取不到「只采那 60 秒」；而建连阶段（ramp 120~300 s）的 CPU 远大于静置窗里那一格的 5~28 ms/s，"
note "100k/w16 那一臂的 event count 是 12.07 s，而静置窗内整臂的差值只有 1.82 s——榜被建连阶段占了。"
note "所以本轮的归因交给上面那道**结构标度门**（Δ 随 n 线性），符号级那一格明写取不到；"
note "要拿到它得给网关加一个采样间隔旋钮或改用 「-F」 更高的静置专用档，属动被测方/新增测量手段，登记归项目所有人排期。"
PERF_N=$(ls -d "$BASE"/*perf* 2>/dev/null | wc -l | tr -d ' ')
note "（磁盘上那 $PERF_N 发 perf 臂的 perf.data 与 perf.report.txt 都留着，作两条理由的原始证据）"

echo "report checks=$CHECKS fails=$FAILED" > "$VERD"
[[ $FAILED -eq 0 ]] && echo "REPORT PASS" >> "$VERD" || echo "REPORT FAIL" >> "$VERD"
note ""
note "判定落盘 $VERD（$([[ $FAILED -eq 0 ]] && echo PASS || echo FAIL)，fails=$FAILED）"
[[ $FAILED -eq 0 ]] || exit 1
exit 0
