#include "app/Cli.h"
#include <cstdio>
#include <string>
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
              defaults.scale == 1 && !defaults.scale_given && defaults.audio_buffer_ms == 50,
          "default options");
    Options empty;
    check(parse({"scrctl", "--title", ""}, empty) == ParseResult::Run && empty.title.empty(),
          "explicit empty string argument");
    const std::vector<std::vector<std::string>> invalid = {
        {"--window-width"},
        {"--window-width", "abc"},
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
        {"--video-source", "unknown"},
        {"--orientation", "45"},
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
