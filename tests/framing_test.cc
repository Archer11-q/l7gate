// S1-06：响应框架判定的纯函数面。
//
// 和 S1-03 的解析器测试同一个理由：这些判定不建 socket、不碰 Buffer，
// 「头齐没齐 / 框架是什么 / 边界走到哪」的每一条结论都能在一个文件里钉死。
// reactor 层的用例只留"接线的语义"（谁在什么时刻调用它们），判定本身的
// 对错在这里判——分错了地方，失败信息就又开始同时指向两个嫌疑人了。
//
// 三条我觉得最值钱的：
//   一、EveryRealPrefixOfAHeadIsNeedMoreNotAnythingElse —— 半包在响应侧同样
//       不是错误，判早了的代价是拿正常的慢上游当坏上游（502 错杀）。
//   二、ChunkedAdvanceIsSplitInvariant —— 同一段字节按**每一种**切法喂进
//       游标，结论必须和一次喂完完全一致。这是"增量"二字的全部含义，
//       也是 ET 半包世界里的生存前提。
//   三、MappingTableOwnsEveryErrorCode —— 22 个错误码逐条点名。表里没有
//       "兜底 400"，所以新错误码进不来这张表就会在 -Wswitch 和这条用例
//       两处同时红；只红一处说明另一处的写法错了。
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "http/response_framing.h"

namespace l7gate::http {
namespace {

using namespace std::literals;  // "…"sv：全称前缀断言的切分写法全靠它

using Framing = ResponseFraming;

// --- analyze_response_head -----------------------------------------------

TEST(S1_06_Framing, AnalyzesContentLengthHead) {
  const auto info = analyze_response_head(
      "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"sv);
  EXPECT_EQ(info.framing, Framing::kContentLength);
  EXPECT_EQ(info.status_code, 200u);
  EXPECT_EQ(info.head_bytes, 38u) << "头长必须**止于终止串**，body 一格都不许算进来";
  EXPECT_EQ(info.declared_body_bytes, 5u);
}

TEST(S1_06_Framing, AnalyzesChunkedHead) {
  const auto info = analyze_response_head(
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"sv);
  EXPECT_EQ(info.framing, Framing::kChunked);
  EXPECT_EQ(info.declared_body_bytes, 0u) << "chunked 没有『声明长度』这个数";
}

TEST(S1_06_Framing, ChunkedTokenIsCaseInsensitiveAndOwsTolerated) {
  const auto info = analyze_response_head(
      "HTTP/1.1 200 OK\r\nTransfer-Encoding:  CHUNKED \r\n\r\n"sv);
  EXPECT_EQ(info.framing, Framing::kChunked);
}

TEST(S1_06_Framing, CloseDelimitedWhenNothingDeclared) {
  const auto info = analyze_response_head("HTTP/1.1 200 OK\r\n\r\n"sv);
  EXPECT_EQ(info.framing, Framing::kCloseDelimited);
}

TEST(S1_06_Framing, ZeroLengthContentLengthIsStillContentLength) {
  // CL:0 与"无 CL"的语义差别在于**完结点**：前者头齐即完结，后者要等 EOF。
  // 判成同一种就丢掉了这条区别，用例在这里把它钉住。
  const auto info = analyze_response_head("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"sv);
  EXPECT_EQ(info.framing, Framing::kContentLength);
  EXPECT_EQ(info.declared_body_bytes, 0u);
}

TEST(S1_06_Framing, NoReasonPhraseHeadIsAccepted) {
  const auto info = analyze_response_head("HTTP/1.1 204\r\n\r\n"sv);
  EXPECT_EQ(info.framing, Framing::kNoBody);
  EXPECT_EQ(info.status_code, 204u);
  EXPECT_EQ(info.head_bytes, 16u);
}

TEST(S1_06_Framing, ProvisionalStatusesHaveNoBodyAndKeepTheGateOpen) {
  for (std::string_view code : {"100"sv, "102"sv, "124"sv}) {
    const std::string head = "HTTP/1.1 " + std::string(code) + " Interim\r\nX-A: b\r\n\r\n";
    const auto info = analyze_response_head(head);
    EXPECT_EQ(info.framing, Framing::kProvisional) << head;
  }
  // 101 在"没配对成功"的世界里也是 1xx：没有最终头，502 的资格还在。
  const auto tunnel_attempt = analyze_response_head(
      "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n\r\n"sv);
  EXPECT_EQ(tunnel_attempt.framing, Framing::kProvisional);
}

TEST(S1_06_Framing, NoBodyStatusesBeatHeaderClaims) {
  // RFC 9112 §6.3 的判定优先级：状态码类别**先于**头的声明。204 带 CL 也是无体。
  const auto with_cl = analyze_response_head("HTTP/1.1 204 No Content\r\nContent-Length: 9\r\n\r\n"sv);
  EXPECT_EQ(with_cl.framing, Framing::kNoBody);
  const auto also_304 = analyze_response_head("HTTP/1.1 304 Not Modified\r\nTransfer-Encoding: chunked\r\n\r\n"sv);
  EXPECT_EQ(also_304.framing, Framing::kNoBody);
}

TEST(S1_06_Framing, EveryRealPrefixOfAHeadIsNeedMoreNotAnythingElse) {
  // 全称断言（和 S1-03 的 EveryRealPrefix 同一条哲学）：对一条完整头，
  // 它的**每一个**真前缀都必须且只能得出 kHeadIncomplete ——
  // 不许判成 Invalid（会把慢上游 502 掉），也不许提前判出框架。
  const std::string full = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\n";
  for (std::size_t i = 0; i < full.size(); ++i) {
    const auto info = analyze_response_head(
        reinterpret_cast<const std::uint8_t*>(full.data()), i);
    EXPECT_EQ(info.framing, Framing::kHeadIncomplete) << "前缀长度 " << i << " 被过早裁决了";
  }
}

TEST(S1_06_Framing, InvalidHeadsAreNamedNotGuessed) {
  struct Case {
    std::string_view name;
    std::string_view head;
  };
  const std::vector<Case> cases = {
      {"bad_version", "HTTP/2.0 200 OK\r\n\r\n"},
      {"not_http", "HELLO/1.1 200 OK\r\n\r\n"},
      {"four_digit_status", "HTTP/1.1 1015 Nope\r\n\r\n"},  // S1-05 的 1015 教训在响应侧的同款
      {"non_digit_status", "HTTP/1.1 2xx OK\r\n\r\n"},
      {"status_out_of_range", "HTTP/1.1 600 Weird\r\n\r\n"},
      {"status_below_100", "HTTP/1.1 099 Weird\r\n\r\n"},
      {"cl_with_te", "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n"},
      {"conflicting_cl", "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n"},
      {"te_not_chunked", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n"},
      {"te_multi_token", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked, gzip\r\n\r\n"},
      {"te_duplicated", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n"},
      {"bare_lf", "HTTP/1.1 200 OK\nContent-Length: 1\r\n\r\n"},
      {"obs_fold", "HTTP/1.1 200 OK\r\nX-A: b\r\n c\r\n\r\n"},
      {"header_no_colon", "HTTP/1.1 200 OK\r\nnocolon\r\n\r\n"},
      {"bad_header_name", "HTTP/1.1 200 OK\r\nX( Bad: 1\r\n\r\n"},
      {"nul_in_value", std::string_view("HTTP/1.1 200 OK\r\nX-A: b\0c\r\n\r\n", 29)},
      {"cl_non_numeric", "HTTP/1.1 200 OK\r\nContent-Length: 3x\r\n\r\n"},
      {"cl_negative", "HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n"},
  };
  for (const auto& c : cases) {
    const auto info = analyze_response_head(c.head);
    EXPECT_EQ(info.framing, Framing::kHeadInvalid) << c.name;
    if (c.head.size() >= 16) {
      EXPECT_GT(info.head_bytes, 0u) << c.name << "：Invalid 也要把头长带出来（观测用）";
    }
  }
  // 一致的多个 CL 是合法的（合并），别和"不一致"一起杀了。
  const auto same_dup = analyze_response_head(
      "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nContent-Length: 4\r\n\r\n"sv);
  EXPECT_EQ(same_dup.framing, Framing::kContentLength);
  EXPECT_EQ(same_dup.declared_body_bytes, 4u);
}

TEST(S1_06_Framing, AnalyzesNullAndTinyInputsSafely) {
  EXPECT_EQ(analyze_response_head(static_cast<const std::uint8_t*>(nullptr), 0).framing,
            Framing::kHeadIncomplete);
  const std::uint8_t one = 'H';
  EXPECT_EQ(analyze_response_head(&one, 1).framing, Framing::kHeadIncomplete);
}

// --- chunked_advance -------------------------------------------------------

TEST(S1_06_Framing, ChunkedAdvanceCompletesOnTerminatingBlankLine) {
  ChunkedProgress p;
  const std::string_view stream = "3\r\nabc\r\n5\r\ndefgh\r\n0\r\n\r\n"sv;
  const std::size_t pos = chunked_advance(p, stream);
  EXPECT_EQ(pos, stream.size());
  EXPECT_EQ(p.phase, ChunkedProgress::Phase::kComplete);
}

TEST(S1_06_Framing, ChunkedAdvanceStopsAtTheBoundaryNotBeyondIt) {
  // 边界之后的字节属于下一条响应。游标必须**停在边界**：多走一格就是
  // 把 pipelining 的下一条响应当成 body 喂进框架判定。
  ChunkedProgress p;
  const std::size_t pos = chunked_advance(p, "0\r\n\r\nGET /next HTTP/1.1\r\n"sv);
  EXPECT_EQ(pos, 5u) << "5 字节之后就是下一条请求/响应的起点";
  EXPECT_EQ(p.phase, ChunkedProgress::Phase::kComplete);
  // 完结之后再来字节也不动。
  const std::size_t again = chunked_advance(p, "more"sv);
  EXPECT_EQ(again, 0u);
}

TEST(S1_06_Framing, ChunkedAdvanceIsSplitInvariant) {
  // 同一段字节，按**每一种**单点切分和二点切分喂，结论必须与一次喂完一致。
  // ET 半包是这里的常态；游标如果有任何"必须一次看全"的隐含前提，这条会红。
  const std::string stream = "4\r\nabcd\r\n2\r\nxy\r\n0\r\nx-trailer: 1\r\n\r\nREST";
  ChunkedProgress once;
  const std::size_t full_pos = chunked_advance(once, stream);

  for (std::size_t i = 1; i < stream.size(); ++i) {
    ChunkedProgress split;
    const std::size_t a = chunked_advance(split, std::string_view(stream).substr(0, i));
    const std::size_t b = chunked_advance(split, std::string_view(stream).substr(i));
    EXPECT_EQ(a + b, full_pos) << "切点 " << i;
    EXPECT_EQ(split.phase, once.phase) << "切点 " << i;
  }
  // 逐字节 dribble 到底：最长的压力形态，也是 O(N) 常数内存的形状证明。
  ChunkedProgress dribbled;
  std::size_t advanced = 0;
  for (std::size_t i = 0; i < stream.size(); ++i) {
    advanced += chunked_advance(dribbled, std::string_view(stream).substr(i, 1));
  }
  EXPECT_EQ(advanced, full_pos);
  EXPECT_EQ(dribbled.phase, ChunkedProgress::Phase::kComplete);
  EXPECT_EQ(full_pos, stream.size() - 4u) << "REST 四字节属于边界之后";
}

TEST(S1_06_Framing, ChunkedAdvanceRejectsBrokenFramingAtTheByteThatBreaksIt) {
  struct Case {
    std::string_view name;
    std::string_view feed;
    std::size_t valid_prefix;  // 停在非法字节**之前**的位置
  };
  // 口径：游标"先吃后判"——把一整行框架字节透传完、在行尾结算时才认非法，
  // 所以 valid_prefix 含那个触发结算的字节；真正的**非法**字节一格都不算。
  const std::vector<Case> cases = {
      {"bad_hex", "Z\r\n", 0},
      {"empty_size_line", "\r\n", 2},
      {"data_terminator_not_crlf", "1\r\naXY", 4},   // "1\r\na" 合法，'X' 才是非法格
      {"bare_lf_size_line", "\n", 0},
      {"trailer_bare_lf", "0\r\nx\n", 4},
  };
  for (const auto& c : cases) {
    ChunkedProgress p;
    const std::size_t pos = chunked_advance(p, c.feed);
    EXPECT_EQ(p.phase, ChunkedProgress::Phase::kInvalid) << c.name;
    EXPECT_EQ(pos, c.valid_prefix) << c.name << "：非法字节之前的部分仍算合法透传";
  }
  // 跨段的 CRLF：CR 落在上一段末尾时不能被当成非法，下一段的 LF 才结算。
  // 这条是"先吃后判"最容易被写错的形态——写成了"逐格判"的话，切分位置会
  // 决定同一段字节是否合法，而 ET 下切分位置由网络说了算。
  ChunkedProgress split;
  EXPECT_EQ(chunked_advance(split, "1\r\na"sv), 4u);
  EXPECT_EQ(split.phase, ChunkedProgress::Phase::kDataCrlf);
  EXPECT_EQ(chunked_advance(split, "\r\n"sv), 2u);
  EXPECT_EQ(split.phase, ChunkedProgress::Phase::kSizeLine);
  // 真正的坏行尾（数据后不是 CR）在切分形态下也必须被抓到。
  ChunkedProgress bad_split;
  EXPECT_EQ(chunked_advance(bad_split, "1\r\na"sv), 4u);
  EXPECT_EQ(chunked_advance(bad_split, "X"sv), 0u);
  EXPECT_EQ(bad_split.phase, ChunkedProgress::Phase::kInvalid);
}

TEST(S1_06_Framing, ChunkedAdvanceDoesNotInterpretExtensions) {
  // chunk-ext 不解释、不校验内容，只等行尾——和"不重组框架"是同一条纪律。
  // 11 + 5 + 5 = 21 字节，正好是整段。
  ChunkedProgress p;
  const std::size_t pos = chunked_advance(p, "3;a=b;foo\r\nabc\r\n0\r\n\r\n"sv);
  EXPECT_EQ(pos, 21u);
  EXPECT_EQ(p.phase, ChunkedProgress::Phase::kComplete);
}

TEST(S1_06_Framing, ChunkedAdvanceOverflowIsSettledAtLineEndNotMidNumber) {
  // 溢出在**行尾**结算：非法之前的字节仍已透传（头已经出去了，断链表态）。
  // 如果中途就判死，同一批字节会有一部分过了『合法』一半没有，边界不可复现。
  // 17 位 F：前 16 位恰好是 UINT64_MAX，第 17 位才是溢出的证据。
  ChunkedProgress p;
  const std::size_t pos = chunked_advance(p, "FFFFFFFFFFFFFFFFF\r\n"sv);
  EXPECT_EQ(p.phase, ChunkedProgress::Phase::kInvalid);
  EXPECT_EQ(pos, 19u) << "整行都是『还没到边界』的框架字节，行尾才结算";

  // 对照：16 位 F 不溢出，它是合法的（虽然大得荒谬）——判定只认溢出与否。
  ChunkedProgress max_ok;
  EXPECT_EQ(chunked_advance(max_ok, "FFFFFFFFFFFFFFFF\r\n"sv), 18u);
  EXPECT_EQ(max_ok.phase, ChunkedProgress::Phase::kData);
}

// --- reject 映射表与自产响应 ------------------------------------------------

TEST(S1_06_Framing, MappingTableOwnsEveryErrorCode) {
  // 22 个错误码逐条点名。switch 没有 default 是编译期的第一道 -Wswitch；
  // 这张表是用例侧的第二道：新错误码没进表，这里的期望集就先对不上。
  struct Case {
    ParseError error;
    std::uint16_t status;
  };
  const std::vector<Case> table = {
      {ParseError::kRequestLineMalformed, 400},
      {ParseError::kMethodInvalid, 400},
      {ParseError::kTargetInvalid, 400},
      {ParseError::kVersionUnsupported, 400},
      {ParseError::kLineEndingNotCRLF, 400},
      {ParseError::kHeaderLineMalformed, 400},
      {ParseError::kHeaderNameInvalid, 400},
      {ParseError::kHeaderValueInvalid, 400},
      {ParseError::kObsFoldRejected, 400},
      {ParseError::kHeaderTooLarge, 413},
      {ParseError::kHeadersTooLarge, 413},
      {ParseError::kTooManyHeaders, 413},
      {ParseError::kContentLengthInvalid, 400},
      {ParseError::kContentLengthDuplicated, 400},
      {ParseError::kContentLengthWithTE, 400},
      {ParseError::kTransferEncodingInvalid, 400},
      {ParseError::kHostDuplicated, 400},
      {ParseError::kHostMissing, 400},
      {ParseError::kChunkSizeInvalid, 400},
      {ParseError::kChunkTerminatorMissing, 400},
      {ParseError::kBodyTooLarge, 413},
      {ParseError::kNotImplemented, 501},
  };
  EXPECT_EQ(table.size(), 22u) << "错误码的总数变了：request_parser.h 加/删了值，这里必须同步";
  for (const auto& c : table) {
    EXPECT_EQ(reply_status_for_parse_error(c.error), c.status) << to_string(c.error);
  }
  EXPECT_EQ(reply_status_for_parse_error(ParseError::kNone), 0u)
      << "kNone 不是错误，不给状态码";
}

TEST(S1_06_Framing, GatewayRepliesAreCompleteAndSelfConsistent) {
  // 每条自产响应都必须**自己就是合法的 CL 响应**：CL 声明的字节数 == body 实际
  // 字节数，状态行和终止串齐。数错 CL 的"错误响应"本身就是需要被 502 的畸形。
  for (std::uint16_t status : {std::uint16_t{400}, std::uint16_t{411}, std::uint16_t{413},
                               std::uint16_t{501}, std::uint16_t{502}}) {
    const std::string_view reply = gateway_error_reply(status);
    ASSERT_FALSE(reply.empty());
    const auto info = analyze_response_head(reply);
    EXPECT_EQ(info.status_code, status);
    EXPECT_EQ(info.framing, Framing::kContentLength);
    EXPECT_EQ(reply.size(), info.head_bytes + info.declared_body_bytes)
        << status << "：CL 与实际 body 字节数不一致";
    EXPECT_NE(reply.find("Connection: close"), std::string_view::npos)
        << "自产响应一律关连接：错误之后没有复用可言";
    const std::string prefix = "HTTP/1.1 " + std::to_string(status) + " ";
    EXPECT_EQ(reply.substr(0, prefix.size()), prefix);
  }
  // 支持集合之外的状态码：空 view，调用方不许当"发送成功"。
  EXPECT_TRUE(gateway_error_reply(503).empty());
  EXPECT_TRUE(gateway_error_reply(0).empty());
}

}  // namespace
}  // namespace l7gate::http
