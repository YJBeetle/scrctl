#include "media/StreamSession.h"

#include <array>
#include <random>

#include "remote/Device.h"

namespace scrctl::media {
namespace {

using namespace scrctl;

/// 主机侧能力位掩码，从可用会话里带出来的观测值。
constexpr uint64_t kClientSupportedFeatures = 140;
constexpr int64_t kAccessNetworkType = 1;
constexpr int64_t kTransportProtocolType = 2;

/// options 里每个值都被包一层带类型名的字典（{"int": …} / {"string": …} /
/// {"uuid": …}）。设备按这个标签决定怎么读，漏一层就是"参数缺失"。
xpc::Value typed(const char *tag, xpc::Value inner) {
    auto d = xpc::make_dict();
    xpc::dict_set(d, tag, std::move(inner));
    return d;
}

/// 一次起流一个的会话号。设备侧是 Swift Codable，声明成 UUID 的字段就必须给
/// 真正的 XPC UUID 对象（16 字节），给 UUID 文本会被直接拒。
std::array<uint8_t, 16> random_uuid_bytes() {
    std::array<uint8_t, 16> out{};
    for (auto &byte : out) {
        byte = static_cast<uint8_t>(std::random_device{}());
    }
    return out;
}

uint16_t pick_port() {
    static std::mt19937 rng{std::random_device{}()};
    return static_cast<uint16_t>(49152 + rng() % 16000);
}

}  // namespace

xpc::Value build_start_request(const std::string &receiver_ip, uint16_t receiver_port,
                               const std::string &sender_ip,
                               const std::vector<uint8_t> &offer_bplist, uint32_t display_id,
                               uint32_t timeout_seconds) {
    auto d = xpc::make_dict();
    xpc::dict_set(d, "clientSupportedFeatures", xpc::make_uint64(kClientSupportedFeatures));
    xpc::dict_set(d, "direction", xpc::make_string("output"));
    xpc::dict_set(d, "negotiatorOffer", xpc::make_data(offer_bplist));

    auto options = xpc::make_dict();
    xpc::dict_set(options, "AVCMediaStreamNegotiatorAccessNetworkType",
                  typed("int", xpc::make_int64(kAccessNetworkType)));
    xpc::dict_set(options, "AVCMediaStreamNegotiatorTransportProtocolType",
                  typed("int", xpc::make_int64(kTransportProtocolType)));
    xpc::dict_set(options, "CoreDeviceVideoDisplayMode",
                  typed("string", xpc::make_string("DisplayByID")));
    xpc::dict_set(options, "VideoStreamForDisplayID",
                  typed("int", xpc::make_int64(display_id)));
    // 会话号：一次起流一个，设备用它关联这条会话。
    // 必须是真正的 XPC UUID 对象（16 字节），不能是 UUID 文本——设备侧是
    // Swift Codable，类型对不上直接拒："Expected to decode UUID but found a
    // OS_xpc_string instead"（code 4864）。
    const auto session_id = random_uuid_bytes();
    xpc::dict_set(options, "avcMediaStreamOptionClientSessionID",
                  typed("uuid", xpc::make_uuid(std::span<const uint8_t>(session_id))));
    xpc::dict_set(d, "options", std::move(options));

    xpc::dict_set(d, "receiverIP", xpc::make_string(receiver_ip));
    xpc::dict_set(d, "receiverPort", xpc::make_uint64(receiver_port));
    xpc::dict_set(d, "senderIP", xpc::make_string(sender_ip));
    xpc::dict_set(d, "timeout", xpc::make_uint64(timeout_seconds));
    xpc::dict_set(d, "type", xpc::make_string("video"));
    return d;
}

std::unique_ptr<StreamSession> StreamSession::start(remote::Device &device,
                                                    const Request &request, std::string &err,
                                                    bool verbose) {
    const auto info = device.rsd().service("com.apple.coredevice.displayservice");
    if (!info) {
        err = "设备目录里没有 displayservice（DDI 是否已挂载？）";
        return nullptr;
    }

    // 先绑端口再起流：设备一返回 answer 就开始推 RTP，晚绑会丢掉带 VPS/SPS/PPS
    // 和首个关键帧的开头几个包。
    const uint16_t port = request.receiver_port != 0 ? request.receiver_port : pick_port();
    auto socket = std::make_unique<net::UdpSocket>(device.rsd().stack(), port);
    if (!socket->bind(err)) {
        err = "绑 UDP 端口 " + std::to_string(port) + " 失败: " + err;
        return nullptr;
    }

    Offer offer = request.offer;
    offer.session_id = static_cast<uint32_t>(std::random_device{}());
    offer.call_id = remote::random_uuid_text();
    const auto blob = build_negotiator_offer(offer);

    const auto &tunnel_params = device.tunnel_params();
    auto input = build_start_request(tunnel_params.client_address, port,
                                     tunnel_params.server_address, blob, request.display_id,
                                     request.timeout_seconds);
    xpc::Value output;
    if (!device.feature("com.apple.coredevice.displayservice",
                        "com.apple.coredevice.feature.startmediastream",
                        "com.apple.coredevice.action.mediastreamstart", input, output, err,
                        verbose, 30000)) {
        return nullptr;
    }

    Started started;
    started.answer = std::move(output);
    // answer 里设备侧的发送端口在 connection.sender.port，形态是字符串还是整数
    // 没固定说法，两种都试。
    const auto *connection = started.answer.find("connection");
    if (connection != nullptr) {
        const auto *sender = connection->find("sender");
        if (sender != nullptr) {
            // 端口在这套协议里有时是整数、有时是字符串（RSD 目录里就是字符串），
            // 两种都接下来：按整数读会拿到 0，然后永远收不到包。
            if (const auto *p = sender->find("port"); p != nullptr) {
                if (p->is_string()) {
                    started.sender_port = static_cast<uint16_t>(std::stoi(p->string));
                } else {
                    started.sender_port = static_cast<uint16_t>(p->as_int_or(0));
                }
            }
        }
    }
    return std::unique_ptr<StreamSession>(new StreamSession(std::move(socket), std::move(started)));
}

StreamSession::~StreamSession() = default;

bool StreamSession::next_packet(std::vector<uint8_t> &packet, int timeout_ms, std::string &err) {
    uint16_t peer_port = 0;
    return socket_->recv(packet, peer_port, timeout_ms, err);
}

uint16_t StreamSession::receiver_port() const { return socket_->local_port(); }

}  // namespace scrctl::media
