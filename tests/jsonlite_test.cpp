// JSON 子集自检。隧道握手与后续 RemoteXPC 元数据都靠它。
#include <cstdio>
#include <string>

#include "jsonlite/Jsonlite.h"

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

}  // namespace

int main() {
    std::printf("== 隧道握手响应 ==\n");
    std::string err;
    auto v = scrctl::json::parse(kHandshake, &err);
    check(v.has_value(), "解析握手响应: " + err);
    if (v) {
        check(v->find("serverRSDPort")->as_int_or(0) == 58425, "整数端口");
        check(v->find("serverAddress")->as_string_or("") == "fd78:6049:c266::1", "IPv6 字符串");
        const auto *cp = v->find("clientParameters");
        check(cp != nullptr && cp->find("address")->as_string_or("") == "fd78:6049:c266::2",
              "嵌套 object 取值");
        check(v->find("Nope") == nullptr, "不存在的键返回 nullptr");
    }

    std::printf("\n== 标量与转义 ==\n");
    auto scalars = scrctl::json::parse(R"({"a":1,"b":-2,"c":1.5,"d":true,"e":false,"f":null})");
    check(scalars.has_value(), "混合标量 object");
    if (scalars) {
        check(scalars->find("b")->as_int_or(0) == -2, "负整数");
        check(scalars->find("c")->kind == scrctl::json::Kind::Double, "小数走 Double");
        check(scalars->find("d")->boolean, "true");
        check(!scalars->find("e")->boolean, "false");
        check(scalars->find("f")->kind == scrctl::json::Kind::Null, "null");
    }

    auto esc = scrctl::json::parse(R"j("q\"b\\n\nuAéВ")j");
    check(esc.has_value() && esc->as_string_or("") == "q\"b\\n\nuAéВ",
          "转义与 \\u（含 BMP 与代理对）");

    auto arr = scrctl::json::parse("[1,2,[3,4],{\"k\":[5]}]");
    check(arr.has_value() && arr->array.size() == 4 &&
              arr->array[2].array.size() == 2,
          "嵌套数组");

    auto empty = scrctl::json::parse(R"({"a":[],"b":{}})");
    check(empty.has_value() && empty->find("a")->array.empty() &&
              empty->find("b")->object.empty(),
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
