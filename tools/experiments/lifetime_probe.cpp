// 测量无 RR 反馈时，显式请求的 RTCP 空闲超时与设备结束会话的时间关系。
//
// --lease 默认 20 秒，不发送 RR；这是一项实验条件，不是设备固定寿命。
// 可在指定时刻停止交替音量键，分别记录媒体活动和会话表状态。
// --max-seconds 是每轮观察预算；RPC 和最终清理仍有各自协议时限。
// 观察预算结束而未确认会话结束时，该轮无寿命样本，返回失败，不能推出另一种到期策略。
#include "ProbeCli.h"
#include <algorithm>
#include <chrono>
#include <limits>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "ProbeWait.h"
#include "hid/Hid.h"
#include "media/StreamSession.h"
#include "remote/Device.h"
#include "rt/RtpHevc.h"

namespace {

using namespace std::chrono_literals;

uint64_t now_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int rounds = 3, max_seconds = 70, feed_until = -1, lease = 20;
    bool quiet = false, verbose = false, dry_run = false;
    CLI::App app("观察无 RR 反馈时的会话结束时间");
    app.add_option("--rounds", rounds, "测量轮数（默认 3）")->transform(scrctl::probe::decimal_integer(1, std::numeric_limits<int>::max()));
    app.add_option("--max-seconds", max_seconds, "每轮自然结束观察预算，秒（默认 70；不含 RPC 与清理时限）")->transform(scrctl::probe::decimal_integer(1, std::numeric_limits<int>::max()));
    app.add_option("--lease", lease, "请求的 RTCP 空闲超时，秒（默认 20，不发送 RR）")->transform(scrctl::probe::decimal_integer(1, std::numeric_limits<int>::max()));
    app.add_option("--feed-until", feed_until, "发送音量键至第几秒；-1 为整个观察期，0 为不发送")->transform(scrctl::probe::decimal_integer(-1, std::numeric_limits<int>::max()));
    app.add_flag("--quiet", quiet, "仅输出每轮结果");
    app.add_flag("-v,--verbose", verbose, "输出协议日志");
    app.add_flag("--dry-run", dry_run, "只显示实验参数，不连接设备");
    app.footer("需独占设备媒体服务：stopmediastream 使用 stopAll，会结束设备上的其它媒体会话。\n观察预算不包含连接、RPC 和停止媒体各自的协议时限。");
    try { app.parse(argc, argv); }
    catch (const CLI::CallForHelp &e) { return app.exit(e); }
    catch (const CLI::ParseError &e) { std::fprintf(stderr, "参数错误：%s\n", e.what()); return 2; }
    const uint64_t observation_ms = static_cast<uint64_t>(max_seconds) * 1000;
    std::printf("实验参数：timeout=%d 秒，RR=off，rounds=%d，observation_ms=%llu，feed_until=%d 秒\n",
                lease, rounds, static_cast<unsigned long long>(observation_ms), feed_until);
    if (dry_run) { return 0; }

    std::string err;
    auto dev = scrctl::remote::Device::establish({}, err, verbose);
    if (!dev) {
        std::fprintf(stderr, "建立会话失败: %s\n", err.c_str());
        return 1;
    }
    std::unique_ptr<scrctl::hid::Buttons> buttons;
    std::string berr;
    buttons = scrctl::hid::Buttons::open(*dev, berr, verbose);
    if (!buttons) {
        std::fprintf(stderr, "开不了按键服务，没法持续制造画面变化: %s\n", berr.c_str());
        return 1;
    }

    std::printf("设备：%s / iOS %s；每轮起一条流，按上述 feed_until 条件发送音量键\n",
                dev->property("ProductType").c_str(), dev->property("OSVersion").c_str());

    std::vector<double> lifetimes;
    std::vector<uint64_t> frames;
    for (int round = 0; round < rounds; ++round) {
        scrctl::media::StreamSession::Request req;
        req.timeout_seconds = static_cast<uint32_t>(lease);
        auto session = scrctl::media::StreamSession::start(*dev, req, err, verbose);
        if (!session) {
            std::fprintf(stderr, "第 %d 轮起流失败: %s\n", round + 1, err.c_str());
            return 1;
        }
        const uint64_t t0 = now_ms();
        std::printf("\n[轮 %d] 起流于 0 ms（收流端口 %u）\n", round + 1, session->receiver_port());

        std::vector<uint8_t> packet;
        uint16_t peer = 0;
        // 调度与统计均使用相对起流时刻，避免绝对时刻和相对时刻混用。
        uint64_t next_second = 1000, next_press = 0, last_video = 0;
        bool was_feeding = true;
        uint64_t video = 0;
        bool up = true;
        double life = -1;
        scrctl::probe::Deadline observation(t0, observation_ms);
        while (!observation.expired(now_ms())) {
            // 每轮最多接收一个数据报。持续来包也不能饿死截止检查、HID 和状态查询。
            if (session->next_packet(packet, peer, static_cast<int>(std::min<uint64_t>(50, observation.remaining(now_ms()))), err)) {
                scrctl::rt::PacketInfo info{};
                if (scrctl::rt::parse_rtp_header(packet, info) &&
                    info.payload_type == session->started().payload_type) {
                    ++video;
                    // 和 next_press / next_second 同一个基准：相对起流的秒表。
                    last_video = now_ms() - t0;
                }
            }
            if (observation.expired(now_ms())) { break; }
            const uint64_t t = now_ms() - t0;
            const bool feeding = feed_until < 0 || t < static_cast<uint64_t>(feed_until) * 1000;
            if (!feeding && was_feeding && !quiet) {
                std::printf("  %5llu ms 停止发送音量键，继续观察媒体活动与会话状态\n", t);
            }
            was_feeding = feeding;
            if (feeding && t >= next_press) {
                next_press = t + 400;
                std::string perr;
                if (!buttons->press(scrctl::hid::button::kUsagePageConsumer,
                                    up ? scrctl::hid::button::kVolumeUp : scrctl::hid::button::kVolumeDown,
                                    60, perr)) {
                    std::fprintf(stderr, "音量键发送失败，不能维持本轮实验条件: %s\n", perr.c_str());
                    std::string cleanup_error;
                    session->stop(*dev, cleanup_error, verbose);
                    return 1;
                }
                up = !up;
            }
            if (observation.expired(now_ms())) { break; }
            if (t >= next_second) {
                next_second += 1000;
                std::string serr;
                const auto st = scrctl::media::StreamSession::probe(
                    *dev, session->started().session_uuid, serr, verbose);
                const bool alive = st == scrctl::media::StreamSession::ServerState::Alive;
                if (!quiet) {
                    std::printf("  %5llu ms 视频包累计 %6llu 距最后视频包 %4llu ms 会话 %s\n", t,
                                static_cast<unsigned long long>(video),
                                static_cast<unsigned long long>(t - last_video),
                                alive ? "在" : (st == scrctl::media::StreamSession::ServerState::Ended
                                                    ? "不在"
                                                    : "问不到"));
                }
                if (!observation.expired(now_ms()) && st == scrctl::media::StreamSession::ServerState::Ended) {
                    life = static_cast<double>(now_ms() - t0);
                    break;
                }
            }
        }
        std::string stop_error;
        const bool stopped = session->stop(*dev, stop_error, verbose);
        if (life < 0) {
            std::fprintf(stderr, "[轮 %d] %d 秒观察预算内未确认会话结束（视频包 %llu 个）；没有寿命样本\n",
                         round + 1, max_seconds, static_cast<unsigned long long>(video));
            if (!stopped) { std::fprintf(stderr, "清理会话失败: %s\n", stop_error.c_str()); }
            return 1;
        }
        if (!stopped) { std::fprintf(stderr, "清理会话失败: %s\n", stop_error.c_str()); return 1; }
        std::printf("[轮 %d] 会话表于 %5.0fms 确认结束，视频包 %llu 个，距最后视频包 %.0fms\n",
                    round + 1, life, static_cast<unsigned long long>(video), life - static_cast<double>(last_video));
        lifetimes.push_back(life);
        frames.push_back(video);
        std::this_thread::sleep_for(500ms);
    }

    std::printf("\n==== 寿命 ====\n");
    for (std::size_t i = 0; i < lifetimes.size(); ++i) {
        std::printf("  轮 %zu: %.0f ms（视频包 %llu 个）\n", i + 1, lifetimes[i],
                    static_cast<unsigned long long>(frames[i]));
    }
    std::printf("\n结果仅描述 timeout=%d 秒且无 RR 的本次配置；与 RR 保活对照比较后才能讨论续期行为。\n", lease);
    return 0;
}
