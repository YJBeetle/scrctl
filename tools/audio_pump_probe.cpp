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
// ②这一问要一个专门的臂：`--mute` 先把设备音量按到零（25 次音量减），量 `--seconds`
// 里包还来不来，测完立刻把音量按回去。**当场按回**不是礼貌：不然设备被人拿起来时
// 是静音的，那是我们留给别人的状态而不是实验。
//
// 一次踩坑要记着：这一臂差点拿"有声音时在发 100 包/秒"当成"没声音时也在发"——
// 前者是测出来的，后者压根没测过。所以每一段的**峰值**也要打出来，它才是
// "此刻到底有没有声音"的读数；只有包数会被静音帧继续发这件事骗过去。
//
// 用法：audio_pump_probe [--seconds 30] [--with-video] [--mute] [-v] [UDID]
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "hid/Hid.h"
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
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-v" || a == "--verbose") {
            verbose = true;
        } else if (a == "--with-video") {
            with_video = true;
        } else if (a == "--mute") {
            mute = true;
        } else if ((a == "--seconds" || a == "-s") && i + 1 < argc) {
            seconds = std::atoi(argv[++i]);
        } else if (a == "-h" || a == "--help") {
            std::printf(
                "用法: %s [--seconds 30] [--with-video] [-v] [UDID]\n"
                "  只跑音频腿看 ①②③；带 --with-video 看 ④；带 --mute 先把设备静音再看"
                "  包还来不来（测完按回音量）。全程不开窗口、本机不出声"
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
    while ((now_ms() - t0) / 1000 < static_cast<uint64_t>(seconds)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
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
        if (st.packets != prev.packets || st.decode_failed != prev.decode_failed ||
            st.restarts != prev.restarts || st.rtcp_failed != prev.rtcp_failed) {
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
