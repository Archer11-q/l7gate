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
#include "pipe_fixture.h"

namespace {

using l7gate::http::ParseError;
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

}  // namespace
