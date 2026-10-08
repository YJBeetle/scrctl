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
#include "i18n/CliLanguage.h"
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
                    error_ = err.empty() ? SCRCTL_TR("Packet receive failed without an error message") : err;
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
    if (values.size() != count) throw CLI::ValidationError(option, "Provide all coordinates");
    for (const auto value : values) {
        if (!std::isfinite(value) || value < 0 || value > 1)
            throw CLI::ValidationError(option, "Coordinates must be finite numbers in [0, 1]");
    }
}

CLI::Validator nonempty_coordinate() {
    return CLI::Validator([](std::string &text) -> std::string {
        return text.find_first_not_of(" \t\r\n\v\f") == std::string::npos
                   ? "Coordinates cannot be empty" : std::string{};
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
            parsed == 0) return "Keyboard surface ID must be a decimal integer in [1, 18446744073709551615]";
        text = std::to_string(parsed);
        return {};
    }, "DECIMAL", "surface_id");
}

void print_plan(const std::vector<QueuedAction> &queue, bool with_stream, bool explicit_device) {
    std::printf(SCRCTL_TR("Dry run: no device connection or input was sent.\n"));
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
        std::fprintf(stderr, SCRCTL_TR("Cannot open screenshot file %s: %s\n"), path.c_str(), std::strerror(errno));
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
        std::fprintf(stderr, SCRCTL_TR("Failed to save screenshot file %s: %s\n"), path.c_str(),
                     reason ? std::strerror(reason) : SCRCTL_TR("Incomplete write"));
        return false;
    }
    std::printf(SCRCTL_TR("Saved screenshot: %zu bytes -> %s\n"), data.size(), path.c_str());
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
    CLI::App cli{SCRCTL_N_("Send HID input in command-line order and capture screenshots")};
    cli.set_help_flag("-h,--help", SCRCTL_N_("Show help"));
    cli.footer(SCRCTL_N_(
        "Queries and actions run in command-line order. --tap defaults to the center; otherwise provide both X and Y.\n"
        "For a center tap on a specific device, put UDID before --tap, or use --tap -- UDID.\n"
        "Without actions or queries, show help. --no-stream alone opens the HID service without sending input.\n"
        "The auxiliary video requests a 20-second RTCP idle timeout and sends no RR; it may stop during a long test.\n"
        "Exit closes the local receiver without calling stopAll, which would stop other media sessions on the device.\n"
        "A successful send does not confirm input took effect. No --probe-reply response does not prove input failed.\n"
        "--dry-run checks arguments and prints the plan without connecting to a device."));
    cli.add_flag("-v,--verbose", verbose, SCRCTL_N_("Print connection and request details"))->disable_flag_override();
    cli.add_flag("--no-stream", no_stream, SCRCTL_N_("Do not start auxiliary video"))->disable_flag_override();
    cli.add_flag("--dry-run", dry_run, SCRCTL_N_("Check arguments and print the ordered plan without connecting"))->disable_flag_override();
    cli.add_option("UDID", serial, SCRCTL_N_("Device identifier; omit to select the only connected device"));
    const auto add_action_flag = [&](const char *name, QueuedAction::Kind kind, const char *help) {
        cli.add_flag_callback(name, [&, kind] { queue.push_back(QueuedAction{kind}); }, help)
            ->trigger_on_parse()->disable_flag_override();
    };
    add_action_flag("--list", QueuedAction::kList, SCRCTL_N_("List the device's HID surfaces"));
    add_action_flag("--raw", QueuedAction::kRaw, SCRCTL_N_("Print the raw connectedServices reply and list HID surfaces"));
    add_action_flag("--stroke", QueuedAction::kStroke, SCRCTL_N_("Draw a short diagonal line near the center"));
    add_action_flag("--probe-reply", QueuedAction::kReply, SCRCTL_N_("Send a center touch and wait for raw replies (diagnostic)"));
    add_action_flag("--paste", QueuedAction::kPaste, SCRCTL_N_("Send Command+V; focus the destination text field first"));
    cli.add_option_function<std::string>("--button", [&](const std::string &name) {
        auto action = QueuedAction{QueuedAction::kButton};
        action.arg = name;
        queue.push_back(std::move(action));
    }, SCRCTL_N_("Press home, lock, volup, voldn or mute once"))
        ->check(CLI::IsMember({"home", "lock", "volup", "voldn", "mute"}))->trigger_on_parse();
    CLI::Option *tap_option = nullptr;
    tap_option = cli.add_option_function<std::vector<double>>("--tap", [&](const std::vector<double> &values) {
        // CLI11 对 expected(0, ...) 跳过空字符串 validator，且会将空浮点值转为 0。
        // 在解析回调中检查原始结果，区分未提供参数的中心默认值和显式空坐标。
        for (const auto &raw : tap_option->results()) {
            if (raw.find_first_not_of(" \t\r\n\v\f") == std::string::npos)
                throw CLI::ValidationError("--tap", "Coordinates cannot be empty");
        }
        check_coordinates(values, "--tap", 2);
        auto action = QueuedAction{QueuedAction::kTap};
        std::copy(values.begin(), values.end(), action.v.begin());
        queue.push_back(std::move(action));
    }, SCRCTL_N_("Tap normalized X Y in [0, 1], holding for 90 ms; defaults to the center"))
        ->expected(0, 2)->allow_extra_args(false)->delimiter(',')->default_str("0.5,0.5")
        ->check(nonempty_coordinate())->trigger_on_parse();
    cli.add_option_function<std::vector<double>>("--line", [&](const std::vector<double> &values) {
        check_coordinates(values, "--line", 4);
        auto action = QueuedAction{QueuedAction::kLine};
        std::copy(values.begin(), values.end(), action.v.begin());
        queue.push_back(std::move(action));
    }, SCRCTL_N_("Draw a line from X0 Y0 to X1 Y1 using normalized coordinates in [0, 1]"))
        ->expected(4)->allow_extra_args(false)->check(nonempty_coordinate())->trigger_on_parse();
    cli.add_option_function<std::string>("--shot", [&](const std::string &path) {
        if (path.empty()) throw CLI::ValidationError("--shot", "Provide a nonempty file path");
        auto action = QueuedAction{QueuedAction::kShot};
        action.arg = path;
        queue.push_back(std::move(action));
    }, SCRCTL_N_("Save a PNG from screencaptureservice to FILE"))
        ->type_size(0, 1)->expected(1)->trigger_on_parse();
    cli.add_option_function<uint64_t>("--keys", [&](uint64_t surface) {
        auto action = QueuedAction{QueuedAction::kKeys};
        action.surface = surface;
        queue.push_back(std::move(action));
    }, SCRCTL_N_("Type a b c on the specified keyboard surface ID"))
        ->transform(surface_id())->trigger_on_parse();
    cli.add_option_function<int>("--swipe-loop", [&](int seconds) {
        auto action = QueuedAction{QueuedAction::kSwipe};
        action.seconds = seconds;
        queue.push_back(std::move(action));
    }, SCRCTL_N_("Drag horizontally for N seconds; 0 sends no drag"))
        ->transform(scrctl::probe::decimal_integer(0, std::numeric_limits<int>::max()))->trigger_on_parse();

    scrctl::i18n::CliLanguage language(cli);
    if (auto code = language.parse(argc, argv)) return *code;
    const bool with_stream = !no_stream;
    if (dry_run) {
        print_plan(queue, with_stream, !serial.empty());
        return 0;
    }
    if (queue.empty() && with_stream) {
        std::printf("%s", language.help().c_str());
        return 0;
    }

    std::string err;
    auto device = scrctl::remote::Device::establish(serial, err, verbose);
    if (!device) {
        std::fprintf(stderr, SCRCTL_TR("Failed to establish device session: %s\n"), err.c_str());
        return 1;
    }
    std::printf(SCRCTL_TR("Device session ready: %s / iOS %s\n"), device->property("ProductType").c_str(),
                device->property("OSVersion").c_str());

    std::unique_ptr<scrctl::media::StreamSession> session;
    std::unique_ptr<Drainer> drainer;
    if (with_stream) {
        scrctl::media::StreamSession::Request req;
        req.timeout_seconds = 20;
        session = scrctl::media::StreamSession::start(*device, req, err, verbose);
        if (session == nullptr) {
            std::fprintf(stderr, SCRCTL_TR("Failed to start auxiliary video: %s\n"), err.c_str());
            return 1;
        }
        drainer = std::make_unique<Drainer>(*session);
        std::printf(SCRCTL_TR("Auxiliary video: requested 20-second RTCP idle timeout, RR=off; exit does not call stopAll.\n"));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        std::printf(SCRCTL_TR("Auxiliary video received %llu packets; packet reception or a successful send does not confirm input took effect.\n"), drainer->packets());
    } else {
        std::printf(SCRCTL_TR("Auxiliary video is disabled for this session.\n"));
    }

    std::unique_ptr<scrctl::hid::Service> hid;
    const auto receiver_ok = [&] {
        const auto failure = drainer ? drainer->error() : std::string{};
        if (failure.empty()) return true;
        std::fprintf(stderr, SCRCTL_TR("Auxiliary video receive failed: %s\n"), failure.c_str());
        return false;
    };
    const auto touch_failed = [&](const char *label, double x, double y) {
        std::fprintf(stderr, SCRCTL_TR("%s failed: %s\n"), label, err.c_str());
        std::string release_error;
        if (!hid->touch(scrctl::hid::kSurfaceMainTouchscreen, x, y, false, release_error))
            std::fprintf(stderr, SCRCTL_TR("Failed to release touch after an error: %s\n"), release_error.c_str());
        return 1;
    };
    const auto keyboard_failed = [&](uint64_t surface) {
        std::fprintf(stderr, SCRCTL_TR("Keyboard input failed: %s\n"), err.c_str());
        std::string release_error;
        if (!hid->send_report(surface, scrctl::hid::keyboard_report({}), release_error))
            std::fprintf(stderr, SCRCTL_TR("Failed to release keyboard keys after an error: %s\n"), release_error.c_str());
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
                    std::fprintf(stderr, SCRCTL_TR("Failed to query the raw HID reply: %s\n"), err.c_str());
                    return 1;
                }
                std::printf(SCRCTL_TR("Raw connectedServices reply:\n%s\n"), scrctl::xpc::describe(reply).c_str());
            }
            auto query = scrctl::hid::Service::open(*device, err, verbose);
            std::vector<scrctl::hid::Service::Surface> surfaces;
            if (!query || !query->surfaces(surfaces, err)) {
                std::fprintf(stderr, SCRCTL_TR("Failed to list HID surfaces: %s\n"), err.c_str());
                return 1;
            }
            for (const auto &surface : surfaces) {
                std::printf(SCRCTL_TR("  Surface %llu (0x%llx)  %s\n"),
                            static_cast<unsigned long long>(surface.service_id),
                            static_cast<unsigned long long>(surface.service_id), surface.name.c_str());
            }
            continue;
        }
        if (!hid) {
            hid = scrctl::hid::Service::open(*device, err, verbose);
            if (!hid) {
                std::fprintf(stderr, SCRCTL_TR("Failed to open the HID service: %s\n"), err.c_str());
                return 1;
            }
            std::printf(SCRCTL_TR("Connected to universalhidservice\n"));
        }
        switch (action.kind) {
        case QueuedAction::kButton: {
            auto buttons = scrctl::hid::Buttons::open(*device, err, verbose);
            if (!buttons) {
                std::fprintf(stderr, SCRCTL_TR("Failed to open the hardware button service: %s\n"), err.c_str());
                return 1;
            }
            const auto code = button_code(action.arg);
            std::printf(SCRCTL_TR("Press hardware button: %s\n"), action.arg.c_str());
            if (!buttons->press(scrctl::hid::button::kUsagePageConsumer, code, 90, err)) {
                std::fprintf(stderr, SCRCTL_TR("Hardware button input failed: %s\n"), err.c_str());
                std::string release_error;
                if (!buttons->release(scrctl::hid::button::kUsagePageConsumer, code, release_error))
                    std::fprintf(stderr, SCRCTL_TR("Failed to release the hardware button after an error: %s\n"), release_error.c_str());
                return 1;
            }
            break;
        }
        case QueuedAction::kTap:
            std::printf(SCRCTL_TR("Tap (%.3f, %.3f)\n"), action.v[0], action.v[1]);
            if (!hid->tap(action.v[0], action.v[1], 90, err))
                return touch_failed(SCRCTL_TR("Tap"), action.v[0], action.v[1]);
            break;
        case QueuedAction::kLine: {
            std::printf(SCRCTL_TR("Draw line (%.3f, %.3f) -> (%.3f, %.3f)\n"),
                        action.v[0], action.v[1], action.v[2], action.v[3]);
            std::vector<std::pair<double, double>> points;
            for (int i = 0; i <= 24; ++i) {
                const double t = i / 24.0;
                points.emplace_back(action.v[0] + (action.v[2] - action.v[0]) * t,
                                    action.v[1] + (action.v[3] - action.v[1]) * t);
            }
            if (!hid->stroke(points, 12, err)) return touch_failed(SCRCTL_TR("Line drawing"), action.v[2], action.v[3]);
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
                std::fprintf(stderr, SCRCTL_TR("Screenshot failed: %s\n"), err.c_str());
                return 1;
            }
            const auto *image = out.find("image");
            if (!image || (image->type != scrctl::xpc::Type::Data &&
                           image->type != scrctl::xpc::Type::FileTransfer) || image->data.empty()) {
                std::fprintf(stderr, SCRCTL_TR("Screenshot reply contains no image data\n"));
                return 1;
            }
            if (!save_screenshot(action.arg, image->data)) return 1;
            break;
        }
        case QueuedAction::kStroke: {
            // 保持在画面中部，减少误触系统边缘手势的可能。
            std::printf(SCRCTL_TR("Draw a short diagonal line near the center\n"));
            const std::vector<std::pair<double, double>> points = {
                {0.44, 0.44}, {0.47, 0.46}, {0.50, 0.48}, {0.53, 0.50}, {0.56, 0.52}};
            if (!hid->stroke(points, 16, err)) return touch_failed(SCRCTL_TR("Drag"), 0.56, 0.52);
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
                if (!hid->stroke(points, 14, err)) return touch_failed(SCRCTL_TR("Drag"), x1, 0.55);
                rightward = !rightward;
                ++flips;
                std::this_thread::sleep_for(std::chrono::milliseconds(240));
            }
            std::printf(SCRCTL_TR("Completed %llu horizontal drag requests\n"), flips);
            break;
        }
        case QueuedAction::kKeys:
            std::printf(SCRCTL_TR("Type a b c on surface %llu\n"), static_cast<unsigned long long>(action.surface));
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
                    return touch_failed(SCRCTL_TR("Report reply query"), 0.5, 0.5);
                std::printf(SCRCTL_TR("Reply for report state=0x%02x: %s\n"), state,
                            scrctl::xpc::describe(reply).substr(0, 500).c_str());
            }
            break;
        case QueuedAction::kPaste:
            std::printf(SCRCTL_TR("Send Command+V\n"));
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
            std::fprintf(stderr, SCRCTL_TR("Failed to open the HID service: %s\n"), err.c_str());
            return 1;
        }
        std::printf(SCRCTL_TR("Connected to universalhidservice; no input was sent.\n"));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (!receiver_ok()) return 1;
    if (drainer) std::printf(SCRCTL_TR("Auxiliary video received %llu packets in total\n"), drainer->packets());
    std::printf(SCRCTL_TR("Requested operations completed. Check the device screen or a screenshot to confirm input.\n"));
    return 0;
}
