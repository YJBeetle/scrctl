#include "remote/Pasteboard.h"

#include <cstdio>
#include <vector>

#include "remote/Device.h"

namespace scrctl::remote {
namespace {

constexpr std::string_view kServiceName = "com.apple.coredevice.pasteboardservice";
constexpr std::string_view kUti = "public.utf8-plain-text";

}  // namespace

xpc::Value Pasteboard::build_pull() {
    auto msg = xpc::make_dict();
    xpc::dict_set(msg, "command", xpc::make_string("PULL"));
    xpc::dict_set(msg, "pasteboardName", xpc::make_string("general"));
    // dataPolicy 是"只带一个键的枚举"：空字典或两个键都被拒（"Invalid number of
    // keys found, expected one."）。allResolved = 把每种表示形式都内联带回来。
    auto all_resolved = xpc::make_dict();
    auto data_policy = xpc::make_dict();
    xpc::dict_set(data_policy, "allResolved", std::move(all_resolved));
    xpc::dict_set(msg, "dataPolicy", std::move(data_policy));
    return msg;
}

xpc::Value Pasteboard::build_set(const std::string &text) {
    auto representation = xpc::make_dict();
    xpc::dict_set(representation, "data",
                  xpc::make_data(std::vector<uint8_t>(text.begin(), text.end())));

    auto data = xpc::make_dict();
    xpc::dict_set(data, std::string(kUti), std::move(representation));

    auto types = xpc::make_array();
    xpc::array_push(types, xpc::make_string(std::string(kUti)));

    auto item = xpc::make_dict();
    xpc::dict_set(item, "types", std::move(types));
    xpc::dict_set(item, "data", std::move(data));

    auto items = xpc::make_array();
    xpc::array_push(items, std::move(item));

    auto msg = xpc::make_dict();
    xpc::dict_set(msg, "command", xpc::make_string("SET"));
    xpc::dict_set(msg, "pasteboardName", xpc::make_string("general"));
    xpc::dict_set(msg, "items", std::move(items));
    return msg;
}

const std::string *Pasteboard::find_text(const xpc::Value &reply) {
    static std::string holder;  // 返回的是指向这里的指针，调用方要当场用完
    const auto *snapshot = reply.find("pasteboard");
    if (snapshot == nullptr) {
        return nullptr;
    }
    const auto *items = snapshot->find("items");
    if (items == nullptr) {
        return nullptr;
    }
    for (const auto &item : items->array) {
        const auto *data = item.find("data");
        if (data == nullptr) {
            continue;
        }
        const auto *rep = data->find(kUti);
        if (rep == nullptr) {
            continue;
        }
        const auto *bytes = rep->find("data");
        if (bytes == nullptr || bytes->data.empty()) {
            continue;
        }
        holder.assign(bytes->data.begin(), bytes->data.end());
        return &holder;
    }
    return nullptr;
}

bool Pasteboard::set_text(Device &device, const std::string &text, std::string &err,
                          bool verbose) {
    auto conn = device.connect(kServiceName, err, verbose);
    if (conn == nullptr) {
        return false;
    }
    xpc::Value reply;
    if (!conn->call(build_set(text), reply, 20000, err)) {
        err = "SET 失败: " + err;
        return false;
    }
    // 设备对形状不对的 SET 也可能回个 SET_REPLY，所以命令名要核一下。
    if (reply.at("command").as_string_or("") != "SET_REPLY") {
        err = "SET 的回信不是 SET_REPLY: " + xpc::describe(reply).substr(0, 300);
        return false;
    }
    return true;
}

bool Pasteboard::get_text(Device &device, std::string &out, std::string &err, bool verbose) {
    auto conn = device.connect(kServiceName, err, verbose);
    if (conn == nullptr) {
        return false;
    }
    xpc::Value reply;
    if (!conn->call(build_pull(), reply, 20000, err)) {
        err = "PULL 失败: " + err;
        return false;
    }
    const auto *text = find_text(reply);
    if (text == nullptr) {
        // 剪贴板是空的、里面只有图片、或者我们的请求形状不对，都走这一支。
        // 把回信带上：不然"没读到"和"设备其实在报错"分不开。
        err = "回信里没有纯文本表示形式: " + xpc::describe(reply).substr(0, 400);
        return false;
    }
    out = *text;
    return true;
}

}  // namespace scrctl::remote
