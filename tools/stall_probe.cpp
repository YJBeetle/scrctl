// 在真机视频流上注入丢包和解码失败，检查恢复状态及会话重建。
//
// A：默认测试。丢弃指定视频包，随后忽略视频、继续接收 SR，并禁止 PLI。
//    这用于检查等待关键帧超时后是否重建会话。丢包位置应避开启动 IDR，
//    且丢包后还要收到若干视频包，拆包器才能识别序号缺口。
// B：--fail-decode N。丢包后先允许 PLI，再让 N 个恢复关键帧解码失败。
//    默认在首次注入失败后停止 PLI；--keep-pli 则继续请求关键帧。
//    失败后一秒内应继续丢弃依赖受损参考帧的 AU，不应发布解码帧；
//    之后还要检查会话重建及出帧恢复。
// C：--fail-every-keyframe N。不注入丢包，从启动开始让 N 个关键帧解码失败，
//    检查达到快速重试上限后是否进入降级，并在后续重试中恢复出帧。
//
// 所有测试关闭包静默重建，避免它干扰等待关键帧及解码失败的恢复路径。
// 默认值对应 A；B 不使用 --ignore-video-after，C 不使用丢包相关选项。
// 运行结果：0 通过，1 失败或连接错误，2 无法判断或配置不适用于本测试。
// CLI11 参数解析错误保留其原有退出码。帮助和参数校验不连接设备。
#include <CLI/CLI.hpp>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "app/DeviceConnection.h"
#include "decode/Decoder.h"
#include "i18n/CliLanguage.h"
#include "i18n/Translation.h"
#include "media/FramePump.h"
#include "remote/Device.h"

namespace {

using clock = std::chrono::steady_clock;

uint64_t now_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch())
            .count());
}

/// 注入失败后观察一秒，比较等待关键帧时丢弃的 AU 数与成功解码的帧数。
constexpr uint64_t kDiscriminateWindowMs = 1000;

}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int stall_ms = 4000;
    int drop_at = 700;
    int ignore_after = 900;
    int fail_decode = 0;
    int fail_every = 0;
    int watch_ms = 20000;
    bool verbose = false;
    bool keep_pli = false;
    std::string wifi, serial;
    CLI::App cli{SCRCTL_N_("Test video recovery with packet loss and decode failures")};
    cli.footer(SCRCTL_N_(
        "Injects faults into a real device stream. --help does not connect to a device."));
    cli.set_help_flag("-h,--help", SCRCTL_N_("Show help"));
    cli.add_option("--wifi", wifi, SCRCTL_N_("LAN address; requires an existing pairing record"));
    cli.add_option("-s,--serial", serial, SCRCTL_N_("Device UDID"));
    cli.add_flag("-v,--verbose", verbose, SCRCTL_N_("Verbose protocol logging"));
    cli.add_option("--stall-ms", stall_ms, SCRCTL_N_("Keyframe wait limit in milliseconds"))
        ->check(CLI::PositiveNumber);
    cli.add_option("--drop-at-packet", drop_at, SCRCTL_N_("Drop this video packet; 0 disables"))
        ->check(CLI::NonNegativeNumber);
    cli.add_option("--ignore-video-after", ignore_after, SCRCTL_N_("Ignore later video packets; 0 disables"))
        ->check(CLI::NonNegativeNumber);
    cli.add_option("--fail-decode", fail_decode, SCRCTL_N_("Fail this many recovery keyframes"))
        ->check(CLI::NonNegativeNumber);
    cli.add_option("--fail-every-keyframe", fail_every, SCRCTL_N_("Fail keyframes including startup"))
        ->check(CLI::NonNegativeNumber);
    cli.add_option("--watch", watch_ms, SCRCTL_N_("Observation duration in milliseconds"))
        ->check(CLI::PositiveNumber);
    cli.add_flag("--keep-pli", keep_pli,
                 SCRCTL_N_(
                     "Keep requesting keyframes after injected failures to test repeated no-output recovery"));
    scrctl::i18n::CliLanguage language(cli);
    try {
        cli.parse(argc, argv);
        if (!language.select()) return 2;
    } catch (const CLI::CallForHelp &) {
        if (!language.select()) return 2;
        std::printf("%s", language.help().c_str());
        return 0;
    } catch (const CLI::ParseError &e) {
        if (!language.select()) return 2;
        std::fprintf(stderr, SCRCTL_TR("Invalid arguments: %s\n"), e.what());
        return e.get_exit_code();
    }
    if (keep_pli && (fail_decode == 0 || fail_every > 0)) {
        std::fprintf(stderr, SCRCTL_TR(
            "--keep-pli requires --fail-decode and is incompatible with --fail-every-keyframe\n"));
        return 2;
    }

    // 拆包器只有收到后续视频包才会识别丢包。保留至少 40 个包的位置间隔，
    // 避免在识别缺口之前就忽略了全部视频。C 不丢包，因此不检查这两项。
    if (fail_every == 0) {
        if (fail_decode == 0 && drop_at > 0 && ignore_after > 0 &&
            ignore_after <= drop_at + 40) {
            std::fprintf(stderr,
                         SCRCTL_TR(
                             "--ignore-video-after(%d) must exceed --drop-at-packet(%d) + 40 so later packets can "
                             "reveal the sequence gap\n"),
                         ignore_after, drop_at);
            return 2;
        }
        // 启动 IDR 缺少分片会使整个会话无法解码，不能用于检查已出帧后的恢复。
        // 已观测的最大启动 IDR 约 64KB、每包约 1300B；200 包的下限留有余量。
        if (drop_at > 0 && drop_at < 200) {
            std::fprintf(stderr,
                         SCRCTL_TR(
                             "--drop-at-packet(%d) must be 0 or at least 200 to avoid dropping part of the startup "
                             "IDR\n"),
                         drop_at);
            return 2;
        }
    }
    // C 至少注入四次失败，才能经过三次快速重建并检查降级路径。
    if (fail_every > 0 && fail_every < 4) {
        std::fprintf(stderr,
                     SCRCTL_TR(
                         "--fail-every-keyframe(%d) must be 0 or at least 4 to test fallback after three "
                         "restarts\n"),
                     fail_every);
        return 2;
    }

    std::string err;
    auto dev = scrctl::app::open_device(serial, wifi, err);
    if (!dev) {
        std::fprintf(stderr, SCRCTL_TR("Failed to establish device session: %s\n"), err.c_str());
        return 1;
    }

    scrctl::media::FramePump::Options o;
    o.stall_restart_ms = stall_ms;
    o.silence_restart_ms = 0;  // 排除包静默重建对本测试的影响。
    if (fail_every > 0) {
        // C 从启动开始注入解码失败，不依赖丢包触发恢复状态。
        // 检查启动关键帧重试达到上限后仍会进入降级，并继续尝试恢复。
        o.debug_fail_decode_of_keyframe = fail_every;
        o.debug_fail_any_keyframe = true;
    } else {
        o.debug_drop_nth_packet = drop_at;
        if (fail_decode == 0) {
            // A 忽略后续视频并抑制 PLI，继续接收 SR。
            o.debug_suppress_pli = true;
            o.debug_ignore_video_after = ignore_after;
        } else {
            // B 先允许 PLI 获取恢复关键帧；默认从注入失败开始抑制 PLI。
            // --keep-pli 则保持请求，检查重复失败是否推迟会话重建。
            o.debug_fail_decode_of_keyframe = fail_decode;
            o.debug_suppress_pli_after_fail = !keep_pli;
        }
    }
    auto pump = scrctl::media::FramePump::start(*dev, o, err, verbose);
    if (pump == nullptr) {
        std::fprintf(stderr, SCRCTL_TR("Failed to start video stream: %s\n"), err.c_str());
        return 1;
    }
    if (fail_every > 0) {
        std::printf(SCRCTL_TR(
            "Test: fail the next %d keyframes including startup; no packet loss or silence restart; "
            "observe fallback and recovery\n"),
                    fail_every);
    } else if (fail_decode == 0) {
        std::printf(SCRCTL_TR(
            "Test: drop video packet %d, ignore video after packet %d, suppress PLI and disable "
            "silence restart; keyframe wait limit %d ms\n"),
                    drop_at, ignore_after, stall_ms);
    } else if (keep_pli) {
        std::printf(SCRCTL_TR(
            "Test: drop packet %d, fail %d recovery keyframes, continue PLI; restart deadline %d "
            "ms\n"),
                    drop_at, fail_decode, stall_ms);
    } else {
        std::printf(SCRCTL_TR(
            "Test: drop packet %d, disable silence restart, wait up to %d ms for recovery; fail %d "
            "recovery keyframes and suppress PLI after the first failure\n"),
                    drop_at, stall_ms, fail_decode);
    }

    if (keep_pli)
        std::printf(SCRCTL_TR(
            "PLI remains enabled while recovery keyframes repeatedly fail decoding\n"));

    const uint64_t t0 = now_ms();
    uint64_t armed_at = 0;
    uint64_t fail_snap_at = 0;
    uint64_t snap_awaiting = 0, snap_decoded = 0, snap_suppressed = 0;
    // 记录首次注入失败后一秒内的增量；窗口结束后不再更新。
    bool window_open = false;
    uint64_t win_awaiting = 0, win_decoded = 0;
    uint64_t restarted_at = 0;
    uint64_t frames_after_restart = 0;  // 重建后新增的解码帧数。
    uint64_t decoded_at_restart = 0;
    // C 记录进入降级及恢复出帧的时刻。
    uint64_t unusable_at = 0, usable_again_at = 0;
    uint64_t restarts_at_unusable = 0, decoded_at_unusable = 0;
    bool last_unusable = false;
    scrctl::media::FramePump::Stats last {};

    while (now_ms() - t0 < static_cast<uint64_t>(watch_ms)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto st = pump->stats();
        if (armed_at == 0 && st.pli_suppressed > 0) {
            armed_at = now_ms();
            std::printf(SCRCTL_TR("  +%-6llu ms  Waiting for keyframe: PLI suppressed %llu times\n"),
                        armed_at - t0, static_cast<unsigned long long>(st.pli_suppressed));
        }
        if (restarted_at == 0 && st.stall_restarts > last.stall_restarts) {
            restarted_at = now_ms();
            decoded_at_restart = st.decoded;
            std::printf(SCRCTL_TR(
                "  +%-6llu ms  Keyframe wait triggered restart (stall_restarts=%llu)\n"), restarted_at - t0,
                        static_cast<unsigned long long>(st.stall_restarts));
        }
        frames_after_restart = restarted_at == 0 ? 0 : st.decoded - decoded_at_restart;
        const bool unusable = pump->video_unusable();
        if (unusable && !last_unusable) {
            unusable_at = now_ms();
            restarts_at_unusable = st.restarts;
            decoded_at_unusable = st.decoded;
            std::printf(SCRCTL_TR(
                "  +%-6llu ms  Video entered fallback (restarts=%llu decoded=%llu)\n"), unusable_at - t0,
                        static_cast<unsigned long long>(st.restarts),
                        static_cast<unsigned long long>(st.decoded));
        } else if (!unusable && last_unusable) {
            usable_again_at = now_ms();
            std::printf(SCRCTL_TR("  +%-6llu ms  Video recovered from fallback (restarts=%llu decoded=%llu)\n"),
                        usable_again_at - t0, static_cast<unsigned long long>(st.restarts),
                        static_cast<unsigned long long>(st.decoded));
        }
        last_unusable = unusable;
        if (fail_snap_at == 0) {
            if (st.forced_decode_failures > last.forced_decode_failures) {
                fail_snap_at = now_ms();
                window_open = true;
                snap_awaiting = st.dropped_awaiting_keyframe;
                snap_decoded = st.decoded;
                snap_suppressed = st.pli_suppressed;
                std::printf(SCRCTL_TR(
                    "  +%-6llu ms  Decode failure injected; baseline: dropped AU=%llu decoded=%llu "
                    "suppressed PLI=%llu\n"),
                            fail_snap_at - t0, static_cast<unsigned long long>(snap_awaiting),
                            static_cast<unsigned long long>(snap_decoded),
                            static_cast<unsigned long long>(snap_suppressed));
            }
        } else if (window_open) {
            win_awaiting = st.dropped_awaiting_keyframe - snap_awaiting;
            win_decoded = st.decoded - snap_decoded;
            if (now_ms() - fail_snap_at > kDiscriminateWindowMs) {
                window_open = false;
                std::printf(SCRCTL_TR(
                    "  +%-6llu ms  Failure observation ended (%llu ms): dropped AU +%llu, decoded frames "
                    "+%llu\n"),
                            now_ms() - t0, static_cast<unsigned long long>(kDiscriminateWindowMs),
                            static_cast<unsigned long long>(win_awaiting),
                            static_cast<unsigned long long>(win_decoded));
            }
        }
        if (fail_decode == 0 && last.sr_packets != st.sr_packets && restarted_at == 0) {
            // A 记录每次新增的 SR，确认忽略视频后发送端报告仍在到达。
            std::printf(SCRCTL_TR("  +%-6llu ms  Sender reports received=%llu\n"), now_ms() - t0,
                        static_cast<unsigned long long>(st.sr_packets));
        }
        last = st;
    }

    const auto st = pump->stats();
    std::printf(SCRCTL_TR(
        "\nSummary: packets=%llu (video=%llu SR=%llu) gaps=%llu PLI=%llu (suppressed=%llu) "
        "AU=%llu dropped=%llu decoded=%llu injected failures=%llu stall restarts=%llu total "
        "restarts=%llu\n"),
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
    if (fail_every > 0) {
        // C 先检查是否进入降级，再检查降级后是否恢复。
        // 仅进入降级不足以说明重试路径仍能恢复出帧。
        std::printf(SCRCTL_TR(
            "Startup failure test: fallback at +%llu ms (restarts=%llu decoded=%llu), recovery=%s, "
            "final decoded frames=%llu\n"),
                    unusable_at == 0 ? 0 : unusable_at - t0,
                    static_cast<unsigned long long>(restarts_at_unusable),
                    static_cast<unsigned long long>(decoded_at_unusable),
                    usable_again_at == 0 ? SCRCTL_TR("not observed")
                            : ("+" + std::to_string(usable_again_at - t0) + " ms").c_str(),
                    static_cast<unsigned long long>(st.decoded));
        if (unusable_at == 0) {
            std::printf(SCRCTL_TR("FAIL: video did not enter fallback after reaching the restart limit\n"));
            return 1;
        }
        if (usable_again_at == 0) {
            std::printf(SCRCTL_TR(
                "INCONCLUSIVE: no recovery after fallback during the %s ms observation; fallback "
                "retries are 60 seconds apart, so --watch must allow enough time\n"),
                        std::to_string(watch_ms).c_str());
            return 2;
        }
        std::printf(SCRCTL_TR(
            "Recovery observed: fallback at +%llu ms after %llu restarts; video recovered at +%llu "
            "ms (%llu ms after fallback); final decoded frames=%llu total restarts=%llu\n"),
                    unusable_at - t0, static_cast<unsigned long long>(restarts_at_unusable),
                    usable_again_at - t0, usable_again_at - unusable_at,
                    static_cast<unsigned long long>(st.decoded),
                    static_cast<unsigned long long>(st.restarts));
        return st.decoded > decoded_at_unusable ? 0 : 1;
    }
    if (fail_decode > 0) {
        if (fail_snap_at == 0) {
            std::printf(SCRCTL_TR(
                "INCONCLUSIVE: no recovery keyframe received for decode failure injection; check "
                "whether packet %d belongs to the startup IDR\n"),
                        drop_at);
            return 2;
        }
        std::printf(SCRCTL_TR(
            "After injected failure (%llu ms): dropped AU +%llu, decoded frames +%llu, suppressed "
            "PLI +%llu\n"),
                    static_cast<unsigned long long>(kDiscriminateWindowMs),
                    static_cast<unsigned long long>(win_awaiting),
                    static_cast<unsigned long long>(win_decoded),
                    static_cast<unsigned long long>(st.pli_suppressed - snap_suppressed));
        const bool kept_awaiting = win_awaiting > 0 && win_decoded == 0;
        const bool restarted_after = restarted_at != 0 && restarted_at > fail_snap_at;
        if (win_decoded > 0) {
            std::printf(SCRCTL_TR(
                "FAIL: within %llu ms after injected keyframe failure, %llu frames were published; "
                "recovery state was cleared before successful keyframe decoding\n"),
                        static_cast<unsigned long long>(kDiscriminateWindowMs),
                        static_cast<unsigned long long>(win_decoded));
            return 1;
        }
        if (!kept_awaiting) {
            std::printf(SCRCTL_TR(
                "INCONCLUSIVE: neither dropped AU nor decoded frames increased during the observation "
                "window; no video progress to compare\n"));
            return 2;
        }
        if (!restarted_after) {
            std::printf(SCRCTL_TR("FAIL: recovery state held (%llu AU dropped), but restart %s\n"),
                        static_cast<unsigned long long>(win_awaiting),
                        restarted_at == 0 ? SCRCTL_TR("was not observed")
                                          : SCRCTL_TR("occurred only before the injected failure"));
            return 1;
        }
        if (keep_pli && restarted_at - fail_snap_at > static_cast<uint64_t>(stall_ms) + 1000) {
            std::printf(SCRCTL_TR(
                "FAIL: repeated failed keyframes delayed restart by %llu ms (limit %d ms plus 1000 ms "
                "tolerance)\n"),
                        static_cast<unsigned long long>(restarted_at - fail_snap_at), stall_ms);
            return 1;
        }
        std::printf(SCRCTL_TR(
            "Recovery state held: failure at +%-llu ms, %llu ms window with %llu AU dropped and no "
            "published frames; restart at +%-llu ms (%llu ms after failure, limit %d ms); decoded "
            "frames after restart=%llu\n"),
                    fail_snap_at - t0, static_cast<unsigned long long>(kDiscriminateWindowMs),
                    static_cast<unsigned long long>(win_awaiting), restarted_at - t0,
                    restarted_at - fail_snap_at, stall_ms,
                    static_cast<unsigned long long>(frames_after_restart));
        return frames_after_restart > 0 ? 0 : 1;
    }
    if (st.debug_dropped_at_ms == 0) {
        std::printf(SCRCTL_TR(
            "INCONCLUSIVE: packet loss was not injected; fewer than %d video packets may have "
            "arrived\n"), drop_at);
        return 2;
    }
    if (st.pli_suppressed == 0) {
        std::printf(SCRCTL_TR(
            "INCONCLUSIVE: keyframe waiting was not observed; restart conditions were not reached\n"));
        return 2;
    }
    if (restarted_at == 0) {
        std::printf(SCRCTL_TR("FAIL: no restart observed within %d ms after packet loss\n"),
                    watch_ms);
        return 1;
    }
    std::printf(SCRCTL_TR(
        "Restart observed: %llu ms after packet loss, %llu ms after keyframe waiting began "
        "(limit %d ms); decoded frames after restart=%llu\n"),
                restarted_at - st.debug_dropped_at_ms, restarted_at - armed_at, stall_ms,
                static_cast<unsigned long long>(frames_after_restart));
    return frames_after_restart > 0 ? 0 : 1;
}
