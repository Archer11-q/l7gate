// S5-04 的握手与回帧原语（tests/ws_frame.h）+ 一真连接端到端。
//
// 这一族的基准值不是我自己的实现算出来的：SHA-1/base64/accept 三组向量都由
// 独立工具（python3 的 hashlib/base64）现算后抄进来。用一个实现去钉另一个实现
// 只能证明两边写错了同一个地方。
//
// 存在理由与"为什么帧的终结在测试夹具这一侧而不是网关里"记在
// docs/adr/ADR-26-ws-echo-upstream-and-headless-dom.md。

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>

#include <gtest/gtest.h>

#include "test_upstream.h"
#include "ws_frame.h"

namespace {

using l7gate::testing::websocket_accept;
using l7gate::testing::WsTake;
using l7gate::testing::take_client_frame;
using l7gate::testing::TestUpstream;

std::string hex_digest(std::string_view msg) {
  std::uint8_t d[20];
  l7gate::testing::sha1(msg, d);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  for (const std::uint8_t b : d) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0xF]);
  }
  return out;
}

std::string b64(std::string_view in) {
  return l7gate::testing::base64_encode(reinterpret_cast<const std::uint8_t*>(in.data()),
                                        in.size());
}

// --- 握手值（python3 hashlib 现算，不是这里算的） -------------------------------

TEST(S5_04_WsHandshake, Sha1MatchesIndependentVectors) {
  EXPECT_EQ(hex_digest(""), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  EXPECT_EQ(hex_digest("abc"), "a9993e364706816aba3e25717850c26c9cd0d89d");
  // 跨过 64 字节分组边界的两档：补齐逻辑（长度写在哪一格、要不要多一个块）错了
  // 只在长输入上暴露。56 那格尤其毒——`0x80` 之后正好放不下长度字段，必须多开一块。
  EXPECT_EQ(hex_digest(std::string(56, 'a')), "c2db330f6083854c99d4b5bfb6e8f29f201be699");
  EXPECT_EQ(hex_digest(std::string(64, 'a')), "0098ba824b5c16427bd7a1122a5a442a25ec644d");
}

TEST(S5_04_WsHandshake, Base64MatchesIndependentVectors) {
  EXPECT_EQ(b64("abc"), "YWJj");
  EXPECT_EQ(b64("a"), "YQ==");
  EXPECT_EQ(b64("fo"), "Zm8=");
  EXPECT_EQ(b64("foo"), "Zm9v");
  EXPECT_EQ(b64("foob"), "Zm9vYg==");
}

TEST(S5_04_WsHandshake, AcceptMatchesIndependentVectors) {
  // 第一条是 RFC 6455 §1.3 的样例；后三条是 python3 现算的随机键。
  EXPECT_EQ(websocket_accept("dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
  EXPECT_EQ(websocket_accept("x3JJHMbDL1EzLkh9GBhXDw=="), "HSmrc0sMlYUkAGmm5OPpG2HaGWk=");
  EXPECT_EQ(websocket_accept("A9sWC12tTLG5PgqoN7bFgQ=="), "oHIhMg1XfxQJ7xRZCGfZUne6S4I=");
  EXPECT_EQ(websocket_accept("abc"), "zwKd5BnoWxQ6GxTxxSmBSHGHX9M=");
}

TEST(S5_04_WsHandshake, SurroundingWhitespaceIsTrimmedNotHashed) {
  // HTTP 取值前的 OWS 如果进了摘要就是另一个值，浏览器照样拒——这一格错在真实请求里必现。
  EXPECT_EQ(websocket_accept("  dGhlIHNhbXBsZSBub25jZQ== "),
            "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

// --- 帧（客户端掩码帧 → 服务端未掩码帧） ---------------------------------------

std::string make_client_frame(std::uint8_t opcode, std::string_view payload, bool fin = true,
                              const char* mask = "\x01\x02\x03\x04") {
  std::string out;
  out.push_back(static_cast<char>((fin ? 0x80 : 0x00) | opcode));
  const std::size_t n = payload.size();
  if (n < 126) {
    out.push_back(static_cast<char>(0x80 | n));
  } else if (n <= 0xFFFF) {
    out.push_back(static_cast<char>(0x80 | 126));
    out.push_back(static_cast<char>((n >> 8) & 0xFF));
    out.push_back(static_cast<char>(n & 0xFF));
  } else {
    out.push_back(static_cast<char>(0x80 | 127));
    for (int i = 7; i >= 0; --i) out.push_back(static_cast<char>((n >> (i * 8)) & 0xFF));
  }
  out.append(mask, 4);
  for (std::size_t i = 0; i < payload.size(); ++i) {
    out.push_back(static_cast<char>(static_cast<std::uint8_t>(payload[i]) ^
                                    static_cast<std::uint8_t>(mask[i % 4])));
  }
  return out;
}

std::string take(const std::string& in, std::size_t* consumed, std::string* out) {
  const WsTake take_result = take_client_frame(in, consumed, out);
  return take_result == WsTake::kNeedMore     ? "need-more"
         : take_result == WsTake::kMalformed  ? "malformed"
         : take_result == WsTake::kClosed     ? "closed"
                                              : "complete";
}

TEST(S5_04_WsFrame, TextEchoUnmasksAndReframesWithoutTouchingPayload) {
  const std::string in = make_client_frame(0x1, "hello");
  std::size_t consumed = 0;
  std::string out;
  EXPECT_EQ(take(in, &consumed, &out), "complete");
  ASSERT_EQ(out.size(), 7u);
  EXPECT_EQ(static_cast<std::uint8_t>(out[0]), 0x81u);  // FIN + text
  EXPECT_EQ(static_cast<std::uint8_t>(out[1]), 0x05u);  // 长度 5，且**掩码位为 0**
  EXPECT_EQ(out.substr(2), "hello");                     // 载荷逐字节原样
  EXPECT_EQ(consumed, 11u);                              // 2 + 4 + 5
}

TEST(S5_04_WsFrame, LengthBoundaryUsesTheTwoAndEightByteForms) {
  const std::string medium(200, 'x');
  const std::string large(70000, 'y');
  std::size_t consumed = 0;
  std::string out;

  EXPECT_EQ(take(make_client_frame(0x2, medium), &consumed, &out), "complete");
  ASSERT_EQ(static_cast<std::uint8_t>(out[1]), 126u);
  EXPECT_EQ(out.size(), 4u + medium.size());

  EXPECT_EQ(take(make_client_frame(0x1, large), &consumed, &out), "complete");
  ASSERT_EQ(static_cast<std::uint8_t>(out[1]), 127u);
  EXPECT_EQ(out.size(), 10u + large.size());
  EXPECT_EQ(out.substr(10), large);
}

TEST(S5_04_WsFrame, HalfFrameWaitsInsteadOfBeingConsumed) {
  // 这一格是仪器自己会犯的错：把半条帧"回显掉"等于替浏览器拼出一个畸形帧。
  // 断的是两条不变式：**一个字节都不许认领**（consumed==0，pending 原样留着等下一次事件）、
  // **不产出任何帧**（out 为空——`take_client_frame` 的契约是每次进来先清空 out，
  // 第一版用例在这里断的是"out 不被改动"，那不是它承诺过的性质，红的是我的断言不是实现）。
  const std::string whole = make_client_frame(0x1, "abcd");
  for (std::size_t cut = 1; cut < whole.size(); ++cut) {
    std::size_t consumed = 999;
    std::string out("untouched");
    EXPECT_EQ(take(whole.substr(0, cut), &consumed, &out), "need-more") << "cut=" << cut;
    EXPECT_EQ(consumed, 0u) << "cut=" << cut;
    EXPECT_TRUE(out.empty()) << "cut=" << cut;
  }
}

TEST(S5_04_WsFrame, UnmaskedClientFrameIsMalformedNotEchoed) {
  std::string in = make_client_frame(0x1, "hi");
  in[1] = static_cast<char>(in[1] & 0x7F);  // 抹掉掩码位
  std::size_t consumed = 0;
  std::string out;
  EXPECT_EQ(take(in, &consumed, &out), "malformed");
}

TEST(S5_04_WsFrame, FragmentedFrameIsRefusedNotStitched) {
  // 夹具不实现分片拼装：接了就等于把它做成一个协议栈（ADR-26 第三节那条边界的另一面）。
  std::size_t consumed = 0;
  std::string out;
  EXPECT_EQ(take(make_client_frame(0x1, "part", /*fin=*/false), &consumed, &out), "malformed");
}

TEST(S5_04_WsFrame, PingBecomesPongAndPongIsSilentAndCloseEndsIt) {
  std::size_t consumed = 0;
  std::string out;
  EXPECT_EQ(take(make_client_frame(0x9, "hb"), &consumed, &out), "complete");
  ASSERT_EQ(static_cast<std::uint8_t>(out[0]), 0x8Au);  // pong
  EXPECT_EQ(out.substr(2), "hb");

  EXPECT_EQ(take(make_client_frame(0xA, "hb"), &consumed, &out), "complete");
  EXPECT_TRUE(out.empty());  // pong 不回话，但那条帧要被消费掉

  EXPECT_EQ(take(make_client_frame(0x8, ""), &consumed, &out), "closed");
  ASSERT_EQ(static_cast<std::uint8_t>(out[0]), 0x88u);  // 回一条 close
}

// --- 真连接端到端：这台夹具开着 --ws-echo 之后确实能被浏览器那类对端对话 ----------

// 与 reactor_test 的夹具同一条姿势：SO_RCVTIMEO 让"对端不该沉默"的用例在秒级失败，
// 而不是拖到 ctest 的 TIMEOUT 30。
class UpstreamClient {
 public:
  ~UpstreamClient() {
    if (fd_ >= 0) ::close(fd_);
  }
  bool connect_to(int port) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return false;
    timeval tv{};
    tv.tv_sec = 5;
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
  }
  void write_all(std::string_view data) {
    std::size_t off = 0;
    while (off < data.size()) {
      const ssize_t n = ::send(fd_, data.data() + off, data.size() - off, MSG_NOSIGNAL);
      ASSERT_GT(n, 0) << strerror(errno);
      off += static_cast<std::size_t>(n);
    }
  }
  // 读到 n 字节或超时（返回 false）。
  bool read_exact(std::string* out, std::size_t n) {
    out->clear();
    char buf[4096];
    while (out->size() < n) {
      const ssize_t got = ::recv(fd_, buf, sizeof(buf), 0);
      if (got <= 0) return false;
      out->append(buf, static_cast<std::size_t>(got));
    }
    return true;
  }
  // 101 头按**终止串**读，不按我数的长度读：两档的 accept 长度不同（现算 28 字符、
  // 固定假值 40 字符），写死一个长度就是给其中一档埋一条"读不到就超时"的用例。
  bool read_head(std::string* out) {
    out->clear();
    char buf[4096];
    while (out->find("\r\n\r\n") == std::string::npos) {
      const ssize_t got = ::recv(fd_, buf, sizeof(buf), 0);
      if (got <= 0) return false;
      out->append(buf, static_cast<std::size_t>(got));
    }
    return true;
  }

 private:
  int fd_ = -1;
};

TEST(S5_04_WsUpstreamEcho, HandshakeAcceptIsComputedFromTheKeyAndFramesComeBack) {
  std::string failure;
  TestUpstream upstream;
  upstream.set_ws_echo(true);
  ASSERT_TRUE(upstream.start(&failure, "127.0.0.1", 0)) << failure;

  UpstreamClient client;
  ASSERT_TRUE(client.connect_to(upstream.port()));
  // 键**不能**用 RFC 样例那一串：样例键的正确 accept 恰好是一个固定串，
  // "按 key 现算"与"把样例值抄死"在这一格上读不出区别（W2 退化第一次就是 0 红，
  // 冒烟红了而 ctest 全绿——那是用例的盲点，不是实现的运气）。
  // 这里换一个别的键，它的正确 accept 与样例串不同（值由 python3 现算）。
  const std::string key = "x3JJHMbDL1EzLkh9GBhXDw==";
  const std::string expect_accept = "HSmrc0sMlYUkAGmm5OPpG2HaGWk=";
  client.write_all("GET /ws HTTP/1.1\r\nHost: demo.test\r\nConnection: keep-alive, Upgrade\r\n"
                   "Upgrade: websocket\r\nSec-WebSocket-Key: " +
                   key + "\r\nSec-WebSocket-Version: 13\r\n\r\n");

  std::string head;
  ASSERT_TRUE(client.read_head(&head)) << "101 头没回全";
  EXPECT_EQ(head.compare(0, 12, "HTTP/1.1 101"), 0);
  // 这一格钉的是 ADR-26 第一节那条 A/B：固定假 accept 会被浏览器判 1006。
  EXPECT_NE(head.find("Sec-WebSocket-Accept: " + expect_accept + "\r\n"), std::string::npos)
      << "accept 没有按 key 现算：\n"
      << head;

  const std::string msg = "roundtrip-through-tunnel";
  client.write_all(make_client_frame(0x1, msg));
  std::string frame;
  ASSERT_TRUE(client.read_exact(&frame, 2 + msg.size())) << "回帧没到齐";
  ASSERT_EQ(static_cast<std::uint8_t>(frame[0]), 0x81u);
  ASSERT_EQ(static_cast<std::uint8_t>(frame[1]), static_cast<std::uint8_t>(msg.size()));
  EXPECT_EQ(frame.substr(2), msg);
  EXPECT_EQ(upstream.ws_frames_echoed(), 1u);
  EXPECT_EQ(upstream.upgrades_101_served(), 1u);

  client.write_all(make_client_frame(0x8, ""));
  std::string close_frame;
  ASSERT_TRUE(client.read_exact(&close_frame, 2)) << "close 没被回";
  EXPECT_EQ(static_cast<std::uint8_t>(close_frame[0]), 0x88u);

  upstream.stop_and_join();
}

TEST(S5_04_WsUpstreamEcho, DefaultOffStillEchoesRawBytesAndKeepsTheFakeAccept) {
  // 默认档必须与本任务之前**逐字节相同**：S2-05 阶段四那条
  // "收到的恰好 = 101 头 + 发出的 2048 帧字节"靠的就是裸回显，两档不可互换。
  std::string failure;
  TestUpstream upstream;
  ASSERT_TRUE(upstream.start(&failure, "127.0.0.1", 0)) << failure;

  UpstreamClient client;
  ASSERT_TRUE(client.connect_to(upstream.port()));
  client.write_all("GET /ws HTTP/1.1\r\nHost: smoke.test\r\nConnection: keep-alive, Upgrade\r\n"
                   "Upgrade: websocket\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                   "Sec-WebSocket-Version: 13\r\n\r\n");
  std::string head;
  ASSERT_TRUE(client.read_head(&head)) << "101 头没回全";
  EXPECT_NE(head.find("Sec-WebSocket-Accept: dGhpcyBpcyBub3QgcmVhbCBjcnlwdG9ncmFwaHk=\r\n"),
            std::string::npos)
      << head;

  const std::string raw(300, '\xAA');  // 不是合法帧：掩码位都没有，正是裸回显要吃的那种字节
  client.write_all(raw);
  std::string back;
  ASSERT_TRUE(client.read_exact(&back, raw.size()));
  EXPECT_EQ(back, raw);
  EXPECT_EQ(upstream.ws_frames_echoed(), 0u);

  upstream.stop_and_join();
}

}  // namespace
