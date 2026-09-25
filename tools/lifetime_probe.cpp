// 探针：一条媒体会话的**寿命**到底是"起流后 20 秒"还是"最后一个画面帧后几秒"。
//
// 为什么这条必须单独量：两种判据的产品处理完全相反。
//   - 若是"画面静止几秒" -> 空闲策略，我们改不了，只能发现死了重起（现在的做法）；
//   - 若是"起流后固定 20 秒" -> 那就是协商参数里明写的 `RTCPTimeoutInterval: 20`
//     在起作用，因为我们从头到尾没回过一条 RTCP。这种是能修的，而且修了以后
//     连"点下去愣一下"的整类卡顿都会消失。
// 上一轮 rtcp_probe 量到的是：画面在 13.1 秒静止，会话在 20.0 秒消失（距最后一个
// 视频包 6.9 秒），而设备自己的 SR 一直每秒发到会话消失为止。"20 秒"这个数太整齐
// 了，不能放过去。
//
// 所以这一轮把内容变化**一直喂着**（交替按音量上/下，音量 HUD 每次按下都浮出淡去，
// 画面必然变化，且不碰任何 App 的内容），看会话还能不能活过 20 秒。
//
// 用法：lifetime_probe [--rounds N] [--max-seconds N] [--quiet] [--verbose]
//   --quiet  只打每次会话的起止与寿命，不打每秒一行
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

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
    int rounds = 3;
    int max_seconds = 70;
    /// 喂画面变化喂到第几秒就停（-1 = 喂满整个观察窗）。加这个开关是为了把"20 秒
    /// 那一刻有没有媒体在发"和"之后静止多久"这两件事分开量：先喂过 20 秒这道门，
    /// 再停手看它什么时候死——死在"停止喂之后若干秒"就是空闲计时器，活得过很久就
    /// 说明那道门是一次性的。
    int feed_until = -1;
    bool quiet = false;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--rounds" && i + 1 < argc) {
            rounds = std::stoi(argv[++i]);
        } else if (a == "--max-seconds" && i + 1 < argc) {
            max_seconds = std::stoi(argv[++i]);
        } else if (a == "--feed-until" && i + 1 < argc) {
            feed_until = std::stoi(argv[++i]);
        } else if (a == "--quiet") {
            quiet = true;
        } else if (a == "-v" || a == "--verbose") {
            verbose = true;
        }
    }

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

    std::printf("设备：%s / iOS %s；每轮起一条流，全程按音量键喂画面变化\n",
                dev->property("ProductType").c_str(), dev->property("OSVersion").c_str());

    std::vector<double> lifetimes;
    std::vector<uint64_t> frames;
    for (int round = 1; round <= rounds; ++round) {
        scrctl::media::StreamSession::Request req;
        auto session = scrctl::media::StreamSession::start(*dev, req, err, verbose);
        if (!session) {
            std::fprintf(stderr, "第 %d 轮起流失败: %s\n", round, err.c_str());
            return 1;
        }
        const uint64_t t0 = now_ms();
        std::printf("\n[轮 %d] 起流于 0 ms（收流端口 %u）\n", round, session->receiver_port());

        std::vector<uint8_t> packet;
        uint16_t peer = 0;
        // 下面这几个都是**相对起流时刻**的秒表读数（`t = now_ms() - t0`），不是绝对
        // 时刻。以前写成 `= t0 + 1000` / `= t0`，于是 `t >= next_press` 和
        // `t >= next_second` 永远不成立：这个工具既不按键喂画面、也不打每秒那一列，
        // 而它交回来的"活了 45 秒"是被当成"证明不是固定 20 秒租期"的证据写进 docs §13
        // 的。一个静默什么都不做的探针，给出的恰恰是对照组数据。
        uint64_t next_second = 1000, next_press = 0, last_video = 0;
        bool was_feeding = true;
        uint64_t video = 0;
        bool up = true;
        double life = -1;
        while (now_ms() - t0 < static_cast<uint64_t>(max_seconds * 1000)) {
            while (session->next_packet(packet, peer, 50, err)) {
                scrctl::rt::PacketInfo info{};
                if (scrctl::rt::parse_rtp_header(packet, info) &&
                    info.payload_type == session->started().payload_type) {
                    ++video;
                    // 和 next_press / next_second 同一个基准：相对起流的秒表。
                    last_video = now_ms() - t0;
                }
            }
            const uint64_t t = now_ms() - t0;
            const bool feeding = feed_until < 0 || t < static_cast<uint64_t>(feed_until) * 1000;
            if (!feeding && was_feeding && !quiet) {
                std::printf("  %5llu ms 停止喂画面变化，从这里开始看它多久死\n", t);
            }
            was_feeding = feeding;
            if (feeding && t >= next_press) {
                next_press = t + 400;
                std::string perr;
                buttons->press(scrctl::hid::button::kUsagePageConsumer,
                               up ? scrctl::hid::button::kVolumeUp
                                  : scrctl::hid::button::kVolumeDown,
                               60, perr);
                up = !up;
            }
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
                if (st == scrctl::media::StreamSession::ServerState::Ended) {
                    life = static_cast<double>(t);
                    break;
                }
            }
        }
        if (life < 0) {
            std::printf("[轮 %d] 活了满 %d 秒没被结束（视频包 %llu 个）\n", round, max_seconds,
                        static_cast<unsigned long long>(video));
        } else {
            std::printf("[轮 %d] 会话在 %5.0f ms 消失，视频包共 %llu 个，距最后一个视频包 "
                        "%.0f ms\n",
                        round, life, static_cast<unsigned long long>(video),
                        life - static_cast<double>(last_video));
            lifetimes.push_back(life);
            frames.push_back(video);
        }
        std::string serr;
        session->stop(*dev, serr, verbose);
        std::this_thread::sleep_for(500ms);
    }

    std::printf("\n==== 寿命 ====\n");
    for (std::size_t i = 0; i < lifetimes.size(); ++i) {
        std::printf("  轮 %zu: %.0f ms（视频包 %llu 个）\n", i + 1, lifetimes[i],
                    static_cast<unsigned long long>(frames[i]));
    }
    std::printf("\n判读：画面全程在动，寿命仍贴着 20 秒 -> 是 `RTCPTimeoutInterval: 20`"
                "在起作用（我们没回 RTCP）；寿命明显超过 20 秒 -> 才是空闲拆流。\n");
    return 0;
}
