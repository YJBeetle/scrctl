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
    /// 列表里的一项。`path` 是 App 在设备上的安装目录，`stop()` 拿它去对进程的可执行
    /// 文件路径。
    struct Entry {
        std::string bundle_id;
        std::string path;
        std::string name;
    };

    /// 启动一个 App。`terminate_existing=true` 会先把在跑的实例杀掉再冷启动——想要
    /// "从这个 App 的初始状态开始"时才用。代价是每次都制造一次"刚杀完就起"的竞态
    /// （实测那样有概率回 code 10004 且 App 真的没起来；launch() 内部会重试，但重试
    /// 换来的是几百毫秒的抖动）。默认 false：只唤起、不动在跑的实例。
    static bool launch(Device &device, const std::string &bundle_id, std::string &err,
                       bool terminate_existing = true, bool verbose = false);

    /// 杀掉一个 App（SIGKILL 给它的所有进程）。App 没在跑时也算成功——Android 那边
    /// `am force-stop` 就是这个语义，调用方不该因为"本来就没开"收到失败。
    static bool stop(Device &device, const std::string &bundle_id, std::string &err,
                     bool verbose = false);

    /// 装上/系统自带的 App 列表。
    ///
    /// 走的是 **streamapplist**，不是 listapps：后者把 239 个 App 塞进一个回信，
    /// 而大回复正是我们传不稳的那一类（实测任一 include* 为 true 时 60 秒不回话，
    /// 而全部为 false 时秒回一个空列表——所以卡住的从来不是那个开关，是尺寸）。
    static bool list(Device &device, std::vector<Entry> &out, std::string &err,
                     bool verbose = false);

    /// 构造 launchapplication 的 input。拆出来是为了能离线自检：这套键值的任何
    /// 一处写错，设备的表现都不是"报错"而是退到别的分支给一句误导的话。
    [[nodiscard]] static xpc::Value build_launch(const std::string &bundle_id,
                                                 bool terminate_existing);

    /// 从 listprocesses 的回信里挑出安装目录落在 `app_path` 下的进程号。
    /// 拆出来是为了能离线钉住：这条匹配写错的表现是"stop 返回真而 App 还活着"，
    /// 线上看不出来。
    static std::vector<int64_t> matching_pids(const xpc::Value &processes,
                                              const std::string &app_path);
};

}  // namespace scrctl::remote
