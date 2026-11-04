#pragma once

// S5-04 的握手与回帧原语：按 `Sec-WebSocket-Key` 算 accept，把一条掩码帧解成载荷、
// 再组成一条未掩码帧吐回去。
//
// 为什么这两件事在测试夹具这一侧而不是网关里：ADR-09 把网关裁成"101 之后不透明双向透传"，
// 而真浏览器要求**服务端回帧必须未掩码**（实测：accept 算对但把客户端帧原样吐回时，
// Chrome 的 `onmessage` 一次都不触发，直接 `error` → `closed code=1006`）。
// 于是"能和浏览器对话的对端"只能在上游那一侧——真实拓扑本来就是这样，WebSocket 的端点
// 是应用，不是代理。四条实测的来路与代价记在 `docs/adr/ADR-26-...md`。
//
// SHA-1 与 base64 都是自带的：不引 OpenSSL（它不在契约 9.1 的版本锁定表里，动它是禁令三），
// 也不引任何第三方库（ADR-04 的同一条立场）。这里的实现只求正确、可读，不求快——它是仪器。
//
// 默认关闭：只有 `--ws-echo` 显式开启才走这条路。S1-05/S2-05 那批"对端越笨、透传越无处藏"
// 的断言继续打在裸回显档上，两档不可互换。

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>

namespace l7gate::testing {

// --- SHA-1（RFC 3174）与 base64（RFC 4648 标准表，无换行） ---------------------

inline void sha1(std::string_view msg, std::uint8_t out[20]) {
  std::uint32_t h0 = 0x67452301u, h1 = 0xEFCDAB89u, h2 = 0x98BADCFEu, h3 = 0x10325476u,
                h4 = 0xC3D2E1F0u;

  std::string data(msg);
  const std::uint64_t bit_len = static_cast<std::uint64_t>(data.size()) * 8u;
  data.push_back(static_cast<char>(0x80));
  while (data.size() % 64u != 56u) data.push_back('\0');
  for (int i = 7; i >= 0; --i) {
    data.push_back(static_cast<char>((bit_len >> (i * 8)) & 0xFFu));
  }

  auto rol = [](std::uint32_t v, int bits) {
    return (v << bits) | (v >> (32 - bits));
  };

  for (std::size_t chunk = 0; chunk < data.size(); chunk += 64u) {
    std::uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
      const std::size_t p = chunk + static_cast<std::size_t>(i) * 4u;
      w[i] = (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[p])) << 24) |
             (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[p + 1])) << 16) |
             (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[p + 2])) << 8) |
             (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[p + 3])));
    }
    for (int i = 16; i < 80; ++i) {
      w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    std::uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
    for (int i = 0; i < 80; ++i) {
      std::uint32_t f = 0, k = 0;
      if (i < 20) {
        f = (b & c) | (~b & d);
        k = 0x5A827999u;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1u;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDCu;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6u;
      }
      const std::uint32_t tmp = rol(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rol(b, 30);
      b = a;
      a = tmp;
    }
    h0 += a;
    h1 += b;
    h2 += c;
    h3 += d;
    h4 += e;
  }

  const std::uint32_t five[5] = {h0, h1, h2, h3, h4};
  for (int i = 0; i < 5; ++i) {
    out[i * 4 + 0] = static_cast<std::uint8_t>(five[i] >> 24);
    out[i * 4 + 1] = static_cast<std::uint8_t>(five[i] >> 16);
    out[i * 4 + 2] = static_cast<std::uint8_t>(five[i] >> 8);
    out[i * 4 + 3] = static_cast<std::uint8_t>(five[i]);
  }
}

inline std::string base64_encode(const std::uint8_t* data, std::size_t len) {
  static constexpr char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((len + 2u) / 3u) * 4u);
  for (std::size_t i = 0; i < len; i += 3u) {
    const std::uint32_t n = (static_cast<std::uint32_t>(data[i]) << 16) |
                            (i + 1 < len ? static_cast<std::uint32_t>(data[i + 1]) << 8 : 0u) |
                            (i + 2 < len ? static_cast<std::uint32_t>(data[i + 2]) : 0u);
    out.push_back(kTable[(n >> 18) & 0x3Fu]);
    out.push_back(kTable[(n >> 12) & 0x3Fu]);
    out.push_back(i + 1 < len ? kTable[(n >> 6) & 0x3Fu] : '=');
    out.push_back(i + 2 < len ? kTable[n & 0x3Fu] : '=');
  }
  return out;
}

// RFC 6455 §1.3 的那条握手校验值：SHA1(key + GUID) 的 base64。
// 键值两端可能有空白（HTTP 折行），先裁掉——多算一个空格就是另一个摘要，浏览器照样拒。
inline std::string websocket_accept(std::string_view key) {
  static constexpr std::string_view kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  std::size_t b = 0, e = key.size();
  while (b < e && (key[b] == ' ' || key[b] == '\t')) ++b;
  while (e > b && (key[e - 1] == ' ' || key[e - 1] == '\t')) --e;
  std::string joined(key.substr(b, e - b));
  joined.append(kGuid.data(), kGuid.size());
  std::uint8_t digest[20];
  sha1(joined, digest);
  return base64_encode(digest, sizeof(digest));
}

// --- 帧：解一条客户端帧，组一条服务端帧 ---------------------------------------

enum class WsTake {
  kComplete,   // 取出了一整帧（out 里可能空——那是"消费掉但不回话"，例如 pong）
  kNeedMore,   // 字节还不齐：pending 原样留着，下一次事件再看
  kMalformed,  // 不按规矩来（未掩码的客户端帧、超长声明、分片帧）——对端该被摘掉
  kClosed,     // 收到 close 帧：out 里是回给它的 close 帧，回完就该收尾
};

inline void append_server_frame(std::string* out, std::uint8_t opcode, std::string_view payload) {
  out->push_back(static_cast<char>(0x80u | opcode));  // FIN=1，服务端不带掩码
  const std::size_t n = payload.size();
  if (n < 126u) {
    out->push_back(static_cast<char>(n));
  } else if (n <= 0xFFFFu) {
    out->push_back(static_cast<char>(126));
    out->push_back(static_cast<char>((n >> 8) & 0xFFu));
    out->push_back(static_cast<char>(n & 0xFFu));
  } else {
    out->push_back(static_cast<char>(127));
    for (int i = 7; i >= 0; --i) out->push_back(static_cast<char>((n >> (i * 8)) & 0xFFu));
  }
  out->append(payload.data(), payload.size());
}

// 上限是给仪器自己的一道闸：这条夹具只服务演示页，不该去接一个声明 4 GiB 的帧。
inline constexpr std::size_t kWsMaxPayload = 1u << 20;

inline WsTake take_client_frame(const std::string& in, std::size_t* consumed, std::string* out) {
  *consumed = 0;
  out->clear();
  if (in.size() < 2u) return WsTake::kNeedMore;
  const auto b0 = static_cast<std::uint8_t>(in[0]);
  const auto b1 = static_cast<std::uint8_t>(in[1]);
  const bool fin = (b0 & 0x80u) != 0;
  const std::uint8_t opcode = b0 & 0x0Fu;
  const bool masked = (b1 & 0x80u) != 0;
  std::size_t len = b1 & 0x7Fu;
  std::size_t header = 2u;

  if (len == 126u) {
    if (in.size() < header + 2u) return WsTake::kNeedMore;
    len = (static_cast<std::size_t>(static_cast<std::uint8_t>(in[header])) << 8) |
          static_cast<std::size_t>(static_cast<std::uint8_t>(in[header + 1]));
    header += 2u;
  } else if (len == 127u) {
    if (in.size() < header + 8u) return WsTake::kNeedMore;
    std::uint64_t wide = 0;
    for (std::size_t i = 0; i < 8u; ++i) {
      wide = (wide << 8) | static_cast<std::uint64_t>(static_cast<std::uint8_t>(in[header + i]));
    }
    if (wide > kWsMaxPayload) return WsTake::kMalformed;
    len = static_cast<std::size_t>(wide);
    header += 8u;
  }
  if (!masked) return WsTake::kMalformed;  // RFC 6455：客户端帧必须带掩码
  if (!fin || (opcode != 0x1u && opcode != 0x2u && opcode != 0x8u && opcode != 0x9u &&
               opcode != 0xAu)) {
    return WsTake::kMalformed;  // 分片与其他操作码：演示页用不到，接了就是把仪器做成协议栈
  }
  if (len > kWsMaxPayload) return WsTake::kMalformed;
  if (in.size() < header + 4u + len) return WsTake::kNeedMore;

  const std::string payload = [&] {
    std::string p = in.substr(header + 4u, len);
    for (std::size_t i = 0; i < p.size(); ++i) {
      p[i] = static_cast<char>(static_cast<std::uint8_t>(p[i]) ^
                               static_cast<std::uint8_t>(in[header + i % 4u]));
    }
    return p;
  }();
  *consumed = header + 4u + len;

  switch (opcode) {
    case 0x1u:  // text
    case 0x2u:  // binary —— 载荷逐字节原样，不改内容、不加前缀（退化 W1 打的就是这一格）
      append_server_frame(out, opcode, payload);
      return WsTake::kComplete;
    case 0x8u:  // close：回一条 close（载荷就是它那 2 字节状态码 + 原因）再收尾
      append_server_frame(out, 0x8u, payload);
      return WsTake::kClosed;
    case 0x9u:  // ping → pong，载荷照抄
      append_server_frame(out, 0xAu, payload);
      return WsTake::kComplete;
    default:    // 0xA pong：消费掉但不回话
      return WsTake::kComplete;
  }
}

}  // namespace l7gate::testing
