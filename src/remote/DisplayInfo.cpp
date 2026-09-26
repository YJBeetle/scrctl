#include "remote/DisplayInfo.h"

#include <chrono>
#include <cstdio>
#include <random>
#include <span>
#include <thread>
#include <utility>

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

/// 流式 feature 的消息体：参数裹在 actualInput 下，外加一个客户端自己生成的
/// sideChannel UUID（设备拿它认这条订阅归谁）。形状与 streamapplist 那条一致。
///
/// 每次订阅都要一个新的 UUID：重订时拿同一个号，设备侧那两条订阅就分不清彼此。
[[nodiscard]] xpc::Value stream_input() {
    std::vector<uint8_t> side(16);
    for (auto &b : side) {
        b = static_cast<uint8_t>(std::random_device {} ());
    }
    auto proxy = xpc::make_dict();
    xpc::dict_set(proxy, "sideChannel", xpc::make_uuid(std::span<const uint8_t>(side)));
    auto input = xpc::make_dict();
    xpc::dict_set(input, "actualInput", xpc::make_dict());
    xpc::dict_set(input, "streamProxy", std::move(proxy));
    return input;
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
    const auto input = stream_input();

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


// ------------------------------------------------------- 常驻订阅 ----

namespace {

/// 单轮等待上限。它同时就是**退出延迟**：循环只在两轮之间看一眼停止标志，
/// 所以 Ctrl-C 最多等这么久。再长就会让人觉得窗口卡住了。
constexpr int kPollMs = 250;
/// 重订之间的间隔。设这么短是因为转屏引起的订阅作废要立刻补上，否则画面会
/// 停在旧朝向好几秒。
constexpr int kReconnectGapMs = 500;
/// 连着多少次订不上就认了。设备拔走之后 `connect` 会一直失败，不收手就是
/// 一个每半秒撞一次门的死循环。
constexpr int kMaxFailedResubscribes = 8;

}  // namespace

DisplayWatcher::DisplayWatcher(Device &device, uint64_t display_id, bool verbose)
    : device_(device), display_id_(display_id), verbose_(verbose) {}

std::unique_ptr<DisplayWatcher> DisplayWatcher::start(Device &device, uint64_t display_id,
                                                      std::string &err, bool verbose) {
    auto watcher = std::unique_ptr<DisplayWatcher>(new DisplayWatcher(device, display_id, verbose));
    // 先订上再放线程：订不上就是订不上，这时候返回 nullptr 让调用方退回"起流前
    // 问一次"那一档，比派一个线程去后台反复撞一扇门好。
    if (!watcher->resubscribe(err)) {
        return nullptr;
    }
    watcher->alive_.store(true);
    watcher->worker_ = std::thread(&DisplayWatcher::loop, watcher.get());
    return watcher;
}

DisplayWatcher::~DisplayWatcher() {
    stop_.store(true);
    if (worker_.joinable()) {
        worker_.join();
    }
}

DisplayWatcher::State DisplayWatcher::latest() const {
    std::lock_guard<std::mutex> lock(mu_);
    return latest_;
}

const Display *DisplayWatcher::pick(const DisplayInfo &info) const {
    const Display *d = info.find(display_id_);
    if (d == nullptr) {
        d = info.primary();  // id 对不上退回主屏：外接屏的 id 是设备分配的，不保证连续
    }
    // "找到了"不等于"能用"：那几块 Wireless-N 在册但尺寸全零。
    if (d != nullptr && (d->width <= 0 || d->height <= 0)) {
        return nullptr;
    }
    return d;
}

bool DisplayWatcher::resubscribe(std::string &err) {
    conn_ = device_.connect(kService, err, verbose_);
    if (conn_ == nullptr) {
        return false;
    }
    if (!conn_->subscribe(kFeature, "", stream_input(), err)) {
        conn_.reset();
        return false;
    }
    return true;
}

void DisplayWatcher::publish(const xpc::Value &element) {
    std::string err;
    const auto info = parse_display_info(element, err);
    if (info == std::nullopt) {
        return;  // 解不动的这一条跳过：常驻订阅没有"失败就退出"的余地
    }
    const Display *d = pick(*info);
    if (d == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (d->orientation == latest_.orientation && d->width == latest_.width &&
        d->height == latest_.height) {
        return;  // 设备把同一条重发了一遍，不算变化
    }
    latest_.orientation = d->orientation;
    latest_.width = d->width;
    latest_.height = d->height;
    ++latest_.seq;
}

void DisplayWatcher::loop() {
    std::vector<xpc::Value> batch;
    int failures = 0;
    while (!stop_.load()) {
        std::string err;
        const auto event = conn_->next_batch(batch, kPollMs, err);
        if (event == ServiceConnection::StreamEvent::Batch) {
            failures = 0;
            for (const auto &element : batch) {
                publish(element);
            }
            continue;
        }
        if (event == ServiceConnection::StreamEvent::Idle) {
            continue;  // 常态：设备只在状态真的变了才推
        }
        // Finished / DeviceError / Broken —— 这条订阅已经作废，换一条连接重订。
        conn_.reset();
        // 退避分片睡。整段睡下去的话，退出最多要慢一整个 gap，而这段等待期间
        // 用户看到的是"按了 Ctrl-C 没反应"。
        for (int slept = 0; slept < kReconnectGapMs && !stop_.load(); slept += 20) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (stop_.load()) {
            break;
        }
        if (resubscribe(err)) {
            failures = 0;
            continue;
        }
        if (++failures >= kMaxFailedResubscribes) {
            // 连着八次订不上，多半是设备已经走了。留下 alive()=false 让调用方知道
            // 这个增强没了，而不是假装还在跟踪——也别再每半秒撞一次门。
            alive_.store(false);
            return;
        }
    }
    alive_.store(false);
}

}  // namespace scrctl::remote
