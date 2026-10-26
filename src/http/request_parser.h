#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace l7gate::http {

// HTTP/1.1 请求解析器（S1-03）。
//
// 为什么它是纯函数、而且必须是纯函数：
// ADR-02 选了 edge-triggered，于是"半包"是这个系统的常态而不是异常。半包的判定
// 逻辑如果和 socket 状态混在一起，我就只能用真实连接去测它，测出来的失败永远
// 分不清"解析错了"还是"时序没对上"。所以这里入参只有 (data, len)，不发 syscall、
// 不持有任何跨调用的状态；重复喂同一段字节必须得到完全相同的结果 —— 这条性质
// 让 22 类畸形输入用例能在一个不建 socket 的文件里跑完。
//
// 它的定位是**校验 + 定位**，不是重写：S1-06 的转发要求「与直连上游逐字节一致」，
// 所以我绝不把 chunked 框架解开来重写请求；原始字节留在 Buffer 里整条搬走，
// 解析器只负责回答"这条请求到哪里结束、它声明了多少 body、字段是什么"。
//
// 关于"从头重扫"的代价：纯函数意味着每补一段数据就重扫一遍已收字节。头部被
// 1 字节 1 片 dribble 成 N 片时扫描量是 O(N²)，这是慢速攻击面。缓解手段（记录
// 已扫描游标）属于**连接对象**（S1-04），不塞进解析器 —— 一旦把状态藏进这里，
// 上面那条"可重复喂入"的可测性就没了。S1-03 把这个代价测成数字，见测试
// WorstCaseDribbleScanCostIsMeasured。

enum class ParseStatus : std::uint8_t {
  kOk,         // 收到一条语法完整、语义可接受的请求
  kNeedMoreData,  // 半包：**不是错误**，等更多字节后重喂
  kError,      // 畸形输入，错误码见 ParseError
};

// 错误码。每一个都对应 request_parser_test.cc 里至少一条用例，
// 而且都必须是"可定义的 4xx 来源"（S1-06 之后由转发路径映射成 400/411/413/501）。
enum class ParseError : std::uint16_t {
  kNone = 0,
  kRequestLineMalformed,   // 段数不对 / 请求行为空
  kMethodInvalid,          // 含非 tchar 字符
  kTargetInvalid,          // 不是 origin-form / asterisk-form，或含控制字符与空格
  kVersionUnsupported,     // 不是 HTTP/1.0 也不是 HTTP/1.1（ADR-01：不做 HTTP/2）
  kLineEndingNotCRLF,      // 裸 LF 或孤立 CR（HTTP/1.1 消息语法要求 CRLF）
  kHeaderLineMalformed,    // header 行缺冒号
  kHeaderNameInvalid,      // 名为空或含非 tchar
  kHeaderValueInvalid,     // 值含 NUL / 裸 CR / 裸 LF / 其他控制字符
  kObsFoldRejected,        // 行首 SP/HT 的旧式折行，RFC 9112 要求代理拒绝
  kHeaderTooLarge,         // 单行超 max_header_line_bytes
  kHeadersTooLarge,        // 头部区（含结尾空行）超 max_headers_bytes
  kTooManyHeaders,         // 条数超 scratch 容量
  kContentLengthInvalid,   // 非纯数字、带符号、或溢出
  kContentLengthDuplicated,  // 多个 CL 且值不一致（一致的合并，见 .cc 注释）
  kContentLengthWithTE,    // CL 与 Transfer-Encoding 并存：请求走私的经典入口
  kTransferEncodingInvalid,  // TE 不是"恰好一个 chunked"
  kHostDuplicated,         // 多个 Host 头
  kHostMissing,            // HTTP/1.1 缺 Host
  kChunkSizeInvalid,       // chunk-size 非合法十六进制或溢出
  kChunkTerminatorMissing, // chunk-size 行 / chunk 数据 / last-chunk 结尾不是 CRLF
  kBodyTooLarge,           // 声明的 body 超 max_body_bytes
  kNotImplemented,         // 语法合法但不在本项目范围内（如 CONNECT 的 authority-form）
};

const char* to_string(ParseError error) noexcept;

enum class BodyKind : std::uint8_t {
  kNone,          // 既没有 CL 也没有 chunked：0 长度 body
  kContentLength,
  kChunked,
};

// header 的一条。name 保留原始大小写（HTTP 字段名大小写不敏感，但转发时要原样还回去），
// value 已去掉两端的 OWS（SP / HTAB）。
struct HeaderEntry {
  std::string_view name;
  std::string_view value;
};

// header 条数上限的默认值。它是 ParseScratch 的默认容量，不是 ParserConfig 的字段
// —— 见下面 ParseScratch 的注释，同一个限制只留一个真相来源。
// 64 条的依据：S1-03 的样本请求（curl 默认 + 少量自定义头）用掉 3-5 条，
// 64 留了十几倍余量，同时把最坏情况的每连接 scratch 压在 2 KiB（64 × 32 字节）。
// 这个数字如果被真实流量打脸，改法和随之而来的 ADR 一起处理，不在 S1 猜。
inline constexpr std::size_t kDefaultMaxHeaderCount = 64;

// 头部条数的载体。解析器是纯函数，所以这块内存由调用方持有、按连接复用：
// 每连接一次分配，而不是每请求一次。它的成本要算进 SLO #2 的每连接内存口径
// （测试 ParseScratchMemoryFootprintIsBounded 会把数字打出来）。
//
// **容量就是"最多允许多少条 header"这条限制本身**，只有这一个真相来源。
// 我原来在 ParserConfig 里也放了一个 max_header_count，结果两个旋钮各说各话：
// 用例给 scratch 分配 64 槽、config 写 8，第 9 条 header 照样通过 —— 配置里那个
// 数字从来没被读过，限制静默失效。现在 config 不再重复这条，见 kDefaultMaxHeaderCount。
class ParseScratch {
 public:
  explicit ParseScratch(std::size_t max_header_count = kDefaultMaxHeaderCount);

  std::size_t max_header_count() const noexcept { return slots_.size(); }
  std::span<HeaderEntry> slots() noexcept { return slots_; }

 private:
  std::vector<HeaderEntry> slots_;
};

struct ParserConfig {
  // 单条 header 行的字节上限（含 CRLF）。对齐 nginx large_client_header_buffers 的量级。
  std::size_t max_header_line_bytes = 8u << 10;    // 8 KiB
  // 整个头部区（请求行 + 所有 header + 结尾空行）的上限。
  std::size_t max_headers_bytes = 64u << 10;       // 64 KiB
  // 单条请求 body 的上限（CL 声明值或 chunked 之和）。超过它是 kBodyTooLarge，
  // 而不是无限等 —— S1-07 的背压管的是"对端太快"，这条管的是"对端太大"。
  std::size_t max_body_bytes = 8u << 20;           // 8 MiB
};

// 解析结果。字段里的 string_view 全部指向**调用方传入的那段字节**，
// 因此它们只在底层内存不动的前提下有效：Buffer 一旦压缩（memmove）或扩容，
// 这些 view 就作废。调用方的正确顺序是「解析 → 需要就当场取用/拷贝 → 再 consume」，
// 而不是「解析 → 先 consume → 之后回头读 view」。这条约束归 S1-04 的连接对象遵守。
struct ParsedRequest {
  std::string_view method;
  std::string_view target;
  std::string_view version;
  std::span<const HeaderEntry> headers;

  std::size_t headers_bytes = 0;   // 请求行 + header 行 + 结尾空行
  std::size_t consumed = 0;        // 整条请求（含 body）的原始字节数

  BodyKind body_kind = BodyKind::kNone;
  std::size_t declared_body_bytes = 0;  // CL 值；chunked 时为各 chunk 之和（解码后长度）
  std::size_t body_raw_bytes = 0;       // body 区占用的**原始**字节（chunked 含框架开销）

  // CL 场景下可直接给出的原始 body；chunked 时为空 —— 数据不连续，我不做解码。
  std::string_view raw_body;

  bool http_1_1 = false;

  // 字段名按大小写不敏感查（HTTP 语义），返回第一次命中的值。
  std::optional<std::string_view> find_header(std::string_view name) const noexcept;
};

// 三态返回值。错误码单独带出来而不是塞进 ParsedRequest，是因为 S1-06 要按它决定
// 回什么状态码（400 / 411 / 413 / 501），而那时 out 里的字段是不可信的。
struct ParseOutcome {
  ParseStatus status = ParseStatus::kNeedMoreData;
  ParseError error = ParseError::kNone;
};

// 解析器的唯一入口。
//   data/len —— 当前已收到的**全部**连续字节（真实调用方传 Buffer 的 readable 区）
//   scratch   —— header 槽位，由调用方按连接持有
//   out       —— 仅当返回 kOk 时有效；其余情况内容不保证，调用方不得读取
// 返回 kOk 时 out.consumed 是这条请求的总字节数；剩余的 len-consumed 字节属于
// 下一条请求（粘包），调用方据此再次调用本函数。
ParseOutcome parse_request(const std::uint8_t* data, std::size_t len,
                           ParseScratch& scratch, const ParserConfig& config,
                           ParsedRequest& out) noexcept;

// 便于测试与 S1-04 复用：字符串口径的重载（不含结尾 '\0'）。
inline ParseOutcome parse_request(std::string_view text, ParseScratch& scratch,
                                  const ParserConfig& config, ParsedRequest& out) noexcept {
  return parse_request(reinterpret_cast<const std::uint8_t*>(text.data()), text.size(),
                       scratch, config, out);
}

// 同一口径的 (指针, 长度) 重载，省掉调用方到处写 reinterpret_cast。
// 副作用是 `parse_request(nullptr, 0, ...)` 在两个指针重载之间歧义，想测空指针
// 得显式 cast 成 const std::uint8_t* —— request_parser_test.cc 里就是这么写的。
inline ParseOutcome parse_request(const char* data, std::size_t len, ParseScratch& scratch,
                                  const ParserConfig& config, ParsedRequest& out) noexcept {
  return parse_request(reinterpret_cast<const std::uint8_t*>(data), len, scratch, config, out);
}

}  // namespace l7gate::http
