#include "remote/DisplayInfo.h"

#include <cstdio>
#include <random>
#include <span>

#include "remote/Device.h"

namespace scrctl::remote {
namespace {

constexpr std::string_view kService = "com.apple.coredevice.deviceinfo";
constexpr std::string_view kFeature = "com.apple.coredevice.feature.displayinfoupdates";

/// 取一个整数，Int64 / UInt64 / **Double** 三种都认。
///
/// 为什么必须认 Double：这条推送里凡是几何的数字全是浮点——实测
/// `currentMode.size` 是 `[Double, Double]`（CoreGraphics 的 CGFloat 就是 double），
/// 而 `displayId` 是 UInt64、`preferredUIScale` 是 Int64。第一版只认两种整数，
/// 结果 `size` 一个都取不到，产品路径上的表现是"问设备问到了个 0x0，退回兜底表"，
/// 而 `describe` 打出来 `1125` 与 `1125.0` 长得一模一样，看日志根本发现不了。
/// 类型要问机器（display_info_probe 会把每个叶子的 XPC 类型打出来），不能看
/// 打印的样子猜。
bool as_int(const xpc::Value *v, long long &out) {
    if (v == nullptr) {
        return false;
    }
    switch (v->type) {
        case xpc::Type::Int64:
            out = v->int64;
            return true;
        case xpc::Type::UInt64:
            out = static_cast<long long>(v->uint64);
            return true;
        case xpc::Type::Double:
            // 尺寸这类值本来就是整数值，只是用 CGFloat 装。四舍五入而不是截断：
            // 编码链路里 1124.9999 这种表示误差是可能出现的。
            out = static_cast<long long>(v->real >= 0 ? v->real + 0.5 : v->real - 0.5);
            return true;
        default:
            return false;
    }
}

bool as_bool(const xpc::Value *v, bool &out) {
    if (v == nullptr || v->type != xpc::Type::Bool) {
        return false;
    }
    out = v->boolean;
    return true;
}

std::string as_string(const xpc::Value *v) {
    return v != nullptr && v->type == xpc::Type::String ? v->string : std::string();
}

/// 读 `[w, h]` 这一对。尺寸、bounds、frame 都是这个形状，而且**元素是 Double**。
bool as_pair(const xpc::Value *v, int &w, int &h) {
    if (v == nullptr || v->type != xpc::Type::Array || v->array.size() < 2) {
        return false;
    }
    long long a = 0, b = 0;
    if (!as_int(&v->array[0], a) || !as_int(&v->array[1], b)) {
        return false;
    }
    w = static_cast<int>(a);
    h = static_cast<int>(b);
    return true;
}

}  // namespace

const Display *DisplayInfo::find(uint64_t id) const {
    for (const auto &d : displays) {
        if (d.id == id) {
            return &d;
        }
    }
    return nullptr;
}

const Display *DisplayInfo::primary() const {
    for (const auto &d : displays) {
        if (d.primary) {
            return &d;
        }
    }
    return nullptr;
}

std::optional<DisplayInfo> parse_display_info(const xpc::Value &element, std::string &err) {
    const auto *list = element.find("displays");
    if (list == nullptr || list->type != xpc::Type::Array) {
        err = "推送里没有 displays 数组：" + xpc::describe(element).substr(0, 200);
        return std::nullopt;
    }
    DisplayInfo info;
    for (const auto &raw : list->array) {
        const auto *id = raw.find("displayId");
        if (id == nullptr) {
            continue;  // 没有 id 的那条我们无从对应，跳过而不是整包判死
        }
        long long raw_id = 0;
        if (!as_int(id, raw_id) || raw_id < 0) {
            continue;
        }
        Display d;
        d.id = static_cast<uint64_t>(raw_id);
        as_bool(raw.find("primary"), d.primary);
        as_bool(raw.find("external"), d.external);
        d.name = as_string(raw.find("name"));
        if (d.name.empty()) {
            // 无线屏那几条同时给了 deviceName 与 name，主屏只给了 name。
            d.name = as_string(raw.find("deviceName"));
        }
        // 可见区尺寸在**当前模式**里，不在显示器这一层：`nativeSize` 是面板物理像素
        // （这台设备 1080x2340），`currentMode.size` 才是画面真正占的那一块（1125x2436）。
        const auto *mode = raw.find("currentMode");
        int w = 0, h = 0;
        if (mode != nullptr && as_pair(mode->find("size"), w, h)) {
            d.width = w;
            d.height = h;
        }
        d.orientation = as_string(raw.find("currentOrientation"));
        info.displays.push_back(std::move(d));
    }
    if (const auto *o = element.find("orientation"); o != nullptr) {
        info.device_orientation = as_string(o->find("currentDeviceOrientation"));
    }
    if (info.displays.empty()) {
        err = "displays 里一条能认的都解不出来";
        return std::nullopt;
    }
    return info;
}

std::optional<DisplayInfo> fetch_display_info(Device &device, std::string &err, bool verbose) {
    auto conn = device.connect(kService, err, verbose);
    if (conn == nullptr) {
        return std::nullopt;
    }
    // 流式 feature 的消息体：参数裹在 actualInput 下，外加一个客户端自己生成的
    // sideChannel UUID（设备拿它认这条订阅归谁）。形状与 streamapplist 那条一致。
    std::vector<uint8_t> side(16);
    for (auto &b : side) {
        b = static_cast<uint8_t>(std::random_device {} ());
    }
    auto proxy = xpc::make_dict();
    xpc::dict_set(proxy, "sideChannel", xpc::make_uuid(std::span<const uint8_t>(side)));
    auto input = xpc::make_dict();
    xpc::dict_set(input, "actualInput", xpc::make_dict());
    xpc::dict_set(input, "streamProxy", std::move(proxy));

    std::optional<DisplayInfo> out;
    std::string parse_err;
    const auto r = conn->stream(
        kFeature, "", input,
        [&](const xpc::Value &element) {
            out = parse_display_info(element, parse_err);
            return out == std::nullopt;  // 解出来就收工；解不出来接着等下一条
        },
        6000, err);
    if (out != std::nullopt) {
        return out;
    }
    if (r == CallResult::Ok && parse_err.empty()) {
        err = std::string(kFeature) + " 没推任何一条就结束了";
    } else if (!parse_err.empty()) {
        err = parse_err;
    }
    return std::nullopt;
}

}  // namespace scrctl::remote
