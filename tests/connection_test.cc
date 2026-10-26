// S1-04：连接对象。
//
// 这里断言的全是"注释里写过的东西"，把它们变成可执行的东西：
//   1. 已扫描游标让 dribble 的扫描量回到线性（S1-03 结尾那笔 1956× 的债）
//   2. 解析出的 view 在 buffer 动过之后必须判定为作废（不能只靠我记得顺序）
//   3. 出向缓冲与 scratch 是惰性的（每连接内存预算，卡点四的数据来源）
//   4. 上游方向不解析请求（S1-04 与 S1-06 的分界）
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <thread>

#include "http/request_parser.h"
#include "net/connection.h"
#include "net/fd.h"
#include "net/stats.h"

namespace {

using l7gate::http::ParseError;
using l7gate::net::Connection;
using l7gate::net::ReactorStats;
using l7gate::net::UniqueFd;

class PipePair {
 public:
  PipePair() {
    int fds[2] = {-1, -1};
    if (::pipe2(fds, O_NONBLOCK) == 0) {
      rfd_ = fds[0];
      wfd_ = fds[1];
    }
  }
  ~PipePair() {
    if (wfd_ >= 0) ::close(wfd_);
  }

  bool ok() const { return rfd_ >= 0 && wfd_ >= 0; }
  // 读端交给 Connection 持有；这里只留写端，析构时写端关闭即可。
  UniqueFd take_read_end() { return UniqueFd{rfd_}; }

  void write_one_byte(char c) {
    while (true) {
      const ssize_t n = ::write(wfd_, &c, 1);
      if (n == 1) return;
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        std::this_thread::yield();
        continue;
      }
      ADD_FAILURE() << "pipe write failed errno=" << errno;
      return;
    }
  }

  void write_all(std::string_view data) {
    std::size_t off = 0;
    int spins = 0;
    while (off < data.size()) {
      const ssize_t n = ::write(wfd_, data.data() + off, data.size() - off);
      if (n > 0) {
        off += static_cast<std::size_t>(n);
        continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        ASSERT_LT(++spins, 1000000) << "管道始终写不进去";
        std::this_thread::yield();
        continue;
      }
      ADD_FAILURE() << "pipe write failed errno=" << errno;
      return;
    }
  }

 private:
  int rfd_ = -1;
  int wfd_ = -1;
};

std::string padding_request(std::size_t header_lines) {
  std::string text = "GET /long/headers HTTP/1.1\r\nHost: gateway.test\r\n";
  for (std::size_t i = 0; i < header_lines; ++i) {
    text += "X-Pad-" + std::to_string(i) + ": ";
    text.append(48, 'a');
    text += "\r\n";
  }
  text += "\r\n";
  return text;
}

Connection::Config default_config() {
  Connection::Config config;
  return config;
}

TEST(S1_04_Connection, HeaderDribbleScanIsLinearNotQuadratic) {
  PipePair pipe;
  ASSERT_TRUE(pipe.ok());
  ReactorStats stats;
  Connection conn(pipe.take_read_end(), Connection::Role::kDownstream, default_config(), stats);

  const std::string request = padding_request(50);
  const std::size_t n = request.size();

  for (char c : request) {
    pipe.write_one_byte(c);
    conn.on_readable();
  }

  // 没有游标时这里是 Σk = n(n+1)/2。n=3867 那一档 S1-03 实测重扫 7,474,911 字节，
  // 而同一份负载加了游标之后只应该扫"新字节 + 每轮最多回退 3 字节"。
  const std::size_t quadratic_reference = n * (n + 1) / 2;
  EXPECT_GT(quadratic_reference, 1'000'000u) << "样本太小，比较没有意义";

  EXPECT_EQ(conn.parse_calls(), 1u) << "游标定位到终止串之前不该调解析器，之后不该重复调";
  EXPECT_LE(conn.header_scan_bytes(), 4 * n)
      << "扫描量回到线性：实测 " << conn.header_scan_bytes() << " / 无游标参照 "
      << quadratic_reference;
  EXPECT_EQ(stats.requests_parsed.load(std::memory_order_relaxed), 1u);

  const auto* parsed = conn.request();
  ASSERT_NE(parsed, nullptr);
  EXPECT_EQ(parsed->method, "GET");
  EXPECT_GT(parsed->headers.size(), 10u);

  // 把两个数打出来，评审时不用重跑就能看见债还得多少。
  std::printf("[S1-04 dribble] 游标扫描=%zu 无游标参照=Σk=%zu 比值=%.0f 倍 parse_calls=%zu\n",
              conn.header_scan_bytes(), quadratic_reference,
              static_cast<double>(quadratic_reference) /
                  static_cast<double>(conn.header_scan_bytes()),
              conn.parse_calls());
}

TEST(S1_04_Connection, RepeatedParseDoesNotDoubleCountRequests) {
  PipePair pipe;
  ASSERT_TRUE(pipe.ok());
  ReactorStats stats;
  Connection conn(pipe.take_read_end(), Connection::Role::kDownstream, default_config(), stats);

  const std::string request = "GET /one HTTP/1.1\r\nHost: gateway.test\r\n\r\n";
  pipe.write_all(request);
  auto first = conn.on_readable();
  ASSERT_TRUE(first.request_ready);
  // 已解析出请求、但调用方还没 finish_request。此时再来一轮（比如超时唤醒）
  // 不该把它当成第二个请求。
  auto second = conn.on_readable();
  EXPECT_FALSE(second.request_ready);
  EXPECT_EQ(conn.parse_calls(), 1u);
  EXPECT_EQ(stats.requests_parsed.load(std::memory_order_relaxed), 1u);
}

TEST(S1_04_Connection, SequentialRequestsOnOneConnectionAdvanceCursor) {
  PipePair pipe;
  ASSERT_TRUE(pipe.ok());
  ReactorStats stats;
  Connection conn(pipe.take_read_end(), Connection::Role::kDownstream, default_config(), stats);

  const std::string a = "GET /first HTTP/1.1\r\nHost: gateway.test\r\n\r\n";
  const std::string b = "GET /second HTTP/1.1\r\nHost: gateway.test\r\n\r\n";

  pipe.write_all(a + b);  // 粘包：两个请求一次到
  auto round = conn.on_readable();
  ASSERT_TRUE(round.request_ready);
  EXPECT_EQ(round.request_bytes, a.size());
  conn.finish_request(round.request_bytes);

  round = conn.on_readable();
  ASSERT_TRUE(round.request_ready);
  EXPECT_EQ(round.request_bytes, b.size());
  const auto* parsed = conn.request();
  ASSERT_NE(parsed, nullptr);
  EXPECT_EQ(parsed->target, "/second");
  conn.finish_request(round.request_bytes);

  EXPECT_EQ(conn.inbound().readable_bytes(), 0u);
}

TEST(S1_04_Connection, ConsumeInvalidatesParsedViews) {
  PipePair pipe;
  ASSERT_TRUE(pipe.ok());
  ReactorStats stats;
  Connection conn(pipe.take_read_end(), Connection::Role::kDownstream, default_config(), stats);

  pipe.write_all("GET /target HTTP/1.1\r\nHost: gateway.test\r\n\r\n");
  auto round = conn.on_readable();
  ASSERT_TRUE(round.request_ready);
  ASSERT_NE(conn.request(), nullptr);

  const std::uint64_t before = conn.inbound().generation();
  conn.inbound().consume(1);  // 模拟"还没取用 view 就先动了 buffer"
  EXPECT_NE(conn.inbound().generation(), before) << "consume 必须推进 generation";
  EXPECT_EQ(conn.request(), nullptr) << "view 已作废，读取必须被拒而不是给过期数据";
}

TEST(S1_04_Connection, GrowthMovesStorageButLosesNoBytes) {
  PipePair pipe;
  ASSERT_TRUE(pipe.ok());
  ReactorStats stats;
  Connection::Config config;
  config.inbound.initial_capacity = 256;
  config.inbound.max_capacity = 64u << 10;
  config.inbound.high_water_mark = 64u << 10;
  Connection conn(pipe.take_read_end(), Connection::Role::kDownstream, config, stats);

  const std::string first = "GET /a HTTP/1.1\r\nHost: gateway.test\r\n\r\n";
  pipe.write_all(first);
  ASSERT_TRUE(conn.on_readable().request_ready);
  ASSERT_NE(conn.request(), nullptr);
  const std::size_t capacity_before = conn.inbound().capacity();
  const std::uint64_t generation_before = conn.inbound().generation();

  // 第二块字节把 256 的初始容量顶满，读循环里必然发生扩容（换 storage_ 地址）。
  const std::string second = padding_request(30);
  pipe.write_all(second);
  conn.on_readable();

  ASSERT_GT(conn.inbound().capacity(), capacity_before) << "这一步没发生扩容，测试的前提不成立";
  EXPECT_GT(conn.inbound().generation(), generation_before)
      << "换了底层地址却不推进 generation，那就是把作废信号丢了";
  // 扩容本身不改内容：可读区还是"第一条 + 第二条"，一个字节都没少。
  // 这条断言是下面那条（重解析而不是丢掉请求）的前提 —— 字节要是会少，
  // "重解析"就只是把丢数据这件事藏得更好看。
  EXPECT_EQ(conn.inbound().readable_bytes(), first.size() + second.size());
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(conn.inbound().readable_data()), first.size()),
            first)
      << "还没被收走的第一条请求，它的字节必须还完整地留在缓冲区里";
}

TEST(S1_04_Connection, ReleaseScratchInvalidatesViews) {
  PipePair pipe;
  ASSERT_TRUE(pipe.ok());
  ReactorStats stats;
  Connection conn(pipe.take_read_end(), Connection::Role::kDownstream, default_config(), stats);

  pipe.write_all("GET /a HTTP/1.1\r\nHost: gateway.test\r\n\r\n");
  ASSERT_TRUE(conn.on_readable().request_ready);
  ASSERT_TRUE(conn.has_scratch());
  ASSERT_NE(conn.request(), nullptr);

  conn.release_scratch();  // S1-05 的 Upgrade 路径会调它
  EXPECT_FALSE(conn.has_scratch()) << "release 之后 scratch 不该还占着内存";
  EXPECT_EQ(conn.request(), nullptr) << "headers 的存储随 scratch 一起没了，view 必须作废";
}

TEST(S1_04_Connection, OutboundAndScratchAreLazy) {
  PipePair pipe;
  ASSERT_TRUE(pipe.ok());
  ReactorStats stats;
  Connection conn(pipe.take_read_end(), Connection::Role::kDownstream, default_config(), stats);

  // 检查本身不许改变被检查的东西。第一版的 getter 是惰性创建的，于是
  // `EXPECT_EQ(conn.scratch(), nullptr)` 这句话自己把 scratch 建了出来 ——
  // 断言失败的原因不是实现错，是接口在说谎。所以这里改成反复调用观测接口，
  // 再用计数器证明一次都没分配。
  for (int i = 0; i < 3; ++i) {
    EXPECT_FALSE(conn.has_outbound());
    EXPECT_FALSE(conn.has_scratch());
    EXPECT_EQ(conn.outbound(), nullptr);
    EXPECT_EQ(conn.scratch(), nullptr);
    EXPECT_EQ(conn.outbound_pending(), 0u);
  }
  EXPECT_EQ(stats.outbound_buffers_created.load(std::memory_order_relaxed), 0u);
  EXPECT_EQ(stats.scratches_created.load(std::memory_order_relaxed), 0u);

  ASSERT_TRUE(conn.queue_bytes("hello", 5));
  EXPECT_TRUE(conn.has_outbound());
  EXPECT_EQ(stats.outbound_buffers_created.load(std::memory_order_relaxed), 1u);

  pipe.write_all("GET /a HTTP/1.1\r\nHost: gateway.test\r\n\r\n");
  conn.on_readable();
  EXPECT_EQ(stats.scratches_created.load(std::memory_order_relaxed), 1u)
      << "解析才需要 scratch，构造连接不该分配它";
}

TEST(S1_04_Connection, QueueBytesRefusesWholeBlockAboveHighWaterMark) {
  PipePair pipe;
  ASSERT_TRUE(pipe.ok());
  ReactorStats stats;
  Connection::Config config;
  config.outbound.initial_capacity = 256;
  config.outbound.max_capacity = 4096;
  config.outbound.high_water_mark = 512;
  Connection conn(pipe.take_read_end(), Connection::Role::kDownstream, config, stats);

  // 600 > 512：整块拒绝，一个字节都不许进缓冲。半装的状态比拒绝更难处理 ——
  // 装进去的是"半个响应"，网关自己就成了畸形报文的来源。
  EXPECT_FALSE(conn.queue_bytes(std::string(600, 'x').data(), 600));
  EXPECT_EQ(conn.outbound_pending(), 0u);

  EXPECT_TRUE(conn.queue_bytes(std::string(400, 'y').data(), 400));
  EXPECT_EQ(conn.outbound_pending(), 400u);
  EXPECT_FALSE(conn.queue_bytes(std::string(200, 'z').data(), 200));
  EXPECT_EQ(conn.outbound_pending(), 400u) << "拒绝之后不该留下部分数据";
}

TEST(S1_04_Connection, UpstreamRoleNeverParses) {
  PipePair pipe;
  ASSERT_TRUE(pipe.ok());
  ReactorStats stats;
  Connection conn(pipe.take_read_end(), Connection::Role::kUpstream, default_config(), stats);

  const std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc";
  pipe.write_all(response);
  auto round = conn.on_readable();
  EXPECT_FALSE(round.request_ready);
  EXPECT_EQ(conn.parse_calls(), 0u);
  EXPECT_EQ(stats.parse_calls.load(std::memory_order_relaxed), 0u);
  EXPECT_EQ(conn.inbound().readable_bytes(), response.size())
      << "上游方向只搬字节，响应解析是 S1-06 的范围";
  EXPECT_FALSE(conn.has_scratch()) << "上游不该为解析请求分配 scratch";
}

TEST(S1_04_Connection, OversizedHeadersRejectedWithoutParsing) {
  PipePair pipe;
  ASSERT_TRUE(pipe.ok());
  ReactorStats stats;
  Connection::Config config;
  config.parser.max_headers_bytes = 2048;
  Connection conn(pipe.take_read_end(), Connection::Role::kDownstream, config, stats);

  // 永远没有 CRLFCRLF 终止串，但字节数一路超过上限 —— 这条路径归游标管，
  // 归的是**容量**裁决（再也不可能合法），不是合法性裁决。
  const std::string never_ends = padding_request(200);
  for (char c : never_ends) {
    pipe.write_one_byte(c);
    auto round = conn.on_readable();
    if (round.reject) {
      EXPECT_EQ(round.error, ParseError::kHeadersTooLarge);
      EXPECT_GT(conn.inbound().readable_bytes(), 2048u);
      EXPECT_EQ(conn.parse_calls(), 0u) << "超限是在定位阶段拦下的，没走到解析器";
      return;
    }
  }
  FAIL() << "头部超限没有被拒绝";
}

TEST(S1_04_Connection, InvalidatedViewsAreReparsedNotDropped) {
  // 请求已解析、还没被调用方收走，这期间新字节把 buffer 顶到了扩容 —— 旧 view 作废。
  // 正确动作是"把同一个请求重解析一次"，而不是丢掉它，也不是停在
  // "request_ready_ 挡着新解析、作废 view 又没人能用"的死区里。
  // 那个死区的表现是一条连接安静地不再处理任何东西，而客户端正在等它的响应。
  PipePair pipe;
  ASSERT_TRUE(pipe.ok());
  ReactorStats stats;
  Connection::Config config;
  config.inbound.initial_capacity = 256;
  config.inbound.max_capacity = 64u << 10;
  config.inbound.high_water_mark = 64u << 10;
  Connection conn(pipe.take_read_end(), Connection::Role::kDownstream, config, stats);

  const std::string first = "GET /a HTTP/1.1\r\nHost: gateway.test\r\n\r\n";
  pipe.write_all(first);
  ASSERT_TRUE(conn.on_readable().request_ready);
  ASSERT_NE(conn.request(), nullptr);
  const std::size_t capacity_before = conn.inbound().capacity();

  // 这一批字节把 256 的初始容量顶满，读循环里必然扩容（storage_ 换地址）。
  pipe.write_all(padding_request(30));
  conn.on_readable();
  ASSERT_GT(conn.inbound().capacity(), capacity_before) << "这一步没扩容，测试前提不成立";

  ASSERT_NE(conn.request(), nullptr) << "view 作废后必须重解析出等价结果，不能返回空";
  EXPECT_EQ(conn.request()->target, "/a") << "重解析拿到的还得是同一个请求";
  EXPECT_EQ(stats.requests_parsed.load(std::memory_order_relaxed), 1u)
      << "同一个请求重解析不许在 requests_parsed 上算两次";
  EXPECT_EQ(conn.parse_calls(), 2u) << "但它确实是一次真的解析工作，parse_calls 要诚实";

  // 收走第一条之后，剩下的字节仍要能被继续处理 —— 游标不能因为重解析而错位。
  conn.finish_request(conn.request()->consumed);
  ASSERT_TRUE(conn.on_readable().request_ready);
  EXPECT_EQ(conn.request()->target, "/long/headers");
}

}  // namespace
