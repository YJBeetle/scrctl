#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scrctl::json {

/// 限定子集 JSON 值。只需覆盖隧道握手与 RemoteXPC 元数据用到的语法。
///
/// 同 plist 的理由：不引第三方依赖，许可保持干净，且解析的确实是外部输入，
/// 自己实现反而能把深度/长度上限写死。
struct Value;
using Object = std::map<std::string, Value, std::less<>>;
using Array = std::vector<Value>;

enum class Kind { Null, Bool, Double, Int, String, Array_, Object_ };

struct Value {
    Kind kind = Kind::Null;
    bool boolean = false;
    int64_t integer = 0;
    double real = 0;
    std::string string;
    std::vector<Value> array;
    std::map<std::string, Value, std::less<>> object;

    [[nodiscard]] bool is_object() const { return kind == Kind::Object_; }
    [[nodiscard]] bool is_string() const { return kind == Kind::String; }
    [[nodiscard]] bool is_number() const { return kind == Kind::Int || kind == Kind::Double; }

    /// 返回 nullptr 表示键不存在。
    [[nodiscard]] const Value *find(std::string_view key) const;
    [[nodiscard]] std::string as_string_or(std::string_view fallback = {}) const;
    [[nodiscard]] int64_t as_int_or(int64_t fallback = 0) const;
};

/// 解析 JSON 文本。失败返回 nullopt 并给出原因。
std::optional<Value> parse(std::string_view text, std::string *err = nullptr);

/// 序列化成紧凑 JSON。只支持标量与 object/array 的常规组合。
[[nodiscard]] std::string write(const Value &v);

}  // namespace scrctl::json
