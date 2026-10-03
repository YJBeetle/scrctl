#include "app/LiveSource.h"

#include "app/DeviceConnection.h"
#include "app/Reap.h"
#include "app/SourcePick.h"
#include "app/ViewGeom.h"
#include <algorithm>
#include <cstdio>
#include <functional>

namespace scrctl::app {

LiveSource::~LiveSource() = default;

bool LiveSource::start(const std::string &serial, const std::string &wifi,
                       const std::string &record_path, bool hw_decode, bool watch_display,
                       bool want_audio, int audio_buffer_ms, const std::string &video_source,
                       const std::string &test_degrade, std::string &err) {
    // 规格写错就在碰设备之前失败：这条旗标是用来打判据的，静默忽略一个写错的规格
    // 等于让人对着一个从没生效的开关读日志。
    if (!test_degrade.empty() &&
        !scrctl::app::parse_degrade_marks(test_degrade, degrade_marks_, err)) {
        err = "--test-degrade 规格不对：" + err;
        return false;
    }
    auto dev = open_device(serial, wifi, err);
    if (!dev) {
        return false;
    }
    device_ = std::make_unique<scrctl::remote::Device>(std::move(*dev));

    // 目录里一条 com.apple.coredevice.* 都没有时（没挂 DDI 的设备就是这个形状），
    // 问几何、挂订阅、起流三步**必然**全失败，而整段目录诊断只需要打一次——起流那步
    // 是致命的、一定会打。实测一台没挂 DDI 的 iPad 上同一段诊断连着打了三遍，
    // 刷屏到没人读，所以前两步在这种情形下闭嘴。
    bool coredevice_family_empty = true;
    for (const auto &s : device_->rsd().services()) {
        if (s.name.rfind("com.apple.coredevice", 0) == 0) {
            coredevice_family_empty = false;
            break;
        }
    }

    scrctl::media::FramePump::Options options;
    options.record_path = record_path;
    options.use_hardware = hw_decode;

    // 显示几何先问设备，再起流。
    //
    // 为什么要问：编码帧的尺寸是"可见区 + HEVC 的 CU 对齐填充"，而这一圈填充多大
    // 协议里没有。此前我们按机型硬编码一档（1136x2464 -> 1125x2436），表外的机型
    // 就把整幅编码帧当可见区——后果是右/下一条垃圾边，而**触摸分母跟着错**，
    // 边缘点不准。`displayinfoupdates` 给的是设备的权威值。
    //
    // 为什么排在起流之前：这一问只要一条 deviceinfo 连接，与媒体会话无关，却要一个
    // 来回；放到起流之后就是让窗口多黑屏一个来回的时间。
    //
    // 问不到不致命：`resolve_crop` 会退回那张表，并把"是兜底"一起打出来。
    {
        std::string derr;
        const auto info = scrctl::remote::fetch_display_info(*device_, derr);
        const scrctl::remote::Display *d =
            info == std::nullopt ? nullptr : info->find(options.display_id);
        if (info != std::nullopt && d == nullptr) {
            // id 对不上时退回主屏：外接屏的 displayId 是设备分配的，不保证连续。
            d = info->primary();
        }
        if (d != nullptr && d->width > 0 && d->height > 0) {
            display_w_ = d->width;
            display_h_ = d->height;
            display_id_ = d->id;
            display_name_ = d->name;
            degrees_ = scrctl::app::orientation_degrees(d->orientation);
        } else if (!coredevice_family_empty) {
            std::fprintf(stderr, "向设备问显示几何失败: %s（退回按机型硬编码的裁剪表）\n",
                         derr.empty() ? "推送里没有可用的尺寸" : derr.c_str());
        }
    }

    // 起流前那一问只够定下"窗口打开时该转多少度"。设备之后转屏我们一无所知，
    // 所以还要有人一直挂在订阅上——它是推模型，不挂着就再也没有第二条消息。
    //
    // 失败只打一行不返回 false：没有它，画面仍然按起流前那一档转正，只是不会
    // 跟着转屏走。为一个增强功能把镜像整个停掉是不划算的。
    if (watch_display) {
        std::string werr;
        watcher_ = scrctl::remote::DisplayWatcher::start(*device_, display_id_, werr, false);
        if (watcher_ == nullptr && !coredevice_family_empty) {
            std::fprintf(stderr, "常驻显示订阅起不来: %s（转屏不会跟着转）\n", werr.c_str());
        }
    }

    // --video-source=screenshot 是**强制**：连媒体流都不去起。原先这个值只在"起流
    // 失败"那条支路里被读，于是流一起成功它就被跳过——而帮助文本承诺的是强制（审查 P2）。
    const bool force_screenshot = video_source == "screenshot";
    if (!force_screenshot) {
        pump_ = scrctl::media::FramePump::start(*device_, options, err);
        if (pump_ != nullptr) {
            // 媒体那本账的尺在泵建好这一刻起表：第一段 --stats 的分母就是真实经过的
            // 时间，而不是 `settle_window` 里那个 1.0 秒的兜底（审查 P2）。
            last_stream_ms_ = SDL_GetTicks64();
        }
    }
    if (pump_ == nullptr) {
        // 兜底门：媒体流被设备按版本拒（iOS 27 以下，code 9021，设备原话里带
        // "requires iOS"）时改走截图轮询。自动降级只在**这一种**失败上发生——别的失败
        // （比如另一客户端占着流）自动降到 2 fps 会把真问题盖住；想强制就是上面那条。
        const bool version_gate = err.find("requires iOS") != std::string::npos;
        if (version_gate || force_screenshot) {
            const std::string stream_err = err;
            std::string serr;
            auto shot = scrctl::media::ScreenshotSource::start(*device_, serr);
            if (shot != nullptr) {
                if (!stream_err.empty()) {
                    std::printf("媒体流不可用：%s\n", stream_err.c_str());
                }
                std::printf("改用截图轮询兜底：实测一次截图约 0.5 秒，画面约 2 fps——能看能操作，"
                            "不是能看视频；触摸/按键注入走同一条 HID 路，不受影响\n");
                shot_ = std::move(shot);
                last_shot_ms_ = SDL_GetTicks64(); // 兜底那本账的尺，与 next() 里那处同规矩
                err.clear();
            } else if (force_screenshot) {
                err = "截图兜底也起不来: " + serr;
                return false;
            }
        }
    }
    if (pump_ == nullptr && shot_ == nullptr) {
        return false;
    }
    scrctl::Frame first;
    if (shot_ != nullptr) {
        uint64_t s = 0;
        if (!shot_->latest(first, s, 5000)) {
            err = "兜底路 5 秒内没拿到第一张截图";
            shot_.reset();
            return false;
        }
        shot_serial_ = 0; // 首帧留给主循环去渲染：这里取它只为读尺寸与打日志
        // 截图的像素尺寸就是可见区尺寸（没有 HEVC 的 CU 填充），而且它已按界面方向
        // 摆正。iOS 18 上 deviceinfo 服务不在目录里，问几何那一问必然空手，这里补上。
        if (display_w_ == 0) {
            display_w_ = static_cast<int>(first.width);
            display_h_ = static_cast<int>(first.height);
            display_id_ = options.display_id;
            display_name_ = "截图即可见区";
            degrees_ = 0;
        }
    } else if (!pump_->latest(first, 5000)) {
        err = "5 秒内没解出第一帧";
        return false;
    }
    if (shot_ != nullptr) {
        std::printf("兜底镜像已建立：%s / iOS %s，截图 %ux%u（约 2 fps）\n",
                    device_->property("ProductType").c_str(),
                    device_->property("OSVersion").c_str(), first.width, first.height);
    } else {
        std::printf("流已建立：%s / iOS %s，收流端口=%u PT=%u，首帧 %ux%u\n",
                    device_->property("ProductType").c_str(),
                    device_->property("OSVersion").c_str(), pump_->receiver_port(),
                    pump_->payload_type(), first.width, first.height);
    }
    // 打在这里而不是打在 `resolve_crop` 里，是因为控制单元那条路根本没有窗口：
    // "几何到底是设备报的还是那张兜底表"必须是**任何**跑法都能一眼看到的读数。
    if (display_w_ > 0) {
        std::printf(
            "显示几何：设备报可见区 %dx%d（displayId=%llu %s），界面旋转顺时针 %d°，码流 %ux%u\n",
            display_w_, display_h_, static_cast<unsigned long long>(display_id_),
            display_name_.c_str(), degrees_, first.width, first.height);
    }
    if (!record_path.empty()) {
        if (shot_ != nullptr) {
            std::printf("兜底路不录 Annex-B（没有码流可录），--record 这次忽略\n");
        } else {
            std::printf("录制到 %s\n", record_path.c_str());
        }
    }

    // 音频腿排在视频腿之后：它要一个 RPC 来回（实测 80~100ms），而窗口的第一帧不该
    // 为声音等这一下。苹果是反过来先起音频的，但那条路为什么不断已经查到别处了
    // （是我们的 UDP 拼装错了，docs §13），顺序在这件事上没有作用。
    //
    // 起不来只打一行、不改返回值：`--no-audio` 之外的失败（设备拒了、非 Apple 平台
    // 没有后端）都不该让整个镜像退出。
    if (want_audio && shot_ != nullptr) {
        std::printf("兜底路没有音频腿：设备系统输出那一路和媒体流同属被版本拒的一族，不再去撞\n");
    }
    if (want_audio && shot_ == nullptr) {
        if (!scrctl::kHaveAudioDecoder) {
            std::fprintf(stderr, "%s\n", scrctl::kNoAudioDecoderMessage);
        } else {
            scrctl::media::AudioPump::Options ao;
            ao.target_backlog_ms = audio_buffer_ms;
            std::string aerr;
            audio_ = scrctl::media::AudioPump::start(*device_, ao, aerr);
            if (audio_ == nullptr) {
                std::fprintf(stderr, "音频腿起不来: %s（画面照常，只是没有声音）\n", aerr.c_str());
            } else {
                // 音频那本账的尺也在这里起表。它在运行中降级那一段**照收照解**（独立
                // 会话、独立线程，与画面从哪来无关），所以切回实时流后它的分母同样
                // 必须是整段兜底时长，而不是媒体分支那把尺剩下的约 1 秒（审查 P2）。
                last_audio_ms_ = SDL_GetTicks64();
                std::printf("音频腿已建立：收流端口=%u PT=%u 后端=%s\n", audio_->receiver_port(),
                            audio_->payload_type(), audio_->backend_name().c_str());
            }
        }
    }
    if (!degrade_marks_.empty()) {
        // 起点取"起流完成这一刻"：这条旗标要复现的是**运行中**降级，计时不该把建隧道/
        // 起流那几秒算进去（那段时间本来就没有画面可降）。
        degrade_t0_ = SDL_GetTicks64();
        std::printf("--test-degrade：从现在起");
        for (std::size_t i = 0; i < degrade_marks_.size(); ++i) {
            std::printf(" %.1f 秒%s", static_cast<double>(degrade_marks_[i]) / 1000.0,
                        i % 2 == 0 ? "强制降级" : "放开");
        }
        std::printf("（只顶 video_dead 一个入参，状态机本身没改）\n");
    }
    return true;
}

bool LiveSource::start_playback(std::string &err) {
    if (audio_ == nullptr) {
        err = "没有音频腿可放（--no-audio、起流失败，或这个构建没有音频后端）";
        return false;
    }
    return audio_out_.open(*audio_, err);
}

bool LiveSource::control(double x, double y, bool down, std::string &err) {
    // 手一动就是"接下来画面一定会变"的信号。设备在画面静止时会把流结束掉，而泵
    // 最快也要等满静默窗口才发现——不催这一次，手感就是"点下去愣一下才动"。
    if (pump_ != nullptr) {
        pump_->wake();
    }
    if (hid_ == nullptr) {
        if (hid_unavailable_) {
            return false;
        }
        hid_ = scrctl::hid::Service::open(*device_, err);
        if (hid_ == nullptr) {
            hid_unavailable_ = true;
            return false;
        }
        std::printf("控制已接通（触摸注入可用）\n");
    }
    return hid_->touch(scrctl::hid::kSurfaceMainTouchscreen, x, y, down, err);
}

bool LiveSource::type_text(const std::string &text, int hold_ms, std::string &err) {
    if (hid_ == nullptr) {
        if (hid_unavailable_) {
            return false;
        }
        hid_ = scrctl::hid::Service::open(*device_, err);
        if (hid_ == nullptr) {
            hid_unavailable_ = true;
            return false;
        }
    }
    return hid_->type_text(text, hold_ms, err);
}

bool LiveSource::button(uint16_t usage_page, uint16_t usage_code, std::string &err) {
    if (buttons_ == nullptr) {
        buttons_ = scrctl::hid::Buttons::open(*device_, err);
        if (buttons_ == nullptr) {
            return false;
        }
    }
    return buttons_->press(usage_page, usage_code, 90, err);
}

void LiveSource::display_size(int &width, int &height) const {
    width = display_w_;
    height = display_h_;
}

int LiveSource::orientation_degrees() const {
    if (shot_ != nullptr) {
        // 截图服务给的是设备合成好的正立图（docs §24），再按朝向转就转歪。起流就降级
        // 的那条路靠 degrees_=0 兜住，运行中切过来也得生效，所以判据挂在 shot_ 上。
        return 0;
    }
    if (watcher_ != nullptr) {
        const auto st = watcher_->latest();
        if (!st.orientation.empty()) {
            return scrctl::app::orientation_degrees(st.orientation);
        }
    }
    return degrees_;
}

bool LiveSource::next(scrctl::Frame &out, int timeout_ms) {
    // 运行中的降级与回升：FramePump 连续重起仍解不出关键帧时置 video_unusable_ 并喊
    // "取帧方请改走截图服务"，真解出关键帧后又清掉。起流那一刻的兜底门只覆盖"起流就
    // 失败"，这一条覆盖"跑着跑着解不出来"——不接的话窗口永久停在旧画面上（审查 P1）。
    // 四格状态账在 pick_picture_source 里、离线跑全组合（tests/app_test.cpp）；语义与
    // MaaFW 控制单元一致（ScrctlSession 每次取帧都问同一个标志）。两条路的序号各记各的
    // （serial_ / shot_serial_），来回切不会把截图的号喂给泵当"since"。
    // 先回收上一轮退下来的截图源：判据是"worker 自己说它退了"，所以这一趟不阻塞。
    // 不收的话每个都揣着一整张解码好的 BGRA 画面（本机实测 1125×2436×4 = 10.5 MiB）
    // 挂到 teardown，反复降级/回升就按切换次数堆内存（审查 P2）。还没退的留到下一帧
    // 再看——它最迟一次截图 RPC（上限 5 秒）后就退，所以表里同时存在的个数是有界的。
    scrctl::app::reap_finished(
        retired_, [](const scrctl::media::ScreenshotSource &src) { return src.worker_done(); });
    const uint64_t now_ticks = SDL_GetTicks64();
    // --test-degrade：把 `video_dead` 这一个入参顶成真，好让运行中降级那条路在真机上
    // 跑起来（真触发器要画面复杂到超出解码后端上限，静止画面上打不响）。状态机本身
    // 一行没改，所以这里跑出来的读数对真降级同样成立。
    const bool forced_dead = scrctl::app::degrade_forced(now_ticks - degrade_t0_, degrade_marks_);
    const uint64_t since_shot_fail = shot_failed_ ? now_ticks - shot_fail_ms_ : UINT64_MAX;
    switch (scrctl::app::pick_picture_source(
        pump_ != nullptr, (pump_ != nullptr && pump_->video_unusable()) || forced_dead,
        shot_ != nullptr, since_shot_fail)) {
    case scrctl::app::SourcePick::kToShot: {
        std::string serr;
        // 首张**不同步拿**：这一切换发生在渲染线程上，同步拿会把窗口冻到一次
        // 截图 RPC 的上限（审查 P3）。worker 起来后约半秒就有第一张，这期间窗口
        // 继续显示媒体流的最后一帧——与"永久停住"的区别是它自己在往前走。
        auto shot = scrctl::media::ScreenshotSource::start(*device_, serr,
                                                           /*capture_first=*/false);
        if (shot != nullptr) {
            std::printf("媒体流当前解不出画面，改走截图轮询兜底（约 2 fps）；"
                        "泵在后台按退避继续试，解出来会切回来\n");
            shot_ = std::move(shot);
            // 序号跟着源一起换：`shot_serial_` 是"上一个截图源交到第几张"，而
            // 新源从 0 起算，latest() 只接受**大于**它的序号。不归零的话第二次
            // 降级要空等"上一轮一共取了多少张"那么久才出画面（第一轮跑了 3 分钟
            // 就是等 3 分钟），期间窗口停在媒体流最后一帧上——症状与这条修复要
            // 解决的"永久停住"几乎没区别（审查 P1）。
            shot_serial_ = 0;
            // 同一族的账还有一本：--stats 的截图速率是拿"上一次打印时的累计张数"
            // 做差，新源从 0 重数，不归零那一次做差就下溢（真机打过 1.8e19/s）。
            last_shot_frames_ = 0;
            // 那本账的尺也在这里起表：第一次结算的分母就是"这个源活了多久"，
            // 而不是一个假的 1.0 秒（审查 P2）。
            last_shot_ms_ = SDL_GetTicks64();
        } else {
            // 冷却期（kShotRetryMs）内不再撞，冷却一过再试：一次失败不判永久，
            // 否则截图 RPC 只是暂时不通时，本次会话就一路停在旧画面上（审查 P2）。
            std::fprintf(stderr, "想降级到截图兜底但它起不来: %s（%llu 秒后再试）\n", serr.c_str(),
                         static_cast<unsigned long long>(scrctl::app::kShotRetryMs / 1000));
            shot_failed_ = true;
            shot_fail_ms_ = now_ticks;
        }
        break;
    }
    case scrctl::app::SourcePick::kToStream:
        // 别在渲染线程上等 worker：request_stop() 只置标志+唤醒，join 在析构里，
        // 而对象挪进 retired_ 之后析构发生在 teardown（审查 P3 的第二次修复——
        // 上一轮把析构挪走了，但那时 stop() 自己还会 join，等于没挪）。
        // worker 最迟在一次截图 RPC（上限 5 秒）结束后退出，这期间它只用 device_，
        // 而 retired_ 声明在 device_ 之后、先于它析构。
        std::printf("媒体流又能解出画面了，切回实时流\n");
        shot_->request_stop();
        retired_.push_back(std::move(shot_));
        shot_.reset();
        break;
    case scrctl::app::SourcePick::kStayShot:
    case scrctl::app::SourcePick::kStayStream:
        break;
    }
    if (shot_ != nullptr) {
        return shot_->latest(out, shot_serial_, timeout_ms);
    }
    if (pump_ == nullptr) {
        return false;
    }
    const uint64_t got = pump_->newer(out, serial_, timeout_ms);
    if (got == 0) {
        return false;
    }
    serial_ = got;
    return true;
}

bool LiveSource::finished() const {
    return device_ != nullptr && device_->stack() != nullptr &&
           !device_->stack()->pump_error().empty();
}

std::string LiveSource::end_reason() const {
    const std::string why =
        device_ != nullptr && device_->stack() != nullptr ? device_->stack()->pump_error() : "";
    return "设备断开了（隧道已死：" + why + "），走正常退出路径";
}

void LiveSource::print_stats() {
    // 隧道内 TCP 的账打在两个分支之前，因为它与"画面从哪条路来"无关——而兜底截图
    // 那条路恰恰最容易撞见重排（每张图一条新连接、每次回复几 MB）。
    //
    // 什么时候打：丢过东西就打；此外**第一段一定打一次**，--debug-net 时每段都打。
    // 那一次不是噪音，是自证——"丢弃 0"与"账根本没接到这条栈上"在日志里长得一模
    // 一样，只有把分母（收到的字节）也打出来一次才分得清。占比是"要不要给这个栈加
    // 乱序重组"的判据，所以它必须带分母（§20：没有分母的数不是读数）。
    if (device_ != nullptr && device_->stack() != nullptr) {
        const auto c = device_->stack()->tcp_counters();
        const bool verbose = device_->stack()->net_debug();
        if (c.dropped_bytes > 0 || verbose || !tcp_line_printed_) {
            const uint64_t now = SDL_GetTicks64();
            const double tcp_secs =
                last_tcp_ms_ == 0
                    ? 1.0
                    : std::max(0.001, static_cast<double>(now - last_tcp_ms_) / 1000.0);
            const uint64_t recv_delta =
                c.recv_bytes >= last_tcp_recv_ ? c.recv_bytes - last_tcp_recv_ : 0;
            const uint64_t drop_delta =
                c.dropped_bytes >= last_tcp_drop_ ? c.dropped_bytes - last_tcp_drop_ : 0;
            const uint64_t total = c.recv_bytes + c.dropped_bytes;
            std::printf(
                "  隧道TCP: 收 %6.1f KB/s 乱序丢弃 %5.1f KB/s｜全程 收 %.2f MB 丢 %llu 段 / "
                "%.2f MB，占 %.2f%%（不重排，靠重传补）\n",
                static_cast<double>(recv_delta) / 1024.0 / tcp_secs,
                static_cast<double>(drop_delta) / 1024.0 / tcp_secs,
                static_cast<double>(c.recv_bytes) / (1024.0 * 1024.0),
                static_cast<unsigned long long>(c.dropped_segments),
                static_cast<double>(c.dropped_bytes) / (1024.0 * 1024.0),
                total == 0
                    ? 0.0
                    : 100.0 * static_cast<double>(c.dropped_bytes) / static_cast<double>(total));
            tcp_line_printed_ = true;
            last_tcp_ms_ = now;
            last_tcp_recv_ = c.recv_bytes;
            last_tcp_drop_ = c.dropped_bytes;
        }
    }
    if (shot_ != nullptr) {
        // 兜底路只有一把尺：截图张数。打速率不打累计（§20 那条教训：没有分母的
        // 数不是读数），失败数单独给——它是"设备开始拒截图"的唯一信号。
        const auto st = shot_->stats();
        const uint64_t now = SDL_GetTicks64();
        // 这一本账自己的尺。以前它与下面媒体那本共用 `last_stats_ms_`，而两本的
        // 计数基线各更新各的：切回实时流后第一段 --stats 会把兜底期间的增量除以
        // 约 1 秒（审查 P2）。
        const double secs = scrctl::app::settle_window(now, last_shot_ms_);
        // 换源会让这个计数从零重数（每次降级都新建一个源）。装上新的源时已经把
        // `last_shot_frames_` 归零，这里再挡一道：真机上打出过
        // `画面 18156244167036960768.00/s`（uint64 做差下溢）。
        const uint64_t shot_frames = scrctl::app::counter_delta(st.frames, last_shot_frames_);
        std::printf("  兜底截图: 画面 %5.2f/s 累计 %llu 张 / %llu KB 失败 %llu\n",
                    static_cast<double>(shot_frames) / secs,
                    static_cast<unsigned long long>(st.frames),
                    static_cast<unsigned long long>(st.bytes / 1024),
                    static_cast<unsigned long long>(st.failures));
        return;
    }
    if (pump_ == nullptr) {
        return;
    }
    const auto st = pump_->stats();
    const uint64_t now = SDL_GetTicks64();
    const double secs = scrctl::app::settle_window(now, last_stream_ms_);
    const auto rate = [&](uint64_t now_value, uint64_t before) {
        return static_cast<double>(now_value - before) / secs;
    };
    // 设备的 SR 每 `RTCPSendInterval` 秒才来一个（实测空闲时会拖到 4 秒以上），
    // 所以它的增量**不能**除以打印窗口，否则一次增量被摊成一秒的速率，数会虚高
    // 好几倍。除以"上一次 SR 变化到现在"的真实间隔，并且把这个间隔一起打出来。
    double dev_rate = 0;
    uint64_t dev_span_ms = 0;
    // 重起会话会让设备侧的累计数归零，做差会下溢成一个天文数字。
    const bool dev_reset = st.dev_sent_packets < last_dev_packets_;
    if (dev_reset) {
        last_dev_packets_ = st.dev_sent_packets;
        last_dev_change_ms_ = now;
        last_dev_rate_ = 0;
    } else if (st.dev_sent_packets != last_dev_packets_ && last_dev_change_ms_ != 0) {
        dev_span_ms = now - last_dev_change_ms_;
        dev_rate = static_cast<double>(st.dev_sent_packets - last_dev_packets_) /
                   std::max(0.001, dev_span_ms / 1000.0);
    } else {
        // 这一档没有新的 SR，沿用上一个 SR 算出来的速率——分母也就还是它的分母。
        dev_rate = last_dev_rate_;
        dev_span_ms = last_dev_span_ms_;
    }
    // 本会话收到的包数。设备 SR 里的累计数是**每条会话从零重数**的，而我们的
    // packets 全程连着涨，所以只有减掉基线两者才在同一条数轴上——以前直接打
    // 全程累计，重起过一次之后读数长成"累计 设备 143 我 5866"，像丢了五千包。
    const uint64_t mine_session =
        st.packets > st.session_packets_base ? st.packets - st.session_packets_base : 0;
    std::printf("  流: 设备发了 %6.0f/s 我收到 %6.0f/s | AU %5.1f/s 解码 %5.1f/s\n", dev_rate,
                rate(st.packets, last_packets_), rate(st.aus, last_aus_),
                rate(st.decoded, last_decoded_));
    // 两个"每秒"的分母不是一把尺：SR 大约每秒才来一个，它的增量只能除以"上一个
    // SR 到现在"，而我们的速率除以打印窗口（实测这个窗口在 0.6~1.3 秒之间飘）。
    // 所以这两个数**相减没有意义**——早先那行 `差 +283 / -283` 就是把它们硬减出来
    // 的，一虚一实读成"在大量丢包"，而真正的丢包读数在下面那行 `序号缺口` 上，
    // 全程是 0。这里把两个分母都打出来，谁看谁会别再犯。
    if (dev_span_ms == 0) {
        std::printf("      分母：设备那档还没有 SR 可除（第一条 SR 未到），我 %.1fs\n", secs);
    } else {
        std::printf("      分母：设备 %.1fs（SR 每 ~1s 一个） 我 %.1fs（两档相减无意义）\n",
                    dev_span_ms / 1000.0, secs);
    }
    // 这一行的两个数是唯一在同一条数轴上的读数（都按会话起点归零），所以它是
    // "设备到底发了多少 vs 我们收到多少"的权威比。AU/解码不在这个轴上：它们
    // 全程连着涨，没有会话基线，所以老实标成"全程"。
    std::printf("      本会话累计 设备 %llu 我 %llu｜全程 AU %llu 解码 %llu\n",
                static_cast<unsigned long long>(st.dev_sent_packets),
                static_cast<unsigned long long>(mine_session),
                static_cast<unsigned long long>(st.aus),
                static_cast<unsigned long long>(st.decoded));
    std::printf("      每帧耗时：拆包 %.1f ms 解码 %.1f ms 交付 %.1f ms（AU %llu 次）\n",
                (st.ms_depacketize) / std::max<uint64_t>(1, st.packets),
                st.ms_decode / std::max<uint64_t>(1, st.decode_calls),
                st.ms_publish / std::max<uint64_t>(1, st.decode_calls),
                static_cast<unsigned long long>(st.decode_calls));
    // 计数器不是一套基线，混在一行里就会读出"重起之后非视频载荷从 19 变成 0，
    // 是不是把 SR 弄丢了"这种假问题：前三个跟着拆包器每会话归零（拆包器换会话就
    // 重建），后四个全程累加。分开标。
    std::printf("      本会话 非视频载荷 %llu 序号缺口 %llu 分片作废 %llu\n",
                static_cast<unsigned long long>(st.other_payload),
                static_cast<unsigned long long>(st.gaps),
                static_cast<unsigned long long>(st.dropped_fragments));
    std::printf("      全程 未出帧 %llu 等关键帧丢 %llu 重起 %llu 超大NAL丢 %llu\n",
                static_cast<unsigned long long>(st.no_output),
                static_cast<unsigned long long>(st.dropped_awaiting_keyframe),
                static_cast<unsigned long long>(st.restarts),
                static_cast<unsigned long long>(st.dropped_oversized));
    // 泵自己按数据报开头分的两类，跨会话连着涨。这两个数是"画面在不在变"的读数：
    // 视频那一档停下来不动而 SR 照每秒一个，就是屏幕静止（流还活着）；两档都停，
    // 才是设备把流结束掉了。
    // 后面那一档是**我们往外发**的续命 RR：设备的会话计时器只在收到它的时候复位，
    // 所以"流为什么断了"先看这三个数的哪一档停了。
    std::printf("      全程 视频数据报 %llu SR 心跳 %llu 发出 RR %llu PLI %llu（视频档停=画面静止，"
                "SR 也停=流死了，RR 不涨=我们没在续命）\n",
                static_cast<unsigned long long>(st.video_packets),
                static_cast<unsigned long long>(st.sr_packets),
                static_cast<unsigned long long>(st.rtcp_sent),
                static_cast<unsigned long long>(st.pli_sent));
    if (audio_ != nullptr) {
        const auto as = audio_->stats();
        // 音频是**第三本账**，也得有自己的尺：兜底那段时间里音频腿照收照解（它是
        // 独立会话、独立线程），共用媒体那把尺的话，切回来这一段会把整段兜底期间
        // 的增量除以约 1 秒。分母的规矩与上面一致——速率除以自己这本账的窗口，
        // 累计数标"全程"。音频腿的分母天生比视频稳：设备在没有声音的时候**照发**包
        // （实测 100 包/秒、20 秒一秒不少），所以这一行的"包"是平的，一旦它掉到 0
        // 就是流死了。
        const double audio_secs = scrctl::app::settle_window(now, last_audio_ms_);
        const auto arate = [&](uint64_t now_value, uint64_t before) {
            return static_cast<double>(now_value - before) / audio_secs;
        };
        std::printf("  音频: 包 %6.0f/s 解出 %6.0f/s 交付 %6.0f 帧/s（出口=%s）\n",
                    arate(as.packets, last_audio_packets_), arate(as.decoded, last_audio_decoded_),
                    arate(audio_out_.delivered(), last_audio_delivered_),
                    audio_out_.dev_open() ? SDL_GetCurrentAudioDriver() : "未开");
        std::printf("      全程 解败 %llu 真丢 %llu 迟到 %llu 丢旧 %llu 调速 %llu 补静音 %llu "
                    "RR %llu/%llu 重起 %llu 缓冲 %zu 帧｜分母 %.1fs\n",
                    static_cast<unsigned long long>(as.decode_failed),
                    static_cast<unsigned long long>(as.seq_lost),
                    static_cast<unsigned long long>(as.out_of_order),
                    static_cast<unsigned long long>(as.dropped_stale),
                    static_cast<unsigned long long>(as.steered),
                    static_cast<unsigned long long>(audio_out_.silence()),
                    static_cast<unsigned long long>(as.rtcp_sent),
                    static_cast<unsigned long long>(as.rtcp_failed),
                    static_cast<unsigned long long>(as.restarts), audio_->buffered_frames(),
                    audio_secs);
        last_audio_packets_ = as.packets;
        last_audio_decoded_ = as.decoded;
        last_audio_delivered_ = audio_out_.delivered();
    }
    last_packets_ = st.packets;
    if (st.dev_sent_packets != last_dev_packets_) {
        last_dev_rate_ = dev_rate;
        last_dev_span_ms_ = dev_span_ms;
        last_dev_change_ms_ = now;
        last_dev_packets_ = st.dev_sent_packets;
    }
    last_aus_ = st.aus;
    last_decoded_ = st.decoded;
}

} // namespace scrctl::app
