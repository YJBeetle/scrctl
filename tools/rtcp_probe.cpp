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
#include "bitstream/AnnexB.h"
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
    /// 设备自己的 RTCP SR 里带"已发包数/已发字节数"（RFC 3550 §6.4.1，偏移 20/24）。
    /// 拿它的每秒增量和我们每秒收到的视频包一比，就能定死"设备只编这么慢"还是
    /// "设备发了但我们没收全"——这是唯一一条不依赖我们自己链路的读数。
    /// 一秒内出现过几个**不同的视频 RTP 时间戳**=设备这一秒真的编了几帧。
    /// 590 包/秒却只解出 12 帧，要么是设备只出 12 帧（每帧 48 包），要么是我们的
    /// AU 切分把几帧并成了一帧——时间戳能把这两件事一眼分开。
    uint32_t last_ts = 0;
    uint32_t distinct_ts = 0;
    /// 同一条时间轴上再放两个下游计数：拆出来的 NAL 数、以及 AU 切分给出的帧数。
    /// 60 个不同时间戳进来却只切出 12 个 AU，责任就在切分器；NAL 就只有 12 份，
    /// 责任在拆包器。
    uint64_t sec_nals = 0, sec_aus = 0;
    scrctl::rt::HevcRtpDepacketizer counter(session->started().payload_type);
    scrctl::AnnexBParser au_counter([&](std::vector<scrctl::Nal> &&au, bool keyframe) {
        ++sec_aus;
    });
    std::vector<uint32_t> ts_steps;
    uint32_t sr_packets = 0, sr_octets = 0;
    uint32_t last_sr_packets = 0, last_sr_octets = 0;

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
                std::vector<uint8_t> annexb;
                if (counter.push(packet, annexb, err)) {
                    const uint64_t nals = counter.stats().nals;
                    static uint64_t reported_nals = 0;
                    if (nals > reported_nals) {
                        sec_nals += nals - reported_nals;
                        reported_nals = nals;
                    }
                    if (!annexb.empty()) {
                        au_counter.feed(annexb.data(), annexb.size());
                    }
                }
                if (info.timestamp != last_ts) {
                    if (last_ts != 0) {
                        ts_steps.push_back(info.timestamp - last_ts);
                    }
                    last_ts = info.timestamp;
                    ++distinct_ts;
                }
            } else {
                ++sec_rtcp;
                // 裸 RTCP SR：4 字节公共头 + 4 SSRC + 8 NTP + 4 RTP ts + 4 包数 + 4 字节数。
                if (packet.size() >= 28 && (packet[1] & 0x7F) == 72) {
                    const auto be32 = [&](std::size_t off) {
                        return (uint32_t(packet[off]) << 24) | (uint32_t(packet[off + 1]) << 16) |
                               (uint32_t(packet[off + 2]) << 8) | packet[off + 3];
                    };
                    sr_packets = be32(20);
                    sr_octets = be32(24);
                }
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
            const long dev_pkts = static_cast<long>(sr_packets - last_sr_packets);
            const long dev_bytes = static_cast<long>(sr_octets - last_sr_octets);
            last_sr_packets = sr_packets;
            last_sr_octets = sr_octets;
            std::printf("%6llu ms  我收到视频包 %4llu  设备 SR 说它发了 %4ld 个 / %7ld 字节  "
                        "差 %4ld  这秒内 %3u 个不同时间戳  NAL %4llu  AU %3llu  距最后视频包 "
                        "%5llu ms  会话 %s\n",
                        t, static_cast<unsigned long long>(sec_video), dev_pkts, dev_bytes,
                        dev_pkts - static_cast<long>(sec_video), distinct_ts,
                        static_cast<unsigned long long>(sec_nals),
                        static_cast<unsigned long long>(sec_aus),
                        static_cast<unsigned long long>(now_ms() - last_video), st_text.c_str());
            sec_video = 0;
            sec_rtcp = 0;
            distinct_ts = 0;
            sec_nals = 0;
            sec_aus = 0;
        }
    }

    std::printf("\nPT 直方图：\n");
    for (const auto &[pt, n] : pt_hist) {
        std::printf("  PT=%-6u %llu 个%s\n", pt, static_cast<unsigned long long>(n),
                    pt == video_pt ? "（协商的视频）" : "");
    }
    std::printf("视频 SSRC=%08x\n", video_ssrc);
    if (ts_steps.size() > 2) {
        std::sort(ts_steps.begin(), ts_steps.end());
        std::printf("相邻两帧的时间戳差：最小 %u 中位 %u 最大 %u（时钟率未知，"
                    "但**不同取值的个数**就是帧数）\n", ts_steps.front(),
                    ts_steps[ts_steps.size() / 2], ts_steps.back());
    }
    return 0;
}
