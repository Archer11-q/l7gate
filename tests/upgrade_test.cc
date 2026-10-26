// S1-05：Upgrade 透传的最小识别面（纯函数部分）。
//
// 分工：`response_head` 只回答"这是不是一个完整的 101 响应头"，所以它的测试
// 全部不碰 fd、不建连接——和 S1-03 解析器同一个理由：判定逻辑与 socket 状态
// 分开，失败才只可能是判定本身错，不会是时序没对上。
// 连接层的游标与短路在 connection_test.cc（S1_05_Connection），走真 socket 的
// 握手闭环、透传不改写、close 兑现与 negative 组合在 reactor_test.cc
// （S1_05_Reactor）。三个文件合起来覆盖验收句的每一个断言词。
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "http/response_head.h"

namespace {

using l7gate::http::find_header_terminator;
using l7gate::http::looks_like_101_response_head;

const std::uint8_t* bytes(std::string_view text) {
  return reinterpret_cast<const std::uint8_t*>(text.data());
}

constexpr std::string_view kRealHead =
    "HTTP/1.1 101 Switching Protocols\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "\r\n";

TEST(S1_05_ResponseHead, FindsTerminatorOnlyWhenFullyPresent) {
  EXPECT_EQ(find_header_terminator(bytes("GET /a HTTP/1.1\r\n"), 17), std::string_view::npos);
  // 跨界形态：终止串被劈成 "…\r\n" + "\r\n…"，from 落在中间也必须找到。
  const std::string_view a = "abcdef\r\n\r\nxyz";
  EXPECT_EQ(find_header_terminator(bytes(a), a.size()), 6u);
  EXPECT_EQ(find_header_terminator(bytes(a), a.size(), 8), std::string_view::npos);
  // "\r\n\r" + "\n…"：三个字节的前缀不算终止串。
  const std::string_view b = "q\r\n\r";
  EXPECT_EQ(find_header_terminator(bytes(b), b.size()), std::string_view::npos);
  const std::string_view c = "q\r\n\n";
  EXPECT_EQ(find_header_terminator(bytes(c), c.size()), std::string_view::npos);
}

TEST(S1_05_ResponseHead, AcceptsExactlyAComplete101Head) {
  EXPECT_TRUE(looks_like_101_response_head(bytes(kRealHead), kRealHead.size()));
  // 最短形态与两个版本的写法都成立（前缀比较只钉 "HTTP/1."）。
  EXPECT_TRUE(looks_like_101_response_head(bytes("HTTP/1.1 101\r\n\r\n"), 16));
  EXPECT_TRUE(looks_like_101_response_head(bytes("HTTP/1.0 101\r\n\r\n"), 16));
}

TEST(S1_05_ResponseHead, EveryRealPrefixOfTheHeadIsNeedMoreNotA101) {
  // 半包不是错误 —— S1-03 给请求侧写的那条全称断言，在响应侧同样成立：
  // 101 头被 TCP 切成任意两段到达是常态，任何一段被判成"确定不是 101"都不可恢复。
  // 这里断言的是更弱也更需要的一条：**前缀永远返回 false**（未判定），
  // 而不是某个前缀返回 true（在字节没到齐时就宣布握手成立）。
  for (std::size_t i = 0; i < kRealHead.size(); ++i) {
    EXPECT_FALSE(looks_like_101_response_head(bytes(kRealHead), i))
        << "长度 " << i << " 的前缀不该被当成完整头判定";
  }
  EXPECT_TRUE(looks_like_101_response_head(bytes(kRealHead), kRealHead.size()));
}

TEST(S1_05_ResponseHead, NearMissesAreAllRejected) {
  const char* kSamples[] = {
      "HTTP/1.1 200 OK\r\n\r\n",          // 正常响应
      "HTTP/1.1 102 Processing\r\n\r\n",  // 最接近的邻居
      "HTTP/1.1 1015 x\r\n\r\n",          // 状态码四位：第 12 字节不是终止符
      "HTTP/1.1 10x\r\n\r\n",             // 状态码位上不是数字
      "HTTP/2 101\r\n\r\n",               // ADR-01：没有 HTTP/2 的文本口径
      "http/1.1 101\r\n\r\n",             // 小写版本前缀（响应状态行不存在这种合法形态）
      "HTTP/1.1x101\r\n\r\n",             // 第 8 字节不是 SP
      "GET /a HTTP/1.1\r\n\r\n",          // 请求伪装成响应头
      "HTTP/1.1 101\r\n\r\nEXTRA",        // 头后还有字节：这不是"头部区的边界"
      "HTTP/1.1 101\r\n",                 // 裸 CRLF 收尾，终止串没到齐
      "",                                 // 空
      "HTTP/1.1 101\r\n\r\n\r\n",         // 终止串之后又一段：len 不再是头的末端
  };
  for (const char* sample : kSamples) {
    const std::string_view text(sample);
    EXPECT_FALSE(looks_like_101_response_head(bytes(text), text.size()))
        << "样本被误判成 101: [" << text << "]";
  }
}

TEST(S1_05_ResponseHead, NullPointerIsNeverA101Head) {
  EXPECT_EQ(find_header_terminator(nullptr, 0), std::string_view::npos);
  EXPECT_FALSE(looks_like_101_response_head(nullptr, 0));
  // 非空指针但长度不足（UBSan 的敏感区：不能越过 len 去读）。
  const char tiny[] = "H\r\n";
  EXPECT_FALSE(looks_like_101_response_head(bytes(tiny), 3));
}

}  // namespace
