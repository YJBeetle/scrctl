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
                                uint32_t packets_received) {
    // w2 = (RTP 时间戳 >> 8) << 16；w3 = 上一帧的包数；
    // w4 = (到达墙钟毫秒 << 16) | 到达间隔抖动；w5 = (累计包数 << 16) | 0xEA61 定值。
    //
    // w4 那个"墙钟"要按**和 RTP 时间戳同一个时基**算：参考实现试过用"自起流以来的毫秒"
    // 填，设备据此算出一个 14–50ms 的假单程时延（VCRC 里看得见），当成拥塞就开始砍帧率。
    // 视频流的 RTP 时间戳是 24kHz，所以到达时刻取 ts/24 ——本地隧道里"到达≈发出"，
    // 于是单程时延≈0，这是诚实值而不是糊弄。抖动报 0 同理。
    const uint32_t ts = last_rtp_ts;
    const uint32_t w2 = ((ts >> 8) & 0xFFFF) << 16;
    const uint32_t w3 = last_frame_pkts & 0xFFFFFFFF;
    const uint32_t arrival_ms = (ts / 24) & 0xFFFF;
    const uint32_t w4 = (arrival_ms << 16) | 0;
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
        } else if (a == "--attempts" && i + 1 < argc) {
            attempts = std::stoi(argv[++i]);
        } else if (a == "--what" && i + 1 < argc) {
            what = argv[++i];
        } else if (a == "-v" || a == "--verbose") {
            verbose = true;
        }
    }

    // 自检：这条探针的全部结论都建立在"我发出去的包是合法的"上面，而它已经两次栽在
    // 长度字段写成 32 位上（docs §13）。所以先把三种包的字节数和头里的 length 域打出来
    // 核对，对不上就直接不作数——宁可这一轮白跑，也不要再拿畸形包去证明"设备不理"。
    {
        const auto rr = build_rr(0x11111111u, 0x22222222u, 3);
        const auto sd = build_sdes(0x11111111u);
        const auto sr = build_sr(0x11111111u, 0, 0);
        const auto rctl = build_rctl(0x11111111u, 0x22222222u, 10, 100);
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
            req.timeout_seconds = timeout_seconds;
            auto session = scrctl::media::StreamSession::start(*dev, req, start_err, verbose);
            if (!session) {
                std::fprintf(stderr, "[%s] 起流失败: %s\n", w.c_str(), start_err.c_str());
                std::this_thread::sleep_for(2s);
                continue;
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
            std::printf("  请求 timeout=%u -> answer RTCPTimeoutInterval=%s RTCPTimeoutEnabled=%s\n",
                        timeout_seconds,
                        has_interval ? std::to_string(rtcp_interval).c_str() : "(没有)",
                        rtcp_enabled == 1 ? "真" : (rtcp_enabled == 0 ? "假/没读到" : "其它"));
            if (has_interval && rtcp_interval != timeout_seconds) {
                std::printf("  两者不等：设备没有照抄我们报的那个数\n");
            }
            if (has_source_port && neg_source_port != session->started().sender_port) {
                std::printf("  两个端口不同：rrsrc* 那几臂发到 streamConfig.SourcePort\n");
            }

            const uint64_t until = t0 + static_cast<uint64_t>(seconds) * 1000;
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
                    const auto rctl =
                        build_rctl(mine_ssrc && has_remote ? neg_remote_ssrc : our_ssrc,
                                   rtp_last_ts, last_frame_pkts, rtp_packets);
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
                    std::printf("  +%3llus 视频包 %6llu SR 心跳 %4llu 发出 RTCP %5llu（租期 %us）\n",
                                static_cast<unsigned long long>((now_ms() - t0) / 1000),
                                static_cast<unsigned long long>(video_seen),
                                static_cast<unsigned long long>(sr_seen),
                                static_cast<unsigned long long>(rtcp_sent), timeout_seconds);
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
