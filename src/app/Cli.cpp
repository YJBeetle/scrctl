#include "i18n/Translation.h"
#include "i18n/CliLanguage.h"
#include "app/Cli.h"
#include "app/RecordFormat.h"
#include "hid/Hid.h"
#include <CLI/CLI.hpp>
#include <algorithm>
#include <charconv>
#include <cctype>
#include <map>
#include <cmath>
#include <cstdio>
#include <vector>

namespace scrctl::app {

ParseResult parse_args(int argc, char **argv, Options &o) {
    CLI::App app{SCRCTL_N_("iOS screen mirroring and control")};
    app.footer(SCRCTL_N_(
        "With no arguments, mirror the connected device.\n"
        "Left mouse button maps to touch. Ordinary keys use the device's keyboard layout and input method.\n"
        "Quit: MOD+Q. Fullscreen: MOD+F or F11. MOD defaults to left Alt or left Super; "
        "change it with --shortcut-mod.\n"
        "Paste computer clipboard: MOD+V (Unicode supported).\n"
        "Home: MOD+H or middle mouse button. Volume: MOD+Up/Down. Lock button: MOD+P.\n"
        "Actual window size: MOD+G. Remove image margins: MOD+W or double-click an empty margin.\n"
        "Rotate display left/right: MOD+Left/Right (recordings unchanged).\n"
        "Mirror display horizontally: MOD+Shift+Left/Right. Mirror vertically: MOD+Shift+Up/Down.\n"
        "Audio is forwarded to the computer by default; --audio-dup keeps phone playback. "
        "Switching routes may pause the phone's player; resume it if needed.\n"
        "The device chooses encoding dimensions, bitrate and frame rate. Display crop and "
        "--display-orientation leave recordings unchanged; --orientation rotates display and "
        "container recordings together. Record selected tracks to .mp4 or .mkv, or use .hevc for raw video.\n"
        "Wireless use requires pairing. --help / --version do not connect to the device."));
    app.set_help_flag("-h,--help", SCRCTL_N_("Show help"));
    app.add_option("--play", o.path, SCRCTL_N_("Play an Annex-B HEVC file"));
    app.add_option("-s,--serial", o.serial, SCRCTL_N_("Device UDID"));
    // 先允许零个值，再显式校验；CLI11 的必填值会把 --wifi= 后的下一个选项吞作地址。
    app.add_option("--wifi", o.wifi, SCRCTL_N_("LAN address, or auto to discover a paired wireless device"))
        ->expected(0, 1);
    app.add_option("--wifi-port", o.wifi_port,
                   SCRCTL_N_("RemotePairing port for a manual LAN address; default: 49152"))
        ->check(CLI::Range(1, 65535))->needs("--wifi");
    auto *pair_command = app.add_flag("--pair", o.pair,
                 SCRCTL_N_("Create or verify a remote pairing record over USB, then exit"))
        ->excludes("--wifi")->excludes("--play");
    app.add_flag("--repair-pairing", o.repair_pairing,
                 SCRCTL_N_("Allow replacing an incomplete or rejected pairing record; requires --pair"))
        ->needs("--pair");
    app.add_option("-r,--record", o.record, SCRCTL_N_(
        "Record selected video/audio tracks to .mp4 or .mkv; other extensions save raw HEVC without audio"))
        ->excludes("--play");
    app.add_option("--start-app", o.start_app, SCRCTL_N_("Launch bundle ID; ? matches name prefix, + terminates the previous instance"));
    app.add_option("--window-title,--title", o.title, SCRCTL_N_("Window title"));
    app.add_option("--render-driver", o.render_driver, SCRCTL_N_("SDL render driver, e.g. metal / software"));
    app.add_option("--video-source", o.video_source,
                   SCRCTL_N_("display for live video (default), screenshot for polling; stream is an alias for display"))
        ->check(CLI::IsMember({"display", "stream", "screenshot"}));
    app.add_option("--test-touch", o.test_touch, SCRCTL_N_("Inject X0,Y0,X1,Y1 swipe and exit (normalized coordinates)"))
        ->delimiter(',')->expected(4);
    const std::map<std::string, uint16_t> button_codes = {
        {"home", hid::button::kHome}, {"lock", hid::button::kLock},
        {"volup", hid::button::kVolumeUp}, {"voldn", hid::button::kVolumeDown},
        {"mute", hid::button::kMute},
    };
    app.add_option("--test-button", o.test_button, SCRCTL_N_("Inject home/lock/volup/voldn/mute"))
        ->check(CLI::IsMember(button_codes));
    app.add_option("--test-type", o.test_type, SCRCTL_N_("Inject ASCII text (device text field must have focus)"));
    app.add_option("--test-degrade", o.test_degrade, SCRCTL_N_("Force alternating video fallback and recovery at T1,T2,... seconds"));
    app.add_option("--copy", o.copy_text, SCRCTL_N_("Write device clipboard and exit (supports Unicode)"));
    app.add_flag("--list-devices", o.list_devices,
                 SCRCTL_N_("List USB and RemotePairing devices; wireless scan defaults to 3 seconds"));
    app.add_option("--discovery-timeout", o.discovery_timeout_ms,
                   SCRCTL_N_("Wireless discovery timeout in milliseconds (0..60000); 0 lists usbmux only"))
        ->check(CLI::Range(0, 60000))->needs("--list-devices");
    app.add_flag("-n,--no-control", o.no_control, SCRCTL_N_("Disable input control"))
        ->excludes("--test-touch")->excludes("--test-button")->excludes("--test-type")
        ->excludes("--start-app");
    app.add_flag("--list-apps", o.list_apps, SCRCTL_N_("List device apps"));
    app.add_flag("--version", o.show_version, SCRCTL_N_("Show version"));
    app.add_flag("-f,--fullscreen", o.fullscreen, SCRCTL_N_("Desktop fullscreen"));
    app.add_flag("--always-on-top", o.always_on_top, SCRCTL_N_("Keep window on top"));
    app.add_flag("--window-borderless", o.borderless, SCRCTL_N_("Borderless window"));
    app.add_flag("--disable-screensaver", o.disable_screensaver, SCRCTL_N_("Prevent local screen sleep while running"));
    app.add_flag("--no-audio", o.no_audio, SCRCTL_N_("Do not start audio stream"));
    app.add_flag("--audio-dup", o.audio_dup,
                 SCRCTL_N_("Keep audio playing on the phone while forwarding it to the computer"))
        ->excludes("--no-audio");
    app.add_flag("--no-audio-playback", o.no_audio_playback,
                 SCRCTL_N_("Disable computer audio playback; capture audio only when recording to MP4 or MKV"));
    app.add_flag("--no-video", o.no_video, SCRCTL_N_("Do not start video stream"));
    app.add_flag("--no-video-playback", o.no_video_playback,
                 SCRCTL_N_("Disable video display; keep encoded video when recording"));
    bool no_playback = false;
    app.add_flag("-N,--no-playback", no_playback,
                 SCRCTL_N_("Disable computer video and audio playback; keep selected recording tracks"));
    app.add_flag("--no-window", o.no_window, SCRCTL_N_("Run without a window"));
    app.add_flag("--hw-decode", o.hw_decode, SCRCTL_N_("Use platform hardware decoder; default: software"));
    app.add_flag("--debug-input", o.debug_input, SCRCTL_N_("Print input coordinates"));
    app.add_flag("--debug-net", o.debug_net, SCRCTL_N_("Add tunnel diagnostics to --stats"));
    app.add_flag("--stats", o.stats, SCRCTL_N_("Print statistics every second"));
    app.add_flag("--paste", o.paste, SCRCTL_N_("Read device clipboard; with --copy, read back after writing"));
    std::string window_x, window_y, shortcut_mod;
    app.add_option("--window-x", window_x,
                   SCRCTL_N_("Window horizontal position; integer or auto (default)"));
    app.add_option("--window-y", window_y,
                   SCRCTL_N_("Window vertical position; integer or auto (default)"));
    app.add_option("--shortcut-mod", shortcut_mod,
                   SCRCTL_N_("Shortcut modifiers, separated by commas: lctrl, rctrl, lalt, ralt, lsuper, rsuper; default: lalt,lsuper"));
    app.add_option("--window-width", o.win_w, SCRCTL_N_("Window width; 0 for automatic"))->check(CLI::NonNegativeNumber);
    app.add_option("--window-height", o.win_h, SCRCTL_N_("Window height; 0 for automatic"))->check(CLI::NonNegativeNumber);
    app.add_option("--time-limit", o.time_limit, SCRCTL_N_("Run duration in seconds; 0 for unlimited"))
        ->check(CLI::NonNegativeNumber);
    app.add_option("--audio-buffer", o.audio_buffer_ms, SCRCTL_N_("Audio buffer in milliseconds; default: 50, maximum: 1000"))
        ->check(CLI::NonNegativeNumber);
    app.add_option("--exit-after", o.exit_after, SCRCTL_N_("Exit after N frames; 0 for unlimited"))
        ->check(CLI::NonNegativeNumber);
    auto *scale = app.add_option("--scale", o.scale, SCRCTL_N_("Finite positive scale; default: fit screen"));
    std::string crop, background;
    std::vector<std::string> verify;
    // 录制只支持旋转，但解析时保留 flip 状态，让后续参数可以按顺序覆盖。
    // 与 scrcpy 一致，仅当最终请求录制且仍有 flip 时拒绝，不将翻转静默丢弃。
    bool record_flip = false;
    const auto rotation = [](const std::string& value, bool& flip) {
        flip = value.starts_with("flip");
        return value == "auto" ? -1 : std::stoi(flip ? value.substr(4) : value);
    };
    // 按解析顺序立即设置方向，组合参数和重复参数的后一次赋值生效。
    // --orientation 同时设置两者；旧 auto 用法仅设置显示，不改变录制方向。
    app.add_option_function<std::string>("--orientation", [&](const std::string& value) {
        o.orientation = rotation(value, o.display_flip);
        if (o.orientation >= 0) {
            o.record_orientation = o.orientation;
            record_flip = o.display_flip;
        }
    }, SCRCTL_N_("Display and recording orientation: 0/90/180/270 or flip0/flip90/flip180/flip270; recording cannot flip; auto changes display only"))
        ->check(CLI::IsMember({"auto", "0", "90", "180", "270", "flip0", "flip90", "flip180", "flip270"}))->trigger_on_parse();
    app.add_option_function<std::string>("--display-orientation", [&](const std::string& value) {
        o.orientation = rotation(value, o.display_flip);
    }, SCRCTL_N_("Display orientation: auto, 0/90/180/270 or flip0/flip90/flip180/flip270; flip horizontally before clockwise rotation"))
        ->check(CLI::IsMember({"auto", "0", "90", "180", "270", "flip0", "flip90", "flip180", "flip270"}))->trigger_on_parse();
    app.add_option_function<std::string>("--record-orientation", [&](const std::string& value) {
        o.record_orientation = rotation(value, record_flip);
    }, SCRCTL_N_("Clockwise MP4/MKV recording rotation: 0/90/180/270; default: 0; display is unchanged"))
        ->check(CLI::IsMember({"0", "90", "180", "270", "flip0", "flip90", "flip180", "flip270"}))->trigger_on_parse();
    app.add_option("--crop", crop,
                   SCRCTL_N_("Crop displayed pixels: WxH+X+Y or W:H:X:Y; recording is unchanged"));
    app.add_option("--background-color", background,
                   SCRCTL_N_("Background color: RGB or RRGGBB, with optional #"));
    app.add_option("--verify", verify, SCRCTL_N_("Read window at frame N into BMP: N FILE (requires a window)"))->expected(2);
    // excludes 需要目标选项已经注册，不能引用后面才创建的独立命令。
    pair_command->excludes("--list-devices")->excludes("--list-apps")
        ->excludes("--copy")->excludes("--paste")->excludes("--start-app");
    i18n::CliLanguage language(app);
    try {
        app.parse(argc, argv);
        if (!language.select()) return ParseResult::Error;
        if (o.no_window || o.no_video || no_playback) o.no_video_playback = true;
        if (no_playback) o.no_audio_playback = true;
        // 没有本机播放或容器音轨消费者时，跳过音频采集和设备路由请求。
        if (o.no_audio_playback && !record_container_format(o.record)) o.no_audio = true;
        if (o.audio_dup && o.no_audio) {
            throw CLI::ValidationError("--audio-dup", SCRCTL_TR(
                "Audio capture is disabled; --audio-dup requires playback or MP4/MKV recording"));
        }
        if (o.video_source == "display") o.video_source = "stream";
        // 按帧数退出和旧诊断选项仍有视频消费者；其余无播放、无录制时不采集视频。
        // 文件回放保留旧 --no-window 的解码/计数用法，新增禁用视频选项没有文件用途。
        if (!o.path.empty() && (app.count("--no-video") || app.count("--no-video-playback") || no_playback)) {
            throw CLI::ValidationError("--play", SCRCTL_TR("File playback requires video; incompatible with video playback disable options"));
        }
        if (o.path.empty() && o.no_video_playback && o.record.empty() && o.exit_after == 0 &&
            !o.hw_decode && o.test_degrade.empty() && o.video_source != "screenshot") {
            o.no_video = true;
        }
        if (o.no_video && !o.record.empty()) {
            if (!record_container_format(o.record)) {
                throw CLI::ValidationError("--record", SCRCTL_TR("Audio-only recording requires MP4 or MKV; raw HEVC cannot contain audio"));
            }
            if (o.no_audio) {
                throw CLI::ValidationError("--record", SCRCTL_TR("Recording requires at least one enabled audio or video track"));
            }
            if (o.record_orientation != 0 || record_flip) {
                throw CLI::ValidationError("--record-orientation", SCRCTL_TR("Recording orientation requires a video track"));
            }
        }
        if (o.no_video && (o.exit_after > 0 || o.hw_decode || !o.test_degrade.empty() ||
                           o.video_source == "screenshot")) {
            throw CLI::ValidationError("--no-video", SCRCTL_TR("Frame counting, hardware decoding, fallback tests and screenshots require video capture"));
        }
        if (record_container_format(o.record) && o.video_source == "screenshot") {
            throw CLI::ValidationError("--record", SCRCTL_TR(
                "Container recording requires live video; screenshot polling cannot be recorded"));
        }
        if (!o.record.empty() && record_flip) {
            throw CLI::ValidationError("--record-orientation", SCRCTL_TR(
                "Recording does not support flipping; use --display-orientation for display-only flipping or reset --record-orientation to a rotation"));
        }
        if (!o.record.empty() && !record_container_format(o.record) && o.record_orientation != 0) {
            throw CLI::ValidationError("--record", SCRCTL_TR(
                "Recording rotation requires MP4 or MKV; use --display-orientation to rotate only the display"));
        }
        if (app.count("--wifi") && std::all_of(o.wifi.begin(), o.wifi.end(), [](unsigned char c) {
                return std::isspace(c);
            })) {
            throw CLI::ValidationError("--wifi", SCRCTL_TR("Requires a nonempty LAN address or auto"));
        }
        if (o.wifi == "auto" && app.count("--wifi-port")) {
            throw CLI::ValidationError("--wifi-port", SCRCTL_TR("Use a manual LAN address; auto uses discovered SRV ports"));
        }
        auto position = [&](const char *name, const std::string &value, std::optional<int> &out) {
            if (!app.count(name)) return;
            if (value == "auto") {
                out.reset();
                return;
            }
            int parsed_position = 0;
            // 沿用 CLI11 的整数转换，保留原有负坐标与溢出检查。
            if (!CLI::detail::lexical_cast(value, parsed_position)) {
                throw CLI::ValidationError(name, SCRCTL_TR("Expected an integer or auto"));
            }
            out = parsed_position;
        };
        position("--window-x", window_x, o.win_x);
        position("--window-y", window_y, o.win_y);
        if (app.count("--shortcut-mod")) {
            const std::map<std::string, uint16_t> modifiers = {
                {"lctrl", KMOD_LCTRL}, {"rctrl", KMOD_RCTRL},
                {"lalt", KMOD_LALT}, {"ralt", KMOD_RALT},
                {"lsuper", KMOD_LGUI}, {"rsuper", KMOD_RGUI},
            };
            uint16_t mask = 0;
            std::size_t start = 0;
            while (true) {
                const auto comma = shortcut_mod.find(',', start);
                const auto token = shortcut_mod.substr(start, comma - start);
                const auto found = modifiers.find(token);
                if (found == modifiers.end()) {
                    throw CLI::ValidationError("--shortcut-mod", SCRCTL_TR(
                        "Use a comma-separated list of lctrl, rctrl, lalt, ralt, lsuper or rsuper"));
                }
                mask |= found->second;
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
            o.shortcut_mods = mask;
        }
        if (!std::all_of(o.test_touch.begin(), o.test_touch.end(), [](double value) {
                return std::isfinite(value) && value >= 0 && value <= 1;
            })) {
            throw CLI::ValidationError("--test-touch", SCRCTL_TR("Coordinates must be finite numbers in [0, 1]"));
        }
        if (!o.test_button.empty()) {
            o.test_button_code = button_codes.at(o.test_button);
        }
        o.scale_given = scale->count() != 0;
        if (!std::isfinite(o.scale) || o.scale <= 0) {
            throw CLI::ValidationError("--scale", SCRCTL_TR("Requires a finite positive number"));
        }
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
                                           SCRCTL_TR("Expected WxH+X+Y or W:H:X:Y, with positive dimensions and nonnegative offsets"));
            }
            o.crop_set = true;
        }
        if (app.count("--background-color")) {
            if (!background.empty() && background.front() == '#') background.erase(0, 1);
            if ((background.size() != 3 && background.size() != 6) ||
                background.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
                throw CLI::ValidationError("--background-color", SCRCTL_TR(
                    "Expected 3 or 6 hexadecimal digits, optionally prefixed with #"));
            }
            const std::size_t channel_length = background.size() == 3 ? 1 : 2;
            for (int i = 0; i < 3; ++i) {
                const auto channel = std::stoul(background.substr(channel_length * i, channel_length), nullptr, 16);
                o.bg[i] = static_cast<uint8_t>(channel_length == 1 ? channel * 17 : channel);
            }
        }
        if (!verify.empty()) {
            const auto &number = verify[0];
            const auto converted =
                std::from_chars(number.data(), number.data() + number.size(), o.verify_at);
            if (converted.ec != std::errc{} || converted.ptr != number.data() + number.size() ||
                o.verify_at <= 0) {
                throw CLI::ValidationError("--verify", SCRCTL_TR("Frame number must be a positive integer"));
            }
            o.verify_path = verify[1];
        }
        if (o.no_window && o.verify_at > 0) {
            throw CLI::ValidationError("--verify", SCRCTL_TR("Requires a window; incompatible with --no-window"));
        }
        if (o.no_video_playback && o.verify_at > 0) {
            throw CLI::ValidationError("--verify", SCRCTL_TR("Window readback requires video playback"));
        }
        const bool standalone = o.list_devices || o.list_apps || o.pair || o.copy_text.has_value() ||
                                o.paste || o.show_version;
        const bool startup_control = !o.start_app.empty() || !o.test_touch.empty() ||
                                     !o.test_button.empty() || !o.test_type.empty();
        if (!standalone && o.no_video && o.no_audio &&
            (o.no_control || (o.no_window && !startup_control))) {
            throw CLI::ValidationError("--no-playback", SCRCTL_TR("Nothing to do: enable media, recording or device control"));
        }
    } catch (const CLI::CallForHelp &) {
        if (!language.select()) return ParseResult::Error;
        std::printf("%s", language.help().c_str());
        return ParseResult::ExitSuccess;
    } catch (const CLI::ParseError &e) {
        language.select();
        std::fprintf(stderr, "%s\n", e.what());
        return ParseResult::Error;
    }
    return ParseResult::Run;
}

} // namespace scrctl::app
