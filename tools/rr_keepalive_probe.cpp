// 比较 USB 隧道上的 RTCP 反馈、会话查询及关键帧请求策略。
// 各对照臂交替运行，分别统计视频 RTP、设备 SR、IDR、主动请求和观察结束时的会话状态。
// SR 到达证明当时存在设备反馈，不能单独保证整个媒体会话正常；结束状态另由 RPC 查询。
// IDR 计数与会话存活是不同指标，请结合 none 对照及完整观察窗口解释结果。
//
// 早期约 20 秒断流及“RTCP 无效”的实验发生在 UDP 发送修复之前，不能作为当前结论。
// 原始记录、UDP 修复后对照及当前边界见 docs/coredevice.md §13「UDP 修复后的反馈对照」
// 和 §30.3「同配置 RR / PLI / 标准 FIR 的三轮对照」。
// 完整参数见 --help；--help 和 --dump-packets 均在设备连接前退出。
#include <CLI/CLI.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "hid/Hid.h"
#include "i18n/CliLanguage.h"
#include "i18n/Translation.h"
#include "media/StreamSession.h"
#include "net/UdpSocket.h"
#include "remote/Device.h"
#include "rt/Rtcp.h"
#include "rt/RtpHevc.h"

namespace {

using namespace std::chrono_literals;
using clock = std::chrono::steady_clock;
// 标准 RTCP 包复用 src/rt/Rtcp.cpp，避免探针与产品路径的字节格式不一致。
// 本文件只保留实验使用的 AVConference 私有 APP 包构造器。
using scrctl::rt::build_rr;
using scrctl::rt::build_sdes;
using scrctl::rt::build_sdes_cname;
using scrctl::rt::build_sr;
using scrctl::rt::is_rtcp_sr;
using scrctl::rt::build_fir;
using scrctl::rt::build_pli;

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

/// RTCP 公共头使用 16 位大端 length，值为整包的 32 位字数减一（RFC 3550 §6.1）。
/// 不可用 put32 写 length，否则会造成后续字段错位。
void put16(std::vector<uint8_t> &v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}

/// 从 answer.connection.streamConfig 读取数值；缺失或类型不支持时返回 false。
/// 同时接受 Bool，RTCPTimeoutEnabled 的实测回复使用这种类型。
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

/// 同为绝对单调时刻，取字符串项（`TxCodecFeatureListString` 这种）。
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

/// 打印同步令牌和 IsltrpEnabled，供音视频对照使用。
/// 参考抓包中视频与音频的令牌不同，探针也曾观察到同样形状；这些字段本身
/// 不证明同步已经完成，也不能据此保证会话存活。历史实验见 docs/coredevice.md §13。
void print_sync_tokens(std::string_view tag, const scrctl::xpc::Value &answer) {
    uint32_t sync = 0, vsync = 0, isltrp = 0;
    const bool has_sync = stream_config_u32(answer, "SyncStreamToken", sync);
    const bool has_vsync =
        stream_config_u32(answer, "VideoSynchronizationSourceStreamToken", vsync);
    stream_config_u32(answer, "IsltrpEnabled", isltrp);
    std::printf("    [%s] SyncStreamToken=%s VideoSynchronizationSourceStreamToken=%s "
                "IsltrpEnabled=%u\n",
                std::string(tag).c_str(),
                has_sync ? std::to_string(sync).c_str() : SCRCTL_TR("(missing)"),
                has_vsync ? std::to_string(vsync).c_str() : SCRCTL_TR("(missing)"), isltrp);
}

/// 实验性 AVConference 反馈：RTCP APP（PT=204），名字 RCTL，32 字节。
/// 字段解释来自参考抓包，尚无设备端正式协议保证；不能由这些字段推断所有设备行为。
/// 使用已收到的 RTP 时间戳；主循环按 20Hz 发送，并在 RTP marker 时发送伴随包。
/// 抓包对齐及 UDP 修复前后的实验边界见 docs/coredevice.md §13。
std::vector<uint8_t> build_rctl(uint32_t our_ssrc, uint32_t last_rtp_ts, uint32_t last_frame_pkts,
                                uint32_t packets_received, uint32_t clock_1024) {
    // w2 = (RTP 时间戳 >> 8) << 16；w3 = 上一帧包数。
    // w4 高 16 位为 1024Hz 本地单调钟，低位抖动填 0；w5 高位为累计包数，低位为 60001。
    // 这些取值与现有抓包主值对齐，16 位字段会回绕；它们的全部设备端语义尚未确认。
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

/// 16 字节 RTCP APP 伴随包：name 为整数 5，载荷为已收到的 RTP 时间戳。
/// 本探针按 marker 位触发，保持现有参考抓包对照的节奏。
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

/// 按 path = value 输出 XPC 树叶子，避免 describe 对字典条目的截断遗漏诊断字段。
/// 深度最多为 6；字符串和设备字段原样输出，数据/UUID 仅显示字节数。
void walk(const scrctl::xpc::Value &v, const std::string &path, int depth) {
    if (depth > 6) {
        return;  // 限制诊断递归深度，不保证打印完整设备树。
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
        std::printf("    %s = %s\n", path.c_str(), v.boolean ? SCRCTL_TR("true") : SCRCTL_TR("false"));
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
        std::printf(SCRCTL_TR("    %s = <%zu bytes>\n"), path.c_str(), v.data.size());
        break;
    default:
        std::printf("    %s = (type %08x)\n", path.c_str(), static_cast<unsigned>(v.type));
        break;
    }
}

/// 在拆包后的 Annex-B 中检查 HEVC IDR（NAL type 19 / 20）。
/// 复用产品 HevcRtpDepacketizer 后再检查，不把任何视频 RTP 或 FU 分片当作 IDR。
bool annexb_has_idr(const std::vector<uint8_t> &b) {
    for (std::size_t i = 0; i + 4 < b.size(); ++i) {
        if (b[i] != 0 || b[i + 1] != 0 || b[i + 2] != 0 || b[i + 3] != 1) {
            continue;
        }
        const unsigned type = (b[i + 4] >> 1) & 0x3Fu;
        if (type == 19 || type == 20) {
            return true;
        }
    }
    return false;
}

/// 关键帧请求前的视频静默阈值。静默后的 IDR 仍需结合对照判断，不能仅按先后顺序归因。
constexpr uint64_t kPliQuietMs = 2500;

struct Arm {
    std::string what;
    /// 历史字段名；实际表示收到过任意匹配 PT 的视频 RTP，不能据此判断 IDR。
    bool got_idr = false;
    /// 首次计时关键帧请求的绝对单调时刻；打印时减去本轮起点。
    uint64_t first_request_ms = 0;
    /// 本轮所有臂都记录拆包后 IDR 的数量，作为请求效果的对照基线。
    uint64_t idr_packets = 0;
    uint64_t idr_after_request_ms = 0;
    uint64_t requests_sent = 0;
    uint64_t last_video_ms = 0;   // 绝对单调时刻，输出时减去 t0
    uint64_t last_sr_ms = 0;      // 同为绝对单调时刻
    uint64_t srs_after_video = 0;  // 视频静默超过 1500ms 时累计收到的 SR；可能跨多个静默区间
    uint64_t polls_alive = 0;     // poll 臂：查会话表答"还在"的次数
    uint64_t last_alive_ms = 0;   // 最后一次答"还在"的时刻
    bool alive_at_end = false;
    std::string note;
};

void print_arm(const Arm &a, uint64_t t0) {
    std::printf(SCRCTL_TR("  [%s] video received=%s IDRs=%llu last video +%llums last SR +%llums SRs during video quiet periods=%llu session at end=%s"),
                a.what.c_str(), a.got_idr ? SCRCTL_TR("yes") : SCRCTL_TR("no"),
                static_cast<unsigned long long>(a.idr_packets),
                static_cast<unsigned long long>(a.last_video_ms > t0 ? a.last_video_ms - t0 : 0),
                static_cast<unsigned long long>(a.last_sr_ms > t0 ? a.last_sr_ms - t0 : 0),
                static_cast<unsigned long long>(a.srs_after_video),
                a.alive_at_end ? SCRCTL_TR("present") : SCRCTL_TR("absent"));
    if (a.requests_sent != 0) {
        std::printf(SCRCTL_TR(" keyframe requests=%llu first at +%llums subsequent IDR "),
                    static_cast<unsigned long long>(a.requests_sent),
                    static_cast<unsigned long long>(a.first_request_ms > t0
                                                        ? a.first_request_ms - t0
                                                        : 0));
        // 运行期字符串通过 %s 打印，不能把其中的百分号当作格式说明。
        const std::string got = a.idr_after_request_ms == 0
            ? std::string(SCRCTL_TR("not observed"))
            : ("+" + std::to_string(a.idr_after_request_ms) + SCRCTL_TR("ms later"));
        std::printf("%s", got.c_str());
    }
    if (a.polls_alive != 0) {
        std::printf(SCRCTL_TR(" session polls reporting alive=%llu last at +%llums"),
                    static_cast<unsigned long long>(a.polls_alive),
                    static_cast<unsigned long long>(a.last_alive_ms > t0 ? a.last_alive_ms - t0 : 0));
    }
    std::printf("%s\n", a.note.empty() ? "" : (" " + a.note).c_str());
}

}  // namespace

/// 打印设备会话表和客户端 UUID 归属，协助区分本探针与其他客户端创建的条目。
/// type/timeout/PT 是辅助线索，缺少某键不能单独证明创建路径；最终查询也不能还原
/// 更早断流的原因。原始归属排查见 docs/coredevice.md §13。
void dump_sessions(scrctl::remote::Device &dev, const std::vector<uint8_t> &our_uuid,
                   const char *tag) {
    std::string qerr;
    const auto st = scrctl::media::StreamSession::status(dev, qerr, false);
    const auto *ss = st.find("sessions");
    const std::size_t n = ss == nullptr ? 0 : ss->array.size();
    std::printf(SCRCTL_TR("  [%s] device session table: %zu entries\n"), tag, n);
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
        std::printf(SCRCTL_TR("    [%zu] %s PT=%llu type=%s timeout key=%s run duration=%llus\n"), i,
                    mine ? SCRCTL_TR("ours") : SCRCTL_TR("other client"),
                    pt == nullptr ? 0ULL : static_cast<unsigned long long>(pt->uint64),
                    type == nullptr ? SCRCTL_TR("(missing; creation path may differ)") : type->string.c_str(),
                    timeout == nullptr ? SCRCTL_TR("no") : SCRCTL_TR("yes"),
                    run == nullptr ? 0ULL : static_cast<unsigned long long>(run->uint64));
    }
}




int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int seconds = 30;
    int attempts = 2;
    // startmediastream 请求的 timeout，默认 20 秒；对照其与 answer 超时字段的关系。
    // 请求值不等于已证明的硬租期。早期断流解释受 UDP 发送缺陷影响，见 docs/coredevice.md §13。
    uint32_t timeout_seconds = 20;
    // 完全省略 timeout 键，与发送 0 不同。已测 feature 路径曾以 code 4865
    // 和 Expected to find key timeout 拒绝；保留选项以复现该边界。
    // 其他客户端会话缺少此键，不能单独证明计时器或创建路径的具体行为。
    bool no_timeout_key = false;
    /// 输出固定 RTCP 样本的十六进制，在设备连接前退出，供抓包对照。
    bool dump_packets = false;
    /// 音频 RTP 原样落盘，每包前加大端 u16 长度，供离线检查头部和载荷。
    /// 包计数表示收到流量，不代表已识别编码或成功解码。
    std::string audio_out;
    // 在请求中附加 XPC UUID 类型的 sessionEventChannel，作为独立对照变量。
    bool event_channel = false;
    /// 保持起流连接，并由同一线程持续 service 和定期查询状态。
    /// 此选项区分连接生命周期与媒体反馈；持有句柄本身不代表已经响应设备控制消息。
    bool hold_connection = false;
    /// 保持显示服务连接，不创建媒体流；具体观测见使用处。
    bool hold_idle = false;
    /// 禁用保持连接上的 5 秒状态查询，仅保留 service，用于隔离主动 RPC 的影响。
    bool hold_no_poll = false;
    /// 先起音频，再以相同 avcMediaStreamOptionClientSessionID 起视频，供分组对照。
    /// 历史实验曾得到与参考抓包相似的同步令牌，但 UDP 修复前的断流结果
    /// 不能证明音视频分组不影响存活。字段形状也不是同步完成的充分证据。
    /// 原始数据及当前结论见 docs/coredevice.md §13、§17。
    bool audio_leg = false;
    /// 在音频端口每秒发送 RR+SDES；独立于仅创建音频的开关，避免混合对照变量。
    bool audio_rr = false;
    /// 文件字节原样作为 negotiatorOffer，绕过构造器，不假定其使用 binary 或 XML plist。
    std::string raw_offer_path;
    std::vector<uint8_t> raw_offer;
    /// 在 com.apple.coredevice.deviceinfo 上订阅 displayinfoupdates。
    /// 它与媒体起流使用独立连接，用于观察显示更新及订阅生命周期。
    /// 参考客户端存在此连接，但其存在不证明它负责媒体保活；早期约 20 秒
    /// 断流假设受 UDP 缺陷影响，历史过程见 docs/coredevice.md §13。
    bool display_subscribe = false;
    /// 起流前附着 universalhidservice，并保持连接但不发送输入。
    /// 该对照只检查 HID 附着的影响；参考客户端的调用顺序不构成所有设备的起流前提。
    /// 虚拟显示面的字段和历史观察见 docs/coredevice.md §13。
    bool hid_attach = false;
    /// 向指定设备端口发送 3 个 UDP 数据报，检查隧道写入及可观察的设备反馈。
    /// UDP 修复后曾收到 3 个 ICMPv6 port-unreachable；无回复仍不能单独证明未投递。
    bool udp_canary = false;
    /// 起流前发送 5 个 ICMPv6 echo request；ICMP 应答不能代替 UDP 路径验证。
    bool ping6 = false;
    /// 向设备 5353 端口单播 DNS 查询。有回复证明本次往返，无回复可能有多种原因。
    bool udp_mdns = false;
    /// 设置随机的 20 位 IPv6 流标签，作为独立网络变量；随机结果也可能为 0。
    /// UDP 修复前的零收包及断流结果不能证明流标签无效，历史记录见 docs/coredevice.md §13。
    bool flow_label = false;
    /// 即使视频仍在到达也发送 PLI/FIR，跳过 2.5 秒静默条件；其它计时规则不变。
    bool request_always = false;
    uint16_t canary_port = 47891;
    // AVC feature 字符串原样放入 offer，并打印 answer 回显以检查协商结果。
    // 默认值来自既有实现；回显不能证明每项 feature 已被设备启用。
    std::string avc_features = "FLS;SW:1;";
    // 视频 RR 和 PLI/FIR 的请求频率，默认 1Hz；RCTL 和音频 RR 保留各自固定周期。
    // 可改变频率做对照，但单次存活或断流不能证明某个通用设备计时模型。
    double hz = 1.0;
    bool dump_status = false;
    std::string what = "none,rrsrc,rrsrcsd";
    bool verbose = false;
    CLI::App cli{SCRCTL_N_("Compare RTCP keepalive and keyframe request strategies over USB")};
    cli.set_help_flag("-h,--help", SCRCTL_N_("Show help and exit without connecting to a device"));
    cli.option_defaults()->take_last();
    scrctl::i18n::CliLanguage language(cli);
    // --hold 的等待时间以 int 毫秒保存，参数上限同时保护现有的加法和乘法。
    cli.add_option("--seconds", seconds, SCRCTL_N_("Observation seconds per arm (default: 30)"))
        ->check(CLI::Range(1, std::numeric_limits<int>::max() / 1000 - 30));
    cli.add_option("--attempts", attempts, SCRCTL_N_("Number of rounds (default: 2)"))
        ->check(CLI::PositiveNumber);
    cli.add_option("--hz", hz, SCRCTL_N_("RR and keyframe request frequency in Hz (default: 1)"))
        ->check(CLI::Validator([](std::string &value) {
            double frequency = 0;
            if (!CLI::detail::lexical_cast(value, frequency) || !std::isfinite(frequency) ||
                frequency <= 0 ||
                1000.0 / frequency >= static_cast<double>(std::numeric_limits<long long>::max())) {
                return std::string(SCRCTL_TR("must be a finite positive frequency with a representable millisecond period"));
            }
            return std::string{};
        }, "FINITE POSITIVE"));
    cli.add_option("--timeout", timeout_seconds, SCRCTL_N_("Negotiation timeout in seconds (default: 20)"));
    cli.add_option("--what", what, SCRCTL_N_("Comma-separated arms (default: none,rrsrc,rrsrcsd)"))
        ->check(CLI::Validator([](std::string &value) {
            static const std::set<std::string> bases = {
                "none", "poll", "poll5", "rr", "rrsame", "rrsdes", "rrp1", "rrall",
                "rrneg", "rrnegp1", "rrnegsr", "rrmine", "rrminep1", "rrminesr",
                "rrminesd", "rrminecname", "rrsrc", "rrsrcsd", "rctl", "rctlrr", "pli", "fir"};
            if (value.empty()) return std::string(SCRCTL_TR("must contain at least one arm"));
            std::size_t pos = 0;
            do {
                const auto comma = value.find(',', pos);
                auto arm = value.substr(pos, comma == std::string::npos ? comma : comma - pos);
                while (!arm.empty() && arm.front() == ' ') arm.erase(arm.begin());
                const auto plus = arm.find('+');
                if (bases.count(arm.substr(0, plus)) == 0)
                    return std::string(SCRCTL_TR("unknown arm: ")) + arm;
                auto flag_pos = plus;
                while (flag_pos != std::string::npos) {
                    const auto next = arm.find('+', flag_pos + 1);
                    const auto flag = arm.substr(flag_pos + 1,
                        next == std::string::npos ? next : next - flag_pos - 1);
                    if (flag != "fb" && flag != "ltrp")
                        return std::string(SCRCTL_TR("unknown arm flag: ")) + flag;
                    flag_pos = next;
                }
                if (comma == std::string::npos) break;
                pos = comma + 1;
            } while (pos <= value.size());
            return std::string{};
        }, "ARMS"));
    cli.add_flag("--no-timeout-key", no_timeout_key, SCRCTL_N_("Omit the timeout key from the request"));
    cli.add_flag("--dump-packets", dump_packets, SCRCTL_N_("Print packet bytes and exit without connecting"));
    cli.add_flag("--dump-status", dump_status, SCRCTL_N_("Print the full final session status"));
    cli.add_option("--audio-out", audio_out, SCRCTL_N_("Write length-prefixed audio RTP packets to FILE"));
    cli.add_flag("--audio-leg", audio_leg, SCRCTL_N_("Start audio with the same client session ID as video"));
    cli.add_flag("--audio-rr", audio_rr, SCRCTL_N_("Start audio and send RR+SDES on its port at 1 Hz"));
    cli.add_option("--offer", raw_offer_path, SCRCTL_N_("Use FILE bytes as the negotiator offer"));
    cli.add_option("--avc-features", avc_features, SCRCTL_N_("AVConference feature string (default: FLS;SW:1;)"));
    cli.add_flag("--event-channel", event_channel, SCRCTL_N_("Include a session event channel UUID"));
    cli.add_flag("--hold", hold_connection, SCRCTL_N_("Keep the stream-start connection open and service it"));
    cli.add_flag("--hold-idle", hold_idle, SCRCTL_N_("Keep a display connection open without starting media"));
    cli.add_flag("--hold-no-poll", hold_no_poll, SCRCTL_N_("Disable status polling on the held connection"));
    cli.add_flag("--display-subscribe", display_subscribe, SCRCTL_N_("Subscribe to display updates before starting"));
    cli.add_flag("--hid-attach", hid_attach, SCRCTL_N_("Attach HID before starting, without sending input"));
    cli.add_flag("--ping6", ping6, SCRCTL_N_("Send five ICMPv6 echo requests before starting"));
    cli.add_flag("--udp-mdns", udp_mdns, SCRCTL_N_("Send a unicast DNS query to device port 5353"));
    auto *canary = cli.add_option("--udp-canary", canary_port,
        SCRCTL_N_("Send three UDP datagrams to optional PORT (default: 47891)"))
        ->expected(0, 1)->default_str("47891")->type_name("PORT");
    cli.add_flag("--flow-label", flow_label, SCRCTL_N_("Set a random IPv6 flow label"));
    cli.add_flag("--request-always", request_always, SCRCTL_N_("Request keyframes even while video is arriving"));
    cli.add_flag("-v,--verbose", verbose, SCRCTL_N_("Print verbose protocol logging"));
    cli.footer(SCRCTL_N_("Arms: none, poll, poll5, rr, rrsame, rrsdes, rrp1, rrall, rrneg, rrnegp1,\nrrnegsr, rrmine, rrminep1, rrminesr, rrminesd, rrminecname, rrsrc, rrsrcsd,\nrctl, rctlrr, pli, fir. Append +fb and/or +ltrp to enable offer flags.\nWithout --help or --dump-packets, this probe connects to the USB device."));
    try {
        cli.parse(argc, argv);
        if (!language.select()) return 2;
    } catch (const CLI::CallForHelp &) {
        if (!language.select()) return 2;
        std::printf("%s", language.help().c_str());
        return 0;
    } catch (const CLI::ParseError &e) {
        if (language.select()) {
            std::fprintf(stderr, SCRCTL_TR("Invalid arguments: %s\n"), e.what());
            return e.get_exit_code();
        }
        return 2;
    }
    udp_canary = canary->count() != 0;
    audio_leg = audio_leg || audio_rr;

    // 请求及诊断共享同一 timeout 表示，区分省略键与数值 0。
    const std::optional<uint32_t> lease =
        no_timeout_key ? std::optional<uint32_t>{} : std::optional<uint32_t>{timeout_seconds};
    const std::string lease_text =
        no_timeout_key ? SCRCTL_TR("timeout key omitted") : std::to_string(timeout_seconds) + "s";

    // 使用 v4 UUID 的版本/变体位，并以 XPC UUID 交付；字符串 UUID 不等同于该类型。
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
        std::printf(SCRCTL_TR("sessionEventChannel = %s\n"), hex);
        event_channel_uuid = u;
    }

    // 在连接设备前检查固定样本的字节数及 RR length；失败时本轮退出。
    // 长度检查只覆盖包形状，不能证明私有字段语义或设备已经接受反馈。
    {
        const auto rr = build_rr(0x11111111u, 0x22222222u, 3);
        const auto sd = build_sdes(0x11111111u);
        const auto sr = build_sr(0x11111111u, 0, 0);
        const auto rctl = build_rctl(0x11111111u, 0x22222222u, 10, 100, 0);
        const auto comp = build_rctl_companion(0x11111111u, 0x22222222u);
        const int rr_len_field = (rr[2] << 8) | rr[3];
        std::printf(SCRCTL_TR("Packet self-check: RR %zu bytes (length=%d) RR+SDES %zu bytes SR %zu bytes RCTL %zu bytes Companion %zu bytes\n"),
                    rr.size(), rr_len_field, rr.size() + sd.size(), sr.size(), rctl.size(),
                    comp.size());
        if (rr.size() != 32 || rr_len_field != 7 || sd.size() != 12 || sr.size() != 28 ||
            rctl.size() != 32 || comp.size() != 16) {
            std::fprintf(stderr, SCRCTL_TR("Unexpected packet shape (expected RR 32 / SDES 12 / SR 28 / RCTL 32 / Companion 16, RR length 7); this round is invalid\n"));
            return 2;
        }
        // 十六进制输出用于与参考抓包逐字段对照；固定样本顺序和内容保持稳定。
        if (dump_packets) {
            const auto hex = [](const std::vector<uint8_t> &b) {
                std::string s;
                static constexpr char kDigits[] = "0123456789abcdef";
                for (std::size_t i = 0; i < b.size(); ++i) {
                    if (i % 4 == 0) {
                        s += i == 0 ? "" : " ";
                    }
                    s += kDigits[b[i] >> 4];
                    s += kDigits[b[i] & 0xF];
                }
                return s;
            };
            std::printf("RR        %s\n", hex(rr).c_str());
            auto compound = rr;
            compound.insert(compound.end(), sd.begin(), sd.end());
            std::printf("RR+SDES   %s\n", hex(compound).c_str());
            std::printf("SR        %s\n", hex(sr).c_str());
            std::printf("RCTL      %s\n", hex(rctl).c_str());
            std::printf(SCRCTL_TR("Companion %s\n"), hex(comp).c_str());
            return 0;
        }
    }

    if (!raw_offer_path.empty()) {
        FILE *f = std::fopen(raw_offer_path.c_str(), "rb");
        if (f == nullptr) {
            std::fprintf(stderr, SCRCTL_TR("Failed to open offer file %s\n"), raw_offer_path.c_str());
            return 1;
        }
        uint8_t buf[4096];
        std::size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
            raw_offer.insert(raw_offer.end(), buf, buf + n);
        }
        std::fclose(f);
        std::printf(SCRCTL_TR("Using %s as raw negotiatorOffer (%zu bytes; offer builder bypassed)\n"),
                    raw_offer_path.c_str(), raw_offer.size());
    }

    std::string err;
    auto dev = scrctl::remote::Device::establish({}, err, verbose);
    if (!dev) {
        std::fprintf(stderr, SCRCTL_TR("Failed to establish device session: %s\n"), err.c_str());
        return 1;
    }

    // ICMP 与 UDP 的投递路径分别观察；收到 ICMP 只能证明对应流量经过隧道。
    // UDP 路径修复与历史误判见 docs/coredevice.md §13「UDP 修复后的反馈对照」。
    if (flow_label) {
        std::random_device rd;
        const uint32_t label = rd() & 0xFFFFFu;
        dev->rsd().stack().set_flow_label(label);
        std::printf(SCRCTL_TR("  Outbound IPv6 flow label: 0x%05x (default: 0)\n"), label);
    }
    if (ping6) {
        auto &st = dev->rsd().stack();
        for (int i = 0; i < 5; ++i) {
            std::string perr;
            const bool ok = st.send_echo_request(0x5343, static_cast<uint16_t>(i + 1), perr);
            if (!ok) {
                std::printf(SCRCTL_TR("  ping6 #%d write failed: %s\n"), i + 1, perr.c_str());
            }
            std::this_thread::sleep_for(400ms);
        }
        std::this_thread::sleep_for(1500ms);
        std::printf(SCRCTL_TR("  ping6 -> %s: ICMP received=%llu echo replies=%llu%s\n"),
                    dev->rsd().stack().peer_text().c_str(),
                    static_cast<unsigned long long>(st.icmp_seen()),
                    static_cast<unsigned long long>(st.echo_replies()),
                    st.echo_replies() > 0 ? SCRCTL_TR(" (non-TCP traffic received through the tunnel)")
                                          : SCRCTL_TR(" (no echo reply; this does not establish UDP failure)"));
        if (!st.icmp_last().empty()) {
            std::printf(SCRCTL_TR("    Last message: %s\n"), st.icmp_last().c_str());
        }
    }

    // DNS 查询使用独立 UDP socket，主循环继续服务隧道以接收回复。
    // 有回复证明本次请求投递；超时可能来自设备策略、查询内容或网络，不能直接判为过滤。
    if (udp_mdns) {
        auto &st = dev->rsd().stack();
        std::unique_ptr<scrctl::net::UdpSocket> sock;
        for (uint16_t p = 39001; p < 39011 && !sock; ++p) {
            auto candidate = std::make_unique<scrctl::net::UdpSocket>(st, p);
            std::string berr;
            if (candidate->bind(berr)) {
                sock = std::move(candidate);
            }
        }
        if (!sock) {
            std::fprintf(stderr, SCRCTL_TR("  udp-mdns: failed to bind a local port\n"));
        } else {
            // DNS 查询头：ID / flags=0 / QDCOUNT=1，其余计数 0；
            // QNAME = _mdns._udp.local，QTYPE=PTR(12)，CLASS=IN(1)。
            const std::vector<uint8_t> qname = {
                5, '_', 'm', 'd', 'n', 's', 4, 'u', 'd', 'p', 5, 'l', 'o', 'c', 'a', 'l', 0};
            std::vector<uint8_t> q;
            const auto put16 = [&q](uint16_t v) {
                q.push_back(static_cast<uint8_t>(v >> 8));
                q.push_back(static_cast<uint8_t>(v));
            };
            put16(0x5c5c);
            put16(0);  // flags：标准查询
            put16(1);  // QDCOUNT
            put16(0);
            put16(0);
            put16(0);
            q.insert(q.end(), qname.begin(), qname.end());
            put16(12);  // PTR
            put16(1);   // IN
            std::string serr;
            int sent = 0;
            for (int i = 0; i < 5; ++i) {
                if (sock->send(q, 5353, serr)) {
                    ++sent;
                }
                std::this_thread::sleep_for(300ms);
            }
            std::vector<uint8_t> reply;
            uint16_t from_port = 0;
            std::string rerr;
            const bool got = sock->recv(reply, from_port, 2500, rerr);
            const std::string verdict =
                got ? (SCRCTL_TR("Reply received: ") + std::to_string(reply.size()) + SCRCTL_TR(" bytes from port ") +
                       std::to_string(from_port) + SCRCTL_TR("; client-to-device UDP delivery confirmed"))
                    : (SCRCTL_TR("No reply (") + rerr + SCRCTL_TR(")"));
            std::printf(SCRCTL_TR("  udp-mdns: local port %u, queries sent=%d, %s\n"),
                        static_cast<unsigned>(sock->local_port()), sent, verdict.c_str());
        }
    }

    // 仅连接 displayservice 并调用 getsupportedmediacapabilities，不起媒体。
    // 先调用后持续 service，可隔离媒体反馈与连接保活；复查是否成功另行计数。
    if (hold_idle) {
        std::string cerr;
        auto conn = dev->connect("com.apple.coredevice.displayservice", cerr, verbose);
        if (conn == nullptr) {
            std::fprintf(stderr, SCRCTL_TR("Failed to open connection: %s\n"), cerr.c_str());
            return 1;
        }
        scrctl::xpc::Value out;
        auto in = scrctl::xpc::make_dict();
        const auto r = conn->invoke("com.apple.coredevice.feature.getmediasupportinfo",
                                    "com.apple.coredevice.action.mediastreamgetsupportinfo", in,
                                    out, 10000, cerr);
        std::printf(SCRCTL_TR("[idle] First call: %s, reply type %d\n"),
                    r == scrctl::remote::CallResult::Ok ? SCRCTL_TR("succeeded") : cerr.c_str(),
                    static_cast<int>(out.type));
        const uint64_t it0 = now_ms();
        uint64_t next_poll = it0 + 5000;
        int polls = 0;
        std::string serr;
        long long died_ms = -1;
        while (now_ms() - it0 < static_cast<uint64_t>(seconds) * 1000) {
            if (!conn->service(200, serr)) {
                died_ms = static_cast<long long>(now_ms() - it0);
                std::printf(SCRCTL_TR("    [idle] Connection closed by peer at +%lldms: %s\n"), died_ms, serr.c_str());
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
                std::printf(SCRCTL_TR("    [idle] +%llus repeat call on the same connection: %s\n"),
                            static_cast<unsigned long long>((now_ms() - it0) / 1000),
                            pr == scrctl::remote::CallResult::Ok ? SCRCTL_TR("succeeded") : qerr.c_str());
                if (pr == scrctl::remote::CallResult::Ok) {
                    ++polls;
                }
            }
        }
        std::printf(SCRCTL_TR("[idle] Result: displayservice connection held for %lldms, %s, successful repeat calls=%d\n"),
                    died_ms >= 0 ? died_ms : static_cast<long long>(now_ms() - it0),
                    died_ms >= 0 ? SCRCTL_TR("closed by device") : SCRCTL_TR("not closed by device"), polls);
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
    std::map<std::string, std::pair<int, int>> tally;  // 对照臂 -> {收到视频包的有效轮数，结束时不存活轮数}

    for (int round = 0; round < attempts; ++round) {
        for (const std::string &w : arms_to_run) {
            // 每个对照臂按 <base>[+flag...] 拆分；合法名称及标志已由 CLI11 校验。
            const auto plus = w.find('+');
            const std::string base = plus == std::string::npos ? w : w.substr(0, plus);
            const std::string flags = plus == std::string::npos ? "" : w.substr(plus + 1);
            const bool fb = flags.find("fb") != std::string::npos;
            const bool ltrp = flags.find("ltrp") != std::string::npos;

            const int poll_every_ms = base == "poll" ? 2000 : (base == "poll5" ? 5000 : 0);
            // 每个 RTCP 变体改变发送者/报告 SSRC、复合包或目的端口：
            // rr：自选 SSRC；rrsame：媒体 SSRC；rrsdes：附加 SDES；rrp1：媒体端口 +1。
            // rrall：媒体 SSRC + SDES + 端口 +1；rrneg：LocalSSRC 发送、RemoteSSRC 报告。
            // rrnegp1/rrnegsr：上述端口/SR 变体；rrmine：RemoteSSRC 发送、LocalSSRC 报告。
            // rrminep1/rrminesr/rrminesd/rrminecname：端口、SR 或 SDES 变体。
            // rrsrc/rrsrcsd：采用 SourcePort；rctl：20Hz RCTL + marker 触发的伴随包。
            // rctlrr：在 RCTL 之外增加 RR；pli/fir：在 rrsrc 的 RR 基线上增加关键帧请求。
            // 已测视频的 LocalSSRC 与设备 RTP SSRC 相等，RemoteSSRC 用作接收端反馈来源。
            // 修复前的负结果不能作为当前 RTCP 无效的证据；当前对照见 docs/coredevice.md §30.3。
            const bool send_pli = base == "pli";
            const bool send_fir = base == "fir";
            const bool send_rr = base.rfind("rr", 0) == 0 || send_pli || send_fir;
            const bool send_rctl = base.rfind("rctl", 0) == 0;
            const bool sdes = base == "rrsdes" || base == "rrall" || base == "rrminesd" ||
                              base == "rrsrcsd" || base == "rctlrr";
            const bool same_ssrc = base == "rrsame" || base == "rrall";
            const bool neg_ssrc = base == "rrneg" || base == "rrnegp1" || base == "rrnegsr";
            const bool mine_ssrc =
                base == "rrmine" || base == "rrminep1" || base == "rrminesr" ||
                base == "rrminesd" || base == "rrminecname" || base == "rrsrc" ||
                base == "rrsrcsd" || base == "rctl" || base == "rctlrr" || send_pli || send_fir;
            // 对应臂优先选择 streamConfig.SourcePort；缺失时保留 sender.port 回退。
            const bool to_source_port =
                base == "rrsrc" || base == "rrsrcsd" || base == "rctl" || base == "rctlrr" ||
                send_pli || send_fir;
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

            // 按参考客户端的顺序先启动音频，再启动视频；两者复用同一个
            // 16 字节 ClientSessionID，以便观察协商字段中的同步关系。
            std::unique_ptr<scrctl::hid::Service> hid_held;
            if (hid_attach) {
                std::string herr;
                hid_held = scrctl::hid::Service::open(*dev, herr, verbose);
                if (hid_held == nullptr) {
                    std::fprintf(stderr, SCRCTL_TR("[%s] HID connection failed: %s (round invalid)\n"), w.c_str(),
                                 herr.c_str());
                    std::this_thread::sleep_for(2s);
                    continue;
                }
                std::vector<scrctl::hid::Service::Surface> faces;
                std::string serr;
                const bool listed = hid_held->surfaces(faces, serr);
                std::printf(SCRCTL_TR("  HID attached without sending input (connectedServices query %s, surfaces=%zu)\n"),
                            listed ? SCRCTL_TR("succeeded") : SCRCTL_TR("failed"), listed ? faces.size() : 0);
            }
            std::vector<uint8_t> shared_session;
            std::unique_ptr<scrctl::media::StreamSession> audio;
            uint32_t audio_sender_ssrc = 0, audio_report_ssrc = 0;
            uint16_t audio_dest_port = 0;
            uint64_t audio_seen = 0;
            uint32_t audio_highest_seq = 0;
            // 音频落盘保留整包 RTP；大端 u16 长度用于离线分割，与产品解码输出不同。
            struct FileCloser {
                int operator()(FILE *f) const { return f == nullptr ? 0 : std::fclose(f); }
            };
            std::unique_ptr<FILE, FileCloser> audio_dump {
                audio_out.empty() ? nullptr : std::fopen(audio_out.c_str(), "wb") };
            if (!audio_out.empty() && audio_dump == nullptr) {
                std::fprintf(stderr, SCRCTL_TR("Failed to open audio output file %s\n"), audio_out.c_str());
            }
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
                    std::fprintf(stderr, SCRCTL_TR("[%s] Failed to start audio stream: %s (round invalid)\n"), w.c_str(),
                                 aerr.c_str());
                    std::this_thread::sleep_for(2s);
                    continue;
                }
                // 按已测 answer 角色取反馈发送者和报告 SSRC，并从 sender.port 取目的端口。
                // 缺失值保留当前回退；输出各值供本次协商核对，不作所有设备保证。
                uint32_t a_remote = 0, a_local = 0;
                stream_config_u32(audio->started().answer, "RemoteSSRC", a_remote);
                stream_config_u32(audio->started().answer, "LocalSSRC", a_local);
                audio_sender_ssrc = a_remote;
                audio_report_ssrc = a_local;
                audio_dest_port = audio->started().sender_port;
                uint32_t audio_pt = audio->started().payload_type;
                std::printf(SCRCTL_TR("  Audio stream started: receive port=%u device send port=%u PT=%u RemoteSSRC=%u LocalSSRC=%u\n"),
                            audio->receiver_port(), audio_dest_port, audio_pt, a_remote, a_local);
                print_sync_tokens(SCRCTL_TR("audio"), audio->started().answer);
                if (!audio_out.empty()) {
                    // 保留完整音频 answer，以检查当前协商的编码与配置；RTP 计数不证明解码。
                    std::printf(SCRCTL_TR("  Full audio answer: %s\n"),
                                scrctl::xpc::describe(audio->started().answer, ~std::size_t { 0 })
                                    .c_str());
                }
                req.client_session_uuid = shared_session;
            }

            // displayinfoupdates 使用独立 deviceinfo 连接，先订阅再起视频。
            // 此连接由订阅线程独占；更新计数只描述本轮实际收到的事件。
            std::atomic<bool> sub_done { false };
            std::atomic<int> sub_elements { 0 };
            std::atomic<bool> sub_failed { false };
            std::thread sub_thread;
            if (display_subscribe) {
                std::string cerr2;
                auto sub = dev->connect("com.apple.coredevice.deviceinfo", cerr2, verbose);
                if (sub == nullptr) {
                    std::fprintf(stderr, SCRCTL_TR("[%s] Failed to connect to deviceinfo: %s\n"), w.c_str(),
                                 cerr2.c_str());
                    sub_failed.store(true);
                } else {
                    std::vector<uint8_t> side(16);
                    for (auto &b : side) {
                        b = static_cast<uint8_t>(std::random_device {} ());
                    }
                    auto proxy = scrctl::xpc::make_dict();
                    scrctl::xpc::dict_set(proxy, "sideChannel",
                                          scrctl::xpc::make_uuid(std::span<const uint8_t>(side)));
                    auto sinput = scrctl::xpc::make_dict();
                    scrctl::xpc::dict_set(sinput, "actualInput", scrctl::xpc::make_dict());
                    scrctl::xpc::dict_set(sinput, "streamProxy", std::move(proxy));
                    const uint64_t sub_t0 = now_ms();
                    // 单次 stream() 的等待上限比媒体观察窗长，避免订阅提前到期。
                    const int hold_ms = (seconds + 30) * 1000;
                    sub_thread = std::thread(
                        [in = std::move(sinput), conn = std::move(sub), &sub_done, &sub_elements,
                         &sub_failed, sub_t0, hold_ms, dev_ptr = &*dev, verbose]() mutable {
                            // 订阅窗口比媒体观察窗长 30 秒；断开后重建连接并订阅。
                            // 该重连只影响显示订阅，不改变媒体控制策略。
                            std::string e2;
                            while (!sub_done.load()) {
                                const auto r = conn->stream(
                                    "com.apple.coredevice.feature.displayinfoupdates", "", in,
                                    [&](const scrctl::xpc::Value &one) {
                                        ++sub_elements;
                                        // 输出完整结构，方便与参考客户端的显示面、方向及背光字段对照。
                                        std::printf(SCRCTL_TR("    [sub] +%lldms display update: %s\n"),
                                                    static_cast<long long>(now_ms() - sub_t0),
                                                    scrctl::xpc::describe(one, ~std::size_t { 0 })
                                                        .c_str());
                                        return !sub_done.load();
                                    },
                                    hold_ms, e2);
                                if (sub_done.load() || r == scrctl::remote::CallResult::Ok) {
                                    return;  // 设备自己发了 finishStreaming，不用再挂
                                }
                                std::printf(SCRCTL_TR("    [sub] +%lldms subscription closed; reconnecting: %s\n"),
                                            static_cast<long long>(now_ms() - sub_t0), e2.c_str());
                                conn = dev_ptr->connect("com.apple.coredevice.deviceinfo", e2,
                                                        verbose);
                                if (conn == nullptr) {
                                    std::printf(SCRCTL_TR("    [sub] Reconnection failed: %s\n"), e2.c_str());
                                    sub_failed.store(true);
                                    return;
                                }
                            }
                        });
                }
            }
            // Channel 的发送/接收没有内部并发锁。起流握手先在主线程完成，
            // 再把同一连接交给保持线程，避免 service 和同步 invoke 争用流。
            std::unique_ptr<scrctl::remote::ServiceConnection> held;
            std::atomic<bool> hold_done { false };
            std::atomic<int> hold_polls { 0 };
            std::atomic<long long> hold_died_ms { -1 };
            std::thread hold_thread;
            if (hold_connection) {
                std::string cerr;
                held = dev->connect("com.apple.coredevice.displayservice", cerr, verbose);
                if (held == nullptr) {
                    std::fprintf(stderr, SCRCTL_TR("[%s] Failed to open connection: %s\n"), w.c_str(), cerr.c_str());
                    std::this_thread::sleep_for(2s);
                    continue;
                }
            }
            auto session = scrctl::media::StreamSession::start(*dev, req, start_err, verbose,
                                                               held.get());
            if (!session) {
                std::fprintf(stderr, SCRCTL_TR("[%s] Failed to start stream: %s\n"), w.c_str(), start_err.c_str());
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
                            std::printf(SCRCTL_TR("    [hold] Connection closed at +%lldms: %s\n"),
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
                                std::printf(SCRCTL_TR("    [hold] +%lldms session entries on the same connection=%zu\n"),
                                            static_cast<long long>(now_ms() - arm_t0),
                                            ss != nullptr && ss->is_array()
                                                ? ss->array.size()
                                                : static_cast<std::size_t>(0));
                            } else {
                                std::printf(SCRCTL_TR("    [hold] +%lldms status query on the same connection failed (%d): %s\n"),
                                            static_cast<long long>(now_ms() - arm_t0),
                                            static_cast<int>(r), qerr.c_str());
                            }
                        }
                    }
                });
            }
            std::printf(SCRCTL_TR("Round %d [%s]: stream started, observing for %d seconds (leave the device idle) offer: allowRTCPFB=%d ltrpEnabled=%d\n"),
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
            // 未收到视频包时 last_video 保持 0，静默时间由发送处回退到起流时刻 t0。
            // 只用来数 IDR 的第二个拆包器：和收流并行跑一份，不参与任何判断路径。
            scrctl::rt::HevcRtpDepacketizer idr_scan(session->started().payload_type);
            uint64_t next_rr = t0;
            uint64_t next_req = t0;
            uint8_t fir_seq = 0;
            const uint64_t rr_period_ms = hz > 0.0 ? std::max(1LL, (long long)(1000.0 / hz)) : 1000;
            uint64_t next_poll = t0;
            // RCTL 那两臂要报的是**真实收到的**东西，所以收包时得记三样：最后一个视频包
            // 的 RTP 时间戳、累计视频包数、以及"上一帧有多少个包"（marker 那一下结算）。
            uint32_t rtp_last_ts = 0;
            uint32_t rtp_packets = 0;
            /// 仅计数成功写入隧道的 RTCP；它不表示设备已接收或接受该包。
            /// 设备 SR 是独立的接收侧观测，需结合结束状态及 none 对照解释。
            uint64_t rtcp_sent = 0;
            uint32_t cur_frame_pkts = 0;
            uint32_t last_frame_pkts = 0;
            uint64_t next_rctl = t0;
            // 自选 SSRC，用于相关对照臂或缺少协商字段时的现有回退。
            const uint32_t our_ssrc = 0x35c0ffeeu;
            // 读取协商 SSRC，按实际 RTP 头核对角色：已测视频 LocalSSRC 为设备发送源，
            // RemoteSSRC 为反馈发送者。字段名本身不替代本次测量。
            uint32_t neg_local_ssrc = 0, neg_remote_ssrc = 0, neg_rtcp_port = 0;
            uint32_t neg_source_port = 0;
            const bool has_local =
                stream_config_u32(session->started().answer, "LocalSSRC", neg_local_ssrc);
            const bool has_remote =
                stream_config_u32(session->started().answer, "RemoteSSRC", neg_remote_ssrc);
            stream_config_u32(session->started().answer, "RTCPRemotePort", neg_rtcp_port);
            // 已有 21 份记录的 sender.port 与 SourcePort 相等；不能据此假定所有设备相等。
            // 已测 RTCPRemotePort/DestPort 为客户端接收端口，反馈发往设备 SourcePort。
            // 这里仍读取并输出本次结果，历史核对见 docs/coredevice.md §13。
            const bool has_source_port =
                stream_config_u32(session->started().answer, "SourcePort", neg_source_port);
            // 将请求 timeout 与 answer 的 RTCP 超时字段并排打印。
            // 相等仅说明字段关系，不足以确定设备计时器的重置条件。
            uint32_t rtcp_interval = 0, rtcp_enabled = 0;
            const bool has_interval =
                stream_config_u32(session->started().answer, "RTCPTimeoutInterval", rtcp_interval);
            stream_config_u32(session->started().answer, "RTCPTimeoutEnabled", rtcp_enabled);
            std::printf("  answer: LocalSSRC=%s RemoteSSRC=%s RTCPRemotePort=%u "
                        "connection.sender.port=%u streamConfig.SourcePort=%u\n",
                        has_local ? std::to_string(neg_local_ssrc).c_str() : SCRCTL_TR("(missing)"),
                        has_remote ? std::to_string(neg_remote_ssrc).c_str() : SCRCTL_TR("(missing)"),
                        neg_rtcp_port, session->started().sender_port, neg_source_port);
            print_sync_tokens(SCRCTL_TR("video"), session->started().answer);
            std::printf(SCRCTL_TR("  Requested timeout=%s -> answer RTCPTimeoutInterval=%s RTCPTimeoutEnabled=%s\n"),
                        lease_text.c_str(),
                        has_interval ? std::to_string(rtcp_interval).c_str() : SCRCTL_TR("(missing)"),
                        rtcp_enabled == 1 ? SCRCTL_TR("true") : (rtcp_enabled == 0 ? SCRCTL_TR("false/not read") : SCRCTL_TR("other")));
            if (!no_timeout_key && has_interval && rtcp_interval != timeout_seconds) {
                std::printf(SCRCTL_TR("  Timeout values differ: the device did not echo the requested value\n"));
            }
            if (has_source_port && neg_source_port != session->started().sender_port) {
                std::printf(SCRCTL_TR("  Ports differ: rrsrc* arms send to streamConfig.SourcePort\n"));
            }
            // 打印 feature 字符串回显；回显存在不代表每项能力均已启用。
            std::string echoed;
            if (stream_config_str(session->started().answer, "TxCodecFeatureListString", echoed)) {
                std::printf(SCRCTL_TR("  Sent %s -> answer TxCodecFeatureListString=%s\n"),
                            avc_features.c_str(), echoed.c_str());
            }

            // 指定 UDP 端口探测只统计成功写入隧道；关闭端口可能产生 ICMPv6 反馈。
            // 无反馈不等于未投递，需结合隧道计数及抓包；早期结果受 UDP 校验和缺陷影响。
            if (udp_canary) {
                for (int i = 0; i < 3; ++i) {
                    std::string cerr;
                    const std::vector<uint8_t> junk = {0x80, 0xcc, 0x00, 0x03, 0, 0, 0, 0,
                                                       0, 0, 0, 0x5a};
                    const bool ok = session->send_rtp(junk, canary_port, cerr);
                    std::printf(SCRCTL_TR("  UDP probe #%d -> %u: %s\n"), i + 1, canary_port,
                                ok ? SCRCTL_TR("written to tunnel") : (SCRCTL_TR("write failed: ") + cerr).c_str());
                    std::this_thread::sleep_for(400ms);
                }
            }

            const uint64_t until = t0 + static_cast<uint64_t>(seconds) * 1000;
            // 音频 RR+SDES 的 1 Hz 发送起点，以及音频收包和发送计数。
            uint64_t next_audio_rr = t0;
            uint64_t audio_rr_sent = 0;
            // 每 10 秒输出实际接收、发送计数，不据此推断设备已接受反馈。
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
                // 每轮最多排空 32 包，避免持续的视频流量阻塞 RTCP 定时发送。
                constexpr int kDrainPerRound = 32;
                for (int drained = 0; drained < kDrainPerRound; ++drained) {
                    if (!session->next_packet(packet, peer, 30, err)) {
                        break;
                    }
                    // 排空循环内也检查观察截止时间，避免持续收包使本轮超过窗口。
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
                        std::vector<uint8_t> au_bytes;
                        std::string derr;
                        const bool pushed =
                            idr_scan.push(packet, au_bytes, derr) && !au_bytes.empty();
                        const bool this_has_idr = pushed && annexb_has_idr(au_bytes);
                        if (this_has_idr) {
                            ++arm.idr_packets;
                        }
                        if (this_has_idr && arm.first_request_ms != 0 &&
                            arm.idr_after_request_ms == 0) {
                            // 只计首次计时请求之后的第一个 IDR，避免把起流 IDR 计入延迟。
                            arm.idr_after_request_ms = now - arm.first_request_ms;
                        }
                        ++video_seen;
                        ++rtp_packets;
                        rtp_last_ts = info.timestamp;
                        ++cur_frame_pkts;
                        // 抓包中的伴随包（name=5）按帧发送，以 RTP marker 标记帧结束。
                        if (info.marker && send_rctl) {
                            last_frame_pkts = cur_frame_pkts;
                            cur_frame_pkts = 0;
                            std::string serr;
                            const auto comp = build_rctl_companion(
                                mine_ssrc && has_remote ? neg_remote_ssrc : our_ssrc,
                                rtp_last_ts);
                            if (!session->send_rtp(comp, dest_port, serr)) {
                                arm.note = SCRCTL_TR("Failed to send RCTL companion packet: ") + serr;
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
                        arm.note = SCRCTL_TR("Failed to send RCTL: ") + serr;
                    } else {
                        ++rtcp_sent;
                    }
                }
                if (send_rr && media_ssrc != 0 && now_ms() >= next_rr) {
                    next_rr += rr_period_ms;
                    std::string serr;
                    // mine 使用 RemoteSSRC，neg 使用 LocalSSRC，same 使用媒体 RTP SSRC；
                    // 其它使用自选 SSRC。保留各臂的现有回退，以便逐项比较。
                    const uint32_t sender_ssrc =
                        mine_ssrc && has_remote ? neg_remote_ssrc
                        : neg_ssrc && has_local ? neg_local_ssrc
                        : same_ssrc             ? media_ssrc
                                                : our_ssrc;
                    // 报告块：mine 优先 LocalSSRC，neg 优先 RemoteSSRC，其余或缺失时使用媒体 SSRC。
                    const uint32_t report_ssrc =
                        mine_ssrc && has_local ? neg_local_ssrc
                        : neg_ssrc && has_remote ? neg_remote_ssrc
                                                 : media_ssrc;
                    std::vector<uint8_t> rr;
                    if (send_sr) {
                        rr = build_sr(sender_ssrc, 0, 0);  // 本探针不发送媒体 RTP，发送计数为 0。
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
                    // 首次发包时打印 RTP SSRC 与协商字段的对照，不把不等于 Local 自动视为等于 Remote。
                    if (!ssrc_role_printed) {
                        ssrc_role_printed = true;
                        std::printf(SCRCTL_TR("  RTP SSRC %u compared with answer %s (%s)\n"), media_ssrc,
                                    media_ssrc == neg_local_ssrc ? "LocalSSRC" : "RemoteSSRC",
                                    media_ssrc == neg_local_ssrc ? SCRCTL_TR("matches LocalSSRC")
                                                                 : SCRCTL_TR("LocalSSRC differs; compare RemoteSSRC with the value above"));
                    }
                    if (!session->send_rtp(rr, dest_port, serr)) {
                        arm.note = SCRCTL_TR("Failed to send RTCP: ") + serr;
                    } else {
                        ++rtcp_sent;
                    }
                }
                // 根据静默条件/request_always 及 --hz 周期发送 PLI 或标准 FIR。
                // IDR 的出现是时间相关性，应结合同期对照；当前三轮记录见 docs/coredevice.md §30.3。
                if ((send_pli || send_fir) && media_ssrc != 0) {
                    const uint64_t n = now_ms();
                    const uint64_t quiet = last_video != 0 ? n - last_video : n - t0;
                    if ((quiet >= kPliQuietMs || request_always) && n >= next_req) {
                        next_req = n + rr_period_ms;
                        std::vector<uint8_t> req_packet;
                        if (send_fir) {
                            req_packet = build_fir(neg_remote_ssrc, fir_seq++,
                                                   neg_local_ssrc != 0 ? neg_local_ssrc
                                                                       : media_ssrc);
                        } else {
                            req_packet = build_pli(neg_remote_ssrc,
                                                   neg_local_ssrc != 0 ? neg_local_ssrc
                                                                       : media_ssrc);
                        }
                        std::string serr;
                        if (!session->send_rtp(req_packet, dest_port, serr)) {
                            arm.note = SCRCTL_TR("Failed to send keyframe request: ") + serr;
                        } else {
                            ++arm.requests_sent;
                            // 起流后 3 秒内的请求不计入首次请求到 IDR 的延迟，
                            // 减少起流 IDR 对计时的影响；IDR 计数仍覆盖本轮接收窗口。
                            // 请求后出现 IDR 是时间相关性，需结合对照臂判断请求效果。
                            if (arm.first_request_ms == 0 && n - t0 >= 3000) {
                                arm.first_request_ms = n;
                                std::printf(SCRCTL_TR("  First %s sent (video quiet for %llums; later requests follow --hz)\n"),
                                            send_fir ? "FIR" : "PLI",
                                            static_cast<unsigned long long>(quiet));
                            }
                        }
                    }
                }
                // 先接收并计数音频 RTP，再按 1Hz 发送音频 RR+SDES。
                // 收包证明本轮存在音频流量，不能单独证明编码或播放正常。
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
                            if (audio_dump != nullptr) {
                                // 保留 RTP 头及载荷，供后续核对 PT、marker、时间戳
                                // 和分包关系。记录长度包含头部。
                                const uint16_t n = static_cast<uint16_t>(ap.size());
                                const uint8_t len[2] = {static_cast<uint8_t>(n >> 8),
                                                        static_cast<uint8_t>(n & 0xFF)};
                                std::fwrite(len, 1, 2, audio_dump.get());
                                std::fwrite(ap.data(), 1, ap.size(), audio_dump.get());
                            }
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
                            arm.note = SCRCTL_TR("Failed to send audio RR: ") + serr;
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
                            SCRCTL_TR("  +%3llus video packets %6llu SRs %4llu RTCP sent %5llu audio packets %6llu audio RR %3llu (requested timeout %s)\n"),
                            static_cast<unsigned long long>((now_ms() - t0) / 1000),
                            static_cast<unsigned long long>(video_seen),
                            static_cast<unsigned long long>(sr_seen),
                            static_cast<unsigned long long>(rtcp_sent),
                            static_cast<unsigned long long>(audio_seen),
                            static_cast<unsigned long long>(audio_rr_sent), lease_text.c_str());
                    } else {
                        std::printf(SCRCTL_TR("  +%3llus video packets %6llu SRs %4llu RTCP sent %5llu (requested timeout %s)\n"),
                                    static_cast<unsigned long long>((now_ms() - t0) / 1000),
                                    static_cast<unsigned long long>(video_seen),
                                    static_cast<unsigned long long>(sr_seen),
                                    static_cast<unsigned long long>(rtcp_sent), lease_text.c_str());
                    }
                    if (dump_status) {
                        std::string qerr;
                        const auto st = scrctl::media::StreamSession::status(*dev, qerr, verbose);
                        // 将本探针 UUID 与设备条目并排打印，检查匹配失败或其他客户端条目。
                        // probe 的 Unknown 与 Missing 不能混为设备已经结束会话。
                        std::printf(SCRCTL_TR("    Our session_uuid = "));
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
                                std::printf(SCRCTL_TR("    Session table entry %zu uuid = "), i);
                                if (u == nullptr) {
                                    std::printf(SCRCTL_TR("(unavailable)"));
                                } else {
                                    for (uint8_t b : u->data) {
                                        std::printf("%02x", b);
                                    }
                                }
                                std::printf("\n");
                            }
                        }
                        walk(st, SCRCTL_TR("Status"), 0);
                    }
                }
            }
            // 在观察窗结束后查询会话归属，辅助核对最终状态。
            // 此处不是断流瞬间的快照，不能独自确定更早中断的原因。
            dump_sessions(*dev, session->started().session_uuid, SCRCTL_TR("observation ended"));
            if (dump_status) {
                std::printf(SCRCTL_TR("  Device status at the end of the observation window:\n"));
                std::string qerr;
                walk(scrctl::media::StreamSession::status(*dev, qerr, verbose), SCRCTL_TR("Status"), 0);
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
            if (sub_thread.joinable()) {
                sub_done.store(true);
                sub_thread.join();
                std::printf(SCRCTL_TR("  [sub] Display updates received=%d%s\n"), sub_elements.load(),
                            sub_failed.load() ? SCRCTL_TR(" (subscription failed during observation)") : "");
            }
            if (hold_thread.joinable()) {
                hold_done.store(true);
                hold_thread.join();
                std::printf(SCRCTL_TR("  [hold] Successful session polls on the same connection=%d; connection %s\n"),
                            hold_polls.load(),
                            hold_died_ms.load() < 0 ? SCRCTL_TR("remained open") : SCRCTL_TR("closed during observation"));
            }
            std::string serr;
            session->stop(*dev, serr, verbose);
            std::this_thread::sleep_for(2s);
        }
    }

    std::printf(SCRCTL_TR("\nSummary (rounds alive at observation end / valid rounds):\n"));
    for (const auto &[w, t] : tally) {
        std::printf(SCRCTL_TR("  %-6s alive %d / %d\n"), w.c_str(), t.first - t.second, t.first);
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
        std::printf(SCRCTL_TR("Control condition met: the none arm ended in every observed valid round.\n"));
        for (const auto &k : {std::string("rr"), std::string("rrsame"), std::string("rrsdes"),
                       std::string("rrp1"), std::string("rrall"), std::string("poll"),
                       std::string("poll5")}) {
            if (total_of(k) == 0) {
                continue;
            }
            std::printf(SCRCTL_TR("  %-6s: %s\n"), k.c_str(),
                        alive_of(k) == total_of(k)
                            ? SCRCTL_TR("alive in every observed round; consistent with keepalive in this comparison")
                            : (alive_of(k) == 0 ? SCRCTL_TR("ended in every observed round; no keepalive effect observed in this comparison")
                                                : SCRCTL_TR("mixed outcomes; more controlled observations are needed")));
        }
    } else {
        std::printf(SCRCTL_TR("Control condition not met: the none arm survived or no valid control round was recorded.\n"));
    }
    return 0;
}
