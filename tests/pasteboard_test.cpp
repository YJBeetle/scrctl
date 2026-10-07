// 剪贴板消息形状的离线自检。
//
// 为什么必须有：SET 的形状不对时设备的表现是**回一个 SET_REPLY 表示成功**，然后
// 把内容丢掉——PULL 回来的条目里 data 是空的。线上完全看不出问题，所以只能在
// 这里把结构钉住。
#include <cstdio>
#include <string>
#include <vector>

#include "remote/Pasteboard.h"
#include "xpc/XpcValue.h"

namespace {

int Failures = 0;

void check(bool ok, const std::string &what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) {
        ++Failures;
    }
}

}  // namespace

int main() {
    using scrctl::remote::Pasteboard;
    using namespace scrctl::xpc;

    std::printf("== SET 的形状 ==\n");
    const auto set = Pasteboard::build_set("你好 scrctl");
    check(set.at("command").as_string_or("") == "SET", "command=SET");
    check(set.at("pasteboardName").as_string_or("") == "general", "pasteboardName=general");
    const auto *items = set.find("items");
    check(items != nullptr && items->is_array() && items->array.size() == 1,
          "items 是单元素数组");
    if (items != nullptr && items->array.size() == 1) {
        const auto &item = items->array[0];
        const auto *types = item.find("types");
        // 这条是实测换来的：types 为空时设备照样回 SET_REPLY，但内容被静默丢掉。
        check(types != nullptr && types->is_array() && !types->array.empty(),
              "types 非空（空了会被静默丢弃）");
        if (types != nullptr && !types->array.empty()) {
            check(types->array[0].as_string_or("") == "public.utf8-plain-text",
                  "types[0] 是 utf8 的 UTI");
        }
        const auto *data = item.find("data");
        const auto *rep = data == nullptr ? nullptr : data->find("public.utf8-plain-text");
        const auto *bytes = rep == nullptr ? nullptr : rep->find("data");
        check(bytes != nullptr && !bytes->data.empty(), "data 里是原生 Data 而不是 base64");
        if (bytes != nullptr) {
            const std::string got(bytes->data.begin(), bytes->data.end());
            check(got == "你好 scrctl", "字节原样往返: " + got);
        }
    }

    std::printf("\n== PULL 的形状 ==\n");
    const auto pull = Pasteboard::build_pull();
    check(pull.at("command").as_string_or("") == "PULL", "command=PULL");
    const auto *policy = pull.find("dataPolicy");
    // dataPolicy 是"只带一个键的枚举"：空字典或两个键都设备直接拒。
    check(policy != nullptr && policy->is_dict() && policy->dict.size() == 1,
          "dataPolicy 恰好一个键");
    check(policy != nullptr && policy->find("allResolved") != nullptr, "那个键是 allResolved");

    std::printf("\n== 从回信里取文本 ==\n");
    check(!Pasteboard::find_text(pull), "不是 PULL_REPLY 的回信取不出文本");
    Value reply = make_dict();
    dict_set(reply, "command", make_string("PULL_REPLY"));
    check(!Pasteboard::find_text(reply), "没有 pasteboard 字段时没有文本结果");

    const auto text_reply = [](const std::string &text) {
        auto result = scrctl::xpc::make_dict();
        scrctl::xpc::dict_set(result, "pasteboard", Pasteboard::build_set(text));
        return result;
    };
    const auto first = Pasteboard::find_text(text_reply("第一段文本"));
    check(first && *first == "第一段文本", "提取 UTF-8 文本");
    const auto second = Pasteboard::find_text(text_reply("第二段文本"));
    check(second && *second == "第二段文本", "提取第二段文本");
    check(first && *first == "第一段文本", "后续提取不改写此前结果");
    const auto empty_text = Pasteboard::find_text(text_reply(""));
    check(empty_text && empty_text->empty(), "空 Data 是存在的空文本，不是缺少文本表示");
    const std::string embedded_nul("A\0B", 3);
    const auto binary_text = Pasteboard::find_text(text_reply(embedded_nul));
    check(binary_text && *binary_text == embedded_nul, "文本按 Data 长度读取，保留内嵌零字节");
    auto invalid_snapshot = Pasteboard::build_set("text");
    auto invalid_items = invalid_snapshot.at("items");
    auto &item = invalid_items.array.front();
    auto wrong_data = make_dict();
    auto wrong_representation = make_dict();
    dict_set(wrong_representation, "data", make_string("not Data"));
    dict_set(wrong_data, "public.utf8-plain-text", std::move(wrong_representation));
    dict_set(item, "data", std::move(wrong_data));
    dict_set(invalid_snapshot, "items", std::move(invalid_items));
    auto wrong_type_reply = make_dict();
    dict_set(wrong_type_reply, "pasteboard", std::move(invalid_snapshot));
    check(!Pasteboard::find_text(wrong_type_reply), "同名字段不是 Data 时不当作空文本");

    std::printf("\n%s (失败 %d 项)\n", Failures == 0 ? "全部通过" : "存在失败", Failures);
    return Failures == 0 ? 0 : 1;
}
