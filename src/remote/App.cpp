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

/// 幂等的读类 RPC：撞上**传输类**失败就换一条连接再问一次。
/// 依据是 Rsd.h 里那条实测——约 15% 的服务连接会撞一次超时/帧错位/对端关闭，而换一条
/// 连接重发往往就成了；设备真答了"不同意"（DeviceError）的话重试只会再拿到同一句话，
/// 所以那种直接交回去。
CallResult ask_idempotent(Device &device, std::string_view feature_identifier,
                          const xpc::Value &input, xpc::Value &output, std::string &err,
                          bool verbose, int timeout_ms) {
    for (int attempt = 1; attempt <= 3; ++attempt) {
        const auto result = device.feature_call(kService, feature_identifier, "", input, output,
                                               err, verbose, timeout_ms);
        if (result != CallResult::TransportError || attempt == 3) {
            return result;
        }
    }
    return CallResult::TransportError;
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

    // 整条流失败时重开一条连接重跑一遍（同样是"传输类失败换连接就好"那一类）。
    const auto collect = [&out](const xpc::Value &one) {
        Entry e {
            .bundle_id = one.at("bundleIdentifier").as_string_or(""),
            .path = one.at("path").as_string_or(""),
            .name = one.at("name").as_string_or(""),
        };
        if (!e.bundle_id.empty()) {
            out.push_back(std::move(e));
        }
        return true;
    };
    for (int attempt = 1; attempt <= 3; ++attempt) {
        auto conn = device.connect(kService, err, verbose);
        if (conn == nullptr) {
            return false;
        }
        const auto got = conn->stream("com.apple.coredevice.feature.streamapplist", "", input,
                                      collect, 30000, err);
        if (got != CallResult::TransportError || attempt == 3) {
            return got == CallResult::Ok;
        }
        out.clear();  // 上一条连接可能已经推了一半
    }
    return false;
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
    if (ask_idempotent(device, "com.apple.coredevice.feature.listprocesses", xpc::make_dict(),
                       procs, err, verbose, 30000) != CallResult::Ok) {
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
        // 这条 RPC 实测会撞"等设备回信超时"——那是**传输类**失败（约 15% 的服务连接
        // 会撞一次），不是设备拒绝，而每次 feature_call 都开新连接，正是它的解法。
        // SIGKILL 重复发是幂等的，所以放心重试。
        bool killed = false;
        for (int attempt = 1; attempt <= 3 && !killed; ++attempt) {
            xpc::Value output;
            const auto result = device.feature_call(
                kService, "com.apple.coredevice.feature.sendsignaltoprocess", "", input, output,
                err, verbose, 30000);
            if (result == CallResult::Ok) {
                killed = true;
            } else if (result == CallResult::DeviceError) {
                err = "杀 " + bundle_id + " 的进程 " + std::to_string(pid) + " 失败: " + err;
                return false;
            }
        }
        if (!killed) {
            err = "杀 " + bundle_id + " 的进程 " + std::to_string(pid) +
                  " 三次都问不到回信: " + err;
            return false;
        }
    }

    // **等它真的没了再返回。** SIGKILL 的 RPC 是"发出去了"，不是"进程已经死了"，
    // 而紧接着的 launchapplication 撞上未死干净的旧进程会回 code 10004 且**真的起不
    // 来**（实测停完立刻起，连试 4 次 500ms 间隔全败；等进程表干净之后再起就一次成）。
    // 让 stop 同步，调用方就不必自己去猜要等多久。
    for (int wait = 0; wait < 40; ++wait) {
        xpc::Value again;
        std::string perr;
        if (ask_idempotent(device, "com.apple.coredevice.feature.listprocesses",
                           xpc::make_dict(), again, perr, verbose, 30000) != CallResult::Ok) {
            break;  // 问不动就不再等，信号已经发出去了
        }
        if (matching_pids(again, it->path).empty()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    std::printf("stop_app(%s)：信号已发出，但进程表里还能看到它（旧进程可能卡在退出路径上）\n",
                bundle_id.c_str());
    return true;
}

}  // namespace scrctl::remote
