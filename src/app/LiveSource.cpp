#include "i18n/Translation.h"
#include "app/LiveSource.h"

#include "app/DeviceConnection.h"
#include "app/Reap.h"
#include "app/RecordFormat.h"
#include "app/SourcePick.h"
#include "app/ViewGeom.h"
#include "media/RecordingMuxer.h"
#include "media/RecordingVideoConfig.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

namespace scrctl::app {

LiveSource::~LiveSource() {
    release_hid();
}

bool LiveSource::finish_recording(std::string &err) {
    audio_out_.close();
    if (audio_ != nullptr) {
        audio_->stop();
    }
    if (!want_video_ && audio_ != nullptr) {
        err = audio_->terminal_error();
        if (!err.empty()) return false;
    }
    std::string video_error;
    const bool video_ok = pump_ == nullptr || pump_->finish_recording(video_error);
    if (recorder_ != nullptr) {
        if (finished()) recorder_->fail(end_reason());
        std::string recording_error;
        const bool recording_ok = recorder_->finish(recording_error);
        if (!recording_ok && !recording_error_reported_) {
            std::fprintf(stderr, SCRCTL_TR("Recording failed: %s. The file may be incomplete.\n"),
                         recording_error.c_str());
            recording_error_reported_ = true;
        }
        err = video_ok ? recording_error : video_error;
        return video_ok && recording_ok;
    }
    err = video_error;
    return video_ok;
}

bool LiveSource::start(const Options &config, std::string &err) {
    const auto &serial = config.serial, &wifi = config.wifi, &record_path = config.record_path;
    const auto &video_source = config.video_source, &test_degrade = config.test_degrade;
    const auto &should_cancel = config.should_cancel;
    const bool hw_decode = config.hw_decode, watch_display = config.watch_display;
    const bool want_audio = config.want_audio, audio_dup = config.audio_dup;
    const bool decode_audio = config.decode_audio, decode_video = config.want_video && config.decode_video;
    const int audio_buffer_ms = config.audio_buffer_ms, record_orientation = config.record_orientation;
    const uint16_t wifi_port = config.wifi_port;
    const auto cancelled = [&] {
        if (!should_cancel || !should_cancel()) {
            return false;
        }
        err = SCRCTL_TR("Device startup cancelled");
        return true;
    };
    want_video_ = config.want_video;
    decode_video_ = decode_video;
    if (cancelled()) {
        return false;
    }
    if (!want_video_ && !record_path.empty()) {
        err = SCRCTL_TR("Recording without video is not supported yet");
        return false;
    }
    if (want_video_ && !decode_video && record_path.empty()) {
        err = SCRCTL_TR("Video capture without decoding requires a recording consumer");
        return false;
    }
    if (want_video_ && !decode_video && video_source == "screenshot") {
        err = SCRCTL_TR("Video capture without decoding requires a live video stream");
        return false;
    }
    if (want_video_ && !decode_video && (!test_degrade.empty() || hw_decode)) {
        err = SCRCTL_TR("Hardware decoding and fallback tests require video decoding");
        return false;
    }
    if (want_video_ && !decode_video && !scrctl::media::recording_idr_checks_available(err)) return false;
    const auto container_format = record_container_format(record_path);
    const bool container_recording = container_format.has_value();
    if (want_audio && !decode_audio && !container_recording) {
        err = SCRCTL_TR("Audio capture without PCM decoding requires a container recording with an audio track");
        return false;
    }
    if (container_recording && video_source == "screenshot") {
        err = SCRCTL_TR("Container recording requires live video; screenshot polling cannot be recorded");
        return false;
    }
    if (container_recording && !scrctl::media::RecordingMuxer::available()) {
        err = SCRCTL_TR("Container recording is unavailable in this build (libavformat required)");
        return false;
    }
    if (container_recording && !scrctl::media::RecordingMuxer::validate_video_orientation(
            *container_format, record_orientation, err)) return false;
    if (!container_recording && !record_path.empty() && record_orientation != 0) {
        err = SCRCTL_TR("Recording rotation requires MP4 or MKV; use --display-orientation to rotate only the display");
        return false;
    }
    // 连接设备前校验降级时刻表。非法参数应明确失败，避免测试实际未启用。
    if (!test_degrade.empty() &&
        !scrctl::app::parse_degrade_marks(test_degrade, degrade_marks_, err)) {
        err = SCRCTL_TR("Invalid --test-degrade: ") + err;
        return false;
    }
    auto dev = open_device(serial, wifi, err, wifi_port);
    if (!dev) {
        return false;
    }
    if (cancelled()) {
        return false;
    }
    device_ = std::make_unique<scrctl::remote::Device>(std::move(*dev));

    if (want_video_) {
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
        if (!container_recording) options.record_path = record_path;
        options.use_hardware = hw_decode;
        options.decode_video = decode_video;

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
            if (d != nullptr) {
                panel_degrees_ = scrctl::app::parse_orientation_degrees(d->orientation);
            }
            if (d != nullptr && d->width > 0 && d->height > 0) {
                display_w_ = d->width;
                display_h_ = d->height;
                display_id_ = d->id;
                display_name_ = d->name;
            } else if (!coredevice_family_empty) {
                std::fprintf(stderr, SCRCTL_TR("Failed to query display dimensions: %s (using model crop table)\n"),
                             derr.empty() ? SCRCTL_TR("Update contains no usable dimensions") : derr.c_str());
            }
        }

        if (cancelled()) {
            return false;
        }
        // 初次查询只确定启动时的朝向；后续变化由常驻显示订阅推送。
        // 订阅失败仍可镜像，视频保留初次查询结果；截图输入必须有持续的朝向来源。
        if (watch_display) {
            std::string werr;
            watcher_ = scrctl::remote::DisplayWatcher::start(*device_, display_id_, werr, false);
            if (watcher_ == nullptr && !coredevice_family_empty) {
                std::fprintf(stderr, SCRCTL_TR("Failed to subscribe to display changes: %s (automatic rotation unavailable)\n"), werr.c_str());
            }
        }

        if (cancelled()) {
            return false;
        }
        // --video-source=screenshot 强制使用截图轮询，不尝试建立媒体流。
        const bool force_screenshot = video_source == "screenshot";
        if (!force_screenshot) {
            if (container_recording) {
                scrctl::media::Recorder::Options ro;
                ro.path = record_path;
                ro.format = *container_format;
                ro.video_orientation = record_orientation;
                ro.include_audio = want_audio;
                recorder_ = scrctl::media::Recorder::start(ro, err);
                if (recorder_ == nullptr) return false;
                options.recorder = recorder_.get();
            }
            pump_ = scrctl::media::FramePump::start(*device_, options, err);
            if (pump_ != nullptr) {
                // 创建媒体泵时建立统计时间基线，首次速率使用真实经过的时间。
                stats_.video_started(SDL_GetTicks64());
            }
        }
        if (cancelled()) {
            return false;
        }
        if (pump_ == nullptr) {
            // 设备因系统版本拒绝媒体流（9021 / requires iOS）时自动改用截图。
            // 其他错误保留原失败结果，例如会话被占用；用户也可显式选择截图模式。
            const bool version_gate = err.find("requires iOS") != std::string::npos;
            if (decode_video && (version_gate || force_screenshot)) {
                if (recorder_ != nullptr) recorder_->fail(SCRCTL_TR("Live video unavailable; recording stopped"));
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
        if (cancelled()) {
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
            // 用局部序号读取首张截图，主循环仍可取得这张图。PNG 是已摆正的可见区，
            // 不能直接用它覆盖面板轴上的尺寸与原始方向；取帧时分别发布这两种几何。
        } else if (decode_video) {
            if (!pump_->latest(first, 5000)) {
                err = SCRCTL_TR("No first decoded frame within 5 seconds");
                return false;
            }
        } else if (!pump_->wait_ready(5000)) {
            err = pump_->capture_error();
            if (err.empty()) err = SCRCTL_TR("No valid recording IDR within 5 seconds");
            return false;
        }
        if (!decode_video) {
            err = pump_->capture_error();
            if (!err.empty()) return false;
        }
        uint32_t stream_w = first.width, stream_h = first.height;
        if (!decode_video) {
            int checked_w = 0, checked_h = 0;
            pump_->size(checked_w, checked_h);
            if (checked_w <= 0 || checked_h <= 0) {
                err = SCRCTL_TR("Video became unavailable before recording startup completed");
                return false;
            }
            stream_w = static_cast<uint32_t>(checked_w);
            stream_h = static_cast<uint32_t>(checked_h);
        }
        if (cancelled()) {
            return false;
        }
        if (screenshot_.source != nullptr) {
            std::printf(SCRCTL_TR("Screenshot mirroring started: %s / iOS %s, %ux%u\n"),
                        device_->property("ProductType").c_str(),
                        device_->property("OSVersion").c_str(), first.width, first.height);
        } else if (decode_video) {
            std::printf(SCRCTL_TR("Video stream started: %s / iOS %s, receive port=%u PT=%u, first frame %ux%u\n"),
                        device_->property("ProductType").c_str(),
                        device_->property("OSVersion").c_str(), pump_->receiver_port(),
                        pump_->payload_type(), first.width, first.height);
        } else {
            std::printf(SCRCTL_TR("Encoded video capture started: %s / iOS %s, receive port=%u PT=%u, checked dimensions %ux%u\n"),
                        device_->property("ProductType").c_str(),
                        device_->property("OSVersion").c_str(), pump_->receiver_port(),
                        pump_->payload_type(), stream_w, stream_h);
        }
        // 此处报告几何来源，无窗口客户端也能看到设备尺寸及旋转结果。
        if (display_w_ > 0) {
            std::printf(
                SCRCTL_TR(
                    "Display geometry: visible area %dx%d (displayId=%llu %s), UI rotation %d "
                    "degrees clockwise, stream %ux%u\n"),
                display_w_, display_h_, static_cast<unsigned long long>(display_id_),
                display_name_.c_str(), panel_degrees_.value_or(0), stream_w, stream_h);
        }
        if (!record_path.empty()) {
            if (screenshot_.source != nullptr && !container_recording) {
                std::printf(SCRCTL_TR("Screenshot mode cannot record Annex-B; ignoring --record\n"));
            } else if (screenshot_.source == nullptr) {
                std::printf(SCRCTL_TR("Recording to %s\n"), record_path.c_str());
            }
        }
    }

    // 有视频时在首帧后建立独立音频会话，避免首帧额外等待音频 RPC（实测
    // 80–100 ms）。无视频时直接建立音频；此时音频启动失败应结束整个任务。
    if (want_audio && screenshot_.source != nullptr) {
        std::printf(SCRCTL_TR("Screenshot mode does not start audio\n"));
    }
    if (want_audio && screenshot_.source == nullptr) {
        if (decode_audio && !scrctl::kHaveAudioDecoder) {
            if (!want_video_) { err = SCRCTL_TR(scrctl::kNoAudioDecoderMessage); return false; }
            if (recorder_ != nullptr) recorder_->fail(SCRCTL_TR(scrctl::kNoAudioDecoderMessage));
            std::fprintf(stderr, "%s\n", SCRCTL_TR(scrctl::kNoAudioDecoderMessage));
        } else {
            scrctl::media::AudioPump::Options ao;
            ao.target_backlog_ms = audio_buffer_ms;
            ao.audio_dup = audio_dup;
            ao.recorder = recorder_.get();
            ao.decode_pcm = decode_audio;
            ao.stop_device_on_exit = !want_video_;
            if (!audio_dup) {
                std::printf(SCRCTL_TR(
                    "Forwarding audio to the computer. Switching routes may pause the phone's "
                    "player; resume playback on the phone if needed.\n"));
            }
            std::string aerr;
            if (cancelled()) {
                return false;
            }
            // 录制首错可能在等待就绪后由异步写入线程报告。仅录制模式在
            // 音频协商前再次检查，避免已失去消费者后仍切换手机音频路由。
            if (want_video_ && !decode_video) {
                err = pump_->capture_error();
                if (!err.empty()) return false;
            }
            audio_ = scrctl::media::AudioPump::start(*device_, ao, aerr);
            if (audio_ == nullptr) {
                if (!want_video_) { err = aerr; return false; }
                if (recorder_ != nullptr) recorder_->fail(aerr);
                std::fprintf(stderr, SCRCTL_TR("Failed to start audio: %s (video continues)\n"), aerr.c_str());
            } else {
                // 音频统计使用独立时间基线。切到截图期间音频仍在独立线程接收，
                // 不能用视频统计窗口计算这段音频增量。
                stats_.audio_started(SDL_GetTicks64());
                std::printf(SCRCTL_TR("Audio stream started: receive port=%u PT=%u backend=%s\n"), audio_->receiver_port(),
                            audio_->payload_type(), audio_->backend_name().c_str());
                std::printf("%s\n", audio_dup
                    ? SCRCTL_TR("Audio routing: phone and computer (--audio-dup)")
                    : SCRCTL_TR("Audio routing: computer; phone playback is suppressed"));
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
    if (!audio_->decoding_enabled()) {
        err = SCRCTL_TR("Audio playback is unavailable because PCM decoding is disabled");
        return false;
    }
    if (!audio_out_.open(*audio_, err)) {
        if (want_video_ && recorder_ == nullptr) abandon_audio();
        return false;
    }
    return true;
}

void LiveSource::abandon_audio() {
    // 先关闭回调，确保它不再持有随后销毁的 AudioPump。
    audio_out_.close();
    if (audio_ != nullptr && recorder_ != nullptr) {
        recorder_->fail(SCRCTL_TR("Audio capture stopped before recording finished"));
    }
    audio_.reset();
}

bool LiveSource::ensure_hid(std::string &err) {
    if (hid_unavailable_) {
        err = hid_error_;
        return false;
    }
    if (device_ == nullptr) {
        err = SCRCTL_TR("Device input is unavailable before a session is started");
        fail_hid(err);
        return false;
    }
    if (hid_ == nullptr) {
        hid_ = scrctl::hid::Service::open(*device_, err);
        if (hid_ == nullptr) {
            fail_hid(err);
            return false;
        }
        std::printf(SCRCTL_TR("Device input connection established\n"));
    }
    return true;
}

void LiveSource::release_hid() {
    if (hid_ == nullptr) return;
    std::string cleanup_error;
    if (touch_down_) {
        touch_down_ = false;
        if (!hid_->touch(scrctl::hid::kSurfaceMainTouchscreen, touch_x_, touch_y_, false,
                         cleanup_error)) {
            std::fprintf(stderr, SCRCTL_TR("Failed to release device touch: %s\n"),
                         cleanup_error.c_str());
        }
    }
    if (keyboard_down_) {
        keyboard_down_ = false;
        if (!hid_->send_report(scrctl::hid::kSurfaceKeyboard, scrctl::hid::keyboard_report({}),
                               cleanup_error)) {
            std::fprintf(stderr, SCRCTL_TR("Failed to release device keys: %s\n"),
                         cleanup_error.c_str());
        }
    }
}

void LiveSource::fail_hid(const std::string &reason) {
    // 前一报告可能已留下接触或修饰键。使用同一连接尽力松开一次，保留原错；
    // 松开报告仍无法确认设备收到。之后所有输入共享失败状态，不自动重连。
    hid_error_ = reason;
    hid_unavailable_ = true;
    release_hid();
}

bool LiveSource::control(double x, double y, bool down, std::string &err) {
    // 输入通常改变画面，主动唤醒静止画面的恢复，不等静默检测窗口结束。
    if (pump_ != nullptr) pump_->wake();
    if (!ensure_hid(err)) return false;
    if (!hid_->touch(scrctl::hid::kSurfaceMainTouchscreen, x, y, down, err)) {
        // 首次按下失败时也可能已部分送达；已有触点则保留最后已发送的坐标。
        if (down && !touch_down_) {
            touch_down_ = true;
            touch_x_ = x;
            touch_y_ = y;
        }
        fail_hid(err);
        return false;
    }
    touch_down_ = down;
    touch_x_ = x;
    touch_y_ = y;
    return true;
}

bool LiveSource::keyboard_state(const std::vector<uint16_t> &usages, std::string &err) {
    if (hid_unavailable_) {
        err = hid_error_;
        return false;
    }
    if (usages.empty() && !keyboard_down_) {
        err.clear();
        return true;
    }
    if (pump_ != nullptr) pump_->wake();
    if (!ensure_hid(err)) return false;
    // send_only 失败不证明报告完全没到达；保留一次尽力松键的机会。
    keyboard_down_ = keyboard_down_ || !usages.empty();
    if (!hid_->send_report(scrctl::hid::kSurfaceKeyboard, scrctl::hid::keyboard_report(usages),
                           err)) {
        fail_hid(err);
        return false;
    }
    keyboard_down_ = !usages.empty();
    return true;
}

bool LiveSource::type_text(const std::string &text, int hold_ms, std::string &err) {
    if (hid_unavailable_) {
        err = hid_error_;
        return false;
    }
    // 空文本不发报告，也不能覆盖由物理键盘留下的按住状态。
    if (text.empty()) {
        err.clear();
        return true;
    }
    if (!ensure_hid(err)) return false;
    keyboard_down_ = true;
    if (!hid_->type_text(text, hold_ms, err)) {
        fail_hid(err);
        return false;
    }
    keyboard_down_ = false;
    return true;
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

FrameGeometry LiveSource::sample_geometry(bool screenshot) const {
    FrameGeometry geometry{display_w_, display_h_, panel_degrees_, screenshot};
    if (watcher_ != nullptr) {
        const auto st = watcher_->latest();
        if (st.width > 0 && st.height > 0) {
            geometry.display_w = st.width;
            geometry.display_h = st.height;
            geometry.panel_degrees = scrctl::app::parse_orientation_degrees(st.orientation);
        }
    }
    if (screenshot && (watcher_ == nullptr || !watcher_->alive())) {
        // 已停止的订阅保留最后快照，但设备可能在此后旋转。不能把旧角度当成
        // 当前 PNG 的方向；按未知几何处理，由 Presenter 释放触点并暂停新输入。
        geometry.panel_degrees.reset();
    }
    return geometry;
}

bool LiveSource::start_screenshot(bool capture_first, std::string &err) {
    auto source = scrctl::media::ScreenshotSource::start(*device_, err, capture_first);
    if (!source) {
        return false;
    }
    // 新源从序号 0 起算；统计基线和时钟必须一起换，防止漏帧、无符号下溢或速率虚高。
    const uint64_t now = SDL_GetTicks64();
    screenshot_ = ScreenshotState{std::move(source), 0, std::nullopt};
    stats_.reset_screenshot(now);
    return true;
}

void LiveSource::update_picture_source() {
    if (!want_video_ || !decode_video_) return;
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
        if (recorder_ != nullptr) recorder_->fail(SCRCTL_TR("Live video unavailable; recording stopped"));
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
    if (!want_video_) {
        // 音频在独立 worker 推进，控制会话也没有像素帧；有限等待避免空转。
        if (!finished()) std::this_thread::sleep_for(std::chrono::milliseconds(std::clamp(timeout_ms, 1, 50)));
        return false;
    }
    update_picture_source();
    if (recorder_ != nullptr && !recording_error_reported_) {
        const auto error = recorder_->error();
        if (!error.empty()) {
            if (decode_video_) {
                std::fprintf(stderr, SCRCTL_TR("Recording failed: %s. Mirroring continues; the file is incomplete.\n"),
                             error.c_str());
            } else {
                std::fprintf(stderr, SCRCTL_TR("Recording failed: %s. The file may be incomplete.\n"),
                             error.c_str());
            }
            recording_error_reported_ = true;
        }
    }
    if (screenshot_.source) {
        if (!screenshot_.source->latest(out, screenshot_.serial, timeout_ms)) {
            return false;
        }
        delivered_geometry_ = sample_geometry(true);
        return true;
    }
    if (!pump_) {
        return false;
    }
    const uint64_t got = pump_->newer(out, serial_, timeout_ms);
    if (got == 0) {
        return false;
    }
    serial_ = got;
    delivered_geometry_ = sample_geometry(false);
    return true;
}

bool LiveSource::finished() const {
    return !encoded_capture_error().empty() ||
           (!want_video_ && audio_ != nullptr && !audio_->terminal_error().empty()) ||
           (device_ != nullptr && device_->stack() != nullptr &&
            !device_->stack()->pump_error().empty());
}

std::string LiveSource::encoded_capture_error() const {
    if (!want_video_ || decode_video_) return {};
    if (pump_ != nullptr) return pump_->capture_error();
    return recorder_ != nullptr ? recorder_->error() : std::string{};
}

std::string LiveSource::end_reason() const {
    const auto capture_error = encoded_capture_error();
    if (!capture_error.empty()) return SCRCTL_TR("Encoded video capture stopped: ") + capture_error;
    if (!want_video_ && audio_ != nullptr) {
        auto audio_error = audio_->terminal_error();
        if (!audio_error.empty()) return SCRCTL_TR("Audio stopped: ") + audio_error;
    }
    const std::string why =
        device_ != nullptr && device_->stack() != nullptr ? device_->stack()->pump_error() : "";
    return SCRCTL_TR("Device connection lost (") + why + SCRCTL_TR("); closing session");
}

void LiveSource::print_stats() {
    LiveStats::Snapshot snapshot;
    if (device_ != nullptr && device_->stack() != nullptr) {
        const auto *stack = device_->stack();
        LiveStats::Tcp tcp;
        tcp.recv_bytes = stack->tcp_counters().recv_bytes;
        tcp.now_ms = SDL_GetTicks64();
        tcp.net_debug = stack->net_debug();
        if (tcp.net_debug) {
            tcp.bad_checksums = stack->bad_checksums();
            tcp.icmp_seen = stack->icmp_seen();
            tcp.echo_replies = stack->echo_replies();
        }
        snapshot.tcp = tcp;
    }
    if (screenshot_.source != nullptr) {
        snapshot.screenshot = LiveStats::Screenshot{screenshot_.source->stats(), SDL_GetTicks64()};
    } else if (pump_ != nullptr) {
        snapshot.video = LiveStats::Video{pump_->stats(), SDL_GetTicks64()};
    }
    if (audio_ != nullptr) {
        LiveStats::Audio audio;
        audio.counters = audio_->stats();
        audio.delivered = audio_out_.delivered();
        audio.output_open = audio_out_.dev_open();
        if (audio.output_open) {
            if (const char *driver = SDL_GetCurrentAudioDriver()) {
                audio.output_driver = driver;
            }
        }
        audio.silence = audio_out_.silence();
        audio.preroll_silence = audio_out_.preroll_silence();
        audio.underrun_silence = audio_out_.underrun_silence();
        audio.underrun_callbacks = audio_out_.underrun_callbacks();
        audio.sample_rate = audio_->sample_rate();
        audio.buffered_frames = audio_->buffered_frames();
        snapshot.audio = std::move(audio);
    }
    stats_.print(snapshot);
}

} // namespace scrctl::app
