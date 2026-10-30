#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// 配置文件用的 JSON 子集解析器（S3-02，ADR-19 第二条）。
//
// **为什么不引第三方库**：契约 8.2 禁令三给了那个位置，而我在同一份 ADR 里的结论是不引。
// 真正的理由不是"少一个依赖"，是我要的语义不是"能读 JSON"，是**读不出就整体失败**：
// 重复键、未知字段、尾逗号、注释、顶层标量、类型不对——每一条都必须是拒绝，
// 而不是"忽略掉继续跑"。这条立场的原型是 `max_header_count` 那次（配置写了、代码没读，
// 限制静默失效）与 `--upstream` 那条（配置写错不该退化成少连一个上游）。
// 库可以换，"拒绝"这件事只能我自己表达。
//
// 子集边界（写在这里，不在代码注释里散落）：
//   值：对象 / 数组 / 字符串 / **十进制整数** / `true` / `false` / `null`。
//   不接受：浮点、`1e3`、`+1`、前导零、单引号、注释、尾逗号、顶层标量、BOM。
//   字符串转义只认 `\"` `\\` `\/` `\b` `\f` `\n` `\r` `\t` 与 4 位 `\uXXXX`
//   （`\uXXXX` 只做 BMP 内的 UTF-8 编码，不处理代理对——配置文件里出现代理对
//    的可能性远低于它为真时我要调试的那个假象，所以直接拒绝）。
//   深度上限 32：配置文件是磁盘上来的输入，嵌套深度不能由它决定我的栈用量。
namespace l7gate::config {

class JsonValue {
 public:
  enum class Kind : std::uint8_t { kObject, kArray, kString, kInt, kBool, kNull };

  // 对象的一个成员。**先声明、到类外才定义**：写成 `std::pair<std::string, JsonValue>`
  // 会在 `JsonValue` 自己还不完整的那一刻就实例化 `pair`，而 libstdc++ 的 `pair` 要求两边
  // 都完整——GCC 15 放得过这条、clang 21 放不过。所以 `l7gate_tests` 在 debug（GCC）全绿，
  // 而 asan-ubsan 与 tsan 两个 clang preset 在 `json.cc` / `gateway_config.cc` 上直接编译失败。
  // `std::vector<Member>` 走的是 C++17（P0646）给 vector 开的那条"允许元素类型暂不完整"的路，
  // 递归因此成立，而两个编译器给的是同一个答案。
  struct Member;

  Kind kind = Kind::kNull;
  std::string text;                                   // kString 的解码结果
  std::int64_t number = 0;                            // kInt
  bool boolean = false;                               // kBool
  std::vector<Member> members;                        // kObject，**按文件里的顺序**
  std::vector<JsonValue> items;                       // kArray

  bool is_object() const noexcept { return kind == Kind::kObject; }
  bool is_array() const noexcept { return kind == Kind::kArray; }
  bool is_string() const noexcept { return kind == Kind::kString; }
  bool is_int() const noexcept { return kind == Kind::kInt; }

  // 对象里按键找值。找不到返回 nullptr——**调用方必须处理**，
  // 因为"缺字段"在这套配置里和"字段类型不对"是同一种错误，都要让启动失败。
  const JsonValue* find(std::string_view key) const noexcept;

  // 报错用：把 Kind 写成人类看得懂的词（"对象" / "字符串" / …）。
  const char* kind_name() const noexcept;
};

// 定义在类外：这里 `JsonValue` 已经完整，递归到此闭合。
struct JsonValue::Member {
  std::string key;
  JsonValue value;
};

// 解析整份文档。成功时 out 是顶层值（必须是对象或数组，否则拒绝）；
// 失败时 error 是一句**带位置**的话（第几字节、为什么），调用方直接打给 stderr。
// 不抛异常：`main.cc` 里没有 try/catch，而"配置文件坏了"应该走退出码 2 那条路，
// 不是走一次未捕获异常看起来像网关崩了。
bool parse_json(std::string_view text, JsonValue* out, std::string* error) noexcept;

}  // namespace l7gate::config
