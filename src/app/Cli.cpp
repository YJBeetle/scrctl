#include "i18n/Translation.h"
#include "i18n/CliLanguage.h"
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
    CLI::App app{SCRCTL_N_("iOS screen mirroring and control")};
    app.footer(SCRCTL_N_(
        "With no arguments, mirror the connected device.\nThe device chooses encoding "
        "dimensions, bitrate and frame rate; overrides are unavailable.\nLeft mouse "
        "button maps to touch. Orientation does not support flip; recording preserves "
        "the source bitstream.\nWireless use requires pairing. --help / --version do not "
        "connect to the device."));
    app.set_help_flag("-h,--help", SCRCTL_N_("Show help"));
    app.add_option("--play", o.path, SCRCTL_N_("Play an Annex-B HEVC file"));
    app.add_option("-s,--serial", o.serial, SCRCTL_N_("Device UDID"));
    app.add_option("--wifi", o.wifi, SCRCTL_N_("LAN address (requires an existing pairing record)"));
    app.add_option("-r,--record", o.record, SCRCTL_N_("Record the live stream as Annex-B"));
    app.add_option("--start-app", o.start_app, SCRCTL_N_("Launch bundle ID; ? matches name prefix, + terminates the previous instance"));
    app.add_option("--window-title,--title", o.title, SCRCTL_N_("Window title"));
    app.add_option("--render-driver", o.render_driver, SCRCTL_N_("SDL render driver, e.g. metal / software"));
    app.add_option("--video-source", o.video_source,
                   SCRCTL_N_("stream for live video or screenshot for polling; default: stream"))
        ->check(CLI::IsMember({"stream", "screenshot"}));
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
    app.add_flag("--list-devices", o.list_devices, SCRCTL_N_("List connected devices"));
    app.add_flag("-n,--no-control", o.no_control, SCRCTL_N_("Disable input control"));
    app.add_flag("--list-apps", o.list_apps, SCRCTL_N_("List device apps"));
    app.add_flag("--version", o.show_version, SCRCTL_N_("Show version"));
    app.add_flag("-f,--fullscreen", o.fullscreen, SCRCTL_N_("Desktop fullscreen"));
    app.add_flag("--always-on-top", o.always_on_top, SCRCTL_N_("Keep window on top"));
    app.add_flag("--window-borderless", o.borderless, SCRCTL_N_("Borderless window"));
    app.add_flag("--disable-screensaver", o.disable_screensaver, SCRCTL_N_("Prevent local screen sleep while running"));
    app.add_flag("--no-audio", o.no_audio, SCRCTL_N_("Do not start audio stream"));
    app.add_flag("--no-audio-playback", o.no_audio_playback, SCRCTL_N_("Receive and decode audio without local playback"));
    app.add_flag("--no-window", o.no_window, SCRCTL_N_("Run without a window"));
    app.add_flag("--hw-decode", o.hw_decode, SCRCTL_N_("Use platform hardware decoder; default: software"));
    app.add_flag("--debug-input", o.debug_input, SCRCTL_N_("Print input coordinates"));
    app.add_flag("--debug-net", o.debug_net, SCRCTL_N_("Add tunnel diagnostics to --stats"));
    app.add_flag("--stats", o.stats, SCRCTL_N_("Print statistics every second"));
    app.add_flag("--paste", o.paste, SCRCTL_N_("Read device clipboard; with --copy, read back after writing"));
    app.add_option("--window-x", o.win_x, SCRCTL_N_("Window horizontal position; default: centered"));
    app.add_option("--window-y", o.win_y, SCRCTL_N_("Window vertical position; default: centered"));
    app.add_option("--window-width", o.win_w, SCRCTL_N_("Window width; 0 for automatic"))->check(CLI::NonNegativeNumber);
    app.add_option("--window-height", o.win_h, SCRCTL_N_("Window height; 0 for automatic"))->check(CLI::NonNegativeNumber);
    app.add_option("--time-limit", o.time_limit, SCRCTL_N_("Run duration in seconds; 0 for unlimited"))
        ->check(CLI::NonNegativeNumber);
    app.add_option("--audio-buffer", o.audio_buffer_ms, SCRCTL_N_("Audio buffer in milliseconds; default: 50, maximum: 1000"))
        ->check(CLI::NonNegativeNumber);
    app.add_option("--exit-after", o.exit_after, SCRCTL_N_("Exit after N frames; 0 for unlimited"))
        ->check(CLI::NonNegativeNumber);
    auto *scale = app.add_option("--scale", o.scale, SCRCTL_N_("Finite positive scale; default: fit screen"));
    std::string orientation, crop, background;
    std::vector<std::string> verify;
    app.add_option("--display-orientation,--orientation", orientation,
                   SCRCTL_N_("Clockwise display orientation; auto follows device and does not affect recording"))
        ->check(CLI::IsMember({"auto", "0", "90", "180", "270"}));
    app.add_option("--crop", crop, SCRCTL_N_("Crop WxH+X+Y or W:H:X:Y"));
    app.add_option("--background-color", background, SCRCTL_N_("Background color #RRGGBB"));
    app.add_option("--verify", verify, SCRCTL_N_("Read window at frame N into BMP: N FILE (requires a window)"))->expected(2);
    i18n::CliLanguage language(app);
    try {
        app.parse(argc, argv);
        if (!language.select()) return ParseResult::Error;
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
                                           SCRCTL_TR("Expected WxH+X+Y or W:H:X:Y, with positive dimensions and nonnegative offsets"));
            }
            o.crop_set = true;
        }
        if (app.count("--background-color")) {
            if (background.size() != 7 || background[0] != '#' ||
                background.find_first_not_of("0123456789abcdefABCDEF", 1) != std::string::npos) {
                throw CLI::ValidationError("--background-color", SCRCTL_TR("Expected #RRGGBB"));
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
                throw CLI::ValidationError("--verify", SCRCTL_TR("Frame number must be a positive integer"));
            }
            o.verify_path = verify[1];
        }
        if (o.no_window && o.verify_at > 0) {
            throw CLI::ValidationError("--verify", SCRCTL_TR("Requires a window; incompatible with --no-window"));
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
