#include "xpc/XpcValue.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace scrctl::xpc {
namespace {

/// 递归深度上限。设备回的东西是不可信输入，字典套字典的炸弹必须先挡住。
constexpr int kMaxDepth = 64;
/// 单个缓冲区允许的上限，跟 plist / json 模块保持一致。
constexpr std::size_t kMaxBuffer = 8u << 20;
/// 字符串 / 数据的长度字段允许的最大值。
constexpr uint32_t kMaxLen = 4u << 20;

std::string u64_to_string(uint64_t v) {
    if (v == 0) {
        return "0";
    }
    std::string s;
    while (v) {
        s.insert(s.begin(), static_cast<char>('0' + v % 10));
        v /= 10;
    }
    return s;
}

std::string hex(uint32_t v) {
    char buf[11];
    std::snprintf(buf, sizeof(buf), "%08x", v);
    return buf;
}

std::string i64_to_string(int64_t v) {
    // 先按无符号取负，避开 INT64_MIN 取溢不出来。
    const bool neg = v < 0;
    const uint64_t mag = neg ? (~static_cast<uint64_t>(v) + 1) : static_cast<uint64_t>(v);
    std::string s = u64_to_string(mag);
    if (neg) {
        s.insert(s.begin(), '-');
    }
    return s;
}

// ------------------------------------------------------------------ 写入 ------

void put_u32(std::vector<uint8_t> &out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 24));
}

void put_u64(std::vector<uint8_t> &out, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
}

/// 补零到 4 字节边界。`start` 是被对齐那一段的起始下标——Apple 的对齐是相对每段
/// 自己的开头，不是相对整个消息，这一点在嵌套容器里会有可观察的差别。
void align4(std::vector<uint8_t> &out, std::size_t start) {
    const std::size_t used = out.size() - start;
    out.insert(out.end(), (4 - used % 4) % 4, 0);
}

/// 写一个「裸」NUL 结尾字符串：没有长度前缀，只有字典的键用。
void put_cstr(std::vector<uint8_t> &out, const std::string &s) {
    const std::size_t start = out.size();
    out.insert(out.end(), s.begin(), s.end());
    out.push_back(0);
    align4(out, start);
}

bool encode_into(std::vector<uint8_t> &out, const Value &v, int depth) {
    if (depth > kMaxDepth) {
        return false;
    }
    put_u32(out, static_cast<uint32_t>(v.type));
    switch (v.type) {
        case Type::Null:
            return true;
        case Type::Bool:
            put_u32(out, v.boolean ? 1 : 0);
            return true;
        case Type::Int64:
            put_u64(out, static_cast<uint64_t>(v.int64));
            return true;
        case Type::UInt64:
        case Type::Date:
            put_u64(out, v.uint64);
            return true;
        case Type::Double: {
            uint64_t bits = 0;
            std::memcpy(&bits, &v.real, sizeof(bits));
            put_u64(out, bits);
            return true;
        }
        case Type::String: {
            // 长度含终结 NUL，这是 XPC 字符串和 XPC 数据最容易搞混的地方。
            const std::size_t start = out.size();
            put_u32(out, static_cast<uint32_t>(v.string.size() + 1));
            out.insert(out.end(), v.string.begin(), v.string.end());
            out.push_back(0);
            align4(out, start);
            return true;
        }
        case Type::Data: {
            const std::size_t start = out.size();
            put_u32(out, static_cast<uint32_t>(v.data.size()));
            out.insert(out.end(), v.data.begin(), v.data.end());
            align4(out, start);
            return true;
        }
        case Type::Uuid:
            if (v.data.size() != 16) {
                return false;
            }
            out.insert(out.end(), v.data.begin(), v.data.end());
            return true;
        case Type::Array:
        case Type::Dict: {
            // 长度前缀记的是「count 字段 + 全部条目」，所以先把内容编到临时缓冲
            // 里量一下，再拼到外层。
            std::vector<uint8_t> content;
            put_u32(content, static_cast<uint32_t>(v.type == Type::Array ? v.array.size()
                                                                         : v.dict.size()));
            if (v.type == Type::Array) {
                for (const auto &item : v.array) {
                    if (!encode_into(content, item, depth + 1)) {
                        return false;
                    }
                }
            } else {
                for (const auto &entry : v.dict) {
                    put_cstr(content, entry.key);
                    if (!encode_into(content, entry.value, depth + 1)) {
                        return false;
                    }
                }
            }
            put_u32(out, static_cast<uint32_t>(content.size()));
            out.insert(out.end(), content.begin(), content.end());
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------------ 读取 ------

class Reader {
public:
    Reader(const uint8_t *begin, const uint8_t *end) : p_(begin), end_(end) {}

    [[nodiscard]] std::size_t remaining() const { return static_cast<std::size_t>(end_ - p_); }
    [[nodiscard]] const uint8_t *here() const { return p_; }
    [[nodiscard]] const std::string &err() const { return err_; }

    void set_err(const std::string &what) {
        if (err_.empty()) {
            err_ = what;
        }
    }

    bool u32(uint32_t &v) {
        if (remaining() < 4) {
            return fail("读 u32 越界");
        }
        v = static_cast<uint32_t>(p_[0]) | static_cast<uint32_t>(p_[1]) << 8 |
            static_cast<uint32_t>(p_[2]) << 16 | static_cast<uint32_t>(p_[3]) << 24;
        p_ += 4;
        return true;
    }

    bool u64(uint64_t &v) {
        if (remaining() < 8) {
            return fail("读 u64 越界");
        }
        v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= static_cast<uint64_t>(p_[i]) << (8 * i);
        }
        p_ += 8;
        return true;
    }

    bool take(std::size_t n, std::vector<uint8_t> &out) {
        if (remaining() < n) {
            return fail("读定长字节越界");
        }
        out.assign(p_, p_ + n);
        p_ += n;
        return true;
    }

    /// 从 `start` 起算补零到 4 字节边界。
    void align_from(const uint8_t *start) {
        const auto used = static_cast<std::size_t>(p_ - start);
        p_ += (4 - used % 4) % 4;
    }

    /// NUL 结尾串 + 补零。没有长度前缀，只能扫到 NUL，所以必须限制在段内。
    bool cstr(std::string &out) {
        const uint8_t *start = p_;
        const uint8_t *nul = static_cast<const uint8_t *>(std::memchr(p_, 0, remaining()));
        if (nul == nullptr) {
            return fail("键在本段内没有 NUL 终结符");
        }
        out.assign(reinterpret_cast<const char *>(p_), static_cast<std::size_t>(nul - p_));
        p_ = nul + 1;
        align_from(start);
        return true;
    }

    /// 开一个只覆盖接下来 n 字节的子读取器，并**立刻**把外层游标推到段末。
    /// 外层无条件前进是故意的：忘记推进容器游标会让同一层的下一个条目读到容器
    /// 内容中间去，而这种错误在「容器正好是最后一个条目」时完全看不出来。
    bool enter(std::size_t n, Reader &out) {
        if (remaining() < n) {
            return fail("长度前缀超出了剩余字节");
        }
        out = Reader(p_, p_ + n);
        p_ += n;
        return true;
    }

    [[nodiscard]] bool ok() const { return err_.empty(); }

private:
    bool fail(const char *what) {
        set_err(what);
        return false;
    }

    const uint8_t *p_;
    const uint8_t *end_;
    std::string err_;
};

bool decode_into(Reader &r, Value &out, int depth) {
    if (depth > kMaxDepth) {
        r.set_err("XPC 嵌套过深");
        return false;
    }
    uint32_t tag = 0;
    if (!r.u32(tag)) {
        return false;
    }
    switch (tag) {
        case static_cast<uint32_t>(Type::Null):
            out = make_null();
            return true;
        case static_cast<uint32_t>(Type::Bool): {
            uint32_t raw = 0;
            if (!r.u32(raw)) {
                return false;
            }
            out = make_bool(raw != 0);
            return true;
        }
        case static_cast<uint32_t>(Type::Int64): {
            uint64_t raw = 0;
            if (!r.u64(raw)) {
                return false;
            }
            out = make_int64(static_cast<int64_t>(raw));
            return true;
        }
        case static_cast<uint32_t>(Type::UInt64): {
            uint64_t raw = 0;
            if (!r.u64(raw)) {
                return false;
            }
            out = make_uint64(raw);
            return true;
        }
        case static_cast<uint32_t>(Type::Date): {
            uint64_t raw = 0;
            if (!r.u64(raw)) {
                return false;
            }
            out = make_date(raw);
            return true;
        }
        case static_cast<uint32_t>(Type::Double): {
            uint64_t bits = 0;
            if (!r.u64(bits)) {
                return false;
            }
            double d = 0;
            std::memcpy(&d, &bits, sizeof(d));
            out = make_double(d);
            return true;
        }
        case static_cast<uint32_t>(Type::String): {
            const uint8_t *start = r.here();
            uint32_t len = 0;
            if (!r.u32(len)) {
                return false;
            }
            if (len == 0 || len > kMaxLen) {
                r.set_err("字符串长度不合理");
                return false;
            }
            std::vector<uint8_t> bytes;
            if (!r.take(len, bytes)) {
                return false;
            }
            r.align_from(start);
            // 长度含终结 NUL；按 C 字符串的语义取到第一个 NUL 为止。
            const auto nul = std::find(bytes.begin(), bytes.end(), uint8_t{0});
            out = make_string(std::string(bytes.begin(), nul == bytes.end() ? bytes.end() : nul));
            return true;
        }
        case static_cast<uint32_t>(Type::Data): {
            const uint8_t *start = r.here();
            uint32_t len = 0;
            if (!r.u32(len)) {
                return false;
            }
            if (len > kMaxLen) {
                r.set_err("数据段长度不合理");
                return false;
            }
            Value v;
            v.type = Type::Data;
            if (!r.take(len, v.data)) {
                return false;
            }
            r.align_from(start);
            out = std::move(v);
            return true;
        }
        case static_cast<uint32_t>(Type::Uuid): {
            Value v;
            v.type = Type::Uuid;
            if (!r.take(16, v.data)) {
                return false;
            }
            out = std::move(v);
            return true;
        }
        case static_cast<uint32_t>(Type::Array):
        case static_cast<uint32_t>(Type::Dict): {
            uint32_t total = 0;
            if (!r.u32(total)) {
                return false;
            }
            if (total < 4) {
                r.set_err("容器长度前缀连 count 字段都装不下");
                return false;
            }
            // 段边界收紧到 total，条目里再出现「本段内找不到 NUL」就是真畸形。
            // enter 会把外层游标一并推到段末，容器后面还有兄弟条目时靠这个前进。
            Reader body(r.here(), r.here());
            if (!r.enter(total, body)) {
                return false;
            }
            uint32_t count = 0;
            if (!body.u32(count)) {
                r.set_err(body.err());
                return false;
            }
            // 数组条目最少 4 字节（只有类型标记的 null），字典条目最少 8 字节
            // （1 字节键补到 4 + 4 字节值）。拿这个下限先拦掉吹牛的 count，
            // 免得为一个伪造的数字 resize 出巨量内存。
            const std::size_t min_entry = tag == static_cast<uint32_t>(Type::Array) ? 4 : 8;
            if (static_cast<std::size_t>(count) > body.remaining() / min_entry) {
                body.set_err("条目数与容器大小不符");
                r.set_err(body.err());
                return false;
            }
            Value v;
            v.type = tag == static_cast<uint32_t>(Type::Array) ? Type::Array : Type::Dict;
            bool parsed = true;
            if (v.type == Type::Array) {
                v.array.resize(count);
                for (uint32_t i = 0; i < count && parsed; ++i) {
                    parsed = decode_into(body, v.array[i], depth + 1);
                }
            } else {
                v.dict.resize(count);
                for (uint32_t i = 0; i < count && parsed; ++i) {
                    parsed = body.cstr(v.dict[i].key) && decode_into(body, v.dict[i].value, depth + 1);
                }
            }
            if (!parsed) {
                r.set_err(body.err().empty() ? "容器条目解码失败" : body.err());
                return false;
            }
            out = std::move(v);
            return true;
        }
        default:
            r.set_err("不支持的 XPC 类型标记 0x" + hex(tag));
            return false;
    }
}

}  // namespace

// ---------------------------------------------------------------- Value ------

const Value *Value::find(std::string_view key) const {
    for (const auto &entry : dict) {
        if (entry.key == key) {
            return &entry.value;
        }
    }
    return nullptr;
}

std::string Value::as_string_or(std::string_view fallback) const {
    return type == Type::String ? string : std::string(fallback);
}

int64_t Value::as_int_or(int64_t fallback) const {
    switch (type) {
        case Type::Int64:
            return int64;
        case Type::UInt64:
            return static_cast<int64_t>(uint64);
        case Type::Bool:
            return boolean ? 1 : 0;
        default:
            return fallback;
    }
}

bool Value::as_bool_or(bool fallback) const { return type == Type::Bool ? boolean : fallback; }

Value make_null() { return Value{}; }

Value make_bool(bool v) {
    Value x;
    x.type = Type::Bool;
    x.boolean = v;
    return x;
}

Value make_int64(int64_t v) {
    Value x;
    x.type = Type::Int64;
    x.int64 = v;
    return x;
}

Value make_uint64(uint64_t v) {
    Value x;
    x.type = Type::UInt64;
    x.uint64 = v;
    return x;
}

Value make_double(double v) {
    Value x;
    x.type = Type::Double;
    x.real = v;
    return x;
}

Value make_date(uint64_t ns) {
    Value x;
    x.type = Type::Date;
    x.uint64 = ns;
    return x;
}

Value make_string(std::string v) {
    Value x;
    x.type = Type::String;
    x.string = std::move(v);
    return x;
}

Value make_data(std::vector<uint8_t> v) {
    Value x;
    x.type = Type::Data;
    x.data = std::move(v);
    return x;
}

Value make_uuid(std::span<const uint8_t> v) {
    Value x;
    x.type = Type::Uuid;
    x.data.assign(v.begin(), v.end());
    return x;
}

Value make_array() {
    Value x;
    x.type = Type::Array;
    return x;
}

Value make_dict() {
    Value x;
    x.type = Type::Dict;
    return x;
}

void array_push(Value &arr, Value item) {
    if (arr.is_array()) {
        arr.array.push_back(std::move(item));
    }
}

void dict_set(Value &dict, std::string key, Value item) {
    if (!dict.is_dict()) {
        return;
    }
    for (auto &entry : dict.dict) {
        if (entry.key == key) {
            entry.value = std::move(item);
            return;
        }
    }
    dict.dict.push_back(Entry{std::move(key), std::move(item)});
}

// ------------------------------------------------------------ 对象编解码 ------

std::vector<uint8_t> encode(const Value &v) {
    std::vector<uint8_t> out;
    if (!encode_into(out, v, 0)) {
        return {};
    }
    return out;
}

std::optional<Value> decode(std::span<const uint8_t> buf, std::string &err) {
    if (buf.size() > kMaxBuffer) {
        err = "缓冲区过大";
        return std::nullopt;
    }
    Reader r(buf.data(), buf.data() + buf.size());
    Value v;
    if (!decode_into(r, v, 0)) {
        err = r.err().empty() ? "解码失败" : r.err();
        return std::nullopt;
    }
    return v;
}

// ---------------------------------------------------------- 消息（wrapper） ---

std::vector<uint8_t> encode_message(uint32_t flags, uint64_t message_id, const Value *body) {
    std::vector<uint8_t> payload;
    if (body != nullptr) {
        const auto obj = encode(*body);
        if (obj.empty()) {
            return {};
        }
        put_u32(payload, kPayloadMagic);
        put_u32(payload, kProtocolVersion);
        payload.insert(payload.end(), obj.begin(), obj.end());
    }
    std::vector<uint8_t> out;
    put_u32(out, kWrapperMagic);
    put_u32(out, flags);
    // 长度只记载荷本身，不含紧随其后的 8 字节消息号——看着别扭，但说明 Apple
    // 原意是「消息体长度」，而消息号属于信封。
    put_u64(out, static_cast<uint64_t>(payload.size()));
    put_u64(out, message_id);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

Status decode_message(std::span<const uint8_t> buf, Message &out, std::size_t &consumed,
                      std::string &err) {
    consumed = 0;
    if (buf.size() < 24) {
        err = "信封不足 24 字节";
        return Status::NeedMore;
    }
    const uint32_t magic = static_cast<uint32_t>(buf[0]) | static_cast<uint32_t>(buf[1]) << 8 |
                           static_cast<uint32_t>(buf[2]) << 16 | static_cast<uint32_t>(buf[3]) << 24;
    if (magic != kWrapperMagic) {
        err = "wrapper magic 不符：0x" + hex(magic);
        return Status::Malformed;
    }
    uint64_t body_len = 0;
    for (int i = 0; i < 8; ++i) {
        body_len |= static_cast<uint64_t>(buf[8 + i]) << (8 * i);
    }
    if (body_len > kMaxBuffer) {
        err = "消息长度不合理：" + u64_to_string(body_len);
        return Status::Malformed;
    }
    const std::size_t total = 24 + static_cast<std::size_t>(body_len);
    if (buf.size() < total) {
        err = "还差 " + u64_to_string(total - buf.size()) + " 字节";
        return Status::NeedMore;
    }

    Message m;
    m.flags = static_cast<uint32_t>(buf[4]) | static_cast<uint32_t>(buf[5]) << 8 |
              static_cast<uint32_t>(buf[6]) << 16 | static_cast<uint32_t>(buf[7]) << 24;
    for (int i = 0; i < 8; ++i) {
        m.message_id |= static_cast<uint64_t>(buf[16 + i]) << (8 * i);
    }
    if (body_len > 0) {
        Reader r(buf.data() + 24, buf.data() + total);
        uint32_t pmagic = 0;
        if (!r.u32(pmagic) || pmagic != kPayloadMagic) {
            err = "payload magic 不符：0x" + hex(pmagic);
            return Status::Malformed;
        }
        uint32_t version = 0;
        if (!r.u32(version)) {
            err = "读不到协议版本";
            return Status::Malformed;
        }
        if (version != kProtocolVersion) {
            err = "XPC 协议版本不支持：0x" + hex(version);
            return Status::Malformed;
        }
        Value v;
        if (!decode_into(r, v, 0)) {
            err = r.err().empty() ? "载荷解码失败" : r.err();
            return Status::Malformed;
        }
        m.has_body = true;
        m.body = std::move(v);
    }
    consumed = total;
    out = std::move(m);
    return Status::Ok;
}

// ----------------------------------------------------------------- 调试 ------

std::string describe(const Value &v) {
    switch (v.type) {
        case Type::Null:
            return "null";
        case Type::Bool:
            return v.boolean ? "true" : "false";
        case Type::Int64:
            return i64_to_string(v.int64);
        case Type::UInt64:
        case Type::Date:
            return u64_to_string(v.uint64);
        case Type::Double: {
            char buf[40];
            std::snprintf(buf, sizeof(buf), "%g", v.real);
            return buf;
        }
        case Type::String: {
            constexpr std::size_t kTruncate = 120;
            if (v.string.size() <= kTruncate) {
                return '"' + v.string + '"';
            }
            return '"' + v.string.substr(0, kTruncate) + "...\"(" + u64_to_string(v.string.size()) +
                   ")";
        }
        case Type::Data:
            return "<" + u64_to_string(v.data.size()) + " bytes>";
        case Type::Uuid: {
            static constexpr char kHex[] = "0123456789abcdef";
            std::string s;
            for (std::size_t i = 0; i < v.data.size(); ++i) {
                if (i == 4 || i == 6 || i == 8 || i == 10) {
                    s += '-';
                }
                s += kHex[v.data[i] >> 4];
                s += kHex[v.data[i] & 0xF];
            }
            return s;
        }
        case Type::Array: {
            std::string s = "[";
            for (std::size_t i = 0; i < v.array.size(); ++i) {
                if (i) {
                    s += ", ";
                }
                s += describe(v.array[i]);
                if (s.size() > 400) {
                    s += ", ...";
                    break;
                }
            }
            return s + ']';
        }
        case Type::Dict: {
            std::string s = "{";
            for (std::size_t i = 0; i < v.dict.size(); ++i) {
                if (i) {
                    s += ", ";
                }
                s += v.dict[i].key;
                s += ": ";
                s += describe(v.dict[i].value);
                if (s.size() > 400) {
                    s += ", ...";
                    break;
                }
            }
            return s + '}';
        }
    }
    return "?";
}

}  // namespace scrctl::xpc
