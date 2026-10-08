#include "i18n/Translation.h"
#include "app/Application.h"

#include <SDL.h>
#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>

#include "app/Cli.h"
#include "app/Commands.h"
#include "app/FileSource.h"
#include "app/LiveSource.h"
#include "app/Presenter.h"
#include "app/SdlRuntime.h"
#include "media/StreamSession.h"

namespace scrctl::app {

namespace {
/// 按当前帧确定裁剪区域。视频优先使用设备报告的可见区尺寸；无法获取时，
/// 使用 media::display_crop 的机型表，去掉 HEVC 对齐填充。截图本身已经是
/// 可见画面，默认保留完整 PNG；源像素方向用于将触摸坐标还原到设备面板。
Crop resolve_crop(const Options &o, const scrctl::Frame &f, FrameGeometry geometry,
                  bool report_geometry) {
    int &display_w = geometry.display_w, &display_h = geometry.display_h;
    const bool from_device = display_w > 0 && display_h > 0;
    if (!from_device && !geometry.screenshot) {
        const auto fallback =
            scrctl::media::display_crop(static_cast<int>(f.width), static_cast<int>(f.height));
        display_w = fallback.w;
        display_h = fallback.h;
    }
    if (report_geometry && !from_device && !geometry.screenshot && !o.crop_set &&
        (static_cast<int>(f.width) != display_w || static_cast<int>(f.height) != display_h)) {
        std::printf(SCRCTL_TR("Visible area %ux%u -> %dx%d (device did not report dimensions; using model table)\n"), f.width,
                    f.height, display_w, display_h);
    }
    // 裁剪范围与坐标变换集中在 make_frame_crop，可用离线用例覆盖。
    return scrctl::app::make_frame_crop(o.crop_set, o.crop_x, o.crop_y, o.crop_w, o.crop_h,
                                        static_cast<int>(f.width), static_cast<int>(f.height),
                                        geometry);
}

} // namespace

int run(int argc, char **argv) {
    // 崩溃时块缓冲的 stdout 会整段丢失，导致"无输出"无法定位。诊断工具
    // 的输出量很小，直接无缓冲。
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Options o;
    const auto parsed = parse_args(argc, argv, o);
    if (parsed != ParseResult::Run) {
        return parsed == ParseResult::ExitSuccess ? 0 : 2;
    }

    if (const auto exit_code = run_standalone_command(o)) {
        return *exit_code;
    }

    // 逆序析构：窗口 -> 媒体源（包含声卡）-> SDL。所有提前返回也遵守此顺序。
    SdlRuntime runtime;
    std::unique_ptr<FrameSource> source;
    LiveSource *live = nullptr;
    if (!o.path.empty()) {
        source = std::make_unique<FileSource>(o.path);
    } else {
        auto made = std::make_unique<LiveSource>();
        std::string err;
        if (!made->start(o.serial, o.wifi, o.record, o.hw_decode, !o.no_window,
                         !o.no_audio, o.audio_buffer_ms, o.video_source, o.test_degrade, err, o.wifi_port)) {
            std::fprintf(stderr, SCRCTL_TR("Failed to start video: %s\n"), err.c_str());
            // 设备通话期间可能拒绝媒体流，错误码为 9022。曾观察到此时会话列表为空，
            // 截图服务仍可用；提示用户结束通话后重试。
            if (err.find("9022") != std::string::npos) {
                std::fprintf(
                    stderr,
                    SCRCTL_TR("Device is in a call. End the call and try again.\n"));
            }
            return 1;
        }
        live = made.get();
        if (o.debug_net) {
            if (auto *st = live->device().stack()) {
                st->set_net_debug(true);
            }
        }
        source = std::move(made);
    }
    if (live == nullptr && !o.start_app.empty()) {
        std::fprintf(stderr,
                     SCRCTL_TR("File playback cannot launch a device app; ignoring --start-app\n"));
    }
    if (live != nullptr && !o.start_app.empty()) {
        const int exit_code = launch_app(live->device(), o.start_app);
        if (exit_code != 0) {
            return exit_code;
        }
    }

    const bool control_enabled = live != nullptr && !o.no_control;

    if (!o.render_driver.empty()) {
        // SDL 渲染驱动提示必须在 SDL_CreateRenderer 前设置，否则不生效。
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, o.render_driver.c_str());
    }
    // 仅在需要窗口或禁止屏保时初始化视频子系统。无窗口模式应能在没有显示
    // 设备的环境下运行；若无条件初始化视频，SDL_Init 失败会阻止这条路径。
    Uint32 sdl_flags = SDL_INIT_TIMER;
    if (!o.no_window || o.disable_screensaver) {
        // SDL_DisableScreenSaver 依赖视频子系统；未初始化时无法关闭屏保。
        sdl_flags |= SDL_INIT_VIDEO;
    }
    if (!runtime.initialize(sdl_flags)) {
        std::fprintf(stderr, SCRCTL_TR("SDL initialization failed: %s\n"), SDL_GetError());
        return 1;
    }
    // 音频子系统单独初始化，失败后仍可继续镜像。无声卡或音频后端不可用时，
    // 不应使视频初始化失败。
    if (live != nullptr && !o.no_audio && !o.no_audio_playback &&
        SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        std::string aerr;
        if (!live->start_playback(aerr)) {
            std::fprintf(stderr, SCRCTL_TR("Failed to open audio output: %s (receiving and decoding continue)\n"), aerr.c_str());
        }
    } else if (live != nullptr && live->has_audio() && o.no_audio_playback) {
        std::printf(SCRCTL_TR("Local audio playback disabled; receiving and decoding continue\n"));
    }
    if (o.disable_screensaver) {
        SDL_DisableScreenSaver();
    }

    // 无窗口触摸测试注入一条直线后退出。拖动通常能产生可截图验证的位移，
    // 单次点击则不一定改变画面。
    if (live != nullptr && !o.test_touch.empty()) {
        const double x0 = o.test_touch[0], y0 = o.test_touch[1];
        const double x1 = o.test_touch[2], y1 = o.test_touch[3];
        std::string cerr;
        bool ok = true;
        for (int i = 0; i <= 20 && ok; ++i) {
            const double t = i / 20.0;
            ok = live->control(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, i < 20, cerr);
            if (i < 20) {
                SDL_Delay(12);
            }
        }
        std::printf("--test-touch (%.3f,%.3f)->(%.3f,%.3f): %s%s\n", x0, y0, x1, y1,
                    ok ? SCRCTL_TR("injected") : SCRCTL_TR("failed"), ok ? "" : cerr.c_str());
        return ok ? 0 : 1;
    }

    // 音量 HUD 显示时间短，另开截图会话可能来不及。此处完成按键注入，
    // 通过当前镜像配合 --verify N 回读画面检查效果。
    if (live != nullptr && !o.test_button.empty()) {
        std::string berr;
        if (live->button(scrctl::hid::button::kUsagePageConsumer, o.test_button_code, berr)) {
            std::printf(SCRCTL_TR("--test-button %s: pressed\n"), o.test_button.c_str());
        } else {
            std::fprintf(stderr, SCRCTL_TR("--test-button %s failed: %s\n"), o.test_button.c_str(),
                         berr.c_str());
            return 1;
        }
    }

    if (live != nullptr && !o.test_type.empty()) {
        std::string terr;
        if (live->type_text(o.test_type, 40, terr)) {
            std::printf(SCRCTL_TR("--test-type %s: injected\n"), o.test_type.c_str());
        } else {
            std::fprintf(stderr, SCRCTL_TR("--test-type failed: %s\n"), terr.c_str());
            return 1;
        }
    }

    int rendered = 0;
    std::unique_ptr<Presenter> presenter;
    // 记录当前窗口朝向；-1 表示尚未创建窗口，首帧需要创建 Presenter。
    int applied_degrees = -1;
    // 首次创建窗口不输出旋转提示。
    bool first_window = true;
    bool window_fullscreen = o.fullscreen;
    bool input_geometry_warned = false;
    const Uint64 start = SDL_GetTicks64();
    Uint64 last_stats_at = SDL_GetTicks64();
    bool quit = false;
    int exit_code = 0;

    /// 将窗口的按下、移动和抬起事件转换为设备触摸。
    /// 注入失败后停止重试，仅输出一次错误，避免每次鼠标移动重复报错。
    std::string control_err;
    bool control_warned = false;
    auto on_touch = [&](double x, double y, bool down) {
        if (!control_enabled || live == nullptr) {
            return;
        }
        if (!live->control(x, y, down, control_err) && !control_warned) {
            control_warned = true;
            std::fprintf(stderr, SCRCTL_TR("Input injection failed (further attempts disabled): %s\n"), control_err.c_str());
        }
    };

    // 复用 Frame 的像素缓冲，避免每帧重新分配并提交约 11 MiB 内存。
    // 取帧仍需复制像素，额外分配会增加渲染延迟。
    scrctl::Frame f;
    int last_rendered = 0;
    while (!quit) {
        if (o.time_limit > 0 &&
            SDL_GetTicks64() - start >= static_cast<Uint64>(o.time_limit) * 1000) {
            std::printf(SCRCTL_TR("Reached --time-limit %d seconds\n"), o.time_limit);
            break;
        }
        if (runtime.stop_requested()) {
            std::printf(SCRCTL_TR("Received exit signal; closing device session\n"));
            break;
        }
        // 统计在取帧之前按时间输出，断流时也能显示 0 fps。
        // 分别报告本次统计窗口帧率和全程平均值，避免恢复后的平均值掩盖当前帧率。
        // 按秒计时，而非按帧数计时，以便比较不同帧率或停顿期间的表现。
        if (o.stats && SDL_GetTicks64() - last_stats_at >= 1000) {
            const Uint64 at = SDL_GetTicks64();
            const double win = std::max(0.001, static_cast<double>(at - last_stats_at) / 1000.0);
            const int got = rendered - last_rendered;
            std::printf(SCRCTL_TR("  Render: %d frames total, %d this interval / %.1f fps, average %.1f fps\n"), rendered, got,
                        got / win,
                        rendered / std::max(0.001, static_cast<double>(at - start) / 1000.0));
            source->print_stats();
            last_stats_at = at;
            last_rendered = rendered;
        }

        // 最多等待 50 ms，之后处理窗口事件，限制关闭和移动操作的响应延迟。
        if (!source->next(f, 50)) {
            if (source->finished()) {
                exit_code = source->failed() ? 1 : 0;
                std::fprintf(exit_code == 0 ? stdout : stderr, "%s\n", source->end_reason().c_str());
                break;
            }
            // 等待下一帧时也需要处理窗口事件，避免窗口失去响应。
            if (presenter != nullptr) {
                quit = presenter->pump(on_touch);
            }
            continue;
        }

        if (o.no_window) {
            // 无窗口模式只消费帧，用于脚本和自动化。
            ++rendered;
            if (o.exit_after > 0 && rendered >= o.exit_after) {
                std::printf(SCRCTL_TR("Reached --exit-after %d\n"), o.exit_after);
                break;
            }
            continue;
        }

        // 朝向改变时重建 Presenter。SDL dummy 驱动下，单独改变窗口和 logical
        // size 不会同步更新绘制面，可能使画面缩到一角；重建可统一窗口与渲染尺寸。
        // 此操作只发生在旋转时，可能短暂闪烁。
        const auto geometry = source->frame_geometry();
        const int degrees = o.orientation >= 0 ? o.orientation :
            (geometry.screenshot ? 0 : geometry.panel_degrees.value_or(0));
        const Crop crop = resolve_crop(o, f, geometry, degrees != applied_degrees);
        if (!crop.input_valid && control_enabled && !input_geometry_warned) {
            std::fprintf(stderr, SCRCTL_TR(
                "Mouse input is unavailable: screenshot orientation or display dimensions are unknown or inconsistent\n"));
        }
        input_geometry_warned = !crop.input_valid;
        if (degrees != applied_degrees) {
            applied_degrees = degrees;
            if (presenter != nullptr) {
                window_fullscreen = presenter->is_fullscreen();
                presenter->release_touch(on_touch);
            }
            presenter.reset();
            presenter = std::make_unique<Presenter>();
            presenter->set_debug_input(o.debug_input);
            WindowSpec spec;
            spec.title = o.title;
            spec.want_w = o.win_w;
            spec.want_h = o.win_h;
            spec.x = o.win_x.value_or(SDL_WINDOWPOS_CENTERED);
            spec.y = o.win_y.value_or(SDL_WINDOWPOS_CENTERED);
            spec.always_on_top = o.always_on_top;
            spec.borderless = o.borderless;
            spec.fullscreen = window_fullscreen;
            spec.want_readback = o.verify_at > 0;
            spec.shortcut_mods = o.shortcut_mods;
            presenter->set_background(o.bg[0], o.bg[1], o.bg[2]);
            if (!presenter->open(static_cast<int>(f.width), static_cast<int>(f.height),
                                 crop, degrees, o.scale, o.scale_given,
                                 spec)) {
                return 1;
            }
            if (first_window) {
                first_window = false;
            } else {
                std::printf(SCRCTL_TR("Render rotation changed to %d degrees clockwise; window recreated\n"), degrees);
            }
        }

        const bool do_verify = o.verify_at > 0 && rendered + 1 == o.verify_at;
        if (!presenter->draw(f, crop, do_verify ? o.verify_path.c_str() : nullptr)) {
            presenter->release_touch(on_touch);
            return 1;
        }
        ++rendered;

        // 文件回放按标称帧率计时；实时流按设备帧到达的节奏显示。
        if (source->paces_itself()) {
            const Uint64 want_ms = static_cast<Uint64>(rendered) * 1000 / 60;
            const Uint64 now = SDL_GetTicks64() - start;
            if (want_ms > now) {
                SDL_Delay(static_cast<Uint32>(want_ms - now));
            }
        }

        if (o.exit_after > 0 && rendered >= o.exit_after) {
            std::printf(SCRCTL_TR("Reached --exit-after %d\n"), o.exit_after);
            break;
        }
        quit = presenter->pump(on_touch);
    }

    if (presenter != nullptr) {
        presenter->release_touch(on_touch);
    }
    if (exit_code != 0) {
        return exit_code;
    }
    if (o.verify_at > 0 && rendered < o.verify_at) {
        std::fprintf(stderr,
                     SCRCTL_TR("Window readback was not reached (requested frame %d, rendered %d)\n"),
                     o.verify_at, rendered);
        return 1;
    }
    std::printf(SCRCTL_TR("Finished: processed %d frames\n"), rendered);
    return 0;
}

} // namespace scrctl::app
