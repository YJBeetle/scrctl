// 探针：这条流认不认 RTCP PLI？
//
// 为什么先问这个：流里没有周期性 IDR，所以一旦丢包把参考链打断，画面就永久坏掉。
// 恢复只有两条路——(a) 发 PLI 请设备立刻给一个关键帧；(b) 拆掉重起媒体会话，
// 新会话必然从 IDR 开始。(a) 便宜得多（一个 12 字节的 UDP 包），但前提是设备理我们。
// 所以先测，测出来再决定库里写什么。
//
// 判据要成立，观察窗里必须**有画面在动**：编码器在静止画面上根本不出帧，
// "PLI 之后没收到 IDR"就分不清是"设备不理"还是"没东西可发"。第一版就栽在这儿
// ——对着静止的无边记画布测，4 秒后一个包都收不到，差点得出错误结论。所以
// 观察窗里由本探针自己拖动画布制造持续变化（只碰画布，不碰别的）。
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "hid/Hid.h"
#include "media/StreamSession.h"
#include "remote/Device.h"
#include "rt/RtpHevc.h"

namespace {

uint64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// RFC 4585 的 PLI：公共头（V=2,P=0,RC=1 / PT=206 / length=2）+ FCI 一个媒体 SSRC。
std::vector<uint8_t> build_pli(uint32_t sender_ssrc, uint32_t media_ssrc) {
    auto put32 = [](std::vector<uint8_t> &v, uint32_t x) {
        v.push_back(static_cast<uint8_t>(x >> 24));
        v.push_back(static_cast<uint8_t>(x >> 16));
        v.push_back(static_cast<uint8_t>(x >> 8));
        v.push_back(static_cast<uint8_t>(x));
    };
    std::vector<uint8_t> p;
    p.push_back(0x81);
    p.push_back(206);
    // length 按 32 位字计且不含公共头：sender SSRC + media SSRC = 2。
    put32(p, 2u);
    put32(p, sender_ssrc);
    put32(p, media_ssrc);
    return p;
}

/// 从 Annex-B 串里数出 IDR 帧（NAL type 19/20/21）。
std::set<int> idr_types_in(const std::vector<uint8_t> &annexb) {
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

}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int baseline_s = 3;
    int watch_s = 6;
    bool random_sender = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--random-sender") {
            random_sender = true;
        } else if (a == "-t" && i + 1 < argc) {
            baseline_s = std::stoi(argv[++i]);
        } else if (a == "-w" && i + 1 < argc) {
            watch_s = std::stoi(argv[++i]);
        }
    }

    std::string err;
    auto device = scrctl::remote::Device::establish({}, err);
    if (!device) {
        std::fprintf(stderr, "建立会话失败: %s\n", err.c_str());
        return 1;
    }
    scrctl::media::StreamSession::Request req;
    auto session = scrctl::media::StreamSession::start(*device, req, err);
    if (session == nullptr) {
        std::fprintf(stderr, "起流失败: %s\n", err.c_str());
        return 1;
    }
    std::printf("流已建立：收流端口=%u PT=%u 设备发送端口=%u\n", session->receiver_port(),
                session->started().payload_type, session->started().sender_port);

    // 观察窗里要一直有画面变化，所以起一条 HID 连接来回拖画布。
    auto hid = scrctl::hid::Service::open(*device, err);
    if (hid == nullptr) {
        std::fprintf(stderr, "打开 HID 服务失败（没有画面变化，判据不成立）: %s\n", err.c_str());
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    scrctl::rt::HevcRtpDepacketizer dep{session->started().payload_type};
    uint32_t ssrc = 0;
    uint16_t rtcp_peer_port = 0;
    std::set<int> baseline_idr;
    std::set<int> after_idr;
    long baseline_pkts = 0, after_pkts = 0;
    bool sent = false;
    const uint64_t t0 = now_ms();
    std::vector<uint8_t> packet;
    std::vector<std::pair<double, double>> wiggle;
    for (int i = 0; i <= 16; ++i) {
        const double t = i / 16.0;
        // 画布中部的一小段来回，幅度不大，只在无边记里挪动视图。
        wiggle.emplace_back(0.35 + 0.25 * (t < 0.5 ? t * 2 : (1 - t) * 2), 0.45);
    }

    while (now_ms() - t0 < static_cast<uint64_t>((baseline_s + watch_s) * 1000)) {
        uint16_t peer_port = 0;
        const bool got = session->next_packet(packet, peer_port, 100, err);
        const bool in_baseline = now_ms() - t0 < static_cast<uint64_t>(baseline_s * 1000);
        if (got) {
            scrctl::rt::PacketInfo info{};
            if (scrctl::rt::parse_rtp_header(packet, info) &&
                info.payload_type == session->started().payload_type) {
                ssrc = info.ssrc;
            } else {
                // 非视频载荷：RTCP 从设备的哪个端口来，PLI 就回哪儿去。
                rtcp_peer_port = peer_port;
            }
            std::vector<uint8_t> annexb;
            dep.push(packet, annexb, err);
            for (int t : idr_types_in(annexb)) {
                (in_baseline ? baseline_idr : after_idr).insert(t);
            }
            ++(in_baseline ? baseline_pkts : after_pkts);
        }

        if (!sent && !in_baseline) {
            sent = true;
            // RTCP 的源端口没观测到就按惯例猜 sender_port+1（RFC 3550：RTP 偶、RTCP 奇）。
            const uint16_t dst = rtcp_peer_port != 0
                                     ? rtcp_peer_port
                                     : static_cast<uint16_t>(session->started().sender_port + 1);
            const uint32_t sender = random_sender ? 0x12345678u : ssrc;
            std::printf("\n>> 发 PLI：目的端口=%u（%s）media SSRC=%08x sender SSRC=%08x\n", dst,
                        rtcp_peer_port != 0 ? "RTCP 实测源端口" : "按 sender+1 猜", ssrc, sender);
            if (!session->send_rtp(build_pli(sender, ssrc), dst, err)) {
                std::fprintf(stderr, "发 PLI 失败: %s\n", err.c_str());
                return 1;
            }
        }
        if (!in_baseline && got) {
            // 每收到一个包就推进一格 wiggle，等于用画面变化给编码器持续喂内容。
            static size_t step = 0;
            step = (step + 1) % wiggle.size();
            std::string derr;
            hid->touch(scrctl::hid::kSurfaceMainTouchscreen, wiggle[step].first,
                       wiggle[step].second, true, derr);
        }
    }
    std::string derr;
    hid->touch(scrctl::hid::kSurfaceMainTouchscreen, 0.5, 0.45, false, derr);

    std::printf("\n基线 %d 秒：包 %ld，IRAP 类型:", baseline_s, baseline_pkts);
    for (int t : baseline_idr) std::printf(" %d", t);
    std::printf("\nPLI 后 %d 秒：包 %ld，IRAP 类型:", watch_s, after_pkts);
    for (int t : after_idr) std::printf(" %d", t);
    std::printf("\n");
    if (after_pkts == 0) {
        std::printf("判据不成立：观察窗里一个包都没有，画面没在动。\n");
        return 2;
    }
    if (!after_idr.empty()) {
        std::printf("结论：PLI 之后的观察窗里出现了 IRAP —— 设备认 PLI，恢复可以走这条路。\n");
    } else {
        std::printf("结论：画面在动、包也在来，但 PLI 之后没有 IRAP —— 这条路不通，"
                    "恢复要靠重起媒体会话。\n");
    }
    return 0;
}
