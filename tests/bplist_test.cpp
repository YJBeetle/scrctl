// 二进制 plist 自检。方向各钉一遍：写出去的要能被规范实现读回来（开发时已用
// plistlib 对过 11 键全通过），读进来的要能消化规范实现写出的东西。
#include <cstdio>
#include <string>
#include <vector>

#include "plist/Bplist.h"

namespace {

int Failures = 0;

void check(bool ok, const std::string &what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) {
        ++Failures;
    }
}

using scrctl::plist::Kind;
using scrctl::plist::Value;

std::vector<uint8_t> unhex(std::string_view s) {
    auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    std::vector<uint8_t> v;
    for (std::size_t i = 0; i + 1 < s.size(); i += 2) {
        v.push_back(static_cast<uint8_t>(nib(s[i]) << 4 | nib(s[i + 1])));
    }
    return v;
}

std::string hex(const std::vector<uint8_t> &b) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string s;
    for (uint8_t v : b) {
        s += kHex[v >> 4];
        s += kHex[v & 0xF];
    }
    return s;
}

/// 规范实现产出的一个覆盖各类型的 bplist，用作读取方向的基准。
constexpr const char *kReference =
    "62706c6973743030d70102030405060708090a0b0c0d0e536269675566616c7365546d6f"
    "6465586e6567617469766553736563547472756555757466313613123456789abcdef008"
    "100513ffffffffffffffff5c706c61696e20737472696e670965542b4e2d658700202713"
    "08171b21262f33383e47484a5360610000000000000101000000000000000f0000000000"
    "000000000000000000006c";

void test_read_reference() {
    std::printf("\n== 读规范实现的产物 ==\n");
    std::string err;
    const auto raw = unhex(kReference);
    auto v = scrctl::plist::parse_binary(raw, &err);
    check(v.has_value(), "解析基准: " + err);
    if (!v) {
        return;
    }
    check(v->keys.size() == 7, "7 个键");
    check(v->find("mode")->integer == 5, "窄整数");
    check(v->find("negative")->integer == -1, "负数按 8 字节形态符号扩展");
    check(v->find("big")->integer == 0x123456789ABCDEF0LL, "8 字节大整数");
    check(v->find("true")->boolean && !v->find("false")->boolean, "布尔");
    check(v->find("sec")->string == "plain string", "ASCII 串");
    check(v->find("utf16")->string == "含中文 ✓", "UTF-16BE 转成 UTF-8");
}

void test_write_shapes() {
    std::printf("\n== 写出的字节形态 ==\n");
    auto d = Value::Dict();
    d.set("k", Value::Bool(true));
    // 3 个对象：字典(d1 + 键引用 01 + 值引用 02)、键 "k"、值 true。
    // 偏移表 08/0b/0d 指向三个对象的起点，尾部给出宽度与表位置。
    check(hex(scrctl::plist::write_binary(d)) ==
              "62706c6973743030" "d10102" "516b" "09" "080b0d"
              "000000000000" "0101" "0000000000000003" "0000000000000000"
              "000000000000000e",
          "最小字典 {k: true}");

    auto n = Value::Dict();
    n.set("a", Value::Int(0));
    n.set("b", Value::Int(255));
    n.set("c", Value::Int(256));
    n.set("d", Value::Int(-2));
    const auto wide = scrctl::plist::write_binary(n);
    std::string err;
    auto back = scrctl::plist::parse_binary(wide, &err);
    check(back.has_value(), "多宽度整数可解回: " + err);
    if (back) {
        check(back->find("a")->integer == 0 && back->find("b")->integer == 255 &&
                  back->find("c")->integer == 256 && back->find("d")->integer == -2,
              "整数宽度选择后值不变");
    }
    // 负数必须占满 8 字节：窄形态没有"有符号"这回事，0xFFFFFFFE 会被读成
    // 4294967294，两个实现就此分道扬镳。
    check(hex(wide).find("13fffffffffffffffe") != std::string::npos, "负数写成 0x13 + 8 字节");
    // 窄形态是无符号的：0x11 ff 就是 255，不是 -1。
    check(hex(wide).find("10ff") != std::string::npos, "255 写成 0x10 + ff（1 字节宽度对应 log2=0）");
    auto signed_back = scrctl::plist::parse_binary(wide);
    check(signed_back && signed_back->find("b")->integer == 255, "1 字节整数按无符号读回 255");

    auto arr = Value::Array();
    for (int i = 0; i < 40; ++i) {
        arr.push(Value::Int(i));
    }
    const auto long_hex = hex(scrctl::plist::write_binary(arr));
    check(long_hex.find("af1028") != std::string::npos, "长度 40 走 0xF + 0x10 + 0x28 长形态");
    auto la = scrctl::plist::parse_binary(unhex(long_hex));
    check(la && la->array.size() == 40 && la->array[39].integer == 39, "长形态计数读得回来");

    auto uni = Value::Dict();
    uni.set("中文键", Value::Str("含非 ASCII ✓"));
    const auto ub = scrctl::plist::write_binary(uni);
    auto ub_back = scrctl::plist::parse_binary(ub);
    check(ub_back && ub_back->find("中文键")->string == "含非 ASCII ✓", "非 ASCII 键与值往返");
    check(hex(ub).find("634e2d6587") != std::string::npos, "非 ASCII 键走 0x63 + UTF-16BE");

    // 星外平面：一个 UTF-16 代理对，两个 UTF-16 单元
    auto emoji = Value::Str(std::string("A") + "\U0001F600");
    const auto eb = scrctl::plist::write_binary(emoji);
    auto eb_back = scrctl::plist::parse_binary(eb);
    check(eb_back && eb_back->string == "A\U0001F600", "代理对往返不失真");

    auto real = Value::Dict();
    real.set("r", Value{Kind::Real, false, 0, 2.25, "", {}, {}, {}});
    auto rb = scrctl::plist::parse_binary(scrctl::plist::write_binary(real));
    check(rb && rb->find("r")->real == 2.25, "8 字节浮点往返");
}

void test_dict_key_order_is_stable() {
    std::printf("\n== 字典键序 ==\n");
    auto a = Value::Dict();
    a.set("z", Value::Int(1));
    a.set("a", Value::Int(2));
    auto b = Value::Dict();
    b.set("a", Value::Int(2));
    b.set("z", Value::Int(1));
    check(scrctl::plist::write_binary(a) == scrctl::plist::write_binary(b),
          "插入序不同、内容相同的字典写出同一份字节（键排序）");
}

void test_reject_malformed() {
    std::printf("\n== 必须拒绝的畸形输入 ==\n");
    std::string err;
    check(!scrctl::plist::parse_binary(unhex("0011223344"), &err).has_value(), "缺魔数");
    check(!scrctl::plist::parse_binary(unhex("62706c6973743030"), &err).has_value(), "只有魔数");

    const auto good = scrctl::plist::write_binary([] {
        auto d = Value::Dict();
        d.set("k", Value::Str("v"));
        return d;
    }());
    for (std::size_t cut = 8; cut + 32 < good.size(); ++cut) {
        std::string e;
        if (scrctl::plist::parse_binary(std::vector<uint8_t>(good.begin(), good.begin() + cut), &e)
                .has_value()) {
            check(false, "截断到 " + std::to_string(cut) + " 字节时应失败");
            return;
        }
    }
    check(true, "每种截断长度都被拒绝");

    auto bad_ref = good;
    bad_ref[8] = 0xD5;  // 声明 5 对键值，可后面只写了 1 对的引用
    check(!scrctl::plist::parse_binary(bad_ref, &err).has_value(),
          "引用数超出剩余字节被拒: " + err);

    auto bad_offset = good;
    bad_offset[bad_offset.size() - 10] = 0x7f;  // 偏移表位置推到文件外
    check(!scrctl::plist::parse_binary(bad_offset, &err).has_value(),
          "偏移表越界被拒: " + err);

    auto bad_width = good;
    bad_width[bad_width.size() - 26] = 0x40;  // offset_size 写成 64
    check(!scrctl::plist::parse_binary(bad_width, &err).has_value(),
          "荒谬的宽度声明被拒: " + err);

    // 自引用字典：0 号对象的值指回自己。没有深度上限就是无限递归。
    const std::string cycle =
        "62706c6973743030"   // magic
        "d10100"             // 字典，1 对；键=1，值=0（自己）
        "516b"               // 对象 1：键 "k"
        "00000000000000"     // 尾部 6 个未用字节
        "0101" "0000000000000002" "0000000000000000" "0000000000000012";
    check(!scrctl::plist::parse_binary(unhex(cycle), &err).has_value(),
          "自引用被深度上限拦下: " + err);
}

}  // namespace

int main() {
    test_read_reference();
    test_write_shapes();
    test_dict_key_order_is_stable();
    test_reject_malformed();
    std::printf("\n%s (失败 %d 项)\n", Failures == 0 ? "全部通过" : "存在失败", Failures);
    return Failures == 0 ? 0 : 1;
}
