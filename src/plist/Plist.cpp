#include "Plist.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace scrctl::plist {
namespace {

constexpr int kMaxDepth = 64;
constexpr size_t kMaxInput = 8u << 20;

// ---------------------------------------------------------------- base64 ----

constexpr std::string_view kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::array<int16_t, 256> b64_table() {
    std::array<int16_t, 256> t{};
    t.fill(-1);
    for (int i = 0; i < 64; ++i) {
        t[static_cast<unsigned char>(kB64[i])] = static_cast<int16_t>(i);
    }
    return t;
}

bool b64_decode(std::string_view in, std::vector<uint8_t> &out) {
    static const auto tbl = b64_table();
    int acc = 0, bits = 0;
    for (char c : in) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') {
            continue;
        }
        const int16_t v = tbl[static_cast<unsigned char>(c)];
        if (v < 0) {
            return false;
        }
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return true;
}

std::string b64_encode(const std::vector<uint8_t> &in) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    while (i + 3 <= in.size()) {
        const uint32_t n = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8) | in[i + 2];
        out += kB64[(n >> 18) & 63];
        out += kB64[(n >> 12) & 63];
        out += kB64[(n >> 6) & 63];
        out += kB64[n & 63];
        i += 3;
    }
    if (const size_t rem = in.size() - i; rem > 0) {
        uint32_t n = uint32_t(in[i]) << 16;
        if (rem == 2) {
            n |= uint32_t(in[i + 1]) << 8;
        }
        out += kB64[(n >> 18) & 63];
        out += kB64[(n >> 12) & 63];
        out += rem == 2 ? kB64[(n >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

// ----------------------------------------------------------------- parse ----

struct Parser {
    std::string_view s;
    size_t i = 0;
    std::string err;

    [[nodiscard]] bool failed() const { return !err.empty(); }

    bool fail(std::string m) {
        err = std::move(m);
        return false;
    }

    [[nodiscard]] bool eof() const { return i >= s.size(); }

    void skip_ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
            ++i;
        }
    }

    bool starts(std::string_view w) const { return s.substr(i).starts_with(w); }

    /// 跳过 <?xml?> 与 <!DOCTYPE> 声明。
    bool skip_prolog() {
        for (;;) {
            skip_ws();
            if (starts("<?")) {
                const auto end = s.find("?>", i);
                if (end == std::string_view::npos) {
                    return fail("XML 声明未闭合");
                }
                i = end + 2;
                continue;
            }
            if (starts("<!")) {
                // DOCTYPE 可能带内部子集 [...]，用 '>' 收尾时要跳过方括号内容。
                size_t j = i + 2;
                bool in_subset = false;
                while (j < s.size()) {
                    const char c = s[j];
                    if (c == '[') {
                        in_subset = true;
                    } else if (c == ']') {
                        in_subset = false;
                    } else if (c == '>' && !in_subset) {
                        break;
                    }
                    ++j;
                }
                if (j >= s.size()) {
                    return fail("DOCTYPE 未闭合");
                }
                i = j + 1;
                continue;
            }
            return true;
        }
    }

    /// 读标签名并吞掉属性，返回是否自闭合。
    bool open_tag(std::string &name, bool &self_closing) {
        if (!starts("<")) {
            return fail("期望 '<'");
        }
        ++i;
        skip_ws();
        const size_t start = i;
        while (i < s.size() && !std::strchr(" \t\n\r/>", s[i])) {
            ++i;
        }
        if (i == start) {
            return fail("标签名为空");
        }
        name = std::string(s.substr(start, i - start));

        // 属性：扫到本标签的 '>' 为止，跳过引号内的 '>'。
        for (;;) {
            if (i >= s.size()) {
                return fail("标签未闭合");
            }
            if (s[i] == '"' || s[i] == '\'') {
                const char q = s[i++];
                while (i < s.size() && s[i] != q) {
                    ++i;
                }
                if (i >= s.size()) {
                    return fail("属性引号未闭合");
                }
                ++i;
                continue;
            }
            if (s[i] == '>') {
                self_closing = false;
                ++i;
                return true;
            }
            if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '>') {
                self_closing = true;
                i += 2;
                return true;
            }
            ++i;
        }
    }

    bool close_tag(std::string_view name) {
        skip_ws();
        if (!starts("</")) {
            return fail("期望 </" + std::string(name) + ">");
        }
        i += 2;
        const size_t start = i;
        while (i < s.size() && s[i] != '>') {
            ++i;
        }
        if (i >= s.size()) {
            return fail("闭合标签未终止");
        }
        if (s.substr(start, i - start) != name) {
            return fail("闭合标签不匹配：" + std::string(name));
        }
        ++i;
        return true;
    }

    /// 读到 next 标签前的文本，并解实体。
    bool text_until(std::string_view next, std::string &out) {
        const auto end = s.find(next, i);
        if (end == std::string_view::npos) {
            return fail("找不到 " + std::string(next));
        }
        out = decode_entities(s.substr(i, end - i));
        i = end;
        return true;
    }

    bool parse_value(int depth, Value &out) {
        if (depth > kMaxDepth) {
            return fail("嵌套过深");
        }
        skip_ws();
        if (!starts("<")) {
            return fail("期望元素起点");
        }
        std::string name;
        bool self_closing = false;
        const size_t before = i;
        if (!open_tag(name, self_closing)) {
            return false;
        }

        if (name == "true" || name == "false") {
            if (!self_closing && !close_tag(name)) {
                return false;
            }
            out = Value::Bool(name == "true");
            return true;
        }
        if (self_closing) {
            if (name == "array") {
                out = Value::Array();
                return true;
            }
            if (name == "dict") {
                out = Value::Dict();
                return true;
            }
            if (name == "string" || name == "data") {
                out.kind = name == "string" ? Kind::String : Kind::Data;
                return true;
            }
            return fail("意外的空元素 " + name);
        }

        if (name == "string") {
            std::string t;
            if (!text_until("</string>", t)) {
                return false;
            }
            i += std::strlen("</string>");
            out = Value::Str(std::move(t));
            return true;
        }
        if (name == "data") {
            std::string t;
            if (!text_until("</data>", t)) {
                return false;
            }
            i += std::strlen("</data>");
            std::vector<uint8_t> d;
            if (!b64_decode(t, d)) {
                return fail("data 不是合法 base64");
            }
            out = Value::OfData(std::move(d));
            return true;
        }
        if (name == "integer" || name == "real") {
            std::string t;
            const std::string_view closer = name == "integer" ? "</integer>" : "</real>";
            if (!text_until(closer, t)) {
                return false;
            }
            i += closer.size();
            // 允许前后空白；integer 允许 0x 前缀（部分 Apple 实现会写）。
            size_t a = t.find_first_not_of(" \t\r\n");
            size_t b = t.find_last_not_of(" \t\r\n");
            if (a == std::string::npos) {
                return fail(name + " 内容为空");
            }
            t = t.substr(a, b - a + 1);
            try {
                if (name == "integer") {
                    out = Value::Int(static_cast<int64_t>(std::stoll(t, nullptr, 0)));
                } else {
                    Value v;
                    v.kind = Kind::Real;
                    v.real = std::stod(t);
                    out = v;
                }
            } catch (const std::exception &) {
                return fail(name + " 数值非法: " + t);
            }
            return true;
        }
        if (name == "array") {
            Value v = Value::Array();
            for (;;) {
                skip_ws();
                if (starts("</array>")) {
                    i += std::strlen("</array>");
                    break;
                }
                Value child;
                if (!parse_value(depth + 1, child)) {
                    return false;
                }
                v.push(std::move(child));
            }
            out = std::move(v);
            return true;
        }
        if (name == "dict") {
            Value v = Value::Dict();
            for (;;) {
                skip_ws();
                if (starts("</dict>")) {
                    i += std::strlen("</dict>");
                    break;
                }
                std::string kname;
                bool ksc = false;
                const size_t kbefore = i;
                if (!open_tag(kname, ksc) || kname != "key") {
                    i = kbefore;
                    return fail("dict 里期望 <key>");
                }
                if (ksc) {
                    return fail("<key> 不能为空");
                }
                std::string key;
                if (!text_until("</key>", key)) {
                    return false;
                }
                i += std::strlen("</key>");
                Value val;
                if (!parse_value(depth + 1, val)) {
                    return false;
                }
                v.set(std::move(key), std::move(val));
            }
            out = std::move(v);
            return true;
        }
        (void)before;
        return fail("不支持的元素 " + name);
    }

    static std::string decode_entities(std::string_view in) {
        std::string out;
        out.reserve(in.size());
        for (size_t j = 0; j < in.size(); ++j) {
            if (in[j] != '&') {
                out.push_back(in[j]);
                continue;
            }
            const auto semi = in.find(';', j);
            if (semi == std::string_view::npos || semi - j > 10) {
                out.push_back(in[j]);
                continue;
            }
            const std::string_view ent = in.substr(j + 1, semi - j - 1);
            auto append_cp = [&](uint32_t cp) {
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
            };
            if (ent == "amp") {
                out.push_back('&');
            } else if (ent == "lt") {
                out.push_back('<');
            } else if (ent == "gt") {
                out.push_back('>');
            } else if (ent == "quot") {
                out.push_back('"');
            } else if (ent == "apos") {
                out.push_back('\'');
            } else if (ent.starts_with("#x") || ent.starts_with("#X")) {
                append_cp(static_cast<uint32_t>(std::stoul(std::string(ent.substr(2)), nullptr, 16)));
            } else if (ent.starts_with("#")) {
                append_cp(static_cast<uint32_t>(std::stoul(std::string(ent.substr(1)))));
            } else {
                out.append(ent.begin(), ent.end());
                out.push_back(';');
            }
            j = semi;
        }
        return out;
    }
};

// ------------------------------------------------------------------ write ---

void escape_into(std::string &out, std::string_view in) {
    for (const char c : in) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            default: out.push_back(c);
        }
    }
}

void write_value(std::string &out, const Value &v, int indent) {
    const std::string pad(static_cast<size_t>(indent) * 4, ' ');
    switch (v.kind) {
        case Kind::Bool:
            out += pad + (v.boolean ? "<true/>\n" : "<false/>\n");
            break;
        case Kind::Int:
            out += pad + "<integer>" + std::to_string(v.integer) + "</integer>\n";
            break;
        case Kind::Real: {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.17g", v.real);
            out += pad + "<real>" + buf + "</real>\n";
            break;
        }
        case Kind::String: {
            out += pad + "<string>";
            escape_into(out, v.string);
            out += "</string>\n";
            break;
        }
        case Kind::Data:
            out += pad + "<data>" + b64_encode(v.data) + "</data>\n";
            break;
        case Kind::Array:
            if (v.array.empty()) {
                out += pad + "<array/>\n";
                return;
            }
            out += pad + "<array>\n";
            for (const auto &c : v.array) {
                write_value(out, c, indent + 1);
            }
            out += pad + "</array>\n";
            break;
        case Kind::Dict:
            if (v.keys.empty()) {
                out += pad + "<dict/>\n";
                return;
            }
            out += pad + "<dict>\n";
            for (size_t k = 0; k < v.keys.size(); ++k) {
                out += pad + "    <key>";
                escape_into(out, v.keys[k]);
                out += "</key>\n";
                write_value(out, v.values[k], indent + 1);
            }
            out += pad + "</dict>\n";
            break;
    }
}

}  // namespace

Value Value::Bool(bool b) {
    Value v;
    v.kind = Kind::Bool;
    v.boolean = b;
    return v;
}
Value Value::Int(int64_t i) {
    Value v;
    v.kind = Kind::Int;
    v.integer = i;
    return v;
}
Value Value::Str(std::string s) {
    Value v;
    v.kind = Kind::String;
    v.string = std::move(s);
    return v;
}
Value Value::OfData(std::vector<uint8_t> d) {
    Value v;
    v.kind = Kind::Data;
    v.data = std::move(d);
    return v;
}
Value Value::Array() {
    Value v;
    v.kind = Kind::Array;
    return v;
}
Value Value::Dict() {
    Value v;
    v.kind = Kind::Dict;
    return v;
}

const Value *Value::find(std::string_view key) const {
    for (size_t i = 0; i < keys.size(); ++i) {
        if (keys[i] == key) {
            return &values[i];
        }
    }
    return nullptr;
}

void Value::set(std::string key, Value v) {
    for (size_t i = 0; i < keys.size(); ++i) {
        if (keys[i] == key) {
            values[i] = std::move(v);
            return;
        }
    }
    keys.push_back(std::move(key));
    values.push_back(std::move(v));
}

void Value::push(Value v) { array.push_back(std::move(v)); }

std::string Value::as_string_or(std::string_view fallback) const {
    return kind == Kind::String ? string : std::string(fallback);
}
int64_t Value::as_int_or(int64_t fallback) const {
    if (kind == Kind::Int) {
        return integer;
    }
    if (kind == Kind::Real) {
        return static_cast<int64_t>(real);
    }
    return fallback;
}
bool Value::as_bool_or(bool fallback) const { return kind == Kind::Bool ? boolean : fallback; }

std::optional<Value> parse(std::string_view xml, std::string *err) {
    if (xml.size() > kMaxInput) {
        if (err != nullptr) {
            *err = "输入过大";
        }
        return std::nullopt;
    }
    Parser p{xml, 0, {}};
    if (!p.skip_prolog()) {
        if (err != nullptr) {
            *err = p.err;
        }
        return std::nullopt;
    }
    p.skip_ws();
    std::string name;
    bool sc = false;
    if (!p.open_tag(name, sc) || name != "plist") {
        if (err != nullptr) {
            *err = "根元素不是 <plist>";
        }
        return std::nullopt;
    }
    Value root;
    if (!p.parse_value(0, root)) {
        if (err != nullptr) {
            *err = p.err;
        }
        return std::nullopt;
    }
    p.skip_ws();
    if (!p.close_tag("plist")) {
        if (err != nullptr) {
            *err = p.err;
        }
        return std::nullopt;
    }
    return root;
}

std::string write(const Value &v) {
    std::string out = R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
)";
    write_value(out, v, 0);
    out += "</plist>\n";
    return out;
}

}  // namespace scrctl::plist
