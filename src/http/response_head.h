#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace l7gate::http {

// S1-05 的响应侧最小识别面。
//
// 为什么这里只有两个函数：Upgrade 透传的判定只需要回答"上游这段字节是不是
// 一个完整的 101 响应头"，而**响应的 HTTP 语义（状态码映射、框架判定、截断
// 检测）全部归 S1-06**。在这里多做任何一件事——比如解 Content-Length、比对
// Upgrade 回显——都是范围扩张，而且会让"S1-06 的逐字节一致"那条验收被这里的
// 半成品语义污染。透传路径的前提恰恰是：识别归识别，改写一律没有。

// 在 data[from..len) 里找头部区终止串 CRLFCRLF，返回终止串**起始处**的下标。
// 找得到终止串的前提是数据连续，所以它天然是半包安全的：没到齐就返回 npos，
// 等更多字节到达后从旧位置重扫即可（隧道判定幂等，重复扫不出错误结论）。
std::size_t find_header_terminator(const std::uint8_t* data, std::size_t len,
                                   std::size_t from = 0) noexcept;

// 判定 data[0..len) 是否是一个**头部区完整**的 101 响应。
// 口径，写死在这里以免将来被"顺手加强"：
//   - data[0..len) 必须恰好是一个响应头（以 CRLFCRLF 结尾，len >= 4）；
//   - 状态行形如 "HTTP/1.x 101"：版本前缀 8 字节、状态码恰好三字节且为 '1','0','1'。
// 合法但非 101 的响应头返回 false；**不完整的头部区同样返回 false**——半包不是
// 错误（S1-03 的全文断言在响应侧同样成立），调用方拿 false 继续等字节就行。
// 本函数不校验 101 之外的一切（不比对 Upgrade/Sec-WebSocket-Accept 回显，
// 不判定状态行其余部分是否合法）：透传不裁决，裁决是 S1-06 的范围。
bool looks_like_101_response_head(const std::uint8_t* data, std::size_t len) noexcept;

}  // namespace l7gate::http
