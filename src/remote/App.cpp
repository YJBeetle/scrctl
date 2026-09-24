#include "remote/App.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>
#include <random>
#include <vector>

#include "plist/Plist.h"
#include "remote/Device.h"

namespace scrctl::remote {
namespace {

constexpr std::string_view kService = "com.apple.coredevice.appservice";
constexpr std::string_view kFeature = "com.apple.coredevice.feature.launchapplication";
constexpr std::string_view kAction = "com.apple.coredevice.action.launch";
constexpr int kSigKill = 9;

/// 空字典的 plist。`platformSpecificOptions` 要的是 Data，而零长 Data 会被拒
/// （"Cannot parse a NULL or zero-length data"），所以哪怕内容什么都没有，也得是
/// 一段解析得动的 plist。
std::vector<uint8_t> empty_plist() {
    const std::string text = plist::write(plist::Value::Dict());
    return {text.begin(), text.end()};
}

/// file:///private/var/... -> /private/var/...
/// 设备的 executableURL 带 scheme，而 app 列表里的 path 不带，不对齐就永远匹配不上。
std::string strip_file_scheme(std::string_view url) {
    constexpr std::string_view kPrefix = "file://";
    if (url.starts_with(kPrefix)) {
        url.remove_prefix(kPrefix.size());
    }
    return std::string(url);
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
    // 刚被杀掉的 App 立刻再起，设备有概率回 code 10004 "The process identifier of the
    // launched application could not be determined"。这不是"参数不对"那类语义拒绝
    // （重试同一件事不会变好），而是**进程还没死干净**：实测停掉之后隔 2 秒起必成，
    // 立刻起则时好时坏（成功那次要 385ms，平时 50ms）。所以按可重试处理。
    // 判据只能从回信文字里认：feature() 把设备侧的 code 折进了 err 字符串。
    constexpr int kAttempts = 4;
    for (int attempt = 1; attempt <= kAttempts; ++attempt) {
        xpc::Value output;
        // 60 秒：这条 RPC 在设备上要做杀旧实例 + 起新实例 + 等进程报告，10 秒的默认值
        // 不够（同一台设备上 listapps 那种调用就会顶到几十秒）。
        if (device.feature(kService, kFeature, kAction, input, output, err, verbose, 60000)) {
            return true;
        }
        if (err.find("10004") == std::string::npos || attempt == kAttempts) {
            return false;
        }
        std::printf("起 %s 撞上旧进程未死干净（第 %d 次），500ms 后重试\n",
                    bundle_id.c_str(), attempt);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return false;
}

std::vector<int64_t> App::matching_pids(const xpc::Value &processes,
                                        const std::string &app_path) {
    std::vector<int64_t> out;
    const auto *tokens = processes.find("processTokens");
    if (tokens == nullptr || app_path.empty()) {
        return out;
    }
    for (const auto &token : tokens->array) {
        const std::string exe =
            strip_file_scheme(token.at("executableURL").at("relative").as_string_or(""));
        // 前缀要连上那个 '/'：/…/Foo.app 不能把 /…/Foo.app2 也算进来，
        // 而进程的可执行文件一定在 App 目录里面（…/Foo.app/Foo）。
        if (exe.starts_with(app_path + "/")) {
            out.push_back(token.at("processIdentifier").as_int_or(0));
        }
    }
    return out;
}

bool App::list(Device &device, std::vector<Entry> &out, std::string &err, bool verbose) {
    auto conn = device.connect(kService, err, verbose);
    if (conn == nullptr) {
        return false;
    }
    if (!device.rsd().supports(kService, "com.apple.coredevice.feature.streamapplist")) {
        err = "这台设备的 appservice 没有声明 streamapplist，不退回 listapps（那个是大回复）";
        return false;
    }

    auto flags = xpc::make_dict();
    for (const char *k : {"includeAppClips", "includeRemovableApps", "includeHiddenApps",
                          "includeInternalApps", "includeDefaultApps", "includeContainerPaths",
                          "includeAppGroupIdentifiers"}) {
        xpc::dict_set(flags, k, xpc::make_bool(true));
    }
    // 要容器访问得持有权力字符串，开着大概率是直接失败而不是变慢。
    xpc::dict_set(flags, "requireContainerAccess", xpc::make_bool(false));

    // sideChannel 是客户端自己生成的 UUID，设备在每条回信里原样带回来。这里不校验
    // 它：一条连接上只跑这一条流，串不了。
    std::vector<uint8_t> side(16);
    for (auto &b : side) {
        b = static_cast<uint8_t>(std::random_device {}());
    }
    auto proxy = xpc::make_dict();
    xpc::dict_set(proxy, "sideChannel", xpc::make_uuid(side));
    auto input = xpc::make_dict();
    xpc::dict_set(input, "actualInput", std::move(flags));
    xpc::dict_set(input, "streamProxy", std::move(proxy));

    const auto got = conn->stream(
        "com.apple.coredevice.feature.streamapplist", "", input,
        [&out](const xpc::Value &one) {
            Entry e {
                .bundle_id = one.at("bundleIdentifier").as_string_or(""),
                .path = one.at("path").as_string_or(""),
                .name = one.at("name").as_string_or(""),
            };
            if (!e.bundle_id.empty()) {
                out.push_back(std::move(e));
            }
            return true;
        },
        30000, err);
    return got == CallResult::Ok;
}

bool App::stop(Device &device, const std::string &bundle_id, std::string &err, bool verbose) {
    std::vector<Entry> apps;
    if (!list(device, apps, err, verbose)) {
        return false;
    }
    const auto it = std::ranges::find(apps, bundle_id, &Entry::bundle_id);
    if (it == apps.end()) {
        err = "设备上找不到 bundle id " + bundle_id;
        return false;
    }

    xpc::Value procs;
    if (!device.feature(kService, "com.apple.coredevice.feature.listprocesses", "",
                        xpc::make_dict(), procs, err, verbose, 30000)) {
        return false;
    }
    const auto pids = matching_pids(procs, it->path);
    if (pids.empty()) {
        // App 根本没在跑。Android 的 `am force-stop` 在这种情况下也是成功的，
        // 所以这里返回真；调用方的意图（"它别在跑"）已经成立。
        std::printf("stop_app(%s)：没在跑，什么都不做\n", bundle_id.c_str());
        return true;
    }
    for (const int64_t pid : pids) {
        auto input = xpc::make_dict();
        auto process = xpc::make_dict();
        xpc::dict_set(process, "processIdentifier", xpc::make_int64(pid));
        xpc::dict_set(input, "process", std::move(process));
        xpc::dict_set(input, "signal", xpc::make_int64(kSigKill));
        xpc::Value output;
        if (!device.feature(kService, "com.apple.coredevice.feature.sendsignaltoprocess", "",
                            input, output, err, verbose, 30000)) {
            err = "杀 " + bundle_id + " 的进程 " + std::to_string(pid) + " 失败: " + err;
            return false;
        }
    }
    return true;
}

}  // namespace scrctl::remote
