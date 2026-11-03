#!/usr/bin/env bash
# S5-01 成本对照（验收 ③）：**开与不开指标面**，每请求 `epoll_modifications` 与 p50/QPS
# 是否可分辨。主尺是 syscall / 等式那一把（已裁第 ⑨ 条第 5 项），p 值只当带外报警器。
#
# 两档都在账上，各说各的分辨率（不许把一档的限定语贴到另一档上）：
#   SHAPE=p999（缺省，**主档**）＝ B-013/B-016/B-018 那一族的形状：`s1-p999.sh` +
#     `CONNECTIONS=100` / 直连定标 120,000（待裁第 ⑧ 条裁 A 之后的新值）/ 经网关 18,000 /
#     稳态门 ≥50 s / workers=1 / 1 KiB 体 / 闭环每请求。仪器是 `l7gate_c100k` 的 HDR 分位，
#     **这一档的分辨率就是一个 HDR 桶 ≈128 µs/请求**——B-018 那句限定贴在这里才对得上。
#     注入用 `s1-p999.sh` 已有的旋钮 `GATEWAY_EXTRA`，不改那个脚本一个字节。
#   SHAPE=wrk＝SLO #1 的标准口径（`s1-p99.sh`，1000 并发 / 60 s / wrk），注入用本轮新加的
#     `GATEWAY_EXTRA_ARGS`（默认空 → 历史档逐字同形）。这一档量的是"高并发工作点上有没有变化"，
#     分辨率由组内极差自己说，不套 HDR 那句。
#
# 用法：
#   PRESET=release ROUNDS=2 bash scripts/bench/s5-01-metrics-cost.sh                 # 主档
#   PRESET=release ROUNDS=2 SHAPE=wrk bash scripts/bench/s5-01-metrics-cost.sh       # 第二操作点
#   SKIP_RUNS=1 SHAPE=... bash scripts/bench/s5-01-metrics-cost.sh                   # 只对落盘重算判定
#   ONLY_ROUNDS=3 ARMS=off ...                                                       # 补跑某一臂某一轮
# 输出：build/bench/s5-01-metrics-cost-<SHAPE>/，退出码 0 = 判定成立（合格性不合格会给 ✗ 并非零退出）。
#
# 判定必须带的那句限定写死在这里，不给"指标面免费"那种读法留口子（主档那份）：
#   本档分辨率是一个 HDR 桶 ≈128 µs/请求，比 B-008 的 8.7 µs 大一个数量级以上，所以
#   "无可分辨"只排除"掉进 µs 级的悬崖"、**不排除个位数 µs**。
set -uo pipefail
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1

PRESET="${PRESET:-release}"
WORKERS="${WORKERS:-1}"
ROUNDS="${ROUNDS:-2}"
SHAPE="${SHAPE:-p999}"
METRICS_PORT="${METRICS_PORT:-18522}"
# `BATCH`：同一 SHAPE 下的**不同定标批次**必须分开落盘——18,000 与 20,000 两批的 p50 混在一个
# 判定池里就是口径混读（每请求整数等式不受影响，分位数受影响）。默认留空＝沿用老目录。
OUT="build/bench/s5-01-metrics-cost-$SHAPE${BATCH:+-$BATCH}"
# 只在**起跑全新的一轮**时清空。补跑（给了 ONLY_ROUNDS/ARMS）或 SKIP_RUNS=1 复算时绝不删——
# 删掉就把已合格的臂一起抹了，判定只剩补跑那一臂可用（本轮就是这么自毁过一次数据的）。
if [[ -z "${ONLY_ROUNDS:-}" && -z "${ARMS:-}" && "${SKIP_RUNS:-0}" != "1" ]]; then
  rm -rf "$OUT"
fi
mkdir -p "$OUT"

[[ "$SHAPE" == "p999" || "$SHAPE" == "wrk" ]] || { echo "SHAPE 只能是 p999 或 wrk" >&2; exit 1; }
[[ -x "build/$PRESET/l7gate" ]] || { echo "先 cmake --build --preset $PRESET（全目标，口径⑪）" >&2; exit 1; }
if [[ "$SHAPE" == "wrk" ]]; then
  command -v wrk >/dev/null || { echo "找不到 wrk" >&2; exit 1; }
fi
(exec 3<>"/dev/tcp/127.0.0.1/$METRICS_PORT") 2>/dev/null && { echo "端口 $METRICS_PORT 被占" >&2; exit 1; }

# ── 形状参数 ────────────────────────────────────────────────────────────────
if [[ "$SHAPE" == "p999" ]]; then
  CONNS=100; REQ_DIRECT=120000; REQ_PROXY=20000; STEADY_MIN_MS=50000
  WIN_LO=50000; WIN_HI=120000; WIN_UNIT=ms; P50_UNIT=µs
  RESOLUTION="本档分辨率是一个 HDR 桶 ≈128 µs/请求（p50 这一列就是从 HDR 分位里取的，读数全是 128 的整数倍），比 B-008 的 8.7 µs 大一个数量级以上——所以这句只排除「掉进 µs 级的悬崖」，不排除个位数 µs，**读不出「指标面免费」"
else
  WIN_LO=45; WIN_HI=90; WIN_UNIT=s; P50_UNIT=ms
  RESOLUTION="这一档是 wrk（1000 并发 / 60 s），没有 HDR 桶；它的分辨率就是上面那个组内极差本身——所以"无可分辨"只说明「抬升小于组内极差」，读不出「指标面免费」"
fi

gval() { # key logfile —— 不假设键在行首（口径⑭(b)）
  grep -o "[[:space:]]$1=[0-9-]*" "$2" 2>/dev/null | head -1 | sed 's/.*=//'
}
# ── 仪器侧的三个取值：都从**原始落盘**里取，不从渲染出的表里取 ──────────────────
# 为什么：`s1-p999.sh`/`s1-p99.sh` 的 summary 里 `经网关` 那一行都可能出现多次
# （wrk 档 + hey 档 / 直连档 + 经网关档），`head -1` 拿到哪个取决于表格顺序——
# "取数的行选错了"这一族坑已经在口径⑰记过两次，这里不给自己再犯的机会。
arm_p50() { # dir → p50
  if [[ "$SHAPE" == "p999" ]]; then
    awk '$1=="经网关"{print $2}' "$1/s1-p999.summary.txt" | head -1
  else
    sed -n 's@^ *50% *\([0-9.]*\)ms.*@\1@p' "$1/proxy.wrk.txt" | head -1
  fi
}
arm_qps() { # dir → QPS
  if [[ "$SHAPE" == "p999" ]]; then
    awk '$1=="经网关"{print $7}' "$1/s1-p999.summary.txt" | head -1
  else
    sed -n 's@^Requests/sec: *\([0-9.]*\)@\1@p' "$1/proxy.wrk.txt" | head -1   # 分隔符用 @：这行里带 `/`
  fi
}
arm_window() { # dir → 经网关那一臂的窗口（判定合格性用；取不到返回 -1）
  if [[ "$SHAPE" == "p999" ]]; then
    awk '$1=="经网关"{print $6}' "$1/s1-p999.summary.txt" | head -1
  else
    local raw unit
    raw=$(sed -n 's@.*requests in \([0-9.]*\)\([smh]\),.*@\1 \2@p' "$1/proxy.wrk.txt" | head -1)
    [[ -z "$raw" ]] && { echo "-1"; return; }
    unit=${raw##* }
    case "$unit" in
      s) awk -v v="${raw%% *}" 'BEGIN{printf "%.0f", v}' ;;
      m) awk -v v="${raw%% *}" 'BEGIN{printf "%.0f", v*60}' ;;
      h) awk -v v="${raw%% *}" 'BEGIN{printf "%.0f", v*3600}' ;;
      *) echo "-1" ;;
    esac
  fi
}
ratio() { # $1=计数 $2=请求数 → 每请求读数（6 位小数）
  # 注意函数体不要压成一行：`... }' }` 这种连写在 bash 5 里解析不过去（本轮踩过一次，
  # 症状是整个脚本"unexpected end of file from {"，报的还是下面几百行的错）。
  awk -v a="$1" -v b="$2" 'BEGIN { if (b+0 > 0) printf "%.6f", a / b; else print "?" }'
}

run_arm() { # dir 额外网关参数…
  local dir=$1; shift
  if [[ "$SHAPE" == "p999" ]]; then
    env CONNECTIONS="$CONNS" REQUESTS_DIRECT="$REQ_DIRECT" REQUESTS_PROXY="$REQ_PROXY" \
        STEADY_MIN_MS="$STEADY_MIN_MS" WORKERS="$WORKERS" GATEWAY_PORT="${GW_PORT:-18084}" \
        ARM_LABEL="${dir##*/}" OUT_DIR="$dir" PRESET="$PRESET" GATEWAY_EXTRA="$*" \
      bash scripts/bench/s1-p999.sh > "$dir.run.log" 2>&1
  else
    OUT_DIR="$dir" PRESET="$PRESET" WORKERS="$WORKERS" GATEWAY_EXTRA_ARGS="$*" \
      bash scripts/bench/s1-p99.sh > "$dir.run.log" 2>&1
  fi
}

# ── 第三臂：scrape 风暴（分辨力对照）───────────────────────────────────────────
# ③ 那句"无可分辨"是一条**上界**。上界必须配一条"这条臂确实量得出东西"的证据，
# 否则它可能只是在量寂静。做法不是改实现，而是把外部负载加到观测面上：
# 同一份二进制、同样开 `--metrics-port`，但另起一条循环把 /metrics 打到不停。
# workers=1 时观测线程与那条唯一的事件循环抢同一批核——这场所要是本档连它都读不出来，
# 那"无可分辨"的适用范围就必须写成"在本档分辨率之下"，而不是"仪器免费"。
STORM_RATE_MS="${STORM_RATE_MS:-20}"
# **不要把这里写成 `PID=$(start_storm …)`**：命令替换要等它的 stdout 管道关闭才返回，
# 而后台那个无限循环的子shell 继承了这根管道 → 永远等下去。本轮就是这么在 storm 臂上
# 挂了两个半小时，一条数都没取。现在：stdout/stderr 重定向到文件，PID 落文件，父进程不接管道。
start_storm() { # port pidfile
  local port=$1 pidfile=$2
  (
    while :; do
      curl -sf -o /dev/null "http://127.0.0.1:$port/metrics" || true
      sleep "$(awk -v ms="$STORM_RATE_MS" 'BEGIN{printf "%.3f", ms/1000}')"
    done
  ) > "${pidfile}.out" 2>&1 &
  echo $! > "$pidfile"
}

storm_alive() { # pidfile → 0/1
  local pidfile=$1
  [[ -f "$pidfile" ]] && kill -0 "$(cat "$pidfile")" 2>/dev/null
}

stop_storm() { # pidfile
  local pidfile=$1 pid
  [[ -f "$pidfile" ]] || return 0
  pid=$(cat "$pidfile")
  pkill -P "$pid" 2>/dev/null
  kill "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null
  rm -f "$pidfile"
}

ARMS="${ARMS:-off on}"
if [[ "${SKIP_RUNS:-0}" == "1" ]]; then
  echo "=== S5-01 成本对照（SHAPE=$SHAPE）：SKIP_RUNS=1，只对 $OUT/ 已落盘的臂重算判定 ==="
else
  echo "=== S5-01 成本对照（SHAPE=$SHAPE）：PRESET=$PRESET workers=$WORKERS rounds=$ROUNDS arms=[$ARMS] ==="
  for ((r = 1; r <= ROUNDS; r++)); do
    if [[ -n "${ONLY_ROUNDS:-}" ]] && [[ " $ONLY_ROUNDS " != *" $r "* ]]; then continue; fi
    for arm in $ARMS; do
      extra=""
      [[ "$arm" == "on" || "$arm" == "storm" ]] && extra="--metrics-port $METRICS_PORT"
      dir="$OUT/$arm-r$r"
      echo "--- 第 $r 轮 / $arm 臂（网关参数：${extra:-（空）}）"
      STORM_PIDFILE="$dir.storm.pid"
      if [[ "$arm" == "storm" ]]; then
        # `STORM_PARALLEL`：一条 curl 循环实测只能到 ~50 次/秒（进程启动就是瓶颈），
        # 那个强度本轮读不出任何东西，所以并行开 N 条（PID 全写进同一个 pidfile）。
        : > "$STORM_PIDFILE"
        for i in $(seq 1 "${STORM_PARALLEL:-1}"); do
          start_storm "$METRICS_PORT" "${STORM_PIDFILE}.$i"
          cat "${STORM_PIDFILE}.$i" >> "$STORM_PIDFILE"
        done
        sleep 1
        ALIVE=0
        for i in $(seq 1 "${STORM_PARALLEL:-1}"); do
          storm_alive "${STORM_PIDFILE}.$i" && ALIVE=$((ALIVE+1))
        done
        if (( ALIVE > 0 )); then
          echo "  scrape 风暴：$ALIVE/${STORM_PARALLEL:-1} 条并行在跑（每条每 ${STORM_RATE_MS}ms 一次）"
        else
          echo "  ！风暴没起来（网关上只会留 `metrics_scrapes_ok=0` 那笔空账，这一臂按不合格处理）"
        fi
      fi
      run_arm "$dir" "$extra"
      rc=$?
      # 退出码**落盘**，不从日志里 grep：本轮我就是用 `grep -o "rc=[0-9]*"` 去 run.log 里找退出码，
      # 命中的却是生命周期行里的 `gw_rc=0`，于是把一条本来正确退出了 1 的脚本读成了"判红却不失败"，
      # 还据此写进三处文档。取码这件事的正确答案是：自己写文件，别在别人的输出里找。
      echo "$rc" > "$dir.rc"
      [[ "$arm" == "storm" ]] && for i in $(seq 1 "${STORM_PARALLEL:-1}"); do stop_storm "${STORM_PIDFILE}.$i"; done
      [[ $rc -eq 0 ]] || echo "  ！$arm-r$r 的取数脚本 rc=$rc（这一臂按不合格处理，见 $dir.run.log）"
    done
  done
fi

echo
echo "=== 逐臂读数（窗口不合格的一臂标 ✗ 并剔出判定，不留白也不偷偷平均）==="
printf '%-10s %9s %10s %10s %12s %12s %12s %9s %s\n' 臂 "p50$P50_UNIT" QPS requests 每请求epoll_MOD 每请求epoll_ADD 每请求epoll_DEL "窗口$WIN_UNIT" 判定 \
  | tee "$OUT/rows.txt" > /dev/null
# 读数与判定**固定扫两臂**：补跑时 `ARMS`/`ONLY_ROUNDS` 只影响上面要不要起跑，不影响这里，
# 否则补跑一臂就会把另一臂从判定里悄悄抹掉。
for ((r = 1; r <= ROUNDS; r++)); do
  for arm in off on storm; do
    dir="$OUT/$arm-r$r"
    # storm 是可选臂：没跑过就不占一行（占一行"缺落盘"会让人以为有一臂坏了）。
    [[ "$arm" == "storm" && ! -d "$dir" ]] && continue
    log="$dir/gateway.log"
    if [[ ! -f "$log" ]]; then
      printf '%-10s %9s\n' "$arm-r$r" "缺落盘" | tee -a "$OUT/rows.txt" > /dev/null; continue
    fi
    parsed=$(gval requests_parsed "$log")
    mods=$(gval epoll_modifications "$log")
    regs=$(gval epoll_registrations "$log")
    dels=$(gval epoll_unregistrations "$log")
    p50=$(arm_p50 "$dir"); qps=$(arm_qps "$dir"); win=$(arm_window "$dir")
    verdict="ok"
    # 风暴臂自己的下界：`metrics_scrapes_ok` 必须真的在涨，否则这一臂的"负载"是想象出来的
    # ——一个没打到的风暴与一个没开的观测面在 p50 上长得一模一样。
    if [[ "$arm" == "storm" ]]; then
      storm_ok=$(gval metrics_scrapes_ok "$log")
      if [[ -z "$storm_ok" || "$storm_ok" == "0" ]]; then
        verdict="✗风暴没打到观测面（metrics_scrapes_ok=${storm_ok:-空}）"
      fi
    fi
    # **合格性读取数门自己的判定**（退出码优先，✗ 文本兜底），不是我另设的窗口区间。
    # 本轮在这里犯过两笔，都写下来免得下一个人再犯：
    #   ① 我把窗口区间设成 45–120 s，于是 49,873 ms 那臂被判成"合格"放了进去——门明明在 50 s。
    #   ② 我用 `grep -o "rc=[0-9]*"` 去 run.log 里找取数脚本的退出码，命中的却是生命周期行里的
    #      `gw_rc=0`，据此在文档里写了一句"`s1-p999.sh` 判红却 rc=0"。**那句是错的**：它末尾就是
    #      `exit "$FAILED"`，`off-r1` 实实在在退了 1、父脚本也记了 `！`。教训是**别在别人的输出里
    #      找自己的数**，所以现在退出码由本脚本写成 `$dir.rc`。
    if [[ "$verdict" == "ok" ]]; then
      if [[ -f "$dir.rc" ]]; then
        if [[ "$(cat "$dir.rc")" != "0" ]]; then
          verdict="✗取数脚本 rc=$(cat "$dir.rc")（被它自己的门判红，剔除）"
        fi
      elif [[ -f "$dir.run.log" ]] && grep -q '✗' "$dir.run.log"; then
        # 改法之前跑出来的历史臂没有 .rc，退回读 ✗ 文本（两者说的是同一件事）。
        verdict="✗被取数门判红（$(grep -o '实际稳态窗口 [0-9]*ms < [0-9]*ms' "$dir.run.log" | head -1)）"
      fi
    fi
    if [[ "$verdict" == "ok" ]] && { [[ "$win" == "-1" ]] || (( $(awk -v w="$win" -v lo="$WIN_LO" -v hi="$WIN_HI" 'BEGIN{print (w+0<lo+0 || w+0>hi+0) ? 1 : 0}') )); }; then
      # 窗口仍兜一道：本轮见过 60 s 的档被环境压成 27 分钟（1998 次 socket 超时 + 640 QPS），
      # 那不是"网关慢"，是整台机器在别的东西上排队。剔掉这件事要在纸上看得见。
      verdict="✗窗口 ${win}$WIN_UNIT 不合格"
    fi
    printf '%-10s %9s %10s %10s %12s %12s %12s %9s %s\n' "$arm-r$r" "${p50:-?}" "${qps:-?}" \
      "${parsed:-?}" "$(ratio "${mods:-0}" "${parsed:-0}")" "$(ratio "${regs:-0}" "${parsed:-0}")" \
      "$(ratio "${dels:-0}" "${parsed:-0}")" "${win}" "$verdict" | tee -a "$OUT/rows.txt" > /dev/null
  done
done

echo
echo "=== 判定 ==="
awk -v res="$RESOLUTION" -v unit="$P50_UNIT" '
  function mid(a, n,   i, lo, hi) {
    if (n == 0) return 0
    lo = a[1]; hi = a[1]
    for (i = 2; i <= n; i++) { if (a[i]+0 < lo+0) lo = a[i]; if (a[i]+0 > hi+0) hi = a[i] }
    return (lo + hi) / 2
  }
  function span(a, n,   i, lo, hi) {
    if (n == 0) return 0
    lo = a[1]; hi = a[1]
    for (i = 2; i <= n; i++) { if (a[i]+0 < lo+0) lo = a[i]; if (a[i]+0 > hi+0) hi = a[i] }
    return hi - lo
  }
  NR == 1 { next }                                    # 表头
  { if ($NF != "ok") { printf "  （剔除 %s：%s）\n", $1, $NF; next }
    # **按臂分流**，一行只进一个数组。这里曾经把每一行同时塞进 off 与 on 两个数组，
    # 于是"跨臂差 +0.000000"是同一个数组和自己相减出来的**假绿**（本轮抓到的是判定文字错，
    # 逐臂原始读数一直是真的）。修的是路由，不是结论。
    if ($1 ~ /^off/) { no++;  offp50[no]=$2;  offqps[no]=$3;  offmod[no]=$5;  offadd[no]=$6;  offdel[no]=$7 }
    else if ($1 ~ /^on-r/) { nn++; onp50[nn]=$2; onqps[nn]=$3; onmod[nn]=$5; onadd[nn]=$6; ondel[nn]=$7 }
    else if ($1 ~ /^storm/) { stn++; stp50[stn]=$2; stqps[stn]=$3; stmod[stn]=$5 }
  }
  END {
    if (no < 2 || nn < 2) {
      printf "  ✗ 有效轮数不足（off=%d on=%d，各需 ≥2）：这一批判不了，得补跑\n", no, nn
      exit 1
    }
    printf "  有效轮：off=%d on=%d\n", no, nn
    om = mid(offmod, no);  nm = mid(onmod, nn)
    so = span(offmod, no); sn = span(onmod, nn)
    printf "  每请求 epoll_modifications：off 中位 %.6f（组内极差 %.6f）on 中位 %.6f（组内极差 %.6f）跨臂差 %+.6f\n", \
      om, so, nm, sn, nm - om
    printf "  每请求 epoll_registrations：off 中位 %.6f on 中位 %.6f 跨臂差 %+.6f\n", \
      mid(offadd, no), mid(onadd, nn), mid(onadd, nn) - mid(offadd, no)
    printf "  每请求 epoll_unregistrations：off 中位 %.6f on 中位 %.6f 跨臂差 %+.6f\n", \
      mid(offdel, no), mid(ondel, nn), mid(ondel, nn) - mid(offdel, no)
    limit = (so > sn ? so : sn)
    diff = nm - om; if (diff < 0) diff = -diff
    if (diff <= limit)
      print  "  → 主尺无可分辨：跨臂差落在组内极差之内（与 B-018 那四级 4.0003–4.0004 同带）"
    else
      print  "  → 主尺出带：跨臂差大于组内极差，照记并归因到具体机制"
    op = mid(offp50, no); np = mid(onp50, nn)
    spo = span(offp50, no); spn = span(onp50, nn); sp = (spo > spn ? spo : spn)
    printf "  p50（%s）：off 中位 %.3f on 中位 %.3f 跨臂差 %+.3f（组内极差上界 %.3f）\n", unit, op, np, np - op, sp
    oq = mid(offqps, no); nq = mid(onqps, nn)
    sqo = span(offqps, no); sqn = span(offqps, nn); sq = (sqo > sqn ? sqo : sqn)
    printf "  QPS：off 中位 %.2f on 中位 %.2f 跨臂差 %+.2f（组内极差上界 %.2f）\n", oq, nq, nq - oq, sq
    dp = np - op; if (dp < 0) dp = -dp
    if (dp <= sp)
      printf "  → p50 无可分辨。限定语一起读，不许读成「指标面免费」：%s\n", res
    else
      print  "  → p50 出带（跨臂差大于组内极差）：照记 + 归因，不许写成「无可分辨」"
    # 三、分辨力对照（第三臂）：这条臂量不量得出"观测面自己在抢 CPU"这件事。
    # **逐轮判、不取中位**：本轮两臂风暴强度不同（1 路 ≈50 次/秒 与 8 路并行），
    # 取中位会把强的那一轮抹平——我第一版就是这么把一条出带的读数读成"仍在带内"的。
    if (stn > 0) {
      lo = offp50[1]; hi = offp50[1]
      for (i = 2; i <= no; i++) { if (offp50[i]+0 < lo) lo = offp50[i]; if (offp50[i]+0 > hi) hi = offp50[i] }
      for (i = 1; i <= nn; i++) { if (onp50[i]+0 < lo) lo = onp50[i]; if (onp50[i]+0 > hi) hi = onp50[i] }
      qlo = offqps[1]; qhi = offqps[1]
      for (i = 2; i <= no; i++) { if (offqps[i]+0 < qlo) qlo = offqps[i]; if (offqps[i]+0 > qhi) qhi = offqps[i] }
      for (i = 1; i <= nn; i++) { if (onqps[i]+0 < qlo) qlo = onqps[i]; if (onqps[i]+0 > qhi) qhi = onqps[i] }
      printf "  off/on 四臂的实际带宽：p50 %.2f–%.2f，QPS %.0f–%.0f\n", lo, hi, qlo, qhi
      out_of_band = 0
      for (i = 1; i <= stn; i++) {
        fp = (stp50[i]+0 > hi || stp50[i]+0 < lo) ? "出带" : "在带内"
        fq = (stqps[i]+0 > qhi || stqps[i]+0 < qlo) ? "出带" : "在带内"
        if (fp == "出带" || fq == "出带") out_of_band = 1
        printf "  风暴臂第 %d 轮：p50 %.2f（%s）QPS %.0f（%s）每请求 epoll_MOD %.6f\n", \
          i, stp50[i], fp, stqps[i], fq, stmod[i]
      }
      if (out_of_band)
        print  "  → 至少一轮风暴出带：这条臂确实量得出观测面带来的负载，所以 off/on 那一层的「无可分辨」不是仪器失灵"
      else
        print  "  → 所有风暴轮都在 off/on 带宽之内：本档连打满观测面都读不出来，③ 那句只能读成「抬升在本档分辨率之下」"
      print  "  （风暴臂的每请求 epoll_MOD 若与 off/on 同带，就是「scrape 不进 worker 的 syscall 面」在负载下的复证）"
    } else {
      print  "  （第三臂 storm 没跑：③ 的「无可分辨」缺一条分辨力对照，见脚本头那段说明）"
    }
  }' "$OUT/rows.txt" || exit 1

echo
echo "落盘：$OUT/（每臂的 gateway.log、summary、观察者原始输出都在）"
echo "DONE"
