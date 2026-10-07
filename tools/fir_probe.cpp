// 研究探针：比较 none、PLI、标准 FIR、NACK 和 RR 等实验臂，记录请求与 IRAP 的时间关系。
//
// 标准 FIR 使用 RFC 5104 §4.3.1 的 PT=206/PSFB、FMT=4、20 字节单项 FCI，
// 由 scrctl::rt::build_fir 构造。fir205 仅将同一布局的 PT 改为 205，是非标准实验臂。
// 旧版探针的 16 字节 FIR 和旧公共构造器的 24 字节 FIR 均不是标准布局，
// 这些包得到的负面结果不能作为标准 FIR 无效的证据。
//
// 2026-10-07 的单设备标准 FIR 观察中，29 次请求期间出现 26 个 IDR，首次计时请求后
// 32ms 观察到 IDR；同时发送 RR 时会话存活 30 秒，跨过协商的 20 秒空闲超时。
// 当轮 allowRTCPFB=0，没有同窗口 none/PLI 对照，不能据此判断 FIR 比 PLI 更有效，
// 或把存活归因于 FIR 单独保活。详细验证范围及后续对照见 docs/coredevice.md。
//
// 本探针先观察 3 秒，再每 2 秒发送一次请求，发送阶段总计 12 秒；分别记录前置观察期
// 的 IRAP 和请求后 1.5 秒内的 IRAP。前置观察用于减少起流 IDR 残留对请求计时的影响。
// IRAP 类型 19/20 为 IDR、21 为 CRA；时间相关性仍需结合相同配置的 none 对照解释。
// PLI 表示图像数据丢失，发送端可选择刷新；FIR 请求解码刷新点，不应保证某种 IDR 形态。
//
// 用法：fir_probe [--what none,plim,plim+fb,firm,firm+fb,fir205m] [--attempts N] [--verbose]
// 后缀 m 使用 answer 的协商 SSRC 角色；+fb/+ltrp 分别修改 offer 的对应开关。
// 标准 FIR 臂也需正确的 SSRC 和目的端口，不能只凭字段合法推定设备收到或受理了请求。
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
#include "rt/Rtcp.h"
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

/// 从 answer 的 `connection.streamConfig` 里取一个数。取不到返回 false。
/// 为什么探针需要它：这里面的 `LocalSSRC`/`RemoteSSRC` 是**从设备视角**起的名字（Local 是
/// 设备自己那条流，Remote 是设备给我们这端分配的那个），所以"请求该由谁发出、针对哪条流"
/// 不是我们能编的。编错了人，设备按 SSRC 配对时对不上号，就会把包丢掉——而我们看到的是
/// "设备不理这种请求"。
bool stream_config_u32(const scrctl::xpc::Value &answer, const char *key, uint32_t &out) {
    const auto *conn = answer.find("connection");
    const auto *cfg = conn != nullptr ? conn->find("streamConfig") : nullptr;
    const auto *wrapped = cfg != nullptr ? cfg->find(key) : nullptr;
    if (wrapped == nullptr) {
        return false;
    }
    const auto *inner = wrapped->find("int");
    const scrctl::xpc::Value &v = inner != nullptr ? *inner : *wrapped;
    if (v.type == scrctl::xpc::Type::Int64) {
        out = static_cast<uint32_t>(v.int64);
        return true;
    }
    if (v.type == scrctl::xpc::Type::UInt64) {
        out = static_cast<uint32_t>(v.uint64);
        return true;
    }
    return false;
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
    /// 前摇（还没开始发请求的那 3 秒）里来的 IRAP 次数。这一列必须是 0，否则本轮的相关
    /// 性读数不作数——那种 IRAP 与我们无关，而它会伪装成“请求被受理了”。上一版就是这么
    /// 中招的：起流自带的那个 IDR 有几十上百个分片，阶段 1 在第一个分片就 break，剩下的
    /// 分片在阶段 3 的头几毫秒里组装完成，于是连从不发包的 none 臂都报出“IRAP 在 +1ms 到”。
    uint64_t irap_preroll = 0;
    /// 观察窗里“这个 AU 含 IRAP”发生了**几次**。注意不能只记“有没有”：画面在动时
    /// 每一次都要重新数，否则一次自发 IDR 就能让后面所有臂都看起来"成功"。
    uint64_t irap_events = 0;
    /// 其中落在"我们上一次发请求之后 1.5 秒内"的次数。这一列才是判据：它把"设备给了
    /// 一个 IDR"和"设备是因为我们才给"分开。对照组 `none` 从不发请求，所以它的这一列
    /// 必然是 0，而它的 `irap_events` 就是那条流**自己**的 IDR 产生率。
    uint64_t irap_after_send = 0;
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

    // 把 --what 按逗号拆开（不用子串匹配：那会让 "pli" 命中 "pli1"）。
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

    for (int round = 0; round < attempts; ++round) {
        for (const std::string &w : arms_to_run) {
            // 臂名形如 `<请求>[m][+<offer 开关>]'：
            //   请求   none | pli | pli1 | fir | fir205（非标准改 PT 实验）| nack | rr
            //   m      发送者 SSRC 用 answer 给我们分配的 `RemoteSSRC`、被请求的流填
            //          设备的 `LocalSSRC`（不带 m 的臂沿用老写法，两边都填设备的流号，
            //          留着当"填错人"的对照）
            //   +fb    offer 里申报 allowRTCPFB=1，用于与未启用该位的实验臂对照
            const auto plus = w.find('+');
            std::string base = plus == std::string::npos ? w : w.substr(0, plus);
            const std::string flags = plus == std::string::npos ? "" : w.substr(plus + 1);
            const bool fb = flags.find("fb") != std::string::npos;
            const bool ltrp = flags.find("ltrp") != std::string::npos;
            if (base == "fir205" || base == "fir205m") {
                std::printf("[%s] 非标准实验臂：标准 FIR 布局的 PT 从 206 改为 205\n", w.c_str());
            }
            const bool mine_ssrc = !base.empty() && base.back() == 'm';
            if (mine_ssrc) {
                base.pop_back();
            }

            scrctl::media::StreamSession::Request req;
            req.offer.allow_rtcp_fb = fb;
            req.offer.ltrp_enabled = ltrp;
            std::string start_err;  // 单独一个串：复用 err 会把上一次读包超时的话当成失败原因
            auto session = scrctl::media::StreamSession::start(*dev, req, start_err, verbose);
            if (!session) {
                std::fprintf(stderr, "[%s] 起流失败: %s\n", w.c_str(), start_err.c_str());
                std::this_thread::sleep_for(2s);
                continue;
            }
            Attempt at;
            at.what = w;
            const uint64_t session_t0 = now_ms();
            scrctl::rt::HevcRtpDepacketizer dp(session->started().payload_type);
            std::vector<uint8_t> packet, annexb;
            uint16_t peer = 0;
            uint32_t media_ssrc = 0;
            uint16_t highest_seq = 0;
            uint64_t last_video = now_ms();
            bool got_idr = false;

            // 阶段 1：等到起流自带的那个 IDR，确认这条流是好的。
            //
            // 内层收包循环必须自己带退出条件。这一整套探针（阶段 1/2/3 三处）原先都只靠
            // 外层那次 `while (next_packet(...))` 收空来退出，而"收空"这件事在以前**总是
            // 发生**——因为流 20 秒就死了。把租期改成我们自己报的数、并且报一个大的之后，
            // 画面一直在动的会话会让这一层永不退出，探针就卡死在收包里。也就是说这些循环
            // 从来没有正确过，只是被那条 20 秒的租期掩护着。
            for (auto until = clock::now() + 6s; clock::now() < until; std::this_thread::sleep_for(5ms)) {
                while (!got_idr && session->next_packet(packet, peer, 30, err)) {
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

            // 阶段 2：最多等待 8 秒，尝试获得视频静默窗口，超时后仍继续观察。
            // 请求后 IRAP 的时间关系需结合对照判断；画面活动和会话存活分别记录，
            // 旧版非标准 FIR 的结果不用于推定当前标准请求是否受理。
            const uint64_t quiet_deadline = now_ms() + 8000;
            while (now_ms() < quiet_deadline && now_ms() - last_video < 2500) {
                while (session->next_packet(packet, peer, 200, err)) {
                    if (now_ms() >= quiet_deadline || now_ms() - last_video >= 2500) {
                        break;  // 内层循环要自己会退出，理由见阶段 1 那段
                    }
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
                // 画面一直没静止。**不跳过这一轮**，改成用相关性判据（见阶段 3 那段）：
                // 静止判据只是让"对照组的 0"变得显然，而它不是必要条件。
                std::printf("[%s] 8 秒内画面没静止过（上一帧距今 %llums），改用\"IRAP 是否跟着"
                            "请求来\"的判据\n", w.c_str(),
                            static_cast<unsigned long long>(now_ms() - last_video));
            } else {
                std::printf("[%s] 已静默 %llums（起流至今 %llu ms），发请求\n", w.c_str(),
                            static_cast<unsigned long long>(now_ms() - last_video),
                            static_cast<unsigned long long>(now_ms() - session_t0));
            }

            // 阶段 3：确定协商 SSRC 后发送请求，并观察 IRAP 的时间关系。
            // 请求里的两个 SSRC。answer 的 `LocalSSRC` 是设备自己那条流（实测与 RTP 头里
            // 那个数相等），`RemoteSSRC` 是设备给我们这端分配的——所以"填对自己"的写法是
            // 发送者=RemoteSSRC、被请求的流=LocalSSRC。不带 m 的臂两个位置都填设备的流号，
            // 那是设备按 SSRC 配对时对不上号的写法，留着当对照。
            uint32_t neg_local_ssrc = 0, neg_remote_ssrc = 0;
            const bool has_local =
                stream_config_u32(session->started().answer, "LocalSSRC", neg_local_ssrc);
            const bool has_remote =
                stream_config_u32(session->started().answer, "RemoteSSRC", neg_remote_ssrc);
            const bool roles_ok = mine_ssrc && has_local && has_remote;
            const uint32_t sender_ssrc = roles_ok ? neg_remote_ssrc : media_ssrc;
            const uint32_t target_ssrc = roles_ok ? neg_local_ssrc : media_ssrc;
            std::printf("[%s] SSRC 角色：%s（发送者=%08x 被请求的流=%08x）\n", w.c_str(),
                        roles_ok ? "用 answer 分配的" : "两个位置都填设备的流号（对照）",
                        sender_ssrc, target_ssrc);
            // 阶段 3：先观察 3 秒，再按 2 秒周期发请求，总观察窗口为 12 秒。
            // 记录前置窗口 IRAP，减少起流 IDR 被误计为请求效果的机会。
            constexpr uint64_t kPreRollMs = 3000;
            constexpr uint64_t kWindowMs = 12000;
            constexpr uint64_t kSendEveryMs = 2000;
            uint8_t fir_seq = 1;

            const uint64_t t0 = now_ms();
            uint64_t next_send = t0 + kPreRollMs;
            uint64_t last_send_ms = 0;
            while (now_ms() - t0 < kWindowMs) {
                if (base != "none" && now_ms() >= next_send) {
                    // pli1 只发一次，用来分清"一发就够"和"得反复催"。
                    next_send += (base == "pli1" ? 999000 : kSendEveryMs);
                    std::vector<uint8_t> msg;
                    if (base == "pli" || base == "pli1") {
                        msg = build_pli(sender_ssrc, target_ssrc);
                    } else if (base == "fir") {
                        msg = scrctl::rt::build_fir(sender_ssrc, fir_seq++, target_ssrc);
                    } else if (base == "fir205") {
                        // 非标准实验：保留标准 FCI 布局，仅修改 PT。
                        msg = scrctl::rt::build_fir(sender_ssrc, fir_seq++, target_ssrc);
                        msg[1] = 205;
                    } else if (base == "nack") {
                        msg = build_nack(sender_ssrc, target_ssrc,
                                         static_cast<uint16_t>(highest_seq + 1), 0);
                    } else {
                        msg = build_rr(sender_ssrc, target_ssrc, highest_seq);
                    }
                    std::string serr;
                    const bool ok = session->send_rtp(msg, session->started().sender_port, serr);
                    last_send_ms = now_ms();
                    std::printf("        %4llu ms 发 %s %zu 字节: ",
                                static_cast<unsigned long long>(now_ms() - t0), w.c_str(),
                                msg.size());
                    hex(msg);
                    std::printf(" -> %s\n", ok ? "已发" : serr.c_str());
                }
                while (session->next_packet(packet, peer, 50, err)) {
                    if (now_ms() - t0 >= kWindowMs) {
                        break;  // 内层循环要自己会退出，理由见阶段 1 那段
                    }
                    scrctl::rt::PacketInfo info{};
                    if (!scrctl::rt::parse_rtp_header(packet, info) ||
                        info.payload_type != session->started().payload_type) {
                        continue;
                    }
                    ++at.packets;
                    media_ssrc = info.ssrc;
                    highest_seq = info.sequence;
                    std::ignore = dp.push(packet, annexb, err);
                    // 只扫**这一帧新追加**的那段。annexb 是累加的（depacketizer 把完整 NAL
                    // 追加进来），不清就会把阶段 1 那个起流自带的 IDR 一直留在窗口里，
                    // 之后每一帧都"又看见一次 IRAP"——那种读数会把三个臂全判成成功。
                    const std::set<int> found = irap_in(annexb);
                    annexb.clear();
                    if (!found.empty()) {
                        const uint64_t now = now_ms();
                        for (int t : found) {
                            at.irap.insert(t);
                        }
                        if (now - t0 < kPreRollMs) {
                            // 前摇里来的 IRAP 与我们的请求无关：它是起流那个 IDR 的尾巴，
                            // 或者是这条流自己产的。两种都让本轮的相关性读数变得可疑。
                            ++at.irap_preroll;
                            std::printf("        %4llu ms 收到 IRAP（前摇内，与请求无关）\n",
                                        static_cast<unsigned long long>(now - t0));
                            continue;
                        }
                        ++at.irap_events;
                        // 相关性判据：画面在动的时候"有没有来 IDR"本身没有意义，有意义的
                        // 只有"是不是跟在我们那一个包后面 1.5 秒内来的"。
                        if (last_send_ms != 0 && now - last_send_ms <= 1500) {
                            ++at.irap_after_send;
                            std::printf("        %4llu ms 收到 IRAP（发请求后 %llums）\n",
                                        static_cast<unsigned long long>(now - t0),
                                        static_cast<unsigned long long>(now - last_send_ms));
                        } else {
                            std::printf("        %4llu ms 收到 IRAP，但距上次发请求 %s，算自发\n",
                                        static_cast<unsigned long long>(now - t0),
                                        last_send_ms == 0 ? "从没发过" : "超过 1.5 秒");
                        }
                    }
                }
            }
            std::printf("[%s] 观察 %llu 秒（前摇 3 秒不发包）：视频包 %llu 个，前摇 IRAP %llu 次，"
                        "IRAP 事件 %llu 次（其中跟在请求后 1.5s 内 %llu 次）类型:",
                        w.c_str(), static_cast<unsigned long long>(kWindowMs / 1000),
                        static_cast<unsigned long long>(at.packets),
                        static_cast<unsigned long long>(at.irap_preroll),
                        static_cast<unsigned long long>(at.irap_events),
                        static_cast<unsigned long long>(at.irap_after_send));
            for (int t : at.irap) {
                std::printf(" %d", t);
            }
            std::printf("\n");
            std::string serr;
            session->stop(*dev, serr, verbose);
            results.push_back(at);
        }
    }

    std::printf("\n==== 汇总（前摇 3 秒不发包，之后每 2 秒发一次请求，共 12 秒）====\n");
    std::printf("%-14s %-9s %-11s %-9s %s\n", "臂", "视频包", "前摇 IRAP", "IRAP 次数",
                "其中跟在请求后 1.5s 内");
    for (const auto &r : results) {
        std::printf("%-14s %-9llu %-11llu %-9llu %llu\n", r.what.c_str(),
                    static_cast<unsigned long long>(r.packets),
                    static_cast<unsigned long long>(r.irap_preroll),
                    static_cast<unsigned long long>(r.irap_events),
                    static_cast<unsigned long long>(r.irap_after_send));
    }
    std::printf("\n判读：先看「前摇 IRAP」这一列，它必须全 0——前摇那 3 秒我们一个包都不发，\n"
                "那里来的 IRAP 只可能是起流那个 IDR 的尾巴或者流自己产的，有它在这一轮后面\n"
                "的相关性读数就不作数。然后看最后一列：只有它是 0 以外的数、而且对照行（none）\n"
                "那一列是 0，才说明设备是因为我们发的那一个包才给的 IDR。\n"
                "「IRAP 次数」非 0 而最后一列是 0，说明这条流自己在产 IDR（画面在动时本来就该\n"
                "怀疑这件事），这种画面下测不出请求受理与否，要换一个真静止的画面。\n");
    return 0;
}
