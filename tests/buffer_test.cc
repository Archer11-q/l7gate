#include "net/buffer.h"

#include "pipe_fixture.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <errno.h>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace l7gate::net {
namespace {

using l7gate::testing::PipeFixture;

// 直接由调用方的 rng 产字节。我第一版写成 make_pattern(n, rng())，
// 每个 chunk 都重建一次 mt19937 —— 那是 3000 万次种子初始化，纯属浪费。
std::vector<std::uint8_t> make_pattern(std::mt19937& rng, std::size_t n) {
  std::vector<std::uint8_t> v(n);
  for (auto& b : v) b = static_cast<std::uint8_t>(rng() & 0xFFu);
  return v;
}

// 一次性用例用的重载：给种子、内部自建 rng。
// 没有它我就得写 make_pattern(std::mt19937(1u), n)，而临时量绑不上
// 非 const 左值引用，编译不过。
std::vector<std::uint8_t> make_pattern(std::uint32_t seed, std::size_t n) {
  std::mt19937 rng(seed);
  return make_pattern(rng, n);
}

// 把 len 字节分块灌进 buffer，返回真正写进去的字节数。
// 必须循环：writable_region 的 hint 只表示"本轮至多想要这么多空间"，实际给多少
// 由当前剩余空间决定。我第一版在这里写了一次性 memcpy + ASSERT_GE(space, len)，
// 那等于替 buffer 保留了"hint 就是预留"的旧行为 —— 而这正是我这轮刚改掉的那条
// （旧行为让 1 KiB keep-alive 的连接也把容量顶到 8 KiB）。测试不能把被改掉的
// 假设继续编进去。
std::size_t append_all(Buffer& buf, const std::uint8_t* data, std::size_t len) {
  std::size_t off = 0;
  while (off < len) {
    const auto [ptr, space] = buf.writable_region(len - off);
    if (space == 0) break;  // 撞容量上限；由调用方判定是否该失败
    const std::size_t take = std::min(space, len - off);
    std::memcpy(ptr, data + off, take);
    buf.commit_written(take);
    off += take;
  }
  return off;
}

// --- 验收标准第 1 条：10000 次随机读写序列，读回与写入完全一致且无越界 -----
TEST(S1_02_Buffer, RandomReadWriteSequenceMatchesReferenceByteForByte) {
  std::mt19937 rng(20261026u);
  // 这条用例只验数据完整性，所以刻意把两条限制线抬到不会被碰到的量级；
  // 限制行为有它自己的用例（StopsAtHighWaterMark...），混在一起的话
  // 一边失败我会分不清是数据错了还是限制生效了。
  Buffer::Config cfg;
  cfg.initial_capacity = 4096;
  cfg.max_capacity = 64u << 20;
  cfg.high_water_mark = 64u << 20;
  Buffer buf(cfg);
  std::vector<std::uint8_t> reference;  // 期望仍可被读出的字节
  std::size_t consumed = 0;             // 已被读走的字节

  std::size_t total_written = 0;
  std::size_t total_read = 0;
  std::uint32_t max_single_write = 0;

  for (int op = 0; op < 10000; ++op) {
    const bool do_write = (rng() % 100u) < 60u || buf.readable_bytes() == 0;
    if (do_write) {
      const std::size_t len = rng() % 3000u;  // 含 0 长度这个边界
      max_single_write = std::max(max_single_write, static_cast<std::uint32_t>(len));
      const auto chunk = make_pattern(rng, len);
      // 走 writable_region + commit_written，和真实读 fd 之后提交是同一条代码路径
      const std::size_t appended = append_all(buf, chunk.data(), len);
      ASSERT_EQ(appended, len) << "op=" << op
          << " 在 64 MiB 上限配置下不该写不进去，说明容量策略有问题";
      reference.insert(reference.end(), chunk.begin(), chunk.end());
      total_written += len;
    } else {
      const std::size_t want =
          std::min<std::size_t>(rng() % 2500u, buf.readable_bytes());
      std::vector<std::uint8_t> out(want);
      const std::size_t got = buf.retrieve(out.data(), want);
      ASSERT_EQ(got, want);
      const std::size_t base = consumed;
      ASSERT_GE(reference.size(), base + got);
      EXPECT_TRUE(std::equal(out.begin(), out.end(),
                             reference.begin() + static_cast<std::ptrdiff_t>(base)))
          << "第 " << op << " 次操作读回的字节与写入序列不一致";
      consumed += got;
      total_read += got;
    }
    // 每一步都断言长度口径，越界在这一步就会暴露
    ASSERT_EQ(buf.readable_bytes(), reference.size() - consumed) << "op=" << op;
  }

  EXPECT_GT(total_written, 0u);
  EXPECT_GT(total_read, 0u);
  EXPECT_LT(total_read, total_written);  // 收尾时刻意留未读完的尾巴

  // 单连接峰值内存的口径落在这里。注意它**不是** SLO #2 的结论：
  // 10000 次随机操作下的容量上界只描述 buffer 自己的形状，
  // 每连接内存要等 100k 长连接稳态实测才有资格说话。
  std::printf("[S1-02] ops=10000 written=%zu read=%zu peak_capacity=%zu final_capacity=%zu max_single_write=%u\n",
              total_written, total_read, buf.peak_capacity(), buf.capacity(), max_single_write);
}

// --- 验收标准第 2 条：ET 契约在 buffer 层成立 ----------------------------
TEST(S1_02_Buffer, ReadUntilBlockedKeepsReadingUntilEagain) {
  PipeFixture pipe;
  if (!pipe.ok()) GTEST_SKIP() << "pipe2 不可用";
  Buffer buf;

  auto part_a = make_pattern(1u, 1500);
  auto part_b = make_pattern(2u, 2200);
  std::string all;
  all.append(reinterpret_cast<const char*>(part_a.data()), part_a.size());
  all.append(reinterpret_cast<const char*>(part_b.data()), part_b.size());
  pipe.write_all(all);

  const auto r = buf.read_until_blocked(pipe.rfd());
  EXPECT_TRUE(r.would_block) << "ET 语义要求读到 EAGAIN 才算本轮结束";
  EXPECT_FALSE(r.peer_closed);
  EXPECT_FALSE(r.limit_reached);
  EXPECT_EQ(r.errno_value, 0);
  EXPECT_EQ(r.bytes_read, all.size());
  EXPECT_EQ(buf.readable_bytes(), all.size());
  // 一次 syscall 不可能既拿完数据又确认读干净了：至少要"读到空"再"撞一次 EAGAIN"。
  EXPECT_GE(r.read_syscalls, 2u) << "read 次数不足以证明真的读到了 EAGAIN";

  std::string got(buf.readable_bytes(), '\0');
  ASSERT_EQ(buf.retrieve(got.data(), got.size()), got.size());
  EXPECT_EQ(got, all);
}

// 粘包：一批数据里含多个完整请求时，Buffer 不能替解析层丢边界。
TEST(S1_02_Buffer, PreservesStickyPacketsWithoutDroppingBytes) {
  PipeFixture pipe;
  if (!pipe.ok()) GTEST_SKIP() << "pipe2 不可用";
  Buffer buf;

  std::string payload;
  for (int i = 0; i < 50; ++i) {
    payload += "GET /r" + std::to_string(i) + " HTTP/1.1\r\nHost: x\r\n\r\n";
  }
  pipe.write_all(payload);

  std::string collected;
  std::size_t rounds = 0;
  while (collected.size() < payload.size()) {
    const auto r = buf.read_until_blocked(pipe.rfd());
    ++rounds;
    EXPECT_TRUE(r.would_block) << "每轮都必须以 EAGAIN 收尾，否则 ET 下会永久漏事件";
    std::string chunk(buf.readable_bytes(), '\0');
    buf.retrieve(chunk.data(), chunk.size());
    collected += chunk;
    ASSERT_LT(rounds, 100u) << "读循环没有收敛";
  }
  EXPECT_EQ(collected, payload);
}

// --- 高水位 / 容量上限：S1-07 背压的钩子，必须在 buffer 层就可观测 --------
TEST(S1_02_Buffer, StopsAtHighWaterMarkAndReportsItAsEventNotError) {
  PipeFixture pipe;
  if (!pipe.ok()) GTEST_SKIP() << "pipe2 不可用";

  Buffer::Config cfg;
  cfg.initial_capacity = 4096;
  cfg.max_capacity = 8192;
  cfg.high_water_mark = 4096;
  Buffer buf(cfg);

  pipe.write_all(std::string(8192, 'A'));

  const auto r = buf.read_until_blocked(pipe.rfd());
  EXPECT_TRUE(r.limit_reached) << "撞高水位必须报成事件，否则调用方无从停读";
  EXPECT_FALSE(r.would_block) << "停读发生在内核之前，本轮不该以 EAGAIN 收尾";
  EXPECT_EQ(r.errno_value, 0) << "撞线不是错误";
  EXPECT_LE(buf.readable_bytes(), cfg.high_water_mark);
  EXPECT_LE(buf.capacity(), cfg.max_capacity);

  // 水位回落后必须恢复读取 —— 否则"停读"就变成永久卡死。
  const std::size_t pending = buf.readable_bytes();
  std::string drain(pending, '\0');
  ASSERT_EQ(buf.retrieve(drain.data(), pending), pending);
  const auto r2 = buf.read_until_blocked(pipe.rfd());
  EXPECT_GT(r2.bytes_read, 0u) << "消费掉水位以下的数据后应当重新读得动";
}

// 上游关闭：读到 0 是 EOF，不是 EAGAIN，两者绝不能混。
TEST(S1_02_Buffer, DistinguishesPeerClosedFromWouldBlock) {
  PipeFixture pipe;
  if (!pipe.ok()) GTEST_SKIP() << "pipe2 不可用";
  Buffer buf;

  const auto data = make_pattern(7u, 100);
  ASSERT_EQ(::write(pipe.wfd(), data.data(), data.size()), static_cast<ssize_t>(data.size()));
  pipe.release_write_end();  // 写端关闭，数据仍在管道里

  const auto r = buf.read_until_blocked(pipe.rfd());
  EXPECT_TRUE(r.peer_closed);
  EXPECT_FALSE(r.would_block);
  EXPECT_EQ(r.bytes_read, 100u) << "EOF 之前到达的数据不能丢";
  EXPECT_EQ(buf.readable_bytes(), 100u);
}

// --- 边界与生命周期 ------------------------------------------------------
TEST(S1_02_Buffer, OutOfBoundsRequestsChangeNothing) {
  Buffer buf;
  const std::size_t before = buf.readable_bytes();
  EXPECT_FALSE(buf.consume(before + 1));
  EXPECT_FALSE(buf.peek_consume(before + 1));
  EXPECT_EQ(buf.readable_bytes(), before);

  std::uint8_t byte = 0;
  EXPECT_EQ(buf.retrieve(&byte, 1), 0u) << "没有可读数据时不能写出一个字节";
  EXPECT_EQ(buf.readable_bytes(), before);

  buf.clear();
  EXPECT_EQ(buf.readable_bytes(), 0u);
}

TEST(S1_02_Buffer, CompactionReclaimsFrontSpaceInsteadOfGrowing) {
  Buffer::Config cfg;
  cfg.initial_capacity = 4096;
  cfg.max_capacity = 1u << 20;
  Buffer buf(cfg);

  const std::size_t start_capacity = buf.capacity();
  for (int round = 0; round < 200; ++round) {
    const auto [ptr, space] = buf.writable_region(2000);
    ASSERT_GE(space, 2000u) << "round=" << round;
    std::memset(ptr, round & 0xFF, 2000);
    buf.commit_written(2000);
    std::vector<std::uint8_t> out(2000);
    ASSERT_EQ(buf.retrieve(out.data(), 2000), 2000u);
  }
  // 反复"写 2000 读 2000"不该把容量推高：读空归零 + 压缩必须兜住稳态 keep-alive。
  EXPECT_EQ(buf.capacity(), start_capacity)
      << "容量涨到 " << buf.capacity() << "，说明归零/压缩路径没兜住";
}

TEST(S1_02_Buffer, MovePreservesReadableData) {
  Buffer a;
  const auto data = make_pattern(9u, 777);
  const auto [ptr, space] = a.writable_region(data.size());
  ASSERT_GE(space, data.size());
  std::memcpy(ptr, data.data(), data.size());
  a.commit_written(data.size());

  Buffer b(std::move(a));
  EXPECT_EQ(b.readable_bytes(), 777u);
  EXPECT_EQ(a.readable_bytes(), 0u);
  // 比对必须用 uint8_t，不能用 std::string：make_pattern 会产出 >127 的字节，
  // 而 char 在 x86-64 Linux 上是有符号的，std::equal 会把 0xC3 和 -61 判成不等。
  // 这个 bug 是测试自己写错，不是 buffer 丢了数据 —— 实测时两条断言只挂了比对那条。
  std::vector<std::uint8_t> got(777);
  ASSERT_EQ(b.retrieve(got.data(), got.size()), 777u);
  EXPECT_TRUE(std::equal(data.begin(), data.end(), got.begin()));
}

// 单连接峰值内存的**可用**口径：默认配置下跑 1 KiB keep-alive 稳态。
// 上面那条 10000 次随机操作测到的 peak=4 MiB 不能拿去做 SLO #2 —— 那是
// 我为了测数据完整性而**故意关掉两条限制线**之后的结果。
// SLO #2 要的是默认配置下、真实报文形状里 buffer 会长成什么样。
TEST(S1_02_Buffer, PeakCapacityUnderOneKiBKeepAliveWorkload) {
  PipeFixture pipe;
  if (!pipe.ok()) GTEST_SKIP() << "pipe2 不可用";

  Buffer buf;  // 默认 Config：initial 4096 / max 1 MiB / hwm 256 KiB
  const std::string one_kb(1024, 'x');

  std::size_t rounds_with_syscall_gt1 = 0;
  for (int i = 0; i < 1000; ++i) {
    pipe.write_all(one_kb);
    const auto r = buf.read_until_blocked(pipe.rfd());
    ASSERT_TRUE(r.would_block) << "第 " << i << " 轮没读到 EAGAIN";
    ASSERT_TRUE(r.limit_reached == false);
    ASSERT_EQ(r.bytes_read, 1024u);
    if (r.read_syscalls > 1) ++rounds_with_syscall_gt1;
    std::uint8_t scratch[1024];
    ASSERT_EQ(buf.retrieve(scratch, sizeof(scratch)), 1024u);
    ASSERT_EQ(buf.readable_bytes(), 0u);
  }

  // 1 KiB 报文在 4 KiB 初始容量上反复收发，容量一次都不该涨。
  // 涨了就说明"读空归零 + 压缩"没兜住稳态，每连接内存会被白白抬高。
  std::printf("[S1-02] keep-alive 1KiB x1000: initial_capacity=%zu peak_capacity=%zu "
              "final_capacity=%zu multi_syscall_rounds=%zu\n",
              buf.capacity(), buf.peak_capacity(), buf.capacity(), rounds_with_syscall_gt1);
  EXPECT_EQ(buf.peak_capacity(), 4096u);
  EXPECT_EQ(buf.capacity(), 4096u);
}

}  // namespace
}  // namespace l7gate::net
