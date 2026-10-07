// 观察独立音频会话的收包、解码、RTCP 续期和缓冲水位。PCM 只用于统计峰值，
// 不打开播放设备或窗口。可同时启动视频，或在指定时刻停止/追加媒体会话。
//
// --mute 只降低手机扬声器音量，不能让镜像音频变为静音；镜像流可能取自音量
// 调节之前。判断内容是否静音应结合 PCM 峰值，不能仅凭收包数量。
// --kill-at 停止设备上的所有媒体会话；--revideo-at 不先停止已有会话，直接起视频。
// 不同会话对彼此的影响需要设备实测，历史结果见 docs/coredevice.md §17.2。
//
// --realtime 按 48 kHz 的标称速率消费 PCM；--late-open 可延迟消费，用于观察
// 开始消费后积压是否回到目标水位。默认模式消费较慢，适合观察环满后的旧帧丢弃。
// 所有参数及音量调整的行为见 --help；帮助和参数错误在设备连接之前返回。
#include <CLI/CLI.hpp>
#include <limits>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "hid/Hid.h"
#include "i18n/CliLanguage.h"
#include "i18n/Translation.h"
#include "xpc/XpcValue.h"
#include "media/AudioPump.h"
#include "media/FramePump.h"
#include "remote/Device.h"

namespace {

uint64_t now_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void print_audio_window(const scrctl::media::AudioPump::Stats &stats,
                        uint64_t elapsed_seconds, std::size_t buffered_frames,
                        uint64_t idle_seconds, std::size_t &window_peak) {
    std::printf(SCRCTL_TR(
        "+%3llus packets=%llu decoded=%llu decode_failed=%llu other_payload=%llu "
        "sequence_gaps=%llu missing_packets=%llu late_or_duplicate=%llu RR_sent/failed=%llu/%llu "
        "dropped_frames=%llu steered_frames=%llu restarts=%llu buffered=%zu_frames idle=%llus "
        "window_peak=%zu\n"),
        static_cast<unsigned long long>(elapsed_seconds),
        static_cast<unsigned long long>(stats.packets),
        static_cast<unsigned long long>(stats.decoded),
        static_cast<unsigned long long>(stats.decode_failed),
        static_cast<unsigned long long>(stats.other_payload),
        static_cast<unsigned long long>(stats.seq_gaps),
        static_cast<unsigned long long>(stats.seq_lost),
        static_cast<unsigned long long>(stats.out_of_order),
        static_cast<unsigned long long>(stats.rtcp_sent),
        static_cast<unsigned long long>(stats.rtcp_failed),
        static_cast<unsigned long long>(stats.dropped_stale),
        static_cast<unsigned long long>(stats.steered),
        static_cast<unsigned long long>(stats.restarts), buffered_frames,
        static_cast<unsigned long long>(idle_seconds), window_peak);
    // 先输出这一窗口的峰值，再清零开始下一窗口；末段峰值由结束统计另行输出。
    window_peak = 0;
}

}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::vector<std::string> udids;
    int seconds = 30;
    bool with_video = false;
    bool verbose = false;
    bool mute = false;
    int kill_at = -1;
    int revideo_at = -1;
    bool realtime = false;
    int late_open = 0;
    CLI::App app{SCRCTL_N_("Inspect audio streaming, recovery and buffer levels")};
    app.footer(SCRCTL_N_(
        "PCM is read only to measure sample peaks; no local audio playback or window is opened. "
        "--mute lowers the phone volume with 25 VolumeDown presses, then sends 25 VolumeUp "
        "presses when the test ends; it does not restore the original volume or silence the "
        "mirrored stream. --help does not connect to a device."));
    app.set_help_flag("-h,--help", SCRCTL_N_("Show help"));
    auto *seconds_option = app.add_option("-s,--seconds", seconds,
        SCRCTL_N_("Observation duration in seconds (positive integer; default: 30)"))
        ->check(CLI::Range(1, std::numeric_limits<int>::max()));
    app.add_flag("--with-video", with_video, SCRCTL_N_("Start a video stream alongside audio"));
    app.add_flag("-v,--verbose", verbose, SCRCTL_N_("Print connection and media details"));
    app.add_flag("--mute", mute, SCRCTL_N_("Lower the phone speaker volume during the test"));
    auto *kill_option = app.add_option("--kill-at", kill_at,
        SCRCTL_N_("Stop all device media streams at N seconds (-1 disables; 0 starts immediately)"))
        ->check(CLI::Range(-1, std::numeric_limits<int>::max()));
    auto *video_option = app.add_option("--revideo-at", revideo_at,
        SCRCTL_N_("Start another video stream at N seconds without stopping streams (-1 disables)"))
        ->check(CLI::Range(-1, std::numeric_limits<int>::max()));
    app.add_flag("--realtime", realtime, SCRCTL_N_("Read 480 audio frames every 10 ms"));
    auto *late_option = app.add_option("--late-open", late_open,
        SCRCTL_N_("Delay PCM consumption by N seconds in --realtime mode (nonnegative; default: 0)"))
        ->check(CLI::Range(0, std::numeric_limits<int>::max()));
    // 保留原入口重复标量取最后一个值、多个位置 UDID 取最后一个的行为。
    for (auto *option : {seconds_option, kill_option, video_option, late_option})
        option->multi_option_policy(CLI::MultiOptionPolicy::TakeLast);
    app.add_option("UDID", udids, SCRCTL_N_("Device identifier (optional; last value is used)"))
        ->expected(-1);
    scrctl::i18n::CliLanguage language(app);
    if (auto code = language.parse(argc, argv)) return *code;
    const std::string_view udid = udids.empty() ? std::string_view{} : udids.back();

    std::string err;
    auto device = scrctl::remote::Device::establish(udid, err, verbose);
    if (!device) {
        std::fprintf(stderr, SCRCTL_TR("Failed to establish device session: %s\n"), err.c_str());
        return 1;
    }

    // 默认音频选项不提供会话 UUID，音频与视频使用独立的会话标识。
    scrctl::media::AudioPump::Options ao;
    auto audio = scrctl::media::AudioPump::start(*device, ao, err, verbose);
    if (audio == nullptr) {
        std::fprintf(stderr, SCRCTL_TR("Failed to start audio stream: %s\n"), err.c_str());
        return 1;
    }
    std::printf(SCRCTL_TR("Audio stream started with its own session: port=%u payload_type=%u decoder=%s\n"),
                audio->receiver_port(), audio->payload_type(), audio->backend_name().c_str());

    std::unique_ptr<scrctl::media::FramePump> video;
    if (with_video) {
        scrctl::media::FramePump::Options vo;
        video = scrctl::media::FramePump::start(*device, vo, err, verbose);
        if (video == nullptr) {
            std::fprintf(stderr, SCRCTL_TR("Failed to start video stream; continuing audio test: %s\n"), err.c_str());
        } else {
            std::printf(SCRCTL_TR("Video stream started alongside audio\n"));
        }
    }

    // 音量键只调整手机扬声器，镜像 PCM 仍可能有声音。测试结束会发送同等数量
    // 的音量加按键；这会提高音量，不能精确恢复测试前的音量。
    std::unique_ptr<scrctl::hid::Buttons> buttons;
    if (mute) {
        std::string berr;
        buttons = scrctl::hid::Buttons::open(*device, berr, verbose);
        if (buttons == nullptr) {
            std::fprintf(stderr, SCRCTL_TR("Cannot open button service; skipping phone volume adjustment: %s\n"), berr.c_str());
        } else {
            for (int i = 0; i < 25; ++i) {
                buttons->press(scrctl::hid::button::kUsagePageConsumer,
                               scrctl::hid::button::kVolumeDown, 30, berr);
            }
            std::printf(SCRCTL_TR("Sent 25 VolumeDown presses; observing audio for %d seconds\n"),
                        seconds);
        }
    }

    const uint64_t t0 = now_ms();
    // 延迟消费模拟音频流已经启动、播放设备尚未打开时产生的积压。
    // 当收流和消费速率相等时，积压不能自行消失；观察水位调节是否将其降至目标。
    const uint64_t drain_from_ms = now_ms() + static_cast<uint64_t>(late_open) * 1000;
    auto next_drain = std::chrono::steady_clock::now();

    scrctl::media::AudioPump::Stats prev {};
    uint64_t last_change_ms = t0;
    uint64_t last_print_ms = t0;
    std::size_t peak = 0;
    std::size_t window_peak = 0;
    // scratch 按采样点分配，read() 接收音频帧数；立体声每帧含两个采样点。
    // 两种单位不能混用，否则 read() 会写出缓冲边界。
    std::vector<int16_t> scratch(480 * 2);
    const std::size_t scratch_frames = scratch.size() / 2;

    // 记录停止请求、观测到静默、收包恢复三个阶段，避免把停止后仍在路上的包
    // 当作恢复。以下进展时间还会随解码失败、重起及 RTCP 失败计数变化而推进。
    bool killed = false;
    uint64_t kill_ms = 0;
    uint64_t back_ms = 0;
    uint64_t packets_at_kill = 0;  ///< 确认静默后的包数，作为恢复判断基线
    bool saw_dead = false;
    std::unique_ptr<scrctl::media::StreamSession> second_video;
    uint64_t second_video_packets = 0;
    uint64_t second_started_ms = 0;
    bool second_reported = false;
    uint64_t longest_audio_silence_ms = 0;
    uint64_t restarts_at_kill = 0;
    while ((now_ms() - t0) / 1000 < static_cast<uint64_t>(seconds)) {
        const bool draining = !realtime || now_ms() >= drain_from_ms;
        if (realtime && draining) {
            // 使用绝对节拍，避免 sleep_for 的调度延迟持续降低消费速率。
            next_drain += std::chrono::milliseconds(10);
            // 落后超过 100 ms 时重新对齐，避免连续补取将音频环耗空。
            if (std::chrono::steady_clock::now() - next_drain > std::chrono::milliseconds(100)) {
                next_drain = std::chrono::steady_clock::now();
            }
            std::this_thread::sleep_until(next_drain);
        } else if (!realtime) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // stopAll 会停止设备上的全部媒体会话，观察 AudioPump 的检测和恢复过程。
        if (kill_at >= 0 && static_cast<int>((now_ms() - t0) / 1000) >= kill_at &&
            !killed) {
            killed = true;
            kill_ms = now_ms();
            packets_at_kill = audio->stats().packets;
            restarts_at_kill = audio->stats().restarts;
            auto input = scrctl::xpc::make_dict();
            scrctl::xpc::dict_set(input, "stopAll", scrctl::xpc::make_bool(true));
            scrctl::xpc::Value output;
            const bool ok = device->feature("com.apple.coredevice.displayservice",
                                            "com.apple.coredevice.feature.stopmediastream",
                                            "com.apple.coredevice.action.mediastreamstop", input,
                                            output, err, verbose, 10000);
            std::printf(SCRCTL_TR("At %d seconds, requested stopAll: %s\n"), kill_at,
                        ok ? SCRCTL_TR("accepted") : err.c_str());
        }
        // 直接追加视频会话，不发送停止请求；是否影响原会话由设备实测判断。
        if (revideo_at >= 0 && static_cast<int>((now_ms() - t0) / 1000) >= revideo_at &&
            second_video == nullptr) {
            scrctl::media::StreamSession::Request vr;
            std::string verr;
            second_video = scrctl::media::StreamSession::start(*device, vr, verr, verbose);
            std::printf(SCRCTL_TR("At %d seconds, requested an additional video session without a stop request: %s\n"), revideo_at,
                        second_video != nullptr
                            ? (SCRCTL_TR("started, receive port=") + std::to_string(second_video->receiver_port())).c_str()
                            : verr.c_str());
            second_started_ms = now_ms();
        }
        if (second_video != nullptr) {
            std::vector<uint8_t> d;
            std::string perr;
            while (second_video->next_packet(d, 0, perr)) {
                ++second_video_packets;
            }
            if (now_ms() - second_started_ms > 3000 && !second_reported) {
                second_reported = true;
                std::printf(SCRCTL_TR("  Additional video session: %llu packets in the first 3 seconds\n"),
                            static_cast<unsigned long long>(second_video_packets));
            }
        }
        // 消费 PCM 并统计峰值，不交给本机音频设备。实时模式延迟开放消费时不读取。
        const std::size_t want = (realtime && draining) ? 480 : scratch_frames;
        const std::size_t got = (realtime && !draining) ? 0 : audio->read(scratch.data(), want);
        if (got > 0) {
            for (std::size_t i = 0; i < got * 2; ++i) {
                const std::size_t a = scratch[i] < 0 ? -static_cast<std::size_t>(scratch[i])
                                                     : static_cast<std::size_t>(scratch[i]);
                if (a > peak) {
                    peak = a;
                }
                if (a > window_peak) {
                    window_peak = a;
                }
            }
        }
        const auto st = audio->stats();
        const bool moved = st.packets != prev.packets || st.decode_failed != prev.decode_failed ||
                           st.restarts != prev.restarts || st.rtcp_failed != prev.rtcp_failed;
        // 在推进 last_change_ms 前计算恢复间隔。先观察至少 400 ms 无进展，
        // 再建立收包基线，避免 stopAll 调用期间仍在路上的包造成虚假的恢复读数。
        if (killed && !saw_dead && now_ms() - last_change_ms >= 400) {
            saw_dead = true;
            // 静默成立后取基线，后续包数增加才记为恢复。
            packets_at_kill = st.packets;
        }
        if (killed && saw_dead && back_ms == 0 && st.packets > packets_at_kill) {
            back_ms = now_ms();
            std::printf(SCRCTL_TR("Audio packets resumed %llu ms after stopAll; idle interval=%llu ms; restarts=%llu\n"),
                        static_cast<unsigned long long>(back_ms - kill_ms),
                        static_cast<unsigned long long>(back_ms - last_change_ms),
                        static_cast<unsigned long long>(st.restarts - restarts_at_kill));
        }
        if (const uint64_t gap = now_ms() - last_change_ms; gap > longest_audio_silence_ms) {
            longest_audio_silence_ms = gap;
        }
        if (moved) {
            last_change_ms = now_ms();
            prev = st;
        }
        if (now_ms() - last_print_ms >= 2000) {
            last_print_ms = now_ms();
            print_audio_window(st, (now_ms() - t0) / 1000, audio->buffered_frames(),
                               (now_ms() - last_change_ms) / 1000, window_peak);
            if (video != nullptr) {
                const auto vs = video->stats();
                std::printf(SCRCTL_TR("        Video: packets=%llu access_units=%llu decoded=%llu other_payload=%llu restarts=%llu\n"),
                            static_cast<unsigned long long>(vs.packets),
                            static_cast<unsigned long long>(vs.aus),
                            static_cast<unsigned long long>(vs.decoded),
                            static_cast<unsigned long long>(vs.other_payload),
                            static_cast<unsigned long long>(vs.restarts));
            }
        }
    }
    const auto st = audio->stats();
    if (buttons != nullptr) {
        std::string berr;
        for (int i = 0; i < 25; ++i) {
            buttons->press(scrctl::hid::button::kUsagePageConsumer,
                           scrctl::hid::button::kVolumeUp, 30, berr);
        }
        std::printf(SCRCTL_TR("Sent 25 VolumeUp presses after the test\n"));
    }
    // 最后不足两秒的窗口不会进入周期输出，单独保留其峰值。
    std::printf(SCRCTL_TR("Longest interval without audio progress: %llu ms\n"),
                static_cast<unsigned long long>(longest_audio_silence_ms));
    std::printf(SCRCTL_TR("Total: packets=%llu decoded=%llu decode_failed=%llu peak=%zu final_window_peak=%zu (16-bit PCM) restarts=%llu\n"),
                static_cast<unsigned long long>(st.packets),
                static_cast<unsigned long long>(st.decoded),
                static_cast<unsigned long long>(st.decode_failed), peak, window_peak,
                static_cast<unsigned long long>(st.restarts));
    // 最长无进展间隔与 PCM 峰值分别描述传输和内容，不能仅由零峰值推断断流。
    audio.reset();
    return 0;
}
