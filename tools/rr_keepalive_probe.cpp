// 探针：每秒发一个 RTCP 接收报告（RR），能不能让设备**不**把这条流结束掉。
//
// 为什么单独问这个：之前那轮"RTCP 全都不理"的实验，判据用错了。那次量的是"发完请求
// 6 秒里有没有 IRAP 到达"（见 tools/fir_probe.cpp 的判据），也就是"能不能把帧催出来";
// 而这里要问的是另一件事——"会话会不会被结束"。一个保活机制成功的表现恰恰是**没有新
// 帧也不死**，用"有没有帧"去判它，注定判成失败。
//
// 而同一批实验留下的现象里其实已经写着答案：docs §13 记着"RR 发到视频端口会让投递
// 几乎停""不再有结束事件"。当时把"没有结束事件"和"一个包都不来了"混成一件事读，
// 于是记成了"RR 有害"。这两件事必须分开量：
//
//   会话还活着但画面静止  -> 只有每秒一个 SR，没有视频包，会话表里 running:true
//   会话被结束            -> 连 SR 都停，会话表里 running:false
//
// 判据用**设备自己那一秒一个的 SR** 当存活信号：它是被动到达的，不引入任何额外流量，
// 而且实测它在会话消失的同一秒才停（docs §13 的时间轴）。所以"最后一个视频包之后 SR
// 还在继续来"就是"会话还活着"，不需要为此额外发任何 RPC——那会污染对照。
// 只在整轮结束时问一次会话表，作为独立佐证。
//
// 对照组 `none` 什么都不发。两臂**交替**各跑若干轮：单次对照不算对照（docs §13）。
//
// 用法：rr_keepalive_probe [--seconds N] [--attempts N] [--what none,rr,poll,poll5] [--verbose]
//
// 第二轮加的 `poll` 臂是因为第一批数据把"空闲超时"这个模型打掉了：四臂里最后一个视频
// 包分别落在 +11.1s / +7.1s / +7.1s / +7.1s，而**每一臂都是 +20.0s 整**停止收 SR 并在
// 会话表里消失。锚点是起流时刻，不是"最后一个视频包"。所以这不是"画面静止 6.9 秒就
// 拆流"（那个数来自另一轮"视频 13.1s 停、20.0s 死"的样本，同样落在 20 这个数上，只是
// 当时先入为主当成了间隔）。
//
// 那为什么另一次实验里"画面全程在动"的会话活了 45 秒？那一轮用的是 `rtcp_probe --death`，
// 它**每秒查一次会话表**。如果查状态这个动作本身会把 20 秒的表推后，那 45 秒就不是
// "有媒体可发所以留着"，而是"我们自己一直在续"。`poll` 臂就是专门验这个的：只查状态、
// 不发任何 RTCP。
//
// 存活信号仍然用设备自己每秒一个的 SR（被动、不引入流量）；`poll` 臂会引入 RPC，所以
// 它的对照意义是"这一臂能不能活过 20 秒"，而不是"SR 数说明什么"。
#include <chrono>
#include <cstdio>
#include <map>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "media/StreamSession.h"
#include "remote/Device.h"
#include "rt/RtpHevc.h"

namespace {

using namespace std::chrono_literals;
using clock = std::chrono::steady_clock;

uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count());
}

void put32(std::vector<uint8_t> &v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x >> 24));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}

/// RR（RFC 3550 §6.4.1）：V=2、RC=1、PT=201、长度 7 个字，加发送者 SSRC 与一个 20
/// 字节报告块，一共 28 字节。
std::vector<uint8_t> build_rr(uint32_t our_ssrc, uint32_t media_ssrc, uint32_t ext_high) {
    std::vector<uint8_t> v;
    v.push_back(0x81);  // V=2, RC=1
    v.push_back(201);   // PT = RR
    put32(v, 7);        // 长度以 4 字节为单位，不含这 4 字节头
    put32(v, our_ssrc);
    put32(v, media_ssrc);
    put32(v, 0);              // 分数丢包 1 + 累积丢包 3：这条流我们不重传，报 0
    put32(v, ext_high);       // 扩展最高序号
    put32(v, 0);              // 抖动
    put32(v, 0);              // LSR / DLSR：老实报 0，不假装算过
    put32(v, 0);
    return v;
}

bool is_rtcp_sr(std::span<const uint8_t> p) {
    return p.size() >= 28 && p[0] == 0x81 && p[1] == 0xc8;
}

/// SDES（PT=202）带一个 CNAME。设备的 SR 就是 SR+SDES 的复合包，所以它大概也要求
/// 我们回的是复合包——单独的 RR 可能被当成"不是合法复合包"而丢掉。
std::vector<uint8_t> build_sdes(uint32_t our_ssrc, std::string_view cname) {
    std::vector<uint8_t> body;
    put32(body, our_ssrc);
    body.push_back(1);  // CNAME
    body.push_back(static_cast<uint8_t>(cname.size()));
    body.insert(body.end(), cname.begin(), cname.end());
    body.push_back(0);  // END
    while (body.size() % 4 != 0) {
        body.push_back(0);
    }
    std::vector<uint8_t> v;
    v.push_back(0x81);  // V=2, SC=1
    v.push_back(202);
    put32(v, static_cast<uint32_t>(body.size() / 4));  // 长度（字）
    v.insert(v.end(), body.begin(), body.end());
    return v;
}

struct Arm {
    std::string what;
    bool got_idr = false;
    uint64_t last_video_ms = 0;   // 相对起流的时刻
    uint64_t last_sr_ms = 0;      // 同上
    uint64_t srs_after_video = 0;  // 最后一个视频包之后还收到多少个 SR
    uint64_t polls_alive = 0;     // poll 臂：查会话表答"还在"的次数
    uint64_t last_alive_ms = 0;   // 最后一次答"还在"的时刻
    bool alive_at_end = false;
    std::string note;
};

void print_arm(const Arm &a, uint64_t t0) {
    std::printf("  [%s] IDR=%s 最后视频包 +%llums 最后 SR +%llums 之后 SR 共 %llu 个 "
                "结束时会话表=%s",
                a.what.c_str(), a.got_idr ? "有" : "无",
                static_cast<unsigned long long>(a.last_video_ms > t0 ? a.last_video_ms - t0 : 0),
                static_cast<unsigned long long>(a.last_sr_ms > t0 ? a.last_sr_ms - t0 : 0),
                static_cast<unsigned long long>(a.srs_after_video),
                a.alive_at_end ? "还在" : "已没了");
    if (a.polls_alive != 0) {
        std::printf(" 查会话表 %llu 次答还在，最后一次 +%llums",
                    static_cast<unsigned long long>(a.polls_alive),
                    static_cast<unsigned long long>(a.last_alive_ms > t0 ? a.last_alive_ms - t0 : 0));
    }
    std::printf("%s\n", a.note.empty() ? "" : (" " + a.note).c_str());
}

}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int seconds = 30;
    int attempts = 2;
    std::string what = "none,rrsame,rrsdes,rrp1,rrall";
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--seconds" && i + 1 < argc) {
            seconds = std::stoi(argv[++i]);
        } else if (a == "--attempts" && i + 1 < argc) {
            attempts = std::stoi(argv[++i]);
        } else if (a == "--what" && i + 1 < argc) {
            what = argv[++i];
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

    // 把 --what 按逗号拆开（不用子串匹配：那会让 "poll" 命中 "poll5"）。
    std::vector<std::string> arms_to_run;
    for (std::size_t p = 0; p < what.size();) {
        const auto c = what.find(',', p);
        auto part = what.substr(p, c == std::string::npos ? std::string::npos : c - p);
        while (!part.empty() && part.front() == ' ') {
            part.erase(part.begin());
        }
        if (!part.empty()) {
            arms_to_run.push_back(part);
        }
        p = c == std::string::npos ? what.size() : c + 1;
    }
    std::map<std::string, std::pair<int, int>> tally;  // 臂 -> {活着, 死了}

    for (int round = 0; round < attempts; ++round) {
        for (const std::string &w : arms_to_run) {
            const int poll_every_ms = w == "poll" ? 2000 : (w == "poll5" ? 5000 : 0);
            // RTCP 变体。寿命实验已经钉死"会话是起流后 20 秒的硬租期，喂画面也救不了"，
            // 而协商参数里就写着 `RTCPTimeoutInterval: 20`——也就是说这 20 秒是"没收到
            // 接收端 RTCP"的超时。我们发裸 RR 它照死，所以问题不是"要不要回 RTCP"，
            // 而是"它认的那一份 RTCP 长什么样"。这几个变体各改一个变量：
            //   rr      裸 RR，发送者 SSRC 用我们自己编的，发到设备那个媒体端口
            //   rrsame  同上，但发送者 SSRC = 设备的媒体 SSRC
            //   rrsdes  RR + SDES(CNAME) 复合包（设备的 SR 就是 SR+SDES 复合来的）
            //   rrp1    裸 RR 发到 媒体端口+1（RFC 3550 的 RTCP 端口惯例）
            //   rrall   复合包 + 媒体 SSRC + 端口+1，全都给
            const bool send_rr = w.rfind("rr", 0) == 0;
            const bool sdes = w == "rrsdes" || w == "rrall";
            const bool same_ssrc = w == "rrsame" || w == "rrall";
            const bool port_plus_one = w == "rrp1" || w == "rrall";
            std::string start_err;
            scrctl::media::StreamSession::Request req;
            auto session = scrctl::media::StreamSession::start(*dev, req, start_err, verbose);
            if (!session) {
                std::fprintf(stderr, "[%s] 起流失败: %s\n", w.c_str(), start_err.c_str());
                std::this_thread::sleep_for(2s);
                continue;
            }
            std::printf("第 %d 轮 [%s]：流已起，观察 %d 秒（不去碰设备，让画面自己静止）\n", round,
                        w.c_str(), seconds);

            Arm arm;
            arm.what = w;
            const uint64_t t0 = now_ms();
            std::vector<uint8_t> packet;
            uint16_t peer = 0;
            uint32_t media_ssrc = 0;
            uint16_t highest_seq = 0;
            uint64_t last_video = 0;
            uint64_t next_rr = t0;
            uint64_t next_poll = t0;
            // 我们自己的 SSRC：不能拿设备那个当发送者，否则设备按 SSRC 配对时会认为
            // 这是它自己的报告而丢掉（也可能更糟：把两条流的报告当成同一条）。
            const uint32_t our_ssrc = 0x35c0ffeeu;

            const uint64_t until = t0 + static_cast<uint64_t>(seconds) * 1000;
            while (now_ms() < until) {
                while (session->next_packet(packet, peer, 30, err)) {
                    const uint64_t now = now_ms();
                    scrctl::rt::PacketInfo info {};
                    if (scrctl::rt::parse_rtp_header(packet, info) &&
                        info.payload_type == session->started().payload_type) {
                        media_ssrc = info.ssrc;
                        highest_seq = info.sequence;
                        last_video = now;
                        arm.last_video_ms = now;
                        arm.got_idr = true;
                    } else if (is_rtcp_sr(packet)) {
                        arm.last_sr_ms = now;
                        if (last_video != 0 && now - last_video > 1500) {
                            ++arm.srs_after_video;
                        }
                    }
                }
                if (send_rr && media_ssrc != 0 && now_ms() >= next_rr) {
                    next_rr += 1000;
                    std::string serr;
                    auto rr = build_rr(same_ssrc ? media_ssrc : our_ssrc, media_ssrc,
                                       highest_seq);
                    if (sdes) {
                        const auto sd = build_sdes(same_ssrc ? media_ssrc : our_ssrc, "scr1");
                        rr.insert(rr.end(), sd.begin(), sd.end());
                    }
                    const uint16_t to_port = session->started().sender_port +
                        (port_plus_one ? 1 : 0);
                    if (!session->send_rtp(rr, to_port, serr)) {
                        arm.note = "RTCP 发送失败: " + serr;
                    }
                }
                if (poll_every_ms != 0 && now_ms() >= next_poll) {
                    next_poll += static_cast<uint64_t>(poll_every_ms);
                    std::string qerr;
                    const auto state = scrctl::media::StreamSession::probe(
                        *dev, session->started().session_uuid, qerr, verbose);
                    if (state == scrctl::media::StreamSession::ServerState::Alive) {
                        ++arm.polls_alive;
                        arm.last_alive_ms = now_ms();
                    }
                }
            }

            std::string perr;
            arm.alive_at_end = scrctl::media::StreamSession::probe(
                                   *dev, session->started().session_uuid, perr, verbose) ==
                scrctl::media::StreamSession::ServerState::Alive;
            print_arm(arm, t0);

            if (arm.got_idr) {
                ++tally[w].first;
                if (!arm.alive_at_end) {
                    ++tally[w].second;
                }
            }
            std::string serr;
            session->stop(*dev, serr, verbose);
            std::this_thread::sleep_for(2s);
        }
    }

    std::printf("\n汇总（活到观察结束的轮数 / 有效轮数）：\n");
    for (const auto &[w, t] : tally) {
        std::printf("  %-6s 活着 %d / %d\n", w.c_str(), t.first - t.second, t.first);
    }
    const auto alive_of = [&](const std::string &k) {
        auto it = tally.find(k);
        return it == tally.end() ? 0 : it->second.first - it->second.second;
    };
    const auto total_of = [&](const std::string &k) {
        auto it = tally.find(k);
        return it == tally.end() ? 0 : it->second.first;
    };
    if (total_of("none") > 0 && alive_of("none") == 0) {
        std::printf("对照成立：什么都不发的臂每次都死。\n");
        for (const auto &k : {std::string("rr"), std::string("rrsame"), std::string("rrsdes"),
                       std::string("rrp1"), std::string("rrall"), std::string("poll"),
                       std::string("poll5")}) {
            if (total_of(k) == 0) {
                continue;
            }
            std::printf("  %-6s：%s\n", k.c_str(),
                        alive_of(k) == total_of(k)
                            ? "每次都活到观察结束 —— 这就是保活动作"
                            : (alive_of(k) == 0 ? "每次都死 —— 它不是保活"
                                                : "时活时死 —— 和间隔有关，要量出那个时限"));
        }
    } else {
        std::printf("对照组没死或没有对照，判据不成立。\n");
    }
    return 0;
}
