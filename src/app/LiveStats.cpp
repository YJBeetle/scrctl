#include "app/LiveStats.h"

#include "app/StatsWindow.h"
#include "i18n/Translation.h"
#include <algorithm>
#include <cstdio>

namespace scrctl::app {

void LiveStats::video_started(uint64_t now_ms) {
    last_stream_ms_ = now_ms;
}

void LiveStats::audio_started(uint64_t now_ms) {
    last_audio_ms_ = now_ms;
}

void LiveStats::reset_screenshot(uint64_t now_ms) {
    last_screenshot_ms_ = now_ms;
    last_screenshot_frames_ = 0;
}

void LiveStats::print(const Snapshot &snapshot) {
    // TCP 字节统计包括实时流和截图连接；lwIP 已负责缓存和恢复乱序段。
    if (snapshot.tcp) {
        const auto &c = *snapshot.tcp;
        const uint64_t now = c.now_ms;
        const double span = last_tcp_ms_ == 0 ? 1.0 : std::max(0.001, (now - last_tcp_ms_) / 1000.0);
        const auto delta = c.recv_bytes >= last_tcp_recv_ ? c.recv_bytes - last_tcp_recv_ : 0;
        std::printf(SCRCTL_TR("  Tunnel TCP: receive %.1f KiB/s, total %.2f MiB (lwIP)\n"),
                    delta / 1024.0 / span, c.recv_bytes / (1024.0 * 1024.0));
        if (c.net_debug) {
            std::printf(SCRCTL_TR("  Network diagnostics: bad checksums %llu, ICMPv6 %llu, echo replies %llu\n"),
                        static_cast<unsigned long long>(c.bad_checksums),
                        static_cast<unsigned long long>(c.icmp_seen),
                        static_cast<unsigned long long>(c.echo_replies));
        }
        last_tcp_ms_ = now;
        last_tcp_recv_ = c.recv_bytes;
    }
    if (snapshot.screenshot) {
        // 截图统计报告本次速率、累计张数、数据量和失败数。使用独立时间窗口，
        // 失败数可用于观察设备是否开始拒绝截图。
        const auto &st = snapshot.screenshot->counters;
        const uint64_t now = snapshot.screenshot->now_ms;
        // 截图和媒体统计分别维护时钟及计数基线，防止切回实时流时将整个截图
        // 期间的增量除以单次打印间隔。
        const double secs = scrctl::app::settle_window(now, last_screenshot_ms_);
        // 新截图源从零计数；安装源时重置基线，计算差值时再防止无符号下溢。
        const uint64_t shot_frames = scrctl::app::counter_delta(st.frames, last_screenshot_frames_);
        std::printf(SCRCTL_TR("  Screenshots: %5.2f/s, total %llu images / %llu KiB, %llu failures\n"),
                    static_cast<double>(shot_frames) / secs,
                    static_cast<unsigned long long>(st.frames),
                    static_cast<unsigned long long>(st.bytes / 1024),
                    static_cast<unsigned long long>(st.failures));
        return;
    }
    if (!snapshot.video) {
        return;
    }
    const auto &st = snapshot.video->counters;
    const uint64_t now = snapshot.video->now_ms;
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
    if (snapshot.audio) {
        const auto &audio = *snapshot.audio;
        const auto &as = audio.counters;
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
                    arate(audio.delivered, last_audio_delivered_),
                    audio.output_open ? audio.output_driver.c_str() : SCRCTL_TR("closed"));
        std::printf(SCRCTL_TR(
            "      All sessions: decode failures %llu, lost %llu, late %llu, dropped old "
            "samples %llu, adjusted samples %llu, silence fill %llu, RR sent/failed "
            "%llu/%llu, restarts %llu, buffered %zu frames, interval %.1f s\n"),
                    static_cast<unsigned long long>(as.decode_failed),
                    static_cast<unsigned long long>(as.seq_lost),
                    static_cast<unsigned long long>(as.out_of_order),
                    static_cast<unsigned long long>(as.dropped_stale),
                    static_cast<unsigned long long>(as.steered),
                    static_cast<unsigned long long>(audio.silence),
                    static_cast<unsigned long long>(as.rtcp_sent),
                    static_cast<unsigned long long>(as.rtcp_failed),
                    static_cast<unsigned long long>(as.restarts), audio.buffered_frames,
                    audio_secs);
        std::printf(SCRCTL_TR(
            "      Playback totals: startup trim %llu frames, preroll silence %llu frames, "
            "underrun silence %llu frames in %llu callbacks\n"),
                    static_cast<unsigned long long>(as.startup_trimmed),
                    static_cast<unsigned long long>(audio.preroll_silence),
                    static_cast<unsigned long long>(audio.underrun_silence),
                    static_cast<unsigned long long>(audio.underrun_callbacks));
        std::printf(SCRCTL_TR(
            "      Audio clock: %s, average buffer %.1f ms, compensation %d ppm, "
            "added/removed %llu/%llu frames, updates %llu, failures %llu\n"),
                    as.clock.active ? SCRCTL_TR("active") : SCRCTL_TR("inactive"),
                    audio.sample_rate > 0 ? as.clock.average_frames * 1000.0 / audio.sample_rate : 0.0,
                    as.clock.compensation_ppm,
                    static_cast<unsigned long long>(as.clock.added_frames),
                    static_cast<unsigned long long>(as.clock.removed_frames),
                    static_cast<unsigned long long>(as.clock.compensation_updates),
                    static_cast<unsigned long long>(as.clock_failed));
        last_audio_packets_ = as.packets;
        last_audio_decoded_ = as.decoded;
        last_audio_delivered_ = audio.delivered;
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
