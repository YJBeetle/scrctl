// 探针：换一个申报给设备的能力位掩码/能力串，编码器的帧率和码率会不会跟着变。
//
// 起因是量到的一个事实：跑游戏时手机自己 60fps 很顺，镜像里却只有 ~12fps，而两种
// 情况下设备发过来的码率都死死顶在 ~5.5Mbps（answer 里 `TXMaxBitrate: 6000000`）。
// 也就是说复杂画面塞不进这个码率上限，编码器选择"保码率、降帧率"——每帧从 10 个
// 包涨到 50 个包，帧数掉 5 倍。
//
// 而这个上限此前被认为改不动：MediaOffer 里记着"把码率阶梯 f2/f3 各缩到 0.25、
// 把分辨率条目 pair_index 从 0 扫到 6，IDR 尺寸和编码尺寸都不变"。但那两轮扫的是
// **offer 里的数**，没扫**能力申报**：
//   - 我们发 `clientSupportedFeatures: 140`（一个观测来的常量），而设备在
//     getmediasupportinfo 里报 `supportedFeatures: 972` —— 差着一串位没申报；
//   - offer 里的能力串实测真的能改变编码器行为：带 `VRAE:0` 时丢输入帧掉到 42fps，
//     去掉后 53–55fps。所以这个字符串是**已证明有效**的旋钮，只是还没扫过别的 token。
//
// 每一档量三个数：帧率（不同 RTP 时间戳的个数/秒）、包/秒、字节/秒，外加 answer 里
// 设备自己交代的 TXMaxBitrate / RateAdaptationEnabled / 编码尺寸。
//
// 用法：bitrate_probe [--client-features 140,972,1023] [--avc-features "FLS;SW:1;"]
//                     [--seconds N] [--verbose]
#include <chrono>
#include <cstdio>
#include <set>
#include <sstream>
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

std::vector<uint64_t> parse_list(const std::string &s) {
    std::vector<uint64_t> out;
    std::stringstream ssv(s);
    std::string item;
    while (std::getline(ssv, item, ',')) {
        if (!item.empty()) {
            out.push_back(std::stoull(item));
        }
    }
    return out;
}

/// 从 answer 里把关心的几个协商值抠出来。路径写死，取不到就报 -1，
/// 免得"没取到"被读成"0"。
struct Negotiated {
    long long tx_max_bitrate = -1;
    long long rate_adaptation = -1;
    long long custom_height = -1;
    long long rtcp_timeout = -1;
};

Negotiated read_answer(const scrctl::xpc::Value &answer) {
    Negotiated n;
    const auto *cfg = answer.find("connection");
    if (cfg == nullptr) {
        return n;
    }
    const auto *sc = cfg->find("streamConfig");
    if (sc == nullptr) {
        return n;
    }
    auto num = [&](const char *key, long long &dst) {
        const auto *wrapped = sc->find(key);
        if (wrapped == nullptr) {
            return;
        }
        // streamConfig 里的值是裸标量还是 {"int":N} 包装，两种都见过，都试。
        const auto *inner = wrapped->find("int");
        const scrctl::xpc::Value &v = inner != nullptr ? *inner : *wrapped;
        if (v.type == scrctl::xpc::Type::Int64) {
            dst = v.int64;
        } else if (v.type == scrctl::xpc::Type::UInt64) {
            dst = static_cast<long long>(v.uint64);
        } else if (v.type == scrctl::xpc::Type::Bool) {
            dst = v.boolean ? 1 : 0;
        } else if (v.type == scrctl::xpc::Type::Double) {
            dst = static_cast<long long>(v.real);
        }
    };
    num("TXMaxBitrate", n.tx_max_bitrate);
    num("RateAdaptationEnabled", n.rate_adaptation);
    num("CustomHeight", n.custom_height);
    num("RTCPTimeoutInterval", n.rtcp_timeout);
    return n;
}

struct Measured {
    double fps = 0;
    double packets_per_s = 0;
    double kbps = 0;
    double packets_per_frame = 0;
    uint64_t bytes = 0;
    uint64_t frames = 0;
    uint64_t packets = 0;
};

Measured measure(scrctl::media::StreamSession &session, int seconds) {
    scrctl::rt::HevcRtpDepacketizer dp(session.started().payload_type);
    std::vector<uint8_t> packet, annexb;
    uint16_t peer = 0;
    std::string err;
    uint32_t last_ts = 0;
    const auto t0 = now_ms();
    Measured m;
    while (now_ms() - t0 < static_cast<uint64_t>(seconds * 1000)) {
        if (!session.next_packet(packet, peer, 100, err)) {
            continue;
        }
        scrctl::rt::PacketInfo info{};
        if (!scrctl::rt::parse_rtp_header(packet, info) ||
            info.payload_type != session.started().payload_type) {
            continue;
        }
        ++m.packets;
        m.bytes += packet.size();
        if (info.timestamp != last_ts) {
            last_ts = info.timestamp;
            ++m.frames;
        }
        std::ignore = dp.push(packet, annexb, err);
    }
    const double secs = static_cast<double>(now_ms() - t0) / 1000.0;
    m.fps = m.frames / secs;
    m.packets_per_s = m.packets / secs;
    m.kbps = m.bytes * 8.0 / secs / 1000.0;
    m.packets_per_frame = m.frames ? static_cast<double>(m.packets) / m.frames : 0.0;
    return m;
}

}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string clients = "140";
    std::string variants = "0";
    std::string avc;
    int seconds = 8;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--client-features" && i + 1 < argc) {
            clients = argv[++i];
        } else if (a == "--rate-variants" && i + 1 < argc) {
            variants = argv[++i];
        } else if (a == "--avc-features" && i + 1 < argc) {
            avc = argv[++i];
        } else if (a == "--seconds" && i + 1 < argc) {
            seconds = std::stoi(argv[++i]);
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
    std::printf("设备：%s / iOS %s；每档起一条流测 %d 秒\n\n",
                dev->property("ProductType").c_str(), dev->property("OSVersion").c_str(),
                seconds);
    std::printf("%-10s %-8s %-16s %8s %9s %9s %10s %10s %s\n", "能力掩码", "码率表档",
                "answer 里 TXMaxBitrate", "帧率", "包/秒", "kbps", "包/帧", "RAE", "CustomHeight");

    for (uint64_t mask : parse_list(clients)) {
      for (uint64_t variant : parse_list(variants)) {
        scrctl::media::StreamSession::Request req;
        req.client_supported_features = mask;
        req.offer.rate_variant = static_cast<int>(variant);
        if (!avc.empty()) {
            req.offer.avc_features = avc;
            req.offer.hevc_features = avc;
        }
        auto session = scrctl::media::StreamSession::start(*dev, req, err, verbose);
        if (!session) {
            std::printf("%-10llu 起流失败: %s\n", static_cast<unsigned long long>(mask),
                        err.c_str());
            continue;
        }
        const auto n = read_answer(session->started().answer);
        const auto m = measure(*session, seconds);
        std::printf("%-10llu %-8llu %-16lld %8.1f %9.0f %9.0f %10.1f %10lld %lld\n",
                    static_cast<unsigned long long>(mask),
                    static_cast<unsigned long long>(variant), n.tx_max_bitrate, m.fps,
                    m.packets_per_s, m.kbps, m.packets_per_frame, n.rate_adaptation,
                    n.custom_height);
        std::string serr;
        session->stop(*dev, serr, verbose);
        session.reset();
        std::this_thread::sleep_for(1500ms);
      }
    }
    return 0;
}
