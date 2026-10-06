#include "app/Application.h"

#include <SDL.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "app/Cli.h"
#include "app/DeviceConnection.h"
#include "app/FileSource.h"
#include "app/LiveSource.h"
#include "app/Presenter.h"
#include "app/SdlRuntime.h"
#include "media/StreamSession.h"
#include "remote/App.h"
#include "remote/Pasteboard.h"

namespace scrctl::app {

namespace {
/// 取得首帧后确定裁剪区域。优先使用设备报告的可见区尺寸；无法获取时，
/// 使用 media::display_crop 的机型表。编码帧可能包含 HEVC 对齐填充，
/// 直接按编码尺寸裁剪会影响画面边缘和触摸坐标。
Crop resolve_crop(const Options &o, const scrctl::Frame &f, const FrameSource &source) {
    int display_w = 0, display_h = 0;
    source.display_size(display_w, display_h);
    const bool from_device = display_w > 0 && display_h > 0;
    if (!from_device) {
        const auto fallback =
            scrctl::media::display_crop(static_cast<int>(f.width), static_cast<int>(f.height));
        display_w = fallback.w;
        display_h = fallback.h;
    }
    if (!from_device && !o.crop_set &&
        (static_cast<int>(f.width) != display_w || static_cast<int>(f.height) != display_h)) {
        std::printf("可见区 %ux%u -> %dx%d（设备未报告尺寸，使用机型表）\n", f.width,
                    f.height, display_w, display_h);
    }
    // 几何与夹取全在 ViewGeom.h 的 make_crop 里，那边可以离线自检。
    return scrctl::app::make_crop(o.crop_set, o.crop_x, o.crop_y, o.crop_w, o.crop_h,
                                  static_cast<int>(f.width), static_cast<int>(f.height), display_w,
                                  display_h);
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

    if (o.show_version) {
        // 版本号由 CMake 的 project(VERSION) 生成，供二进制输出和问题报告使用。
        std::printf("scrctl %s\n", SCRCTL_VERSION_STRING);
        return 0;
    }

    if (o.list_devices) {
        std::string err;
        auto devices = scrctl::remote::Device::list(err);
        if (!err.empty()) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        for (const auto &d : devices) {
            std::printf("%s  %s\n", d.udid.c_str(), d.connection_type.c_str());
        }
        return 0;
    }

    // 剪贴板命令不需要视频或窗口，执行后直接退出。键盘注入仅支持 US 布局的
    // ASCII；中文和 emoji 应使用剪贴板。
    // 同时指定 --copy 和 --paste 时，写入后读回验证。设备可能对无效结构返回
    // SET_REPLY 后丢弃内容，例如 types 为空时，仅检查写入回复不能确认保存成功。
    if (o.list_apps) {
        std::string err;
        auto dev = open_device(o.serial, o.wifi, err);
        if (!dev) {
            std::fprintf(stderr, "建立会话失败: %s\n", err.c_str());
            return 1;
        }
        std::vector<scrctl::remote::App::Entry> apps;
        if (!scrctl::remote::App::list(*dev, apps, err)) {
            std::fprintf(stderr, "列 App 失败: %s\n", err.c_str());
            return 1;
        }
        for (const auto &e : apps) {
            std::printf("%s\t%s\n", e.bundle_id.c_str(), e.name.c_str());
        }
        std::printf("共 %zu 个\n", apps.size());
        return 0;
    }

    if (!o.copy_text.empty() || o.paste) {
        std::string err;
        auto dev = open_device(o.serial, o.wifi, err);
        if (!dev) {
            std::fprintf(stderr, "建立会话失败: %s\n", err.c_str());
            return 1;
        }
        int rc = 0;
        if (!o.copy_text.empty()) {
            if (scrctl::remote::Pasteboard::set_text(*dev, o.copy_text, err)) {
                std::printf("已写入设备剪贴板：%zu 字节\n", o.copy_text.size());
            } else {
                std::fprintf(stderr, "--copy 写入失败: %s\n", err.c_str());
                rc = 1;
            }
        }
        if (o.paste) {
            std::string text;
            if (scrctl::remote::Pasteboard::get_text(*dev, text, err)) {
                std::printf("设备剪贴板（%zu 字节）：%s\n", text.size(), text.c_str());
            } else {
                std::fprintf(stderr, "--paste 读取失败: %s\n", err.c_str());
                rc = 1;
            }
        }
        return rc;
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
        if (!made->start(o.serial, o.wifi, o.record, o.hw_decode, !o.no_window && o.orientation < 0,
                         !o.no_audio, o.audio_buffer_ms, o.video_source, o.test_degrade, err)) {
            std::fprintf(stderr, "启动视频失败: %s\n", err.c_str());
            // 设备通话期间可能拒绝媒体流，错误码为 9022。曾观察到此时会话列表为空，
            // 截图服务仍可用；提示用户结束通话后重试。
            if (err.find("9022") != std::string::npos) {
                std::fprintf(
                    stderr,
                    "设备正在通话，请结束通话后重试。\n");
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
                     "文件回放模式无法启动设备应用，已忽略 --start-app\n");
    }
    if (live != nullptr && !o.start_app.empty()) {
        std::string spec = o.start_app;
        bool by_name = false;
        bool terminate = false;
        // 应用名称前缀使用 scrcpy 的顺序：先 ?（按名称），再 +（终止已有实例）。
        while (!spec.empty()) {
            if (spec.front() == '?') {
                by_name = true;
            } else if (spec.front() == '+') {
                terminate = true;
            } else {
                break;
            }
            spec.erase(spec.begin());
        }
        if (spec.empty()) {
            std::fprintf(stderr, "--start-app 的名字是空的\n");
            return 2;
        }
        std::string target = spec;
        if (by_name) {
            // 按名称启动需要先获取完整应用列表，回复可能有数 MiB；按 bundle ID 启动
            // 不需要这一步，因此通常更快。
            std::string lerr;
            std::vector<scrctl::remote::App::Entry> apps;
            if (!scrctl::remote::App::list(live->device(), apps, lerr)) {
                std::fprintf(stderr, "按名字找 App 失败: %s\n", lerr.c_str());
                return 1;
            }
            auto lower = [](std::string v) {
                for (char &c : v) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                return v;
            };
            const std::string want = lower(spec);
            const scrctl::remote::App::Entry *hit = nullptr;
            for (const auto &e : apps) {
                if (lower(e.name).rfind(want, 0) == 0) {
                    hit = &e;
                    break;
                }
            }
            if (hit == nullptr) {
                std::fprintf(stderr, "没有名字以 %s 开头的 App\n", spec.c_str());
                return 1;
            }
            target = hit->bundle_id;
            std::printf("--start-app=?%s -> %s\n", spec.c_str(), target.c_str());
        }
        std::string lerr;
        if (!scrctl::remote::App::launch(live->device(), target, lerr, terminate)) {
            std::fprintf(stderr, "启动 %s 失败: %s\n", target.c_str(), lerr.c_str());
            return 1;
        }
        std::printf("已启动 %s%s\n", target.c_str(), terminate ? "（已先终止原实例）" : "");
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
        std::fprintf(stderr, "SDL 初始化失败: %s\n", SDL_GetError());
        return 1;
    }
    // 音频子系统单独初始化，失败后仍可继续镜像。无声卡或音频后端不可用时，
    // 不应使视频初始化失败。
    if (live != nullptr && !o.no_audio && !o.no_audio_playback &&
        SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        std::string aerr;
        if (!live->start_playback(aerr)) {
            std::fprintf(stderr, "打开音频输出失败: %s（继续接收和解码音频）\n", aerr.c_str());
        }
    } else if (live != nullptr && live->has_audio() && o.no_audio_playback) {
        std::printf("已禁用本机音频播放，继续接收和解码音频\n");
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
                    ok ? "已注入" : "失败", ok ? "" : cerr.c_str());
        return ok ? 0 : 1;
    }

    // 音量 HUD 显示时间短，另开截图会话可能来不及。此处完成按键注入，
    // 通过当前镜像配合 --verify N 回读画面检查效果。
    if (live != nullptr && !o.test_button.empty()) {
        std::string berr;
        if (live->button(scrctl::hid::button::kUsagePageConsumer, o.test_button_code, berr)) {
            std::printf("--test-button %s: 已按下\n", o.test_button.c_str());
        } else {
            std::fprintf(stderr, "--test-button %s 失败: %s\n", o.test_button.c_str(),
                         berr.c_str());
            return 1;
        }
    }

    if (live != nullptr && !o.test_type.empty()) {
        std::string terr;
        if (live->type_text(o.test_type, 40, terr)) {
            std::printf("--test-type %s: 已注入\n", o.test_type.c_str());
        } else {
            std::fprintf(stderr, "--test-type 失败: %s\n", terr.c_str());
            return 1;
        }
    }

    int rendered = 0;
    std::unique_ptr<Presenter> presenter;
    // 记录当前窗口朝向；-1 表示尚未创建窗口，首帧需要创建 Presenter。
    int applied_degrees = -1;
    // 首次创建窗口不输出旋转提示。
    bool first_window = true;
    const Uint64 start = SDL_GetTicks64();
    Uint64 last_stats_at = SDL_GetTicks64();
    bool quit = false;

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
            std::fprintf(stderr, "注入输入失败（已停止尝试）: %s\n", control_err.c_str());
        }
    };

    // 复用 Frame 的像素缓冲，避免每帧重新分配并提交约 11 MiB 内存。
    // 取帧仍需复制像素，额外分配会增加渲染延迟。
    scrctl::Frame f;
    int last_rendered = 0;
    while (!quit) {
        if (o.time_limit > 0 &&
            SDL_GetTicks64() - start >= static_cast<Uint64>(o.time_limit) * 1000) {
            std::printf("达到 --time-limit %d 秒\n", o.time_limit);
            break;
        }
        if (runtime.stop_requested()) {
            std::printf("收到退出信号，正在关闭设备会话\n");
            break;
        }
        // 统计在取帧之前按时间输出，断流时也能显示 0 fps。
        // 分别报告本次统计窗口帧率和全程平均值，避免恢复后的平均值掩盖当前帧率。
        // 按秒计时，而非按帧数计时，以便比较不同帧率或停顿期间的表现。
        if (o.stats && SDL_GetTicks64() - last_stats_at >= 1000) {
            const Uint64 at = SDL_GetTicks64();
            const double win = std::max(0.001, static_cast<double>(at - last_stats_at) / 1000.0);
            const int got = rendered - last_rendered;
            std::printf("  渲染：累计 %d 帧，本次 %d 帧 / %.1f fps，全程平均 %.1f fps\n", rendered, got,
                        got / win,
                        rendered / std::max(0.001, static_cast<double>(at - start) / 1000.0));
            source->print_stats();
            last_stats_at = at;
            last_rendered = rendered;
        }

        // 最多等待 50 ms，之后处理窗口事件，限制关闭和移动操作的响应延迟。
        if (!source->next(f, 50)) {
            if (source->finished()) {
                std::printf("%s\n", source->end_reason().c_str());
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
                std::printf("达到 --exit-after %d\n", o.exit_after);
                break;
            }
            continue;
        }

        // 朝向改变时重建 Presenter。SDL dummy 驱动下，单独改变窗口和 logical
        // size 不会同步更新绘制面，可能使画面缩到一角；重建可统一窗口与渲染尺寸。
        // 此操作只发生在旋转时，可能短暂闪烁。
        const int degrees = o.orientation >= 0 ? o.orientation : source->orientation_degrees();
        if (degrees != applied_degrees) {
            applied_degrees = degrees;
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
            spec.fullscreen = o.fullscreen;
            spec.want_readback = o.verify_at > 0;
            presenter->set_background(o.bg[0], o.bg[1], o.bg[2]);
            if (!presenter->open(static_cast<int>(f.width), static_cast<int>(f.height),
                                 resolve_crop(o, f, *source), degrees, o.scale, o.scale_given,
                                 spec)) {
                return 1;
            }
            if (first_window) {
                first_window = false;
            } else {
                std::printf("显示方向已改为顺时针 %d°，窗口已重建\n", degrees);
            }
        }

        const bool do_verify = o.verify_at > 0 && rendered + 1 == o.verify_at;
        presenter->draw(f, do_verify ? o.verify_path.c_str() : nullptr);
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
            std::printf("达到 --exit-after %d\n", o.exit_after);
            break;
        }
        quit = presenter->pump(on_touch);
    }

    std::printf("完成：渲染 %d 帧\n", rendered);
    return 0;
}

} // namespace scrctl::app
