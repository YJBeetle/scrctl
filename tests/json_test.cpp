// JSON 协议边界自检。隧道握手与后续 RemoteXPC 元数据都靠它。
#include <cstdio>
#include <string>

#include "json/Json.h"

namespace {

int Failures = 0;

void check(bool ok, const std::string &what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) {
        ++Failures;
    }
}

constexpr const char *kHandshake = R"({
  "type": "serverHandshakeResponse",
  "serverAddress": "fd78:6049:c266::1",
  "serverRSDPort": 58425,
  "mtu": 16000,
  "clientParameters": { "address": "fd78:6049:c266::2", "mtu": 16000 }
})";

} // namespace

int main() {
    std::printf("== 隧道握手响应 ==\n");
    std::string err;
    auto v = scrctl::json::parse(kHandshake, &err);
    check(v.has_value(), "解析握手响应: " + err);
    if (v) {
        check(scrctl::json::as_int_or(*scrctl::json::find(*v, "serverRSDPort"), 0) == 58425,
              "整数端口");
        check(scrctl::json::as_string_or(*scrctl::json::find(*v, "serverAddress"), "") ==
                  "fd78:6049:c266::1",
              "IPv6 字符串");
        const auto *cp = scrctl::json::find(*v, "clientParameters");
        check(cp != nullptr && scrctl::json::as_string_or(*scrctl::json::find(*cp, "address"),
                                                          "") == "fd78:6049:c266::2",
              "嵌套 object 取值");
        check(scrctl::json::find(*v, "Nope") == nullptr, "不存在的键返回 nullptr");
    }

    std::printf("\n== 标量与转义 ==\n");
    auto scalars = scrctl::json::parse(R"({"a":1,"b":-2,"c":1.5,"d":true,"e":false,"f":null})");
    check(scalars.has_value(), "混合标量 object");
    if (scalars) {
        check(scrctl::json::as_int_or(*scrctl::json::find(*scalars, "b"), 0) == -2, "负整数");
        check(scrctl::json::find(*scalars, "c")->is_number_float(), "小数走 Double");
        check(scrctl::json::find(*scalars, "d")->get<bool>(), "true");
        check(!scrctl::json::find(*scalars, "e")->get<bool>(), "false");
        check(scrctl::json::find(*scalars, "f")->is_null(), "null");
    }

    auto esc = scrctl::json::parse(R"j("q\"b\\n\nuAéВ")j");
    check(esc.has_value() && scrctl::json::as_string_or(*esc, "") == "q\"b\\n\nuAéВ",
          "转义与 \\u（含 BMP 与代理对）");

    auto arr = scrctl::json::parse("[1,2,[3,4],{\"k\":[5]}]");
    check(arr.has_value() && arr->size() == 4 && (*arr)[2].size() == 2, "嵌套数组");

    auto empty = scrctl::json::parse(R"({"a":[],"b":{}})");
    check(empty.has_value() && scrctl::json::find(*empty, "a")->empty() &&
              scrctl::json::find(*empty, "b")->empty(),
          "空数组与空对象");

    // 10 层嵌套远在深度上限之内，必须正常接受。
    auto nested = scrctl::json::parse("[[[[[[[[[[1]]]]]]]]]]");
    check(nested.has_value(), "10 层嵌套应被正常接受");

    std::printf("\n== 必须拒绝的畸形输入 ==\n");
    check(!scrctl::json::parse("{").has_value(), "截断的 object");
    check(!scrctl::json::parse(R"({"a"})").has_value(), "缺值的键");
    check(!scrctl::json::parse(R"({"a":1,})").has_value(), "尾随逗号");
    check(!scrctl::json::parse(R"([1] x)").has_value(), "尾部多余内容");
    check(!scrctl::json::parse(R"j("unterminated)j").has_value(), "未闭合字符串");

    std::string deep = "[";
    for (int i = 0; i < 200; ++i) {
        deep += "[";
    }
    deep += "1";
    for (int i = 0; i < 201; ++i) {
        deep += "]";
    }
    check(!scrctl::json::parse(deep).has_value(), "超深嵌套被深度上限拦下");

    for (const auto *bad : {"01", "+1", "1e", "1-2", "1.2.3", R"("\uD800")"}) {
        check(!scrctl::json::parse(bad), std::string("非法 JSON 必须拒绝: ") + bad);
    }
    check(!scrctl::json::parse(std::string("\"line\nfeed\"")), "字符串内原始换行必须拒绝");
    check(!scrctl::json::parse(std::string("\"") + static_cast<char>(0xff) + "\""),
          "非法 UTF-8 必须拒绝");
    check(!scrctl::json::parse(std::string((4u << 20) + 1, ' ')), "输入大小上限");
    const std::string depth64 = std::string(64, '[') + "0" + std::string(64, ']');
    check(scrctl::json::parse(depth64).has_value(), "64 层边界应接受");
    check(!scrctl::json::parse("[" + depth64 + "]"), "65 层边界应拒绝");
    const auto wide = scrctl::json::parse("18446744073709551615");
    check(wide && scrctl::json::as_int_or(*wide, -1) == -1, "uint64 溢出不能变成负整数");
    check(scrctl::json::as_int_or(scrctl::json::Value(0x1p63), -1) == -1,
          "浮点整数越界返回 fallback");
    check(scrctl::json::find(scrctl::json::Value::array(), "x") == nullptr, "非对象查键不抛异常");

    std::printf("\n== 往返 ==\n");
    auto src = scrctl::json::parse(kHandshake);
    check(src.has_value(), "源可解析");
    if (src) {
        auto again = scrctl::json::parse(scrctl::json::write(*src));
        check(again.has_value() && scrctl::json::write(*again) == scrctl::json::write(*src),
              "write -> parse -> write 稳定");
    }

    std::printf("\n%s (失败 %d 项)\n", Failures == 0 ? "全部通过" : "存在失败", Failures);
    return Failures == 0 ? 0 : 1;
}
