#include "http/response_framing.h"

#include <cstdint>

#include "http/response_head.h"

namespace l7gate::http {

namespace {

constexpr std::uint8_t kCR = 0x0D;
constexpr std::uint8_t kLF = 0x0A;
constexpr std::uint8_t kSP = 0x20;
constexpr std::uint8_t kHT = 0x09;

bool is_hex_digit(std::uint8_t c) noexcept {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

std::uint8_t hex_value(std::uint8_t c) noexcept {
  if (c >= '0' && c <= '9') return static_cast<std::uint8_t>(c - '0');
  if (c >= 'a' && c <= 'f') return static_cast<std::uint8_t>(c - 'a' + 10);
  return static_cast<std::uint8_t>(c - 'A' + 10);
}

bool is_tchar(std::uint8_t c) noexcept {
  // RFC 9110 §5.6.2 的 tchar 全集。
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return true;
  switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+':
    case '-': case '.': case '^': case '_': case '`': case '|': case '~':
      return true;
    default:
      return false;
  }
}

bool is_ows(std::uint8_t c) noexcept { return c == kSP || c == kHT; }

bool value_byte_ok(std::uint8_t c) noexcept {
  // 值里允许可见字符、HTAB 与 obs-text(0x80-0xFF)；NUL、裸 CR/LF 与其他控制符不允许。
  // 和请求侧 kHeaderValueInvalid 同一条口径——同一条规则在两侧长得不一样，
  // 迟早有人问"哪边是对的"。
  if (c == kHT) return true;
  if (c >= 0x80) return true;
  return c >= 0x20 && c != 0x7F;
}

std::string_view lower_token(std::string_view s, char* buf, std::size_t cap) noexcept {
  std::size_t n = s.size() < cap ? s.size() : cap;
  for (std::size_t i = 0; i < n; ++i) {
    const char c = s[i];
    buf[i] = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  }
  return std::string_view(buf, n);
}

// 在头部区里逐个认 CL/TE，并按 RFC 9112 §6.3 的优先级定框架。
// 返回 false 表示这条响应头直接不可接受。判定集合与请求侧一致：
// **代理没有"猜"的余地**——CL+TE 并存、多个不一致 CL、非法 TE 都是走私载荷的
// 形态，只不过方向反过来。
bool collect_framing_claims(std::string_view headers, ResponseHeadInfo& info) noexcept {
  bool saw_te = false;
  bool saw_cl = false;
  std::uint64_t cl_value = 0;

  std::size_t pos = 0;
  while (pos < headers.size()) {
    const std::size_t eol = headers.find("\r\n", pos);
    if (eol == std::string_view::npos) return false;  // 行没按 CRLF 收尾
    const std::string_view line = headers.substr(pos, eol - pos);
    pos = eol + 2;
    if (line.empty()) continue;  // 终止串前的空行，被头视图天然吸收，这里是双保险
    const std::uint8_t first = static_cast<std::uint8_t>(line[0]);
    if (is_ows(first)) return false;  // obs-fold：代理必须拒（同请求侧）
    // 行内不许再出现 CR 或 LF：`\nX` 会骗过 find("\r\n") 的切分（"A\nB\r\n" 的
    // 第一行视图是 "A\nB"，裸 LF 藏在里面），逐字节查才是和请求侧
    // kLineEndingNotCRLF 同一条口径——两侧对"行尾"的定义不能长得不一样。
    if (line.find('\n') != std::string_view::npos) return false;   // 裸 LF
    if (line.find('\r') != std::string_view::npos) return false;   // 孤立 CR

    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) return false;
    const std::string_view name = line.substr(0, colon);
    for (const char c : name) {
      if (!is_tchar(static_cast<std::uint8_t>(c))) return false;
    }
    std::string_view value = line.substr(colon + 1);
    while (!value.empty() && is_ows(static_cast<std::uint8_t>(value.front()))) value.remove_prefix(1);
    while (!value.empty() && is_ows(static_cast<std::uint8_t>(value.back()))) value.remove_suffix(1);
    for (const char c : value) {
      if (!value_byte_ok(static_cast<std::uint8_t>(c))) return false;
    }

    char lbuf[32];
    const std::string_view lname = lower_token(name, lbuf, sizeof(lbuf));

    if (lname == "content-length") {
      if (value.empty()) return false;
      std::uint64_t v = 0;
      for (const char c : value) {
        if (c < '0' || c > '9') return false;  // 非纯数字 / 带符号 / 混合：一律拒
        if (v > (UINT64_MAX - static_cast<std::uint64_t>(c - '0')) / 10) return false;
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
      }
      if (saw_cl && v != cl_value) return false;  // 多个不一致的 CL
      saw_cl = true;
      cl_value = v;
    } else if (lname == "transfer-encoding") {
      // 恰好一个 `chunked` 记号。多值、重复、其他算法在 S1 的范围外，而"范围外"
      // 在响应侧同样只能是**拒**：网关不认识框架就没法判边界，与其猜一个
      // "大概是 chunked"，不如把这头当坏上游（→ 502，判定见 reactor）。
      if (saw_te) return false;  // 重复的 TE 头：框架声明本身有两个来源，走私形态
      saw_te = true;
      std::string_view rest = value;
      int tokens = 0;
      while (true) {
        const std::size_t comma = rest.find(',');
        std::string_view item = (comma == std::string_view::npos) ? rest : rest.substr(0, comma);
        while (!item.empty() && is_ows(static_cast<std::uint8_t>(item.front()))) item.remove_prefix(1);
        while (!item.empty() && is_ows(static_cast<std::uint8_t>(item.back()))) item.remove_suffix(1);
        ++tokens;
        char ibuf[16];
        if (lower_token(item, ibuf, sizeof(ibuf)) != "chunked") return false;
        if (comma == std::string_view::npos) break;
        rest = rest.substr(comma + 1);
      }
      if (tokens != 1) return false;  // 逗号分隔的多值（哪怕全是 chunked）：S1 不支持
    }
  }

  if (saw_te && saw_cl) return false;  // CL+TE：走私经典入口

  // 框架结论。状态码类别**优先于**头的声明：204/304 即使带 CL 也按无 body 处理
  // ——判定顺序是 RFC 9112 §6.3 写的，不是我挑的。
  const unsigned code = info.status_code;
  if (code >= 100 && code < 200) {
    info.framing = ResponseFraming::kProvisional;
  } else if (code == 204 || code == 304) {
    info.framing = ResponseFraming::kNoBody;
  } else if (saw_te) {
    info.framing = ResponseFraming::kChunked;
  } else if (saw_cl) {
    info.framing = ResponseFraming::kContentLength;
    info.declared_body_bytes = static_cast<std::size_t>(cl_value);
  } else {
    info.framing = ResponseFraming::kCloseDelimited;
  }
  return true;
}

}  // namespace

ResponseHeadInfo analyze_response_head(const std::uint8_t* data, std::size_t len) noexcept {
  ResponseHeadInfo info;
  if (data == nullptr || len < 16) {
    return info;  // kHeadIncomplete：连"最短可能完整"都没到，不值得开判
  }
  const std::size_t term = find_header_terminator(data, len);
  if (term == std::string_view::npos) return info;  // 头没到齐：什么都不判（半包不是错误）
  info.head_bytes = term + 4;

  // 状态行：HTTP/1.<digit> SP 3DIGIT (SP reason | CR 收尾)。与 101 判定共用逐字节
  // 口径，只是状态码这里要的是"三位数字"而不是三个字面量。
  if (data[0] != 'H' || data[1] != 'T' || data[2] != 'T' || data[3] != 'P' || data[4] != '/') {
    info.framing = ResponseFraming::kHeadInvalid;
    return info;
  }
  if (data[5] != '1' || data[6] != '.' || data[7] < '0' || data[7] > '9' || data[8] != ' ') {
    info.framing = ResponseFraming::kHeadInvalid;
    return info;
  }
  if (data[9] < '0' || data[9] > '9' || data[10] < '0' || data[10] > '9' ||
      data[11] < '0' || data[11] > '9') {
    info.framing = ResponseFraming::kHeadInvalid;
    return info;
  }
  if (data[12] != ' ' && data[12] != kCR) {
    info.framing = ResponseFraming::kHeadInvalid;  // 第四位数字 = 不存在这种状态码（1015 教训，S1-05 同款）
    return info;
  }
  info.status_code = static_cast<unsigned>(data[9] - '0') * 100u +
                     static_cast<unsigned>(data[10] - '0') * 10u +
                     static_cast<unsigned>(data[11] - '0');
  if (info.status_code < 100 || info.status_code > 599) {
    info.framing = ResponseFraming::kHeadInvalid;  // 编号出界：没有这种响应
    return info;
  }

  // 状态行之后的头部区。先定状态行的终点。
  std::size_t hl = 12;
  if (data[12] == kSP) {
    // 有 reason：状态行的终点是它自己的 CRLF。这一趟扫描同时兼任"状态行里
    // 不许藏控制符"的检查——裸 LF 会让 find("\r\n") 把整行连着后面的 header
    // 吞成一条状态行（"200 OK\nContent-Length: 1\r\n"），CL 就此蒸发、框架被
    // 误判成 close-delimited。那条响应会一路发到 EOF 才被发现长度不对，
    // 而客户端按 CL 解读的下游早已错位。
    std::size_t i = 12;
    bool found = false;
    while (i + 1 < info.head_bytes) {
      if (data[i] == kLF) {
        info.framing = ResponseFraming::kHeadInvalid;  // 裸 LF：状态行的行尾不是 CRLF
        return info;
      }
      if (data[i] == kCR) {
        if (data[i + 1] != kLF) {
          info.framing = ResponseFraming::kHeadInvalid;  // 孤立 CR
          return info;
        }
        hl = i + 2;
        found = true;
        break;
      }
      ++i;
    }
    if (!found) {
      info.framing = ResponseFraming::kHeadInvalid;  // reason 后面没有 CRLF 收尾
      return info;
    }
  } else {
    if (data[13] != kLF) {
      info.framing = ResponseFraming::kHeadInvalid;  // "204\rX"：孤立 CR 不是行尾
      return info;
    }
    hl = 14;  // 无 reason："HTTP/1.1 204\r\n…"，CRLF 就在 12/13
  }

  // 头视图 = 状态行之后到**倒数第二个 CRLF 之前**。终止串是 CRLFCRLF，
  // 去掉最后 2 字节（空行自身的 CRLF）后，每个头部行都带着自己的 CRLF 结束。
  const std::string_view headers(reinterpret_cast<const char*>(data) + hl,
                                 info.head_bytes >= hl + 2 ? info.head_bytes - hl - 2 : 0);
  if (!collect_framing_claims(headers, info)) {
    info.framing = ResponseFraming::kHeadInvalid;
  }
  return info;
}

std::size_t chunked_advance(ChunkedProgress& p, const std::uint8_t* data, std::size_t len) noexcept {
  std::size_t i = 0;
  while (i < len) {
    switch (p.phase) {
      case ChunkedProgress::Phase::kComplete:
      case ChunkedProgress::Phase::kInvalid:
        return i;

      case ChunkedProgress::Phase::kSizeLine: {
        const std::uint8_t c = data[i];
        if (p.line_saw_cr) {
          if (c != kLF) {
            p.phase = ChunkedProgress::Phase::kInvalid;  // 孤立 CR
            return i;
          }
          p.line_saw_cr = false;
          ++i;
          if (!p.size_any || p.size_overflow) {
            p.phase = ChunkedProgress::Phase::kInvalid;  // 空 size 行 / 溢出
            return i;
          }
          if (p.size_acc == 0) {
            p.phase = ChunkedProgress::Phase::kTrailers;
            p.line_len = 0;
          } else {
            p.remaining = static_cast<std::size_t>(p.size_acc);
            p.phase = ChunkedProgress::Phase::kData;
          }
          p.size_acc = 0;
          p.size_any = false;
          p.size_overflow = false;
          p.ext_mode = false;
          continue;
        }
        if (p.ext_mode) {
          if (c == kCR) {
            p.line_saw_cr = true;
          } else if (c == kLF) {
            p.phase = ChunkedProgress::Phase::kInvalid;  // ext 里的裸 LF：行尾不是 CRLF
          }
          ++i;  // chunk-ext 内容不解释，只等行尾
          continue;
        }
        if (c == kCR) {
          p.line_saw_cr = true;
          ++i;
          continue;
        }
        if (c == kLF) {
          p.phase = ChunkedProgress::Phase::kInvalid;  // 裸 LF
          return i;
        }
        if (c == ';') {
          p.ext_mode = true;
          ++i;
          continue;
        }
        if (is_hex_digit(c)) {
          const std::uint64_t v = hex_value(c);
          if (!p.size_overflow) {
            if (p.size_acc > (UINT64_MAX - v) / 16) {
              p.size_overflow = true;  // 行尾结算时才判死：字节先透传，边界不回头
            } else {
              p.size_acc = p.size_acc * 16 + v;
            }
          }
          p.size_any = true;
          ++i;
          continue;
        }
        p.phase = ChunkedProgress::Phase::kInvalid;  // 非 hex 且非 ext
        return i;
      }

      case ChunkedProgress::Phase::kData: {
        const std::size_t take = p.remaining < (len - i) ? p.remaining : (len - i);
        i += take;
        p.remaining -= take;
        if (p.remaining == 0) {
          p.phase = ChunkedProgress::Phase::kDataCrlf;
          p.crlf_seen = 0;
        }
        continue;
      }

      case ChunkedProgress::Phase::kDataCrlf: {
        const std::uint8_t c = data[i];
        if (p.crlf_seen == 0) {
          if (c != kCR) {
            p.phase = ChunkedProgress::Phase::kInvalid;
            return i;
          }
          p.crlf_seen = 1;
          ++i;
          continue;
        }
        if (c != kLF) {
          p.phase = ChunkedProgress::Phase::kInvalid;
          return i;
        }
        p.crlf_seen = 0;
        p.phase = ChunkedProgress::Phase::kSizeLine;
        ++i;
        continue;
      }

      case ChunkedProgress::Phase::kTrailers: {
        const std::uint8_t c = data[i];
        if (p.line_saw_cr) {
          if (c == kLF) {
            p.line_saw_cr = false;
            ++i;
            if (p.line_len == 0) {
              p.phase = ChunkedProgress::Phase::kComplete;
              return i;  // 空行结算：chunked 流到此为止，后面的字节属于下一条响应
            }
            p.line_len = 0;
            continue;
          }
          // CR 不在行尾：trailer 值里的合法字节，继续。
          p.line_saw_cr = false;
          p.line_len += 2;
          ++i;
          continue;
        }
        if (c == kCR) {
          p.line_saw_cr = true;
          ++i;
          continue;
        }
        if (c == kLF) {
          p.phase = ChunkedProgress::Phase::kInvalid;  // trailer 里的裸 LF
          return i;
        }
        ++p.line_len;
        ++i;
        continue;
      }
    }
  }
  return i;
}

std::uint16_t reply_status_for_parse_error(ParseError error) noexcept {
  // 刻意不写 default：新增 ParseError 而忘了归类时，clang 的 -Wswitch 会点名，
  // 而"未映射即当 400"的兜底会把新错误码静默吞成语义错误的响应。
  switch (error) {
    case ParseError::kNone: return 0;
    case ParseError::kRequestLineMalformed: return 400;
    case ParseError::kMethodInvalid: return 400;
    case ParseError::kTargetInvalid: return 400;
    case ParseError::kVersionUnsupported: return 400;  // 与 nginx 同口径：不认识的版本行就是坏请求
    case ParseError::kLineEndingNotCRLF: return 400;
    case ParseError::kHeaderLineMalformed: return 400;
    case ParseError::kHeaderNameInvalid: return 400;
    case ParseError::kHeaderValueInvalid: return 400;
    case ParseError::kObsFoldRejected: return 400;
    case ParseError::kHeaderTooLarge: return 413;
    case ParseError::kHeadersTooLarge: return 413;
    case ParseError::kTooManyHeaders: return 413;
    case ParseError::kContentLengthInvalid: return 400;
    case ParseError::kContentLengthDuplicated: return 400;
    case ParseError::kContentLengthWithTE: return 400;
    case ParseError::kTransferEncodingInvalid: return 400;
    case ParseError::kHostDuplicated: return 400;
    case ParseError::kHostMissing: return 400;
    case ParseError::kChunkSizeInvalid: return 400;
    case ParseError::kChunkTerminatorMissing: return 400;
    case ParseError::kBodyTooLarge: return 413;
    case ParseError::kNotImplemented: return 501;
  }
  return 0;  // 全部 22+1 个值都已列出；走到这里只可能是枚举加了新值（-Wswitch 已点名）
}

std::string_view gateway_error_reply(std::uint16_t status) noexcept {
  // 每一条都是"头 + CL + Connection: close + body"的完整响应；body 就是 reason
  // 加一个换行，CL 与字面字节数一一对死（测试逐条核对，不靠人肉数）。
  switch (status) {
    case 400:
      return "HTTP/1.1 400 Bad Request\r\n"
             "Content-Length: 12\r\n"
             "Connection: close\r\n"
             "\r\n"
             "Bad Request\n";
    case 411:
      return "HTTP/1.1 411 Length Required\r\n"
             "Content-Length: 16\r\n"
             "Connection: close\r\n"
             "\r\n"
             "Length Required\n";
    case 413:
      return "HTTP/1.1 413 Content Too Large\r\n"
             "Content-Length: 18\r\n"
             "Connection: close\r\n"
             "\r\n"
             "Content Too Large\n";
    case 501:
      return "HTTP/1.1 501 Not Implemented\r\n"
             "Content-Length: 16\r\n"
             "Connection: close\r\n"
             "\r\n"
             "Not Implemented\n";
    case 502:
      return "HTTP/1.1 502 Bad Gateway\r\n"
             "Content-Length: 12\r\n"
             "Connection: close\r\n"
             "\r\n"
             "Bad Gateway\n";
    default:
      return {};  // 调用方不许把空 view 当"发送成功"（头文件明写）
  }
}

}  // namespace l7gate::http
