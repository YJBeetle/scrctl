#include "plist/Bplist.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace scrctl::plist {
namespace {

constexpr int kMaxDepth = 64;
constexpr std::size_t kMaxObjects = 1u << 20;

std::size_t be_at(const uint8_t *p, std::size_t n) {
    std::size_t v = 0;
    for (std::size_t i = 0; i < n; ++i) {
        v = v << 8 | p[i];
    }
    return v;
}

int width_for(std::size_t max_value) {
    if (max_value <= 0xFF) {
        return 1;
    }
    if (max_value <= 0xFFFF) {
        return 2;
    }
    if (max_value <= 0xFFFFFF) {
        return 3;
    }
    return 4;
}

void put_be(std::vector<uint8_t> &out, uint64_t v, int width) {
    for (int i = width - 1; i >= 0; --i) {
        out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
}

// ------------------------------------------------------------------ 读取 ------

class BinaryParser {
public:
    BinaryParser(const uint8_t *data, std::size_t len) : d_(data), n_(len) {}

    std::optional<Value> run(std::string &err) {
        if (n_ < 40) {
            err = "bplist 太短";
            return std::nullopt;
        }
        if (std::memcmp(d_, "bplist", 6) != 0) {
            err = "缺 bplist 魔数";
            return std::nullopt;
        }
        // 尾部 32 字节：6 未用 + 偏移长度 + 引用长度 + 对象数 + 根编号 + 偏移表位置
        const uint8_t *trailer = d_ + n_ - 32;
        offset_size_ = trailer[6];
        ref_size_ = trailer[7];
        const std::size_t count = be_at(trailer + 8, 8);
        const std::size_t top = be_at(trailer + 16, 8);
        const std::size_t table = be_at(trailer + 24, 8);
        if (offset_size_ == 0 || offset_size_ > 8 || ref_size_ == 0 || ref_size_ > 8) {
            err = "bplist 尾部声明的字节宽度不合理";
            return std::nullopt;
        }
        if (count == 0 || count > kMaxObjects) {
            err = "bplist 对象数不合理: " + std::to_string(count);
            return std::nullopt;
        }
        if (table > n_ - 32 || count > (n_ - 32 - table) / offset_size_) {
            err = "偏移表越界或被截断";
            return std::nullopt;
        }
        if (top >= count) {
            err = "根对象编号越界";
            return std::nullopt;
        }
        offsets_.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            offsets_[i] = be_at(d_ + table + i * offset_size_, offset_size_);
            if (offsets_[i] >= n_ - 32) {
                err = "对象偏移落在尾部之后";
                return std::nullopt;
            }
        }
        Value out;
        if (!decode(top, 0, out)) {
            err = err_;
            return std::nullopt;
        }
        return out;
    }

private:
    bool fail(const char *what) {
        if (err_.empty()) {
            err_ = what;
        }
        return false;
    }

    /// 标志字节低 4 位为 0xF 时，长度改用「0x1X + X 字节大端」的长形态。
    bool read_length(std::size_t &pos, uint8_t low, std::size_t &out) {
        if (low != 0xF) {
            out = low;
            return true;
        }
        if (pos + 1 > n_) {
            return fail("长度字段被截断");
        }
        const uint8_t wide = d_[pos++];
        if ((wide >> 4) != 1) {
            return fail("长形态的计数头不是 0x1X");
        }
        const std::size_t width = std::size_t{1} << (wide & 0xF);
        if (pos + width > n_) {
            return fail("计数字节被截断");
        }
        out = be_at(d_ + pos, width);
        pos += width;
        return true;
    }

    bool decode(std::size_t idx, int depth, Value &out) {
        if (depth > kMaxDepth) {
            return fail("嵌套过深（可能有环）");
        }
        if (idx >= offsets_.size()) {
            return fail("对象编号越界");
        }
        std::size_t pos = offsets_[idx];
        const uint8_t marker = d_[pos++];
        const uint8_t high = marker >> 4;
        const uint8_t low = marker & 0xF;

        switch (high) {
            case 0x0:
                if (low == 0) {
                    out = Value();  // null 落到 Bool(false)：子集里没有 null 这一类
                    return true;
                }
                if (low == 8 || low == 9) {
                    out = Value::Bool(low == 9);
                    return true;
                }
                return fail("保留标志位");
            case 0x1: {  // 整数，字节数 = 2^low
                if (low > 4) {
                    return fail("整数宽度超过 8 字节");
                }
                const std::size_t width = std::size_t{1} << low;
                if (pos + width > n_) {
                    return fail("整数被截断");
                }
                // 窄形态一律按**无符号**读：这个格式里没有"有符号窄整数"的概念，
                // 负数在写入方就用满 8 字节。按符号扩展会把 0xFF 读成 -1，
                // 而 0xFF 的正确读法是 255。
                out.kind = Kind::Int;
                out.integer = static_cast<int64_t>(be_at(d_ + pos, width));
                return true;
            }
            case 0x2: {  // 浮点
                if (low != 3) {
                    return fail("只支持 8 字节浮点");
                }
                if (pos + 8 > n_) {
                    return fail("浮点被截断");
                }
                const uint64_t bits = be_at(d_ + pos, 8);
                double v = 0;
                std::memcpy(&v, &bits, sizeof(v));
                out.kind = Kind::Real;
                out.real = v;
                return true;
            }
            case 0x4: {  // data
                std::size_t len = 0;
                if (!read_length(pos, low, len) || pos + len > n_) {
                    return fail("data 越界");
                }
                out = Value::OfData(std::vector<uint8_t>(d_ + pos, d_ + pos + len));
                return true;
            }
            case 0x5: {  // ASCII
                std::size_t len = 0;
                if (!read_length(pos, low, len) || pos + len > n_) {
                    return fail("字符串越界");
                }
                out.kind = Kind::String;
                out.string.assign(reinterpret_cast<const char *>(d_ + pos), len);
                return true;
            }
            case 0x6: {  // UTF-16BE -> UTF-8
                std::size_t chars = 0;
                if (!read_length(pos, low, chars) || pos + chars * 2 > n_) {
                    return fail("UTF-16 串越界");
                }
                out.kind = Kind::String;
                for (std::size_t i = 0; i < chars; ++i) {
                    uint32_t u = static_cast<uint32_t>(d_[pos + i * 2]) << 8 | d_[pos + i * 2 + 1];
                    if (u >= 0xD800 && u < 0xDC00 && i + 1 < chars) {
                        const uint32_t lo =
                            static_cast<uint32_t>(d_[pos + (i + 1) * 2]) << 8 |
                            d_[pos + (i + 1) * 2 + 1];
                        if (lo >= 0xDC00 && lo < 0xE000) {
                            u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                            ++i;
                        }
                    }
                    append_utf8(out.string, u);
                }
                return true;
            }
            case 0xA:
            case 0xD: {  // 数组 / 字典
                std::size_t count = 0;
                if (!read_length(pos, low, count)) {
                    return false;
                }
                if (count > (n_ - pos) / ref_size_) {
                    return fail("元素引用数超出剩余字节");
                }
                if (high == 0xA) {
                    out = Value::Array();
                    out.array.resize(count);
                    for (std::size_t i = 0; i < count; ++i) {
                        const std::size_t child = be_at(d_ + pos + i * ref_size_, ref_size_);
                        if (!decode(child, depth + 1, out.array[i])) {
                            return false;
                        }
                    }
                    return true;
                }
                out = Value::Dict();
                for (std::size_t i = 0; i < count; ++i) {
                    const std::size_t key = be_at(d_ + pos + i * ref_size_, ref_size_);
                    const std::size_t val =
                        be_at(d_ + pos + (count + i) * ref_size_, ref_size_);
                    Value k;
                    Value v;
                    if (!decode(key, depth + 1, k) || !k.is_string()) {
                        return fail("字典键必须是字符串");
                    }
                    if (!decode(val, depth + 1, v)) {
                        return false;
                    }
                    out.keys.push_back(k.string);
                    out.values.push_back(std::move(v));
                }
                return true;
            }
            default:
                return fail("不支持的 bplist 标志");
        }
    }

    static void append_utf8(std::string &out, uint32_t u) {
        if (u < 0x80) {
            out.push_back(static_cast<char>(u));
        } else if (u < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (u >> 6)));
            out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        } else if (u < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (u >> 12)));
            out.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (u >> 18)));
            out.push_back(static_cast<char>(0x80 | ((u >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        }
    }

    const uint8_t *d_;
    std::size_t n_;
    std::size_t offset_size_ = 1;
    std::size_t ref_size_ = 1;
    std::vector<std::size_t> offsets_;
    std::string err_;
};

// ------------------------------------------------------------------ 写入 ------

/// 标志字节低 4 位为 0xF 时，长度改用「0x1X + X 字节大端计数」的长形态。
void put_count(std::vector<uint8_t> &out, uint8_t high, std::size_t len) {
    if (len < 0xF) {
        out.push_back(static_cast<uint8_t>(high << 4 | len));
        return;
    }
    out.push_back(static_cast<uint8_t>(high << 4 | 0xF));
    const int width = width_for(len);
    out.push_back(static_cast<uint8_t>(0x10 | (width == 1 ? 0 : width == 2 ? 1 : 2)));
    put_be(out, len, width);
}

/// UTF-8 文本占多少个 UTF-16 单元（星外平面要算两个）。
std::size_t utf16_length(std::string_view s) {
    std::size_t units = 0;
    for (std::size_t i = 0; i < s.size();) {
        const auto b = static_cast<uint8_t>(s[i]);
        if (b < 0x80) {
            i += 1;
        } else if (b < 0xE0) {
            i += 2;
        } else if (b < 0xF0) {
            i += 3;
        } else {
            i += 4;
            ++units;
        }
        ++units;
    }
    return units;
}

void put_utf16be(std::vector<uint8_t> &out, std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        const auto b = static_cast<uint8_t>(s[i]);
        uint32_t u = 0;
        if (b < 0x80) {
            u = b;
            i += 1;
        } else if (b < 0xE0) {
            u = (b & 0x1FU) << 6 | (static_cast<uint8_t>(s[i + 1]) & 0x3FU);
            i += 2;
        } else if (b < 0xF0) {
            u = (b & 0x0FU) << 12 | (static_cast<uint8_t>(s[i + 1]) & 0x3FU) << 6 |
                (static_cast<uint8_t>(s[i + 2]) & 0x3FU);
            i += 3;
        } else {
            u = (b & 0x07U) << 18 | (static_cast<uint8_t>(s[i + 1]) & 0x3FU) << 12 |
                (static_cast<uint8_t>(s[i + 2]) & 0x3FU) << 6 | (static_cast<uint8_t>(s[i + 3]) & 0x3FU);
            i += 4;
            u -= 0x10000;
            put_be(out, 0xD800 + (u >> 10), 2);
            u = 0xDC00 + (u & 0x3FF);
        }
        put_be(out, u, 2);
    }
}

/// 一个待写对象。先给每个节点分配编号，再按编号写引用——引用宽度取决于对象总数，
/// 而对象总数要展开完才知道，所以必须两段式。
struct Node {
    const Value *v = nullptr;      ///< 值节点
    std::string key;               ///< 键节点（v 为空）
    std::vector<std::size_t> refs;  ///< 字典是「全部键 + 全部值」，数组是各元素
};

std::size_t add_string(std::vector<Node> &nodes, std::string_view text) {
    nodes.push_back(Node{nullptr, std::string(text), {}});
    return nodes.size() - 1;
}

std::size_t build(std::vector<Node> &nodes, const Value &v) {
    const std::size_t id = nodes.size();
    nodes.push_back(Node{&v, {}, {}});
    std::vector<std::size_t> refs;
    if (v.is_dict()) {
        // 苹果自己的写法是键排序，跟着排，别让下游按「插入序」猜。
        std::vector<std::size_t> order(v.keys.size());
        for (std::size_t i = 0; i < order.size(); ++i) {
            order[i] = i;
        }
        std::sort(order.begin(), order.end(),
                  [&](std::size_t a, std::size_t b) { return v.keys[a] < v.keys[b]; });
        // 格式要求引用区是「全部键 + 全部值」两段，所以先排完键再排值。
        for (const auto slot : order) {
            const std::size_t key_id = add_string(nodes, v.keys[slot]);
            refs.push_back(key_id);
        }
        for (const auto slot : order) {
            const std::size_t value_id = build(nodes, v.values[slot]);
            refs.push_back(value_id);
        }
    } else if (v.is_array()) {
        for (const auto &item : v.array) {
            const std::size_t child = build(nodes, item);
            refs.push_back(child);
        }
    }
    // 必须重新按下标取，不能跨上面的 build/add_string 持有 nodes 里的引用：
    // 那些 push_back 会让 vector 扩容，把先前拿的引用连同它的内容一起搬到新地址，
    // 于是 refs 写进了已经作废的内存——表现为对象数莫名其妙地少。
    nodes[id].refs = std::move(refs);
    return id;
}

void put_ref(std::vector<uint8_t> &out, std::size_t id, int ref_size) {
    put_be(out, id, ref_size);
}

}  // namespace

std::optional<Value> parse_binary(const uint8_t *data, std::size_t len, std::string *err) {
    std::string local;
    auto out = BinaryParser(data, len).run(local);
    if (!out && err != nullptr) {
        *err = local;
    }
    return out;
}

std::optional<Value> parse_binary(const std::vector<uint8_t> &bytes, std::string *err) {
    return parse_binary(bytes.data(), bytes.size(), err);
}

std::vector<uint8_t> write_binary(const Value &root) {
    std::vector<Node> nodes;
    build(nodes, root);
    const std::size_t count = nodes.size();
    if (count == 0 || count > kMaxObjects) {
        return {};
    }
    const int ref_size = static_cast<int>(width_for(count - 1));

    std::vector<std::vector<uint8_t>> bodies(count);
    for (std::size_t id = 0; id < count; ++id) {
        auto &out = bodies[id];
        const Node &node = nodes[id];
        if (node.v == nullptr) {
            // 键节点：只会是字符串。
            const std::string &k = node.key;
            const bool ascii =
                std::all_of(k.begin(), k.end(), [](char c) { return static_cast<uint8_t>(c) < 0x80; });
            if (ascii) {
                put_count(out, 0x5, k.size());
                out.insert(out.end(), k.begin(), k.end());
            } else {
                put_count(out, 0x6, utf16_length(k));
                put_utf16be(out, k);
            }
            continue;
        }
        const Value &v = *node.v;
        switch (v.kind) {
            case Kind::Bool:
                out.push_back(v.boolean ? 0x09 : 0x08);
                break;
            case Kind::Int: {
                const uint64_t raw = static_cast<uint64_t>(v.integer);
                int width = 8;
                if (v.integer >= 0 && v.integer <= 0xFF) {
                    width = 1;
                } else if (v.integer >= 0 && v.integer <= 0xFFFF) {
                    width = 2;
                } else if (v.integer >= 0 && v.integer <= 0xFFFFFFFFLL) {
                    width = 4;
                }
                // 负数一律走 8 字节。这个格式里没有"有符号窄整数"的概念：4 字节的
                // 0xFFFFFFFE 会被按无符号读成 4294967294。苹果的写法就是负数直接
                // 用满 8 字节，跟着写才不会让对端读出一个完全不同的数。
                out.push_back(static_cast<uint8_t>(0x10 | (width == 1 ? 0 : width == 2 ? 1
                                                                       : width == 4   ? 2
                                                                                        : 3)));
                put_be(out, raw, width);
                break;
            }
            case Kind::Real: {
                uint64_t bits = 0;
                std::memcpy(&bits, &v.real, sizeof(bits));
                out.push_back(0x23);
                put_be(out, bits, 8);
                break;
            }
            case Kind::String: {
                const bool ascii = std::all_of(v.string.begin(), v.string.end(),
                                               [](char c) { return static_cast<uint8_t>(c) < 0x80; });
                if (ascii) {
                    put_count(out, 0x5, v.string.size());
                    out.insert(out.end(), v.string.begin(), v.string.end());
                } else {
                    put_count(out, 0x6, utf16_length(v.string));
                    put_utf16be(out, v.string);
                }
                break;
            }
            case Kind::Data:
                put_count(out, 0x4, v.data.size());
                out.insert(out.end(), v.data.begin(), v.data.end());
                break;
            case Kind::Array:
                put_count(out, 0xA, node.refs.size());
                for (const auto r : node.refs) {
                    put_ref(out, r, ref_size);
                }
                break;
            case Kind::Dict: {
                const auto pairs = static_cast<std::size_t>(node.refs.size() / 2);
                put_count(out, 0xD, pairs);
                for (const auto r : node.refs) {
                    put_ref(out, r, ref_size);
                }
                break;
            }
        }
    }

    // 偏移表占几字节，取决于文件总长；而总长又取决于偏移表占几字节。从 1 字节
    // 起迭代两轮就收敛（每轮只可能让总长变长，不会来回摆）。
    int offset_size = 1;
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::size_t total = 8 + 32 + count * static_cast<std::size_t>(offset_size);
        for (const auto &b : bodies) {
            total += b.size();
        }
        const int needed = static_cast<int>(width_for(total));
        if (needed == offset_size) {
            std::vector<uint8_t> out;
            out.insert(out.end(), {'b', 'p', 'l', 'i', 's', 't', '0', '0'});
            std::vector<std::size_t> offsets(count);
            for (std::size_t id = 0; id < count; ++id) {
                offsets[id] = out.size();
                out.insert(out.end(), bodies[id].begin(), bodies[id].end());
            }
            const std::size_t table = out.size();
            for (const auto o : offsets) {
                put_be(out, o, offset_size);
            }
            for (int i = 0; i < 6; ++i) {
                out.push_back(0);
            }
            out.push_back(static_cast<uint8_t>(offset_size));
            out.push_back(static_cast<uint8_t>(ref_size));
            put_be(out, count, 8);
            put_be(out, 0, 8);  // 根对象就是 0 号
            put_be(out, table, 8);
            return out;
        }
        offset_size = needed;
    }
    return {};
}

}  // namespace scrctl::plist
