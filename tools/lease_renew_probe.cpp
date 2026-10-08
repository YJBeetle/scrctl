// 观察生产 FramePump 的无新帧间隔，以及这些间隔是否与会话重建重叠。
//
// 产品请求 20 秒 RTCP 空闲超时，并每秒发送 RR 续期；不能预设每 20 秒一定重建。
// 本工具保留正常 RR 和自动恢复。--feed 交替发送音量键，用于提供可观察的显示变化。
// 不发送音量键时，长时间没有新视频帧可能只是画面静止，不能仅凭帧间隔判定断流。
// 重建计数增量只能说明间隔中发生过重建，不能把整个间隔都归因于重建或声称丢失同样时长的内容。
#include "ProbeCli.h"
#include <algorithm>
#include <limits>
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
    /// 该无新帧间隔中发生的重建次数；仅用于相关性记录，不等同于故障成因。
    uint64_t restarts = 0;
    /// 这一段里按了几下音量键（只有 --feed 时才可能非 0）。
    int presses = 0;

    [[nodiscard]] uint64_t len_ms() const { return end_ms - start_ms; }
};

}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string udid;
    int seconds = 50, press_every_ms = 400;
    bool feed = false, verbose = false, dry_run = false;
    CLI::App app("观察正常 RR 保活下的新帧间隔与会话重建");
    app.add_option("UDID", udid, "设备 UDID");
    app.add_option("--seconds", seconds, "观察秒数（默认 50）")->transform(scrctl::probe::decimal_integer(1, std::numeric_limits<int>::max()));
    app.add_option("--press-ms", press_every_ms, "音量键发送间隔，毫秒（默认 400）")->transform(scrctl::probe::decimal_integer(1, std::numeric_limits<int>::max()));
    app.add_flag("--feed", feed, "交替发送音量键，观察显示变化");
    app.add_flag("-v,--verbose", verbose, "输出协议日志");
    app.add_flag("--dry-run", dry_run, "只显示实验参数，不连接设备");
    app.footer("需独占设备媒体服务：stopmediastream 使用 stopAll，会结束设备上的其它媒体会话。\n观察预算不包含连接、RPC 和停止媒体各自的协议时限。");
    try { app.parse(argc, argv); }
    catch (const CLI::CallForHelp &e) { return app.exit(e); }
    catch (const CLI::ParseError &e) { std::fprintf(stderr, "参数错误：%s\n", e.what()); return 2; }
    const uint64_t observation_ms = static_cast<uint64_t>(seconds) * 1000;
    std::printf("实验参数：timeout=20 秒，RR=on（1Hz），observation_ms=%llu，feed=%s，press_ms=%d\n",
                static_cast<unsigned long long>(observation_ms), feed ? "on" : "off", press_every_ms);
    if (dry_run) { return 0; }

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

    // 使用生产默认参数：周期 RR 和静默/关键帧恢复保持启用。
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
                feed ? "交替发送音量键" : "不发送音量键");

    std::vector<Stall> stalls;
    std::vector<uint64_t> press_at;
    uint64_t last_frame_at = t0;
    uint64_t restarts_at_last_frame = pump->stats().restarts;
    uint64_t prev_serial = pump->serial();
    uint64_t next_press = 0;
    uint64_t next_row = 1000;
    bool up = true;
    scrctl::Frame f;
    while (now_ms() - t0 < observation_ms) {
        const uint64_t got = pump->newer(f, prev_serial, 50);
        const uint64_t t = now_ms() - t0;
        if (feed && t >= next_press) {
            next_press = t + static_cast<uint64_t>(press_every_ms);
            std::string perr;
            if (!buttons->press(scrctl::hid::button::kUsagePageConsumer,
                                up ? scrctl::hid::button::kVolumeUp : scrctl::hid::button::kVolumeDown,
                                60, perr)) {
                std::fprintf(stderr, "音量键发送失败，不能维持实验条件: %s\n", perr.c_str());
                return 1;
            }
            up = !up;
            press_at.push_back(t);
        }
        if (got != 0) {
            const uint64_t now = now_ms();
            // 记录至少 250ms 的无新帧间隔；阈值是本探针的统计条件。
            if (now - last_frame_at >= 250) {
                Stall s;
                s.start_ms = (last_frame_at == t0 ? 0 : last_frame_at - t0);
                s.end_ms = now - t0;
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
    uint64_t restart_overlap_ms = 0;
    for (const auto &s : stalls) {
        if (s.restarts > 0) {
            restart_overlap_ms += s.len_ms();
        }
        std::printf("  +%5llu -> %5llu ms 长 %5llu ms 其间重起 %llu 次",
                    static_cast<unsigned long long>(s.start_ms),
                    static_cast<unsigned long long>(s.end_ms),
                    static_cast<unsigned long long>(s.len_ms()),
                    static_cast<unsigned long long>(s.restarts));
        if (s.presses > 0) {
            std::printf(" 其间成功发送音量键 %d 次", s.presses);
        }
        std::printf("\n");
    }
    std::printf("RR 发送成功 %llu 次，失败 %llu 次\n", static_cast<unsigned long long>(st.rtcp_sent),
                static_cast<unsigned long long>(st.rtcp_failed));
    std::printf("与重建重叠的已闭合无新帧间隔共 %llums / %d 秒，占 %.2f%%；这不是实际丢帧时长。\n",
                static_cast<unsigned long long>(restart_overlap_ms), seconds,
                100.0 * static_cast<double>(restart_overlap_ms) / static_cast<double>(observation_ms));
    if (now_ms() - last_frame_at >= 250) {
        std::printf("观察结束时仍有一个未闭合的新帧间隔，持续 %llums；未计入上面的已闭合间隔。\n",
                    static_cast<unsigned long long>(now_ms() - last_frame_at));
    }
    if (st.restarts == 0) {
        std::printf("本次未观察到会话重建；正常 RR 保活不要求每 20 秒更换会话。\n");
    }
    std::printf("音量键共成功发送 %zu 次；显示变化和触摸结果仍需结合画面观察。\n", press_at.size());
    return 0;
}
