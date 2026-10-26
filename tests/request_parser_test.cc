#include "http/request_parser.h"

#include "net/buffer.h"
#include "pipe_fixture.h"

#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace l7gate::http {
namespace {

using l7gate::testing::PipeFixture;

std::string make_request(std::string_view request_line,
                         std::initializer_list<std::string_view> headers,
                         std::string_view body = {}) {
  std::string out(request_line);
  out += "\r\n";
  for (std::string_view h : headers) {
    out += h;
    out += "\r\n";
  }
  out += "\r\n";
  out += body;
  return out;
}

// 小限额配置：超长类用例如果都按默认 8 KiB / 64 KiB 造，测试要写出几十 KB 的字面量，
// 读的人分不清"超限判定"和"字符串拼接"哪个在起作用。把限额调小，用例就能一眼看完。
ParserConfig small_limits_config() {
  ParserConfig cfg;
  cfg.max_header_line_bytes = 64;
  cfg.max_headers_bytes = 256;
  cfg.max_body_bytes = 128;
  return cfg;
}

// 小限额用例配套的小 scratch：容量就是条数限制本身（见 request_parser.h 的
// ParseScratch 注释），所以"条数超限"这条用例必须用 8 槽的 scratch 才测得到。
constexpr std::size_t kSmallScratchCapacity = 8;

bool within_input(const std::uint8_t* data, std::size_t len, std::string_view view) {
  // 畸形输入用例真正的风险不是"返回了哪个错误码"，而是解析器越界读 —— 越界在 -O0
  // 下未必当场 segfault（ASan 会抓到），但"读到了别人内存里的字节还当成字段解释"
  // 是逻辑层面的错，所以除了靠 ASan，我直接把每条返回的 view 是否落在调用方给的那段
  // 字节之内断言出来。
  //
  // 空 view 一律算合法：kNone body 的 raw_body 是默认构造的 string_view（data()==nullptr），
  // 它没有读任何一个字节。第一版我没写这条，fuzz 里 10000 个 kNone 用例全被我自己的
  // 断言误判成越界。
  if (view.empty()) return true;
  const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(data);
  const std::uintptr_t at = reinterpret_cast<std::uintptr_t>(view.data());
  return at >= base && (at - base) + view.size() <= len;
}

// --- 验收标准第 1 条：合法样本逐字段相等 ------------------------------------

TEST(S1_03_Parser, ValidGetRequestParsesFieldByField) {
  const std::string text = make_request("GET /index.html HTTP/1.1",
                                        {"Host: example.com", "User-Agent: curl/8.0", "Accept: */*"});
  ParseScratch scratch{16};
  ParsedRequest req;
  const ParseOutcome got = parse_request(text, scratch, ParserConfig{}, req);

  ASSERT_EQ(got.status, ParseStatus::kOk) << to_string(got.error);
  EXPECT_EQ(req.method, "GET");
  EXPECT_EQ(req.target, "/index.html");
  EXPECT_EQ(req.version, "HTTP/1.1");
  EXPECT_TRUE(req.http_1_1);
  EXPECT_EQ(req.headers.size(), 3u);
  EXPECT_EQ(req.headers[0].name, "Host");
  EXPECT_EQ(req.headers[0].value, "example.com");
  EXPECT_EQ(req.headers[2].name, "Accept");
  EXPECT_EQ(req.headers[2].value, "*/*");
  EXPECT_EQ(req.body_kind, BodyKind::kNone);
  EXPECT_EQ(req.declared_body_bytes, 0u);
  EXPECT_TRUE(req.raw_body.empty());
  // headers_bytes 的口径：请求行 + 每条 header 行 + 结尾空行，全部含 CRLF。
  EXPECT_EQ(req.headers_bytes, text.size());
  EXPECT_EQ(req.consumed, text.size());

  // 大小写不敏感查找是 HTTP 字段名的语义要求，转发时又要原样还回名字。
  const auto ua = req.find_header("user-agent");
  ASSERT_TRUE(ua.has_value());
  EXPECT_EQ(*ua, "curl/8.0");
  EXPECT_EQ(req.headers[1].name, "User-Agent");  // 原始大小写没被改
  EXPECT_FALSE(req.find_header("X-Nope").has_value());
}

TEST(S1_03_Parser, HeaderValueOwsIsTrimmedButInnerSpacesKept) {
  const std::string clean = make_request("GET / HTTP/1.1",
                                         {"Host:   h", "X-Long:  a b", "X-Empty: "});
  ParseScratch scratch{16};
  ParsedRequest req;
  const ParseOutcome got = parse_request(clean, scratch, ParserConfig{}, req);
  ASSERT_EQ(got.status, ParseStatus::kOk) << to_string(got.error);
  EXPECT_EQ(req.headers[0].value, "h");
  EXPECT_EQ(req.headers[1].value, "a b");   // 中间的空格不属于 OWS，不能剥
  EXPECT_EQ(req.headers[2].value, "");
  EXPECT_EQ(req.headers[2].name, "X-Empty");
}

TEST(S1_03_Parser, ContentLengthBodyFields) {
  const std::string body = "hello";
  const std::string text = make_request("POST /api/v1/items HTTP/1.1",
                                         {"Host: h", "Content-Length: 5"}, body);
  ParseScratch scratch{16};
  ParsedRequest req;
  const ParseOutcome got = parse_request(text, scratch, ParserConfig{}, req);

  ASSERT_EQ(got.status, ParseStatus::kOk) << to_string(got.error);
  EXPECT_EQ(req.body_kind, BodyKind::kContentLength);
  EXPECT_EQ(req.declared_body_bytes, 5u);
  EXPECT_EQ(req.body_raw_bytes, 5u);
  EXPECT_EQ(req.raw_body, body);
  EXPECT_EQ(req.consumed, text.size());
  EXPECT_EQ(req.consumed - req.headers_bytes, body.size());
  EXPECT_EQ(req.headers_bytes + req.declared_body_bytes, req.consumed);
}

// chunked 的验收重点是"框架被校验、但不被解码也不被重写"：
// declared 是解码后的总长，consumed 是含框架的原始长度，两个数必须分开。
TEST(S1_03_Parser, ChunkedFramingIsValidatedNotDecoded) {
  const std::string chunks = "5\r\nhello\r\n6;a=b\r\nworld2\r\n0\r\nX-Trailer: v\r\n\r\n";
  const std::string text =
      make_request("POST /u HTTP/1.1", {"Host: h", "Transfer-Encoding: chunked"}, chunks);
  ParseScratch scratch{16};
  ParsedRequest req;
  const ParseOutcome got = parse_request(text, scratch, ParserConfig{}, req);

  ASSERT_EQ(got.status, ParseStatus::kOk) << to_string(got.error);
  EXPECT_EQ(req.body_kind, BodyKind::kChunked);
  EXPECT_EQ(req.declared_body_bytes, 11u);        // 5 + 6，解码后的字节数
  EXPECT_EQ(req.consumed, text.size());           // 原始字节，含 chunk 头尾与 trailer
  EXPECT_EQ(req.body_raw_bytes, chunks.size());   // 框架占的原始空间
  EXPECT_TRUE(req.raw_body.empty());              // 不连续，所以我不给"已解码 body"
  // trailer 被当成框架跳掉，不是被当成下一条请求的请求行 —— 这条决定 consumed 的值。
  EXPECT_EQ(req.consumed - req.headers_bytes, chunks.size());
}

TEST(S1_03_Parser, ChunkedWithoutTrailerAndWithBareLastChunk) {
  const std::string chunks = "0\r\n\r\n";
  const std::string text =
      make_request("POST /u HTTP/1.1", {"Host: h", "Transfer-Encoding: chunked"}, chunks);
  ParseScratch scratch{16};
  ParsedRequest req;
  const ParseOutcome got = parse_request(text, scratch, ParserConfig{}, req);
  ASSERT_EQ(got.status, ParseStatus::kOk) << to_string(got.error);
  EXPECT_EQ(req.declared_body_bytes, 0u);
  EXPECT_EQ(req.consumed, text.size());
}

TEST(S1_03_Parser, DuplicateIdenticalContentLengthIsMerged) {
  // RFC 9112：多个**相同**的 CL 可以合并，不同值才是走私。
  const std::string text = make_request("POST /u HTTP/1.1",
                                        {"Host: h", "Content-Length: 5", "CONTENT-LENGTH: 5"}, "hello");
  ParseScratch scratch{16};
  ParsedRequest req;
  const ParseOutcome got = parse_request(text, scratch, ParserConfig{}, req);
  ASSERT_EQ(got.status, ParseStatus::kOk) << to_string(got.error);
  EXPECT_EQ(req.declared_body_bytes, 5u);
}

TEST(S1_03_Parser, Http10WithoutHostIsAccepted) {
  const std::string text = make_request("GET / HTTP/1.0", {"User-Agent: x"});
  ParseScratch scratch{16};
  ParsedRequest req;
  const ParseOutcome got = parse_request(text, scratch, ParserConfig{}, req);
  ASSERT_EQ(got.status, ParseStatus::kOk) << to_string(got.error);
  EXPECT_FALSE(req.http_1_1);
}

TEST(S1_03_Parser, AsteriskFormTargetIsAccepted) {
  const std::string text = make_request("OPTIONS * HTTP/1.1", {"Host: h"});
  ParseScratch scratch{16};
  ParsedRequest req;
  const ParseOutcome got = parse_request(text, scratch, ParserConfig{}, req);
  ASSERT_EQ(got.status, ParseStatus::kOk) << to_string(got.error);
  EXPECT_EQ(req.target, "*");
}

// 纯函数必须是可重入的：同一段字节喂两次，结果逐字段一致，且第二次不依赖第一次的痕迹。
TEST(S1_03_Parser, RepeatedParseOfSameBytesIsIdentical) {
  const std::string text =
      make_request("GET /a HTTP/1.1", {"Host: h", "X-A: 1"}, "");
  ParseScratch scratch{16};
  ParsedRequest first;
  ParsedRequest second;
  ASSERT_EQ(parse_request(text, scratch, ParserConfig{}, first).status, ParseStatus::kOk);
  ASSERT_EQ(parse_request(text, scratch, ParserConfig{}, second).status, ParseStatus::kOk);
  EXPECT_EQ(first.consumed, second.consumed);
  EXPECT_EQ(first.headers_bytes, second.headers_bytes);
  EXPECT_EQ(first.method, second.method);
  ASSERT_EQ(first.headers.size(), second.headers.size());
  for (std::size_t i = 0; i < first.headers.size(); ++i) {
    EXPECT_EQ(first.headers[i].name, second.headers[i].name);
    EXPECT_EQ(first.headers[i].value, second.headers[i].value);
  }
}

// --- 验收标准第 2 条的另一面：半包永远不是错误 ------------------------------

// 把一条合法请求截断到**每一个**长度：结果只能是 kOk 或 kNeedMoreData。
// 这条比逐条列举半包形态更有价值 —— 它把"截断判成错误"这一整类 bug 一次封掉，
// 而那一类 bug 的后果是网关把正常慢客户端当攻击处理、回 400 断连。
TEST(S1_03_Parser, EveryPrefixOfAValidRequestIsNeedMoreOrOk) {
  const std::vector<std::string> samples = {
      make_request("GET /index.html HTTP/1.1", {"Host: example.com", "Accept: */*"}),
      make_request("POST /api HTTP/1.1", {"Host: h", "Content-Length: 5"}, "hello"),
      make_request("POST /api HTTP/1.1", {"Host: h", "Transfer-Encoding: chunked"},
                   "5\r\nhello\r\n0\r\nX-T: 1\r\n\r\n"),
      make_request("GET / HTTP/1.1", {"Host: h", "X-A: 1", "X-B: 2", "X-C: 3"}),
  };
  ParseScratch scratch{16};
  for (const std::string& sample : samples) {
    for (std::size_t cut = 0; cut < sample.size(); ++cut) {
      ParsedRequest req;
      const ParseOutcome got = parse_request(sample.data(), cut, scratch, ParserConfig{}, req);
      ASSERT_TRUE(got.status == ParseStatus::kOk || got.status == ParseStatus::kNeedMoreData)
          << "截到第 " << cut << " 字节被判成错误: " << to_string(got.error)
          << " 样本首行=" << sample.substr(0, sample.find("\r\n"));
      if (got.status == ParseStatus::kOk) {
        EXPECT_LE(req.consumed, cut);
      }
    }
    ParsedRequest whole;
    ASSERT_EQ(parse_request(sample, scratch, ParserConfig{}, whole).status, ParseStatus::kOk);
  }
}

// 粘包：一个 buffer 里两条完整请求，靠 consumed 衔接，结果与单独解析一致。
TEST(S1_03_Parser, StickyRequestsParseViaConsumed) {
  const std::string a = make_request("GET /first HTTP/1.1", {"Host: h"});
  const std::string b = make_request("POST /second HTTP/1.1", {"Host: h", "Content-Length: 3"}, "xyz");
  const std::string both = a + b;
  ParseScratch scratch{16};

  ParsedRequest first;
  const ParseOutcome got_a = parse_request(both.data(), both.size(), scratch, ParserConfig{}, first);
  ASSERT_EQ(got_a.status, ParseStatus::kOk) << to_string(got_a.error);
  EXPECT_EQ(first.consumed, a.size());
  EXPECT_EQ(first.target, "/first");

  const std::string_view tail = std::string_view(both).substr(a.size());
  ParsedRequest second;
  const ParseOutcome got_b = parse_request(tail, scratch, ParserConfig{}, second);
  ASSERT_EQ(got_b.status, ParseStatus::kOk) << to_string(got_b.error);
  EXPECT_EQ(second.consumed, b.size());
  EXPECT_EQ(second.target, "/second");
  EXPECT_EQ(second.declared_body_bytes, 3u);
  EXPECT_EQ(second.raw_body, "xyz");

  // 与"单独解析"逐字段相等 —— 这是"Buffer 不替解析层丢边界"的解析侧证明。
  ParsedRequest solo_a;
  ASSERT_EQ(parse_request(a, scratch, ParserConfig{}, solo_a).status, ParseStatus::kOk);
  EXPECT_EQ(solo_a.consumed, first.consumed);
  EXPECT_EQ(solo_a.headers_bytes, first.headers_bytes);
}

TEST(S1_03_Parser, CompleteRequestFollowedByPartialRemainder) {
  const std::string full = make_request("GET /a HTTP/1.1", {"Host: h"});
  const std::string partial = "GET /b HTT";
  ParseScratch scratch{16};
  ParsedRequest first;
  ASSERT_EQ(parse_request(full + partial, scratch, ParserConfig{}, first).status, ParseStatus::kOk);
  EXPECT_EQ(first.consumed, full.size());

  const std::string_view rest = std::string_view(full + partial).substr(full.size());
  ParsedRequest second;
  // 注意：rest 是临时串的 view，这里立刻用掉，不留悬垂 —— 与 Buffer 的 view 生命周期
  // 约束同一个道理（见 request_parser.h 的 ParsedRequest 注释）。
  const ParseOutcome got =
      parse_request(std::string_view(partial), scratch, ParserConfig{}, second);
  EXPECT_EQ(got.status, ParseStatus::kNeedMoreData);
  EXPECT_NE(rest.data(), nullptr);
}

// 与 S1-02 的接缝：数据经真管道 1 字节 1 字节 dribble 进来，每轮 read 后解析一次。
// 解析器不碰 fd —— 这条用例证明它只需要 Buffer 给的那段连续字节。
TEST(S1_03_Parser, DribblesThroughBufferAndParseEachRound) {
  PipeFixture pipe;
  if (!pipe.ok()) GTEST_SKIP() << "pipe2 不可用";

  const std::string request =
      make_request("POST /upload HTTP/1.1", {"Host: h", "Content-Length: 10"}, "0123456789");
  net::Buffer buf;
  ParseScratch scratch{16};

  std::size_t rounds = 0;
  std::size_t need_more_rounds = 0;
  ParsedRequest req;
  bool done = false;
  for (char ch : request) {
    // 一个字节一个字节写：每次 ::read 都会拿到一段"不完整"的字节，
    // 这正是 ET + 半包在真实网络里的样子。
    ASSERT_EQ(::write(pipe.wfd(), &ch, 1), 1);
    const auto read = buf.read_until_blocked(pipe.rfd());
    ASSERT_TRUE(read.would_block) << "第 " << rounds << " 轮没读到 EAGAIN";
    ++rounds;
    ParsedRequest candidate;
    const ParseOutcome got =
        parse_request(buf.readable_data(), buf.readable_bytes(), scratch, ParserConfig{}, candidate);
    if (got.status == ParseStatus::kNeedMoreData) {
      ++need_more_rounds;
      continue;
    }
    ASSERT_EQ(got.status, ParseStatus::kOk) << "dribble 过程中被判成错误: " << to_string(got.error);
    req = candidate;
    done = true;
    break;
  }
  ASSERT_TRUE(done) << "喂完全部字节仍未解析成功";
  EXPECT_GT(need_more_rounds, 0u) << "每轮都判完整，说明半包识别根本没生效";
  EXPECT_EQ(req.consumed, request.size());
  EXPECT_EQ(req.raw_body, "0123456789");
  EXPECT_EQ(buf.readable_bytes(), request.size()) << "Buffer 里的字节不能被解析器消耗";
}

// --- 验收标准第 3 条：畸形输入返回错误码，不崩溃、不死循环 -------------------

struct MalformedCase {
  const char* name;
  std::string payload;
  ParseError expected;
  bool use_small_limits = false;
};

std::vector<MalformedCase> malformed_cases() {
  std::vector<MalformedCase> v;
  auto add = [&](const char* name, std::string payload, ParseError expected,
                 bool small = false) {
    v.push_back({name, std::move(payload), expected, small});
  };

  // 1-2 请求行
  add("请求行只有两段", make_request("GET /", {"Host: h"}), ParseError::kRequestLineMalformed);
  add("请求行为空（起始即 CRLF）", "\r\nHost: h\r\n\r\n", ParseError::kRequestLineMalformed);
  add("请求行只有一段", make_request("GET", {"Host: h"}), ParseError::kRequestLineMalformed);
  // 3 version
  add("版本是 HTTP/2.0", make_request("GET / HTTP/2.0", {"Host: h"}),
      ParseError::kVersionUnsupported);
  add("版本残缺", make_request("GET / HTTP/1.", {"Host: h"}), ParseError::kVersionUnsupported);
  add("版本根本不是 HTTP", make_request("GET / ftp", {"Host: h"}), ParseError::kVersionUnsupported);
  // 4 method
  add("method 含非法字符", make_request("GE(T / HTTP/1.1", {"Host: h"}), ParseError::kMethodInvalid);
  // 5 target
  add("target 是 authority-form", make_request("GET ex.com:80 HTTP/1.1", {"Host: h"}),
      ParseError::kTargetInvalid);
  add("target 含控制字符", std::string("GET /\x01x HTTP/1.1\r\nHost: h\r\n\r\n"),
      ParseError::kTargetInvalid);
  // 6 行尾
  add("裸 LF 行尾", "GET / HTTP/1.1\nHost: h\n\n", ParseError::kLineEndingNotCRLF);
  add("孤立 CR 后接非 LF", "GET / HTTP/1.1\rHost: h\r\n\r\n", ParseError::kLineEndingNotCRLF);
  // 7-8 header 名
  add("header 行缺冒号", make_request("GET / HTTP/1.1", {"Host h"}),
      ParseError::kHeaderLineMalformed);
  add("header 冒号前为空", make_request("GET / HTTP/1.1", {": h"}),
      ParseError::kHeaderLineMalformed);
  add("header 名含空格", make_request("GET / HTTP/1.1", {"Ho st: h", "Host: h"}),
      ParseError::kHeaderNameInvalid);
  // 9 header 值
  add("header 值含 NUL",
      std::string("GET / HTTP/1.1\r\nHost: h\x01x\r\n\r\n"), ParseError::kHeaderValueInvalid);
  // 10 obs-fold
  add("obs-fold 折行", make_request("GET / HTTP/1.1", {"Host: h", "\tfolded"}),
      ParseError::kObsFoldRejected);
  // 11-13 超限
  add("单行超长", make_request("GET / HTTP/1.1", {"Host: h", std::string(70, 'A') + ": v"}),
      ParseError::kHeaderTooLarge, true);
  add("头部区总量超限",
      make_request("GET / HTTP/1.1",
                   {"Host: h", std::string(50, 'a') + ":0", std::string(50, 'b') + ":1",
                    std::string(50, 'c') + ":2", std::string(50, 'd') + ":3",
                    std::string(50, 'e') + ":4"}),
      ParseError::kHeadersTooLarge, true);
  add("header 条数超 scratch 容量",
      make_request("GET / HTTP/1.1", {"Host: h", "A: 1", "B: 2", "C: 3", "D: 4", "E: 5",
                                      "F: 6", "G: 7", "H: 8", "I: 9"}),
      ParseError::kTooManyHeaders, true);
  // 14-15 Content-Length
  add("CL 非数字", make_request("GET / HTTP/1.1", {"Host: h", "Content-Length: abc"}),
      ParseError::kContentLengthInvalid);
  add("CL 带负号", make_request("GET / HTTP/1.1", {"Host: h", "Content-Length: -1"}),
      ParseError::kContentLengthInvalid);
  add("CL 为空", make_request("GET / HTTP/1.1", {"Host: h", "Content-Length:"}),
      ParseError::kContentLengthInvalid);
  add("CL 多个且值不一致",
      make_request("POST / HTTP/1.1", {"Host: h", "Content-Length: 5", "Content-Length: 6"}, "hello"),
      ParseError::kContentLengthDuplicated);
  // 16 CL + TE 并存
  add("CL 与 TE 并存",
      make_request("POST / HTTP/1.1", {"Host: h", "Content-Length: 5", "Transfer-Encoding: chunked"},
                   "5\r\nhello\r\n0\r\n\r\n"),
      ParseError::kContentLengthWithTE);
  // 17 TE
  add("TE 是 gzip",
      make_request("POST / HTTP/1.1", {"Host: h", "Transfer-Encoding: gzip"}, "x"),
      ParseError::kTransferEncodingInvalid);
  add("TE 多值",
      make_request("POST / HTTP/1.1", {"Host: h", "Transfer-Encoding: chunked, gzip"},
                   "5\r\nhello\r\n0\r\n\r\n"),
      ParseError::kTransferEncodingInvalid);
  add("TE 重复头",
      make_request("POST / HTTP/1.1",
                   {"Host: h", "Transfer-Encoding: chunked", "Transfer-Encoding: chunked"},
                   "0\r\n\r\n"),
      ParseError::kTransferEncodingInvalid);
  // 18-19 Host
  add("多个 Host", make_request("GET / HTTP/1.1", {"Host: a", "Host: b"}),
      ParseError::kHostDuplicated);
  add("HTTP/1.1 缺 Host", make_request("GET / HTTP/1.1", {"Accept: */*"}),
      ParseError::kHostMissing);
  // 20-21 chunk 框架
  add("chunk-size 非十六进制",
      make_request("POST / HTTP/1.1", {"Host: h", "Transfer-Encoding: chunked"}, "ZZ\r\nxx\r\n0\r\n\r\n"),
      ParseError::kChunkSizeInvalid);
  add("chunk-size 行为空",
      make_request("POST / HTTP/1.1", {"Host: h", "Transfer-Encoding: chunked"}, "\r\n0\r\n\r\n"),
      ParseError::kChunkSizeInvalid);
  add("chunk-size 行裸 LF",
      make_request("POST / HTTP/1.1", {"Host: h", "Transfer-Encoding: chunked"}, "3\nabc\r\n0\r\n\r\n"),
      ParseError::kChunkTerminatorMissing);
  add("chunk 数据后缺 CRLF",
      make_request("POST / HTTP/1.1", {"Host: h", "Transfer-Encoding: chunked"},
                   "3\r\nabcX\r\n0\r\n\r\n"),
      ParseError::kChunkTerminatorMissing);
  // 22 体量
  add("CL 声明超过 body 上限",
      make_request("POST / HTTP/1.1", {"Host: h", "Content-Length: 1000"},
                   std::string(1000, 'z')),
      ParseError::kBodyTooLarge, true);
  add("chunk 之和超过 body 上限",
      make_request("POST / HTTP/1.1", {"Host: h", "Transfer-Encoding: chunked"},
                   "ff\r\n" + std::string(255, 'z') + "\r\n0\r\n\r\n"),
      ParseError::kBodyTooLarge, true);
  // 23 合法但本项目不做
  add("CONNECT 隧道", make_request("CONNECT example.com:443 HTTP/1.1", {"Host: example.com"}),
      ParseError::kNotImplemented);
  add("absolute-form 目标",
      make_request("GET http://example.com/a HTTP/1.1", {"Host: example.com"}),
      ParseError::kNotImplemented);
  return v;
}

TEST(S1_03_Parser, MalformedInputsReturnDefinedErrorCodes) {
  int index = 0;
  for (const MalformedCase& c : malformed_cases()) {
    ++index;
    const ParserConfig cfg = c.use_small_limits ? small_limits_config() : ParserConfig{};
    // 条数限制由 scratch 容量表达，所以每条用例按自己的限额配容量。
    ParseScratch scratch{c.use_small_limits ? kSmallScratchCapacity : kDefaultMaxHeaderCount};
    ParsedRequest req;
    const ParseOutcome got = parse_request(c.payload, scratch, cfg, req);
    EXPECT_EQ(got.status, ParseStatus::kError)
        << "第 " << index << " 类「" << c.name << "」没有被判成错误（status="
        << static_cast<int>(got.status) << "）";
    EXPECT_EQ(got.error, c.expected) << "第 " << index << " 类「" << c.name
                                    << "」报的是 " << to_string(got.error) << "，期望 "
                                    << to_string(c.expected);
  }
  std::printf("[S1-03] 畸形输入用例=%d 类（契约要求 ≥12）\n", index);
  EXPECT_GE(index, 12);
}

// 同一批畸形输入跑 20 遍结果必须一致：这条断言防的是"某些路径靠 scratch 里的
// 残留状态才恰好判对"，那种实现换一次复用顺序就从 400 变成 500 或者更糟。
TEST(S1_03_Parser, MalformedVerdictsAreStableAcrossReuse) {
  for (const MalformedCase& c : malformed_cases()) {
    const ParserConfig cfg = c.use_small_limits ? small_limits_config() : ParserConfig{};
    ParseScratch scratch{c.use_small_limits ? kSmallScratchCapacity : kDefaultMaxHeaderCount};
    ParsedRequest first;
    const ParseOutcome a = parse_request(c.payload, scratch, cfg, first);
    for (int i = 0; i < 20; ++i) {
      ParsedRequest again;
      const ParseOutcome b = parse_request(c.payload, scratch, cfg, again);
      ASSERT_EQ(a.status, b.status) << c.name;
      ASSERT_EQ(a.error, b.error) << c.name;
    }
  }
}

TEST(S1_03_Parser, ErrorCodesHaveUniqueNames) {
  // 错误码是 S1-06 映射 4xx 的依据，重名或漏名会让"回什么状态码"变成猜。
  const ParseError all[] = {
      ParseError::kRequestLineMalformed, ParseError::kMethodInvalid, ParseError::kTargetInvalid,
      ParseError::kVersionUnsupported, ParseError::kLineEndingNotCRLF,
      ParseError::kHeaderLineMalformed, ParseError::kHeaderNameInvalid,
      ParseError::kHeaderValueInvalid, ParseError::kObsFoldRejected, ParseError::kHeaderTooLarge,
      ParseError::kHeadersTooLarge, ParseError::kTooManyHeaders, ParseError::kContentLengthInvalid,
      ParseError::kContentLengthDuplicated, ParseError::kContentLengthWithTE,
      ParseError::kTransferEncodingInvalid, ParseError::kHostDuplicated, ParseError::kHostMissing,
      ParseError::kChunkSizeInvalid, ParseError::kChunkTerminatorMissing, ParseError::kBodyTooLarge,
      ParseError::kNotImplemented,
  };
  std::set<std::string_view> names;
  for (ParseError e : all) {
    std::string_view n = to_string(e);
    EXPECT_NE(n, "unknown_error_code") << "枚举值 " << static_cast<int>(e) << " 没进 to_string";
    EXPECT_TRUE(names.insert(n).second) << "错误码名字重复: " << n;
  }
  EXPECT_EQ(names.size(), 22u);
  EXPECT_STREQ(to_string(ParseError::kNone), "none");
}

// --- 随机字节：不崩溃、不死循环、返回的 view 不越界 --------------------------

TEST(S1_03_Parser, RandomBytesNeverEscapeTheirBounds) {
  std::mt19937 rng(20261026u);
  // 字母表里刻意塞满 CRLF、':'、SP、'/'、十六进制字符与控制符；均匀随机字节的话
  // 绝大多数样本会在第一个检查点就返回，扫不到框架解析那几段。
  // 用 char[] + sizeof 定长构造 string_view：同一个字面量里含 '\0' 时，
  // std::string(raw) 走 strlen 会在第一个 NUL 处截断，我想要的 NUL 反而进不来。
  const char raw_alphabet[] = "abzZ09;:,/*? \t\r\n\x01\x7f\0ABCDEFGH"
                              "host:content-length\r\nchunked HTTP/1.1 GET POST";
  const std::string_view alphabet(raw_alphabet, sizeof(raw_alphabet) - 1);
  ASSERT_NE(alphabet.find('\0'), std::string_view::npos) << "字母表里的 NUL 被截断了";
  ParseScratch scratch{64};
  const ParserConfig cfg = small_limits_config();

  std::size_t ok_count = 0;
  std::size_t need_more_count = 0;
  std::size_t error_count = 0;
  for (int iter = 0; iter < 40000; ++iter) {
    std::string payload;
    const std::size_t n = 1 + rng() % 96;
    for (std::size_t i = 0; i < n; ++i) payload += alphabet[rng() % alphabet.size()];
    if (iter % 4 == 0) payload = make_request("GET /a HTTP/1.1", {"Host: h"}) + payload;

    ParsedRequest req;
    const ParseOutcome got = parse_request(payload, scratch, cfg, req);
    switch (got.status) {
      case ParseStatus::kOk: {
        ++ok_count;
        const std::uint8_t* data = reinterpret_cast<const std::uint8_t*>(payload.data());
        EXPECT_LE(req.consumed, payload.size());
        EXPECT_LE(req.headers_bytes, req.consumed);
        EXPECT_TRUE(within_input(data, payload.size(), req.method));
        EXPECT_TRUE(within_input(data, payload.size(), req.target));
        EXPECT_TRUE(within_input(data, payload.size(), req.version));
        EXPECT_TRUE(within_input(data, payload.size(), req.raw_body));
        for (const HeaderEntry& h : req.headers) {
          EXPECT_TRUE(within_input(data, payload.size(), h.name));
          EXPECT_TRUE(within_input(data, payload.size(), h.value));
        }
        break;
      }
      case ParseStatus::kNeedMoreData: ++need_more_count; break;
      case ParseStatus::kError:
        ++error_count;
        EXPECT_NE(got.error, ParseError::kNone);
        EXPECT_NE(to_string(got.error), "unknown_error_code")
            << "payload=" << payload.size() << " 字节";
        break;
    }
  }
  std::printf("[S1-03] 随机字节 40000 例：ok=%zu need_more=%zu error=%zu\n", ok_count,
              need_more_count, error_count);
  // 三类都必须真的被覆盖到，否则这个 fuzz 只是看着热闹。
  EXPECT_GT(ok_count, 0u);
  EXPECT_GT(need_more_count, 0u);
  EXPECT_GT(error_count, 0u);
}

// 空指针与 0 长度不能是错误，也不能被当成语义完整的请求。
TEST(S1_03_Parser, EmptyInputIsNeedMoreData) {
  ParseScratch scratch{16};
  ParsedRequest req;
  const ParseOutcome got = parse_request(static_cast<const std::uint8_t*>(nullptr), 0, scratch,
                                        ParserConfig{}, req);
  EXPECT_EQ(got.status, ParseStatus::kNeedMoreData);
  EXPECT_EQ(got.error, ParseError::kNone);
  // 半包/空输入时 out 被重置成默认构造，字段不能带任何上一次调用的残留。
  EXPECT_TRUE(req.method.empty());
  EXPECT_TRUE(req.headers.empty());
  EXPECT_EQ(req.consumed, 0u);
}

// --- 口径与代价：三个数字，都要打出来 ---------------------------------------

// scratch 是每连接一次分配的结构，它的尺寸直接进 SLO #2 的每连接内存口径。
TEST(S1_03_Parser, ParseScratchMemoryFootprintIsBounded) {
  const std::size_t entries = sizeof(HeaderEntry);
  const std::size_t scratch_bytes = kDefaultMaxHeaderCount * entries;
  std::printf("[S1-03] sizeof(HeaderEntry)=%zu max_header_count=%zu scratch≈%zu 字节/连接 "
              "(Buffer initial_capacity=4096)\n",
              entries, kDefaultMaxHeaderCount, scratch_bytes);
  EXPECT_EQ(entries, 2u * sizeof(std::string_view));
  // 上限：scratch 不得超过 SLO #2 预算（20 KiB）的四分之一，否则光解析器就吃掉了预算。
  EXPECT_LE(scratch_bytes, 5u << 10);

  ParseScratch scratch;  // 默认容量
  EXPECT_EQ(scratch.max_header_count(), kDefaultMaxHeaderCount);

  // 条数限制的**唯一真相来源**是 scratch 容量：同一段 10 条 header 的字节，
  // 8 槽必须拒、64 槽必须过。这条就是那个"config 里也写了一份 max_header_count、
  // 结果两份各说各话、限制静默失效"bug 的回归测试。
  const std::string ten_headers =
      make_request("GET / HTTP/1.1", {"Host: h", "A: 1", "B: 2", "C: 3", "D: 4",
                                      "E: 5", "F: 6", "G: 7", "H: 8", "I: 9"});
  ParsedRequest req;
  {
    ParseScratch tight{kSmallScratchCapacity};
    const ParseOutcome got = parse_request(ten_headers, tight, ParserConfig{}, req);
    ASSERT_EQ(got.status, ParseStatus::kError);
    EXPECT_EQ(got.error, ParseError::kTooManyHeaders);
  }
  {
    ParseScratch roomy{};
    const ParseOutcome got = parse_request(ten_headers, roomy, ParserConfig{}, req);
    ASSERT_EQ(got.status, ParseStatus::kOk) << to_string(got.error);
    EXPECT_EQ(req.headers.size(), 10u);
  }
}

// 解析吞吐。归因用，不是 SLO —— 数字只有和同一 preset 的构建类型一起记才有意义。
TEST(S1_03_Parser, ParseThroughputIsMeasured) {
  const std::string request =
      make_request("GET /index.html HTTP/1.1",
                   {"Host: example.com", "User-Agent: curl/8.0.1", "Accept: */*",
                    "Content-Length: 5"},
                   "hello");
  ParseScratch scratch{64};
  ParsedRequest req;

  // 先热一遍，也确认每条都被判成 kOk（否则吞吐数字测的是错误路径）。
  for (int i = 0; i < 1000; ++i) {
    ASSERT_EQ(parse_request(request, scratch, ParserConfig{}, req).status, ParseStatus::kOk);
  }

  constexpr int kIters = 200000;
  const auto begin = std::chrono::steady_clock::now();
  std::size_t sink = 0;
  for (int i = 0; i < kIters; ++i) {
    const ParseOutcome got = parse_request(request, scratch, ParserConfig{}, req);
    sink += (got.status == ParseStatus::kOk) ? req.consumed : 0;
  }
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now() - begin)
                      .count();
  ASSERT_GT(sink, 0u);
  const double secs = static_cast<double>(ns) / 1e9;
  const double bytes = static_cast<double>(request.size()) * static_cast<double>(kIters);
  std::printf("[S1-03] 解析吞吐：request=%zu 字节 x %d 次 = %.1f MB/s，%.0f req/s（单线程、无 syscall）\n",
              request.size(), kIters, bytes / secs / 1e6, static_cast<double>(kIters) / secs);
}

// 纯函数的代价，测成数字而不是留一句"可能有性能问题"：
// 头部被 1 字节 1 片 dribble 时，每补一个字节就从头重扫，总扫描量是 O(N²)。
TEST(S1_03_Parser, WorstCaseDribbleScanCostIsMeasured) {
  std::string request = "GET / HTTP/1.1\r\nHost: example.com\r\n";
  for (int i = 0; i < 60; ++i) {
    request += "X-Padding-" + std::to_string(i) + ": " + std::string(48, 'v') + "\r\n";
  }
  request += "\r\n";
  ASSERT_LT(request.size(), ParserConfig{}.max_headers_bytes);

  ParseScratch scratch{64};
  ParsedRequest req;

  // 单次完整解析的基线。
  const auto single_begin = std::chrono::steady_clock::now();
  for (int i = 0; i < 1000; ++i) {
    ASSERT_EQ(parse_request(request, scratch, ParserConfig{}, req).status, ParseStatus::kOk);
  }
  const auto single_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - single_begin)
                             .count();

  // 逐字节 dribble：每个前缀一次调用，且都必须返回 kNeedMoreData（只有最后一次例外）。
  std::size_t scanned_bytes = 0;
  const auto drib_begin = std::chrono::steady_clock::now();
  for (std::size_t cut = 1; cut < request.size(); ++cut) {
    ParsedRequest partial;
    const ParseOutcome got = parse_request(request.data(), cut, scratch, ParserConfig{}, partial);
    ASSERT_EQ(got.status, ParseStatus::kNeedMoreData)
        << "第 " << cut << " 字节处不该完整，实际 " << to_string(got.error);
    scanned_bytes += cut;
  }
  const auto drib_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - drib_begin)
                           .count();

  const double single_per_parse = static_cast<double>(single_ns) / 1000.0;
  const double dribble_total = static_cast<double>(drib_ns);
  std::printf("[S1-03] dribble 代价：请求头 %zu 字节，逐字节喂 %zu 次、重扫 %llu 字节，"
              "耗时 %.0f ns；单次完整解析 %.0f ns/次（比值 %.1fx）。"
              "缓解（连接对象记已扫描游标）归 S1-04\n",
              request.size(), request.size() - 1,
              static_cast<unsigned long long>(scanned_bytes), dribble_total, single_per_parse,
              dribble_total / (single_per_parse > 0 ? single_per_parse : 1.0));
  // 不做性能断言（那会把这条用例变成随机失败的源），只要求数字被打出来。
}

}  // namespace
}  // namespace l7gate::http
