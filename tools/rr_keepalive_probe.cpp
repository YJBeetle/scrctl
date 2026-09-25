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
// 用法：rr_keepalive_probe [--seconds N] [--attempts N] [--what none,rr,poll,poll5]
//                          [--timeout N] [--hold] [--event-channel] [--avc-features STR] [--verbose]
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
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <random>
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

/// RTCP 公共头里的长度是**16 位**（RFC 3550 §6.1：V/RC 1 字节 + PT 1 字节 + length 2
/// 字节）。这个坑在本文件里踩过一次、docs §13 还专门记着"拿自己组装的包去证明设备不理
/// 之前，先核对它的字节数"，结果今天又用 `put32(v, 7)` 写了一遍：整包从第 3 字节起错位
/// 两字节，于是"设备不理我们"这个结论的证据基础又是一个畸形包。
void put16(std::vector<uint8_t> &v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}

/// RR（RFC 3550 §6.4.2）：V=2、RC=1、PT=201、长度 7（= 头之后还有 7 个字），加发送者
/// SSRC 与一个 24 字节报告块，一共 32 字节。报告块里第一个字是"媒体 SSRC"，第二个字是
/// 分数丢包 1 字节 + 累计丢包 3 字节。
std::vector<uint8_t> build_rr(uint32_t our_ssrc, uint32_t media_ssrc, uint32_t ext_high) {
    std::vector<uint8_t> v;
    v.push_back(0x81);  // V=2, RC=1
    v.push_back(201);   // PT = RR
    put16(v, 7);        // 长度以 4 字节为单位，不含第一个字
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

/// SR（RFC 3550 §6.4.1）：RC=0 的那一档，28 字节。为什么也要试它——设备在 answer 里
/// 给我们分配了一个 `LocalSSRC`，也就是说它心里有一个"发送方=你"的位置；某些实现只认
/// 发送者报告（它按 SSRC 配对，收到一个从没收过包的源发来的 RR 会被当成对不上号）。
std::vector<uint8_t> build_sr(uint32_t our_ssrc, uint32_t packets, uint32_t octets) {
    std::vector<uint8_t> v;
    v.push_back(0x80);  // V=2, RC=0
    v.push_back(200);   // PT = SR
    put16(v, 6);        // 头之后还有 6 个字
    put32(v, our_ssrc);
    put32(v, 0);        // NTP 时间戳高位：不假装算过
    put32(v, 0);        // NTP 低位
    put32(v, 0);        // RTP 时间戳
    put32(v, packets);
    put32(v, octets);
    return v;
}

/// SDES（PT=202）带一个**空 CNAME**——12 字节，正是 Xcode 那种复合包的后半段
/// （pymobiledevice3 的注释：'Minimal SDES with an empty CNAME (matches Xcode's
/// compound RR+SDES)'）。早先我们给它塞了个 "scr1" 的 CNAME，长度字段跟着变 1 个字，
/// 于是"设备不理复合包"这条结论测的其实是另一种包。
std::vector<uint8_t> build_sdes(uint32_t our_ssrc) {
    std::vector<uint8_t> v;
    v.push_back(0x81);  // V=2, SC=1
    v.push_back(202);
    put16(v, 2);        // SSRC 1 字 + CNAME 块 1 字
    put32(v, our_ssrc);
    v.push_back(1);     // CNAME
    v.push_back(0);     // 长度 0
    v.push_back(0);     // 补到 4 字节边界
    v.push_back(0);
    return v;
}

/// 从 answer 的 `connection.streamConfig` 里取一个数。取不到返回 false。
/// 布尔也按数读：`RTCPTimeoutEnabled` 这种键在线上就是 XPC 的 Bool 类型，只按
/// Int64/UInt64 找会当成"没有这个键"。
bool stream_config_u32(const scrctl::xpc::Value &answer, const char *key, uint32_t &out) {
    const auto *conn = answer.find("connection");
    const auto *cfg = conn != nullptr ? conn->find("streamConfig") : nullptr;
    const auto *v = cfg != nullptr ? cfg->find(key) : nullptr;
    if (v == nullptr) {
        return false;
    }
    switch (v->type) {
    case scrctl::xpc::Type::Bool:
        out = v->boolean ? 1u : 0u;
        return true;
    case scrctl::xpc::Type::Int64:
        out = static_cast<uint32_t>(v->int64);
        return true;
    case scrctl::xpc::Type::UInt64:
        out = static_cast<uint32_t>(v->uint64);
        return true;
    case scrctl::xpc::Type::Double:
        out = static_cast<uint32_t>(v->real);
        return true;
    default:
        return false;
    }
}

/// 同上，取字符串项（`TxCodecFeatureListString` 这种）。
bool stream_config_str(const scrctl::xpc::Value &answer, const char *key, std::string &out) {
    const auto *conn = answer.find("connection");
    const auto *cfg = conn != nullptr ? conn->find("streamConfig") : nullptr;
    const auto *v = cfg != nullptr ? cfg->find(key) : nullptr;
    if (v == nullptr || v->type != scrctl::xpc::Type::String) {
        return false;
    }
    out = v->string;
    return true;
}

/// SDES（PT=202）带一个**实义 CNAME**。留着是为了把"空 CNAME 才有效"这个假设也测一遍
/// （rrminecname 那一臂），而不是因为我们有理由相信它管用。
std::vector<uint8_t> build_sdes_cname(uint32_t our_ssrc, std::string_view cname) {
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
    put16(v, static_cast<uint16_t>(body.size() / 4));  // 长度（字）
    v.insert(v.end(), body.begin(), body.end());
    return v;
}

/// AVConference 的接收端反馈包：RTCP APP（PT=204），名字 "RCTL"，32 字节。
///
/// 这是本轮改主意的来源。参考实现的抓包记着（原文注释）："tag 0x85000004 然后 8 个
/// u16：[0]=收到的 RTP 时间戳>>8，[1..2]=0，[3]=抖动/丢包(0)，[4]=1024Hz 接收端墙钟，
/// [5]=接收质量，[6]=帧数，[7]=接受的最高码率(kbps)"，并且**约 20 个/秒**；同一条流上
/// 还并行一种 name=5、16 字节的伴随包，节奏约 35 个/秒（每帧一个），内容是**收到的那个
/// RTP 时间戳**。两句要照抄的话：
///   "Echoing the *received* RTP timestamp is what makes the device act on the feedback
///    (a synthetic clock was ignored). Xcode sends this and no PLIs."
/// 也就是说 Apple 的客户端在视频端口上灌的不是 RR 也不是 PLI，而是这种厂商私有 APP 包。
/// 我们此前把所有变体的 RR/SDES/SR 都对齐了字节还是 20.0 秒死——那么"设备认的那种 RTCP"
/// 很可能根本不是 RFC 3550 里那几种，而是这个。
std::vector<uint8_t> build_rctl(uint32_t our_ssrc, uint32_t last_rtp_ts, uint32_t last_frame_pkts,
                                uint32_t packets_received, uint32_t clock_1024) {
    // w2 = (RTP 时间戳 >> 8) << 16；w3 = 上一帧的包数；
    // w4 = (1024Hz 墙钟 << 16) | 抖动；w5 = (累计包数 << 16) | 60001。
    //
    // w4 这个钟要按 **1024Hz 的本地单调时间**填，不是按 RTP 时间戳推。这是把苹果那 1469 个
    // RCTL 和它当时的收包状态逐个对齐量出来的：w4 高 16 位的全场斜率是 **1024.0/s**（16 位
    // 会回绕），而 RTP 时间戳是 24kHz、`ts/24` 只有 1000/s——按 ts 推会在 20 秒里差出约
    // 480ms，设备据此算出的单程时延就会一路漂。参考实现早期那句"用自起流以来的毫秒"就是
    // 踩在这个 2.4% 上。
    // w5 低 16 位固定 60001 也是同一批测量的结果：苹果全场分布是 {60001: 1075, 60000: 355,
    // 0: 39}，主值就是它（我一度只看前 12 个样本以为苹果发 0，查完全场才发现是我们对）。
    const uint32_t ts = last_rtp_ts;
    const uint32_t w2 = ((ts >> 8) & 0xFFFF) << 16;
    const uint32_t w3 = last_frame_pkts & 0xFFFFFFFF;
    const uint32_t w4 = ((clock_1024 & 0xFFFF) << 16) | 0;
    const uint32_t w5 = ((packets_received & 0xFFFF) << 16) | 0xEA61;
    std::vector<uint8_t> v;
    v.push_back(0x80);  // V=2, subtype=0
    v.push_back(204);   // PT = APP
    put16(v, 7);        // 头之后 7 个字（共 32 字节）
    put32(v, our_ssrc);
    v.insert(v.end(), {'R', 'C', 'T', 'L'});
    put32(v, 0x85000004u);
    put32(v, w2);
    put32(v, w3);
    put32(v, w4);
    put32(v, w5);
    return v;
}

/// RCTL 的伴随包：同一种 APP（PT=204），但 name 换成整数 5，只带一个字——
/// 收到的那个 RTP 时间戳。16 字节。抓包里的节奏是**每帧一个**。
std::vector<uint8_t> build_rctl_companion(uint32_t our_ssrc, uint32_t last_rtp_ts) {
    std::vector<uint8_t> v;
    v.push_back(0x80);
    v.push_back(204);
    put16(v, 3);  // 头之后 3 个字（共 16 字节）
    put32(v, our_ssrc);
    put32(v, 5);
    put32(v, last_rtp_ts);
    return v;
}

/// 把一棵 xpc 树的全部叶子打出来（`path = value` 一行一个）。
///
/// 为什么不用 `describe()`：它给字典条目做截断，而"设备有没有收到我们发的 RTCP"这件事
/// 恰恰藏在一个我们事先想不到的键里——只打自己预先想到的那几个键，就永远发现不了设备
/// 其实给了一个我们没读的键。这条教训在 `bitrate_probe --dump-answer` 上已经用过一次。
void walk(const scrctl::xpc::Value &v, const std::string &path, int depth) {
    if (depth > 6) {
        return;  // 防御性：设备的树不该这么深，真到了就是形状和预期不一样
    }
    switch (v.type) {
    case scrctl::xpc::Type::Dict:
        for (const auto &e : v.dict) {
            walk(e.value, path.empty() ? std::string(e.key) : path + "." + std::string(e.key),
                 depth + 1);
        }
        break;
    case scrctl::xpc::Type::Array:
        for (std::size_t i = 0; i < v.array.size(); ++i) {
            walk(v.array[i], path + "[" + std::to_string(i) + "]", depth + 1);
        }
        break;
    case scrctl::xpc::Type::String:
        std::printf("    %s = \"%s\"\n", path.c_str(), v.string.c_str());
        break;
    case scrctl::xpc::Type::Bool:
        std::printf("    %s = %s\n", path.c_str(), v.boolean ? "真" : "假");
        break;
    case scrctl::xpc::Type::Int64:
        std::printf("    %s = %lld\n", path.c_str(), static_cast<long long>(v.int64));
        break;
    case scrctl::xpc::Type::UInt64:
        std::printf("    %s = %llu\n", path.c_str(),
                    static_cast<unsigned long long>(v.uint64));
        break;
    case scrctl::xpc::Type::Double:
        std::printf("    %s = %.6g\n", path.c_str(), v.real);
        break;
    case scrctl::xpc::Type::Data:
    case scrctl::xpc::Type::Uuid:
        std::printf("    %s = <%zu 字节>\n", path.c_str(), v.data.size());
        break;
    default:
        std::printf("    %s = (type %08x)\n", path.c_str(), static_cast<unsigned>(v.type));
        break;
    }
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

/// 把设备会话表里每一条的**身份**打出来：是我们的还是别人的。
///
/// 为什么非要有这一段：一台设备只容一条媒体流，而"会话没了"有两种完全不同的原因——
/// 租期到点被设备摘掉，和**别的客户端（Xcode DeviceHub、另一个探针）发了它自己的
/// startmediastream 把我们的顶掉**。只看"我们收不到包了"这两种一模一样，而它们的结论
/// 完全相反：前者说明租期模型成立，后者说明这一轮数据根本不作数。
///
/// 身份怎么认：设备会把客户端请求里的键回显到会话条目上，所以
/// `type` / `timeout` 在不在、PT 是 100 还是 101、`clientSessionID` 等不等我们的，
/// 三条一起看就能认出这条是谁建的。
void dump_sessions(scrctl::remote::Device &dev, const std::vector<uint8_t> &our_uuid,
                   const char *tag) {
    std::string qerr;
    const auto st = scrctl::media::StreamSession::status(dev, qerr, false);
    const auto *ss = st.find("sessions");
    const std::size_t n = ss == nullptr ? 0 : ss->array.size();
    std::printf("  [%s] 设备表里 %zu 条会话\n", tag, n);
    if (ss == nullptr) {
        return;
    }
    for (std::size_t i = 0; i < ss->array.size(); ++i) {
        const auto &s = ss->array[i];
        const auto *conn = s.find("connection");
        const auto *cfg = conn == nullptr ? nullptr : conn->find("streamConfig");
        const auto *pt = cfg == nullptr ? nullptr : cfg->find("TxPayloadType");
        const auto *type = s.find("type");
        const auto *timeout = s.find("timeout");
        const auto *opt = conn == nullptr ? nullptr : conn->find("options");
        const auto *sid = opt == nullptr ? nullptr : opt->find("avcMediaStreamOptionClientSessionID");
        const auto *u = sid == nullptr ? nullptr : sid->find("uuid");
        const bool mine = u != nullptr && u->data == our_uuid;
        const auto *stat = s.find("status");
        const auto *run = stat == nullptr ? nullptr : stat->find("runDurationSeconds");
        std::printf("    [%zu] %s PT=%llu type=%s timeout键=%s 活了=%llus\n", i,
                    mine ? "我们的" : "别人的",
                    pt == nullptr ? 0ULL : static_cast<unsigned long long>(pt->uint64),
                    type == nullptr ? "(没有→不是这个 feature 建的)" : type->string.c_str(),
                    timeout == nullptr ? "无" : "有",
                    run == nullptr ? 0ULL : static_cast<unsigned long long>(run->uint64));
    }
}

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int seconds = 30;
    int attempts = 2;
    // 我们自己在 startmediastream 请求里发出去的那个 `timeout`。默认 20 —— 和设备的
    // `RTCPTimeoutInterval: 20`、以及实测那条 20.0 秒租期**是同一个数**。
    //
    // 这件事到此为止一直没人怀疑过，而所有"保活"实验都是在 it=20 下跑的：如果租期长度
    // 根本就是我们报的这个数（pymobiledevice3 把它注释成"negotiation timeout"，也就是
    // 当成客户端等回复的超时），那么回多少种 RTCP 都不可能把流留住超过 20 秒——因为
    // 那个 20 是我们自己写的。改这一个整数就能判掉这个假设，比造包便宜两个数量级。
    uint32_t timeout_seconds = 20;
    // 整个 `timeout` 键都不发（而不是发 0）。动机是从设备自己的会话表里读到的现象：
    // Xcode DeviceHub 正在镜像时，它的会话条目里没有 `timeout` 键、`RTCPTimeoutInterval`
    // 仍是 20.0，而 `runDurationSeconds` 爬到了 158 秒没换过会话。当时唯一的读法是
    // "报数=硬租期，不报=能被 RTCP 复位的空闲计时器"。
    //
    // **实测否掉了它：feature 层要求这个键，不发直接起不了流**
    // （`code 4865 / "Expected to find key timeout."`，`none` 和 `rrsrcsd` 两臂都撞在这）。
    // 留下的结论比原假设更要紧：会话条目里的 `timeout`/`type` 是 feature 层替客户端补的，
    // 所以 DeviceHub 那条会话根本不是从这个 feature 建的，它的"不断流"不能拿来当
    // "存在我们没找到的保活"的证据。这一臂留着，是为了让这条推理链随时可以重跑。
    bool no_timeout_key = false;
    // 在请求里带上 `sessionEventChannel`（一个 XPC UUID）。这是抓包对齐出来的、我们和
    // Xcode DeviceHub 的请求之间唯一差的一个键——苹果的 `timeout` 也是 20，却活了 715 秒。
    // 这一臂就是判"是不是这个键让租期失效"。
    bool event_channel = false;
    /// 握着那条起流连接不放，并在它上面持续 service() + 定期查状态。
    ///
    /// 为什么要这一位：95.8 秒的苹果抓包里，carrying 两次 mediastreamstart 的那条
    /// displayservice 连接（sport 61689 -> dport 54626）SYN 之后**整场没有 FIN 也没有
    /// RST**，一路活到抓包结束；而它的会话在报着 20 秒租期的情况下 74 秒没断。我们这边
    /// 每次 feature 调用都是"开连接→发一次→丢连接"（Device::feature_call），所以
    /// "会话有没有一个还开着的宿主连接"是从没被控住的变量。
    /// 上一轮"握着连接"那一臂测的是握了但没人读它——设备发 PING 没人 ACK，10 秒就
    /// cancel；这一臂把 service() 放上，先证明我们能不能握 60 秒，再看租期。
    bool hold_connection = false;
    /// 只握连接不起流（见使用处的说明）。
    bool hold_idle = false;
    /// 握着连接时不要每 5 秒在同一条连接上查一次状态。
    /// 为什么要有：查状态本身是一次请求，而"我们在一条设备上刚答完话的连接上再发一次
    /// 请求"这件事完全可能是把会话搞死的原因——不去掉它，就分不清"设备自己关的"和
    /// "被我们第二次调用搞关的"。
    bool hold_no_poll = false;
    /// 起一条**音频腿**，和视频腿共用同一个 `avcMediaStreamOptionClientSessionID`。
    ///
    /// 这是最后一个还没控住的结构性差异。苹果那份 95.8 秒抓包里的形状是：先 `type:"audio"`
    /// 再 `type:"video"`，两次 `ClientSessionID` 都是同一个 UUID，而那条精确 1.000Hz、
    /// 整场从不空档的 `RR+SDES` 发在**音频腿**上（客户端 52800 -> 设备 54228，74 个，
    /// 间隔 1.000±0.001s）；**视频腿上几乎没有 RR**（整场只有 46 个，间隔 1~4 秒地跳）。
    /// 如果设备的计时器挂在"这条 ClientSessionID"而不是"这条腿"上，那喂住它的就是音频腿，
    /// 而我们从来只有视频腿独活——这能同时解释"我们报多少秒死多少秒"和"苹果报了 20 却
    /// 永远不到点"。p3 也有 `start_audio_stream`，但它自己的会话表里跑起来只剩视频一条
    /// （`+40s sessions=1`），所以"p3 也断"**不能**否掉这条假设。
    bool audio_leg = false;
    /// 在音频腿上按 1Hz 发 RR+SDES（苹果就是这么做的）。和 `--audio-leg` 分开是必要的：
    /// 只起腿 = 验"设备是不是按 ClientSessionID 分组来免租期"；起腿 + 发 RR = 验
    /// "喂住计时器的是音频腿的 RTCP"。两个解释的修法完全不同，不能一次混着测。
    bool audio_rr = false;
    /// 把这份文件里的字节**原样**当 negotiatorOffer 发（绕开我们自己的构造器）。
    /// 给的是 Xcode DeviceHub 抓包里那次起流当场发出的 482 字节原文。
    std::string raw_offer_path;
    std::vector<uint8_t> raw_offer;
    /// 在 `com.apple.coredevice.deviceinfo` 上挂一条 **displayinfoupdates** 流式订阅。
    ///
    /// 这是今晚从"我们自己的日志"里翻出来的不对称逼出来的方向：同一份请求、同样报
    /// `timeout=20`，我们的**音频腿活过了 20 秒**（+30s 已收 1878 包、+40s 1999 包），
    /// 而视频腿精确死在 19997ms。也就是说被回收的不是"会话"，是**视频**那条。
    /// 什么会让设备的视频采集会话变成孤儿？抓包里苹果在 `deviceinfo`（设备端口 54583）上
    /// 有一条**抓包开始之前就已建立**的长连接，整场只推了一次
    /// `sideChannelStatus{pushing:[方向 / primary LCD / 6 个 wireless 显示器 / 背光]}`，
    /// 而那次推送的时刻是 **+20.71s——视频起流（+20.69s）之后 0.02 秒**。这个 feature 就挂在
    /// `deviceinfo` 的列表里：`com.apple.coredevice.feature.displayinfoupdates`。
    /// 如果"有人在订阅显示变化"就是设备判定这个显示采集有人在用的依据，那它就能同时解释
    /// 苹果 74 秒不断、我们 20 秒必死、以及我们的音频腿为什么不受影响。
    bool display_subscribe = false;
    // AVC 那条形串。抓包对齐到的最后一处可见差别：苹果发 `FLS;VRAE:0;SW:1;`，我们和 p3
    // 都发 `FLS;SW:1;`（p3 还专门注释说 VRAE:0 不能进）。设备会把它回显成
    // `TxCodecFeatureListString`，所以这条改动是可以在 answer 里验证"它收没收下"的——
    // 不然"发了个被设备默默丢掉的字符串"和"这个字符串真的进了协商"就分不清了。
    std::string avc_features = "FLS;SW:1;";
    // RTCP 的发送频率（每秒几个）。默认 1 是照参考实现的口径（"One RR/s keeps it
    // alive"）。为什么要能改：如果设备那个计时器真的"收到 RTCP 就复位"，那 1/s 在
    // `--timeout 6` 下必然活过 6 秒；反过来，1/s 不够而 5/s 够，说明它要的是"在
    // `RTCPSendInterval` 之内至少收到一个"。只试一种频率就宣布"RTCP 不能续命"，
    // 是拿一个样本当结论。
    double hz = 1.0;
    bool dump_status = false;
    std::string what = "none,rrsrc,rrsrcsd";
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--seconds" && i + 1 < argc) {
            seconds = std::stoi(argv[++i]);
        } else if (a == "--hz" && i + 1 < argc) {
            hz = std::stod(argv[++i]);
        } else if (a == "--dump-status") {
            dump_status = true;
        } else if (a == "--timeout" && i + 1 < argc) {
            timeout_seconds = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (a == "--no-timeout-key") {
            no_timeout_key = true;
        } else if (a == "--event-channel") {
            event_channel = true;
        } else if (a == "--hold") {
            hold_connection = true;
        } else if (a == "--hold-idle") {
            hold_idle = true;
        } else if (a == "--hold-no-poll") {
            hold_no_poll = true;
        } else if (a == "--audio-leg") {
            audio_leg = true;
        } else if (a == "--audio-rr") {
            audio_leg = true;
            audio_rr = true;
        } else if (a == "--offer" && i + 1 < argc) {
            raw_offer_path = argv[++i];
        } else if (a == "--avc-features" && i + 1 < argc) {
            avc_features = argv[++i];
        } else if (a == "--attempts" && i + 1 < argc) {
            attempts = std::stoi(argv[++i]);
        } else if (a == "--what" && i + 1 < argc) {
            what = argv[++i];
        } else if (a == "-v" || a == "--verbose") {
            verbose = true;
        }
    }

    // 后面所有打印都走这两个，免得某一处还按"我们一定发了 timeout"来印数字。
    const std::optional<uint32_t> lease =
        no_timeout_key ? std::optional<uint32_t>{} : std::optional<uint32_t>{timeout_seconds};
    const std::string lease_text =
        no_timeout_key ? "不发 timeout 键" : std::to_string(timeout_seconds) + "s";

    // 事件通道号：现编一个 v4 UUID 带上。为什么要自己填版本/变体位：设备侧是 Swift
    // Codable 的 UUID，形状不对时它会用一句很干脆的拒绝把整条请求挡掉（我们已经在
    // `avcMediaStreamOptionClientSessionID` 上撞过一次 "Expected to decode UUID but
    // found a OS_xpc_string instead"），别把那种拒绝误读成"这个键不被接受"。
    std::optional<std::vector<uint8_t>> event_channel_uuid;
    if (event_channel) {
        std::vector<uint8_t> u(16);
        std::random_device rd;
        for (auto &b : u) b = static_cast<uint8_t>(rd());
        u[6] = static_cast<uint8_t>((u[6] & 0x0f) | 0x40);
        u[8] = static_cast<uint8_t>((u[8] & 0x3f) | 0x80);
        char hex[37];
        std::snprintf(hex, sizeof(hex),
                      "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                      u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11],
                      u[12], u[13], u[14], u[15]);
        std::printf("带 sessionEventChannel = %s\n", hex);
        event_channel_uuid = u;
    }

    // 自检：这条探针的全部结论都建立在"我发出去的包是合法的"上面，而它已经两次栽在
    // 长度字段写成 32 位上（docs §13）。所以先把三种包的字节数和头里的 length 域打出来
    // 核对，对不上就直接不作数——宁可这一轮白跑，也不要再拿畸形包去证明"设备不理"。
    {
        const auto rr = build_rr(0x11111111u, 0x22222222u, 3);
        const auto sd = build_sdes(0x11111111u);
        const auto sr = build_sr(0x11111111u, 0, 0);
        const auto rctl = build_rctl(0x11111111u, 0x22222222u, 10, 100, 0);
        const auto comp = build_rctl_companion(0x11111111u, 0x22222222u);
        const int rr_len_field = (rr[2] << 8) | rr[3];
        std::printf("包自检：RR %zu 字节（头里 length=%d）  RR+SDES 复合 %zu 字节  SR %zu 字节  "
                    "RCTL %zu 字节  伴随 %zu 字节\n",
                    rr.size(), rr_len_field, rr.size() + sd.size(), sr.size(), rctl.size(),
                    comp.size());
        if (rr.size() != 32 || rr_len_field != 7 || sd.size() != 12 || sr.size() != 28 ||
            rctl.size() != 32 || comp.size() != 16) {
            std::fprintf(stderr, "包形状不对（应为 RR 32 / SDES 12 / SR 28 / RCTL 32 / 伴随 16，"
                                 "RR length 7），这一轮不作数\n");
            return 2;
        }
    }

    if (!raw_offer_path.empty()) {
        FILE *f = std::fopen(raw_offer_path.c_str(), "rb");
        if (f == nullptr) {
            std::fprintf(stderr, "打不开 offer 文件 %s\n", raw_offer_path.c_str());
            return 1;
        }
        uint8_t buf[4096];
        std::size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
            raw_offer.insert(raw_offer.end(), buf, buf + n);
        }
        std::fclose(f);
        std::printf("用 %s 的原文当 negotiatorOffer（%zu 字节，不走我们自己的构造器）\n",
                    raw_offer_path.c_str(), raw_offer.size());
    }

    std::string err;
    auto dev = scrctl::remote::Device::establish({}, err, verbose);
    if (!dev) {
        std::fprintf(stderr, "建立会话失败: %s\n", err.c_str());
        return 1;
    }

    // `--hold-idle`：不开媒体会话，只开一条 displayservice 连接、调一次 getsupportinfo，
    // 然后握着它空转 --seconds 秒。
    //
    // 为什么要单独问这个：握连接那一臂里设备在 +241ms 就把连接关了，而媒体流同时刻冻住
    // （61 个包、0 个 SR）。这两种解释完全相反，且都还在桌上：
    //   (a) "答完话就关"是**这条服务连接本身**的固有生命周期——那我们和苹果差的是别的；
    //   (b) 关连接是**会话被结束**的一个症状——那连接根本不是原因。
    // 把媒体会话从实验里拿掉就能二选一：这里如果也在 ~240ms 被关，是 (a)；如果空转 30 秒
    // 不被关，那 240ms 那个关是跟着会话一起来的，是 (b)。
    if (hold_idle) {
        std::string cerr;
        auto conn = dev->connect("com.apple.coredevice.displayservice", cerr, verbose);
        if (conn == nullptr) {
            std::fprintf(stderr, "开连接失败: %s\n", cerr.c_str());
            return 1;
        }
        scrctl::xpc::Value out;
        auto in = scrctl::xpc::make_dict();
        const auto r = conn->invoke("com.apple.coredevice.feature.getmediasupportinfo",
                                    "com.apple.coredevice.action.mediastreamgetsupportinfo", in,
                                    out, 10000, cerr);
        std::printf("[idle] 第一次调用：%s，输出类型 %d\n",
                    r == scrctl::remote::CallResult::Ok ? "成功" : cerr.c_str(),
                    static_cast<int>(out.type));
        const uint64_t it0 = now_ms();
        uint64_t next_poll = it0 + 5000;
        int polls = 0;
        std::string serr;
        long long died_ms = -1;
        while (now_ms() - it0 < static_cast<uint64_t>(seconds) * 1000) {
            if (!conn->service(200, serr)) {
                died_ms = static_cast<long long>(now_ms() - it0);
                std::printf("    [idle] 连接在 +%lldms 被对端关掉: %s\n", died_ms, serr.c_str());
                break;
            }
            if (now_ms() >= next_poll) {
                next_poll += 5000;
                scrctl::xpc::Value po;
                std::string qerr;
                auto pi = scrctl::xpc::make_dict();
                const auto pr = conn->invoke(
                    "com.apple.coredevice.feature.getmediasupportinfo",
                    "com.apple.coredevice.action.mediastreamgetsupportinfo", pi, po, 10000, qerr);
                std::printf("    [idle] +%llus 同一条连接再调一次：%s\n",
                            static_cast<unsigned long long>((now_ms() - it0) / 1000),
                            pr == scrctl::remote::CallResult::Ok ? "成功" : qerr.c_str());
                if (pr == scrctl::remote::CallResult::Ok) {
                    ++polls;
                }
            }
        }
        std::printf("[idle] 结论：握着这条 displayservice 连接 %lld 毫秒，%s，同连接复查成功 %d 次\n",
                    died_ms >= 0 ? died_ms : static_cast<long long>(now_ms() - it0),
                    died_ms >= 0 ? "被设备关了" : "设备没关", polls);
        return 0;
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
            // 臂名形如 `<包变体>[+<offer 开关>[+...]]'，比如 `rrsrcsd+fb'、`rctl+fb+ltrp'。
            // 用 + 拆而不是把开关焊进变体名里：开关有 2 个、包变体有一串，全组合展开成
            // 三十多个名字，每个组合都得重写一遍判断条件。
            const auto plus = w.find('+');
            const std::string base = plus == std::string::npos ? w : w.substr(0, plus);
            const std::string flags = plus == std::string::npos ? "" : w.substr(plus + 1);
            const bool fb = flags.find("fb") != std::string::npos;
            const bool ltrp = flags.find("ltrp") != std::string::npos;

            const int poll_every_ms = base == "poll" ? 2000 : (base == "poll5" ? 5000 : 0);
            // RTCP 变体。这一批臂是"回 RTCP 能不能续命"这个问题留下的：当时以为会话是
            // 起流后 20 秒的硬租期、而协商参数里写着 `RTCPTimeoutInterval: 20`，于是把
            // 那 20 秒读成"没收到接收端 RTCP 的超时"。**这个解释后来被推翻了**（那个 20
            // 就是我们在请求里报的 `timeout`，见 --timeout），但这批臂作为"RTCP 能不能
            // 延长租期"的负结果仍然成立——答案是完全不能，报多长就多少秒死，一视同仁。
            // 这几个变体各改一个变量：
            //   rr      裸 RR，发送者 SSRC 用我们自己编的，发到设备那个媒体端口
            //   rrsame  同上，但发送者 SSRC = 设备的媒体 SSRC
            //   rrsdes  RR + SDES(CNAME) 复合包（设备的 SR 就是 SR+SDES 复合来的）
            //   rrp1    裸 RR 发到 媒体端口+1（RFC 3550 的 RTCP 端口惯例）
            //   rrall   复合包 + 媒体 SSRC + 端口+1，全都给
            //   rrneg   发送者 SSRC = answer 的 `LocalSSRC`，报告块 = `RemoteSSRC`
            //   rrnegp1 rrneg 但发到 媒体端口+1
            //   rrnegsr rrneg 但发 SR 而不是 RR
            //   rrmine  **反过来**：发送者 = `RemoteSSRC`，报告块 = `LocalSSRC`
            //   rrminep1/rrminesr 同上两件事的端口/SR 变体
            //   rrsrc / rrsrcsd 同上，但目的端口换成 streamConfig.SourcePort
            //   rctl    AVConference 的 RTCP APP "RCTL"（20/s）+ 每帧一个 name=5 伴随包
            //   rctlrr  RCTL 那一套 + 每秒一个 RR+SDES 复合包
            //
            // rrmine 这一组才是上一轮的重点。上一轮跑出来才发现 answer 里的 `LocalSSRC`
            // 就是 RTP 头里设备自己那个 SSRC（探针把报告块与实际包头一比就露馅了），也就
            // 是说这两个名字是**从设备的视角**起的：Local = 设备自己发的那条流，Remote =
            // 设备给我们这一端分配的 SSRC。那么"RTCP 发送者该填谁"根本不是我们能编的——
            // 它已经替我们编好了。前面所有臂（包括 rrneg）都填错了人。
            //
            // rctl 这一组是本轮的重点，理由见 build_rctl 上面那段：把 RFC 3550 那几种包
            // 的字节、SSRC、端口全对上了仍然 20.0 秒死，而 Apple 客户端在视频端口上灌的
            // 是这种 PT=204 的厂商 APP 包，"Xcode sends this and no PLIs"。
            const bool send_rr = base.rfind("rr", 0) == 0;
            const bool send_rctl = base.rfind("rctl", 0) == 0;
            const bool sdes = base == "rrsdes" || base == "rrall" || base == "rrminesd" ||
                              base == "rrsrcsd" || base == "rctlrr";
            const bool same_ssrc = base == "rrsame" || base == "rrall";
            const bool neg_ssrc = base == "rrneg" || base == "rrnegp1" || base == "rrnegsr";
            const bool mine_ssrc =
                base == "rrmine" || base == "rrminep1" || base == "rrminesr" ||
                base == "rrminesd" || base == "rrminecname" || base == "rrsrc" ||
                base == "rrsrcsd" || base == "rctl" || base == "rctlrr";
            // 发到 streamConfig.SourcePort（pymobiledevice3 用的就是它），而不是
            // connection.sender.port。RCTL 那两臂没有别的选项——按抓包它就是这个目的。
            const bool to_source_port =
                base == "rrsrc" || base == "rrsrcsd" || base == "rctl" || base == "rctlrr";
            const bool send_sr = base == "rrnegsr" || base == "rrminesr";
            const bool port_plus_one =
                base == "rrp1" || base == "rrall" || base == "rrnegp1" || base == "rrminep1";
            std::string start_err;
            scrctl::media::StreamSession::Request req;
            req.offer.allow_rtcp_fb = fb;
            req.offer.ltrp_enabled = ltrp;
            req.offer.avc_features = avc_features;
            req.timeout_seconds = lease;
            req.session_event_channel = event_channel_uuid;
            req.raw_offer = raw_offer;

            // 苹果是**先起音频再起视频**，两条腿共用同一个 ClientSessionID。这里照那个
            // 顺序来：先生成/复用这 16 字节，起音频腿，再用同一个 UUID 起视频腿。
            std::vector<uint8_t> shared_session;
            std::unique_ptr<scrctl::media::StreamSession> audio;
            uint32_t audio_sender_ssrc = 0, audio_report_ssrc = 0;
            uint16_t audio_dest_port = 0;
            uint64_t audio_seen = 0;
            uint32_t audio_highest_seq = 0;
            if (audio_leg) {
                std::random_device rd;
                shared_session.resize(16);
                for (auto &b : shared_session) {
                    b = static_cast<uint8_t>(rd() & 0xFF);
                }
                scrctl::media::StreamSession::Request areq;
                areq.audio = true;
                areq.client_session_uuid = shared_session;
                areq.timeout_seconds = lease;
                areq.offer = req.offer;
                std::string aerr;
                audio = scrctl::media::StreamSession::start(*dev, areq, aerr, verbose);
                if (!audio) {
                    std::fprintf(stderr, "[%s] 音频腿起流失败: %s（这一臂不作数）\n", w.c_str(),
                                 aerr.c_str());
                    std::this_thread::sleep_for(2s);
                    continue;
                }
                // 设备在音频 answer 里给的三个数，就是我们在音频腿上发 RTCP 要用的三个数
                // （发送者 = 它给我们分配的 RemoteSSRC，报告块 = 它自己的 LocalSSRC，
                // 目的端口 = 它的 sender.port）。取不到就停在这一臂上，别拿编的数发包。
                uint32_t a_remote = 0, a_local = 0;
                stream_config_u32(audio->started().answer, "RemoteSSRC", a_remote);
                stream_config_u32(audio->started().answer, "LocalSSRC", a_local);
                audio_sender_ssrc = a_remote;
                audio_report_ssrc = a_local;
                audio_dest_port = audio->started().sender_port;
                uint32_t audio_pt = audio->started().payload_type;
                std::printf("  音频腿已起：收流端口=%u 设备发送端口=%u PT=%u "
                            "RemoteSSRC=%u LocalSSRC=%u\n",
                            audio->receiver_port(), audio_dest_port, audio_pt, a_remote, a_local);
                req.client_session_uuid = shared_session;
            }

            // 握着连接那一臂：自己开一条 displayservice 连接，用它来起流，然后**不放**，
            // 另起一个线程只管 service()——空转时替这条连接读一眼，好让设备的 PING 有人
            // 应答。查状态也走这同一条连接：这样"连接还活着"和"会话还在表里"是同一时刻
            // 从同一条链路上拿到的两个证据，而不是两条连接各说一套。
            // 一个线程独占这条 Channel 是有意的：Channel 的 rx_/pending_ 没有锁，
            // service() 和 invoke() 并发跑会互相吃掉字节。
            std::unique_ptr<scrctl::remote::ServiceConnection> held;
            std::atomic<bool> hold_done { false };
            std::atomic<int> hold_polls { 0 };
            std::atomic<long long> hold_died_ms { -1 };
            std::thread hold_thread;
            if (hold_connection) {
                std::string cerr;
                held = dev->connect("com.apple.coredevice.displayservice", cerr, verbose);
                if (held == nullptr) {
                    std::fprintf(stderr, "[%s] 开连接失败: %s\n", w.c_str(), cerr.c_str());
                    std::this_thread::sleep_for(2s);
                    continue;
                }
            }
            auto session = scrctl::media::StreamSession::start(*dev, req, start_err, verbose,
                                                               held.get());
            if (!session) {
                std::fprintf(stderr, "[%s] 起流失败: %s\n", w.c_str(), start_err.c_str());
                std::this_thread::sleep_for(2s);
                continue;
            }
            const uint64_t arm_t0 = now_ms();
            if (held != nullptr) {
                hold_thread = std::thread([&] {
                    std::string serr;
                    uint64_t next_status = 0;
                    while (!hold_done.load()) {
                        if (!held->service(200, serr)) {
                            hold_died_ms.store(static_cast<long long>(now_ms() - arm_t0));
                            std::printf("    [hold] 连接在 +%lldms 断了: %s\n",
                                        static_cast<long long>(now_ms() - arm_t0), serr.c_str());
                            return;
                        }
                        if (!hold_no_poll && now_ms() >= next_status) {
                            next_status = now_ms() + 5000;
                            scrctl::xpc::Value out;
                            std::string qerr;
                            auto in = scrctl::xpc::make_dict();
                            const auto r = held->invoke(
                                "com.apple.coredevice.feature.getmediastreamserverstatus",
                                "com.apple.coredevice.action.mediastreamstatus", in, out, 10000,
                                qerr);
                            if (r == scrctl::remote::CallResult::Ok) {
                                ++hold_polls;
                                const auto *ss = out.find("sessions");
                                std::printf("    [hold] +%lldms 同一条连接查到会话 %zu 条\n",
                                            static_cast<long long>(now_ms() - arm_t0),
                                            ss != nullptr && ss->is_array()
                                                ? ss->array.size()
                                                : static_cast<std::size_t>(0));
                            } else {
                                std::printf("    [hold] +%lldms 同一条连接查状态失败(%d): %s\n",
                                            static_cast<long long>(now_ms() - arm_t0),
                                            static_cast<int>(r), qerr.c_str());
                            }
                        }
                    }
                });
            }
            std::printf("第 %d 轮 [%s]：流已起，观察 %d 秒（不去碰设备，让画面自己静止）"
                        "offer: allowRTCPFB=%d ltrpEnabled=%d\n",
                        round, w.c_str(), seconds, fb ? 1 : 0, ltrp ? 1 : 0);

            Arm arm;
            arm.what = w;
            const uint64_t t0 = now_ms();
            std::vector<uint8_t> packet;
            uint16_t peer = 0;
            uint32_t media_ssrc = 0;
            bool ssrc_role_printed = false;
            uint16_t highest_seq = 0;
            uint64_t last_video = 0;
            uint64_t next_rr = t0;
            const uint64_t rr_period_ms = hz > 0.0 ? std::max(1LL, (long long)(1000.0 / hz)) : 1000;
            uint64_t next_poll = t0;
            // RCTL 那两臂要报的是**真实收到的**东西，所以收包时得记三样：最后一个视频包
            // 的 RTP 时间戳、累计视频包数、以及"上一帧有多少个包"（marker 那一下结算）。
            uint32_t rtp_last_ts = 0;
            uint32_t rtp_packets = 0;
            /// 我们**发出去**了多少个 RTCP。这一列是探针的自证：判活看的是设备那边的
            /// SR 时钟，而"我这侧一个 RTCP 都没发出去"和"发了但设备不认"在那一列上完全
            /// 一样——不记这个数，就会把"什么都没做"读成"做了没用"（lifetime_probe 栽过
            /// 的那个坑，docs §13 记着）。
            uint64_t rtcp_sent = 0;
            uint32_t cur_frame_pkts = 0;
            uint32_t last_frame_pkts = 0;
            uint64_t next_rctl = t0;
            // 我们自己的 SSRC：不能拿设备那个当发送者，否则设备按 SSRC 配对时会认为
            // 这是它自己的报告而丢掉（也可能更糟：把两条流的报告当成同一条）。
            const uint32_t our_ssrc = 0x35c0ffeeu;
            // 设备在 answer 里**已经给我们分配过一个 SSRC**（`LocalSSRC`），并且写明了
            // 它那条流的 `RemoteSSRC`。上面那句注释的推理没错，但结论应该是"用设备分配的
            // 那个"，而不是"自己编一个"。这两个数是 `bitrate_probe --dump-answer` 露出来的。
            uint32_t neg_local_ssrc = 0, neg_remote_ssrc = 0, neg_rtcp_port = 0;
            uint32_t neg_source_port = 0;
            const bool has_local =
                stream_config_u32(session->started().answer, "LocalSSRC", neg_local_ssrc);
            const bool has_remote =
                stream_config_u32(session->started().answer, "RemoteSSRC", neg_remote_ssrc);
            stream_config_u32(session->started().answer, "RTCPRemotePort", neg_rtcp_port);
            // **answer 里有两个"设备那边的端口"**：`connection.sender.port`（scrctl 一直
            // 拿它当 sender_port，也就是我们所有 RTCP 实验的目的端口）和
            // `streamConfig.SourcePort`。实测这两个数不一样（一次跑出来是 54351 与 61422）。
            // pymobiledevice3 发 RTCP/PLI 用的是后者。如果设备的 RTCP 监听在 SourcePort 上，
            // 那我们前面所有"设备不理 RTCP"的结论都是发到了一个没人收的端口上得到的。
            const bool has_source_port =
                stream_config_u32(session->started().answer, "SourcePort", neg_source_port);
            // 那两个 RTCP 超时键是这一节全部推理的起点，所以要每臂都打出来看**它跟着谁变**：
            // 如果 `RTCPTimeoutInterval` 跟着我们请求里的 `timeout` 走，那这条租期就不是
            // 设备定的，是我们自己报的。
            uint32_t rtcp_interval = 0, rtcp_enabled = 0;
            const bool has_interval =
                stream_config_u32(session->started().answer, "RTCPTimeoutInterval", rtcp_interval);
            stream_config_u32(session->started().answer, "RTCPTimeoutEnabled", rtcp_enabled);
            std::printf("  answer: LocalSSRC=%s RemoteSSRC=%s RTCPRemotePort=%u "
                        "connection.sender.port=%u streamConfig.SourcePort=%u\n",
                        has_local ? std::to_string(neg_local_ssrc).c_str() : "(没有)",
                        has_remote ? std::to_string(neg_remote_ssrc).c_str() : "(没有)",
                        neg_rtcp_port, session->started().sender_port, neg_source_port);
            std::printf("  请求 timeout=%s -> answer RTCPTimeoutInterval=%s RTCPTimeoutEnabled=%s\n",
                        lease_text.c_str(),
                        has_interval ? std::to_string(rtcp_interval).c_str() : "(没有)",
                        rtcp_enabled == 1 ? "真" : (rtcp_enabled == 0 ? "假/没读到" : "其它"));
            if (!no_timeout_key && has_interval && rtcp_interval != timeout_seconds) {
                std::printf("  两者不等：设备没有照抄我们报的那个数\n");
            }
            if (has_source_port && neg_source_port != session->started().sender_port) {
                std::printf("  两个端口不同：rrsrc* 那几臂发到 streamConfig.SourcePort\n");
            }
            // 我们发出去的特性串到底进没进协商，看设备回显的那一条。没有这一行的话，
            // "设备收下了 VRAE:0"和"设备把它丢了"在结果上完全一样。
            std::string echoed;
            if (stream_config_str(session->started().answer, "TxCodecFeatureListString", echoed)) {
                std::printf("  我们发 %s -> 设备回显 TxCodecFeatureListString=%s\n",
                            avc_features.c_str(), echoed.c_str());
            }

            const uint64_t until = t0 + static_cast<uint64_t>(seconds) * 1000;
            // 音频腿上每秒一个 RR+SDES 的节奏起点，和它自己收到的包/发出去的计数。
            uint64_t next_audio_rr = t0;
            uint64_t audio_rr_sent = 0;
            // 每 10 秒打一行进度。长观察窗（分钟级）没有这一行的话，探针看起来像卡死，
            // 而"它其实还在收包"正是本轮要报的答案——探针要能证明自己做了事。
            uint64_t next_tick = t0 + 10000;
            uint64_t video_seen = 0;
            uint64_t sr_seen = 0;
            // 目的端口：RCTL 与 rrsrc* 那几臂发到 streamConfig.SourcePort，其余发到
            // answer 里 connection.sender.port（scrctl 一直用的那个）。
            const uint16_t dest_port = static_cast<uint16_t>(
                to_source_port && has_source_port
                    ? neg_source_port
                    : session->started().sender_port + (port_plus_one ? 1 : 0));
            while (now_ms() < until) {
                // 每圈最多收 kDrainPerRound 个包就回到外层。不封顶的话外层那些"到点就
                // 发一个 RTCP"的判断**在忙画面上永远轮不到**：视频包一秒几百个地来，内层
                // 的 `while (next_packet(...))` 一直不空,于是 --hz 10 实测只发出 21 个包
                // （和 --hz 1 一模一样），"频率"这一维等于没测。
                constexpr int kDrainPerRound = 32;
                for (int drained = 0; drained < kDrainPerRound; ++drained) {
                    if (!session->next_packet(packet, peer, 30, err)) {
                        break;
                    }
                    // 出包循环里也要看时刻。**这一条是长租期暴露出来的 bug**：租期只有
                    // 20 秒时，流一死 next_packet 就开始超时返回 false，内层循环必然退出，
                    // 于是"内层循环会因为流一直活着而永不退出"这件事从来没暴露过。
                    // 把 timeout 提到 3600 之后，探针在 150 秒的观察窗之后仍然卡在这一层
                    // 收包——那一刻它其实已经给出了本轮最重要的答案：流还活着。
                    if (now_ms() >= until) {
                        break;
                    }
                    const uint64_t now = now_ms();
                    scrctl::rt::PacketInfo info {};
                    if (scrctl::rt::parse_rtp_header(packet, info) &&
                        info.payload_type == session->started().payload_type) {
                        media_ssrc = info.ssrc;
                        highest_seq = info.sequence;
                        last_video = now;
                        arm.last_video_ms = now;
                        arm.got_idr = true;
                        ++video_seen;
                        ++rtp_packets;
                        rtp_last_ts = info.timestamp;
                        ++cur_frame_pkts;
                        // 抓包里的伴随包（name=5）是**每帧一个**，跟着 marker 位走。
                        if (info.marker && send_rctl) {
                            last_frame_pkts = cur_frame_pkts;
                            cur_frame_pkts = 0;
                            std::string serr;
                            const auto comp = build_rctl_companion(
                                mine_ssrc && has_remote ? neg_remote_ssrc : our_ssrc,
                                rtp_last_ts);
                            if (!session->send_rtp(comp, dest_port, serr)) {
                                arm.note = "RCTL 伴随包发送失败: " + serr;
                            } else {
                                ++rtcp_sent;
                            }
                        }
                    } else if (is_rtcp_sr(packet)) {
                        ++sr_seen;
                        arm.last_sr_ms = now;
                        if (last_video != 0 && now - last_video > 1500) {
                            ++arm.srs_after_video;
                        }
                    }
                }
                if (send_rctl && rtp_packets != 0 && now_ms() >= next_rctl) {
                    next_rctl += 50;  // 抓包里的节奏：约 20 个/秒
                    std::string serr;
                    // 1024Hz 本地单调钟，从本轮起流那一刻算（苹果那个也是回绕的 16 位）。
                    const uint32_t clock_1024 =
                        static_cast<uint32_t>((now_ms() - t0) * 1024 / 1000);
                    const auto rctl =
                        build_rctl(mine_ssrc && has_remote ? neg_remote_ssrc : our_ssrc,
                                   rtp_last_ts, last_frame_pkts, rtp_packets, clock_1024);
                    if (!session->send_rtp(rctl, dest_port, serr)) {
                        arm.note = "RCTL 发送失败: " + serr;
                    } else {
                        ++rtcp_sent;
                    }
                }
                if (send_rr && media_ssrc != 0 && now_ms() >= next_rr) {
                    next_rr += rr_period_ms;
                    std::string serr;
                    // 发送者 SSRC：
                    //   mine 臂用 answer 的 `RemoteSSRC`（设备给我们这端分配的）
                    //   neg  臂用 `LocalSSRC`（上一轮证明那其实是设备自己的流，所以这臂
                    //          是"填错人"的那一版，留着当对照）
                    //   same 臂故意用设备的媒体 SSRC；其余用自己编的
                    const uint32_t sender_ssrc =
                        mine_ssrc && has_remote ? neg_remote_ssrc
                        : neg_ssrc && has_local ? neg_local_ssrc
                        : same_ssrc             ? media_ssrc
                                                : our_ssrc;
                    // 报告块里指认的流：mine 臂指设备自己那条流（`LocalSSRC`），其余指实际
                    // 收到的 RTP 头里那个。
                    const uint32_t report_ssrc =
                        mine_ssrc && has_local ? neg_local_ssrc
                        : neg_ssrc && has_remote ? neg_remote_ssrc
                                                 : media_ssrc;
                    std::vector<uint8_t> rr;
                    if (send_sr) {
                        rr = build_sr(sender_ssrc, 0, 0);  // 我们一个 RTP 都没发，如实报 0
                    } else {
                        rr = build_rr(sender_ssrc, report_ssrc, highest_seq);
                        if (base == "rrminecname") {
                            const auto sd = build_sdes_cname(sender_ssrc, "scrctl");
                            rr.insert(rr.end(), sd.begin(), sd.end());
                        } else if (sdes) {
                            const auto sd = build_sdes(sender_ssrc);
                            rr.insert(rr.end(), sd.begin(), sd.end());
                        }
                    }
                    // 命名口径的证据就打在第一次发包时：设备的 RTP 头 SSRC 到底等于
                    // answer 里的哪一个。这一行决定了上面两种填法哪个才是"填对自己"。
                    if (!ssrc_role_printed) {
                        ssrc_role_printed = true;
                        std::printf("  RTP 头里的 SSRC %u == answer 的 %s（%s）\n", media_ssrc,
                                    media_ssrc == neg_local_ssrc ? "LocalSSRC" : "RemoteSSRC",
                                    media_ssrc == neg_local_ssrc ? "所以 Local 是设备自己那条流"
                                                                 : "所以 Remote 是设备自己那条流");
                    }
                    if (!session->send_rtp(rr, dest_port, serr)) {
                        arm.note = "RTCP 发送失败: " + serr;
                    } else {
                        ++rtcp_sent;
                    }
                }
                // 音频腿这一段做两件事，顺序不能反：先把它**实际收到**的包记下来（这是
                // "设备到底替不替我们建这条腿"的唯一一手证据——answer 回了不代表在推流），
                // 再按苹果那个节奏每秒发一个 RR+SDES。
                if (audio) {
                    std::vector<uint8_t> ap;
                    uint16_t apeer = 0;
                    std::string aerr;
                    for (int drained = 0; drained < 8; ++drained) {
                        if (!audio->next_packet(ap, apeer, 1, aerr)) {
                            break;
                        }
                        scrctl::rt::PacketInfo ai {};
                        if (scrctl::rt::parse_rtp_header(ap, ai) &&
                            ai.payload_type == audio->started().payload_type) {
                            ++audio_seen;
                            audio_highest_seq = ai.sequence;
                        }
                    }
                    if (audio_rr && audio_sender_ssrc != 0 && now_ms() >= next_audio_rr) {
                        next_audio_rr += 1000;
                        auto arr = build_rr(audio_sender_ssrc, audio_report_ssrc,
                                            static_cast<uint32_t>(audio_highest_seq));
                        const auto asd = build_sdes(audio_sender_ssrc);
                        arr.insert(arr.end(), asd.begin(), asd.end());
                        std::string serr;
                        if (!audio->send_rtp(arr, audio_dest_port, serr)) {
                            arm.note = "音频腿 RR 发送失败: " + serr;
                        } else {
                            ++audio_rr_sent;
                        }
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
                if (now_ms() >= next_tick) {
                    next_tick += 10000;
                    if (audio) {
                        std::printf(
                            "  +%3llus 视频包 %6llu SR 心跳 %4llu 发出 RTCP %5llu"
                            " 音频包 %6llu 音频 RR %3llu（租期 %s）\n",
                            static_cast<unsigned long long>((now_ms() - t0) / 1000),
                            static_cast<unsigned long long>(video_seen),
                            static_cast<unsigned long long>(sr_seen),
                            static_cast<unsigned long long>(rtcp_sent),
                            static_cast<unsigned long long>(audio_seen),
                            static_cast<unsigned long long>(audio_rr_sent), lease_text.c_str());
                    } else {
                        std::printf("  +%3llus 视频包 %6llu SR 心跳 %4llu 发出 RTCP %5llu（租期 %s）\n",
                                    static_cast<unsigned long long>((now_ms() - t0) / 1000),
                                    static_cast<unsigned long long>(video_seen),
                                    static_cast<unsigned long long>(sr_seen),
                                    static_cast<unsigned long long>(rtcp_sent), lease_text.c_str());
                    }
                    if (dump_status) {
                        std::string qerr;
                        const auto st = scrctl::media::StreamSession::status(*dev, qerr, verbose);
                        // 把自己的 uuid 和表里每一条的 uuid 并排打出来。为什么要这么麻烦：
                        // `probe()` 只回"在/不在/不知道"，而"不在"有两种完全不同的原因——
                        // 会话真的结束了，和**我们的 uuid 没匹配上**（字节序、包装层级都
                        // 能让这两种长得一模一样）。分不清这个，就会把一条活着的会话判成
                        // 死了并重起，而这条判据是泵里救流那条路的依据。
                        std::printf("    我们的 session_uuid = ");
                        for (uint8_t b : session->started().session_uuid) {
                            std::printf("%02x", b);
                        }
                        std::printf("\n");
                        if (const auto *ss = st.find("sessions"); ss != nullptr) {
                            for (std::size_t i = 0; i < ss->array.size(); ++i) {
                                const auto *opt = ss->array[i].find("connection");
                                const auto *w = opt != nullptr
                                    ? opt->at("options")
                                          .find("avcMediaStreamOptionClientSessionID")
                                    : nullptr;
                                const auto *u = w != nullptr ? w->find("uuid") : nullptr;
                                std::printf("    表里第 %zu 条 uuid = ", i);
                                if (u == nullptr) {
                                    std::printf("(读不到)");
                                } else {
                                    for (uint8_t b : u->data) {
                                        std::printf("%02x", b);
                                    }
                                }
                                std::printf("\n");
                            }
                        }
                        walk(st, "状态", 0);
                    }
                }
            }
            // 收不到包的那一刻就先看表，不要等到最后。这一条是给"到底是到点死还是被顶掉"
            // 留的证据——那一轮里 +16.99s 的死法和 20 秒租期对不上，事后才发现后台开着
            // DeviceHub，而当时的输出没有任何一位能证明不是被抢的。
            // 死的那一刻先看一眼表：这条会话到底是设备按租期摘掉的，还是被别的客户端
            // （后台开着的 Xcode DeviceHub）顶掉的——两者的结论完全相反，而只看"我们收不
            // 到包"根本分不开。
            dump_sessions(*dev, session->started().session_uuid, "刚停");
            if (dump_status) {
                std::printf("  观察窗结束时的设备状态原文：\n");
                std::string qerr;
                walk(scrctl::media::StreamSession::status(*dev, qerr, verbose), "状态", 0);
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
            if (hold_thread.joinable()) {
                hold_done.store(true);
                hold_thread.join();
                std::printf("  [hold] 这一臂握着连接：同一条连接上成功查到会话 %d 次，连接%s\n",
                            hold_polls.load(),
                            hold_died_ms.load() < 0 ? "整场没断" : "中途断了");
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
