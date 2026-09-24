#include "remote/App.h"

#include <vector>

#include "plist/Plist.h"
#include "remote/Device.h"

namespace scrctl::remote {
namespace {

constexpr std::string_view kService = "com.apple.coredevice.appservice";
constexpr std::string_view kFeature = "com.apple.coredevice.feature.launchapplication";
constexpr std::string_view kAction = "com.apple.coredevice.action.launch";

/// 空字典的 plist。`platformSpecificOptions` 要的是 Data，而零长 Data 会被拒
/// （"Cannot parse a NULL or zero-length data"），所以哪怕内容什么都没有，也得是
/// 一段解析得动的 plist。
std::vector<uint8_t> empty_plist() {
    const std::string text = plist::write(plist::Value::Dict());
    return {text.begin(), text.end()};
}

}  // namespace

xpc::Value App::build_launch(const std::string &bundle_id, bool terminate_existing) {
    // bundle id 套一层 _0：这是 XPC 里"带关联值的枚举 case"的通用形状，设备侧
    // applicationSpecifier 就是一个枚举（bundleIdentifier / url / path 三选一）。
    auto which = xpc::make_dict();
    xpc::dict_set(which, "_0", xpc::make_string(bundle_id));
    auto specifier = xpc::make_dict();
    xpc::dict_set(specifier, "bundleIdentifier", std::move(which));

    auto options = xpc::make_dict();
    xpc::dict_set(options, "arguments", xpc::make_array());
    xpc::dict_set(options, "environmentVariables", xpc::make_dict());
    xpc::dict_set(options, "standardIOUsesPseudoterminals", xpc::make_bool(true));
    xpc::dict_set(options, "startStopped", xpc::make_bool(false));
    xpc::dict_set(options, "terminateExisting", xpc::make_bool(terminate_existing));
    auto user = xpc::make_dict();
    xpc::dict_set(user, "shortName", xpc::make_string("mobile"));
    xpc::dict_set(options, "user", std::move(user));
    xpc::dict_set(options, "platformSpecificOptions",
                  xpc::make_data(empty_plist()));

    auto input = xpc::make_dict();
    xpc::dict_set(input, "applicationSpecifier", std::move(specifier));
    xpc::dict_set(input, "options", std::move(options));
    xpc::dict_set(input, "standardIOIdentifiers", xpc::make_dict());
    return input;
}

bool App::launch(Device &device, const std::string &bundle_id, std::string &err,
                 bool terminate_existing, bool verbose) {
    const auto input = build_launch(bundle_id, terminate_existing);
    xpc::Value output;
    // 60 秒：这条 RPC 在设备上要做杀旧实例 + 起新实例 + 等进程报告，10 秒的默认值
    // 不够（同一台设备上 listapps 那种调用就会顶到几十秒）。
    if (!device.feature(kService, kFeature, kAction, input, output, err, verbose, 60000)) {
        return false;
    }
    return true;
}

}  // namespace scrctl::remote
