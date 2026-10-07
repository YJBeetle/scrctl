#include "app/Application.h"
#include "app/SdlRuntime.h"

#include <SDL.h>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
int failures = 0;
volatile std::sig_atomic_t caller_signal = 0;
void caller_handler(int signal) {
    caller_signal = signal;
#ifdef _WIN32
    // Windows CRT 的 signal 处理器只生效一次；测试调用者显式保留自己的处理器。
    std::signal(signal, caller_handler);
#endif
}

void check(bool ok, const char *message) {
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

int run(std::vector<std::string> args) {
    std::vector<char *> argv;
    for (auto &arg : args) {
        argv.push_back(arg.data());
    }
    return scrctl::app::run(static_cast<int>(argv.size()), argv.data());
}
} // namespace

int main() {
    const auto previous_int = std::signal(SIGINT, caller_handler);
    const auto previous_term = std::signal(SIGTERM, caller_handler);
    {
        scrctl::app::SdlRuntime runtime;
        check(runtime.initialize(SDL_INIT_TIMER), "initialize timer");
        std::raise(SIGTERM);
        std::raise(SIGTERM);
        check(runtime.stop_requested() && caller_signal == 0,
              "signal requests orderly exit while runtime is active");
    }
    check(SDL_WasInit(0) == 0, "SDL shuts down after signal");
    std::raise(SIGINT);
    check(caller_signal == SIGINT, "restore caller SIGINT handler");
    std::raise(SIGTERM);
    check(caller_signal == SIGTERM, "restore caller SIGTERM handler");
    {
        scrctl::app::SdlRuntime runtime;
        check(runtime.initialize(SDL_INIT_TIMER) && !runtime.stop_requested(),
              "next runtime does not inherit previous exit request");
    }

    const auto path =
        std::filesystem::temp_directory_path() /
        ("scrctl-lifecycle-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".hevc");
    std::ofstream(path, std::ios::binary).close();
    SDL_setenv("SDL_VIDEODRIVER", "scrctl-invalid-driver", 1);
    check(run({"scrctl", "--play", path.string(), "--no-audio"}) == 1,
          "application reports SDL initialization failure");
    check(SDL_WasInit(0) == 0, "failed initialization cleans partially started SDL subsystems");
    check(run({"scrctl", "--play", path.string(), "--no-window", "--no-audio"}) == 0,
          "headless playback does not require video initialization");
    check(SDL_WasInit(0) == 0, "normal application return cleans SDL");
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    const auto readback_path = path.parent_path() / (path.stem().string() + ".bmp");
    check(run({"scrctl", "--play", path.string(), "--no-audio", "--verify", "1",
               readback_path.string()}) == 1,
          "empty playback reports that requested readback frame was not reached");
    check(!std::filesystem::exists(readback_path), "unreached readback does not create an image");
    check(SDL_WasInit(0) == 0, "unreached readback cleans SDL");
    caller_signal = 0;
    std::raise(SIGTERM);
    check(caller_signal == SIGTERM, "application restores caller signal handler");
    std::filesystem::remove(path);
    if (previous_int != SIG_ERR) {
        std::signal(SIGINT, previous_int);
    }
    if (previous_term != SIG_ERR) {
        std::signal(SIGTERM, previous_term);
    }
    return failures ? 1 : 0;
}
