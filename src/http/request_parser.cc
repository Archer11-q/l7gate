#include "http/request_parser.h"

#include <algorithm>
#include <limits>

namespace l7gate::http {
namespace {

constexpr std::uint8_t kCR = 0x0D;
constexpr std::uint8_t kLF = 0x0A;
constexpr std::uint8_t kSP = 0x20;
constexpr std::uint8_t kHT = 0x09;
constexpr std::uint8_t kDEL = 0x7F;

// RFC 9110 tchar：method 与 header 名只允许这些字符加数字字母。
bool is_tchar(std::uint8_t c) noexcept {
  switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'': case '*':
    case '+': case '-': case '.': case '^': case '_': case '`': case '|': case '~':
      return true;
    default:
      return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
  }
}

bool is_hex_digit(std::uint8_t c) noexcept {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

std::uint8_t ascii_lower(std::uint8_t c) noexcept {
  return (c >= 'A' && c <= 'Z') ? static_cast<std::uint8_t>(c + 32u) : c;
}

bool iequals(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (ascii_lower(static_cast<std::uint8_t>(a[i])) !=
        ascii_lower(static_cast<std::uint8_t>(b[i]))) {
      return false;
    }
  }
  return true;
}

std::string_view as_view(const std::uint8_t* data, std::size_t offset, std::size_t n) noexcept {
  return std::string_view(reinterpret_cast<const char*>(data + offset), n);
}

// 去掉两端的 OWS（SP / HTAB）。RFC 9110 要求接收方在解释字段值前先剥掉它们。
std::string_view trim_ows(std::string_view s) noexcept {
  std::size_t b = 0;
  std::size_t e = s.size();
  while (b < e && (static_cast<std::uint8_t>(s[b]) == kSP || static_cast<std::uint8_t>(s[b]) == kHT)) ++b;
  while (e > b && (static_cast<std::uint8_t>(s[e - 1]) == kSP ||
                   static_cast<std::uint8_t>(s[e - 1]) == kHT)) --e;
  return s.substr(b, e - b);
}

enum class LineKind : std::uint8_t { kFound, kBadEnding, kTooLong, kNeedMore };

struct LineScan {
  LineKind kind = LineKind::kNeedMore;
  std::size_t cr_index = 0;  // kFound 时指向 '\r'
};

// 在 [pos, pos + window) 内找行尾，window = 内容上限 + 2（CRLF 本身）。
// 为什么带窗口而不是整段扫到底：超长行是畸形输入的主要形态之一，窗口让它在收到
// "足以断定超限"的字节时立刻报错，而不是等对端把 1 GiB 挤完。也正因为这个，
// kTooLong 必须排在 kNeedMore 之前判 —— 反过来的话畸形输入会伪装成半包，
// 我拿不到 400，只会无限等。
//
// 循环上界取 window-1：这样 CR 最多落在 pos+max_line，找到的内容长度**不可能**
// 超过 max_line，超限判定不会 off-by-one 地放过 max_line+1 字节的行。
// 孤立 CR（后面不是 LF）在这里就判 kBadEnding，不留到"内容里混着 CR"再由上层
// 按字段解析时报一个不相干的错误码 —— 那种情况下报 version_unsupported 是误导。
LineScan scan_line(const std::uint8_t* data, std::size_t len, std::size_t pos,
                   std::size_t window) noexcept {
  const std::size_t stop = std::min(len, pos + window - 1);
  for (std::size_t i = pos; i < stop; ++i) {
    const std::uint8_t b = data[i];
    if (b == kCR) {
      if (i + 1 >= len) return {LineKind::kNeedMore, i};
      if (data[i + 1] != kLF) return {LineKind::kBadEnding, i};
      return {LineKind::kFound, i};
    }
    if (b == kLF) return {LineKind::kBadEnding, i};  // 裸 LF：前面的 CR 分支已处理过
  }
  if (len - pos >= window) return {LineKind::kTooLong, pos + window};
  return {LineKind::kNeedMore, stop};
}

// 纯数字 + 不溢出 + 不超过 max_value。空串、'+"/'-'、内嵌空格一律 false。
// 溢出用"乘之前先比阈值"判，不依赖回绕 —— 有符号回绕正是 UBSan 会抓的东西。
bool parse_decimal(std::string_view text, std::size_t max_value, std::size_t& out) noexcept {
  if (text.empty()) return false;
  std::size_t value = 0;
  for (char ch : text) {
    if (ch < '0' || ch > '9') return false;
    const std::size_t digit = static_cast<std::size_t>(ch - '0');
    if (value > (max_value - digit) / 10u) return false;
    value = value * 10u + digit;
  }
  out = value;
  return true;
}

bool parse_hex(std::string_view text, std::size_t max_value, std::size_t& out) noexcept {
  if (text.empty()) return false;
  std::size_t value = 0;
  for (char ch : text) {
    const std::uint8_t b = static_cast<std::uint8_t>(ch);
    if (!is_hex_digit(b)) return false;
    std::size_t digit = 0;
    if (b >= '0' && b <= '9') digit = static_cast<std::size_t>(b - '0');
    else if (b >= 'a' && b <= 'f') digit = static_cast<std::size_t>(b - 'a') + 10u;
    else digit = static_cast<std::size_t>(b - 'A') + 10u;
    if (value > (max_value - digit) / 16u) return false;
    value = value * 16u + digit;
  }
  out = value;
  return true;
}

// 数字串本身合法、只是超过上限时，报 body 太大而不是语法错误。
// 调用方已经知道 parse_* 失败了，这里只负责把两种失败分开。
ParseError oversize_or_invalid(std::string_view text, bool (*valid_char)(std::uint8_t),
                               ParseError invalid) noexcept {
  if (text.empty()) return invalid;
  for (char ch : text) {
    if (!valid_char(static_cast<std::uint8_t>(ch))) return invalid;
  }
  return ParseError::kBodyTooLarge;
}

bool char_is_digit(std::uint8_t c) noexcept { return c >= '0' && c <= '9'; }

ParseOutcome ok() noexcept { return {ParseStatus::kOk, ParseError::kNone}; }
ParseOutcome need_more() noexcept { return {ParseStatus::kNeedMoreData, ParseError::kNone}; }
ParseOutcome fail(ParseError error) noexcept { return {ParseStatus::kError, error}; }

}  // namespace

const char* to_string(ParseError error) noexcept {
  switch (error) {
    case ParseError::kNone: return "none";
    case ParseError::kRequestLineMalformed: return "request_line_malformed";
    case ParseError::kMethodInvalid: return "method_invalid";
    case ParseError::kTargetInvalid: return "target_invalid";
    case ParseError::kVersionUnsupported: return "version_unsupported";
    case ParseError::kLineEndingNotCRLF: return "line_ending_not_crlf";
    case ParseError::kHeaderLineMalformed: return "header_line_malformed";
    case ParseError::kHeaderNameInvalid: return "header_name_invalid";
    case ParseError::kHeaderValueInvalid: return "header_value_invalid";
    case ParseError::kObsFoldRejected: return "obs_fold_rejected";
    case ParseError::kHeaderTooLarge: return "header_too_large";
    case ParseError::kHeadersTooLarge: return "headers_too_large";
    case ParseError::kTooManyHeaders: return "too_many_headers";
    case ParseError::kContentLengthInvalid: return "content_length_invalid";
    case ParseError::kContentLengthDuplicated: return "content_length_duplicated";
    case ParseError::kContentLengthWithTE: return "content_length_with_te";
    case ParseError::kTransferEncodingInvalid: return "transfer_encoding_invalid";
    case ParseError::kHostDuplicated: return "host_duplicated";
    case ParseError::kHostMissing: return "host_missing";
    case ParseError::kChunkSizeInvalid: return "chunk_size_invalid";
    case ParseError::kChunkTerminatorMissing: return "chunk_terminator_missing";
    case ParseError::kBodyTooLarge: return "body_too_large";
    case ParseError::kNotImplemented: return "not_implemented";
  }
  return "unknown_error_code";
}

ParseScratch::ParseScratch(std::size_t max_header_count) : slots_(max_header_count) {}

std::optional<std::string_view> ParsedRequest::find_header(std::string_view name) const noexcept {
  for (const HeaderEntry& entry : headers) {
    if (iequals(entry.name, name)) return entry.value;
  }
  return std::nullopt;
}

ParseOutcome parse_request(const std::uint8_t* data, std::size_t len,
                           ParseScratch& scratch, const ParserConfig& config,
                           ParsedRequest& out) noexcept {
  out = ParsedRequest{};
  if (data == nullptr || len == 0) return need_more();

  const std::size_t line_window = config.max_header_line_bytes + 2;

  // --- 1. 请求行 ---------------------------------------------------------
  // 起始就是空行（收到 CRLF 开头）我选择**拒绝**而不是照 RFC 忽略掉：忽略意味着
  // 转出去的字节和收进来的不一致，而"多一个 CRLF"本身就是走私载荷的常见形态之一。
  {
    const LineScan rl = scan_line(data, len, 0, line_window);
    if (rl.kind == LineKind::kBadEnding) return fail(ParseError::kLineEndingNotCRLF);
    if (rl.kind == LineKind::kTooLong) return fail(ParseError::kHeaderTooLarge);
    if (rl.kind == LineKind::kNeedMore) return need_more();

    const std::size_t line_end = rl.cr_index;  // 指向请求行的 '\r'
    if (line_end == 0) return fail(ParseError::kRequestLineMalformed);

    // 请求行的分隔符必须是**单个** SP（RFC 9112 §3.0 不允许空白填充）。
    std::size_t sp1 = std::numeric_limits<std::size_t>::max();
    for (std::size_t i = 0; i < line_end; ++i) {
      if (data[i] == kSP) { sp1 = i; break; }
    }
    if (sp1 == std::numeric_limits<std::size_t>::max()) {
      return fail(ParseError::kRequestLineMalformed);
    }
    std::size_t sp2 = std::numeric_limits<std::size_t>::max();
    for (std::size_t i = sp1 + 1; i < line_end; ++i) {
      if (data[i] == kSP) { sp2 = i; break; }
    }
    if (sp2 == std::numeric_limits<std::size_t>::max()) {
      return fail(ParseError::kRequestLineMalformed);
    }

    const std::string_view method = as_view(data, 0, sp1);
    const std::string_view target = as_view(data, sp1 + 1, sp2 - sp1 - 1);
    const std::string_view version = as_view(data, sp2 + 1, line_end - sp2 - 1);

    if (method.empty()) return fail(ParseError::kRequestLineMalformed);
    for (std::size_t i = 0; i < sp1; ++i) {
      if (!is_tchar(data[i])) return fail(ParseError::kMethodInvalid);
    }
    if (version.empty()) return fail(ParseError::kRequestLineMalformed);
    if (!iequals(version, "HTTP/1.1") && !iequals(version, "HTTP/1.0")) {
      return fail(ParseError::kVersionUnsupported);
    }

    if (target.empty()) return fail(ParseError::kTargetInvalid);
    for (std::size_t i = sp1 + 1; i < sp2; ++i) {
      const std::uint8_t b = data[i];
      // 控制字符、SP、HTAB 都不允许出现在 target 里（b < 0x21 一次覆盖）；
      // b >= 0x80 按 obs-text 放行。
      if (b < 0x21 || b == kDEL) return fail(ParseError::kTargetInvalid);
    }
    // CONNECT 的目标是 authority-form（host:port），转发它需要隧道语义；
    // absolute-form 要转发前改写成 origin-form。两者都不在 S1 范围，但都**语法合法**，
    // 所以给 kNotImplemented 而不是 kTargetInvalid —— 调用方要区分"400 畸形"和
    // "501 我还不做"。
    if (iequals(method, "CONNECT")) return fail(ParseError::kNotImplemented);
    if (target == "*") {
      // asterisk-form：OPTIONS * 这类，合法，直接透传。
    } else if (target.front() == '/') {
      // origin-form：反向代理收到的正常形态。
    } else if ((target.size() >= 7 && iequals(target.substr(0, 7), "http://")) ||
               (target.size() >= 8 && iequals(target.substr(0, 8), "https://"))) {
      return fail(ParseError::kNotImplemented);
    } else {
      return fail(ParseError::kTargetInvalid);
    }

    out.method = method;
    out.target = target;
    out.version = version;
    out.http_1_1 = iequals(version, "HTTP/1.1");
    out.headers_bytes = line_end + 2;
  }

  // --- 2. header 区 ------------------------------------------------------
  std::span<HeaderEntry> slots = scratch.slots();
  std::size_t header_count = 0;
  std::size_t pos = out.headers_bytes;

  for (;;) {
    const LineScan hl = scan_line(data, len, pos, line_window);
    if (hl.kind == LineKind::kBadEnding) return fail(ParseError::kLineEndingNotCRLF);
    if (hl.kind == LineKind::kTooLong) return fail(ParseError::kHeaderTooLarge);
    if (hl.kind == LineKind::kNeedMore) return need_more();

    const std::size_t line_begin = pos;
    const std::size_t line_end = hl.cr_index;
    pos = line_end + 2;

    if (out.headers_bytes + (pos - line_begin) > config.max_headers_bytes) {
      return fail(ParseError::kHeadersTooLarge);
    }
    out.headers_bytes += pos - line_begin;

    if (line_end == line_begin) break;  // 空行：头部区到此结束

    const std::uint8_t first = data[line_begin];
    if (first == kSP || first == kHT) return fail(ParseError::kObsFoldRejected);

    std::size_t colon = std::numeric_limits<std::size_t>::max();
    for (std::size_t i = line_begin; i < line_end; ++i) {
      if (data[i] == ':') { colon = i; break; }
    }
    if (colon == std::numeric_limits<std::size_t>::max() || colon == line_begin) {
      return fail(ParseError::kHeaderLineMalformed);
    }
    for (std::size_t i = line_begin; i < colon; ++i) {
      if (!is_tchar(data[i])) return fail(ParseError::kHeaderNameInvalid);
    }

    const std::size_t value_begin = colon + 1;
    for (std::size_t i = value_begin; i < line_end; ++i) {
      const std::uint8_t b = data[i];
      // 裸 CR/LF 在 scan_line 那一层就拦掉了；这里管 NUL、其余 C0 控制符与 DEL。
      // HTAB 允许（VCHAR 之间的空白），obs-text（>=0x80）按 RFC 放行。
      if ((b != kHT && b < 0x20) || b == kDEL) return fail(ParseError::kHeaderValueInvalid);
    }

    if (header_count >= slots.size()) return fail(ParseError::kTooManyHeaders);
    slots[header_count] = HeaderEntry{
        as_view(data, line_begin, colon - line_begin),
        trim_ows(as_view(data, value_begin, line_end - value_begin))};
    ++header_count;
  }

  out.headers = slots.first(header_count);

  // --- 3. 头部语义：Host / Content-Length / Transfer-Encoding ------------
  // 检查顺序是刻意的：语法（上面那一圈）→ CL/TE 的合法性与冲突 → Host → body 框架。
  // 同一段字节同时命中多条规则时报第一个，测试就按这个顺序断言。
  std::size_t host_count = 0;
  std::size_t cl_count = 0;
  std::size_t te_count = 0;
  std::size_t cl_value = 0;
  bool te_ok = false;
  for (const HeaderEntry& entry : out.headers) {
    if (iequals(entry.name, "Host")) {
      ++host_count;
    } else if (iequals(entry.name, "Content-Length")) {
      ++cl_count;
      std::size_t value = 0;
      if (!parse_decimal(entry.value, config.max_body_bytes, value)) {
        return fail(oversize_or_invalid(entry.value, char_is_digit,
                                       ParseError::kContentLengthInvalid));
      }
      // 多个**相同**的 CL 按 RFC 9112 合并成一个；不同值没有"取哪个"的余地，只能拒。
      if (cl_count > 1 && value != cl_value) return fail(ParseError::kContentLengthDuplicated);
      cl_value = value;
    } else if (iequals(entry.name, "Transfer-Encoding")) {
      ++te_count;
      // 只接受"恰好一个 chunked"：多值（chunked, gzip）、非 chunked、重复头全拒。
      // 这些正是 CL.TE / TE.CL 走私的全部载荷形态，代理没有"猜"的余地。
      const std::string_view v = trim_ows(entry.value);
      te_ok = (te_count == 1) && v.find(',') == std::string_view::npos && iequals(v, "chunked");
    }
  }

  if (te_count > 0 && !te_ok) return fail(ParseError::kTransferEncodingInvalid);
  if (cl_count > 0 && te_count > 0) return fail(ParseError::kContentLengthWithTE);
  if (host_count > 1) return fail(ParseError::kHostDuplicated);
  if (out.http_1_1 && host_count == 0) return fail(ParseError::kHostMissing);

  // --- 4. body 框架 ------------------------------------------------------
  if (cl_count == 0 && te_count == 0) {
    out.body_kind = BodyKind::kNone;
    out.consumed = out.headers_bytes;
    return ok();
  }

  if (cl_count > 0) {
    out.body_kind = BodyKind::kContentLength;
    out.declared_body_bytes = cl_value;
    out.body_raw_bytes = cl_value;
    const std::size_t need = out.headers_bytes + cl_value;
    if (len < need) return need_more();
    out.raw_body = as_view(data, out.headers_bytes, cl_value);
    out.consumed = need;
    return ok();
  }

  // chunked：逐块校验框架，**不解码不重写**。declared 是各 chunk 之和（解码后长度），
  // consumed 含全部框架开销 —— 这两个数分开，S1-06 才能既逐字节透传又知道真实体量。
  out.body_kind = BodyKind::kChunked;
  std::size_t cursor = out.headers_bytes;
  std::size_t decoded = 0;
  for (;;) {
    const LineScan sl = scan_line(data, len, cursor, line_window);
    if (sl.kind == LineKind::kBadEnding) return fail(ParseError::kChunkTerminatorMissing);
    if (sl.kind == LineKind::kTooLong) return fail(ParseError::kChunkSizeInvalid);
    if (sl.kind == LineKind::kNeedMore) return need_more();

    std::string_view hex = as_view(data, cursor, sl.cr_index - cursor);
    // chunk 扩展（";name=v"）在 HTTP/1.1 合法且对转发无意义，截断即忽略。
    const std::size_t semi = hex.find(';');
    if (semi != std::string_view::npos) hex = hex.substr(0, semi);

    std::size_t chunk_size = 0;
    if (!parse_hex(hex, config.max_body_bytes, chunk_size)) {
      return fail(oversize_or_invalid(hex, is_hex_digit, ParseError::kChunkSizeInvalid));
    }

    if (chunk_size == 0) {
      // last-chunk 之后是 trailer 区（可以为空），以一个空行收尾。S1 不理解 trailer
      // 内容（ADR-09：不重写、不理解），但必须把它当框架跳过 —— 否则 trailer 会被
      // 误认成下一条请求的请求行，那是功能级的错位而不是格式问题。
      cursor = sl.cr_index + 2;
      for (;;) {
        const LineScan tl = scan_line(data, len, cursor, line_window);
        if (tl.kind == LineKind::kBadEnding) return fail(ParseError::kChunkTerminatorMissing);
        if (tl.kind == LineKind::kTooLong) return fail(ParseError::kChunkSizeInvalid);
        if (tl.kind == LineKind::kNeedMore) return need_more();
        const bool closing_empty_line = (tl.cr_index == cursor);
        cursor = tl.cr_index + 2;
        if (closing_empty_line) break;
      }
      out.declared_body_bytes = decoded;
      out.body_raw_bytes = cursor - out.headers_bytes;
      out.consumed = cursor;
      return ok();
    }

    if (chunk_size > config.max_body_bytes - decoded) return fail(ParseError::kBodyTooLarge);
    decoded += chunk_size;

    const std::size_t data_end = sl.cr_index + 2 + chunk_size;
    if (len < data_end + 2) return need_more();
    if (data[data_end] != kCR || data[data_end + 1] != kLF) {
      return fail(ParseError::kChunkTerminatorMissing);
    }
    cursor = data_end + 2;
  }
}

}  // namespace l7gate::http
