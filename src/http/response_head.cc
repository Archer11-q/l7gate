#include "http/response_head.h"

namespace l7gate::http {

namespace {
constexpr std::uint8_t kCR = 0x0D;
constexpr std::uint8_t kLF = 0x0A;
}  // namespace

std::size_t find_header_terminator(const std::uint8_t* data, std::size_t len,
                                   std::size_t from) noexcept {
  if (data == nullptr || len < 4) return std::string_view::npos;
  if (from + 4 > len) return std::string_view::npos;
  for (std::size_t i = from; i + 4 <= len; ++i) {
    if (data[i] == kCR && data[i + 1] == kLF && data[i + 2] == kCR && data[i + 3] == kLF) {
      return i;
    }
  }
  return std::string_view::npos;
}

bool looks_like_101_response_head(const std::uint8_t* data, std::size_t len) noexcept {
  // 最短合法 101 头 "HTTP/1.1 101\r\n\r\n" 是 16 字节；比较用到的最大下标是 12
  // （状态码后那一格），它被 16 的下限天然覆盖，不用单独判越界。
  if (data == nullptr || len < 16) return false;
  if (find_header_terminator(data, len) != len - 4) {
    return false;  // 终止串必须恰好收尾：len 就是"头部区完整"的那个边界
  }
  // 版本前缀：H T T P / 1 . <digit> <SP>。HTTP/2 走的是另一套编码，不在这套
  // 文本口径里（ADR-01），所以第 6 字节必须是 '1'。
  if (data[0] != 'H' || data[1] != 'T' || data[2] != 'T' || data[3] != 'P' || data[4] != '/') {
    return false;
  }
  if (data[5] != '1' || data[6] != '.' || data[8] != ' ') return false;
  // 状态码恰好三位：data[9..11] 是 "101" 之后，data[12] 必须是 reason phrase
  // 的空格或行尾 CR（"HTTP/1.1 101\r\n\r\n" 这种无 reason 的形态）。不查这一字
  // 节，"HTTP/1.1 1015 …" 会被判成 101——四位码不是三位码的前缀合法形态，
  // 1015 也不存在，误判的代价是把一个普通响应头当隧道起点。
  if (data[12] != ' ' && data[12] != '\r') return false;
  return data[9] == '1' && data[10] == '0' && data[11] == '1';
}

}  // namespace l7gate::http
