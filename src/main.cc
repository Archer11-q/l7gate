#include <cstdio>
#include <cstring>
#include <string>

#include "core/build_info.h"

namespace {

void usage(std::FILE* out) {
  std::fprintf(out,
      "l7gate —— L7 流量治理网关（开发中）\n"
      "用法: l7gate [--self-check]\n"
      "转发路径尚未实现，按 TASKS.md 的 S1-04/S1-06 落地。\n");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--self-check") == 0) {
    // 压测脚本用它把环境事实落到 BENCHMARK.md，避免手抄。
    std::printf("%s\n", l7gate::core::render_one_line(l7gate::core::current()).c_str());
    return 0;
  }
  if (argc == 2 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0)) {
    usage(stdout);
    return 0;
  }

  // 我拒绝在这里塞一个"能监听端口但什么都不做"的假壳：
  // 那会让 --self-check 之外的一切输出看起来像进度，而它不是。
  usage(stderr);
  std::fprintf(stderr, "状态: 尚未实现（S1-01 只交付构建骨架）\n");
  return 2;
}
