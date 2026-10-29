// S1-04：连接对象。
//
// 这里断言的全是"注释里写过的东西"，把它们变成可执行的东西：
//   1. 已扫描游标让 dribble 的扫描量回到线性（S1-03 结尾那笔 1956× 的债）
//   2. 解析出的 view 在 buffer 动过之后必须判定为作废（不能只靠我记得顺序）
//   3. 出向缓冲与 scratch 是惰性的（每连接内存预算，卡点四的数据来源）
//   4. 上游方向不解析请求（S1-04 与 S1-06 的分界）
#include <fcntl.h>
#include <malloc.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "http/request_parser.h"
#include "net/connection.h"
#include "net/fd.h"
#include "net/stats.h"
#include "pipe_fixture.h"

namespace {

using l7gate::http::ParseError;
using l7gate::http::ParseScratch;
using l7gate::net::Buffer;
using l7gate::net::Connection;
using l7gate::net::ReactorStats;
using l7gate::net::UniqueFd;
using l7gate::testing::PipePair;

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

// --- S1-05：101 检测与隧道态的连接层语义 -----------------------------------
// reactor_test.cc 里的 S1_05_Reactor 用真 socket 走完整闭环；这里只打连接对象
// 自己的两条不变量：**半包不许误判**（和 S1-03 请求侧那条全称断言同一条哲学），
// 以及**隧道里没有解析**（on_readable 的短路不靠调用方记得绕开它）。

TEST(S1_05_Connection, ResponseHeadHalfPacketIsNeverATunnel) {
  constexpr const char* kHead =
      "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
      "Connection: Upgrade\r\n\r\n";
  PipePair pipe;
  ASSERT_TRUE(pipe.ok());
  ReactorStats stats;
  Connection conn(pipe.take_read_end(), Connection::Role::kUpstream, default_config(), stats);

  const std::string_view head(kHead);
  // 每一个真前缀都必须"没结论"，而不是报错或误判——这些前缀会原样躺在缓冲里
  // 等下一批字节，若某一轮把它当非 101 定论，隧道就永远进不去了。
  // 每写一字节都要 read_raw() 把它搬进入向缓冲：detect 看的是缓冲，不是 fd——
  // 少了这一步，整个循环扫的是一片空缓冲，"全绿"证明的是 nothing。
  for (std::size_t i = 0; i + 1 < head.size(); ++i) {
    pipe.write_all(head.substr(i, 1));
    conn.read_raw();
    EXPECT_EQ(conn.detect_response_head_101(), std::string_view::npos)
        << "前缀长度 " << i + 1 << " 不该有任何结论";
  }
  // 最后一个字节落地：完整头，返回的长度必须恰好是整个头。
  pipe.write_all(head.substr(head.size() - 1));
  conn.read_raw();
  const std::size_t got = conn.detect_response_head_101();
  ASSERT_NE(got, std::string_view::npos);
  EXPECT_EQ(got, head.size()) << "报出来的头长要能把 101 整头原样交给下游，一字节不多一字节不少";
}

TEST(S1_05_Connection, OtherResponseHeadsAreNotTunnels) {
  const char* kSamples[] = {
      "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n",
      "HTTP/1.1 1100 x\r\n\r\n\r\n",     // 状态码四位 + 头后还有字节：三个口径同时不成立
      "HTTP/1.1 1015 x\r\n\r\n",         // 第 12 字节既不是 SP 也不是 CR：三位码判定不许放宽
      "HTTP/2 101 -\r\n\r\n",            // ADR-01：没有 HTTP/2
      "GEauxHTTP/1.1 101\r\n\r\n",       // 不从头对齐的伪装串
  };
  for (const char* sample : kSamples) {
    PipePair pipe;
    ASSERT_TRUE(pipe.ok());
    ReactorStats stats;
    Connection conn(pipe.take_read_end(), Connection::Role::kUpstream, default_config(), stats);
    const std::string_view text(sample);
    pipe.write_all(text);
    conn.on_readable();  // 走生产路径：字节经由 read_raw 所在的同一只读 fd 进入缓冲
    EXPECT_EQ(conn.detect_response_head_101(), std::string_view::npos) << "样本不该成隧道: " << sample;
  }
  // 与之相对的一条正例，钉住上面的口径：前缀比较只钉 "HTTP/1."，**minor 版本号
  // 不在判定范围内**——上游回 "HTTP/1.0 101" 是真实存在的形态（老上游 + 网关不
  // 挑剔版本），把它拒掉等于把能成的握手判成不能成。这条不是从实现里抄出来的
  // 宽容，是验收句"101 之后走透传"的字面含义：判定认的是 101 这个状态码。
  {
    PipePair pipe;
    ASSERT_TRUE(pipe.ok());
    ReactorStats stats;
    Connection conn(pipe.take_read_end(), Connection::Role::kUpstream, default_config(), stats);
    constexpr const char* kHead10 = "HTTP/1.0 101\r\n\r\n";
    const std::string_view head(kHead10);
    pipe.write_all(head);
    conn.on_readable();
    EXPECT_EQ(conn.detect_response_head_101(), head.size())
        << "HTTP/1.0 的完整 101 头必须成立：口径已写在实现注释与 ADR-09 里";
  }
  // 纯函数与连接层的分工在这一条里说透：looks_like_101 拿 "…\r\n\r\nEXTRA" 整串
  // 当 len 会拒（终止串不在末端），但 detect **必须**接受——它问的是"可读区的
  // 开头是不是一个完整 101 头、占几个字节"，头后那些字节恰恰是隧道的第一帧。
  // 把 detect 也判成 npos 才是 bug：101 + 首帧同段到达（TCP 合包的常态）时，
  // 隧道永远进不去，首帧还会被当未识别的响应头一直压着。
  {
    PipePair pipe;
    ASSERT_TRUE(pipe.ok());
    ReactorStats stats;
    Connection conn(pipe.take_read_end(), Connection::Role::kUpstream, default_config(), stats);
    constexpr const char* kHeadWithFrame =
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n\r\n\x81\x05\xff\xfe";
    const std::string_view with_frame(kHeadWithFrame);
    const std::size_t head_len = with_frame.size() - 4;
    pipe.write_all(with_frame);
    conn.on_readable();
    EXPECT_EQ(conn.detect_response_head_101(), head_len)
        << "报出来的头长必须恰好切在终止串末端：多一字节就吞了帧，少一字节帧前漏字节";
  }
}

TEST(S1_05_Connection, TunnelModeShortCircuitsParsing) {
  PipePair pipe;
  ASSERT_TRUE(pipe.ok());
  ReactorStats stats;
  Connection conn(pipe.take_read_end(), Connection::Role::kDownstream, default_config(), stats);

  // 先把一条正常请求解析出来（scratch 存在），再进隧道并释放 scratch。
  pipe.write_all("GET /a HTTP/1.1\r\nHost: gateway.test\r\n\r\n");
  ASSERT_TRUE(conn.on_readable().request_ready);
  ASSERT_TRUE(conn.has_scratch());
  conn.enter_tunnel();
  conn.release_scratch();
  ASSERT_FALSE(conn.has_scratch());

  // 隧道里喂"畸形 HTTP 的二进制帧"：不许产生任何解析动作、不许 reject。
  // 这正是 on_readable 短路存在的理由——少了它，这串字节会被当请求解析成畸形，
  // 透传逻辑自己把隧道掐了（ADR-09 说的"卡死"之外的另一种死法）。
  conn.on_readable();  // 把 /a 的请求字节消费掉之前先模拟 reactor 的收走
  conn.finish_request(conn.inbound().readable_bytes());
  pipe.write_all("\x81\x05\xff\x00" "binary-not-http");  // 拆成两段字面量：不拆的话 \x00b 会被当十六进制吞并
  const auto round = conn.on_readable();
  EXPECT_FALSE(round.request_ready);
  EXPECT_FALSE(round.reject);
  EXPECT_EQ(conn.parse_calls(), 1u) << "隧道里的字节不该再触发一次解析";
  EXPECT_FALSE(conn.has_scratch()) << "没有解析发生，scratch 也就不该被惰性重建——白省的 2048 字节回来了";
  EXPECT_EQ(stats.scratches_created.load(std::memory_order_relaxed), 1u);
  EXPECT_EQ(conn.request(), nullptr);
}

TEST(S1_05_Connection, UpgradeIntentSurvivesRequestAndIsTakenOnce) {
  // 交接段点名的"意图是一次性的"钉在这里，不钉在 reactor 层：时序责任全在
  // mark / take / enter 这三个接口上，而 reactor 用例跑在宽松 echo 上游上，
  // 同一条连接追加第二条请求会被上游当隧道字节弹回——在那儿断言测的是对端脾气。
  PipePair pipe;
  ASSERT_TRUE(pipe.ok());
  ReactorStats stats;
  Connection conn(pipe.take_read_end(), Connection::Role::kDownstream, default_config(), stats);

  pipe.write_all("GET /ws HTTP/1.1\r\nHost: gateway.test\r\n"
                 "Connection: keep-alive, Upgrade\r\nUpgrade: websocket\r\n\r\n");
  ASSERT_TRUE(conn.on_readable().request_ready);
  ASSERT_NE(conn.request(), nullptr);

  conn.mark_upgrade_request();  // 转发前调用，此时 view 还有效
  EXPECT_TRUE(conn.pending_upgrade());
  const std::size_t consumed = conn.request()->consumed;
  conn.finish_request(consumed);
  EXPECT_EQ(conn.request(), nullptr) << "前提：请求连同 view 已经被消费掉";
  // 意图必须活得比 request 久——101 是**下一个事件**里才到的，那时 view 早作废了。
  EXPECT_TRUE(conn.pending_upgrade()) << "握手意图随请求消费一起消失：隧道永远进不去";

  EXPECT_TRUE(conn.take_upgrade_pending());
  EXPECT_FALSE(conn.pending_upgrade());
  // 用"取"不用"读"的意义就在这两行：同一条连接上更晚的普通响应，
  // 不许被更早一次没成形的 upgrade 意图误点燃。
  EXPECT_FALSE(conn.take_upgrade_pending()) << "take-once：第二次取必须是空";

  // enter_tunnel 幂等：重复调用无副作用（reactor 的触发点只有一个，但"只有一个"
  // 是靠代码读出来的，不是靠接口保证的——接口自己保证，才不用每次都去读调用方）。
  conn.enter_tunnel();
  conn.enter_tunnel();
  EXPECT_TRUE(conn.tunneling());
}

// ==================== S2-07 第③项：每连接内存的构成表 ====================
//
// 这一节还的是两笔账，都不许靠我算：
//
// 一，**B-007 那个 17.47 KiB/对 至今只有商数，没有分子的分摊**。SLO #2 判的就是"每连接内存"，
//     而它的构成从 S1 起欠着一张表（入向 buffer / 出向 buffer / scratch / `Connection` 对象 /
//     池摊销 / 分配器取整）。这里每一格都是当场读的：容量是问 `Buffer::capacity()` 要的，
//     对象大小是问 `sizeof` 要的，隧道态有没有留 scratch 是问 `has_scratch()` 要的，
//     分配器取整是问 `mallinfo2()` 要的（它报的是**堆里实际占用**，含取整，不含我推测）。
//     字节量抄自那一档网关自己的账，`EXPECT_EQ` 把它们钉住：163 B 的升级请求
//     （`bytes_down_to_upstream=16,300,000 ÷ 100,000`）与 129 B 的 101 头
//     （`bytes_tunnel_up=12,900,000 ÷ 100,000`），业务帧零条 —— 所以这张表算的是
//     **SLO #2 那一档**的持有成本，不是任意负载下的。
//
// 二，**S1-07 留下的那 0.50 KiB/连接 空项**（128 条空闲连接从 5.31 涨到 5.81，而对象只涨 8 B）。
//     下面第二条用例判的是"这个数在 N=128 上到底有没有分辨率"：同一条代码路径上取 N=128 与
//     N=1024 两个点，堆字节/连接应当与 N 无关；而 VmRSS 是按**堆扩展步长**一跳一跳长的，
//     步长实测出来除以 128 就是那一格的分辨率。归得掉就归，归不掉的部分按验收句明写"仍无法归因 + 量级"。
//
// 复刻的分配形状与生产一致：`std::unordered_map<int, std::shared_ptr<Connection>>` 注册表 +
// `make_shared`（控制块与对象同一次分配）。换成裸 new 或 vector<Connection> 就少算了注册表节点
// 与控制块，那张表会短一截而短的那一截正好是我想要知道的东西。

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr bool k_heap_is_instrumented = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
constexpr bool k_heap_is_instrumented = true;
#else
constexpr bool k_heap_is_instrumented = false;
#endif
#else
constexpr bool k_heap_is_instrumented = false;
#endif

// 隧道那一条下游请求（c100k 的 kUpgradeRequest 原文，163 字节）。
constexpr char kTunnelUpgradeRequest[] =
    "GET /ws HTTP/1.1\r\n"
    "Host: c100k.test\r\n"
    "Connection: keep-alive, Upgrade\r\n"
    "Upgrade: websocket\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "\r\n";

// 夹具回的 101 头（bench_upstream 的 upgrade_response_ 原文，129 字节）。
constexpr char kTunnelSwitchingHead[] =
    "HTTP/1.1 101 Switching Protocols\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
    "\r\n";

// B-007 那一档的实测锚点：(1,751,020 − 4,444) KiB ÷ 100,000 对。
constexpr double kB007PerPairKiB = 17.47;
constexpr std::size_t kB007PerPairBytes = static_cast<std::size_t>(kB007PerPairKiB * 1024.0);

std::uint64_t heap_bytes_in_use() {
  // glibc 2.33+ 的 mallinfo2（本机 glibc 2.43）。uordblks = 堆里"在用户手里"的字节数，
  // 已经含上每次分配的取整；它不是 RSS，也不含 mmap 出去的大块 —— 所以这两栏在表里分开列。
  return static_cast<std::uint64_t>(::mallinfo2().uordblks);
}

// **这条仪器在 sanitizer 下是瞎的**：ASan/TSan 换掉分配器，`mallinfo2()` 一律报 0
// （2026-10-29 实测：asan-ubsan 下 N=1024 那一段"堆在用量 +0 KiB"，而同一段 VmRSS +12,660 KiB/连接，
// 那是插桩的影子内存不是我的 Connection）。所以判定必须整个跳过，而不是留几条断言在那里
// 以"0 > 0"的形状红掉——红的原因和要判的事毫无关系，那种红只会把人往错的地方引。
// 同一条立场见 ARCHITECTURE 1.10 的 `k_rss_is_instrumented`（sanitizer 下不判 20 KiB 预算）。
bool heap_accounting_available() {
  return !k_heap_is_instrumented && heap_bytes_in_use() > 0;
}

std::uint64_t process_rss_bytes() {
  std::FILE* file = std::fopen("/proc/self/status", "r");
  if (file == nullptr) return 0;  // 读不到就是没测到；调用方按"没测到"处理，不拿 0 当结论
  char line[256];
  std::uint64_t rss_kib = 0;
  bool found = false;
  while (std::fgets(line, sizeof(line), file)) {
    if (std::strncmp(line, "VmRSS:", 6) == 0) {
      rss_kib = std::strtoull(line + 6, nullptr, 10);
      found = true;
      break;
    }
  }
  std::fclose(file);
  return found ? rss_kib * 1024ull : 0;
}

TEST(S2_07_MemoryComposition, TunnelPairHoldsExactlyWhatTheTableClaims) {
  // 先把字节量钉住：这两个数如果和网关那一档的账不一致，整张表算的就是另一种负载。
  EXPECT_EQ(sizeof(kTunnelUpgradeRequest) - 1, 163u)
      << "B-007 的 bytes_down_to_upstream=16,300,000 / established=100,000 = 每条 163 字节";
  EXPECT_EQ(sizeof(kTunnelSwitchingHead) - 1, 129u)
      << "B-007 的 bytes_tunnel_up=12,900,000 / 100,000 = 每条 129 字节";

  constexpr std::size_t kPairs = 256;
  ReactorStats stats;
  // 与 reactor.h:350 同一个容器类型、同一种 make_shared：注册表节点与控制块都是每连接的钱。
  std::unordered_map<int, std::shared_ptr<Connection>> registry;
  registry.reserve(kPairs * 2);
  Connection::Config config;  // 默认：inbound/outbound 的 initial_capacity 都是 4096

  const std::uint64_t heap_before = heap_bytes_in_use();
  const std::uint64_t rss_before = process_rss_bytes();

  for (std::size_t i = 0; i < kPairs; ++i) {
    PipePair downstream_pipe;
    PipePair upstream_pipe;
    ASSERT_TRUE(downstream_pipe.ok());
    ASSERT_TRUE(upstream_pipe.ok());
    // 先把两条侧各自要走的字节灌进管道，再建对象：隧道配对里下游收 163 B 请求、
    // 上游收 129 B 的 101 头，这两个量决定 buffer 停在哪个容量。
    downstream_pipe.write_all(kTunnelUpgradeRequest);
    upstream_pipe.write_all(kTunnelSwitchingHead);

    auto downstream = std::make_shared<Connection>(downstream_pipe.take_read_end(),
                                                   Connection::Role::kDownstream, config, stats);
    auto upstream = std::make_shared<Connection>(upstream_pipe.take_read_end(),
                                                 Connection::Role::kUpstream, config, stats);
    registry[downstream->fd()] = downstream;
    registry[upstream->fd()] = upstream;

    // 转发路径的顺序照 reactor 的来：解析 → 记 upgrade 意图 → 把请求字节排进上游出向 →
    // 上游的 101 头排进下游出向 → 消费请求 → 进隧道 → 释放 scratch。
    const auto down_round = downstream->on_readable();
    ASSERT_TRUE(down_round.request_ready) << "第 " << i << " 条升级请求没解析成请求";
    ASSERT_TRUE(downstream->has_scratch()) << "解析发生了却没人建 scratch：表里的 scratch 一格是空的";
    downstream->mark_upgrade_request();
    ASSERT_TRUE(upstream->queue_bytes(kTunnelUpgradeRequest, sizeof(kTunnelUpgradeRequest) - 1));

    const auto up_round = upstream->on_readable();
    EXPECT_FALSE(up_round.request_ready) << "上游方向不该解析请求（S1-04 定的分界）";
    ASSERT_TRUE(downstream->queue_bytes(kTunnelSwitchingHead, sizeof(kTunnelSwitchingHead) - 1));

    downstream->finish_request(down_round.request_bytes);
    downstream->enter_tunnel();
    downstream->release_scratch();  // 全仓唯一触发点在 maybe_enter_tunnel 之后（ARCHITECTURE 1.13）
    upstream->enter_tunnel();
  }

  const std::uint64_t heap_after = heap_bytes_in_use();
  const std::uint64_t rss_after = process_rss_bytes();

  // ---- 逐格问对象，不问我的算术 ----
  std::size_t inbound_capacity = 0;
  std::size_t outbound_capacity = 0;
  std::size_t scratch_retained = 0;
  std::size_t tunneling_pairs = 0;
  for (const auto& entry : registry) {
    const auto& conn = entry.second;
    inbound_capacity += conn->inbound().capacity();
    if (conn->has_outbound()) outbound_capacity += conn->outbound()->capacity();
    if (conn->has_scratch()) ++scratch_retained;
    if (conn->tunneling()) ++tunneling_pairs;
  }
  EXPECT_EQ(tunneling_pairs, kPairs * 2) << "两侧都该在隧道态：隧道态才是 SLO #2 的口径";
  EXPECT_EQ(scratch_retained, 0u) << "隧道化之后还留着 scratch：每对多 2,048 字节，100k 下就是 200 MiB";
  EXPECT_EQ(stats.load(stats.scratches_created), static_cast<std::uint64_t>(kPairs))
      << "scratch 只该下游侧建（B-007：scratches_created=100,000 = 对数，不是 2×）";
  EXPECT_EQ(stats.load(stats.outbound_buffers_created), static_cast<std::uint64_t>(kPairs * 2))
      << "出向 buffer 两侧各一块（B-007：outbound_buffers_created=200,000 = 2×对数）";
  EXPECT_EQ(stats.load(stats.connections_constructed), static_cast<std::uint64_t>(kPairs * 2))
      << "连接对象自增 constructed（connection.cc:25）：两侧各一条，2×对数";

  // 结构那一半（容量、scratch 是否释放、几个对象、两侧都进隧道）在**任何**构建里都判——
  // 它们与分配器无关。分配器相关的那一半在下面这条 skip 之后。
  EXPECT_EQ(inbound_capacity, kPairs * 2 * 4096u);
  EXPECT_EQ(outbound_capacity, kPairs * 2 * 4096u);
  if (!heap_accounting_available()) {
    std::printf("[S2-07 构成表] 本机当前构建的堆会计不可用（sanitizer 换分配器，mallinfo2 报 0）："
                "数值那一半跳过，判定归 debug/release；结构那一半（容量 %zu / scratch 保留 %zu / "
                "隧道两侧 %zu）已判过\n",
                inbound_capacity, scratch_retained, tunneling_pairs);
    GTEST_SKIP() << "堆会计在这条构建里不可用，数值判定跳过（不是通过，是不判）";
  }

  const double heap_per_pair =
      static_cast<double>(heap_after - heap_before) / static_cast<double>(kPairs);
  const double rss_per_pair =
      static_cast<double>(rss_after > rss_before ? rss_after - rss_before : 0) /
      static_cast<double>(kPairs);
  // 四个 buffer 存储 + 两块堆上 Buffer 对象 + 两个 Connection（含控制块与注册表节点）。
  const std::size_t storages = inbound_capacity + outbound_capacity;
  // **每一对**的可点名字节。四个 buffer 存储按容量算；`Connection` 两个；出向 `Buffer` 对象
  // 两个（入向那个是 `Connection` 的内联成员，已经含在 528 里，不能再数一遍——我第一版就是
  // 拿 256 对的**总和**去和"每对"的锚点比，那条 EXPECT_LE 当场把它抓住：4,509,696 vs 17,889）。
  const double explicit_per_pair =
      static_cast<double>(storages) / static_cast<double>(kPairs) +
      2.0 * static_cast<double>(sizeof(Connection)) +
      2.0 * static_cast<double>(sizeof(Buffer));
  // 数不进去的那一角：两个 `shared_ptr` 控制块、注册表的两个节点、以及每次分配的取整。
  const double unenumerated_per_pair = heap_per_pair - explicit_per_pair;

  std::printf("[S2-07 构成表] 隧道对=%zu 入向容量和=%zu 出向容量和=%zu 存储合计=%zu B\n",
              kPairs, inbound_capacity, outbound_capacity, storages);
  std::printf("[S2-07 构成表] sizeof(Connection)=%zu sizeof(Buffer)=%zu sizeof(ParseScratch)=%zu "
              "inbound 初始容量=%zu\n",
              sizeof(Connection), sizeof(Buffer), sizeof(ParseScratch),
              config.inbound.initial_capacity);
  std::printf("[S2-07 构成表] 每对：可点名=%.0f B（存储 %.0f + 连接对象 %.0f + 出向 Buffer 对象 %.0f）"
              " 堆在用量=%.0f B（未点名一角=%.0f B：控制块+注册表节点+取整） RSS 增量=%.0f B/对（%.2f KiB）\n",
              explicit_per_pair, static_cast<double>(storages) / static_cast<double>(kPairs),
              2.0 * static_cast<double>(sizeof(Connection)),
              2.0 * static_cast<double>(sizeof(Buffer)),
              heap_per_pair, unenumerated_per_pair, rss_per_pair, rss_per_pair / 1024.0);
  std::printf("[S2-07 构成表] B-007 实测锚点=%.2f KiB/对（%zu B）→ 本档堆实测占它的 %.2f%%，"
              "scratch 保留=%.0f B（隧道化后已释放）\n",
              kB007PerPairKiB, kB007PerPairBytes,
              heap_per_pair * 100.0 / static_cast<double>(kB007PerPairBytes), 0.0);

  // 可点名的一格必须**装得下**在 B-007 那个商里，否则说明表里有格是凭空多算的。
  EXPECT_LE(explicit_per_pair, static_cast<double>(kB007PerPairBytes))
      << "点名的部分已经超过 B-007 的实测商：构成里有凭空多算的一格";
  // 走到这里说明堆会计可用（sanitizer 那几台在上面就 skip 了）。
  // 这一条是构成表的承重断言：**在进程里量到的每对堆字节**不得超过**生产实测的每对 RSS 商**。
  // 2026-10-29 debug 实测 17,861 B/对 vs B-007 的 17,889 B/对 = **0.16%** ——
  // 所以这条不是"留个松界混过去"，是两张独立取数的表（ctest 进程内 256 对 / 生产 100k 对）
  // 落在同一个数上。5% 的容差留给页粒度，不给方向性放宽。
  EXPECT_LE(heap_per_pair, kB007PerPairBytes * 1.05)
      << "本档实测每对堆字节 " << heap_per_pair << " B 明显超过 B-007 的 "
      << kB007PerPairBytes << " B/对：构成表里有一格是我算出来的，不是量出来的";
  // 未点名那一角要**小**：它是控制块 + 注册表节点 + 取整，不是"另一笔内存"。
  // 超过 1 KiB/对就说明我对分配器的理解有缺口，那一格必须显式写"仍无法归因"。
  EXPECT_GT(unenumerated_per_pair, 0.0)
      << "取整与注册表节点这一格是 0 或负数？说明我对分配器的理解有误";
  EXPECT_LT(unenumerated_per_pair, 1024.0)
      << "未点名一角 " << unenumerated_per_pair << " B/对 超过 1 KiB：构成表还欠一格";
}

TEST(S2_07_MemoryComposition, IdleConnectionCellAtN128IsBelowItsOwnResolution) {
  // 那 0.50 KiB/连接（5.31 → 5.81）是**两次 N=128 的 VmRSS 读数之差**，而对象只涨了 8 B。
  // 这一条问的是那把尺子本身：同一条代码路径上取 N=128 与 N=1024 两个点，
  //   ① 每连接的**堆在用量**应当与 N 无关（真变了就会随 N 走，而且 8 B 是量得出来的）；
  //   ② 同一段的 **VmRSS 增量**却可以完全不跟着堆走（本轮 N=128 那一段实测 +0 KiB，
  //      而同一段堆里要了 575 KiB）——页在更早的分配里就已经驻留了。
  // 两条合起来就是一句可判的话：在 N=128 这个尺度上，RSS/连接 的仪器粒度大于那条待判的差额，
  // 所以 5.31 与 5.81 之差**不构成"每连接内存变了"的证据**。归不掉的话我不说它归掉了。
  constexpr std::size_t kSmallN = 128;
  constexpr std::size_t kLargeN = 1024;
  ReactorStats stats;
  Connection::Config config;
  std::vector<std::shared_ptr<Connection>> small;
  std::vector<std::shared_ptr<Connection>> large;
  small.reserve(kSmallN);
  large.reserve(kLargeN);

  const std::uint64_t heap_origin = heap_bytes_in_use();
  const std::uint64_t rss_origin = process_rss_bytes();

  std::uint64_t rss_at_128 = 0;
  std::uint64_t heap_at_128 = 0;
  std::uint64_t largest_jump_bytes = 0;
  std::uint64_t previous_rss = rss_origin;
  std::size_t jumps = 0;

  for (std::size_t i = 0; i < kLargeN; ++i) {
    auto pipe = std::make_unique<PipePair>();
    ASSERT_TRUE(pipe->ok());
    auto conn = std::make_shared<Connection>(pipe->take_read_end(),
                                             Connection::Role::kDownstream, config, stats);
    // 故意**不发请求**：S1-07 那一格是"128 条空闲连接"，惰性分配下它只有对象 + 入向 buffer。
    // 写端随 pipe 析构关掉也无妨，这条用例一次都不读。
    if (i + 1 == kSmallN) {
      rss_at_128 = process_rss_bytes();
      heap_at_128 = heap_bytes_in_use();
    }
    const std::uint64_t now = process_rss_bytes();
    if (now > previous_rss) {
      const std::uint64_t delta = now - previous_rss;
      // 只把"比一页大得多"的跳变当堆扩展：一页 4 KiB 的常态增长是这条连接自己要的内存。
      if (delta > 16384) {
        ++jumps;
        if (delta > largest_jump_bytes) largest_jump_bytes = delta;
      }
    }
    previous_rss = now;
    if (i + 1 <= kSmallN) small.push_back(conn);
    else large.push_back(conn);
  }
  const std::uint64_t rss_at_1024 = process_rss_bytes();
  const std::uint64_t heap_at_1024 = heap_bytes_in_use();

  const double heap_per_conn_at_128 =
      static_cast<double>(heap_at_128 - heap_origin) / static_cast<double>(kSmallN);
  const double heap_per_conn_at_1024 =
      static_cast<double>(heap_at_1024 - heap_origin) / static_cast<double>(kLargeN);
  const double rss_per_conn_at_128 =
      static_cast<double>(rss_at_128 > rss_origin ? rss_at_128 - rss_origin : 0) /
      static_cast<double>(kSmallN);
  const double rss_per_conn_at_1024 =
      static_cast<double>(rss_at_1024 > rss_origin ? rss_at_1024 - rss_origin : 0) /
      static_cast<double>(kLargeN);

  const std::uint64_t heap_at_128_growth = heap_at_128 > heap_origin ? heap_at_128 - heap_origin : 0;
  const std::uint64_t heap_at_1024_growth = heap_at_1024 > heap_origin ? heap_at_1024 - heap_origin : 0;
  const std::uint64_t rss_at_128_growth = rss_at_128 > rss_origin ? rss_at_128 - rss_origin : 0;
  const std::uint64_t rss_at_1024_growth =
      rss_at_1024 > rss_origin ? rss_at_1024 - rss_origin : 0;

  std::printf("[S2-07 分辨率] 空闲下游连接的堆实测：N=128 时 %.0f B/连接、N=1024 时 %.0f B/连接"
              "（差 %.0f B；对象本身从 S1-06 到 S1-07 只涨了 8 B）\n",
              heap_per_conn_at_128, heap_per_conn_at_1024,
              heap_per_conn_at_128 > heap_per_conn_at_1024 ? heap_per_conn_at_128 - heap_per_conn_at_1024
                                                           : heap_per_conn_at_1024 - heap_per_conn_at_128);
  std::printf("[S2-07 分辨率] 同两段的 VmRSS：N=128 时 +%.0f KiB（%.0f B/连接）、"
              "N=1024 时 +%.0f KiB（%.0f B/连接）；同程堆在用量分别 +%.0f KiB / +%.0f KiB\n",
              static_cast<double>(rss_at_128_growth) / 1024.0, rss_per_conn_at_128,
              static_cast<double>(rss_at_1024_growth) / 1024.0, rss_per_conn_at_1024,
              static_cast<double>(heap_at_128_growth) / 1024.0,
              static_cast<double>(heap_at_1024_growth) / 1024.0);
  std::printf("[S2-07 分辨率] 途中出现 %zu 次 >16 KiB 的 VmRSS 跳变，最大一次 %.0f KiB\n",
              jumps, static_cast<double>(largest_jump_bytes) / 1024.0);

  if (!heap_accounting_available()) {
    std::printf("[S2-07 分辨率] 结构那一半已判（%zu 条连接、每条一个 fd 与一块入向缓冲）；"
                "堆会计与 RSS 粒度这一半在下面跳过（不是通过，是不判）\n", kLargeN);
    GTEST_SKIP() << "堆会计在这条构建里不可用，粒度判定跳过";
  }
  ASSERT_GT(heap_per_conn_at_1024, 0.0) << "N=1024 没量到堆增长：这一条根本没建连接";
  {
    // **每连接堆字节与 N 无关**：这条是那一格归因的地基。S1-07 那两次读数差 0.50 KiB/连接
    // （= 128 条上一共 64 KiB），而对象只涨 8 B（= 1 KiB），所以差的不可能是每连接对象。
    EXPECT_NEAR(heap_per_conn_at_128, heap_per_conn_at_1024, 128.0)
        << "同一份代码在 N=128 与 N=1024 上每连接堆字节差了超过 128 B：那 0.50 KiB 里有真的";
    // 而 VmRSS 在 N=128 这一段**几乎不动**（2026-10-29 实测 +0 KiB，而同一段堆里要了 575 KiB）：
    // 页在更早的分配里就已经驻留了。所以在 N=128 上用 RSS 差值判"每连接内存变了多少"，
    // 仪器本身的分辨率就大于那条待判的差额（0.50 KiB/连接）。
    if (rss_per_conn_at_128 < heap_per_conn_at_128) {
      std::printf("[S2-07 分辨率] 归因结论：N=128 时 VmRSS/连接（%.0f B）**小于**堆实测/连接（%.0f B），"
                  "说明那个尺度上的 RSS 读数由**页驻留时机**主导，不是每连接内存。"
                  "S1-07 的 5.31→5.81 KiB/连接（差 0.50 KiB = 总共 64 KiB）落在同一类粒度里 → "
                  "**归为读数粒度，不是未归因的内存**；每连接内存的可引用数字以堆实测与本仓库 "
                  "100k 档（B-007）为准。\n",
                  rss_per_conn_at_128, heap_per_conn_at_128);
    } else {
      std::printf("[S2-07 分辨率] 本轮 N=128 的 VmRSS/连接（%.0f B）不低于堆实测/连接（%.0f B）："
                  "粒度这一解释在本轮不成立 → 那 0.50 KiB/连接**按原样记为仍无法归因**，"
                  "量级 = 128 条上一共 64 KiB，外推 100k 连接对象 = 约 49 MiB（ROADMAP 重算过）\n",
                  rss_per_conn_at_128, heap_per_conn_at_128);
    }
  }

  // 显式释放，别让这 1024 条连接活到下一条用例里去影响它的基线。
  large.clear();
  small.clear();
}

// S3-01（ADR-18 后果段）：路由给上游连接加的那个"目标桶"标签**不许多付 8 字节**。
// 这条断言的分量全在 B-011 那张构成表上：它把 `Connection`×2 = 1,056 B 逐条钉进了
// 每对 17,861 B，与生产 100k 对的 RSS 商 17,889 B/对 差 0.16%。标签要是把对象抬到 536，
// 100k 对就多 1.6 MiB，那张表连同 #2 的可引用数字要整张重写——所以这里要红的不是
// "对象变大了"这句抱怨，而是那条已经进档的账。
// 用 static_assert 而不是只写 EXPECT：变大在**编译期**就该挡住，不给"这条用例没被跑到"
// 留任何余地（1.10 那条"跳过＝不判，不是通过"的纪律，同一个方向）。
TEST(S3_01_ConnectionLayout, TargetLabelFitsInTheExistingPadding) {
  static_assert(sizeof(Connection) == 528,
                "S3-01 的目标标签必须落在 armed_events_/read_stopped_at_line_ 之后的空位里");
  EXPECT_EQ(sizeof(Connection), 528u);

  PipePair pipe;
  ASSERT_TRUE(pipe.ok());
  ReactorStats stats;
  Connection conn(pipe.take_read_end(), Connection::Role::kUpstream, default_config(), stats);

  // 默认 0：单目标配置下"只有一个桶"，而 S2 时代根本没有这个字段——默认必须是 0 才
  // 不让既有用例（构造路径没写它的那些）看到未初始化值。
  EXPECT_EQ(conn.upstream_target(), 0u);
  conn.set_upstream_target(7);
  EXPECT_EQ(conn.upstream_target(), 7u);
  conn.set_upstream_target(0xffff);  // 哨兵也能存下：它是 uint16_t 的满值空间
  EXPECT_EQ(conn.upstream_target(), 0xffffu);
}

}  // namespace
