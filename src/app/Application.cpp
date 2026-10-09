#include "i18n/Translation.h"
#include "app/Application.h"

#include <SDL.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "app/Cli.h"
#include "app/Commands.h"
#include "app/ClipboardPasteJob.h"
#include "app/FileSource.h"
#include "app/LiveSource.h"
#include "app/Presenter.h"
#include "app/RecordFormat.h"
#include "media/RecordingVideoConfig.h"
#include "app/SdlRuntime.h"
#include "media/StreamSession.h"
#include "remote/Pasteboard.h"

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
    if (!o.render_driver.empty()) {
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, o.render_driver.c_str());
    }
    // 无窗口模式不要求显示设备；禁止屏保仍需要视频子系统。
    Uint32 sdl_flags = SDL_INIT_TIMER;
    if (!o.no_window || o.disable_screensaver) {
        sdl_flags |= SDL_INIT_VIDEO;
    }
    if (!runtime.initialize(sdl_flags)) {
        std::fprintf(stderr, SCRCTL_TR("SDL initialization failed: %s\n"), SDL_GetError());
        return 1;
    }
    const auto exit_requested = [&] {
        if (!runtime.stop_requested()) {
            return false;
        }
        std::printf(SCRCTL_TR("Received exit signal; closing device session\n"));
        return true;
    };
    if (exit_requested()) {
        return 0;
    }
    // 本机播放后端已不可用时，在音频起流前停住，避免压制手机却没有电脑声音。
    // 关闭本机播放时，CLI 只在有容器音轨录制消费者时保留音频采集；
    // 这种录制不需要本机音频后端，仍采用用户所选路由。
    std::string audio_init_error;
    const bool want_audio = runtime.prepare_audio(
        o.path.empty() && !o.no_audio && o.video_source != "screenshot",
        !o.no_audio_playback, audio_init_error);
    if (!audio_init_error.empty()) {
        if (o.no_video) {
            std::fprintf(stderr, SCRCTL_TR("Failed to initialize audio output: %s\n"),
                         audio_init_error.c_str());
            return 1;
        }
        std::fprintf(stderr, SCRCTL_TR("Failed to initialize audio output: %s (audio disabled; video continues)\n"),
                     audio_init_error.c_str());
        if (record_container_format(o.record) && !o.no_audio) {
            std::fprintf(stderr, "%s\n", SCRCTL_TR(
                "Audio recording requires an audio stream. Use --no-audio-playback to record "
                "without a sound device, or --no-audio to record video only."));
            return 1;
        }
    }
    if (exit_requested()) {
        return 0;
    }
    std::unique_ptr<FrameSource> source;
    // 作业借用 Device；必须先取消并等待，再销毁 source 中的设备与隧道。
    std::unique_ptr<ClipboardPasteJob> paste_job;
    LiveSource *live = nullptr;
    // 返回值必须在 FILE 刷新、关闭之后确定。析构仍负责兜底，但无法修改已经
    // 求值的 return 0；取消启动时 made 也可能已经拥有视频 worker 和录制文件。
    const auto finish_source = [](LiveSource *active, int code) {
        std::string err;
        if (active != nullptr && !active->finish_recording(err) && code == 0) {
            return 1;
        }
        return code;
    };
    if (!o.path.empty()) {
        source = std::make_unique<FileSource>(o.path, o.hw_decode);
    } else {
        auto made = std::make_unique<LiveSource>();
        std::string err;
        // 无窗口录制直接消费编码 AU。依赖像素的旧选项仍走解码路径：
        // --exit-after 继续按已交付帧计数，硬件后端和强制降级也不被忽略。
        const bool needs_pixels = !o.no_video_playback || o.record.empty() ||
            o.video_source == "screenshot" || o.exit_after > 0 || o.hw_decode ||
            !o.test_degrade.empty();
        // 未编入公开 IDR 检查能力时沿用旧解码路径，保留平台后端的裸流录制。
        // LiveSource 显式请求不解码时仍严格检查能力，不接受未经检查的就绪。
        std::string capture_check_error;
        LiveSource::Options live_options;
        live_options.serial = o.serial;
        live_options.wifi = o.wifi;
        live_options.wifi_port = o.wifi_port;
        live_options.record_path = o.record;
        live_options.hw_decode = o.hw_decode;
        live_options.watch_display = !o.no_video_playback;
        live_options.want_video = !o.no_video;
        live_options.decode_video = !o.no_video && (needs_pixels ||
            !scrctl::media::recording_idr_checks_available(capture_check_error));
        live_options.want_audio = want_audio;
        live_options.decode_audio = !o.no_audio_playback;
        live_options.audio_buffer_ms = o.audio_buffer_ms;
        live_options.audio_dup = o.audio_dup;
        live_options.video_source = o.video_source;
        live_options.test_degrade = o.test_degrade;
        live_options.record_orientation = o.record_orientation;
        live_options.should_cancel = [&runtime] { return runtime.stop_requested(); };
        if (!made->start(live_options, err)) {
            if (exit_requested()) {
                return finish_source(made.get(), 0);
            }
            std::fprintf(stderr, SCRCTL_TR("Failed to start device session: %s\n"), err.c_str());
            // 设备通话期间可能拒绝媒体流，错误码为 9022。曾观察到此时会话列表为空，
            // 截图服务仍可用；提示用户结束通话后重试。
            if (err.find("9022") != std::string::npos) {
                std::fprintf(
                    stderr,
                    SCRCTL_TR("Device is in a call. End the call and try again.\n"));
            }
            return finish_source(made.get(), 1);
        }
        live = made.get();
        if (o.debug_net) {
            if (auto *st = live->device().stack()) {
                st->set_net_debug(true);
            }
        }
        source = std::move(made);
    }
    const auto finish_exit = [&](int code) {
        if (paste_job) paste_job->shutdown();
        return finish_source(live, code);
    };
    if (exit_requested()) {
        return finish_exit(0);
    }
    if (live == nullptr && !o.start_app.empty()) {
        std::fprintf(stderr,
                     SCRCTL_TR("File playback cannot launch a device app; ignoring --start-app\n"));
    }
    if (live != nullptr && !o.start_app.empty()) {
        const int exit_code = launch_app(live->device(), o.start_app);
        if (exit_code != 0) {
            return finish_exit(exit_code);
        }
    }
    if (exit_requested()) {
        return finish_exit(0);
    }

    const bool control_enabled = live != nullptr && !o.no_control;
    if (control_enabled && !o.no_window) {
        paste_job = std::make_unique<ClipboardPasteJob>(
            [live](const std::string &text, std::stop_token cancel) {
                ClipboardPasteJob::OperationResult result;
                std::string readback;
                if (scrctl::remote::Pasteboard::set_text(live->device(), text, result.error,
                                                        false, cancel, 5000) &&
                    !cancel.stop_requested() &&
                    scrctl::remote::Pasteboard::get_text(live->device(), readback, result.error,
                                                        false, cancel, 5000)) {
                    result.success = readback == text;
                    if (!result.success)
                        result.error = SCRCTL_TR("Device clipboard text did not match; paste skipped");
                }
                result.cancelled = cancel.stop_requested();
                return result;
            });
    }

    if (live != nullptr && live->has_audio() && !o.no_audio_playback) {
        std::string aerr;
        if (!live->start_playback(aerr)) {
            if (!live->has_video()) {
                std::fprintf(stderr, SCRCTL_TR("Failed to open audio output: %s\n"), aerr.c_str());
                return finish_exit(1);
            }
            if (record_container_format(o.record)) {
                std::fprintf(stderr, SCRCTL_TR(
                    "Failed to open audio output: %s (recording and audio reception continue)\n"),
                    aerr.c_str());
            } else {
                std::fprintf(stderr, SCRCTL_TR(
                    "Failed to open audio output: %s (audio disabled; video continues). The phone's "
                    "audio route may remain active until session expiry (about 20 seconds); resume "
                    "its player if needed.\n"),
                    aerr.c_str());
            }
        }
    } else if (live != nullptr && live->has_audio() && o.no_audio_playback) {
        std::printf(SCRCTL_TR("Local audio playback disabled; recording original AAC without PCM decoding\n"));
    }
    if (exit_requested()) {
        return finish_exit(0);
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
        return finish_exit(ok ? 0 : 1);
    }

    // 音量 HUD 显示时间短，另开截图会话可能来不及。此处完成按键注入，
    // 通过当前镜像配合 --verify N 回读画面检查效果。
    if (exit_requested()) {
        return finish_exit(0);
    }
    if (live != nullptr && !o.test_button.empty()) {
        std::string berr;
        if (live->button(scrctl::hid::button::kUsagePageConsumer, o.test_button_code, berr)) {
            std::printf(SCRCTL_TR("--test-button %s: pressed\n"), o.test_button.c_str());
        } else {
            std::fprintf(stderr, SCRCTL_TR("--test-button %s failed: %s\n"), o.test_button.c_str(),
                         berr.c_str());
            return finish_exit(1);
        }
    }

    if (exit_requested()) {
        return finish_exit(0);
    }
    if (live != nullptr && !o.test_type.empty()) {
        std::string terr;
        if (live->type_text(o.test_type, 40, terr)) {
            std::printf(SCRCTL_TR("--test-type %s: injected\n"), o.test_type.c_str());
        } else {
            std::fprintf(stderr, SCRCTL_TR("--test-type failed: %s\n"), terr.c_str());
            return finish_exit(1);
        }
    }
    // 只有显式启动操作的无媒体/无窗口命令，操作完成后没有事件消费者。
    if (live != nullptr && !live->has_video() && !live->has_audio() && o.no_window) {
        return finish_exit(0);
    }

    int rendered = 0;
    std::unique_ptr<Presenter> presenter;
    // 只在首帧创建窗口；之后复用窗口和渲染器，按有效内容尺寸更新布局。
    int applied_degrees = -1;
    int applied_view_w = 0, applied_view_h = 0;
    bool input_geometry_warned = false;
    const Uint64 start = SDL_GetTicks64();
    Uint64 last_stats_at = SDL_GetTicks64();
    bool quit = false;
    int exit_code = 0;
    uint64_t input_epoch = 0, seen_generation = 0, next_paste_id = 0;
    std::optional<std::chrono::steady_clock::time_point> paste_started;
    const auto cancel_paste = [&] {
        ++input_epoch;
        paste_started.reset();
        if (paste_job) paste_job->cancel();
    };
    const auto synchronize_input = [&] {
        if (presenter && seen_generation != presenter->input_generation()) {
            seen_generation = presenter->input_generation();
            cancel_paste();
        }
    };

    /// 触摸和键盘共享控制门控及首次失败，停止发送后只输出一次错误。
    std::string control_err;
    bool control_warned = false;
    const auto input_failed = [&] {
        control_warned = true;
        cancel_paste();
        std::fprintf(stderr, SCRCTL_TR("Input injection failed (further attempts disabled): %s\n"),
                     control_err.c_str());
    };
    auto on_touch = [&](double x, double y, bool down) {
        if (!control_enabled || live == nullptr || control_warned || o.no_video_playback) {
            return;
        }
        if (!live->control(x, y, down, control_err)) {
            input_failed();
        }
    };
    const auto on_keyboard = [&](const std::vector<uint16_t> &usages) {
        if (!control_enabled || live == nullptr || control_warned) return;
        if (!live->keyboard_state(usages, control_err)) input_failed();
    };
    const auto on_paste = [&] {
        synchronize_input();
        if (!paste_job || control_warned || !presenter || runtime.stop_requested()) return;
        if (!presenter->ready_for_paste()) {
            std::fprintf(stderr, "%s\n", SCRCTL_TR("Release the held keys or mouse button before pasting"));
            return;
        }
        if (paste_job->busy()) {
            std::fprintf(stderr, "%s\n", SCRCTL_TR("Clipboard paste is already in progress"));
            return;
        }
        // 只有明确 MOD+V 才读取本机剪贴板；不在后台同步或轮询用户文本。
        if (!SDL_HasClipboardText()) return;
        const std::unique_ptr<char, decltype(&SDL_free)> text(SDL_GetClipboardText(), SDL_free);
        if (!text) {
            std::fprintf(stderr, SCRCTL_TR("Failed to read computer clipboard: %s\n"), SDL_GetError());
            return;
        }
        const auto size = std::strlen(text.get());
        if (size == 0) return;
        if (size > ClipboardPasteJob::kMaxTextBytes) {
            std::fprintf(stderr, "%s\n", SCRCTL_TR("Clipboard text exceeds the 1 MiB paste limit"));
            return;
        }
        const auto started = paste_job->start(++next_paste_id, input_epoch,
                                              std::string(text.get(), size));
        if (started == ClipboardPasteJob::StartStatus::Started) {
            paste_started = std::chrono::steady_clock::now();
        } else {
            std::fprintf(stderr, "%s\n", SCRCTL_TR("Could not start clipboard paste"));
        }
    };
    const auto service_paste = [&] {
        synchronize_input();
        if (!paste_job) return;
        if (quit || control_warned || runtime.stop_requested()) cancel_paste();
        if (paste_started && std::chrono::steady_clock::now() - *paste_started >=
                                 std::chrono::seconds(5)) {
            cancel_paste();
            std::fprintf(stderr, "%s\n", SCRCTL_TR("Clipboard paste timed out"));
        }
        const auto result = paste_job->poll();
        if (!result) return;
        paste_started.reset();
        if (result->cancelled || result->id != next_paste_id || result->generation != input_epoch || quit || control_warned ||
            runtime.stop_requested() || !presenter || !presenter->ready_for_paste()) return;
        if (!result->success) {
            std::fprintf(stderr, SCRCTL_TR("Clipboard paste failed: %s\n"),
                         result->error.empty() ? SCRCTL_TR("No diagnostic available") : result->error.c_str());
            return;
        }
        // 与普通键盘串行发送，先修饰键再主键。每一步失败都经过同一个输入门控。
        // SET/PULL 一致和本地发送成功不代表目标控件一定接受了文字。
        constexpr uint16_t paste_key = scrctl::hid::key::kA + ('v' - 'a');
        on_keyboard({scrctl::hid::key::kGuiLeft});
        on_keyboard({scrctl::hid::key::kGuiLeft, paste_key});
        on_keyboard({});
    };

    const auto window_spec = [&] {
        WindowSpec spec;
        spec.title = o.title;
        spec.want_w = o.win_w;
        spec.want_h = o.win_h;
        spec.x = o.win_x.value_or(SDL_WINDOWPOS_CENTERED);
        spec.y = o.win_y.value_or(SDL_WINDOWPOS_CENTERED);
        spec.always_on_top = o.always_on_top;
        spec.borderless = o.borderless;
        spec.fullscreen = o.fullscreen;
        spec.want_readback = o.verify_at > 0;
        spec.shortcut_mods = o.shortcut_mods;
        spec.horizontal_flip = o.display_flip;
        return spec;
    };
    if (o.no_video_playback && !o.no_window) {
        presenter = std::make_unique<Presenter>();
        presenter->set_debug_input(o.debug_input);
        presenter->set_background(o.bg[0], o.bg[1], o.bg[2]);
        if (!presenter->open_background(window_spec())) return finish_exit(1);
        seen_generation = presenter->input_generation();
    }

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
            if (live == nullptr || live->video_decoding_enabled()) {
                const int got = rendered - last_rendered;
                std::printf(SCRCTL_TR("  Render: %d frames total, %d this interval / %.1f fps, average %.1f fps\n"), rendered, got,
                            got / win,
                            rendered / std::max(0.001, static_cast<double>(at - start) / 1000.0));
            }
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
                quit = presenter->pump(on_touch, on_keyboard, on_paste);
            }
            service_paste();
            continue;
        }

        if (o.no_video_playback) {
            // 旧诊断选项仍可消费真实帧；背景窗口只处理键盘和本地快捷键。
            ++rendered;
            if (o.exit_after > 0 && rendered >= o.exit_after) {
                std::printf(SCRCTL_TR("Reached --exit-after %d\n"), o.exit_after);
                break;
            }
            if (presenter) quit = presenter->pump(on_touch, on_keyboard, on_paste);
            service_paste();
            continue;
        }

        // 有效内容包括裁剪后的宽高和窗口旋转；编码填充变化只更新纹理。
        // 启动窗口参数只在首次 open 时生效，转屏继续沿用用户实际窗口布局。
        const auto geometry = source->frame_geometry();
        const int degrees = o.orientation >= 0 ? o.orientation :
            (geometry.screenshot ? 0 : geometry.panel_degrees.value_or(0));
        const Crop crop = resolve_crop(o, f, geometry, degrees != applied_degrees);
        if (!crop.input_valid && control_enabled && !input_geometry_warned) {
            std::fprintf(stderr, SCRCTL_TR(
                "Mouse input is unavailable: screenshot orientation or display dimensions are unknown or inconsistent\n"));
        }
        input_geometry_warned = !crop.input_valid;
        int view_w = 0, view_h = 0;
        viewport_size(crop, degrees, view_w, view_h);
        const bool content_changed = degrees != applied_degrees ||
                                     view_w != applied_view_w || view_h != applied_view_h;
        if (content_changed) {
            cancel_paste();
        }
        if (presenter == nullptr) {
            presenter = std::make_unique<Presenter>();
            presenter->set_debug_input(o.debug_input);
            presenter->set_background(o.bg[0], o.bg[1], o.bg[2]);
            if (!presenter->open(static_cast<int>(f.width), static_cast<int>(f.height),
                                 crop, degrees, o.scale, o.scale_given,
                                 window_spec())) {
                return finish_exit(1);
            }
            seen_generation = presenter->input_generation();
        } else if (content_changed) {
            if (!presenter->update_content(crop, degrees, on_touch, on_keyboard)) {
                return finish_exit(1);
            }
            seen_generation = presenter->input_generation();
            std::printf(SCRCTL_TR(
                "Render content changed to %dx%d; rotation %d degrees clockwise\n"),
                view_w, view_h, degrees);
        }
        applied_degrees = degrees;
        applied_view_w = view_w;
        applied_view_h = view_h;

        const bool do_verify = o.verify_at > 0 && rendered + 1 == o.verify_at;
        if (!presenter->draw(f, crop, do_verify ? o.verify_path.c_str() : nullptr)) {
            presenter->release_input(on_touch, on_keyboard);
            return finish_exit(1);
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
        quit = presenter->pump(on_touch, on_keyboard, on_paste);
        service_paste();
    }

    if (presenter != nullptr) {
        presenter->release_input(on_touch, on_keyboard);
    }
    exit_code = finish_exit(exit_code);
    if (exit_code != 0) {
        return exit_code;
    }
    if (o.verify_at > 0 && rendered < o.verify_at) {
        std::fprintf(stderr,
                     SCRCTL_TR("Window readback was not reached (requested frame %d, rendered %d)\n"),
                     o.verify_at, rendered);
        return 1;
    }
    if (live != nullptr && !live->has_video()) {
        std::printf(SCRCTL_TR("Finished: device session closed\n"));
    } else if (live != nullptr && !live->video_decoding_enabled()) {
        std::printf(SCRCTL_TR("Finished: encoded video recording finalized\n"));
    } else {
        std::printf(SCRCTL_TR("Finished: processed %d frames\n"), rendered);
    }
    return 0;
}

} // namespace scrctl::app
