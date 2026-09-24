#pragma once

#include <string>

#include "remote/Rsd.h"

namespace scrctl::remote {

class Device;

/// 启停设备上的 App。
///
/// `appservice` 吃标准 CoreDevice 外壳（feature + action），但请求体的形状有两处
/// 反直觉，都是"照着字段名猜一定猜错"的地方：
///
/// ```text
/// { applicationSpecifier: { bundleIdentifier: { _0: "com.apple.mobilesafari" } },
///   options: { arguments: [], environmentVariables: {},
///              standardIOUsesPseudoterminals: true, startStopped: false,
///              terminateExisting: true, user: { shortName: "mobile" },
///              platformSpecificOptions: <Data: 一段 plist> },
///   standardIOIdentifiers: {} }
/// ```
///
/// 1. **bundle id 在顶层的 `applicationSpecifier` 里，不在 `options` 里**，而且它
///    自己还要再套一层 `_0`（XPC 里带关联值的枚举 case 就是这个形状）。之前把
///    bundle id / url 往 `options` 里塞，设备一律回
///    "A URL to open must be specified in the launch options."——因为一个 specifier
///    都没认出来时，它退回到"按 URL 启动"那条分支去要 url。
/// 2. **`platformSpecificOptions` 不能是零长 Data**：设备回 "Cannot parse a NULL or
///    zero-length data"。它是一段 plist（这里给空字典的 plist），不是字符串。
class App {
public:
    /// 启动一个 App。terminate_existing=true 时先把在跑的实例杀掉——自动化要的
    /// 是"从这个 App 的冷启动状态开始"，而不是"回到它上次停下的界面"。
    static bool launch(Device &device, const std::string &bundle_id, std::string &err,
                       bool terminate_existing = true, bool verbose = false);

    /// 构造 launchapplication 的 input。拆出来是为了能离线自检：这套键值的任何
    /// 一处写错，设备的表现都不是"报错"而是退到别的分支给一句误导的话。
    [[nodiscard]] static xpc::Value build_launch(const std::string &bundle_id,
                                                 bool terminate_existing);
};

}  // namespace scrctl::remote
