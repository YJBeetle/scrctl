#include "jsonlite/Jsonlite.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace scrctl::json {
namespace {

constexpr int kMaxDepth = 64;
constexpr size_t kMaxInput = 4u << 20;

void append_utf8(std::string &out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

struct Parser {
    std::string_view s;
    size_t i = 0;
    std::string err;

    bool fail(std::string m) {
        if (err.empty()) {
            err = std::move(m);
        }
        return false;
    }

    void skip_ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
            ++i;
        }
    }

    bool eat(char c) {
        skip_ws();
        if (i < s.size() && s[i] == c) {
            ++i;
            return true;
        }
        return false;
    }

    bool parse_value(int depth, Value &out) {
        if (depth > kMaxDepth) {
            return fail("嵌套过深");
        }
        skip_ws();
        if (i >= s.size()) {
            return fail("意外结束");
        }
        const char c = s[i];
        if (c == '{') {
            return parse_object(depth, out);
        }
        if (c == '[') {
            return parse_array(depth, out);
        }
        if (c == '"') {
            out.kind = Kind::String;
            return parse_string(out.string);
        }
        if (c == 't' || c == 'f' || c == 'n') {
            return parse_literal(out);
        }
        return parse_number(out);
    }

    bool parse_string(std::string &out) {
        if (!eat('"')) {
            return fail("期望 \"");
        }
        out.clear();
        while (i < s.size()) {
            const char c = s[i++];
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (i >= s.size()) {
                break;
            }
            const char e = s[i++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    uint32_t cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        if (i >= s.size()) {
                            return fail("\\u 不完整");
                        }
                        const char h = s[i++];
                        uint32_t d;
                        if (h >= '0' && h <= '9') {
                            d = static_cast<uint32_t>(h - '0');
                        } else if (h >= 'a' && h <= 'f') {
                            d = static_cast<uint32_t>(h - 'a' + 10);
                        } else if (h >= 'A' && h <= 'F') {
                            d = static_cast<uint32_t>(h - 'A' + 10);
                        } else {
                            return fail("\\u 含非法字符");
                        }
                        cp = cp * 16 + d;
                    }
                    // 代理对：高代理后必须紧跟 \uDCxx，否则按替换字符处理。
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                            i += 2;
                            uint32_t low = 0;
                            for (int k = 0; k < 4; ++k) {
                                if (i >= s.size()) {
                                    return fail("低代理不完整");
                                }
                                const char h = s[i++];
                                uint32_t d;
                                if (h >= '0' && h <= '9') {
                                    d = static_cast<uint32_t>(h - '0');
                                } else if (h >= 'a' && h <= 'f') {
                                    d = static_cast<uint32_t>(h - 'a' + 10);
                                } else if (h >= 'A' && h <= 'F') {
                                    d = static_cast<uint32_t>(h - 'A' + 10);
                                } else {
                                    return fail("低代理含非法字符");
                                }
                                low = low * 16 + d;
                            }
                            if (low >= 0xDC00 && low <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            } else {
                                cp = 0xFFFD;
                            }
                        } else {
                            cp = 0xFFFD;
                        }
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: return fail("非法转义");
            }
        }
        return fail("字符串未闭合");
    }

    bool parse_number(Value &out) {
        const size_t start = i;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
            ++i;
        }
        bool is_real = false;
        while (i < s.size()) {
            const char c = s[i];
            if (c >= '0' && c <= '9') {
                ++i;
            } else if (c == '.' || c == 'e' || c == 'E' || c == '-' || c == '+') {
                is_real = is_real || c == '.' || c == 'e' || c == 'E';
                ++i;
            } else {
                break;
            }
        }
        if (i == start) {
            return fail("期望数值");
        }
        const std::string tok(s.substr(start, i - start));
        try {
            if (is_real) {
                out.kind = Kind::Double;
                out.real = std::stod(tok);
            } else {
                out.kind = Kind::Int;
                out.integer = static_cast<int64_t>(std::stoll(tok));
            }
        } catch (const std::exception &) {
            return fail("数值非法: " + tok);
        }
        return true;
    }

    bool parse_literal(Value &out) {
        if (s.substr(i).starts_with("true")) {
            i += 4;
            out = Value();
            out.kind = Kind::Bool;
            out.boolean = true;
            return true;
        }
        if (s.substr(i).starts_with("false")) {
            i += 5;
            out = Value();
            out.kind = Kind::Bool;
            return true;
        }
        if (s.substr(i).starts_with("null")) {
            i += 4;
            out = Value();
            return true;
        }
        return fail("非法字面量");
    }

    bool parse_array(int depth, Value &out) {
        ++i;  // '['
        out.kind = Kind::Array_;
        if (eat(']')) {
            return true;
        }
        for (;;) {
            Value child;
            if (!parse_value(depth + 1, child)) {
                return false;
            }
            out.array.push_back(std::move(child));
            if (eat(',')) {
                continue;
            }
            if (eat(']')) {
                return true;
            }
            return fail("数组缺 , 或 ]");
        }
    }

    bool parse_object(int depth, Value &out) {
        ++i;  // '{'
        out.kind = Kind::Object_;
        if (eat('}')) {
            return true;
        }
        for (;;) {
            skip_ws();
            std::string key;
            if (!parse_string(key)) {
                return false;
            }
            if (!eat(':')) {
                return fail("object 键后缺 :");
            }
            Value child;
            if (!parse_value(depth + 1, child)) {
                return false;
            }
            out.object[key] = std::move(child);
            if (eat(',')) {
                continue;
            }
            if (eat('}')) {
                return true;
            }
            return fail("object 缺 , 或 }");
        }
    }
};

void escape_string(std::string &out, std::string_view in) {
    out.push_back('"');
    for (const char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

}  // namespace

const Value *Value::find(std::string_view key) const {
    if (kind != Kind::Object_) {
        return nullptr;
    }
    const auto it = object.find(key);
    return it == object.end() ? nullptr : &it->second;
}

std::string Value::as_string_or(std::string_view fallback) const {
    return kind == Kind::String ? string : std::string(fallback);
}

int64_t Value::as_int_or(int64_t fallback) const {
    if (kind == Kind::Int) {
        return integer;
    }
    if (kind == Kind::Double) {
        return static_cast<int64_t>(real);
    }
    return fallback;
}

std::optional<Value> parse(std::string_view text, std::string *err) {
    if (text.size() > kMaxInput) {
        if (err != nullptr) {
            *err = "输入过大";
        }
        return std::nullopt;
    }
    Parser p{text, 0, {}};
    Value root;
    if (!p.parse_value(0, root)) {
        if (err != nullptr) {
            *err = p.err;
        }
        return std::nullopt;
    }
    p.skip_ws();
    if (p.i != p.s.size()) {
        if (err != nullptr) {
            *err = "尾部有多余内容";
        }
        return std::nullopt;
    }
    return root;
}

std::string write(const Value &v) {
    std::string out;
    switch (v.kind) {
        case Kind::Null: out = "null"; break;
        case Kind::Bool: out = v.boolean ? "true" : "false"; break;
        case Kind::Int: out = std::to_string(v.integer); break;
        case Kind::Double: {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.17g", v.real);
            out = buf;
            break;
        }
        case Kind::String: escape_string(out, v.string); break;
        case Kind::Array_: {
            out.push_back('[');
            for (size_t k = 0; k < v.array.size(); ++k) {
                if (k) {
                    out.push_back(',');
                }
                out += write(v.array[k]);
            }
            out.push_back(']');
            break;
        }
        case Kind::Object_: {
            out.push_back('{');
            bool first = true;
            for (const auto &[k, val] : v.object) {
                if (!first) {
                    out.push_back(',');
                }
                first = false;
                escape_string(out, k);
                out.push_back(':');
                out += write(val);
            }
            out.push_back('}');
            break;
        }
    }
    return out;
}

}  // namespace scrctl::json
