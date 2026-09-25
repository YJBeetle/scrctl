// 探针：静止画面上主动请设备给一个 IDR，它给不给。
//
// 为什么当时问这个：那一轮寿命实验报出的是"画面全程有变化时会话活了 45 秒，静止 6.9
// 秒就被结束"，于是推断设备那个空闲计时器盯的是**它自己有没有媒体可发**。
//
// **这两个数后来都被推翻了**：那个"45 秒"来自一个静默什么都不做的坏探针（秒表基准写错，
// 既不按键喂画面也不打每秒那一列，见 lifetime_probe 里的说明），而 6.9 秒是拿一个样本
// 当间隔——同一个数据里另一次是视频 7.07 秒停、会话仍在 20.0 秒消失。现在的模型是**起
// 流后约 20 秒的硬租期**，喂画面、回 RTCP 都不能延长（docs §13）。这个改动之后本探针
// 交给它的结论已经不作数了，探针本身留着：FIR/NACK 设备理不理这件事与租期模型无关。
// 那么能喂活这条流的办法就是让静止画面上也产出帧，而这件事标准协议里有现成的请求：
//
//   PLI  (RFC 4585 §6.3.1, PT=206 FCI 类型 1) —— "参考画面坏了，给个关键帧"。
//        已经试过，设备不理（docs §13）。
//   FIR  (RFC 5104 §4.3,   PT=206 FCI 类型 4) —— "强制立刻发一个 IDR"。
//        **没试过。** 和 PLI 的区别不是措辞：FIR 带序列号、要求发送端必须响应，
//        而 PLI 允许发送端自己判断。之前只试了 PLI 就下结论"关键帧请求这条路不通"，
//        是试了一个而漏了另一个。
//   NACK (RFC 4585 §6.2.1, PT=205 FCI 类型 1) —— 重传指定包。没试过，顺带一起看。
//   RR   (RFC 3550 §6.4.1) —— 接收报告。上次 A/B 里"发到视频端口就不再有结束事件"
//        这个现象一直没解释，一并复测一遍（这次的包每个字段都算过字节数，见 build_rr）。
//
// 判据：静默到点之后发一次请求，观察 6 秒里有没有 IRAP（NAL type 19/20/21）到达。
// 对照组 `none` 什么都不发，用来证明"静止画面上本来一个包都不会来"——少了这条，
// "发了就有帧"这个结论不成立（docs §13 里第一版 PLI 实验就是栽在这）。
//
// 用法：fir_probe [--what none,pli,fir,nack,rr] [--attempts N] [--verbose]
#include <chrono>
#include <cstdio>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "hid/Hid.h"
#include "media/StreamSession.h"
#include "remote/Device.h"
#include "rt/RtpHevc.h"

namespace {

using namespace std::chrono_literals;
using clock = std::chrono::steady_clock;

uint64_t now_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void put32(std::vector<uint8_t> &v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x >> 24));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}

/// RTCP 公共头：V=2、PT、**16 位**长度字段（本包总字数 - 1）。
/// 这里原先把长度写成了 32 位，整包从第三个字起错位两字节——"设备不理 PLI"
/// 那个旧结论就是拿那种包测出来的。
void rtcp_header(std::vector<uint8_t> &v, uint8_t rc, uint8_t pt, uint16_t words_after_header) {
    v.push_back(static_cast<uint8_t>(0x80 | (rc & 0x1F)));
    v.push_back(pt);
    v.push_back(static_cast<uint8_t>(words_after_header >> 8));
    v.push_back(static_cast<uint8_t>(words_after_header & 0xFF));
}

/// PLI（RFC 4585 §6.3.1）：头 + sender SSRC + media SSRC，共 12 字节 = 3 字。
std::vector<uint8_t> build_pli(uint32_t sender, uint32_t media) {
    std::vector<uint8_t> v;
    rtcp_header(v, 1, 206, 2);
    put32(v, sender);
    put32(v, media);
    return v;
}

/// FIR（RFC 5104 §4.3）：FCI 类型放在 RC 位（=4），头 + sender + media + 序列号，
/// "对所有源"时不再带 per-SSRC 条目，所以整包 16 字节 = 4 字，长度字段 = 3。
std::vector<uint8_t> build_fir(uint32_t sender, uint32_t media, uint16_t seq) {
    std::vector<uint8_t> v;
    rtcp_header(v, 4, 206, 3);
    put32(v, sender);
    put32(v, media);
    put32(v, seq);
    return v;
}

/// NACK（RFC 4585 §6.2.1）：FCI 类型 1，一个 FCI 项 = PID(16)+BLP(16)。
std::vector<uint8_t> build_nack(uint32_t sender, uint32_t media, uint16_t pid, uint16_t blp) {
    std::vector<uint8_t> v;
    rtcp_header(v, 1, 205, 3);
    put32(v, sender);
    put32(v, media);
    v.push_back(static_cast<uint8_t>(pid >> 8));
    v.push_back(static_cast<uint8_t>(pid));
    v.push_back(static_cast<uint8_t>(blp >> 8));
    v.push_back(static_cast<uint8_t>(blp));
    return v;
}

/// RR（RFC 3550 §6.4.1）：头 + sender + media + 一个 20 字节报告块 = 32 字节 = 8 字，
/// 长度字段 = 7。上次那个包到底是不是这个字节数已经查不到了（代码没提交），
/// 所以这次把长度当场打出来核对。
std::vector<uint8_t> build_rr(uint32_t sender, uint32_t media, uint32_t ext_high) {
    std::vector<uint8_t> v;
    rtcp_header(v, 1, 201, 7);
    put32(v, sender);
    put32(v, media);
    // 报告块：分数丢包 1 + 累积丢包 3 + 扩展最高序号 4 + 抖动 4 + LSR 4 + DLSR 4。
    v.push_back(0);
    v.push_back(0);
    v.push_back(0);
    v.push_back(0);
    put32(v, ext_high);
    put32(v, 0);
    put32(v, 0);  // 没收到过带 NTP 的 SR 之外的东西，LSR/DLSR 老实报 0
    put32(v, 0);
    return v;
}

/// 从 Annex-B 串里数出 IRAP（NAL type 19/20/21 = IDR/CRA/BLI）。
std::set<int> irap_in(const std::vector<uint8_t> &annexb) {
    std::set<int> found;
    for (std::size_t i = 0; i + 5 < annexb.size(); ++i) {
        if (annexb[i] == 0 && annexb[i + 1] == 0 && annexb[i + 2] == 0 && annexb[i + 3] == 1) {
            const int type = (annexb[i + 4] >> 1) & 0x3F;
            if (type >= 19 && type <= 21) {
                found.insert(type);
            }
        }
    }
    return found;
}

struct Attempt {
    std::string what;
    uint64_t packets = 0;
    std::set<int> irap;
    bool ended = false;
};

void hex(std::span<const uint8_t> b) {
    for (uint8_t x : b) {
        std::printf("%02x", x);
    }
}

}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string what = "none,pli,pli1,fir,nack,rr";
    int attempts = 1;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--what" && i + 1 < argc) {
            what = argv[++i];
        } else if (a == "--attempts" && i + 1 < argc) {
            attempts = std::stoi(argv[++i]);
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
    std::vector<Attempt> results;

    for (int round = 0; round < attempts; ++round) {
        for (const std::string w : {std::string("none"), std::string("pli"), std::string("pli1"),
                                    std::string("fir"), std::string("nack"), std::string("rr")}) {
            if (what.find(w) == std::string::npos) {
                continue;
            }
            scrctl::media::StreamSession::Request req;
            std::string start_err;  // 单独一个串：复用 err 会把上一次读包超时的话当成失败原因
            auto session = scrctl::media::StreamSession::start(*dev, req, start_err, verbose);
            if (!session) {
                std::fprintf(stderr, "[%s] 起流失败: %s\n", w.c_str(), start_err.c_str());
                std::this_thread::sleep_for(2s);
                continue;
            }
            Attempt at;
            at.what = w;
            scrctl::rt::HevcRtpDepacketizer dp(session->started().payload_type);
            std::vector<uint8_t> packet, annexb;
            uint16_t peer = 0;
            uint32_t media_ssrc = 0;
            uint16_t highest_seq = 0;
            uint64_t last_video = now_ms();
            bool got_idr = false;

            // 阶段 1：等到起流自带的那个 IDR，确认这条流是好的。
            for (auto until = clock::now() + 6s; clock::now() < until; std::this_thread::sleep_for(5ms)) {
                while (session->next_packet(packet, peer, 30, err)) {
                    scrctl::rt::PacketInfo info{};
                    if (!scrctl::rt::parse_rtp_header(packet, info) ||
                        info.payload_type != session->started().payload_type) {
                        continue;
                    }
                    media_ssrc = info.ssrc;
                    highest_seq = info.sequence;
                    last_video = now_ms();
                    std::ignore = dp.push(packet, annexb, err);
                    if (!irap_in(annexb).empty()) {
                        got_idr = true;
                    }
                }
                if (got_idr) {
                    break;
                }
            }
            if (!got_idr) {
                std::printf("[%s] 没等到起流 IDR，判据不成立，跳过\n", w.c_str());
                std::string serr;
                session->stop(*dev, serr, verbose);
                continue;
            }
            std::printf("\n[%s] 已拿到起流 IDR（媒体 SSRC=%08x），等画面静止…\n", w.c_str(),
                        media_ssrc);

            // 阶段 2：等到静默（不碰设备，画面自然停下来）。最多等 20 秒；
            // 要是屏幕本来就在动，等不到静默就跳过这一轮。
            const uint64_t quiet_deadline = now_ms() + 20000;
            while (now_ms() < quiet_deadline && now_ms() - last_video < 2500) {
                while (session->next_packet(packet, peer, 200, err)) {
                    scrctl::rt::PacketInfo info{};
                    if (scrctl::rt::parse_rtp_header(packet, info) &&
                        info.payload_type == session->started().payload_type) {
                        media_ssrc = info.ssrc;
                        highest_seq = info.sequence;
                        last_video = now_ms();
                    }
                }
            }
            if (now_ms() - last_video < 2500) {
                std::printf("[%s] 20 秒内画面一直没静止，判据不成立，跳过\n", w.c_str());
                std::string serr;
                session->stop(*dev, serr, verbose);
                continue;
            }
            std::printf("[%s] 已静默 %llu ms，发请求\n", w.c_str(),
                        static_cast<unsigned long long>(now_ms() - last_video));

            // 阶段 3：发一次请求，观察 4 秒。
            uint16_t fir_seq = 1;
            uint64_t next_send = now_ms();
            const uint64_t t0 = now_ms();
            while (now_ms() - t0 < 6000) {
                if (w != "none" && now_ms() >= next_send) {
                    // pli1 只发一次，用来分清"一发就够"和"得反复催"。
                    next_send += (w == "pli1" ? 99000 : 1000);
                    std::vector<uint8_t> msg;
                    if (w == "pli" || w == "pli1") {
                        msg = build_pli(media_ssrc, media_ssrc);
                    } else if (w == "fir") {
                        msg = build_fir(media_ssrc, media_ssrc, fir_seq++);
                    } else if (w == "nack") {
                        msg = build_nack(media_ssrc, media_ssrc, static_cast<uint16_t>(highest_seq + 1), 0);
                    } else {
                        msg = build_rr(media_ssrc, media_ssrc, highest_seq);
                    }
                    std::string serr;
                    const bool ok = session->send_rtp(msg, session->started().sender_port, serr);
                    std::printf("        %4llu ms 发 %s %zu 字节: ",
                                static_cast<unsigned long long>(now_ms() - t0), w.c_str(),
                                msg.size());
                    hex(msg);
                    std::printf(" -> %s\n", ok ? "已发" : serr.c_str());
                }
                while (session->next_packet(packet, peer, 50, err)) {
                    scrctl::rt::PacketInfo info{};
                    if (!scrctl::rt::parse_rtp_header(packet, info) ||
                        info.payload_type != session->started().payload_type) {
                        continue;
                    }
                    ++at.packets;
                    media_ssrc = info.ssrc;
                    highest_seq = info.sequence;
                    std::ignore = dp.push(packet, annexb, err);
                    for (int t : irap_in(annexb)) {
                        at.irap.insert(t);
                    }
                }
            }
            std::printf("[%s] 观察 6 秒：视频包 %llu 个，IRAP:", w.c_str(),
                        static_cast<unsigned long long>(at.packets));
            for (int t : at.irap) {
                std::printf(" %d", t);
            }
            std::printf("\n");
            std::string serr;
            session->stop(*dev, serr, verbose);
            results.push_back(at);
        }
    }

    std::printf("\n==== 汇总（静默 2.5 秒后发请求，观察 6 秒）====\n");
    std::printf("%-6s %-10s %s\n", "请求", "视频包", "到的 IRAP");
    for (const auto &r : results) {
        std::printf("%-6s %-10llu", r.what.c_str(), static_cast<unsigned long long>(r.packets));
        if (r.irap.empty()) {
            std::printf(" -");
        }
        for (int t : r.irap) {
            std::printf(" %d", t);
        }
        std::printf("\n");
    }
    std::printf("\n判读：none 那行必须是 0 包（否则画面没静止，判据不成立）；"
                "某行同时给出包和 IRAP，就说明设备认这种请求，静止画面也能被它自己喂活。\n");
    return 0;
}
