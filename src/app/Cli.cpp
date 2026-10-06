#include "app/Cli.h"
#include "hid/Hid.h"
#include <CLI/CLI.hpp>
#include <algorithm>
#include <charconv>
#include <map>
#include <cmath>
#include <cstdio>
#include <vector>

namespace scrctl::app {

ParseResult parse_args(int argc, char **argv, Options &o) {
    CLI::App app{"iOS 屏幕镜像与控制"};
    app.footer("无参数时镜像当前连接的设备。\n"
               "设备决定编码尺寸、码率和帧率，暂不支持修改这些参数。\n"
               "鼠标左键映射为触摸；方向不支持 flip，录制保留原始码流。\n"
               "无线需要先配对；--help / --version 不连接设备。");
    app.set_help_flag("-h,--help", "显示帮助");
    app.add_option("--play", o.path, "回放 Annex-B HEVC 文件");
    app.add_option("-s,--serial", o.serial, "设备 UDID");
    app.add_option("--wifi", o.wifi, "局域网地址（需要已有配对记录）");
    app.add_option("-r,--record", o.record, "将实时流录为 Annex-B");
    app.add_option("--start-app", o.start_app, "启动 bundle id；? 按名称前缀，+ 先终止实例");
    app.add_option("--window-title,--title", o.title, "窗口标题");
    app.add_option("--render-driver", o.render_driver, "SDL 渲染驱动，如 metal / software");
    app.add_option("--video-source", o.video_source,
                   "stream 实时流或 screenshot 截图轮询，默认 stream")
        ->check(CLI::IsMember({"stream", "screenshot"}));
    app.add_option("--test-touch", o.test_touch, "注入直线 X0,Y0,X1,Y1 后退出（归一化坐标）")
        ->delimiter(',')->expected(4);
    const std::map<std::string, uint16_t> button_codes = {
        {"home", hid::button::kHome}, {"lock", hid::button::kLock},
        {"volup", hid::button::kVolumeUp}, {"voldn", hid::button::kVolumeDown},
        {"mute", hid::button::kMute},
    };
    app.add_option("--test-button", o.test_button, "注入 home/lock/volup/voldn/mute")
        ->check(CLI::IsMember(button_codes));
    app.add_option("--test-type", o.test_type, "注入 ASCII 文本（设备需已聚焦文本框）");
    app.add_option("--test-degrade", o.test_degrade, "按 T1,T2,... 秒交替强制降级与恢复");
    app.add_option("--copy", o.copy_text, "写入设备剪贴板后退出（支持中文）");
    app.add_flag("--list-devices", o.list_devices, "列出连接的设备");
    app.add_flag("-n,--no-control", o.no_control, "关闭输入控制");
    app.add_flag("--list-apps", o.list_apps, "列出设备 App");
    app.add_flag("--version", o.show_version, "显示版本");
    app.add_flag("-f,--fullscreen", o.fullscreen, "桌面全屏");
    app.add_flag("--always-on-top", o.always_on_top, "窗口置顶");
    app.add_flag("--window-borderless", o.borderless, "无边框窗口");
    app.add_flag("--disable-screensaver", o.disable_screensaver, "运行期间禁止本机息屏");
    app.add_flag("--no-audio", o.no_audio, "不启动音频流");
    app.add_flag("--no-audio-playback", o.no_audio_playback, "接收和解码音频，但本机不播放");
    app.add_flag("--no-window", o.no_window, "无窗口运行");
    app.add_flag("--hw-decode", o.hw_decode, "使用平台硬件解码，默认软件解码");
    app.add_flag("--debug-input", o.debug_input, "打印输入坐标");
    app.add_flag("--debug-net", o.debug_net, "打印隧道网络诊断");
    app.add_flag("--stats", o.stats, "每秒打印统计");
    app.add_flag("--paste", o.paste, "读取设备剪贴板；与 --copy 同用时写后读回");
    app.add_option("--window-x", o.win_x, "窗口横坐标，默认居中");
    app.add_option("--window-y", o.win_y, "窗口纵坐标，默认居中");
    app.add_option("--window-width", o.win_w, "窗口宽度，0 为自动")->check(CLI::NonNegativeNumber);
    app.add_option("--window-height", o.win_h, "窗口高度，0 为自动")->check(CLI::NonNegativeNumber);
    app.add_option("--time-limit", o.time_limit, "运行秒数，0 为不限")
        ->check(CLI::NonNegativeNumber);
    app.add_option("--audio-buffer", o.audio_buffer_ms, "音频缓冲毫秒数，默认 50，上限 1000")
        ->check(CLI::NonNegativeNumber);
    app.add_option("--exit-after", o.exit_after, "处理 N 帧后退出，0 为不限")
        ->check(CLI::NonNegativeNumber);
    auto *scale = app.add_option("--scale", o.scale, "有限正数缩放比例，默认自动适应屏幕");
    std::string orientation, crop, background;
    std::vector<std::string> verify;
    app.add_option("--display-orientation,--orientation", orientation,
                   "显示顺时针朝向；auto 跟随设备，不影响录制")
        ->check(CLI::IsMember({"auto", "0", "90", "180", "270"}));
    app.add_option("--crop", crop, "裁剪 WxH+X+Y 或 W:H:X:Y");
    app.add_option("--background-color", background, "背景色 #RRGGBB");
    app.add_option("--verify", verify, "第 N 帧回读窗口为 BMP：N FILE（需要窗口）")->expected(2);
    try {
        app.parse(argc, argv);
        if (!std::all_of(o.test_touch.begin(), o.test_touch.end(), [](double value) {
                return std::isfinite(value) && value >= 0 && value <= 1;
            })) {
            throw CLI::ValidationError("--test-touch", "坐标必须是 [0, 1] 内的有限数");
        }
        if (!o.test_button.empty()) {
            o.test_button_code = button_codes.at(o.test_button);
        }
        o.scale_given = scale->count() != 0;
        if (!std::isfinite(o.scale) || o.scale <= 0) {
            throw CLI::ValidationError("--scale", "需要有限正数");
        }
        if (!orientation.empty())
            o.orientation = orientation == "auto" ? -1 : std::stoi(orientation);
        if (app.count("--crop")) {
            int consumed = 0;
            const bool dimensions = std::sscanf(crop.c_str(), "%dx%d+%d+%d%n", &o.crop_w, &o.crop_h,
                                                &o.crop_x, &o.crop_y, &consumed) == 4;
            if (!dimensions) {
                consumed = 0;
                if (std::sscanf(crop.c_str(), "%d:%d:%d:%d%n", &o.crop_w, &o.crop_h, &o.crop_x,
                                &o.crop_y, &consumed) != 4)
                    consumed = 0;
            }
            if (consumed != static_cast<int>(crop.size()) || consumed == 0 || o.crop_w <= 0 ||
                o.crop_h <= 0 || o.crop_x < 0 || o.crop_y < 0) {
                throw CLI::ValidationError("--crop",
                                           "格式应为 WxH+X+Y 或 W:H:X:Y，尺寸为正、偏移非负");
            }
            o.crop_set = true;
        }
        if (app.count("--background-color")) {
            if (background.size() != 7 || background[0] != '#' ||
                background.find_first_not_of("0123456789abcdefABCDEF", 1) != std::string::npos) {
                throw CLI::ValidationError("--background-color", "需要 #RRGGBB");
            }
            for (int i = 0; i < 3; ++i) {
                o.bg[i] =
                    static_cast<uint8_t>(std::stoul(background.substr(1 + 2 * i, 2), nullptr, 16));
            }
        }
        if (!verify.empty()) {
            const auto &number = verify[0];
            const auto converted =
                std::from_chars(number.data(), number.data() + number.size(), o.verify_at);
            if (converted.ec != std::errc{} || converted.ptr != number.data() + number.size() ||
                o.verify_at <= 0) {
                throw CLI::ValidationError("--verify", "帧号需要正整数");
            }
            o.verify_path = verify[1];
        }
        if (o.no_window && o.verify_at > 0) {
            throw CLI::ValidationError("--verify", "需要窗口，不能与 --no-window 同用");
        }
    } catch (const CLI::CallForHelp &) {
        std::printf("%s", app.help().c_str());
        return ParseResult::ExitSuccess;
    } catch (const CLI::ParseError &e) {
        std::fprintf(stderr, "%s\n", e.what());
        return ParseResult::Error;
    }
    return ParseResult::Run;
}

} // namespace scrctl::app
