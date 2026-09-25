// 探针：租期接续那一次"没有新帧"到底看不看得见。
//
// 为什么要有这条：当时以为设备给每条媒体会话的租期是起流后约 20 秒、续不上（docs §13），
// 所以每 20 秒必然要换一次会话，而换会话有约 300ms 拿不到新帧（`two_session_probe` 实测：
// RPC 84ms + 首个 IDR 306ms，而且第二条一起来第一条就从设备表里消失，"先起新的再切"
// 这种无缝交接不成立）。这笔钱非付不可，泵里能做的只有**挑时刻**：画面静止的时候接，
// 用户看不见；画面在动的时候接，是一次看得见的顿挫。
//
// 于是"到点就重起"改成了"到点之后在剩下的 1.4 秒里等一个静止间隙，等不到才硬接"。
// 这个改动对不对，只能实测两种画面下各发生什么：
//
//   --feed（画面一直在动） 接续那次应当是一条 ~300ms 的顿挫，且**顿挫里我们按过音量键**
//                          ——按一下必然带来画面变化，没帧就是真吃掉了内容（这种顿挫
//                          是租期逼出来的，不是策略选的）。
//   不喂（画面静止）       接续仍然发生，但接续之前早就没新帧了：那 300ms 落在一张本来
//                          就停着的画面上，不构成任何可见损失。
//
// 判据全取自泵对外的读数（serial 的间隔 + stats().restarts 的增量），不靠人眼看窗口。
// 把顿挫归到"接续"头上靠的是 restarts 的增量：一次没有新帧的间隔里重起计数涨了，才是
// 换会话造成的；没涨却静默很久，那是画面本来就静止。
//
// 用法：lease_renew_probe [--seconds N] [--feed] [--press-ms N] [--verbose] [UDID]
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "hid/Hid.h"
#include "media/FramePump.h"
#include "remote/Device.h"

namespace {

uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count());
}

/// 一次"没有新帧"的间隔，全部时刻都相对探针起点。
struct Stall {
    uint64_t start_ms = 0;
    uint64_t end_ms = 0;
    /// 这一段里重起了几次：>0 就说明这条顿挫是换会话换来的，而不是别的毛病。
    uint64_t restarts = 0;
    /// 这一段里按了几下音量键（只有 --feed 时才可能非 0）。
    int presses = 0;

    [[nodiscard]] uint64_t len_ms() const { return end_ms - start_ms; }
};

}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string_view udid;
    int seconds = 50;
    bool feed = false;
    int press_every_ms = 400;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "-v" || a == "--verbose") {
            verbose = true;
        } else if (a == "--feed") {
            feed = true;
        } else if (a == "--seconds" && i + 1 < argc) {
            seconds = std::atoi(argv[++i]);
        } else if (a == "--press-ms" && i + 1 < argc) {
            press_every_ms = std::atoi(argv[++i]);
        } else if (a == "-h" || a == "--help") {
            std::printf("用法: %s [--seconds N] [--feed] [--press-ms N] [-v] [UDID]\n", argv[0]);
            return 0;
        } else if (a.starts_with("-")) {
            std::fprintf(stderr, "未知选项 %s\n", std::string(a).c_str());
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
    std::unique_ptr<scrctl::hid::Buttons> buttons;
    if (feed) {
        std::string berr;
        buttons = scrctl::hid::Buttons::open(*device, berr, verbose);
        if (!buttons) {
            std::fprintf(stderr, "开不了按键服务: %s\n", berr.c_str());
            return 1;
        }
    }

    // 泵用默认参数：silence_restart_ms=3000 > 0，租期接续那条分支才是活的。
    scrctl::media::FramePump::Options options;
    auto pump = scrctl::media::FramePump::start(*device, options, err, verbose);
    if (!pump) {
        std::fprintf(stderr, "起流失败: %s\n", err.c_str());
        return 1;
    }
    scrctl::Frame first;
    if (!pump->latest(first, 5000)) {
        std::fprintf(stderr, "没有第一帧\n");
        return 1;
    }

    const uint64_t t0 = now_ms();
    std::printf("泵已起流（收流端口 %u），观察 %d 秒，%s\n", pump->receiver_port(), seconds,
                feed ? "每按一次音量键喂画面变化" : "不喂（让画面静止下来）");

    std::vector<Stall> stalls;
    std::vector<uint64_t> press_at;
    uint64_t last_frame_at = t0;
    uint64_t restarts_at_last_frame = pump->stats().restarts;
    uint64_t prev_serial = pump->serial();
    uint64_t next_press = 0;
    uint64_t next_row = 1000;
    bool up = true;
    scrctl::Frame f;
    while (now_ms() - t0 < static_cast<uint64_t>(seconds) * 1000) {
        const uint64_t got = pump->newer(f, prev_serial, 50);
        const uint64_t t = now_ms() - t0;
        if (feed && t >= next_press) {
            next_press = t + static_cast<uint64_t>(press_every_ms);
            std::string perr;
            buttons->press(scrctl::hid::button::kUsagePageConsumer,
                           up ? scrctl::hid::button::kVolumeUp : scrctl::hid::button::kVolumeDown,
                           60, perr);
            up = !up;
            press_at.push_back(t);
        }
        if (got != 0) {
            const uint64_t now = now_ms();
            // 只有"确实停了一下"才算顿挫：画面在动时两帧之间本来就几十毫秒，50ms 的
            // 轮询粒度下不算异常。阈值 250ms 比一次接续短一些。
            if (now - last_frame_at >= 250) {
                Stall s;
                s.start_ms = (last_frame_at == t0 ? 0 : last_frame_at - t0);
                s.end_ms = t;
                s.restarts = pump->stats().restarts - restarts_at_last_frame;
                for (const uint64_t p : press_at) {
                    if (p >= s.start_ms && p <= s.end_ms) {
                        ++s.presses;
                    }
                }
                stalls.push_back(s);
            }
            prev_serial = got;
            last_frame_at = now;
            restarts_at_last_frame = pump->stats().restarts;
        }
        if (t >= next_row) {
            next_row += 1000;
            const auto st = pump->stats();
            std::printf(
                "  +%3llus 帧号 %6llu 解帧 %6llu 收包 %7llu（视频 %6llu SR %3llu）重起 %llu "
                "距上一帧 %5llums %s\n",
                static_cast<unsigned long long>(t / 1000),
                static_cast<unsigned long long>(prev_serial),
                static_cast<unsigned long long>(st.decoded),
                static_cast<unsigned long long>(st.packets),
                static_cast<unsigned long long>(st.video_packets),
                static_cast<unsigned long long>(st.sr_packets),
                static_cast<unsigned long long>(st.restarts),
                static_cast<unsigned long long>(now_ms() - last_frame_at),
                pump->reviving() ? "救流中" : "");
        }
    }

    const auto st = pump->stats();
    std::printf("\n==== 顿挫（>=250ms 没有新帧）====\n");
    std::printf("观察 %d 秒：重起 %llu 次，按键 %zu 次，顿挫 %zu 条\n", seconds,
                static_cast<unsigned long long>(st.restarts), press_at.size(), stalls.size());
    uint64_t renewal_ms = 0;
    for (const auto &s : stalls) {
        if (s.restarts > 0) {
            renewal_ms += s.len_ms();
        }
        std::printf("  +%5llu -> %5llu ms 长 %5llu ms 其间重起 %llu 次",
                    static_cast<unsigned long long>(s.start_ms),
                    static_cast<unsigned long long>(s.end_ms),
                    static_cast<unsigned long long>(s.len_ms()),
                    static_cast<unsigned long long>(s.restarts));
        if (s.presses > 0) {
            std::printf(" 其间按键 %d 次（画面必然在变）", s.presses);
        }
        std::printf("\n");
    }
    std::printf("\n判读：\n");
    if (feed) {
        std::printf("  画面全程在动（按了 %zu 次键），每一次租期接续都应当是一条 ~300ms 的"
                    "顿挫、且落在按键流里 —— 这种是设备的租期逼出来的，不是策略挑的。\n",
                    press_at.size());
        std::printf("  接续一共吃掉 %llu ms / %d 秒，占 %.2f%%。\n",
                    static_cast<unsigned long long>(renewal_ms), seconds,
                    seconds > 0 ? 100.0 * static_cast<double>(renewal_ms) /
                                      (1000.0 * static_cast<double>(seconds))
                                : 0.0);
    } else {
        std::printf("  没有按键，画面是静止的：静止时设备一个视频包都不发，所以"
                    "「长时间没有新帧」是常态而不是故障。要看的是那些重起是不是都发生了、"
                    "以及每次重起之后帧号还在往前走。\n");
    }
    return 0;
}
