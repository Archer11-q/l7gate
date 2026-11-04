#!/usr/bin/env awk -f
# folded 栈 → 火焰图 SVG（自带的极简渲染器，不引 FlameGraph 那份 perl——它是新工具依赖，
# 而契约 8.2 禁令三说新依赖要先写 ADR；这里要的只是把 perf 自己的采样画出来，不是新的测量）。
#
# 输入：`perf script` 折叠出来的行，格式  frame;frame;...;leaf COUNT
# 输出：自包含 SVG（矩形按占比宽度着色、<title> 里带完整帧名，浏览器直接打开）
# 用法：awk -f scripts/report/perf-fold-to-svg.sh -v title="l7gate" -o out.svg in.folded
#   （或用 -o 省略走 stdout）
BEGIN { FS = ";"; W = 1200; ROWH = 16; PAD = 4; TOTAL = 0; }
NF >= 1 && $NF ~ / [0-9]+$/ {
  line = $0
  n = match(line, / [0-9]+$/)
  cnt = substr(line, n + 1) + 0
  stack = substr(line, 1, n - 1)
  TOTAL += cnt
  folds[stack] += cnt
  next
}
END {
  if (TOTAL == 0) { print "<!-- 没有采样，不画空图 -->"; exit 0 }
  # 折叠栈拆成节点表：key = 前缀路径，value = 计数
  nodes = 0
  for (f in folds) {
    c = folds[f]
    split(f, parts, ";")
    path = ""
    for (i = 1; i <= length(parts); i++) {
      path = (path == "" ? parts[i] : path ";" parts[i])
      if (!(path in seen)) { seen[path] = ++nodes; name[path] = parts[i]; depth[path] = i - 1 }
      sum[path] += c
    }
    rootsplit = f
  }
  # 根节点：所有 depth==0 的
  H = 40
  for (k in seen) if (depth[k] + 1 > H / ROWH) H = (depth[k] + 1) * ROWH + 40
  maxd = 0
  for (k in seen) if (depth[k] > maxd) maxd = depth[k]

  printf "<?xml version=\"1.0\" standalone=\"no\"?>\n"
  printf "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" height=\"%d\" viewBox=\"0 0 %d %d\">\n", W, H + 40, W, H + 40
  printf "<rect width=\"100%%\" height=\"100%%\" fill=\"#fcfbf7\"/>\n"
  printf "<text x=\"%d\" y=\"20\" font-family=\"monospace\" font-size=\"14\">%s — %d 采样 · 只有用户态栈（paranoid=2，内核帧取不到）</text>\n", PAD, title, TOTAL

  # 按深度逐层排：同一深度的节点按各自父路径的 x 递进（这里用简化的贪心：以 sum 降序铺满一行）
  for (d = 0; d <= maxd; d++) {
    # 收集这一层的节点
    m = 0
    for (k in seen) if (depth[k] == d) { level[++m] = k }
    if (m == 0) continue
    # 排序（插入排序，节点数在这一档是几百级，够用）
    for (i = 2; i <= m; i++) {
      key = level[i]; j = i - 1
      while (j >= 1 && sum[level[j]] < sum[key]) { level[j + 1] = level[j]; j-- }
      level[j + 1] = key
    }
    x = PAD
    for (i = 1; i <= m; i++) {
      k = level[i]
      w = (W - 2 * PAD) * sum[k] / TOTAL
      if (w < 0.4) continue
      y = 30 + (maxd - d) * ROWH
      r = 200 + (int(sum[k] * 7) % 55)
      g = 90 + (int(sum[k] * 13) % 90)
      printf "<rect x=\"%.2f\" y=\"%d\" width=\"%.2f\" height=\"%d\" fill=\"rgb(%d,%d,40)\" stroke=\"#fcfbf7\" stroke-width=\"0.4\"><title>%s (%d, %.2f%%)</title></rect>\n", x, y, w, ROWH - 2, r, g, k, sum[k], 100.0 * sum[k] / TOTAL
      if (w > 42) printf "<text x=\"%.2f\" y=\"%d\" font-family=\"monospace\" font-size=\"9\" fill=\"#241f18\" clip-path=\"inset(0 0 0 0)\">%s</text>\n", x + 2, y + 11, esc(name[k])
      x += w
    }
  }
  printf "</svg>\n"
}
function esc(s) { gsub(/&/, "\\&amp;", s); gsub(/</, "\\&lt;", s); gsub(/>/, "\\&gt;", s); return s }
