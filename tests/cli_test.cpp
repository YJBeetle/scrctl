#include "app/Cli.h"
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using scrctl::app::Options;
using scrctl::app::ParseResult;

namespace {
int failures = 0;
ParseResult parse(std::vector<std::string> args, Options &options) {
    std::vector<char *> argv;
    for (auto &arg : args)
        argv.push_back(arg.data());
    return scrctl::app::parse_args(static_cast<int>(argv.size()), argv.data(), options);
}
void check(bool ok, const char *message) {
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}
} // namespace

int main() {
    Options o;
    check(parse({"scrctl", "-s", "device", "--scale=0.5", "--title=a=b", "--window-x", "-30",
                 "--orientation", "270", "--crop", "100:200:3:4", "--background-color=#aB10fF",
                 "--verify", "12", "frame.png", "--no-audio"},
                o) == ParseResult::Run,
          "aliases, equals syntax, negative position and two-value verify");
    check(o.serial == "device" && o.title == "a=b" && o.scale_given && o.scale == .5 &&
              o.win_x == -30 && o.orientation == 270 && o.crop_set && o.crop_w == 100 &&
              o.crop_y == 4 && o.bg[0] == 171 && o.bg[2] == 255 && o.verify_at == 12 &&
              o.verify_path == "frame.png" && o.no_audio,
          "parsed values preserved");
    Options defaults;
    check(parse({"scrctl"}, defaults) == ParseResult::Run && !defaults.win_x &&
              !defaults.win_y && defaults.scale == 1 && !defaults.scale_given &&
              defaults.audio_buffer_ms == 50 && !defaults.audio_dup &&
              defaults.shortcut_mods == (KMOD_LALT | KMOD_LGUI),
          "default options");
    Options duplicate_audio;
    check(parse({"scrctl", "--audio-dup"}, duplicate_audio) == ParseResult::Run &&
              duplicate_audio.audio_dup && !duplicate_audio.no_audio,
          "audio duplication is an explicit option; default routing remains computer-only");
    Options capture_audio;
    check(parse({"scrctl", "--audio-dup", "--no-audio-playback"}, capture_audio) ==
              ParseResult::Run && capture_audio.audio_dup && capture_audio.no_audio_playback,
          "phone playback can be retained while disabling computer playback");
    Options pair;
    check(parse({"scrctl", "--pair", "-s", "device"}, pair) == ParseResult::Run &&
              pair.pair && !pair.repair_pairing && pair.serial == "device",
          "USB remote pairing is an explicit standalone command");
    Options repair;
    check(parse({"scrctl", "--pair", "--repair-pairing"}, repair) == ParseResult::Run &&
              repair.pair && repair.repair_pairing,
          "repair requires explicit pairing mode");
    Options wifi_port;
    check(parse({"scrctl", "--wifi", "10.0.0.7", "--wifi-port", "55000"}, wifi_port) == ParseResult::Run &&
              wifi_port.wifi_port == 55000,
          "manual wireless address preserves a specified RemotePairing port");
    Options wifi_auto;
    check(parse({"scrctl", "--wifi", "auto"}, wifi_auto) == ParseResult::Run &&
              wifi_auto.wifi == "auto",
          "wireless discovery mode");
    Options display_source;
    check(parse({"scrctl", "--video-source=display"}, display_source) == ParseResult::Run &&
              display_source.video_source == "stream",
          "scrcpy display source selects the existing live stream implementation");
    Options legacy_source;
    check(parse({"scrctl", "--video-source=stream"}, legacy_source) == ParseResult::Run &&
              legacy_source.video_source == "stream",
          "the stream source remains available for existing scripts");
    Options screenshot_source;
    check(parse({"scrctl", "--video-source=screenshot"}, screenshot_source) == ParseResult::Run &&
              screenshot_source.video_source == "screenshot",
          "screenshot polling remains a distinct extension");
    Options mkv;
    check(parse({"scrctl", "-r", "capture.MkV", "--no-audio"}, mkv) == ParseResult::Run &&
              mkv.record == "capture.MkV" && mkv.no_audio,
          "container recording accepts a video-only request and preserves its path");
    Options invalid_mkv;
    check(parse({"scrctl", "-r", "capture.MKV", "--video-source=screenshot"}, invalid_mkv) ==
              ParseResult::Error,
          "screenshot container recording is rejected before connecting to a device");
    Options mp4;
    check(parse({"scrctl", "-r", "capture.Mp4", "--audio-dup", "--no-audio-playback"}, mp4) ==
              ParseResult::Run && mp4.record == "capture.Mp4" && mp4.audio_dup && mp4.no_audio_playback,
          "MP4 recording preserves the selected audio route and capture-only request");
    Options invalid_mp4;
    check(parse({"scrctl", "-r", "capture.mp4", "--video-source=screenshot"}, invalid_mp4) ==
              ParseResult::Error,
          "screenshot MP4 recording is rejected before connecting to a device");
    Options playback_recording;
    check(parse({"scrctl", "--play", "input.hevc", "--record", "output.mkv"},
                playback_recording) == ParseResult::Error,
          "file playback cannot silently ignore a requested recording");
    Options positions;
    check(parse({"scrctl", "--window-x", "-30", "--window-y=50"}, positions) ==
              ParseResult::Run && positions.win_x == -30 && positions.win_y == 50,
          "explicit window positions");
    check(parse({"scrctl", "--window-x=auto"}, positions) == ParseResult::Run &&
              !positions.win_x && positions.win_y == 50,
          "auto clears only the specified horizontal position on a later parse");
    check(parse({"scrctl", "--window-y", "auto"}, positions) == ParseResult::Run &&
              !positions.win_x && !positions.win_y,
          "auto clears the specified vertical position on a later parse");
    check(parse({"scrctl", "--window-x=0", "--window-y=-12"}, positions) == ParseResult::Run &&
              positions.win_x == 0 && positions.win_y == -12,
          "explicit positions replace automatic positions, preserving zero and negatives");
    check(parse({"scrctl"}, positions) == ParseResult::Run &&
              positions.win_x == 0 && positions.win_y == -12,
          "an omitted position does not clear an existing optional value");
    check(parse({"scrctl", "--window-x", std::to_string(std::numeric_limits<int>::min()),
                 "--window-y", std::to_string(std::numeric_limits<int>::max())}, positions) ==
              ParseResult::Run && positions.win_x == std::numeric_limits<int>::min() &&
              positions.win_y == std::numeric_limits<int>::max(),
          "window positions preserve the existing integer bounds");
    for (const char *color : {"aB10fF", "#aB10fF", "aBf", "#aBf"}) {
        Options colors;
        const bool shorthand = std::string(color).find("10") == std::string::npos;
        check(parse({"scrctl", "--background-color", color}, colors) == ParseResult::Run &&
                  colors.bg[0] == (shorthand ? 170 : 171) &&
                  colors.bg[1] == (shorthand ? 187 : 16) && colors.bg[2] == 255,
              "background color accepts short and long hex with or without a hash");
    }
    const std::vector<std::pair<std::string, uint16_t>> modifier_cases = {
        {"lctrl", KMOD_LCTRL}, {"rctrl", KMOD_RCTRL}, {"lalt", KMOD_LALT},
        {"ralt", KMOD_RALT}, {"lsuper", KMOD_LGUI}, {"rsuper", KMOD_RGUI},
        {"lctrl,lsuper", KMOD_LCTRL | KMOD_LGUI},
        {"lalt,lalt", KMOD_LALT},
        {"lctrl,rctrl,lalt,ralt,lsuper,rsuper", KMOD_CTRL | KMOD_ALT | KMOD_GUI},
    };
    for (const auto &[names, mask] : modifier_cases) {
        Options modifiers;
        check(parse({"scrctl", "--shortcut-mod", names}, modifiers) == ParseResult::Run &&
                  modifiers.shortcut_mods == mask,
              "shortcut modifier names map to SDL masks and duplicate keys are idempotent");
    }
    Options changed_modifiers;
    check(parse({"scrctl", "--shortcut-mod=rctrl"}, changed_modifiers) == ParseResult::Run &&
              changed_modifiers.shortcut_mods == KMOD_RCTRL,
          "an explicit modifier replaces the default mask");
    check(parse({"scrctl", "--shortcut-mod=lalt,lsuper"}, changed_modifiers) ==
              ParseResult::Run && changed_modifiers.shortcut_mods == (KMOD_LALT | KMOD_LGUI),
          "a later modifier list replaces the previously selected mask");
    check(parse({"scrctl", "--shortcut-mod=lctrl,"}, changed_modifiers) ==
              ParseResult::Error && changed_modifiers.shortcut_mods == (KMOD_LALT | KMOD_LGUI),
          "an invalid list does not publish a partially parsed modifier mask");
    Options width_only;
    check(parse({"scrctl", "--window-width", "240"}, width_only) == ParseResult::Run &&
              width_only.win_w == 240 && width_only.win_h == 0,
          "window width can be provided independently");
    Options height_only;
    check(parse({"scrctl", "--window-height", "480"}, height_only) == ParseResult::Run &&
              height_only.win_w == 0 && height_only.win_h == 480,
          "window height can be provided independently");
    Options automatic_size;
    check(parse({"scrctl", "--window-width", "0", "--window-height", "0"}, automatic_size) ==
              ParseResult::Run && automatic_size.win_w == 0 && automatic_size.win_h == 0,
          "zero window dimensions preserve automatic sizing");
    Options empty;
    check(parse({"scrctl", "--title", ""}, empty) == ParseResult::Run && empty.title.empty(),
          "explicit empty string argument");
    check(!defaults.copy_text, "clipboard write is absent by default");
    Options empty_copy;
    check(parse({"scrctl", "--copy", ""}, empty_copy) == ParseResult::Run &&
              empty_copy.copy_text && empty_copy.copy_text->empty(),
          "explicit empty clipboard write remains a standalone command");
    Options unicode_copy;
    check(parse({"scrctl", "--copy=中文🙂", "--paste"}, unicode_copy) == ParseResult::Run &&
              unicode_copy.copy_text && *unicode_copy.copy_text == "中文🙂" && unicode_copy.paste,
          "Unicode clipboard write and read-back options remain distinct");
    Options readonly_copy;
    check(parse({"scrctl", "--no-control", "--copy", "", "--paste"}, readonly_copy) ==
              ParseResult::Run && readonly_copy.no_control && readonly_copy.copy_text &&
              readonly_copy.copy_text->empty() && readonly_copy.paste,
          "no-control preserves standalone clipboard write and read-back commands");
    for (const std::vector<std::string> &injection : {
             std::vector<std::string>{"--test-touch", "0,0,1,1"},
             std::vector<std::string>{"--test-button", "home"},
             std::vector<std::string>{"--test-type", "q"},
             std::vector<std::string>{"--test-type", ""}}) {
        auto args = injection;
        args.insert(args.begin(), {"scrctl", "--no-control"});
        Options conflict;
        check(parse(args, conflict) == ParseResult::Error,
              "no-control rejects each explicitly requested input injection");
        args.erase(args.begin() + 1);
        args.push_back("-n");
        Options reverse_conflict;
        check(parse(args, reverse_conflict) == ParseResult::Error,
              "no-control conflicts are independent of argument order and short alias");
    }
    Options input;
    Options discovery;
    check(parse({"scrctl", "--list-devices"}, discovery) == ParseResult::Run &&
              discovery.list_devices && discovery.discovery_timeout_ms == 3000,
          "device listing defaults to a bounded three-second wireless scan");
    for (const char *timeout : {"0", "60000"}) {
        Options bounded;
        check(parse({"scrctl", "--list-devices", "--discovery-timeout", timeout}, bounded) ==
                  ParseResult::Run && bounded.discovery_timeout_ms == std::stoi(timeout),
              "wireless discovery accepts both timeout bounds");
    }
    check(parse({"scrctl", "--test-touch", "0,.25,.75,1", "--test-button", "home"}, input) ==
              ParseResult::Run && input.test_touch == std::vector<double>({0, .25, .75, 1}) &&
              input.test_button_code != 0, "typed input coordinates and HID button");
    for (const char *name : {"home", "lock", "volup", "voldn", "mute"}) {
        Options button;
        check(parse({"scrctl", "--test-button", name}, button) == ParseResult::Run &&
                  button.test_button == name && button.test_button_code != 0,
              "all supported button names");
    }
    const std::vector<std::vector<std::string>> invalid = {
        {"--test-touch", ""},
        {"--test-touch", "0,0,1"},
        {"--test-touch", "0,0,1,1,1"},
        {"--test-touch", "0,0,1,1junk"},
        {"--test-touch", "0,0,nan,1"},
        {"--test-touch", "0,0,inf,1"},
        {"--test-touch", "0,0,1.01,1"},
        {"--test-touch", "-.1,0,1,1"},
        {"--test-button", "invalid-button"},
        {"--window-width"},
        {"--window-width", "abc"},
        {"--window-x"},
        {"--window-x", ""},
        {"--window-y", ""},
        {"--window-x", "Auto"},
        {"--window-y", "12junk"},
        {"--window-x", std::to_string(static_cast<long long>(std::numeric_limits<int>::max()) + 1)},
        {"--window-y", std::to_string(static_cast<long long>(std::numeric_limits<int>::min()) - 1)},
        {"--window-width", "-1"},
        {"--window-height", "-1"},
        {"--window-width", std::to_string(static_cast<long long>(std::numeric_limits<int>::max()) + 1)},
        {"--window-height", std::to_string(static_cast<long long>(std::numeric_limits<int>::max()) + 1)},
        {"--time-limit", "3junk"},
        {"--exit-after", "-1"},
        {"--scale", "nan"},
        {"--scale", "0"},
        {"--verify", "2"},
        {"--verify", "0", "frame.png"},
        {"--verify", "2", "frame.png", "--no-window"},
        {"--crop", "1:2:3:4junk"},
        {"--crop", "0:2:3:4"},
        {"--background-color", "#001122junk"},
        {"--background-color", "#zz1122"},
        {"--background-color", ""},
        {"--background-color", "#"},
        {"--background-color", "12"},
        {"--background-color", "#1234"},
        {"--background-color", " 123456"},
        {"--background-color", "0x123456"},
        {"--shortcut-mod"},
        {"--shortcut-mod", ""},
        {"--shortcut-mod", ",lalt"},
        {"--shortcut-mod", "lalt,"},
        {"--shortcut-mod", "lalt,,lsuper"},
        {"--shortcut-mod", "lalt+lsuper"},
        {"--shortcut-mod", "LALT"},
        {"--shortcut-mod", "lalt, lsuper"},
        {"--shortcut-mod", "ctrl"},
        {"--shortcut-mod", "none"},
        {"--video-source", "unknown"},
        {"--video-source", "camera"},
        {"--no-audio", "--audio-dup"},
        {"--audio-dup", "--no-audio"},
        {"--orientation", "45"},
        {"--discovery-timeout", "10"},
        {"--pair", "--wifi", "10.0.0.1"},
        {"--pair", "--list-devices"},
        {"--pair", "--play", "video.hevc"},
        {"--repair-pairing"},
        {"--wifi-port", "49152"},
        {"--wifi", "10.0.0.1", "--wifi-port", "0"},
        {"--wifi", "10.0.0.1", "--wifi-port", "65536"},
        {"--wifi", "auto", "--wifi-port", "49152"},
        {"--list-devices", "--discovery-timeout", "-1"},
        {"--list-devices", "--discovery-timeout", "60001"},
        {"--list-devices", "--discovery-timeout", "invalid"},
        {"--unknown"},
    };
    for (auto args : invalid) {
        args.insert(args.begin(), "scrctl");
        Options candidate;
        check(parse(args, candidate) == ParseResult::Error, args[1].c_str());
    }
    Options help;
    check(parse({"scrctl", "--help"}, help) == ParseResult::ExitSuccess,
          "help returns without terminating process");
    return failures ? 1 : 0;
}
