#include "app/Application.h"
#include "app/LiveSource.h"
#include "app/SdlRuntime.h"
#include "decode/Decoder.h"

#include <SDL.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {
int failures = 0;
int checks = 0;
volatile std::sig_atomic_t caller_signal = 0;
void caller_handler(int signal) {
    caller_signal = signal;
#ifdef _WIN32
    // Windows CRT 的 signal 处理器只生效一次；测试调用者显式保留自己的处理器。
    std::signal(signal, caller_handler);
#endif
}

void check(bool ok, const char *message) {
    ++checks;
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

// 只控制公开解码器工厂，运行真实 Application/FileSource/Presenter/SDL 路径。
// 小 NAL 夹具不代表合法 HEVC；此处验收窗口生命周期，不验收编解码器。
struct PlaybackState {
    bool active = false;
    bool padding_only = false;
    bool watching = false;
    int decoded = 0;
    std::atomic<Uint32> window_id{0};
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
} playback;

int SDLCALL observe_window(void *, SDL_Event *event) {
    if (event && event->type == SDL_WINDOWEVENT) {
        Uint32 empty = 0;
        playback.window_id.compare_exchange_strong(empty, event->window.windowID);
    }
    return 1;
}

class ControlledPlaybackDecoder final : public scrctl::Decoder {
  public:
    ~ControlledPlaybackDecoder() override {
        if (playback.watching) SDL_DelEventWatch(observe_window, nullptr);
        playback.watching = false;
    }
    bool configure(const scrctl::Nal &, const scrctl::Nal &, const scrctl::Nal &) override {
        return true;
    }
    bool decode(const std::vector<scrctl::Nal> &, scrctl::Frame &out) override {
        if (!playback.active) return false;
        const int index = ++playback.decoded;
        if (index == 1) {
            SDL_AddEventWatch(observe_window, nullptr);
            playback.watching = true;
        } else if (index == 2) {
            playback.window = SDL_GetWindowFromID(playback.window_id.load());
            check(playback.window != nullptr, "actual Application created a window before the next frame");
            if (playback.window) {
                playback.renderer = SDL_GetRenderer(playback.window);
                SDL_SetWindowSize(playback.window, 96, 144);
                SDL_SetWindowPosition(playback.window, 100, 120);
            }
        } else if (index <= 4) {
            SDL_Window *window = SDL_GetWindowFromID(playback.window_id.load());
            check(window && window == playback.window &&
                      SDL_GetRenderer(window) == playback.renderer,
                  "actual Application retains windowID and renderer across content changes");
            if (window) {
                int w = 0, h = 0, x = 0, y = 0;
                SDL_GetWindowSize(window, &w, &h);
                SDL_GetWindowPosition(window, &x, &y);
                const bool landscape = !playback.padding_only && index == 3;
                check(w == (landscape ? 144 : 96) && h == (landscape ? 96 : 144) &&
                          x == 100 && y == 120,
                      "actual Application retains user scale and position instead of reapplying startup parameters");
            }
        }
        if (index > 4) return false;
        const bool landscape = !playback.padding_only && index == 2;
        out.width = playback.padding_only && index == 2 ? 80 : landscape ? 96 : 64;
        out.height = playback.padding_only && index == 2 ? 112 : landscape ? 64 : 96;
        out.row_pitch = out.width * 4;
        out.pixels.assign(static_cast<std::size_t>(out.row_pitch) * out.height, 0x80);
        return true;
    }
    const char *backend_name() const override { return "ControlledPlaybackLifecycle"; }
};

void playback_window_lifecycle(const std::filesystem::path &path, bool padding_only) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    const auto nal = [&](std::initializer_list<unsigned char> bytes) {
        const unsigned char prefix[]{0, 0, 0, 1};
        file.write(reinterpret_cast<const char *>(prefix), sizeof prefix);
        for (const auto byte : bytes) file.put(static_cast<char>(byte));
    };
    nal({0x40, 1, 0x55}); nal({0x42, 1, 0x55}); nal({0x44, 1, 0x55});
    for (unsigned char i = 1; i <= 4; ++i) nal({0x26, 1, 0x80, i, 0x55});
    file.close();
    playback.active = true;
    playback.padding_only = padding_only;
    playback.decoded = 0;
    playback.window_id.store(0);
    playback.window = nullptr; playback.renderer = nullptr;
    std::vector<std::string> args{"scrctl", "--play", path.string(), "--no-audio",
                                  "--window-width", "80", "--window-height", "120",
                                  "--window-x", "40", "--window-y", "60", "--exit-after", "4"};
    if (padding_only) args.insert(args.end(), {"--crop", "64:96:0:0"});
    check(run(std::move(args)) == 0 && playback.decoded == 4,
          "actual Application consumes controlled content and exits normally");
    check(SDL_WasInit(0) == 0 && !playback.watching,
          "controlled playback releases its window, observer and SDL runtime");
    playback.active = false;
}
} // namespace

namespace scrctl {
std::unique_ptr<Decoder> create_software_decoder() {
    // 空文件的既有回归仍应经过 Annex-B 参数集检查，不被“没有解码器”提前遮住。
    return std::make_unique<ControlledPlaybackDecoder>();
}
std::unique_ptr<Decoder> create_platform_decoder() {
    return create_software_decoder();
}
} // namespace scrctl

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
        scrctl::app::LiveSource source;
        std::string err;
        check(!source.start("must-not-connect", "must-not-resolve", "", false, false,
                            true, 200, "stream", "", err, 49152, false,
                            [&runtime] { return runtime.stop_requested(); }) &&
                  !err.empty() && !source.has_audio(),
              "an exit signal cancels actual startup before device connection or audio routing");
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
    SDL_setenv("SDL_AUDIODRIVER", "scrctl-invalid-audio-driver", 1);
    check(run({"scrctl", "--record", "must-not-create.mkv", "--serial", "must-not-connect",
               "--no-window"}) == 1 && !std::filesystem::exists("must-not-create.mkv"),
          "audio output startup failure cannot silently turn requested AV recording into video-only");
    check(SDL_WasInit(0) == 0, "rejected AV recording cleans SDL before device connection");
    {
        scrctl::app::SdlRuntime runtime;
        std::string err;
        check(!runtime.prepare_audio(true, true, err) && !err.empty(),
              "audio preparation without the owning SDL runtime fails safely");
        check(runtime.initialize(SDL_INIT_TIMER), "initialize runtime before audio gating");
        check(!runtime.prepare_audio(false, true, err) && err.empty() &&
                  SDL_WasInit(SDL_INIT_AUDIO) == 0,
              "no-audio does not initialize the failing audio backend");
        check(runtime.prepare_audio(true, false, err) && err.empty() &&
                  SDL_WasInit(SDL_INIT_AUDIO) == 0,
              "explicit no-audio-playback may capture without an audio output backend");
        check(!runtime.prepare_audio(true, true, err) && !err.empty() &&
                  SDL_WasInit(SDL_INIT_AUDIO) == 0 && SDL_WasInit(SDL_INIT_TIMER) != 0,
              "audio backend failure disables the audio request while preserving other SDL subsystems");
        SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
        check(runtime.prepare_audio(true, true, err) && err.empty() &&
                  SDL_WasInit(SDL_INIT_AUDIO) != 0,
              "a usable backend allows audio streaming before the device route is changed");
    }
    check(SDL_WasInit(0) == 0, "audio preparation and failures are cleaned by the runtime");

    {
        scrctl::app::LiveSource source;
        std::string err;
        const auto reject_encoded = [&](const std::string &record, const std::string &kind,
                                       bool hardware, const std::string &degrade,
                                       const char *expected) {
            err.clear();
            return !source.start("must-not-connect", "must-not-resolve", record,
                                 hardware, false, false, 200, kind, degrade, err,
                                 49152, false, {}, 0, true, false) &&
                   err.find(expected) != std::string::npos && !source.has_audio() &&
                   !source.video_decoding_enabled();
        };
        check(reject_encoded("", "stream", false, "", "requires a recording consumer"),
              "encoded video without a consumer fails before device connection");
        check(reject_encoded("must-not-create.hevc", "screenshot", false, "", "requires a live video stream") &&
                  !std::filesystem::exists("must-not-create.hevc"),
              "encoded video cannot silently start screenshots or create a raw recording");
        check(reject_encoded("must-not-create.hevc", "stream", true, "", "require video decoding") &&
                  !std::filesystem::exists("must-not-create.hevc"),
              "encoded capture cannot ignore a requested hardware decoder");
        check(reject_encoded("must-not-create.hevc", "stream", false, "1,2", "require video decoding") &&
                  !std::filesystem::exists("must-not-create.hevc"),
              "encoded capture cannot ignore a requested screenshot fallback test");
    }

    {
        scrctl::app::LiveSource source;
        std::string err = "previous error";
        check(!source.start("must-not-connect", "must-not-resolve", "", false, false,
                            true, 200, "stream", "", err, 49152, false, {}, 0, false) &&
                  err.find("requires a container recording") != std::string::npos && !source.has_audio(),
              "capture-only without a container fails before connecting a device or routing audio");
        check(source.keyboard_state({}, err) && err.empty(),
              "empty keyboard cleanup needs no device session or HID connection");
        err = "previous error";
        check(source.type_text("", 0, err) && err.empty(),
              "empty text needs no device session and leaves keyboard ownership unchanged");
        check(!source.keyboard_state({scrctl::hid::key::kA}, err) && !err.empty(),
              "keyboard input without a device session fails safely");
        const auto first_error = err;
        err.clear();
        check(!source.control(0.5, 0.5, true, err) && err == first_error,
              "touch shares keyboard failure state and preserves its first error");
        err.clear();
        check(!source.type_text("", 0, err) && err == first_error,
              "empty text preserves a latched input failure");
    }

    const auto path =
        std::filesystem::temp_directory_path() /
        ("scrctl-lifecycle-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".hevc");
    std::ofstream(path, std::ios::binary).close();
    SDL_setenv("SDL_VIDEODRIVER", "scrctl-invalid-driver", 1);
    check(run({"scrctl", "--play", path.string() + ".missing", "--no-window", "--no-audio"}) == 1,
          "missing playback file reports failure without a verify request");
    check(SDL_WasInit(0) == 0, "file open failure cleans SDL");
    check(run({"scrctl", "--play", path.string(), "--no-audio"}) == 1,
          "application reports SDL initialization failure");
    check(SDL_WasInit(0) == 0, "failed initialization cleans partially started SDL subsystems");
    check(run({"scrctl", "--play", path.string(), "--no-window", "--no-audio"}) == 1,
          "empty playback fails even without a window or verify request");
    check(SDL_WasInit(0) == 0, "normal application return cleans SDL");
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    const auto readback_path = path.parent_path() / (path.stem().string() + ".bmp");
    check(run({"scrctl", "--play", path.string(), "--no-audio", "--verify", "1",
               readback_path.string()}) == 1,
          "empty playback fails before requested readback can be reached");
    check(!std::filesystem::exists(readback_path), "unreached readback does not create an image");
    check(SDL_WasInit(0) == 0, "unreached readback cleans SDL");
    playback_window_lifecycle(path, false);
    playback_window_lifecycle(path, true);
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
    std::printf("app_lifecycle_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
