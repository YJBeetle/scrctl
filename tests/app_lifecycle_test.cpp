#include "app/Application.h"
#include "app/LiveSource.h"
#include "app/LiveStats.h"
#include "app/SdlRuntime.h"
#include "decode/Decoder.h"
#include "i18n/Translation.h"

#include <SDL.h>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

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

std::string stats_output(scrctl::app::LiveStats &stats,
                         const scrctl::app::LiveStats::Snapshot &snapshot) {
    std::fflush(stdout);
    FILE *file = std::tmpfile();
    check(file != nullptr, "formatter output capture creates its private temporary file");
    if (file == nullptr) return {};
#ifdef _WIN32
    const int output = _fileno(stdout);
    const int saved = _dup(output);
    const int redirected = saved < 0 ? -1 : _dup2(_fileno(file), output);
#else
    const int output = fileno(stdout);
    const int saved = dup(output);
    const int redirected = saved < 0 ? -1 : dup2(fileno(file), output);
#endif
    check(saved >= 0 && redirected >= 0, "formatter output capture redirects only this process stdout");
    if (saved >= 0 && redirected >= 0) {
        stats.print(snapshot);
        std::fflush(stdout);
#ifdef _WIN32
        check(_dup2(saved, output) >= 0, "formatter output capture restores stdout");
#else
        check(dup2(saved, output) >= 0, "formatter output capture restores stdout");
#endif
    }
#ifdef _WIN32
    if (saved >= 0) _close(saved);
#else
    if (saved >= 0) close(saved);
#endif
    std::rewind(file);
    std::string result;
    char buffer[2048];
    while (const auto size = std::fread(buffer, 1, sizeof buffer, file)) result.append(buffer, size);
    std::fclose(file);
    return result;
}

void check_audio_rate(const std::string &output, double received) {
    const auto position = output.find("  Audio: received ");
    double actual = -1, decoded = -1, delivered = -1;
    const int fields = position == std::string::npos ? 0 :
        std::sscanf(output.c_str() + position,
                    "  Audio: received %lf packets/s, decoded %lf packets/s, delivered %lf frames/s",
                    &actual, &decoded, &delivered);
    check(fields == 3 && actual == received && decoded == 0 && delivered == 0,
          "actual formatter reports the independent audio packet rate with zero PCM decoding or delivery");
}

void audio_only_stats() {
    using Stats = scrctl::app::LiveStats;
    check(scrctl::i18n::initialize("en"), "formatter regression selects English deterministically");
    Stats stats;
    stats.audio_started(1000);
    Stats::Snapshot snapshot;
    Stats::Tcp tcp;
    tcp.now_ms = 9000;
    tcp.recv_bytes = 1024;
    snapshot.tcp = tcp;
    Stats::Audio audio;
    audio.now_ms = 2000;
    audio.counters.packets = 100;
    audio.counters.rtcp_sent = 5;
    audio.counters.rtcp_failed = 1;
    snapshot.audio = audio;
    auto output = stats_output(stats, snapshot);
    check_audio_rate(output, 100);
    check(output.find("  Tunnel TCP:") != std::string::npos && output.find("  Video:") == std::string::npos,
          "audio-only snapshot prints its actual tunnel and audio without inventing a video line");
    check(output.find("RR sent/failed 5/1") != std::string::npos &&
              output.find("buffered 0 frames, interval 1.0 s") != std::string::npos &&
              output.find("Audio clock: inactive") != std::string::npos &&
              output.find("(output=closed)") != std::string::npos,
          "capture-only statistics retain RR, the real audio interval and zero-buffer inactive playback");

    snapshot.tcp->now_ms = 19000;
    snapshot.audio->now_ms = 3500;
    snapshot.audio->counters.packets = 400;
    snapshot.audio->counters.rtcp_sent = 8;
    snapshot.audio->counters.rtcp_failed = 2;
    output = stats_output(stats, snapshot);
    check_audio_rate(output, 200);
    check(output.find("RR sent/failed 8/2") != std::string::npos &&
              output.find("buffered 0 frames, interval 1.5 s") != std::string::npos,
          "the second audio-only print updates audio counters and its own time baseline, not the TCP window");

    Stats::Video video;
    video.now_ms = 29000;
    snapshot.video = video;
    snapshot.audio->now_ms = 5000;
    snapshot.audio->counters.packets = 550;
    output = stats_output(stats, snapshot);
    check_audio_rate(output, 100);
    check(output.find("  Video:") != std::string::npos && output.find("  Audio:") != std::string::npos,
          "ordinary AV statistics still print both existing media branches");

    stats.reset_screenshot(5000);
    Stats::Screenshot screenshot;
    screenshot.now_ms = 6000;
    screenshot.counters.frames = 4;
    snapshot.screenshot = screenshot;
    snapshot.audio->now_ms = 6000;
    snapshot.audio->counters.packets = 650;
    output = stats_output(stats, snapshot);
    check(output.find("  Screenshots:") != std::string::npos &&
              output.find("  Audio:") == std::string::npos && output.find("  Video:") == std::string::npos,
          "screenshot statistics retain their existing priority over background media");
    snapshot.screenshot.reset();
    snapshot.video.reset();
    snapshot.audio->now_ms = 7000;
    snapshot.audio->counters.packets = 750;
    output = stats_output(stats, snapshot);
    check_audio_rate(output, 100);
    check(output.find("buffered 0 frames, interval 2.0 s") != std::string::npos,
          "printing screenshots does not settle or lose the background audio interval");
}

// 只控制公开解码器工厂，运行真实 Application/FileSource/Presenter/SDL 路径。
// 小 NAL 夹具不代表合法 HEVC；此处验收窗口生命周期，不验收编解码器。
struct PlaybackState {
    bool active = false;
    bool padding_only = false;
    bool rotation = false;
    int base_degrees = 0;
    bool flip = false;
    bool watching = false;
    int decoded = 0;
    std::atomic<Uint32> window_id{0};
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
} playback;

constexpr std::array<uint32_t, 4> quadrant_colors{
    0xffff0000u, 0xff00ff00u, 0xff0000ffu, 0xffffff00u};

void queue_rotation(SDL_Keycode key, SDL_Scancode scancode) {
    SDL_Event event{};
    event.type = SDL_WINDOWEVENT;
    event.window.windowID = playback.window_id.load();
    event.window.event = SDL_WINDOWEVENT_FOCUS_GAINED;
    check(SDL_PushEvent(&event) == 1, "actual playback queues its own focus event");
    const auto send_key = [&](Uint32 type, SDL_Keycode symbol, SDL_Scancode code, Uint16 mods) {
        event = {};
        event.type = type;
        event.key.windowID = playback.window_id.load();
        event.key.state = type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
        event.key.keysym.sym = symbol;
        event.key.keysym.scancode = code;
        event.key.keysym.mod = mods;
        check(SDL_PushEvent(&event) == 1, "actual playback queues a complete MOD-arrow key sequence");
    };
    send_key(SDL_KEYDOWN, SDLK_LALT, SDL_SCANCODE_LALT, KMOD_LALT);
    send_key(SDL_KEYDOWN, key, scancode, KMOD_LALT);
    send_key(SDL_KEYUP, key, scancode, KMOD_LALT);
    send_key(SDL_KEYUP, SDLK_LALT, SDL_SCANCODE_LALT, KMOD_NONE);
}

void check_playback_rotation(int index) {
    SDL_Window *window = SDL_GetWindowFromID(playback.window_id.load());
    check(window && window == playback.window && SDL_GetRenderer(window) == playback.renderer,
          "display shortcut and subsequent frames keep the actual Application window and renderer");
    if (!window) return;
    int x = 0, y = 0, width = 0, height = 0;
    SDL_GetWindowPosition(window, &x, &y);
    check(x == 100 && y == 120, "display shortcut preserves the user's window position");
    if (!playback.renderer) return;
    check(SDL_GetRendererOutputSize(playback.renderer, &width, &height) == 0 && width > 0 && height > 0,
          "actual playback exposes a valid retained drawable after rotation");
    if (width <= 0 || height <= 0) return;
    std::vector<uint32_t> pixels(static_cast<std::size_t>(width) * height);
    check(SDL_RenderReadPixels(playback.renderer, nullptr, SDL_PIXELFORMAT_ARGB8888,
                              pixels.data(), width * 4) == 0,
          "actual Application's retained display can be read without drawing another frame");
    // 独立的色块顺序，不调用 Presenter/ViewGeom 的旋转或坐标公式。
    constexpr std::array<std::array<int, 4>, 4> corner_order{{
        {{0, 1, 2, 3}}, {{2, 0, 3, 1}}, {{3, 2, 1, 0}}, {{1, 3, 0, 2}}
    }};
    const int degrees = (playback.base_degrees + (index <= 6 ? 90 : 0)) % 360;
    for (int corner = 0; corner < 4; ++corner) {
        const int px = (corner % 2 ? 3 : 1) * width / 4;
        const int py = (corner / 2 ? 3 : 1) * height / 4;
        const int source_corner = corner_order[degrees / 90][corner] ^ (playback.flip ? 1 : 0);
        check(pixels[static_cast<std::size_t>(py) * width + px] == quadrant_colors[source_corner],
              "Application retains the shortcut direction, CLI flip and source geometry in real pixels");
    }
}

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
                if (playback.rotation) queue_rotation(SDLK_RIGHT, SDL_SCANCODE_RIGHT);
            }
        } else if (playback.rotation && index <= 8) {
            check_playback_rotation(index);
            if (index == 6) queue_rotation(SDLK_LEFT, SDL_SCANCODE_LEFT);
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
        if (index > (playback.rotation ? 8 : 4)) return false;
        const bool landscape = playback.rotation ? index >= 5 : !playback.padding_only && index == 2;
        out.width = playback.padding_only && index == 2 ? 80 : landscape ? 96 : 64;
        out.height = playback.padding_only && index == 2 ? 112 : landscape ? 64 : 96;
        out.row_pitch = out.width * 4;
        out.pixels.assign(static_cast<std::size_t>(out.row_pitch) * out.height, 0x80);
        if (playback.rotation) {
            for (uint32_t y = 0; y < out.height; ++y) {
                for (uint32_t x = 0; x < out.width; ++x) {
                    const auto color = quadrant_colors[(y >= out.height / 2 ? 2 : 0) +
                                                       (x >= out.width / 2 ? 1 : 0)];
                    std::memcpy(out.pixels.data() + static_cast<std::size_t>(y) * out.row_pitch + x * 4,
                                &color, sizeof color);
                }
            }
        }
        return true;
    }
    const char *backend_name() const override { return "ControlledPlaybackLifecycle"; }
};

void write_playback_fixture(const std::filesystem::path &path, unsigned frames) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    const auto nal = [&](std::initializer_list<unsigned char> bytes) {
        const unsigned char prefix[]{0, 0, 0, 1};
        file.write(reinterpret_cast<const char *>(prefix), sizeof prefix);
        for (const auto byte : bytes) file.put(static_cast<char>(byte));
    };
    nal({0x40, 1, 0x55}); nal({0x42, 1, 0x55}); nal({0x44, 1, 0x55});
    for (unsigned char i = 1; i <= frames; ++i) nal({0x26, 1, 0x80, i, 0x55});
    file.close();
}

void playback_window_lifecycle(const std::filesystem::path &path, bool padding_only) {
    write_playback_fixture(path, 4);
    playback.active = true;
    playback.padding_only = padding_only;
    playback.rotation = false;
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

void playback_rotation_lifecycle(const std::filesystem::path &path) {
    for (int base : {0, 90, 180, 270}) {
        write_playback_fixture(path, 8);
        playback.active = true;
        playback.padding_only = false;
        playback.rotation = true;
        playback.base_degrees = base;
        playback.flip = base == 90 || base == 270;
        playback.decoded = 0;
        playback.window_id.store(0);
        playback.window = nullptr;
        playback.renderer = nullptr;
        std::vector<std::string> args{
            "scrctl", "--play", path.string(), "--no-audio", "--no-control",
            "--render-driver=software", "--window-width=80", "--window-height=120",
            "--exit-after=8", "--display-orientation=" +
                (base == 0 ? std::string("auto") :
                 (playback.flip ? "flip" : "") + std::to_string(base))
        };
        check(run(std::move(args)) == 0 && playback.decoded == 8,
              "actual Application counts only eight decoded frames, including two static shortcut redraws");
        check(SDL_WasInit(0) == 0 && !playback.watching,
              "display shortcut playback releases its window, observer and SDL runtime");
        playback.active = false;
        playback.rotation = false;
    }
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
    audio_only_stats();
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
        scrctl::app::LiveSource::Options options;
        options.serial = "must-not-connect";
        options.wifi = "must-not-resolve";
        options.watch_display = false;
        options.audio_buffer_ms = 200;
        options.should_cancel = [&runtime] { return runtime.stop_requested(); };
        check(!source.start(options, err) &&
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
    check(run({"scrctl", "--no-video", "--no-window", "--serial", "must-not-connect"}) == 1,
          "audio-only output failure is fatal before opening a device or routing its sound");
    check(SDL_WasInit(0) == 0, "rejected audio-only startup cleans the SDL runtime");
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
            scrctl::app::LiveSource::Options options;
            options.serial = "must-not-connect";
            options.wifi = "must-not-resolve";
            options.record_path = record;
            options.hw_decode = hardware;
            options.watch_display = false;
            options.want_audio = false;
            options.audio_buffer_ms = 200;
            options.video_source = kind;
            options.test_degrade = degrade;
            options.decode_video = false;
            return !source.start(options, err) &&
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
        scrctl::app::LiveSource::Options options;
        options.serial = "must-not-connect";
        options.wifi = "must-not-resolve";
        options.watch_display = false;
        options.audio_buffer_ms = 200;
        options.decode_audio = false;
        check(!source.start(options, err) &&
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

    {
        scrctl::app::LiveSource source;
        scrctl::app::LiveSource::Options options;
        options.serial = "must-not-connect";
        options.wifi = "must-not-resolve";
        options.want_video = false;
        options.record_path = "must-not-create.hevc";
        std::string err;
        check(!source.start(options, err) && !source.has_video() &&
                  !source.video_decoding_enabled() && !source.has_audio() &&
                  !err.empty() && !std::filesystem::exists(options.record_path),
              "audio-only raw HEVC is rejected before connection or file creation");
        options.record_path = "must-not-create.mkv";
        options.want_audio = false;
        scrctl::app::LiveSource no_tracks;
        check(!no_tracks.start(options, err) && !no_tracks.has_video() && !no_tracks.has_audio() &&
                  err.find("requires audio capture") != std::string::npos &&
                  !std::filesystem::exists(options.record_path),
              "audio-only container without audio fails before connecting or creating a file");
        options.want_audio = true;
        options.record_orientation = 90;
        scrctl::app::LiveSource rotated_audio;
        check(!rotated_audio.start(options, err) &&
                  err.find("requires a video track") != std::string::npos &&
                  !std::filesystem::exists(options.record_path),
              "audio-only recording rejects video rotation before connection or file creation");
        options.record_orientation = 0;
        options.record_path.clear();
        options.want_audio = false;
        options.should_cancel = [] { return true; };
        scrctl::app::LiveSource cancelled;
        check(!cancelled.start(options, err) && !err.empty() &&
                  !cancelled.has_video() && !cancelled.video_decoding_enabled() && !cancelled.has_audio(),
              "control-only startup obeys cancellation before opening the device");
        scrctl::Frame frame;
        const auto begin = std::chrono::steady_clock::now();
        check(!cancelled.next(frame, 50) && !frame &&
                  std::chrono::steady_clock::now() - begin >= std::chrono::milliseconds(40),
              "no-video event polling has a bounded wait and does not invent frames");
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
    playback_rotation_lifecycle(path);
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
