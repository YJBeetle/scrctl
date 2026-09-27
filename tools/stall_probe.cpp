// 探针：两条"等 IDR 等不到"的恢复判据，各自在真机上到不到点。
//
// 为什么需要它：这两条判据的存在理由都是"设备那边没把 IDR 送回来时把会话拆了重来"，
// 而那恰好是最难自然撞见的现场——要三件事同时成立（丢了包、IDR 没来、之后只剩每秒一个
// SR）。没有专门档位的话它就只能靠推理相信，而 §20 记的正是这条判据**因为摆放位置**
// 错过过一次：判据内容对、代码走不到，两个月里两次。
//
// 两臂（`--fail-decode` 开不开）用同一批档位造出两个不同的现场：
//
// 臂 A「后备重起到不到点」（默认，不带 `--fail-decode`）
//   * `--drop-at-packet N`：第 N 个视频包我们**不吃**，制造一次真实的序号缺口。设备
//     那边照发不误，所以这是我们单方面知道的一次丢包。N 必须落在开头那个 IDR **之后**
//     （默认 700；§20.1 栽过一次：N=12 时缺口打在 IDR 自己身上，每条会话都注定解不出
//     东西，量到的全是"重起—失败—重起"）。
//   * PLI 一个都不发（`debug_suppress_pli`）。于是 IDR 必然不来（这条流不周期发 IDR，
//     实测基线 30 秒才 1 个），唯一还能救场的就是被测的那条后备。
//   * `--ignore-video-after M`：第 M 个之后只吃 SR —— 本地模拟"画面静止"。为什么要在
//     本地模拟而不是把设备按到静止：那样一来"屏幕上还有没有在出帧"就成了实验前提的
//     一部分，既难控制也难复现（第一版就是栽在这儿：按了 home 以为静止，实际 PiP 还在
//     放，159 包/秒一刻没停）。被测的判据关心的只是"这一轮有没有拿到可解析的视频字节"，
//     那就从这一位直接切。
//   * `silence_restart_ms = 0`：那条判据一关，"重起发生了"就只剩一个来源，归因干净。
//
// 臂 B「IDR 到手而解不出时，恢复状态是不是清得太早」（`--fail-decode N`）
//   被测的是清除时机：状态是在"解析器交出一个看起来干净的关键帧 AU"时清，还是在"这个
//   关键帧真的解出来并发布了"之后才清。清早了，泵会以为已经恢复，于是把参考链断掉的
//   P 帧直接发布出去（画面是花的），而且不再要 IDR、后备也永不武装。
//   现场要反过来造：**先真的拿到一个 IDR** 才有东西可以"假装解不出"，所以这一臂
//   不压 PLI（一压就没有 IDR 来当受害者），而是改成"从假装失败那一刻起才压"。顺序是
//   丢包 -> 武装 -> 发一个 PLI -> 设备回一个 IDR（实测 20~35ms）-> 把这个 IDR 判死 ->
//   此后 PLI 全按住。于是"还有没有东西救场"只剩后备一条路。
//   判据取**失败之后第一秒**的两个增量，不看谁重起了：修过的版本 `dropped_awaiting_keyframe`
//   继续涨而 `decoded` 一动不动，没修的版本反过来。这一对读数隔几毫秒就分岔，比"等一次
//   重起"灵敏得多，也不容易被别的路径糊过去（§20.1 的教训：用增量类读数时两臂一模一样，
//   因为这条流上还有别的原因会重新武装）。
//
// 用法：stall_probe [--stall-ms 4000] [--drop-at-packet 700]
//                  [--ignore-video-after 900] [--fail-decode N] [--watch 20000] [-v]
// 默认值是臂 A 的一整套。跑臂 B 要显式给 `--fail-decode 1`，此时 `--ignore-video-after`
// 不参与（那一臂要的就是后面的 AU 继续上门）。
// 退出码：0 判据成立，1 判据不成立（被测代码有问题），2 这一臂白跑（现场没造出来）。
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

/// 假装失败之后要观察多久才够判分岔。一帧 16~33ms，一秒就是 30~60 个 AU，
/// 修过与否在这一秒里的差别是"几十 vs 0"，不是"1 vs 0"。
constexpr uint64_t kDiscriminateWindowMs = 1000;

}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int stall_ms = 4000;
    int drop_at = 700;
    int ignore_after = 900;
    int fail_decode = 0;
    int watch_ms = 20000;
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
        } else if (a == "--fail-decode" && i + 1 < argc) {
            fail_decode = std::stoi(argv[++i]);
        } else if (a == "--watch" && i + 1 < argc) {
            watch_ms = std::stoi(argv[++i]);
        }
    }

    // 两个档位之间有硬约束：**缺口只有在"后面的包到了"之后才看得见**。丢包点之后
    // 一个视频包都不吃，拆包器就永远看不到跳号，`seq_gaps` 不涨，后备判据压根不武装
    // ——第一版默认值正好是 drop=60/ignore=60，于是一次都测不出来（跑出来像"没到点"，
    // 实际是"没发生"）。留 40 个包的下限是给跳号一点到达时间。
    if (fail_decode == 0 && drop_at > 0 && ignore_after > 0 &&
        ignore_after <= drop_at + 40) {
        std::fprintf(stderr,
                     "--ignore-video-after(%d) 必须比 --drop-at-packet(%d)+40 大：\n"
                     "                     丢包点之后还要收若干个包，缺口才看得见\n",
                     ignore_after, drop_at);
        return 2;
    }
    // 丢包点落在开头那个 IDR 里 = 每次会话都注定解不出东西（IDR 自己缺了分片，
    // 后面所有帧的参考链都是断的），那这一臂从头到尾在量的就不是被测判据。IDR 最大
    // 实测 64KB、一包 ~1300B，所以 200 包这个下限留了三倍的余量。
    if (drop_at > 0 && drop_at < 200) {
        std::fprintf(stderr,
                     "--drop-at-packet(%d) 太小：要落在会话开头那个 IDR 之后（>=200），\n"
                     "                     否则缺口打在 IDR 自己身上，量到的全是重起循环\n",
                     drop_at);
        return 2;
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
    if (fail_decode == 0) {
        // 臂 A：IDR 永远不来 + 只剩心跳。
        o.debug_suppress_pli = true;
        o.debug_ignore_video_after = ignore_after;
    } else {
        // 臂 B：PLI 照发（不然没有 IDR 可以判死），视频照吃（要后面的 AU 继续上门才有
        // 东西可数），但从假装失败那一刻起把 PLI 掐断。
        o.debug_fail_decode_of_keyframe = fail_decode;
        o.debug_suppress_pli_after_fail = true;
    }
    auto pump = scrctl::media::FramePump::start(*dev, o, err, verbose);
    if (pump == nullptr) {
        std::fprintf(stderr, "起泵失败: %s\n", err.c_str());
        return 1;
    }
    if (fail_decode == 0) {
        std::printf("档位在跑：第 %d 个视频包不吃、第 %d 个之后只剩 SR、PLI 一个不发、"
                    "静默重起关闭、等满 %d 毫秒就重起\n",
                    drop_at, ignore_after, stall_ms);
    } else {
        std::printf("档位在跑：第 %d 个视频包不吃、静默重起关闭、等满 %d 毫秒就重起；"
                    "PLI 照发直到把接下来 %d 个\"来修丢包的 IDR\"判死，此后 PLI 全按住\n",
                    drop_at, stall_ms, fail_decode);
    }

    const uint64_t t0 = now_ms();
    uint64_t armed_at = 0;
    uint64_t fail_snap_at = 0;
    uint64_t snap_awaiting = 0, snap_decoded = 0, snap_suppressed = 0;
    // 假装失败之后那段窗口里的增量。窗口一关就定死读数，别让后面的事件再改它。
    bool window_open = false;
    uint64_t win_awaiting = 0, win_decoded = 0;
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
        frames_after_restart = restarted_at == 0 ? 0 : st.decoded - decoded_at_restart;
        if (fail_snap_at == 0) {
            if (st.forced_decode_failures > last.forced_decode_failures) {
                fail_snap_at = now_ms();
                window_open = true;
                snap_awaiting = st.dropped_awaiting_keyframe;
                snap_decoded = st.decoded;
                snap_suppressed = st.pli_suppressed;
                std::printf("  +%-6llu ms  假装失败发生了：此后只看增量（此前挡下 %llu 个 AU、解出 "
                            "%llu 帧、按住 PLI %llu 次）\n",
                            fail_snap_at - t0, static_cast<unsigned long long>(snap_awaiting),
                            static_cast<unsigned long long>(snap_decoded),
                            static_cast<unsigned long long>(snap_suppressed));
            }
        } else if (window_open) {
            win_awaiting = st.dropped_awaiting_keyframe - snap_awaiting;
            win_decoded = st.decoded - snap_decoded;
            if (now_ms() - fail_snap_at > kDiscriminateWindowMs) {
                window_open = false;
                std::printf("  +%-6llu ms  判据窗口关掉（%llu 毫秒）：挡下 AU +%llu、解出帧 +%llu\n",
                            now_ms() - t0, static_cast<unsigned long long>(kDiscriminateWindowMs),
                            static_cast<unsigned long long>(win_awaiting),
                            static_cast<unsigned long long>(win_decoded));
            }
        }
        if (fail_decode == 0 && last.sr_packets != st.sr_packets && restarted_at == 0) {
            // 臂 A 里每一条 SR 都打一位：那一臂的关键前提就是"只剩心跳在走"，而
            // `last_packet_ms_` 正是被它刷新到让静默判据永远不响的。
            std::printf("  +%-6llu ms  SR=%llu（心跳还在，画面已经静止）\n", now_ms() - t0,
                        static_cast<unsigned long long>(st.sr_packets));
        }
        last = st;
    }

    const auto st = pump->stats();
    std::printf("\n读账：包=%llu（视频 %llu / SR %llu） 缺口=%llu PLI=%llu(按住 %llu) AU=%llu "
                "挡下=%llu 解出=%llu 判死=%llu 后备重起=%llu 总重起=%llu\n",
                static_cast<unsigned long long>(st.packets),
                static_cast<unsigned long long>(st.video_packets),
                static_cast<unsigned long long>(st.sr_packets),
                static_cast<unsigned long long>(st.gaps),
                static_cast<unsigned long long>(st.pli_sent),
                static_cast<unsigned long long>(st.pli_suppressed),
                static_cast<unsigned long long>(st.aus),
                static_cast<unsigned long long>(st.dropped_awaiting_keyframe),
                static_cast<unsigned long long>(st.decoded),
                static_cast<unsigned long long>(st.forced_decode_failures),
                static_cast<unsigned long long>(st.stall_restarts),
                static_cast<unsigned long long>(st.restarts));
    if (fail_decode > 0) {
        if (fail_snap_at == 0) {
            std::printf("判据不成立：那次假装失败根本没发生（丢包之后没等到一个干净的 IDR）\n"
                        "            检查 %d 这个丢包点是否落在会话开头的 IDR 之内\n",
                        drop_at);
            return 2;
        }
        std::printf("判据窗口读数：假装失败之后 %llu 毫秒里 挡下 AU +%llu、解出帧 +%llu、"
                    "按住 PLI +%llu\n",
                    static_cast<unsigned long long>(kDiscriminateWindowMs),
                    static_cast<unsigned long long>(win_awaiting),
                    static_cast<unsigned long long>(win_decoded),
                    static_cast<unsigned long long>(st.pli_suppressed - snap_suppressed));
        const bool kept_awaiting = win_awaiting > 0 && win_decoded == 0;
        const bool restarted_after = restarted_at != 0 && restarted_at > fail_snap_at;
        if (win_decoded > 0) {
            std::printf("P1 判据：**没修**。假装失败之后 %llu 毫秒里就发布了 %llu 帧——而这一串"
                        "帧的参考链是断的（上一个 IDR 被判死了），恢复状态在解码成功之前"
                        "就被清掉了\n",
                        static_cast<unsigned long long>(kDiscriminateWindowMs),
                        static_cast<unsigned long long>(win_decoded));
            return 1;
        }
        if (!kept_awaiting) {
            std::printf("判据不成立：窗口里两个增量都是 0，说明这一秒根本没有 AU 送上门\n"
                        "            （视频路被饿死了？这一臂不该带 --ignore-video-after）\n");
            return 2;
        }
        if (!restarted_after) {
            std::printf("P1 判据：状态没提前清（挡下了 %llu 个 AU），但后备重起 %s —— 画面对了、"
                        "救场的那一步没跟上，再看一眼 stalled 那段的三个条件\n",
                        static_cast<unsigned long long>(win_awaiting),
                        restarted_at == 0 ? "一次都没发生" : "只发生在失败之前");
            return 1;
        }
        std::printf("P1 判据：修过的。假装失败 +%-llu ms -> 此后 %llu 毫秒挡下 %llu 个 AU、发布 0 "
                    "帧（仍在等干净关键帧）-> 后备重起 +%-llu ms（失败之后 %llu ms，阈值 %d）-> "
                    "又解出 %llu 帧\n",
                    fail_snap_at - t0, static_cast<unsigned long long>(kDiscriminateWindowMs),
                    static_cast<unsigned long long>(win_awaiting), restarted_at - t0,
                    restarted_at - fail_snap_at, stall_ms,
                    static_cast<unsigned long long>(frames_after_restart));
        return frames_after_restart > 0 ? 0 : 1;
    }
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
