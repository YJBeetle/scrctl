// XPC 二进制对象图自检。全部离线：RemoteXPC 那一段一旦接上真机就已经没有
// 单独验编解码的机会了，所以对齐、长度前缀这些坑必须在没有设备的时候先钉死。
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "xpc/XpcValue.h"

namespace {

int Failures = 0;

void check(bool ok, const std::string &what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) {
        ++Failures;
    }
}

std::string hex(std::span<const uint8_t> bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string s;
    for (const uint8_t b : bytes) {
        s += kHex[b >> 4];
        s += kHex[b & 0xF];
    }
    return s;
}

std::string hex_n(const std::vector<uint8_t> &v, std::size_t n) {
    return hex(std::span<const uint8_t>(v.data(), std::min(n, v.size())));
}

using namespace scrctl::xpc;

void expect_bytes(const Value &v, const std::string &want, const std::string &what) {
    const auto got = encode(v);
    if (hex(got) == want) {
        check(true, what);
        return;
    }
    check(false, what + "\n        实际 " + hex(got) + "\n        期望 " + want);
}

std::vector<uint8_t> unhex(std::string_view s) {
    auto nibble = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') {
            return static_cast<uint8_t>(c - '0');
        }
        return static_cast<uint8_t>(c - 'a' + 10);
    };
    std::vector<uint8_t> v;
    for (std::size_t i = 0; i + 1 < s.size(); i += 2) {
        v.push_back(static_cast<uint8_t>(nibble(s[i]) << 4 | nibble(s[i + 1])));
    }
    return v;
}

std::vector<uint8_t> bytes_of(std::initializer_list<int> vals) {
    std::vector<uint8_t> out;
    for (const int v : vals) {
        out.push_back(static_cast<uint8_t>(v));
    }
    return out;
}

/// 一条按线上格式逐字节写出的握手消息，用作解码方向的基准。分行只是排版，
/// 长度由下面的 264 字节断言把住——漏一个字符就解不出来。
constexpr const char *kHandshakeGolden =
    "920bb02901010000f0000000000000000000000000000000423713420500000000f00000"
    "e0000000050000004d6573736167655479706500009000000a00000048616e647368616b"
    "650000004d6573736167696e6750726f746f636f6c56657273696f6e0000000000400000"
    "0700000000000000555549440000000000a00000101112131415161718191a1b1c1d1e1f"
    "50726f70657274696573000000f000004c0000000200000052656d6f7465585043566572"
    "73696f6e466c61677300000000400000060000000000000153656e73697469766550726f"
    "7065727469657356697369626c6500000020000001000000536572766963657300000000"
    "00f000000400000000000000";

/// 解码方向必须单独钉一遍：前面所有往返检查都是「我的编码器写、我的解码器读」，
/// 两边同时错成同一个样子就抓不住（真正抓到那次信封字段位置写反，是靠断言字节
/// 偏移才暴露的）。这份基准按同一格式由另一套独立实现产出，仓库里只留字节。
void test_golden_decode() {
    std::printf("\n== 解码基准 ==\n");
    const auto raw = unhex(kHandshakeGolden);
    check(raw.size() == 24 + 0xf0, "基准长度 = 24 字节信封 + 声明的载荷长度");

    Message m;
    std::size_t used = 0;
    std::string err;
    check(decode_message(raw, m, used, err) == Status::Ok, "解出握手消息: " + err);
    check(m.flags == (kFlagAlwaysSet | kFlagDataPresent), "flags");
    check(m.message_id == 0, "message_id");
    check(m.body.find("MessageType")->string == "Handshake", "字符串字段");
    check(m.body.find("MessagingProtocolVersion")->uint64 == 7, "uint64 字段");
    check(m.body.find("UUID")->type == Type::Uuid && m.body.find("UUID")->data.size() == 16,
          "UUID 定长 16，无长度前缀");
    check(describe(*m.body.find("UUID")) == "10111213-1415-1617-1819-1a1b1c1d1e1f",
          "UUID 字节序原样");
    const auto *props = m.body.find("Properties");
    check(props->find("RemoteXPCVersionFlags")->uint64 == 0x0100000000000006ULL, "嵌套字典里的 uint64");
    check(props->find("SensitivePropertiesVisible")->boolean, "嵌套字典里的 bool");
    check(m.body.find("Services")->is_dict() && m.body.find("Services")->dict.empty(), "空字典");

    const auto again = encode_message(m.flags, m.message_id, &m.body);
    check(hex(again) == kHandshakeGolden, "解出来再编回去，与基准逐字节相同");
}


/// 往返：解出来再编回去，字节必须完全一致。这是最能抓住对齐/长度前缀错位的检查。
void roundtrip(const Value &v, const std::string &what) {
    const auto wire = encode(v);
    std::string err;
    auto back = decode(wire, err);
    if (!back) {
        check(false, what + " 解码失败: " + err);
        return;
    }
    check(encode(*back) == wire, what + " 往返字节一致");
}

void test_scalars() {
    std::printf("\n== 标量的逐字节形态 ==\n");
    expect_bytes(make_null(), "00100000", "null");
    expect_bytes(make_bool(true), "00200000" "01000000", "bool true");
    expect_bytes(make_bool(false), "00200000" "00000000", "bool false");
    expect_bytes(make_int64(-2), "00300000" "feffffffffffffff", "int64 -2");
    expect_bytes(make_uint64(7), "00400000" "0700000000000000", "uint64 7");
    expect_bytes(make_date(1000000000ULL), "00700000" "00ca9a3b00000000", "date 1s");
    expect_bytes(make_double(1.5), "00500000" "000000000000f83f", "double 1.5");

    // 字符串的长度前缀**包含**终结 NUL，整段还要补零到 4 字节边界。
    expect_bytes(make_string(""), "00900000" "01000000" "00" "000000", "空串：长度=1，补 5");
    expect_bytes(make_string("a"), "00900000" "02000000" "6100" "0000", "串 a");
    expect_bytes(make_string("ab"), "00900000" "03000000" "616200" "00", "串 ab");
    expect_bytes(make_string("abc"), "00900000" "04000000" "61626300", "串 abc 无需补零");
    expect_bytes(make_string("Handshake"), "00900000" "0a000000" "48616e647368616b6500" "0000",
                 "串 Handshake：10 字节，补 2");

    // 数据的长度前缀就是裸字节数。
    expect_bytes(make_data({}), "00800000" "00000000", "空数据");
    expect_bytes(make_data({1, 2, 3}), "00800000" "03000000" "010203" "00", "数据 3 字节补 1");
    expect_bytes(make_data({1, 2, 3, 4}), "00800000" "04000000" "01020304", "数据 4 字节不补");

    uint8_t uuid[16];
    for (int i = 0; i < 16; ++i) {
        uuid[i] = static_cast<uint8_t>(i);
    }
    expect_bytes(make_uuid(uuid), "00a00000" "000102030405060708090a0b0c0d0e0f", "uuid 无长度前缀");
}

void test_containers() {
    std::printf("\n== 容器：长度前缀与键的对齐 ==\n");
    expect_bytes(make_dict(), "00f00000" "04000000" "00000000", "空字典：长度=4，只有一个 count");

    Value one = make_dict();
    dict_set(one, "A", make_bool(true));
    // count(4) + 键 "A\\0" 补到 4 + 布尔对象 8 = 16
    expect_bytes(one, "00f00000" "10000000"
                      "01000000"
                      "41000000"
                      "00200000" "01000000",
                 "字典 {A: true}");

    // 键是「裸」C 串，没有长度前缀——这是它和字符串对象唯一的区别，也是最容易写错的地方。
    Value keys = make_dict();
    dict_set(keys, "abc", make_null());
    expect_bytes(keys, "00f00000" "0c000000"
                       "01000000"
                       "61626300"  // "abc" + NUL，正好 4
                       "00100000",
                 "键 abc 天然对齐");

    Value arr = make_array();
    array_push(arr, make_int64(1));
    array_push(arr, make_int64(2));
    expect_bytes(arr, "00e00000" "1c000000"
                      "02000000"
                      "00300000" "0100000000000000"
                      "00300000" "0200000000000000",
                 "数组 [1, 2]：长度=4+12+12");

    std::printf("\n== 嵌套往返 ==\n");
    Value msg = make_dict();
    dict_set(msg, "MessageType", make_string("Handshake"));
    dict_set(msg, "MessagingProtocolVersion", make_uint64(7));
    dict_set(msg, "UUID", make_uuid(bytes_of({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16})));
    Value props = make_dict();
    dict_set(props, "RemoteXPCVersionFlags", make_uint64(0x0100000000000006ULL));
    dict_set(props, "SensitivePropertiesVisible", make_bool(true));
    dict_set(msg, "Properties", std::move(props));
    dict_set(msg, "Services", make_dict());
    roundtrip(msg, "设备握手字典");

    Value deep = make_dict();
    dict_set(deep, "list", [&] {
        Value a = make_array();
        for (int i = 0; i < 5; ++i) {
            Value inner = make_dict();
            dict_set(inner, "s", make_string(std::string(static_cast<size_t>(i + 1), 'x')));
            dict_set(inner, "n", make_int64(-i));
            dict_set(inner, "d", make_data(bytes_of({0xfe, 0xff})));
            array_push(a, std::move(inner));
        }
        return a;
    }());
    roundtrip(deep, "数组套字典，含变长键与数据");
    const auto *list = deep.find("list");
    check(list != nullptr && list->array.size() == 5, "嵌套取值 find + array");
    check(list && list->array[3].find("s")->string == "xxxx", "第 4 项的字符串");

    Value mixed = make_dict();
    dict_set(mixed, "weird keys", make_string("包含空格的键"));
    dict_set(mixed, "", make_string("空键"));
    roundtrip(mixed, "怪键字典");
}

void test_message_envelope() {
    std::printf("\n== 消息信封 ==\n");
    Value body = make_dict();
    dict_set(body, "Command", make_string("DoIt"));
    const auto wire = encode_message(kFlagAlwaysSet | kFlagDataPresent | kFlagWantingReply, 3, &body);

    check(wire.size() > 24, "有载荷的消息长于 24 字节头");
    uint64_t recorded = 0;
    for (int i = 0; i < 8; ++i) {
        recorded |= static_cast<uint64_t>(wire[8 + i]) << (8 * i);
    }
    check(recorded + 24 == wire.size(), "u64 长度字段 == 载荷长度（不含信封）");
    check(hex_n(wire, 4) == "920bb029", "wrapper magic 小端");
    check(hex(std::span<const uint8_t>(wire.data() + 24, 4)) == "42371342", "payload magic 小端");

    Message m;
    std::size_t consumed = 0;
    std::string err;
    check(decode_message(wire, m, consumed, err) == Status::Ok && err.empty(), "解出消息: " + err);
    check(m.flags == (kFlagAlwaysSet | kFlagDataPresent | kFlagWantingReply), "flags 原样");
    check(m.message_id == 3, "message_id 原样");
    check(m.has_body && m.body.find("Command")->string == "DoIt", "载荷内容");
    check(consumed == wire.size(), "consumed == 全长");

    // 空载荷：终止帧 / INIT_HANDSHAKE 帧要的就是这个形状。
    const auto empty = encode_message(kFlagAlwaysSet | kFlagInitHandshake, 0, nullptr);
    check(hex(empty) == "920bb029" "01004000" "0000000000000000" "0000000000000000",
          "空载荷 = 24 字节，长度记 0 但消息号仍占 8 字节");
    Message e;
    check(decode_message(empty, e, consumed, err) == Status::Ok && !e.has_body, "空载荷可解");

    std::printf("\n== 粘包与半包 ==\n");
    std::vector<uint8_t> glued = wire;
    glued.insert(glued.end(), empty.begin(), empty.end());
    Message first;
    check(decode_message(glued, first, consumed, err) == Status::Ok && consumed == wire.size(),
          "两条消息粘在一起时只吃第一条");
    Message second;
    std::span<const uint8_t> rest(glued.data() + consumed, glued.size() - consumed);
    check(decode_message(rest, second, consumed, err) == Status::Ok && second.has_body == false,
          "剩下的字节能解出第二条");

    for (std::size_t cut = 0; cut < wire.size(); ++cut) {
        Message tmp;
        std::size_t c2 = 0;
        const auto st = decode_message({wire.data(), cut}, tmp, c2, err);
        if (st != Status::NeedMore) {
            check(false, "截断到 " + std::to_string(cut) + " 字节时应报 NeedMore");
            return;
        }
    }
    check(true, "每一个截断长度都报 NeedMore（而不是 Malformed）");

    std::printf("\n== 必须拒绝的畸形输入 ==\n");
    auto bad_magic = wire;
    bad_magic[0] ^= 0xff;
    Message tmp;
    std::size_t c = 0;
    check(decode_message(bad_magic, tmp, c, err) == Status::Malformed, "wrapper magic 坏了");

    auto huge = wire;
    for (int i = 8; i < 16; ++i) {  // 长度字段在信封的第 8 字节，不是第 16
        huge[i] = 0xff;
    }
    check(decode_message(huge, tmp, c, err) == Status::Malformed, "长度字段吹牛");

    check(!decode(bytes_of({0x00, 0x11, 0x00, 0x00}), err).has_value(), "未知类型标记");
    check(!decode(bytes_of({0x00, 0x90, 0x00, 0x00, 0x05, 0x00, 0x00}), err).has_value(),
          "字符串截断（连长度前缀都不完整）");
    check(!decode(bytes_of({0x00, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}), err).has_value(),
          "字符串长度为 0（至少要含 NUL）");

    // 两种拦法都要：长度前缀虚高（第一道就挡住），以及前缀合法但 count 吹牛
    // ——后者更阴，因为它骗过了第一道检查，只留下「按 count 预留内存」这个坑。
    std::vector<uint8_t> lying_len = bytes_of({0x00, 0xf0, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00});
    check(!decode(lying_len, err).has_value(), "容器长度前缀虚高被拦: " + err);

    std::vector<uint8_t> lying_count = bytes_of({0x00, 0xf0, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
                                                 0xff, 0xff, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00});
    check(!decode(lying_count, err).has_value(), "count 与容器大小不符被拦: " + err);
    check(err.find("条目数") != std::string::npos, "报的是条目数不符，而不是长度前缀");

    std::string deep_err;
    Value leaf = make_null();
    for (int i = 0; i < 200; ++i) {
        Value wrapper = make_dict();
        dict_set(wrapper, "k", std::move(leaf));
        leaf = std::move(wrapper);
    }
    check(encode(leaf).empty(), "编码侧深度上限生效");
    const auto shallow = encode(make_dict());
    check(!shallow.empty(), "对照：浅字典正常编码");
}

void test_describe() {
    std::printf("\n== 可读化 ==\n");
    Value d = make_dict();
    dict_set(d, "Port", make_uint64(49152));
    dict_set(d, "SSLRequired", make_bool(false));
    dict_set(d, "Offset", make_int64(-5));
    dict_set(d, "Name", make_string(std::string(200, 'y')));
    const auto s = describe(d);
    check(s.find("Port: 49152") != std::string::npos, "整数字段: " + s);
    check(s.find("SSLRequired: false") != std::string::npos, "布尔字段");
    check(s.find("Offset: -5") != std::string::npos, "负 int64 不带符号就成 184467…");
    check(s.find("(200)") != std::string::npos && s.size() < 400, "超长字符串被截断");

    uint8_t u[16];
    for (int i = 0; i < 16; ++i) {
        u[i] = static_cast<uint8_t>(0xa0 + i);
    }
    check(describe(make_uuid(std::span<const uint8_t>(u, 16))) ==
              "a0a1a2a3-a4a5-a6a7-a8a9-aaabacadaeaf",
          "UUID 打成分组形式");
}

}  // namespace

int main() {
    test_scalars();
    test_containers();
    test_golden_decode();
    test_message_envelope();
    test_describe();
    std::printf("\n%s (失败 %d 项)\n", Failures == 0 ? "全部通过" : "存在失败", Failures);
    return Failures == 0 ? 0 : 1;
}
