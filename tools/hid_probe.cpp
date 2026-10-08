// 按命令行顺序发送 HID 动作，可在同一会话中穿插截图核对屏幕变化。
// send-only 的成功只表示本地发送完成；输入是否生效仍需设备画面或截图确认。
#include "ProbeCli.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "hid/Hid.h"
#include "i18n/Translation.h"
#include "media/StreamSession.h"
#include "remote/Device.h"
#include "xpc/XpcValue.h"

namespace {

// 辅助流只用于收包，不解码、不发送 RR。超时是正常空闲；其它错误结束线程，
// 避免已失效的接收端反复立即返回。主线程读取计数和错误时与工作线程同步。
class Drainer {
public:
    explicit Drainer(scrctl::media::StreamSession &session) : session_(session) {
        worker_ = std::thread([this] {
            std::string err;
            std::vector<uint8_t> packet;
            while (!stopping_.load()) {
                if (session_.next_packet(packet, 50, err)) {
                    ++packets_;
                } else if (err != SCRCTL_TR("UDP receive timed out")) {
                    std::lock_guard lock(error_mutex_);
                    error_ = err.empty() ? "收包失败，未提供错误原因" : err;
                    break;
                }
            }
        });
    }
    ~Drainer() {
        stopping_ = true;
        if (worker_.joinable()) worker_.join();
    }
    [[nodiscard]] unsigned long long packets() const { return packets_.load(); }
    [[nodiscard]] std::string error() const {
        std::lock_guard lock(error_mutex_);
        return error_;
    }

private:
    scrctl::media::StreamSession &session_;
    std::thread worker_;
    std::atomic<bool> stopping_{false};
    std::atomic<unsigned long long> packets_{0};
    mutable std::mutex error_mutex_;
    std::string error_;
};

struct QueuedAction {
    enum Kind { kButton, kTap, kLine, kShot, kStroke, kSwipe, kKeys, kReply, kPaste,
                kList, kRaw } kind;
    std::array<double, 4> v{};
    std::string arg;
    uint64_t surface = 0;
    int seconds = 0;
};

uint16_t button_code(const std::string &name) {
    static const std::pair<const char *, uint16_t> codes[] = {
        {"home", scrctl::hid::button::kHome}, {"lock", scrctl::hid::button::kLock},
        {"volup", scrctl::hid::button::kVolumeUp},
        {"voldn", scrctl::hid::button::kVolumeDown}, {"mute", scrctl::hid::button::kMute},
    };
    for (const auto &[label, code] : codes) {
        if (name == label) return code;
    }
    return 0;
}

void check_coordinates(const std::vector<double> &values, const char *option, size_t count) {
    if (values.size() != count) throw CLI::ValidationError(option, "需要提供完整的坐标组");
    for (const auto value : values) {
        if (!std::isfinite(value) || value < 0 || value > 1)
            throw CLI::ValidationError(option, "坐标必须是 0..1 范围内的有限数值");
    }
}

CLI::Validator nonempty_coordinate() {
    return CLI::Validator([](std::string &text) -> std::string {
        return text.find_first_not_of(" \t\r\n\v\f") == std::string::npos
                   ? "坐标不能为空" : std::string{};
    }, "COORDINATE", "nonempty_coordinate");
}

// 与旧工具一致，键盘面 ID 按十进制读取；完整检查后再交给 CLI11 转换。
// 不限制 UDID 的格式，设备选择仍由 Device::establish 检查。
CLI::Validator surface_id() {
    return CLI::Validator([](std::string &text) -> std::string {
        std::string_view value = text;
        constexpr std::string_view whitespace = " \t\r\n\v\f";
        while (!value.empty() && whitespace.find(value.front()) != std::string_view::npos)
            value.remove_prefix(1);
        while (!value.empty() && whitespace.find(value.back()) != std::string_view::npos)
            value.remove_suffix(1);
        if (!value.empty() && value.front() == '+') value.remove_prefix(1);
        uint64_t parsed = 0;
        const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed, 10);
        if (value.empty() || result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
            parsed == 0) return "键盘面 ID 必须是 1..18446744073709551615 的完整十进制整数";
        text = std::to_string(parsed);
        return {};
    }, "DECIMAL", "surface_id");
}

void print_plan(const std::vector<QueuedAction> &queue, bool with_stream, bool explicit_device) {
    std::printf("离线预演：未连接设备，以下计划不表示输入已经生效。\n");
    std::printf("device=%s stream=%s timeout=20 RR=off\n",
                explicit_device ? "explicit" : "auto", with_stream ? "on" : "off");
    size_t index = 0;
    for (const auto &action : queue) {
        std::printf("action[%zu]: ", ++index);
        switch (action.kind) {
        case QueuedAction::kButton: std::printf("button %s", action.arg.c_str()); break;
        case QueuedAction::kTap: std::printf("tap %.6g %.6g", action.v[0], action.v[1]); break;
        case QueuedAction::kLine:
            std::printf("line %.6g %.6g %.6g %.6g", action.v[0], action.v[1], action.v[2], action.v[3]);
            break;
        case QueuedAction::kShot: std::printf("shot %s", action.arg.c_str()); break;
        case QueuedAction::kStroke: std::printf("stroke"); break;
        case QueuedAction::kSwipe: std::printf("swipe-loop %d", action.seconds); break;
        case QueuedAction::kKeys:
            std::printf("keys %llu", static_cast<unsigned long long>(action.surface)); break;
        case QueuedAction::kReply: std::printf("probe-reply"); break;
        case QueuedAction::kPaste: std::printf("paste"); break;
        case QueuedAction::kList: std::printf("list"); break;
        case QueuedAction::kRaw: std::printf("raw"); break;
        }
        std::printf("\n");
    }
}

bool save_screenshot(const std::string &path, const std::vector<uint8_t> &data) {
    std::FILE *fp = std::fopen(path.c_str(), "wb");
    if (fp == nullptr) {
        std::fprintf(stderr, "无法打开截图文件 %s: %s\n", path.c_str(), std::strerror(errno));
        return false;
    }
    errno = 0;
    const auto written = std::fwrite(data.data(), 1, data.size(), fp);
    const bool write_failed = written != data.size() || std::ferror(fp);
    const int write_error = errno;
    // fclose 也会刷新缓冲区，写入返回完整长度不保证文件已经保存成功。
    errno = 0;
    const int close_result = std::fclose(fp);
    const int close_error = errno;
    if (write_failed || close_result != 0) {
        const int reason = write_failed ? write_error : close_error;
        std::fprintf(stderr, "保存截图文件 %s 失败: %s\n", path.c_str(),
                     reason ? std::strerror(reason) : "写入长度不足");
        return false;
    }
    std::printf("截图 %zu 字节 -> %s\n", data.size(), path.c_str());
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    bool verbose = false;
    bool no_stream = false;
    bool dry_run = false;
    std::vector<QueuedAction> queue;
    std::string serial;
    CLI::App cli{"按指定顺序发送 HID 动作并截图核对"};
    cli.set_help_flag("-h,--help", "显示帮助");
    cli.footer("所有查询和动作按参数出现顺序执行。--tap 不带坐标时点击中心；带坐标时必须提供 X Y。\n"
               "中心点击并指定设备时，将 UDID 放在 --tap 前，或使用 --tap -- UDID。\n"
               "不提供动作或查询时显示帮助；--no-stream 单独使用可检查无媒体流的服务连接。\n"
               "辅助视频显式请求 20 秒 RTCP 空闲超时，不发送 RR，不保证长时间持续收包。\n"
               "退出仅关闭本地接收端，不调用 stopAll；stopAll 会停止设备上其它媒体会话。\n"
               "发送成功不确认输入已生效；--probe-reply 无回信也不能单独证明输入失败。\n"
               "--dry-run 只检查参数并显示计划，不连接设备。");
    cli.add_flag("-v,--verbose", verbose, "显示连接详情")->disable_flag_override();
    cli.add_flag("--no-stream", no_stream, "不启动辅助视频流")->disable_flag_override();
    cli.add_flag("--dry-run", dry_run, "只检查参数并显示有序计划")->disable_flag_override();
    cli.add_option("UDID", serial, "设备标识；省略时选择唯一已连接设备");
    const auto add_action_flag = [&](const char *name, QueuedAction::Kind kind, const char *help) {
        cli.add_flag_callback(name, [&, kind] { queue.push_back(QueuedAction{kind}); }, help)
            ->trigger_on_parse()->disable_flag_override();
    };
    add_action_flag("--list", QueuedAction::kList, "列出设备注册的 HID 面");
    add_action_flag("--raw", QueuedAction::kRaw, "输出 connectedServices 原始回复并列出 HID 面");
    add_action_flag("--stroke", QueuedAction::kStroke, "在屏幕中央画一条短斜线");
    add_action_flag("--probe-reply", QueuedAction::kReply, "发送中心触摸并等待原始回复（诊断用途）");
    add_action_flag("--paste", QueuedAction::kPaste, "发送 Command+V，目标输入框须已有焦点");
    cli.add_option_function<std::string>("--button", [&](const std::string &name) {
        auto action = QueuedAction{QueuedAction::kButton};
        action.arg = name;
        queue.push_back(std::move(action));
    }, "按一次 home/lock/volup/voldn/mute")
        ->check(CLI::IsMember({"home", "lock", "volup", "voldn", "mute"}))->trigger_on_parse();
    CLI::Option *tap_option = nullptr;
    tap_option = cli.add_option_function<std::vector<double>>("--tap", [&](const std::vector<double> &values) {
        // CLI11 对 expected(0, ...) 跳过空字符串 validator，且会将空浮点值转为 0。
        // 在解析回调中检查原始结果，区分未提供参数的中心默认值和显式空坐标。
        for (const auto &raw : tap_option->results()) {
            if (raw.find_first_not_of(" \t\r\n\v\f") == std::string::npos)
                throw CLI::ValidationError("--tap", "坐标不能为空");
        }
        check_coordinates(values, "--tap", 2);
        auto action = QueuedAction{QueuedAction::kTap};
        std::copy(values.begin(), values.end(), action.v.begin());
        queue.push_back(std::move(action));
    }, "点击归一化坐标 X Y（0..1）；省略时为中心，按住 90ms")
        ->expected(0, 2)->allow_extra_args(false)->delimiter(',')->default_str("0.5,0.5")
        ->check(nonempty_coordinate())->trigger_on_parse();
    cli.add_option_function<std::vector<double>>("--line", [&](const std::vector<double> &values) {
        check_coordinates(values, "--line", 4);
        auto action = QueuedAction{QueuedAction::kLine};
        std::copy(values.begin(), values.end(), action.v.begin());
        queue.push_back(std::move(action));
    }, "沿 X0 Y0 X1 Y1 画插值直线；坐标为 0..1")
        ->expected(4)->allow_extra_args(false)->check(nonempty_coordinate())->trigger_on_parse();
    cli.add_option_function<std::string>("--shot", [&](const std::string &path) {
        if (path.empty()) throw CLI::ValidationError("--shot", "需要提供非空文件路径");
        auto action = QueuedAction{QueuedAction::kShot};
        action.arg = path;
        queue.push_back(std::move(action));
    }, "将 screencaptureservice PNG 保存到 FILE")
        ->type_size(0, 1)->expected(1)->trigger_on_parse();
    cli.add_option_function<uint64_t>("--keys", [&](uint64_t surface) {
        auto action = QueuedAction{QueuedAction::kKeys};
        action.surface = surface;
        queue.push_back(std::move(action));
    }, "向指定键盘面 ID 发送 a b c")
        ->transform(surface_id())->trigger_on_parse();
    cli.add_option_function<int>("--swipe-loop", [&](int seconds) {
        auto action = QueuedAction{QueuedAction::kSwipe};
        action.seconds = seconds;
        queue.push_back(std::move(action));
    }, "连续横向拖动 N 秒；0 表示不拖动")
        ->transform(scrctl::probe::decimal_integer(0, std::numeric_limits<int>::max()))->trigger_on_parse();

    try {
        cli.parse(argc, argv);
    } catch (const CLI::CallForHelp &) {
        std::printf("%s", cli.help().c_str());
        return 0;
    } catch (const CLI::ParseError &error) {
        std::fprintf(stderr, "参数无效: %s\n", error.what());
        return 2;
    }
    const bool with_stream = !no_stream;
    if (dry_run) {
        print_plan(queue, with_stream, !serial.empty());
        return 0;
    }
    if (queue.empty() && with_stream) {
        std::printf("%s", cli.help().c_str());
        return 0;
    }

    std::string err;
    auto device = scrctl::remote::Device::establish(serial, err, verbose);
    if (!device) {
        std::fprintf(stderr, "建立会话失败: %s\n", err.c_str());
        return 1;
    }
    std::printf("会话就绪：%s / iOS %s\n", device->property("ProductType").c_str(),
                device->property("OSVersion").c_str());

    std::unique_ptr<scrctl::media::StreamSession> session;
    std::unique_ptr<Drainer> drainer;
    if (with_stream) {
        scrctl::media::StreamSession::Request req;
        req.timeout_seconds = 20;
        session = scrctl::media::StreamSession::start(*device, req, err, verbose);
        if (session == nullptr) {
            std::fprintf(stderr, "起流失败: %s\n", err.c_str());
            return 1;
        }
        drainer = std::make_unique<Drainer>(*session);
        std::printf("辅助视频：请求 20 秒 RTCP 空闲超时，RR=off；退出不调用 stopAll。\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        std::printf("辅助视频已收 %llu 个包；收包或发送成功不确认输入已生效。\n", drainer->packets());
    } else {
        std::printf("本次会话不启动辅助视频流。\n");
    }

    std::unique_ptr<scrctl::hid::Service> hid;
    const auto receiver_ok = [&] {
        const auto failure = drainer ? drainer->error() : std::string{};
        if (failure.empty()) return true;
        std::fprintf(stderr, "辅助视频收包失败: %s\n", failure.c_str());
        return false;
    };
    const auto touch_failed = [&](const char *label, double x, double y) {
        std::fprintf(stderr, "%s失败: %s\n", label, err.c_str());
        std::string release_error;
        if (!hid->touch(scrctl::hid::kSurfaceMainTouchscreen, x, y, false, release_error))
            std::fprintf(stderr, "补发触摸抬起失败: %s\n", release_error.c_str());
        return 1;
    };
    const auto keyboard_failed = [&](uint64_t surface) {
        std::fprintf(stderr, "键盘发送失败: %s\n", err.c_str());
        std::string release_error;
        if (!hid->send_report(surface, scrctl::hid::keyboard_report({}), release_error))
            std::fprintf(stderr, "补发键盘松键失败: %s\n", release_error.c_str());
        return 1;
    };

    // 枚举可能结束设备端连接，每项查询使用独立实例。下一项输入必须重新打开，
    // 查询前也释放已有注入通道，避免误用已被设备关闭的连接。
    for (const auto &action : queue) {
        if (!receiver_ok()) return 1;
        if (action.kind == QueuedAction::kList || action.kind == QueuedAction::kRaw) {
            hid.reset();
            if (action.kind == QueuedAction::kRaw) {
                auto query = scrctl::hid::Service::open(*device, err, verbose);
                scrctl::xpc::Value reply;
                if (!query || !query->raw_connected_services(reply, err)) {
                    std::fprintf(stderr, "取 HID 原文失败: %s\n", err.c_str());
                    return 1;
                }
                std::printf("connectedServices 原文:\n%s\n", scrctl::xpc::describe(reply).c_str());
            }
            auto query = scrctl::hid::Service::open(*device, err, verbose);
            std::vector<scrctl::hid::Service::Surface> surfaces;
            if (!query || !query->surfaces(surfaces, err)) {
                std::fprintf(stderr, "列面失败: %s\n", err.c_str());
                return 1;
            }
            for (const auto &surface : surfaces) {
                std::printf("  面 %llu (0x%llx)  %s\n",
                            static_cast<unsigned long long>(surface.service_id),
                            static_cast<unsigned long long>(surface.service_id), surface.name.c_str());
            }
            continue;
        }
        if (!hid) {
            hid = scrctl::hid::Service::open(*device, err, verbose);
            if (!hid) {
                std::fprintf(stderr, "打开 HID 服务失败: %s\n", err.c_str());
                return 1;
            }
            std::printf("universalhidservice 已连接\n");
        }
        switch (action.kind) {
        case QueuedAction::kButton: {
            auto buttons = scrctl::hid::Buttons::open(*device, err, verbose);
            if (!buttons) {
                std::fprintf(stderr, "打开按键面失败: %s\n", err.c_str());
                return 1;
            }
            const auto code = button_code(action.arg);
            std::printf("按硬件键 %s\n", action.arg.c_str());
            if (!buttons->press(scrctl::hid::button::kUsagePageConsumer, code, 90, err)) {
                std::fprintf(stderr, "按键失败: %s\n", err.c_str());
                std::string release_error;
                if (!buttons->release(scrctl::hid::button::kUsagePageConsumer, code, release_error))
                    std::fprintf(stderr, "补发硬件按键抬起失败: %s\n", release_error.c_str());
                return 1;
            }
            break;
        }
        case QueuedAction::kTap:
            std::printf("点击 (%.3f, %.3f)\n", action.v[0], action.v[1]);
            if (!hid->tap(action.v[0], action.v[1], 90, err))
                return touch_failed("点击", action.v[0], action.v[1]);
            break;
        case QueuedAction::kLine: {
            std::printf("画线 (%.3f,%.3f) -> (%.3f,%.3f)\n",
                        action.v[0], action.v[1], action.v[2], action.v[3]);
            std::vector<std::pair<double, double>> points;
            for (int i = 0; i <= 24; ++i) {
                const double t = i / 24.0;
                points.emplace_back(action.v[0] + (action.v[2] - action.v[0]) * t,
                                    action.v[1] + (action.v[3] - action.v[1]) * t);
            }
            if (!hid->stroke(points, 12, err)) return touch_failed("画线", action.v[2], action.v[3]);
            break;
        }
        case QueuedAction::kShot: {
            auto input = scrctl::xpc::make_dict();
            scrctl::xpc::dict_set(input, "displayUniqueID", scrctl::xpc::make_null());
            scrctl::xpc::dict_set(input, "requestedFormat", scrctl::xpc::make_string("png"));
            scrctl::xpc::Value out;
            if (!device->feature("com.apple.coredevice.screencaptureservice",
                                 "com.apple.coredevice.feature.capturescreenshot",
                                 "com.apple.coredevice.action.capturescreenshot", input, out, err,
                                 verbose, 15000)) {
                std::fprintf(stderr, "截图失败: %s\n", err.c_str());
                return 1;
            }
            const auto *image = out.find("image");
            if (!image || (image->type != scrctl::xpc::Type::Data &&
                           image->type != scrctl::xpc::Type::FileTransfer) || image->data.empty()) {
                std::fprintf(stderr, "截图回信里没有 image 字节\n");
                return 1;
            }
            if (!save_screenshot(action.arg, image->data)) return 1;
            break;
        }
        case QueuedAction::kStroke: {
            // 保持在画面中部，减少误触系统边缘手势的可能。
            std::printf("画一条中央短斜线\n");
            const std::vector<std::pair<double, double>> points = {
                {0.44, 0.44}, {0.47, 0.46}, {0.50, 0.48}, {0.53, 0.50}, {0.56, 0.52}};
            if (!hid->stroke(points, 16, err)) return touch_failed("拖动", 0.56, 0.52);
            break;
        }
        case QueuedAction::kSwipe: {
            // 在无边记等画布中可作为持续变化的画面源；其它应用的手势效果需另行确认。
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(action.seconds);
            bool rightward = true;
            unsigned long long flips = 0;
            while (std::chrono::steady_clock::now() < until) {
                if (!receiver_ok()) return 1;
                const double x0 = rightward ? 0.82 : 0.18;
                const double x1 = rightward ? 0.18 : 0.82;
                std::vector<std::pair<double, double>> points;
                for (int i = 0; i <= 16; ++i)
                    points.emplace_back(x0 + (x1 - x0) * (i / 16.0), 0.55);
                if (!hid->stroke(points, 14, err)) return touch_failed("拖动", x1, 0.55);
                rightward = !rightward;
                ++flips;
                std::this_thread::sleep_for(std::chrono::milliseconds(240));
            }
            std::printf("横向拖动请求完成 %llu 次\n", flips);
            break;
        }
        case QueuedAction::kKeys:
            std::printf("在面 %llu 上发送 a b c\n", static_cast<unsigned long long>(action.surface));
            for (uint16_t usage : {scrctl::hid::key::kA, uint16_t{scrctl::hid::key::kA + 1},
                                  uint16_t{scrctl::hid::key::kA + 2}}) {
                if (!hid->type(action.surface, {usage}, 60, err)) return keyboard_failed(action.surface);
            }
            break;
        case QueuedAction::kReply:
            // 正常 send-only 输入可能没有逐条回信。诊断等待失败仍补发抬起，
            // 返回非零表示未完成该查询，不表示已经证实设备没有接受触摸。
            for (const auto state : {scrctl::hid::kStateContact, scrctl::hid::kStateRelease}) {
                scrctl::xpc::Value reply;
                const auto report = scrctl::hid::touchscreen_report(
                    state, scrctl::hid::normalize(0.5), scrctl::hid::normalize(0.5));
                if (!hid->send_report(scrctl::hid::kSurfaceMainTouchscreen, report, err, &reply))
                    return touch_failed("报告回复查询", 0.5, 0.5);
                std::printf("报告 state=0x%02x 有回信: %s\n", state,
                            scrctl::xpc::describe(reply).substr(0, 500).c_str());
            }
            break;
        case QueuedAction::kPaste:
            std::printf("发送 Command+V\n");
            if (!hid->press_chord(scrctl::hid::kSurfaceKeyboard,
                                  {scrctl::hid::key::kGuiLeft, uint16_t{scrctl::hid::key::kA + 21}},
                                  60, err)) return keyboard_failed(scrctl::hid::kSurfaceKeyboard);
            break;
        case QueuedAction::kList:
        case QueuedAction::kRaw: break;  // 查询已在上方完成。
        }
    }
    // --no-stream 单独使用仍建立 HID 服务连接，但不会发送隐式输入。
    if (queue.empty()) {
        hid = scrctl::hid::Service::open(*device, err, verbose);
        if (!hid) {
            std::fprintf(stderr, "打开 HID 服务失败: %s\n", err.c_str());
            return 1;
        }
        std::printf("universalhidservice 已连接；未发送输入。\n");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (!receiver_ok()) return 1;
    if (drainer) std::printf("结束时辅助视频已收 %llu 个包\n", drainer->packets());
    std::printf("计划已发送；输入效果请在设备画面或截图中确认。\n");
    return 0;
}
