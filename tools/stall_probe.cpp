// 探针：PLI 不成就重起那条后备，在"画面冻住、只剩 SR 心跳"时到底会不会到点。
//
// 为什么需要它：这条判据的整个存在理由是"设备对 PLI 没反应时把会话拆了重来"，而那
// 恰好是最难自然撞见的现场——要三件事同时成立（丢了包、IDR 没来、之后只剩每秒一个
// SR）。没有专门档位的话它就只能靠推理相信，而 §20 记的正是这条判据**因为摆放位置**
// 错过过一次：判据内容对、代码走不到，两个月里两次。
//
// 于是给它做了两个"专门制造不一致"的档位（见 FramePump::Options）：
//   * `--drop-at-packet N`：第 N 个视频包我们**不吃**，制造一次真实的序号缺口。设备
//     那边照发不误，所以这是我们单方面知道的一次丢包。
//   * 压住 PLI：一个都不发。于是 IDR 必然不来（这条流不周期发 IDR，实测基线 30 秒
//     才 1 个），唯一还能救场的就是被测的那条后备。
//   两者都开着时 `silence_restart_ms` 被设成 0：那条判据一关，"重起发生了"就只有
//   一个可能的来源，归因是干净的。
//
// 第二个档位 `--ignore-video-after M`：第 M 个视频包之后一个都不吃（SR 照吃）。它模拟的
// 就是"画面静止、只剩心跳"。为什么要在本地模拟而不是去把设备按到静止：那样一来"屏幕上
// 到底还有没有在出帧"就成了实验前提的一部分，而它既难控制也难复现（第一版就是栽在这儿：
// 按了 home 以为画面静止，实际 PiP 还在放，159 包/秒一刻没停）。被测的判据关心的只是
// "泵这一轮有没有拿到可解析的视频字节"，所以直接从这一位切进去，现场与屏幕内容无关。
//
// 还有一件事要现场造：**让画面静止**。静帧时设备不再发视频字节、只发 SR，而那正是
// 旧写法（挂在"解析出了视频字节"之后）永远等不到的东西。做法是在丢包之后按一下 home
// 退回主屏——这一按同时是本探针唯一的"画面内容变了"的时刻，所以判"有没有新帧"用的是
// **解码器又出图了**，不是像素差分（SR 让 `last_packet_ms_` 一直刷新，这里没有别的尺）。
//
// 用法：stall_probe [--stall-ms 4000] [--drop-at-packet 60] [--home-after-ms 700]
//                  [--watch 15000] [-v]
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "decode/Decoder.h"
#include "media/FramePump.h"
#include "remote/Device.h"

namespace {

using clock = std::chrono::steady_clock;

uint64_t now_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch())
            .count());
}

}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int stall_ms = 4000;
    int drop_at = 60;
    int ignore_after = 60;
    int watch_ms = 15000;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-v" || a == "--verbose") {
            verbose = true;
        } else if (a == "--stall-ms" && i + 1 < argc) {
            stall_ms = std::stoi(argv[++i]);
        } else if (a == "--drop-at-packet" && i + 1 < argc) {
            drop_at = std::stoi(argv[++i]);
        } else if (a == "--ignore-video-after" && i + 1 < argc) {
            ignore_after = std::stoi(argv[++i]);
        } else if (a == "--watch" && i + 1 < argc) {
            watch_ms = std::stoi(argv[++i]);
        }
    }

    std::string err;
    auto dev = scrctl::remote::Device::establish({}, err, verbose);
    if (!dev) {
        std::fprintf(stderr, "建立会话失败: %s\n", err.c_str());
        return 1;
    }

    scrctl::media::FramePump::Options o;
    o.stall_restart_ms = stall_ms;
    o.silence_restart_ms = 0;  // 关掉，免得两把尺混在一起说不清是谁救的场
    o.debug_drop_nth_packet = drop_at;
    o.debug_suppress_pli = true;
    o.debug_ignore_video_after = ignore_after;
    auto pump = scrctl::media::FramePump::start(*dev, o, err, verbose);
    if (pump == nullptr) {
        std::fprintf(stderr, "起泵失败: %s\n", err.c_str());
        return 1;
    }
    std::printf("档位在跑：第 %d 个视频包不吃、第 %d 个之后只剩 SR、PLI 一个不发、"
                "静默重起关闭、等满 %d 毫秒就重起\n",
                drop_at, ignore_after, stall_ms);

    const uint64_t t0 = now_ms();
    uint64_t armed_at = 0;
    uint64_t restarted_at = 0;
    uint64_t frames_after_restart = 0;  // 重起之后新解出的帧数（差值，不是采样次数）
    uint64_t decoded_at_restart = 0;
    scrctl::media::FramePump::Stats last {};

    while (now_ms() - t0 < static_cast<uint64_t>(watch_ms)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto st = pump->stats();
        if (armed_at == 0 && st.pli_suppressed > 0) {
            armed_at = now_ms();
            std::printf("  +%-6llu ms  已武装（PLI 被压住 %llu 次，即我们确实走到了要 IDR 那一步）\n",
                        armed_at - t0, static_cast<unsigned long long>(st.pli_suppressed));
        }
        if (restarted_at == 0 && st.stall_restarts > last.stall_restarts) {
            restarted_at = now_ms();
            decoded_at_restart = st.decoded;
            std::printf("  +%-6llu ms  后备重起发生了（stall_restarts=%llu）\n", restarted_at - t0,
                        static_cast<unsigned long long>(st.stall_restarts));
        }
        frames_after_restart =
            restarted_at == 0 ? 0 : st.decoded - decoded_at_restart;
        if (last.sr_packets != st.sr_packets && restarted_at == 0) {
            // 按完 home 之后每一条 SR 都打一位：这一臂的关键前提就是"只剩心跳在走"，
            // 而 `last_packet_ms_` 正是被它刷新到让静默判据永远不响的。
            std::printf("  +%-6llu ms  SR=%llu（心跳还在，画面已经静止）\n", now_ms() - t0,
                        static_cast<unsigned long long>(st.sr_packets));
        }
        last = st;
    }

    const auto st = pump->stats();
    std::printf("\n读账：包=%llu（视频 %llu / SR %llu） 缺口=%llu PLI=%llu(压住 %llu) "
                "解出=%llu 出图=%llu 后备重起=%llu 总重起=%llu\n",
                static_cast<unsigned long long>(st.packets),
                static_cast<unsigned long long>(st.video_packets),
                static_cast<unsigned long long>(st.sr_packets),
                static_cast<unsigned long long>(st.gaps),
                static_cast<unsigned long long>(st.pli_sent),
                static_cast<unsigned long long>(st.pli_suppressed),
                static_cast<unsigned long long>(st.aus),
                static_cast<unsigned long long>(st.decoded),
                static_cast<unsigned long long>(st.stall_restarts),
                static_cast<unsigned long long>(st.restarts));
    if (st.debug_dropped_at_ms == 0) {
        std::printf("判据不成立：一次包都没丢掉（视频包少于 %d 个？），这一臂白跑\n", drop_at);
        return 2;
    }
    if (st.pli_suppressed == 0) {
        std::printf("判据不成立：从没武装过，后备判据根本没被触发条件满足\n");
        return 2;
    }
    if (restarted_at == 0) {
        std::printf("结论：**没到点**。丢包后 %d 毫秒里后备重起一次都没发生——画面就停在这里\n",
                    watch_ms);
        return 1;
    }
    std::printf("结论：到点了。丢包 -> 重起 %llu ms，武装 -> 重起 %llu ms（阈值 %d ms），"
                "重起之后又解出 %llu 帧\n",
                restarted_at - st.debug_dropped_at_ms, restarted_at - armed_at, stall_ms,
                static_cast<unsigned long long>(frames_after_restart));
    return frames_after_restart > 0 ? 0 : 1;
}
