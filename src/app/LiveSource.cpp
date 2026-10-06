#include "i18n/Translation.h"
#include "app/LiveSource.h"

#include "app/DeviceConnection.h"
#include "app/Reap.h"
#include "app/SourcePick.h"
#include "app/ViewGeom.h"
#include <algorithm>
#include <cstdio>

namespace scrctl::app {

LiveSource::~LiveSource() = default;

bool LiveSource::start(const std::string &serial, const std::string &wifi,
                       const std::string &record_path, bool hw_decode, bool watch_display,
                       bool want_audio, int audio_buffer_ms, const std::string &video_source,
                       const std::string &test_degrade, std::string &err) {
    // 连接设备前校验降级时刻表。非法参数应明确失败，避免测试实际未启用。
    if (!test_degrade.empty() &&
        !scrctl::app::parse_degrade_marks(test_degrade, degrade_marks_, err)) {
        err = SCRCTL_TR("Invalid --test-degrade: ") + err;
        return false;
    }
    auto dev = open_device(serial, wifi, err);
    if (!dev) {
        return false;
    }
    device_ = std::make_unique<scrctl::remote::Device>(std::move(*dev));

    // 目录没有 com.apple.coredevice.* 服务时，显示查询、订阅和媒体建立均不可用。
    // 媒体建立会输出完整目录诊断，此处避免显示查询和订阅重复输出同一错误。
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

    // 起流前查询显示几何。编码尺寸包含 HEVC 对齐填充，设备报告的可见区
    // 可用于正确裁剪和触摸映射。机型表仅覆盖已测设备，无法通用于其他尺寸。
    // 查询使用独立 deviceinfo 连接；提前执行可减少首帧显示后的额外等待。
    // 失败时 resolve_crop 使用机型表，并标明尺寸来源。
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
            std::fprintf(stderr, SCRCTL_TR("Failed to query display dimensions: %s (using model crop table)\n"),
                         derr.empty() ? SCRCTL_TR("Update contains no usable dimensions") : derr.c_str());
        }
    }

    // 初次查询只确定启动时的朝向；后续变化由常驻显示订阅推送。
    // 订阅失败仍可镜像，但朝向保持初次查询结果，无法自动跟随旋转。
    if (watch_display) {
        std::string werr;
        watcher_ = scrctl::remote::DisplayWatcher::start(*device_, display_id_, werr, false);
        if (watcher_ == nullptr && !coredevice_family_empty) {
            std::fprintf(stderr, SCRCTL_TR("Failed to subscribe to display changes: %s (automatic rotation unavailable)\n"), werr.c_str());
        }
    }

    // --video-source=screenshot 强制使用截图轮询，不尝试建立媒体流。
    const bool force_screenshot = video_source == "screenshot";
    if (!force_screenshot) {
        pump_ = scrctl::media::FramePump::start(*device_, options, err);
        if (pump_ != nullptr) {
            // 创建媒体泵时建立统计时间基线，首次速率使用真实经过的时间。
            last_stream_ms_ = SDL_GetTicks64();
        }
    }
    if (pump_ == nullptr) {
        // 设备因系统版本拒绝媒体流（9021 / requires iOS）时自动改用截图。
        // 其他错误保留原失败结果，例如会话被占用；用户也可显式选择截图模式。
        const bool version_gate = err.find("requires iOS") != std::string::npos;
        if (version_gate || force_screenshot) {
            const std::string stream_err = err;
            std::string serr;
            if (start_screenshot(/*capture_first=*/true, serr)) {
                if (!stream_err.empty()) {
                    std::printf(SCRCTL_TR("Media stream unavailable: %s\n"), stream_err.c_str());
                }
                std::printf(SCRCTL_TR(
                    "Using screenshot polling; refresh rate depends on capture time. Input control "
                    "remains available\n"));
                err.clear();
            } else if (force_screenshot) {
                err = SCRCTL_TR("Failed to start screenshot polling: ") + serr;
                return false;
            }
        }
    }
    if (pump_ == nullptr && screenshot_.source == nullptr) {
        return false;
    }
    scrctl::Frame first;
    if (screenshot_.source != nullptr) {
        uint64_t s = 0;
        if (!screenshot_.source->latest(first, s, 5000)) {
            err = SCRCTL_TR("No first screenshot within 5 seconds");
            screenshot_.source.reset();
            return false;
        }
        // 用局部序号读取首张截图，主循环仍可取得这张图。截图已裁到可见区并按
        // 界面方向摆正，可在 deviceinfo 不可用时提供显示尺寸，例如 iOS 18 设备。
        if (display_w_ == 0) {
            display_w_ = static_cast<int>(first.width);
            display_h_ = static_cast<int>(first.height);
            display_id_ = options.display_id;
            display_name_ = SCRCTL_TR("Screenshot is the visible area");
            degrees_ = 0;
        }
    } else if (!pump_->latest(first, 5000)) {
        err = SCRCTL_TR("No first decoded frame within 5 seconds");
        return false;
    }
    if (screenshot_.source != nullptr) {
        std::printf(SCRCTL_TR("Screenshot mirroring started: %s / iOS %s, %ux%u\n"),
                    device_->property("ProductType").c_str(),
                    device_->property("OSVersion").c_str(), first.width, first.height);
    } else {
        std::printf(SCRCTL_TR("Video stream started: %s / iOS %s, receive port=%u PT=%u, first frame %ux%u\n"),
                    device_->property("ProductType").c_str(),
                    device_->property("OSVersion").c_str(), pump_->receiver_port(),
                    pump_->payload_type(), first.width, first.height);
    }
    // 此处报告几何来源，无窗口客户端也能看到设备尺寸及旋转结果。
    if (display_w_ > 0) {
        std::printf(
            SCRCTL_TR(
                "Display geometry: visible area %dx%d (displayId=%llu %s), UI rotation %d "
                "degrees clockwise, stream %ux%u\n"),
            display_w_, display_h_, static_cast<unsigned long long>(display_id_),
            display_name_.c_str(), degrees_, first.width, first.height);
    }
    if (!record_path.empty()) {
        if (screenshot_.source != nullptr) {
            std::printf(SCRCTL_TR("Screenshot mode cannot record Annex-B; ignoring --record\n"));
        } else {
            std::printf(SCRCTL_TR("Recording to %s\n"), record_path.c_str());
        }
    }

    // 在取得视频首帧后建立音频，避免首帧额外等待一次音频 RPC（实测约
    // 80–100 ms）。音频失败只输出错误，视频仍继续。音频与视频使用独立会话。
    if (want_audio && screenshot_.source != nullptr) {
        std::printf(SCRCTL_TR("Screenshot mode does not start audio\n"));
    }
    if (want_audio && screenshot_.source == nullptr) {
        if (!scrctl::kHaveAudioDecoder) {
            std::fprintf(stderr, "%s\n", SCRCTL_TR(scrctl::kNoAudioDecoderMessage));
        } else {
            scrctl::media::AudioPump::Options ao;
            ao.target_backlog_ms = audio_buffer_ms;
            std::string aerr;
            audio_ = scrctl::media::AudioPump::start(*device_, ao, aerr);
            if (audio_ == nullptr) {
                std::fprintf(stderr, SCRCTL_TR("Failed to start audio: %s (video continues)\n"), aerr.c_str());
            } else {
                // 音频统计使用独立时间基线。切到截图期间音频仍在独立线程接收和解码，
                // 不能用视频统计窗口计算这段音频增量。
                last_audio_ms_ = SDL_GetTicks64();
                std::printf(SCRCTL_TR("Audio stream started: receive port=%u PT=%u backend=%s\n"), audio_->receiver_port(),
                            audio_->payload_type(), audio_->backend_name().c_str());
            }
        }
    }
    if (!degrade_marks_.empty()) {
        // 降级时刻表从视频启动完成时计时，排除配对、隧道和起流耗时。
        degrade_t0_ = SDL_GetTicks64();
        std::printf(SCRCTL_TR("--test-degrade: from now,"));
        for (std::size_t i = 0; i < degrade_marks_.size(); ++i) {
            std::printf(SCRCTL_TR(" at %.1f seconds: %s"), static_cast<double>(degrade_marks_[i]) / 1000.0,
                        i % 2 == 0 ? SCRCTL_TR("force fallback") : SCRCTL_TR("release override"));
        }
        std::printf(SCRCTL_TR(" (test mode)\n"));
    }
    return true;
}

bool LiveSource::start_playback(std::string &err) {
    if (audio_ == nullptr) {
        err = SCRCTL_TR("No playable audio stream (disabled, failed to start, or unsupported build)");
        return false;
    }
    return audio_out_.open(*audio_, err);
}

bool LiveSource::control(double x, double y, bool down, std::string &err) {
    // 输入操作通常会改变画面，因此主动唤醒视频恢复。否则设备已停止静止
    // 画面的流时，需要等静默检测窗口结束才能恢复，增加输入后的显示延迟。
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
        std::printf(SCRCTL_TR("Control connected (touch injection available)\n"));
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
    if (screenshot_.source != nullptr) {
        // 截图已经按设备界面方向合成，不再应用实时码流的旋转。
        // 启动截图和运行中切换到截图都使用同一条件。
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

bool LiveSource::start_screenshot(bool capture_first, std::string &err) {
    auto source = scrctl::media::ScreenshotSource::start(*device_, err, capture_first);
    if (!source) {
        return false;
    }
    // 新源从序号 0 起算；统计基线和时钟必须一起换，防止漏帧、无符号下溢或速率虚高。
    screenshot_ = ScreenshotState{std::move(source), 0, SDL_GetTicks64(), 0, std::nullopt};
    return true;
}

void LiveSource::update_picture_source() {
    // 只回收已退出的 worker，避免在渲染线程上等待截图 RPC；未退出的源仍持有 Device。
    scrctl::app::reap_finished(
        retired_, [](const scrctl::media::ScreenshotSource &src) { return src.worker_done(); });
    const uint64_t now = SDL_GetTicks64();
    const bool forced_dead = scrctl::app::degrade_forced(now - degrade_t0_, degrade_marks_);
    const uint64_t since_failure = screenshot_.failed_at ? now - *screenshot_.failed_at : UINT64_MAX;
    const auto action = scrctl::app::pick_picture_source(
        pump_ != nullptr, (pump_ != nullptr && pump_->video_unusable()) || forced_dead,
        screenshot_.source != nullptr, since_failure);
    switch (action) {
    case scrctl::app::SourcePick::kToShot: {
        std::string err;
        // 运行中切换异步取首张，窗口继续显示最后一帧；启动时则同步取得尺寸。
        if (start_screenshot(/*capture_first=*/false, err)) {
            std::printf(SCRCTL_TR("Live video unavailable; using screenshot polling while retrying video in the background\n"));
        } else {
            screenshot_.failed_at = now;
            std::fprintf(stderr, SCRCTL_TR("Failed to switch to screenshots: %s (retry in %llu seconds)\n"), err.c_str(),
                         static_cast<unsigned long long>(scrctl::app::kShotRetryMs / 1000));
        }
        break;
    }
    case scrctl::app::SourcePick::kToStream:
        std::printf(SCRCTL_TR("Live video recovered; switched back to video stream\n"));
        screenshot_.source->request_stop();
        retired_.push_back(std::move(screenshot_.source));
        break;
    case scrctl::app::SourcePick::kStayShot:
    case scrctl::app::SourcePick::kStayStream:
        break;
    }
}

bool LiveSource::next(scrctl::Frame &out, int timeout_ms) {
    update_picture_source();
    if (screenshot_.source) {
        return screenshot_.source->latest(out, screenshot_.serial, timeout_ms);
    }
    if (!pump_) {
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
    return SCRCTL_TR("Device connection lost (") + why + SCRCTL_TR("); closing session");
}

void LiveSource::print_stats() {
    // TCP 字节统计包括实时流和截图连接；lwIP 已负责缓存和恢复乱序段。
    if (device_ != nullptr && device_->stack() != nullptr) {
        const auto c = device_->stack()->tcp_counters();
        const uint64_t now = SDL_GetTicks64();
        const double span = last_tcp_ms_ == 0 ? 1.0 : std::max(0.001, (now - last_tcp_ms_) / 1000.0);
        const auto delta = c.recv_bytes >= last_tcp_recv_ ? c.recv_bytes - last_tcp_recv_ : 0;
        std::printf(SCRCTL_TR("  Tunnel TCP: receive %.1f KiB/s, total %.2f MiB (lwIP)\n"),
                    delta / 1024.0 / span, c.recv_bytes / (1024.0 * 1024.0));
        if (device_->stack()->net_debug()) {
            std::printf(SCRCTL_TR("  Network diagnostics: bad checksums %llu, ICMPv6 %llu, echo replies %llu\n"),
                        static_cast<unsigned long long>(device_->stack()->bad_checksums()),
                        static_cast<unsigned long long>(device_->stack()->icmp_seen()),
                        static_cast<unsigned long long>(device_->stack()->echo_replies()));
        }
        last_tcp_ms_ = now;
        last_tcp_recv_ = c.recv_bytes;
    }
    if (screenshot_.source != nullptr) {
        // 截图统计报告本次速率、累计张数、数据量和失败数。使用独立时间窗口，
        // 失败数可用于观察设备是否开始拒绝截图。
        const auto st = screenshot_.source->stats();
        const uint64_t now = SDL_GetTicks64();
        // 截图和媒体统计分别维护时钟及计数基线，防止切回实时流时将整个截图
        // 期间的增量除以单次打印间隔。
        const double secs = scrctl::app::settle_window(now, screenshot_.stats_ms);
        // 新截图源从零计数；安装源时重置基线，计算差值时再防止无符号下溢。
        const uint64_t shot_frames = scrctl::app::counter_delta(st.frames, screenshot_.frames_base);
        std::printf(SCRCTL_TR("  Screenshots: %5.2f/s, total %llu images / %llu KiB, %llu failures\n"),
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
    // SR 更新间隔可能超过一秒，空闲时实测超过四秒。设备速率按两次 SR
    // 变化之间的真实时间计算，并报告该间隔，不能直接使用日志打印周期。
    double dev_rate = 0;
    uint64_t dev_span_ms = 0;
    // 新会话的设备累计计数从零开始，计算差值前处理计数回退。
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
        // 没有新 SR 时保留上次设备速率及其采样间隔。
        dev_rate = last_dev_rate_;
        dev_span_ms = last_dev_span_ms_;
    }
    // 设备 SR 按会话计数，本地 packets 按进程累计。减去会话基线后再比较，
    // 避免将旧会话的数据误算为当前会话丢包。
    const uint64_t mine_session =
        st.packets > st.session_packets_base ? st.packets - st.session_packets_base : 0;
    std::printf(SCRCTL_TR(
        "  Video: device video packets %6.0f/s, local datagrams (including RTCP) "
        "%6.0f/s, assembled %5.1f/s, decoded %5.1f/s\n"), dev_rate,
                rate(st.packets, last_packets_), rate(st.aus, last_aus_),
                rate(st.decoded, last_decoded_));
    // 设备速率按 SR 更新间隔计算，本地速率按打印间隔计算，两者时间窗口
    // 不同，不能直接相减判断丢包。输出两个窗口，并单独报告 RTP 序号缺口。
    if (dev_span_ms == 0) {
        std::printf(SCRCTL_TR("      Sampling: device SR not received yet, local %.1f s\n"), secs);
    } else {
        std::printf(SCRCTL_TR(
            "      Sampling: device %.1f s, local %.1f s (different intervals; rates cannot "
            "be subtracted directly)\n"),
                    dev_span_ms / 1000.0, secs);
    }
    // 设备 SR 报告累计视频包，本地数据报计数含 RTCP，两者均按当前会话
    // 显示但不能直接相减；AU 和解码计数跨会话累计。
    std::printf(SCRCTL_TR(
        "      Current session: device video packets %llu, local datagrams (including "
        "RTCP) %llu; all sessions: assembled %llu, decoded %llu\n"),
                static_cast<unsigned long long>(st.dev_sent_packets),
                static_cast<unsigned long long>(mine_session),
                static_cast<unsigned long long>(st.aus),
                static_cast<unsigned long long>(st.decoded));
    std::printf(SCRCTL_TR("      Per frame: depacketize %.1f ms, decode %.1f ms, deliver %.1f ms (%llu AU calls)\n"),
                (st.ms_depacketize) / std::max<uint64_t>(1, st.packets),
                st.ms_decode / std::max<uint64_t>(1, st.decode_calls),
                st.ms_publish / std::max<uint64_t>(1, st.decode_calls),
                static_cast<unsigned long long>(st.decode_calls));
    // 拆包器计数随会话重建而清零；泵计数跨会话累加，输出中分别标明范围。
    std::printf(SCRCTL_TR("      Current session: non-video packets %llu, sequence gaps %llu, dropped fragments %llu\n"),
                static_cast<unsigned long long>(st.other_payload),
                static_cast<unsigned long long>(st.gaps),
                static_cast<unsigned long long>(st.dropped_fragments));
    std::printf(SCRCTL_TR(
        "      All sessions: no output %llu, dropped waiting for keyframe %llu, restarts "
        "%llu, oversized NAL drops %llu\n"),
                static_cast<unsigned long long>(st.no_output),
                static_cast<unsigned long long>(st.dropped_awaiting_keyframe),
                static_cast<unsigned long long>(st.restarts),
                static_cast<unsigned long long>(st.dropped_oversized));
    // 视频数据报和 SR 接收计数跨会话累加。视频停止但 SR 增长可能只是画面
    // 静止；两者都停止时需要检查连接或会话。RR 是本地发出的保活反馈，
    // 发送成功不等同于设备已经收到。
    std::printf(SCRCTL_TR("      All sessions: video packets %llu, SR %llu, sent RR %llu, PLI %llu\n"),
                static_cast<unsigned long long>(st.video_packets),
                static_cast<unsigned long long>(st.sr_packets),
                static_cast<unsigned long long>(st.rtcp_sent),
                static_cast<unsigned long long>(st.pli_sent));
    if (audio_ != nullptr) {
        const auto as = audio_->stats();
        // 音频使用独立统计窗口；截图模式期间也在持续收包和解码。
        // 不能把音频增量除以视频分支的计时窗口。音频无声时仍可收到静音包，
        // 持续收包率降到零可作为排查连接或会话的线索。
        const double audio_secs = scrctl::app::settle_window(now, last_audio_ms_);
        const auto arate = [&](uint64_t now_value, uint64_t before) {
            return static_cast<double>(now_value - before) / audio_secs;
        };
        std::printf(SCRCTL_TR(
            "  Audio: received %6.0f packets/s, decoded %6.0f packets/s, delivered %6.0f "
            "frames/s (output=%s)\n"),
                    arate(as.packets, last_audio_packets_), arate(as.decoded, last_audio_decoded_),
                    arate(audio_out_.delivered(), last_audio_delivered_),
                    audio_out_.dev_open() ? SDL_GetCurrentAudioDriver() : SCRCTL_TR("closed"));
        std::printf(SCRCTL_TR(
            "      All sessions: decode failures %llu, lost %llu, late %llu, dropped old "
            "samples %llu, adjusted samples %llu, silence fill %llu, RR sent/failed "
            "%llu/%llu, restarts %llu, buffered %zu frames, interval %.1f s\n"),
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
