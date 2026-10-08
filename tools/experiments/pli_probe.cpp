// 发送一次标准 RTCP PLI，比较请求前后接收到的视频 RTP 和完整 IRAP NAL。
// 报文复用产品构造器，目的端口和 SSRC 使用本次起流协商结果。
//
// 本探针会在基线和请求后的窗口内持续发送触摸移动。运行前应打开允许测试的
// 空白画布，并选择合适的绘图或移动工具；相同触摸在其它界面可能触发其它操作，
// 不能保证只移动视图。触摸写入成功也不证明画面发生变化，需要结合视频接收观察。
//
// 请求后的 IRAP 只是时间相关性，单轮结果不能证明它由 PLI 触发；未观察到 IRAP
// 也不能证明设备不支持 PLI。重复对照和 RR 保活实验使用 rr_keepalive_probe。
// 本次实验显式请求 20 秒 RTCP 空闲超时，不发送周期 RR，也不调用会影响并存音视频
// 的 stopAll。超过该超时的观察可能遇到设备停流，不能把停流归因于 PLI。
#include <CLI/CLI.hpp>
#include <chrono>
#include <cstdio>
#include <limits>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "ProbeCli.h"
#include "hid/Hid.h"
#include "media/StreamSession.h"
#include "remote/Device.h"
#include "rt/Rtcp.h"
#include "rt/RtpHevc.h"

namespace {

uint64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// 记录完整 Annex-B NAL 中已定义的 IRAP 类型：BLA 16..18、IDR 19..20、CRA 21。
/// 此处只记录类型，不解码画面，也不统计完整图像数量。
std::set<int> irap_types_in(const std::vector<uint8_t> &annexb) {
    std::set<int> found;
    for (std::size_t i = 0; i + 5 < annexb.size(); ++i) {
        if (annexb[i] == 0 && annexb[i + 1] == 0 && annexb[i + 2] == 0 && annexb[i + 3] == 1) {
            const int type = (annexb[i + 4] >> 1) & 0x3F;
            if (type >= 16 && type <= 21) {
                found.insert(type);
            }
        }
    }
    return found;
}

/// 正常结束时显式检查抬起报告发送结果；析构在提前退出时补发抬起。
struct TouchRelease {
    scrctl::hid::Service &hid;
    double x = 0.5;
    double y = 0.45;
    bool contact = false;

    bool close(std::string &err) {
        if (!contact) {
            err.clear();
            return true;
        }
        if (!hid.touch(scrctl::hid::kSurfaceMainTouchscreen, x, y, false, err)) return false;
        contact = false;
        return true;
    }

    ~TouchRelease() {
        if (!contact) return;
        std::string err;
        if (!close(err)) {
            std::fprintf(stderr, "释放触摸失败: %s\n", err.c_str());
        }
    }
};

}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int baseline_s = 3;
    int watch_s = 6;
    bool random_sender = false;
    bool dry_run = false;
    CLI::App app("发送单次 PLI 并观察请求前后的视频 RTP 和 IRAP");
    app.footer("本次请求使用 20 秒 RTCP 空闲超时，不发送周期 RR。\n"
               "--help 和 --dry-run 不连接设备，也不发送触摸。\n"
               "请求后的 IRAP 只表示时间相关性；重复对照请使用 rr_keepalive_probe。");
    // 共用转换器保留旧参数的十进制含义，并完整校验输入和范围。
    app.add_option("-t", baseline_s, "请求前的基线秒数，0 表示立即请求（默认 3）")
        ->transform(scrctl::probe::decimal_integer(0, std::numeric_limits<int>::max()))
        ->multi_option_policy(CLI::MultiOptionPolicy::TakeLast);
    app.add_option("-w", watch_s, "请求后的观察秒数，至少 1 秒（默认 6）")
        ->transform(scrctl::probe::decimal_integer(1, std::numeric_limits<int>::max()))
        ->multi_option_policy(CLI::MultiOptionPolicy::TakeLast);
    app.add_flag("--random-sender", random_sender, "用固定自选 SSRC 替代协商发送者，作为对照");
    app.add_flag("--dry-run", dry_run, "只显示实验参数和时间预算，不连接设备");
    try {
        app.parse(argc, argv);
    } catch (const CLI::CallForHelp &error) {
        return app.exit(error);
    } catch (const CLI::ParseError &error) {
        std::fprintf(stderr, "参数错误：%s\n", error.what());
        return 2;
    }
    // 两个参数已限制到 int 的非负范围，先提升再乘加；最大总时长为
    // 2 * INT_MAX * 1000 毫秒，能够用 uint64_t 表示，不在有符号 int 中计算。
    const uint64_t baseline_ms = static_cast<uint64_t>(baseline_s) * 1000;
    const uint64_t watch_ms = static_cast<uint64_t>(watch_s) * 1000;
    const uint64_t total_ms = baseline_ms + watch_ms;
    std::printf("实验参数：timeout=20 秒，RR=off，baseline_ms=%llu，watch_ms=%llu，"
                "total_ms=%llu，sender=%s\n",
                static_cast<unsigned long long>(baseline_ms),
                static_cast<unsigned long long>(watch_ms),
                static_cast<unsigned long long>(total_ms),
                random_sender ? "fixed-experimental" : "negotiated");
    if (dry_run) return 0;

    std::string err;
    auto device = scrctl::remote::Device::establish({}, err);
    if (!device) {
        std::fprintf(stderr, "建立会话失败: %s\n", err.c_str());
        return 1;
    }
    scrctl::media::StreamSession::Request req;
    req.timeout_seconds = 20;  // 本次单次请求实验的条件，不沿用低层 3600 秒默认值。
    auto session = scrctl::media::StreamSession::start(*device, req, err);
    if (session == nullptr) {
        std::fprintf(stderr, "起流失败: %s\n", err.c_str());
        return 1;
    }
    const auto &started = session->started();
    const auto &config = started.answer.at("connection").at("streamConfig");
    if (started.sender_port == 0 || config.find("LocalSSRC") == nullptr ||
        config.find("RemoteSSRC") == nullptr) {
        std::fprintf(stderr, "协商回复缺少反馈端口或 SSRC，无法发送本次会话的 PLI。\n");
        return 2;
    }
    std::printf("流已建立：收流端口=%u PT=%u 反馈目的端口=%u LocalSSRC=%08x RemoteSSRC=%08x\n",
                session->receiver_port(), started.payload_type, started.sender_port,
                started.local_ssrc, started.remote_ssrc);
    std::printf("本次只发送一条 PLI，不发送周期 RR；起流请求 timeout=%u 秒。\n",
                req.timeout_seconds.value_or(0));
    std::printf("观察时间超过 RTCP 空闲超时时可能停流；本轮不据此判断 PLI 行为。\n");

    // 基线和请求后的窗口使用相同触摸移动，避免只在请求后引入额外画面变化。
    auto hid = scrctl::hid::Service::open(*device, err);
    if (hid == nullptr) {
        std::fprintf(stderr, "打开 HID 服务失败，无法执行触摸移动: %s\n", err.c_str());
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    TouchRelease release{*hid};

    scrctl::rt::HevcRtpDepacketizer dep{session->started().payload_type};
    std::set<int> baseline_irap;
    std::set<int> after_irap;
    long baseline_pkts = 0, after_pkts = 0;
    long baseline_sr = 0, after_sr = 0;
    long depacketize_errors = 0;
    bool ssrc_observed = false;
    bool sent = false;
    const uint64_t t0 = now_ms();
    uint64_t next_touch = t0;
    std::size_t step = 0;
    std::vector<uint8_t> packet;
    std::vector<std::pair<double, double>> wiggle;
    for (int i = 0; i <= 16; ++i) {
        const double t = i / 16.0;
        // 画布中部的一小段来回，具体是绘图还是移动由当前工具决定。
        wiggle.emplace_back(0.35 + 0.25 * (t < 0.5 ? t * 2 : (1 - t) * 2), 0.45);
    }

    while (now_ms() - t0 < total_ms) {
        if (!sent && now_ms() - t0 >= baseline_ms) {
            // 已测设备将 RTP 和 RTCP 复用到 sender.port；不猜测 sender.port+1，
            // RTCPRemotePort 是客户端接收端口，也不是反馈目的地。
            const uint32_t sender = random_sender ? 0x12345678u : started.remote_ssrc;
            std::printf("\n发送 PLI：目的端口=%u media SSRC=%08x sender SSRC=%08x（%s）\n",
                        started.sender_port, started.local_ssrc, sender,
                        random_sender ? "固定自选发送者，对照模式" : "本次协商的发送者");
            if (!session->send_rtp(scrctl::rt::build_pli(sender, started.local_ssrc),
                                   started.sender_port, err)) {
                std::fprintf(stderr, "发 PLI 失败: %s\n", err.c_str());
                return 1;
            }
            sent = true;
            std::printf("PLI 已写入隧道；这不证明设备已收到或处理。\n");
        }
        if (now_ms() >= next_touch) {
            next_touch = now_ms() + 100;
            release.x = wiggle[step].first;
            release.y = wiggle[step].second;
            release.contact = true;
            if (!hid->touch(scrctl::hid::kSurfaceMainTouchscreen, release.x, release.y, true,
                            err)) {
                std::fprintf(stderr, "触摸移动发送失败: %s\n", err.c_str());
                return 1;
            }
            step = (step + 1) % wiggle.size();
        }
        uint16_t peer_port = 0;
        const bool got = session->next_packet(packet, peer_port, 100, err);
        if (got) {
            if (scrctl::rt::is_rtcp_sr(packet)) {
                ++(sent ? after_sr : baseline_sr);
                continue;
            }
            scrctl::rt::PacketInfo info{};
            if (scrctl::rt::parse_rtp_header(packet, info) &&
                info.payload_type == session->started().payload_type) {
                if (info.ssrc != started.local_ssrc) {
                    std::fprintf(stderr, "视频 RTP SSRC=%08x 与协商 LocalSSRC=%08x 不一致，"
                                        "停止本轮观察。\n", info.ssrc, started.local_ssrc);
                    return 2;
                }
                if (!ssrc_observed) {
                    ssrc_observed = true;
                    std::printf("已观察到视频 RTP：源端口=%u SSRC=%08x（匹配 LocalSSRC）。\n",
                                peer_port, info.ssrc);
                }
                ++(sent ? after_pkts : baseline_pkts);
                std::vector<uint8_t> annexb;
                if (!dep.push(packet, annexb, err)) {
                    ++depacketize_errors;
                    continue;
                }
                for (int t : irap_types_in(annexb)) {
                    (sent ? after_irap : baseline_irap).insert(t);
                }
            }
        }
    }

    if (!release.close(err)) {
        std::fprintf(stderr, "释放触摸失败，无法正常结束本轮观察: %s\n", err.c_str());
        return 1;
    }
    std::printf("\n基线 %d 秒：视频 RTP %ld，SR %ld，IRAP 类型:", baseline_s, baseline_pkts,
                baseline_sr);
    for (int t : baseline_irap) std::printf(" %d", t);
    std::printf("\nPLI 后 %d 秒：视频 RTP %ld，SR %ld，IRAP 类型:", watch_s, after_pkts,
                after_sr);
    for (int t : after_irap) std::printf(" %d", t);
    std::printf("\n视频拆包失败次数：%ld\n", depacketize_errors);
    if (!sent) {
        std::printf("本轮未发送 PLI，没有请求后观察结果。\n");
        return 2;
    }
    if (after_pkts == 0) {
        std::printf("请求后未收到视频 RTP，无法判断关键帧响应；SR 本身不代表有新画面。\n");
        return 2;
    }
    if (!after_irap.empty()) {
        std::printf("请求后的接收窗口内观察到 IRAP；本轮没有同期对照，不能确定由 PLI 触发。\n");
    } else {
        std::printf("请求后收到视频 RTP，但未观察到完整 IRAP NAL；不能据此判定设备不支持 PLI。\n");
    }
    return 0;
}
