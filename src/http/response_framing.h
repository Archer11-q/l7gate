#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "http/request_parser.h"

// 响应侧的框架知识（S1-06）。
//
// 为什么它和 request_parser 一样必须是纯函数族：判定"这条响应完结没有"如果和
// socket 状态混在一起，测出来的失败永远分不清"框架判错了"还是"时序没对上"——
// S1-03 用同一个理由换来了 21 条不建 socket 的断言，这里照抄那条经验。
//
// 分工边界（与 ARCHITECTURE 1.4 第二条同口径）：这里只做**校验 + 定位**，
// 永远不解码框架。chunked 的字节一字节都不动地透传下去，`ChunkedProgress`
// 只是数"框架边界走到哪了"，它不重组 body。重写框架的代价在 S1-03 已经付过
// 一次学费：「与直连上游逐字节一致」这条验收就永远证不了。

namespace l7gate::http {

// --- 响应头判定 -----------------------------------------------------------

// 完整响应头（终止串 CRLFCRLF 已出现）的框架结论。
// 半包**不是错误**，也不做任何早期判定：状态行即使语法可疑，在头没到齐之前
// 一律回 kHeadIncomplete——这条和请求侧的全称前缀断言是同一条哲学，
// 判早了的代价是网关拿正常的慢上游当坏上游处理。
enum class ResponseFraming : std::uint8_t {
  kHeadIncomplete,    // 头还没到齐：唯一的"无结论"状态
  kHeadInvalid,       // 头齐了但语法/框架声明不可接受（坏状态行、CL+TE 并存、多个不一致 CL、非法 TE）
  kProvisional,       // 1xx（101 之外）：本条无 body，后面还会跟**另一个**响应头
  kNoBody,            // 204/304：按定义无 body，头结束即响应完结
  kContentLength,     // CL：declared_body_bytes 有效
  kChunked,           // Transfer-Encoding: chunked（恰好一个记号）
  kCloseDelimited,    // 无 CL 无 TE 的其余响应：body 以连接关闭定界（EOF **就是**完结）
};

struct ResponseHeadInfo {
  ResponseFraming framing = ResponseFraming::kHeadIncomplete;
  unsigned status_code = 0;          // 仅当 framing != kHeadIncomplete 有意义
  std::size_t head_bytes = 0;        // 含终止串 CRLFCRLF 的完整头长度（Invalid 时也给出，便于观测）
  std::size_t declared_body_bytes = 0;  // 仅 kContentLength
};

// data/len 指向**可读区起点**的那段连续字节；头前有别的字节（未消费干净）时
// 结果不可信——调用方（Reactor）保证只在"一条响应的开头"调用。
ResponseHeadInfo analyze_response_head(const std::uint8_t* data, std::size_t len) noexcept;

// 便于测试的字符串口径。
inline ResponseHeadInfo analyze_response_head(std::string_view text) noexcept {
  return analyze_response_head(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

// --- chunked 流的边界游标 --------------------------------------------------
//
// 增量推进：每轮把**新到达且即将透传**的那段字节喂进去，游标停在边界上
// （`advance` 返回本次消费的字节数，遇到完结或非法就停）。它不是解码器：
// 状态只有phase/剩余数/几格累加器，内存是常数，chunk 数据本身一字节不碰。

struct ChunkedProgress {
  enum class Phase : std::uint8_t { kSizeLine, kData, kDataCrlf, kTrailers, kComplete, kInvalid };
  Phase phase = Phase::kSizeLine;
  std::size_t remaining = 0;   // kData：本 chunk 还剩几字节
  std::size_t crlf_seen = 0;   // kDataCrlf：已见的 CRLF 前缀长度
  std::uint64_t size_acc = 0;  // kSizeLine：hex 累加值（只用来认 0 与溢出）
  bool size_any = false;       // 已至少读到一位 hex
  bool line_saw_cr = false;    // kSizeLine/kTrailers：行内 CR 状态
  std::size_t line_len = 0;    // kTrailers：当前行已见字节数（不含 CR）
  bool ext_mode = false;       // kSizeLine：已见 ';'，chunk-ext 内容不解释，只等行尾
  bool size_overflow = false;
};

// 喂入 data[0..len)，推进游标，返回**消费到的字节数**：
//   返回 len          —— 全部是"还没到边界"的合法框架字节；
//   停在 [0,len)      —— 第 pos 字节起属于**下一条响应**或下一段语义（kComplete）；
//   phase=kInvalid    —— 框架坏了（非法 hex、缺 CRLF、溢出），pos 之前的字节仍已透传，
//                        调用方按"截断"处理：坏框架没有"之后"可言。
std::size_t chunked_advance(ChunkedProgress& p, const std::uint8_t* data, std::size_t len) noexcept;

inline std::size_t chunked_advance(ChunkedProgress& p, std::string_view text) noexcept {
  return chunked_advance(p, reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

// --- 网关自产响应 -----------------------------------------------------------

// ParseError → 网关回给下游的状态码。集合按契约/ARCHITECTURE 第 3 节点名的
// 400/411/413/501 收窄执行：22 个错误码全部有归属，**没有**"未映射即默认 400"
// 这种偷懒兜底——新加错误码时这张表编译不过不了（switch 无 default），
// 逼下一次改动来认账。
// 411 在这张表里存在但当前没有任何错误码映射到它：本解析器把"无 CL 无 TE"
// 判成无 body 而不是"要求长度"，这是 HTTP/1.1 的合法解读，不为凑数字硬造触发路径。
std::uint16_t reply_status_for_parse_error(ParseError error) noexcept;

// 自产响应的完整字节（含 CL 与 `Connection: close`）。支持的入参：400/411/413/501/502。
// 返回的 string_view 指向静态存储，可长期持有。未支持的状态码返回空 view——
// 调用方不许拿空 view 当"发送成功"。
std::string_view gateway_error_reply(std::uint16_t status) noexcept;

}  // namespace l7gate::http
