#include "config/json.h"

#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace l7gate::config {
namespace {

constexpr int kMaxDepth = 32;

const char* hex_digit_value(char c, int* out) noexcept {
  if (c >= '0' && c <= '9') { *out = c - '0'; return nullptr; }
  if (c >= 'a' && c <= 'f') { *out = c - 'a' + 10; return nullptr; }
  if (c >= 'A' && c <= 'F') { *out = c - 'A' + 10; return nullptr; }
  return "非法的 \\u 十六进制数字";
}

void append_utf8(std::string* out, std::uint32_t code) {
  if (code < 0x80u) {
    out->push_back(static_cast<char>(code));
  } else if (code < 0x800u) {
    out->push_back(static_cast<char>(0xC0u | (code >> 6)));
    out->push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
  } else {
    out->push_back(static_cast<char>(0xE0u | (code >> 12)));
    out->push_back(static_cast<char>(0x80u | ((code >> 6) & 0x3Fu)));
    out->push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
  }
}

struct Parser {
  std::string_view text;
  std::size_t pos = 0;
  std::string* error = nullptr;

  bool fail(std::string_view why) {
    // 位置报**字节偏移**而不是行号：配置文件是人手写的，行号友好，但用例里断的是
    // "坏输入必须被拒"这件事，而报错文本只要求能定位。行号要等 S5 的配置编辑面一起做，
    // 现在假装有行号（数错一次就是一条错的话）比没有更坏。
    *error = "偏移 " + std::to_string(pos) + "：" + std::string(why);
    return false;
  }

  void skip_ws() {
    while (pos < text.size()) {
      const char c = text[pos];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos;
        continue;
      }
      // 注释在这套子集里不被接受，所以这里也**不能**跳过 `//`：让调用方的
      // "期望一个值"分支直接把它报成非法值，比在这里悄悄吃掉要好。
      break;
    }
  }

  bool peek(char c) const noexcept { return pos < text.size() && text[pos] == c; }

  bool literal(std::string_view word) {
    if (text.substr(pos, word.size()) != word) return false;
    pos += word.size();
    return true;
  }

  bool parse_string(std::string* out) {
    if (!peek('"')) return fail("期望字符串（配置文件里的键与值都必须是带双引号的字符串）");
    ++pos;  // 开引号
    std::size_t start = pos;  // 连续片段的起点，遇到转义才 flush
    auto flush = [&]() {
      if (pos > start) out->append(text.data() + start, pos - start);
      start = pos;
    };
    while (true) {
      if (pos >= text.size()) return fail("字符串没有结束引号");
      const char c = text[pos];
      if (c == '"') {
        flush();
        ++pos;
        return true;
      }
      if (c == '\\') {
        flush();
        ++pos;
        if (pos >= text.size()) return fail("反斜杠后面没有字符");
        const char e = text[pos++];
        switch (e) {
          case '"': out->push_back('"'); break;
          case '\\': out->push_back('\\'); break;
          case '/': out->push_back('/'); break;
          case 'b': out->push_back('\b'); break;
          case 'f': out->push_back('\f'); break;
          case 'n': out->push_back('\n'); break;
          case 'r': out->push_back('\r'); break;
          case 't': out->push_back('\t'); break;
          case 'u': {
            if (pos + 4 > text.size()) return fail("\\u 后面不足 4 位十六进制");
            std::uint32_t code = 0;
            for (std::size_t i = 0; i < 4u; ++i) {
              int digit = 0;
              if (const char* why = hex_digit_value(text[pos + i], &digit); why != nullptr) {
                pos += i;
                return fail(why);
              }
              code = (code << 4) | static_cast<std::uint32_t>(digit);
            }
            pos += 4;
            // 代理对拒绝（子集边界写在头文件里）：留下它等于允许一份"看起来配了、
            // 实际存的是半个字符"的地址或键名。
            if (code >= 0xD800u && code <= 0xDFFFu) return fail("\\u 落在代理对区间，本子集不接受");
            append_utf8(out, code);
            break;
          }
          default: return fail("不支持的转义序列");
        }
        start = pos;
        continue;
      }
      // 裸控制字符拒绝：JSON 要求它们在字符串里必须转义。让它们进来等于允许
      // 一个 CRLF 塞进 pattern 里，而 `pattern` 后面会被正则编译——那是自己造走私面。
      if (static_cast<unsigned char>(c) < 0x20u) return fail("字符串里有未转义的控制字符");
      ++pos;
    }
  }

  bool parse_number(JsonValue* out) {
    const std::size_t start = pos;
    if (peek('-')) ++pos;  // 只允许前导负号：`+1` 不是 JSON
    if (pos >= text.size() || !std::isdigit(static_cast<unsigned char>(text[pos]))) {
      pos = start;
      return fail("期望数字");
    }
    // 前导零拒绝（`01` 在配置文件里通常是有人把八进制或者手抖当成了十进制）。
    if (text[pos] == '0' && pos + 1 < text.size() &&
        std::isdigit(static_cast<unsigned char>(text[pos + 1]))) {
      return fail("不接受带前导零的数字");
    }
    while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) ++pos;
    if (pos < text.size()) {
      const char c = text[pos];
      if (c == '.' || c == 'e' || c == 'E') return fail("本子集只接受十进制整数，不接受小数或指数");
    }
    const std::string_view digits = text.substr(start, pos - start);
    // 端口、vnodes 这些字段都是"小到能塞进 uint16/uint32"的量，64 位的解析范围足够，
    // 溢出只在真的写了 20 位数字时才可能发生——那时报"越界"而不是让它绕回负数。
    std::int64_t value = 0;
    bool negative = false;
    std::size_t i = 0;
    if (digits[0] == '-') {
      negative = true;
      i = 1;
    }
    for (; i < digits.size(); ++i) {
      const int d = digits[i] - '0';
      if (value > (INT64_MAX - d) / 10) return fail("数字超出 64 位范围");
      value = value * 10 + d;
    }
    out->kind = JsonValue::Kind::kInt;
    out->number = negative ? -value : value;
    return true;
  }

  bool parse_value(JsonValue* out, int depth) {
    if (depth > kMaxDepth) return fail("嵌套超过 32 层（本子集的深度上限）");
    skip_ws();
    if (pos >= text.size()) return fail("期望一个值，文件却结束了");
    const char c = text[pos];
    if (c == '{') return parse_object(out, depth);
    if (c == '[') return parse_array(out, depth);
    if (c == '"') {
      out->kind = JsonValue::Kind::kString;
      return parse_string(&out->text);
    }
    if (c == 't') {
      if (!literal("true") || (pos < text.size() && std::isalpha(
                                   static_cast<unsigned char>(text[pos])))) {
        return fail("`t` 开头的合法字面量只有 true");
      }
      out->kind = JsonValue::Kind::kBool;
      out->boolean = true;
      return true;
    }
    if (c == 'f') {
      if (!literal("false") || (pos < text.size() && std::isalpha(
                                    static_cast<unsigned char>(text[pos])))) {
        return fail("`f` 开头的合法字面量只有 false");
      }
      out->kind = JsonValue::Kind::kBool;
      out->boolean = false;
      return true;
    }
    if (c == 'n') {
      if (!literal("null") || (pos < text.size() && std::isalpha(
                                   static_cast<unsigned char>(text[pos])))) {
        return fail("`n` 开头的合法字面量只有 null");
      }
      out->kind = JsonValue::Kind::kNull;
      return true;
    }
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return parse_number(out);
    // NaN / Infinity 走到这里就落到这一条——它们不是 JSON，而"字段值写了个 NaN"
    // 在最坏情况下会被读成一个 0，那比拒绝危险得多。
    return fail("无法识别的值");
  }

  bool parse_object(JsonValue* out, int depth) {
    out->kind = JsonValue::Kind::kObject;
    ++pos;  // '{'
    skip_ws();
    if (peek('}')) {
      ++pos;
      return true;  // 空对象合法（是否允许由上层字段校验决定，不由这里）
    }
    while (true) {
      skip_ws();
      std::string key;
      if (!parse_string(&key)) return false;
      skip_ws();
      if (!peek(':')) return fail("键名后面缺冒号");
      ++pos;
      JsonValue value;
      if (!parse_value(&value, depth + 1)) return false;
      // 重复键拒绝：多数库是"后者覆盖前者"，而那不是一份配置该有的语义——
      // 写了两次 `port` 的人要么是没注意，要么是在测最后一行生效；两种都该被挡在启动前。
      for (const auto& existing : out->members) {
        if (existing.key == key) {
          pos -= key.size() + 2;
          return fail("同一个对象里出现了重复的键");
        }
      }
      out->members.push_back(JsonValue::Member{std::move(key), std::move(value)});
      skip_ws();
      if (peek(',')) {
        ++pos;
        skip_ws();
        if (peek('}')) return fail("不接受尾逗号");  // 尾逗号是最常见的手改残留
        continue;
      }
      if (peek('}')) {
        ++pos;
        return true;
      }
      return fail("对象里期望逗号或右花括号");
    }
  }

  bool parse_array(JsonValue* out, int depth) {
    out->kind = JsonValue::Kind::kArray;
    ++pos;  // '['
    skip_ws();
    if (peek(']')) {
      ++pos;
      return true;  // 空数组合法；"这个字段不能为空"由上层字段校验说
    }
    while (true) {
      JsonValue value;
      if (!parse_value(&value, depth + 1)) return false;
      out->items.push_back(std::move(value));
      skip_ws();
      if (peek(',')) {
        ++pos;
        skip_ws();
        if (peek(']')) return fail("不接受尾逗号");
        continue;
      }
      if (peek(']')) {
        ++pos;
        return true;
      }
      return fail("数组里期望逗号或右方括号");
    }
  }
};

}  // namespace

const JsonValue* JsonValue::find(std::string_view key) const noexcept {
  for (const auto& member : members) {
    if (member.key == key) return &member.value;
  }
  return nullptr;
}

const char* JsonValue::kind_name() const noexcept {
  switch (kind) {
    case Kind::kObject: return "对象";
    case Kind::kArray: return "数组";
    case Kind::kString: return "字符串";
    case Kind::kInt: return "整数";
    case Kind::kBool: return "布尔";
    case Kind::kNull: return "null";
  }
  return "?";
}

bool parse_json(std::string_view text, JsonValue* out, std::string* error) noexcept {
  Parser parser{.text = text, .error = error};
  parser.skip_ws();
  // 顶层只接受对象或数组（BOM 也在这里挡掉：`\\xEF\\xBB\\xBF` 是编辑器写的，
  // 不是配置的内容，而"跳过它"会让同一份文件在两种编辑器下有不同解释）。
  if (parser.pos < parser.text.size() &&
      static_cast<unsigned char>(parser.text[parser.pos]) == 0xEFu) {
    return parser.fail("不接受 BOM（请存成无 BOM 的 UTF-8）");
  }
  if (!parser.parse_value(out, 0)) return false;
  if (!out->is_object() && !out->is_array()) {
    parser.pos = 0;
    return parser.fail("顶层必须是对象或数组，不能是标量");
  }
  parser.skip_ws();
  if (parser.pos != parser.text.size()) return parser.fail("值的后面还有内容（多余的一个字符？）");
  return true;
}

}  // namespace l7gate::config
