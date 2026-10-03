#include "app/Application.h"

#include <SDL.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <csignal>
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
#include "media/StreamSession.h"
#include "remote/App.h"
#include "remote/Pasteboard.h"

namespace scrctl::app {

namespace {
/// Ctrl-C 与 `kill` 应当让进程走正常退出路径（停流、关会话），而不是只能被 SIGKILL。
/// 这件事在 SDL 之后才成立：SDL 初始化时会接管 SIGINT/SIGTERM，而它接管之后没有任何
/// 东西转达给这个循环——实测 `kill -TERM`、`kill -INT` 和 Ctrl-C 都动不了它（进程照跑，
/// 最后只能 kill -9）。后果不只是"退不出去"：它一边跑一边占着设备那条媒体会话，而同一台
/// 设备同时只容得下一条（第二条 startmediastream 会把第一条顶掉，docs §13），于是两个
/// scrctl 互相拆对方的流——实测就是这样刷出"每 2.5 秒被设备结束一次流"的假象，而设备
/// 什么都没做错。
std::atomic<bool> g_stop_requested{false};

void on_stop_signal(int) {
    g_stop_requested = true;
}
/// 首帧到手后定下"看哪一块"。
///
/// 可见区尺寸的**来源顺序**是这条路径的重点：
/// 1. 设备自己报的（`FrameSource::display_size`，起流前向 displayinfoupdates 要的）；
/// 2. 问不到才退回 `media::display_crop` 那张按机型硬编码的表。
/// 表里只有我们量过的那一档（1136x2464 -> 1125x2436），别的机型落到表外就是整幅当
/// 可见区——右边/下边留一条垃圾边，而触摸分母也跟着错。所以第 1 条能走就一定走它。
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
        std::printf("可见区 %ux%u -> %dx%d（按机型硬编码的兜底表：问设备没问到）\n", f.width,
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
        // 版本号只有一个来源：CMake 里那个 `project(... VERSION)`。写第二处迟早会对不上，
        // 而"发的二进制里印的版本"是用户报问题时唯一能引用的东西。
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

    // 剪贴板是一条独立的路：不需要视频流、不需要窗口，所以放在起流之前，办完就退。
    //
    // 为什么必须有这两条：键盘注入只覆盖 US 布局的 ASCII，中文与 emoji 进不了设备
    // （见 src/remote/Pasteboard.h 的说明）。
    //
    // `--copy` 与 `--paste` 同时给时是"写完立刻读回"，这不是顺手：dtpasteboardd 对
    // 形状不对的内容会**回一个 SET_REPLY 表示收下、然后把内容丢掉**（types 为空就是
    // 这种情况），只发不读的话这种失败在本地完全看不出来。
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

    std::unique_ptr<FrameSource> source;
    LiveSource *live = nullptr;
    if (!o.path.empty()) {
        source = std::make_unique<FileSource>(o.path);
    } else {
        auto made = std::make_unique<LiveSource>();
        std::string err;
        if (!made->start(o.serial, o.wifi, o.record, o.hw_decode, !o.no_window && o.orientation < 0,
                         !o.no_audio, o.audio_buffer_ms, o.video_source, o.test_degrade, err)) {
            std::fprintf(stderr, "起流失败: %s\n", err.c_str());
            // 设备在通话中会直接拒绝起流（code 9022）。实测这时它的会话表是空的
            // （getmediastreamserverstatus 回 sessions: []），所以不是"有条旧流占着"，
            // 重试也不会成——不点出来，用户只会以为是我们的流没起来。截图服务不受影响，
            // 受影响的只有这条视频流。
            if (err.find("9022") != std::string::npos) {
                std::fprintf(
                    stderr,
                    "提示：设备正在通话，挂断之后再试即可（不是本地的问题，重起 scrctl 没用）。\n");
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
                     "--start-app 只对真机实时流有意义（--play 的时候没有设备可启动），已忽略\n");
    }
    if (live != nullptr && !o.start_app.empty()) {
        std::string spec = o.start_app;
        bool by_name = false;
        bool terminate = false;
        // 前缀顺序照 scrcpy：`?` 在前、`+` 在后（它的文档就是按这个顺序写的）。
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
            // 按名字找要先把整张表拉回来——那是一份几 MB 的回复，所以这一步比按
            // bundle id 慢一个数量级，scrcpy 的文档里也明说了这件事。
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
        std::printf("已启动 %s%s\n", target.c_str(), terminate ? "（先杀掉了在跑的实例）" : "");
    }

    const bool control_enabled = live != nullptr && !o.no_control;

    if (!o.render_driver.empty()) {
        // 必须在 SDL_CreateRenderer 之前设；设晚了没有任何提示，只是驱动还是默认那个。
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, o.render_driver.c_str());
    }
    // 视频子系统**按需**初始化。以前是无条件 `SDL_INIT_VIDEO | SDL_INIT_TIMER`，于是
    // `--no-window`（脚本、控制单元、CI）这条压根不碰屏幕的路径也要先能开出视频设备：
    // 在没有显示器、或者 `SDL_VIDEODRIVER` 被指坏了的环境里它直接退在 `SDL_Init`，而报的
    // 是一句"SDL 初始化失败"——听起来像整个工具起不来，其实它连窗口都不打算开。
    // 音频子系统早就是"单独初始化 + 容许失败"这个形状了，视频这边只是没人补上同一条规矩。
    Uint32 sdl_flags = SDL_INIT_TIMER;
    if (!o.no_window || o.disable_screensaver) {
        // 后者借视频子系统：`SDL_DisableScreenSaver` 在 cocoa 那边归视频设备管，
        // 没初始化视频就是个静默的不做事——那比不起窗口更骗人。
        sdl_flags |= SDL_INIT_VIDEO;
    }
    if (SDL_Init(sdl_flags) != 0) {
        std::fprintf(stderr, "SDL 初始化失败: %s\n", SDL_GetError());
        return 1;
    }
    // 音频子系统**单独**初始化并且容许失败：一台没有声卡的机器（CI、无 PulseAudio 的
    // Linux、被拔掉的接口设备）上把 SDL_INIT_AUDIO 塞进主 SDL_Init 会让整个镜像起不来，
    // 而"没有声音"从来不是不能镜像的理由。
    if (live != nullptr && !o.no_audio && !o.no_audio_playback &&
        SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        std::string aerr;
        if (!live->start_playback(aerr)) {
            std::fprintf(stderr, "打不开音频出口: %s（音频腿照收，只是不出声）\n", aerr.c_str());
        }
    } else if (live != nullptr && live->has_audio() && o.no_audio_playback) {
        std::printf("--no-audio-playback：音频腿在收与解，只是不在本机放\n");
    }
    // 必须在 SDL_Init **之后**：signal() 是抢椅子，谁最后装谁说了算，先装会被它盖掉
    // （而 sdl2-compat 没有提供 SDL_HINT_NO_SIGNALS 可以让它别接）。
    std::signal(SIGINT, on_stop_signal);
    std::signal(SIGTERM, on_stop_signal);
    if (o.disable_screensaver) {
        SDL_DisableScreenSaver();
    }

    // 输入通路的无头自检：注入一条直线就退出。用直线而不是点一下，是因为
    // "画布被拖走一段"在截图上可判定，而一次点击在多数应用里没有可见后果。
    if (live != nullptr && !o.test_touch.empty()) {
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        if (std::sscanf(o.test_touch.c_str(), "%f,%f,%f,%f", &x0, &y0, &x1, &y1) != 4) {
            std::fprintf(stderr, "--test-touch 格式应为 X0,Y0,X1,Y1，收到 %s\n",
                         o.test_touch.c_str());
            return 2;
        }
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

    // 按键的判据要另想办法：音量 HUD 只显示一秒多，另起一次截图会话根本来不及。
    // 所以这里只负责"按下去"，看效果交给同一进程里已经在跑的镜像——
    // 配 --verify N 回读第 N 帧，HUD 就在那一帧里。
    if (live != nullptr && !o.test_button.empty()) {
        static const std::pair<const char *, uint16_t> kCodes[] = {
            {"home", scrctl::hid::button::kHome},      {"lock", scrctl::hid::button::kLock},
            {"volup", scrctl::hid::button::kVolumeUp}, {"voldn", scrctl::hid::button::kVolumeDown},
            {"mute", scrctl::hid::button::kMute},
        };
        uint16_t code = 0;
        for (const auto &e : kCodes) {
            if (o.test_button == e.first) {
                code = e.second;
                break;
            }
        }
        if (code == 0) {
            std::fprintf(stderr, "不认识按键 %s（可用：home/lock/volup/voldn/mute）\n",
                         o.test_button.c_str());
            return 2;
        }
        std::string berr;
        if (live->button(scrctl::hid::button::kUsagePageConsumer, code, berr)) {
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
    // 窗口现在按哪一档朝向摆着。-1 = 还没建过窗口，所以第一帧必然进一次重建分支。
    int applied_degrees = -1;
    // 只为少打一行：第一次建窗口不需要喊"旋转 -> 重建"。
    bool first_window = true;
    const Uint64 start = SDL_GetTicks64();
    Uint64 last_stats_at = SDL_GetTicks64();
    bool quit = false;

    /// 窗口里的一次按下/移动/抬起 -> 设备上的接触/抬起。
    ///
    /// 注入失败只打一次：这是本地窗口在动鼠标，失败刷屏会把有用的帧率信息冲掉。
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

    // 帧缓冲要跨迭代复用：每轮新建一个 Frame 意味着每帧重新申请 11MB、重新缺页，
    // 而取帧那边是 `out = frame_` 的整幅拷贝——两者叠起来实测就是每帧几十毫秒。
    scrctl::Frame f;
    int last_rendered = 0;
    while (!quit) {
        if (o.time_limit > 0 &&
            SDL_GetTicks64() - start >= static_cast<Uint64>(o.time_limit) * 1000) {
            std::printf("达到 --time-limit %d 秒\n", o.time_limit);
            break;
        }
        if (g_stop_requested.load()) {
            std::printf("收到退出信号，走正常退出路径（要把设备侧那条流停掉）\n");
            break;
        }
        // 读数放在取帧**之前**。以前它挂在"这一轮取到帧了"那条分支里，于是断流的那
        // 几秒恰好是不打印的那几秒——日志在最有信息量的时刻静音，恢复之后又连着几行
        // 看不出为什么掉帧（用户报"有时候会断"，而日志里那一段什么都沒有，只有事后
        // 被拉低的平均帧率）。没帧的时候窗口照样过，打出来就是 0 fps，那才是真相。
        //
        // 按秒催、不按"每 60 帧"（12fps 时每 60 帧是 5 秒，读数摊在很长的窗口上，
        // 速率和累计值分不出来）；而且**必须打本段的速率**：`渲染 N 帧` 那一路历史上
        // 打的是"总数 / 全程时间"，一次 3 秒的停顿会把平均帧率压到 47，之后每一行都
        // 显示 47.3、47.6、47.9、48.2，看起来像"恢复之后还在持续掉帧"——而实测那几段
        // 的瞬时值是 55、55、56，早就好了。平均数只能用来发现"一直在掉"，不能用来
        // 判断"现在在掉"。
        if (o.stats && SDL_GetTicks64() - last_stats_at >= 1000) {
            const Uint64 at = SDL_GetTicks64();
            const double win = std::max(0.001, static_cast<double>(at - last_stats_at) / 1000.0);
            const int got = rendered - last_rendered;
            std::printf("  渲染 %d 帧（本段 %d 帧 = %.1f fps，全程均 %.1f fps）\n", rendered, got,
                        got / win,
                        rendered / std::max(0.001, static_cast<double>(at - start) / 1000.0));
            source->print_stats();
            last_stats_at = at;
            last_rendered = rendered;
        }

        // 50ms：再长一点，等帧期间窗口对关闭/移动的反应就开始发木。
        if (!source->next(f, 50)) {
            if (source->finished()) {
                std::printf("%s\n", source->end_reason().c_str());
                break;
            }
            // 没帧可画也要让窗口活着——此刻基本都是在等下一帧到达，而等帧的时候
            // 不泵事件，窗口就是"未响应"。
            if (presenter != nullptr) {
                quit = presenter->pump(on_touch);
            }
            continue;
        }

        if (o.no_window) {
            // 无窗口模式：只消费帧不画。给脚本/自动化用（Maa 那条路就不要窗口）。
            ++rendered;
            if (o.exit_after > 0 && rendered >= o.exit_after) {
                std::printf("达到 --exit-after %d\n", o.exit_after);
                break;
            }
            continue;
        }

        // 朝向变了就把整个 Presenter 重建一次，而不是原地改窗口尺寸。
        //
        // 为什么重建：原地改要 `SDL_SetWindowSize` + 重设 logical size，而实测在
        // dummy 驱动下绘制面尺寸根本不跟着窗口变（render_test 就是这么抓到的）。
        // 真驱动大概率跟得上，但"大概率"在这儿不够——尺寸不跟着变时 SDL 会按新的
        // logical size 等比留边，画面缩在窗口一角，那正是我们要修的 bug 的新版本。
        // 重建走的是启动时那条已经验过的路，代价只是窗口闪一下（转屏本来就是一个
        // 动作，不是每帧的事）。
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
                std::printf("界面旋转 -> 顺时针 %d°，窗口按新朝向重建\n", degrees);
            }
        }

        const bool do_verify = o.verify_at > 0 && rendered + 1 == o.verify_at;
        presenter->draw(f, do_verify ? o.verify_path.c_str() : nullptr);
        ++rendered;

        // 只有文件回放需要自己按标称帧率追节拍；实时流的到达节奏就是设备的节奏。
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
    presenter.reset();
    // 关声卡必须在 SDL_Quit 之前：Quit 把音频子系统拆了之后再去
    // SDL_CloseAudioDevice，操作的就是一个已经不存在的上下文。
    if (live != nullptr) {
        live->stop_playback();
    }
    SDL_Quit();
    return 0;
}

} // namespace scrctl::app
