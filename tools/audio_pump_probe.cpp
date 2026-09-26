// 探针：音频腿接进产品之前，先把四个只能问设备的问题一次问清楚。
//
// 这四个问题每一个都会决定 `AudioPump` 里的一段逻辑，而猜的代价是"声音时不时卡一下"
// 这种在现场才发现、又最难查的症状：
//   ① 音频腿**自己不**跟视频腿共用 ClientSessionID 能不能起流？（产品故意分开：
//      共用的话 `probe()` 就退化成"至少一条腿活着"，视频那条判活的尺会被音频掩护掉）
//   ② 设备在**没有声音**的时候还发不发音频包？—— 视频腿的答案是"一个视频包都不发，
//      但 RTCP SR 照发"（docs §13），如果音频同构，那"多久没包算死"的阈值就必须留得
//      比任何静默段落都长，只能靠 probe 判死。
//   ③ 每秒一个 RR 能不能把音频这条会话也续过 20 秒？（视频腿已实测能，但那是另一条
//      会话、另一套 SSRC，不能代推）
//   ④ 音频腿与视频腿同时存在时，两边各收到多少包、谁有没有被顶掉。
//
// 判据 ③ 只要"跑过 20 秒而重起=0"就成立（对照组视频腿早就做过：不发 RTCP 精确
// 19.97 秒死），所以这里不再单独留一个不发 RTCP 的臂。
//
// ②这一问要一段"承载的声音确实是零"的观测。`--mute` 是按硬件音量减把**手机自己的
// 喇叭**关掉（25 次，测完按回）——但它**管不着这条流**：实测音量按到 0 之后解出来的
// 帧峰值仍然是 12780/22097 这个量级，`audioSystemOutput` 是音量之前的抽头（和
// AirPlay 一样，docs §17.2 ②）。所以这一臂造不出"镜像里的静音"，它只能让手机自己不响；
// 真正的全零来自内容自己静下去（应用切在两集之间）。
//
// 留着它的理由就剩两条：夜里跑实验不吵人，以及把"音量键影响不到镜像"这件事钉在一个
// 可执行的探针上。
//
// 一次踩坑要记着：这一臂差点拿"有声音时在发 100 包/秒"当成"没声音时也在发"——
// 前者是测出来的，后者压根没测过。所以每一段的**峰值**也要打出来，它才是
// "此刻到底有没有声音"的读数；只有包数会被静音帧继续发这件事骗过去。
//
// 还有一问不在那四条里，是接进产品之后才冒出来的：**视频腿重起会连带把音频会话带走**。
// `stopmediastream` 只有 `{stopAll: true}` 这一种形状，它停的是设备上所有会话，而
// `FramePump::restart()` 每次重起都要发它一下。所以"画面坏了一次重起"这件事的代价
// 不该只有一画面卡顿，还得看声音掉多久——`--kill-at 秒` 就是照那个形状发一次 stopAll，
// 然后量从发出到音频重新有包之间隔了多久。
//
// 但那条实验只量了"发 stopAll 会连带什么"，它不回答**该不该**发。看着像不该：第二条
// `startmediastream` 本来就会把第一条顶掉（docs §13，`tools/two_session_probe`），所以
// 那一下 stop 对自己的目的没有增量。`--revideo-at N` 就是去验这个推断的：第 N 秒
// **一个 stop 都不发、直接起第二条视频会话**，看音频这条腿掉不掉包。
//
// **推断被否了**（两臂各跑一遍，判据是最后那行"音频最长包静默"）：
//   只有音频腿在场 + 起一条视频  → 两条都活 18 秒，音频最长静默 268ms、零重起；
//   音频与视频都在场 + 再起第二条视频 → 音频断（400ms 静默后被判死重起），
//                                     旧那条视频也断（FramePump 自己记了一次重起）。
// 所以"视频腿重起会连带杀音频"与发不发 stopAll 无关，是设备侧的会话表本身就这样。
// stopAll 那一条留着只是让它更早更确定。结论与对策写在 docs §17.2 ⑤。
//
// 用法：audio_pump_probe [--seconds 30] [--with-video] [--mute] [--kill-at 8]
//                       [--revideo-at N] [-v] [UDID]
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "hid/Hid.h"
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

}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string_view udid;
    int seconds = 30;
    bool with_video = false;
    bool verbose = false;
    bool mute = false;
    int kill_at = -1;
    int revideo_at = -1;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-v" || a == "--verbose") {
            verbose = true;
        } else if (a == "--with-video") {
            with_video = true;
        } else if (a == "--mute") {
            mute = true;
        } else if ((a == "--kill-at") && i + 1 < argc) {
            kill_at = std::atoi(argv[++i]);
        } else if ((a == "--revideo-at") && i + 1 < argc) {
            revideo_at = std::atoi(argv[++i]);
        } else if ((a == "--seconds" || a == "-s") && i + 1 < argc) {
            seconds = std::atoi(argv[++i]);
        } else if (a == "-h" || a == "--help") {
            std::printf(
                "用法: %s [--seconds 30] [--with-video] [--mute] [--kill-at 8] [-v] [UDID]\n"
                "  只跑音频腿看 ①②③；带 --with-video 看 ④；带 --mute 先把设备静音再看"
                "  包还来不来（测完按回音量）；--kill-at N 在第 N 秒发一次 stopAll，"
                "量音频掉多久。全程不开窗口、本机不出声"
                "（PCM 只取出来算峰值，不接任何音频设备）。\n",
                argv[0]);
            return 0;
        } else if (a.starts_with("-")) {
            std::fprintf(stderr, "未知选项 %s\n", a.c_str());
            return 2;
        } else {
            udid = a;
        }
    }

    std::string err;
    auto device = scrctl::remote::Device::establish(udid, err, verbose);
    if (!device) {
        std::fprintf(stderr, "建立会话失败: %s\n", err.c_str());
        return 1;
    }

    // ①：Options 留默认（client_session_uuid 为空）就是"本腿自己一个会话号"。
    scrctl::media::AudioPump::Options ao;
    auto audio = scrctl::media::AudioPump::start(*device, ao, err, verbose);
    if (audio == nullptr) {
        std::fprintf(stderr, "① 音频腿起流失败: %s\n", err.c_str());
        return 1;
    }
    std::printf("① 音频腿已起（独立 ClientSessionID）：收流端口=%u PT=%u 后端=%s\n",
                audio->receiver_port(), audio->payload_type(), audio->backend_name());

    std::unique_ptr<scrctl::media::FramePump> video;
    if (with_video) {
        scrctl::media::FramePump::Options vo;
        video = scrctl::media::FramePump::start(*device, vo, err, verbose);
        if (video == nullptr) {
            std::fprintf(stderr, "④ 视频腿起流失败（音频腿照跑）: %s\n", err.c_str());
        } else {
            std::printf("④ 视频腿同时已起\n");
        }
    }

    // ②：把设备按到静音再量。用硬件音量键而不是"暂停播放"，是因为播放器的控制面
    // 会自己收起、而且它的进度条只认拖动不认轻点（§16.1 踩过），音量键没有这些歧义。
    std::unique_ptr<scrctl::hid::Buttons> buttons;
    if (mute) {
        std::string berr;
        buttons = scrctl::hid::Buttons::open(*device, berr, verbose);
        if (buttons == nullptr) {
            std::fprintf(stderr, "② 打不开按键服务: %s（这一臂测不成）\n", berr.c_str());
        } else {
            for (int i = 0; i < 25; ++i) {
                buttons->press(scrctl::hid::button::kUsagePageConsumer,
                               scrctl::hid::button::kVolumeDown, 30, berr);
            }
            std::printf("② 已把设备音量按到零（25 次音量减），测 %d 秒里包还来不来\n",
                        seconds);
        }
    }

    const uint64_t t0 = now_ms();
    scrctl::media::AudioPump::Stats prev {};
    uint64_t last_change_ms = t0;
    uint64_t last_print_ms = t0;
    std::size_t peak = 0;
    std::size_t window_peak = 0;
    // 缓冲按**采样**数，而 `read()` 要的是**帧**数（一帧 = channels 个采样）。
    // 这两个单位混过一次：探针把 scratch.size()（960 个采样）当 960 帧传进去，
    // read() 就往 1920 字节的缓冲里写了 3840 字节——ASan 报在 read 里，而真正
    // 崩的是几百毫秒后另一个线程的一次 malloc。
    std::vector<int16_t> scratch(480 * 2);
    const std::size_t scratch_frames = scratch.size() / 2;

    // stopAll 之后要量的三个时刻：发出的那一刻、包停住的那一刻（就是 last_change_ms
    // 停住不动的那个值）、以及包重新动起来的这一刻。
    bool killed = false;
    uint64_t kill_ms = 0;
    uint64_t back_ms = 0;
    uint64_t packets_at_kill = 0;  ///< 静默坐实那一刻的包数，恢复的基线
    bool saw_dead = false;
    std::unique_ptr<scrctl::media::StreamSession> second_video;
    uint64_t second_video_packets = 0;
    uint64_t second_started_ms = 0;
    bool second_reported = false;
    uint64_t longest_audio_silence_ms = 0;
    uint64_t restarts_at_kill = 0;
    while ((now_ms() - t0) / 1000 < static_cast<uint64_t>(seconds)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        // 照 `FramePump::restart()` 那个形状发一次 stopAll：它停的是设备上**所有**会话，
        // 所以这一发之后音频腿也死了。要量的就是音频自己多久才发现、发现了多久才恢复。
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
            std::printf("!! 第 %d 秒发出 stopAll（模拟视频腿重起）：%s\n", kill_at,
                        ok ? "设备已收" : err.c_str());
        }
        // 直接起第二条视频会话，**不**先停第一条：设备会自己把旧的顶掉。
        if (revideo_at >= 0 && static_cast<int>((now_ms() - t0) / 1000) >= revideo_at &&
            second_video == nullptr) {
            scrctl::media::StreamSession::Request vr;
            std::string verr;
            second_video = scrctl::media::StreamSession::start(*device, vr, verr, verbose);
            std::printf("!! 第 %d 秒直接起第二条视频会话（没有发任何 stop）：%s\n", revideo_at,
                        second_video != nullptr
                            ? ("已起，收流端口=" + std::to_string(second_video->receiver_port())).c_str()
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
                std::printf("   第二条会话 3 秒里收到 %llu 个包\n",
                            static_cast<unsigned long long>(second_video_packets));
            }
        }
        // 把缓冲里的东西取出来只算峰值：不取就会一直堆到 0.5 秒然后开始丢旧帧，
        // 那样"丢旧帧"这一位会被探针自己的不作为污染。
        const std::size_t got = audio->read(scratch.data(), scratch_frames);
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
        // 恢复的读数要在 `last_change_ms` 被推进**之前**算，否则"静默多久"这一位
        // 永远是自己减自己（0）。这类"两个时间戳谁先动"的错只有一种防法：把要报的
        // 那个差值当场打出来看一眼是不是 0。
        // "静默过"必须先成立才算掉线开始：stopAll 那条 RPC 本身要 100~200ms，期间
        // 设备的包还在路上，直接拿"包数比发出前多了"当恢复会读出 56ms 这种荒唐数
        // （第一版就是这么错的，而它差一点被当成"音频根本不在乎会话停没停"的证据）。
        if (killed && !saw_dead && now_ms() - last_change_ms >= 400) {
            saw_dead = true;
            // 基线要在**见过静默之后**取，不能用发出 stopAll 那一刻的包数：那条 RPC
            // 之后设备的包还在路上（实测又多了十几个才彻底停），拿旧基线会在
            // "静默刚坐实"的瞬间就判成"已经恢复了"，把恢复时间读成 0。
            packets_at_kill = st.packets;
        }
        if (killed && saw_dead && back_ms == 0 && st.packets > packets_at_kill) {
            back_ms = now_ms();
            std::printf(">> 音频恢复：stopAll 之后 %llu ms 才重新有包，其中包静默 %llu ms；"
                        "期间重起 %llu 次\n",
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
            window_peak = 0;
            std::printf(
                "+%3llus 包=%llu 解出=%llu 解败=%llu 非音频=%llu 缺口=%llu 真丢=%llu "
                "迟到=%llu RR=%llu/%llu 丢旧=%llu 重起=%llu 缓冲=%zu帧 静默=%llus "
                "本段峰值=%zu\n",
                static_cast<unsigned long long>((now_ms() - t0) / 1000),
                static_cast<unsigned long long>(st.packets),
                static_cast<unsigned long long>(st.decoded),
                static_cast<unsigned long long>(st.decode_failed),
                static_cast<unsigned long long>(st.other_payload),
                static_cast<unsigned long long>(st.seq_gaps),
                static_cast<unsigned long long>(st.seq_lost),
                static_cast<unsigned long long>(st.out_of_order),
                static_cast<unsigned long long>(st.rtcp_sent),
                static_cast<unsigned long long>(st.rtcp_failed),
                static_cast<unsigned long long>(st.dropped_stale),
                static_cast<unsigned long long>(st.restarts), audio->buffered_frames(),
                static_cast<unsigned long long>((now_ms() - last_change_ms) / 1000),
                window_peak);
            if (video != nullptr) {
                const auto vs = video->stats();
                std::printf("        视频：包=%llu AU=%llu 出图=%llu 非视频=%llu 重起=%llu\n",
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
        std::printf("② 已按回音量（25 次音量加）\n");
    }
    // 末段峰值必须一起打：打印窗口是 2 秒而循环到点就退，最后那不到 2 秒的样本
    // 从来不会出现在任何一行"本段峰值"里。只打全程峰值的话，读的人会以为
    // "本段全是 0 而合计不是 0"是自相矛盾（它其实只是最后那一段没打）。
    std::printf("音频最长包静默 %llu ms（第二条视频会话起来之后有没有掉过包，看这一位）\n",
                static_cast<unsigned long long>(longest_audio_silence_ms));
    std::printf("合计：包=%llu 解出=%llu 解败=%llu 峰值=%zu 末段峰值=%zu（满幅 32767）"
                " 重起=%llu\n",
                static_cast<unsigned long long>(st.packets),
                static_cast<unsigned long long>(st.decoded),
                static_cast<unsigned long long>(st.decode_failed), peak, window_peak,
                static_cast<unsigned long long>(st.restarts));
    // ②的判据就是最后那个"静默"：跑完后不主动出声，看它在没有声音的时段里到底还发不发包。
    audio.reset();
    return 0;
}
