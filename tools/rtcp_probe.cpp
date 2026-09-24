// 探针：把设备在"起流 -> 静默 -> 结束会话"整个过程中发回来的**每一个数据报**
// 按时间轴原样打出来。
//
// 为什么要这一层插桩：拆流的原因只剩"设备自己的策略"这一条，而它总得有个说法。
// 之前只知道"3~5 秒没包 + 会话表里消失了"，但从没看过消失**之前**设备上发了什么。
// 如果它在停之前发过一个 RTCP BYE、或某个 PT 的包带"我要停了"的意思，那断流就是
// 可预期、可提前应对的；如果它就是无声无息地不再发，那是服务端的空闲计时器。
// 这两种对产品处理完全不同，只能看字节。
//
// 顺带把两件一直没记的事钉下来：
//   1. answer 原文里到底有没有接收端 SSRC、RTCP 端口一类的字段（只打前 900 字节
//      看不见深层键，这里不截断）；
//   2. 混在视频端口上、PT 不等于协商 PT 的那些包到底是什么——之前只记成"RTCP PT=72"，
//      载荷内容一次都没看过，而那 8 字节扩展头语义也一直没记。
//
// 用法：rtcp_probe [--seconds N] [--death] [--verbose]
//   --death  一边打包一边每秒问一次会话表，把"在/不在"标在同一条时间轴上
#include <chrono>
#include <cstdio>
#include <map>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "media/StreamSession.h"
#include "remote/Device.h"
#include "rt/RtpHevc.h"
#include "xpc/XpcValue.h"

namespace {

using namespace std::chrono_literals;

uint64_t now_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void hexdump(std::span<const uint8_t> b, std::size_t cap) {
    const std::size_t n = std::min(b.size(), cap);
    for (std::size_t i = 0; i < n; ++i) {
        std::printf("%02x", b[i]);
        if (i % 8 == 7) {
            std::printf(" ");
        }
    }
    if (b.size() > cap) {
        std::printf(" …(共 %zu 字节)", b.size());
    }
}

/// 猜一猜载荷是不是一个 RTCP 公共头：V=2、PT 在 200..204/205/206 里、长度自洽。
/// 只是"看着像"，所以打出来时标成猜测。
void maybe_rtcp(std::span<const uint8_t> p) {
    if (p.size() < 8) {
        std::printf("        载荷不足 8 字节，没法判\n");
        return;
    }
    const int version = p[0] >> 6;
    const int rc = p[0] & 0x1F;
    const int pt = p[1];
    const unsigned len_words = (p[2] << 8) | p[3];
    const std::size_t total = (len_words + 1) * 4u;
    std::printf("        按 RTCP 公共头解: V=%d RC=%d PT=%d 长度字段=%u -> 整包 %zu 字节，"
                "实收 %zu 字节 %s\n",
                version, rc, pt, len_words, total, p.size(),
                total == p.size() ? "自洽" : "不自洽");
    const auto be32 = [&](std::size_t off) {
        return off + 4 <= p.size()
                   ? ((uint32_t(p[off]) << 24) | (uint32_t(p[off + 1]) << 16) |
                      (uint32_t(p[off + 2]) << 8) | p[off + 3])
                   : 0u;
    };
    std::printf("        前两个 32 位字: %08x %08x\n", be32(4), be32(8));
    if (pt == 200 && p.size() >= 32) {  // SR
        std::printf("        像 SR：ntp=%u.%u  rtp_ts=%u 包数=%u 字节数=%u\n", be32(20), be32(24),
                    be32(28), be32(32), p.size() > 36 ? be32(36) : 0u);
    }
}

}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int seconds = 12;
    bool death = false;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--seconds" && i + 1 < argc) {
            seconds = std::stoi(argv[++i]);
        } else if (a == "--death") {
            death = true;
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
    scrctl::media::StreamSession::Request req;
    auto session = scrctl::media::StreamSession::start(*dev, req, err, verbose);
    if (!session) {
        std::fprintf(stderr, "起流失败: %s\n", err.c_str());
        return 1;
    }
    const uint8_t video_pt = session->started().payload_type;
    std::printf("起流：收流端口=%u 设备发送端口=%u 协商视频 PT=%u\n", session->receiver_port(),
                session->started().sender_port, video_pt);
    std::printf("answer 全文（不截断）:\n%s\n\n",
                scrctl::xpc::describe(session->started().answer).c_str());

    const uint64_t t0 = now_ms();
    std::map<uint32_t, uint64_t> pt_hist;
    std::vector<uint8_t> packet;
    uint64_t next_second = t0 + 1000;
    uint64_t sec_video = 0, sec_rtcp = 0;
    uint64_t last_video = t0;
    uint32_t video_ssrc = 0;
    uint64_t shown_odd = 0;
    bool ours = true;
    std::string st_err;

    while (now_ms() - t0 < static_cast<uint64_t>(seconds * 1000)) {
        uint16_t peer_port = 0;
        const bool got = session->next_packet(packet, peer_port, 200, err);
        if (got) {
            scrctl::rt::PacketInfo info{};
            const bool parsed = scrctl::rt::parse_rtp_header(packet, info);
            const bool video = parsed && info.payload_type == video_pt;
            pt_hist[parsed ? info.payload_type : 999999]++;
            if (video) {
                ++sec_video;
                last_video = now_ms();
                video_ssrc = info.ssrc;
            } else {
                ++sec_rtcp;
            }
            if (!video && shown_odd < 3) {
                ++shown_odd;
                std::printf("%6llu ms 非视频包 PT=%u 整包 %zu 字节，前 24 字节: ",
                            static_cast<unsigned long long>(now_ms() - t0), info.payload_type,
                            packet.size());
                hexdump(packet, 24);
                std::printf("\n        RTP 头: seq=%u ts=%u ssrc=%08x marker=%d 载荷起点=%zu "
                            "源端口=%u\n",
                            info.sequence, info.timestamp, info.ssrc, info.marker ? 1 : 0,
                            info.payload_offset, peer_port);
                const std::size_t off = std::min(info.payload_offset, packet.size());
                std::printf("        载荷 [%zu:]: ", off);
                hexdump(std::span<const uint8_t>(packet).subspan(off), 48);
                std::printf("\n");
                if (off < packet.size()) {
                    maybe_rtcp(std::span<const uint8_t>(packet).subspan(off));
                }
                std::printf("\n");
            }
        }
        if (now_ms() >= next_second) {
            const uint64_t t = next_second - t0;
            next_second += 1000;
            std::string st_text;
            if (death) {
                const auto st = scrctl::media::StreamSession::probe(
                    *dev, session->started().session_uuid, st_err, verbose);
                const bool was = ours;
                ours = st == scrctl::media::StreamSession::ServerState::Alive;
                st_text = ours ? "在"
                               : (st == scrctl::media::StreamSession::ServerState::Ended ? "不在"
                                                                                         : "问不到");
                if (was != ours) {
                    st_text += "  <<< 刚刚变了";
                }
            }
            std::printf("%6llu ms  视频包 %4llu  非视频 %2llu  距最后视频包 %5llu ms  会话 %s\n", t,
                        static_cast<unsigned long long>(sec_video),
                        static_cast<unsigned long long>(sec_rtcp),
                        static_cast<unsigned long long>(now_ms() - last_video), st_text.c_str());
            sec_video = 0;
            sec_rtcp = 0;
        }
    }

    std::printf("\nPT 直方图：\n");
    for (const auto &[pt, n] : pt_hist) {
        std::printf("  PT=%-6u %llu 个%s\n", pt, static_cast<unsigned long long>(n),
                    pt == video_pt ? "（协商的视频）" : "");
    }
    std::printf("视频 SSRC=%08x\n", video_ssrc);
    return 0;
}
