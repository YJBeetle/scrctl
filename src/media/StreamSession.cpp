#include "media/StreamSession.h"

#include <array>
#include <random>

#include "remote/Device.h"

namespace scrctl::media {
namespace {

using namespace scrctl;

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
                               std::optional<uint32_t> timeout_seconds,
                               uint64_t client_supported_features,
                               const std::vector<uint8_t> &event_channel_uuid, bool audio,
                               const std::vector<uint8_t> &shared_client_session_uuid) {
    auto d = xpc::make_dict();
    xpc::dict_set(d, "clientSupportedFeatures", xpc::make_uint64(client_supported_features));
    xpc::dict_set(d, "direction", xpc::make_string("output"));
    xpc::dict_set(d, "negotiatorOffer", xpc::make_data(offer_bplist));

    auto options = xpc::make_dict();
    xpc::dict_set(options, "AVCMediaStreamNegotiatorAccessNetworkType",
                  typed("int", xpc::make_int64(kAccessNetworkType)));
    xpc::dict_set(options, "AVCMediaStreamNegotiatorTransportProtocolType",
                  typed("int", xpc::make_int64(kTransportProtocolType)));
    // 这两位是视频专用的：苹果那份抓包里，音频腿的 options 只有 AccessNetworkType /
    // TransportProtocolType / ClientSessionID 三个键，而视频腿多了这两个。带上它们设备
    // 会把这条流按"某个显示号的视频"处理，音频腿要的 `source: {audioSystemOutput: {}}`
    // 就拿不到了。
    if (!audio) {
        xpc::dict_set(options, "CoreDeviceVideoDisplayMode",
                      typed("string", xpc::make_string("DisplayByID")));
        xpc::dict_set(options, "VideoStreamForDisplayID",
                      typed("int", xpc::make_int64(display_id)));
    }
    // 会话号：一次起流一个，设备用它关联这条会话。
    // 必须是真正的 XPC UUID 对象（16 字节），不能是 UUID 文本——设备侧是
    // Swift Codable，类型对不上直接拒："Expected to decode UUID but found a
    // OS_xpc_string instead"（code 4864）。
    //
    // `shared_client_session_uuid` 非空时用它：苹果是**先起音频再起视频、两条腿共用同一个
    // ClientSessionID**（抓包里两次 start 的都是 6afeae6c-…），所以"这个会话有几条腿"是
    // 靠这一个 UUID 关联的。留空 = 本腿自己生成一个（单腿客户端的旧行为）。
    std::vector<uint8_t> session_id = shared_client_session_uuid;
    if (session_id.empty()) {
        const auto fresh = random_uuid_bytes();
        session_id.assign(fresh.begin(), fresh.end());
    }
    xpc::dict_set(options, "avcMediaStreamOptionClientSessionID",
                  typed("uuid", xpc::make_uuid(std::span<const uint8_t>(session_id))));
    xpc::dict_set(d, "options", std::move(options));

    xpc::dict_set(d, "receiverIP", xpc::make_string(receiver_ip));
    xpc::dict_set(d, "receiverPort", xpc::make_uint64(receiver_port));
    xpc::dict_set(d, "senderIP", xpc::make_string(sender_ip));
    // 这个键在 CoreDevice 的 feature 层是**必填**的：不发就直接被拒
    // （`code 4865 / Expected to find key timeout.`，两臂实测）。
    //
    // 曾经在这里写过"苹果不发这个键、所以它不到期"，那是**错的**：抓它的请求原文，
    // `timeout` 明明白白是 20。当时误读的来源是把设备会话表的回显当成了请求原文。
    if (timeout_seconds.has_value()) {
        xpc::dict_set(d, "timeout", xpc::make_uint64(*timeout_seconds));
    }
    xpc::dict_set(d, "type", xpc::make_string(audio ? "audio" : "video"));
    // 苹果有、我们没有的唯一一个键（抓包对齐出来的）。空 vector = 不发。
    if (!event_channel_uuid.empty()) {
        xpc::dict_set(d, "sessionEventChannel",
                      xpc::make_uuid(std::span<const uint8_t>(event_channel_uuid)));
    }
    return d;
}

std::unique_ptr<StreamSession> StreamSession::start(remote::Device &device,
                                                   const Request &request, std::string &err,
                                                   bool verbose,
                                                   remote::ServiceConnection *on_conn) {
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
    offer.is_audio = request.audio;
    offer.session_id = static_cast<uint32_t>(std::random_device{}());
    offer.call_id = remote::random_uuid_text();
    // `raw_offer` 非空时**整个跳过**我们自己的构造器，原样发这份字节。为什么要这个口子：
    // 逐字段"对齐到苹果"是有损的——对齐的人只会挑自己想到要对的字段。把苹果当场发出去的那
    // 482 字节原封不动发一遍，才是"请求侧差异"这个变量的**上界**：如果连这个都不改变租期，
    // 那差异就一定不在 offer 里，可以直接把整条 offer 假设关掉。
    // 注意它带着苹果那次的 SSRC/CallID，所以这一臂**不要**再发 RTCP（SSRC 对不上人）。
    std::vector<uint8_t> blob =
        request.raw_offer.empty() ? build_negotiator_offer(offer) : request.raw_offer;

    const auto &tunnel_params = device.tunnel_params();
    const std::vector<uint8_t> event_channel =
        request.session_event_channel.value_or(std::vector<uint8_t>{});
    auto input = build_start_request(tunnel_params.client_address, port,
                                     tunnel_params.server_address, blob, request.display_id,
                                     request.timeout_seconds,
                                     request.client_supported_features, event_channel,
                                     request.audio, request.client_session_uuid);
    xpc::Value output;
    if (on_conn != nullptr) {
        // 在调用方持有的那条连接上起流。注意 invoke 的返回值有三态，这里只关心
        // "设备有没有按我们的请求建会话"，所以非 Ok 一律算失败并把 err 交出去。
        const auto r = on_conn->invoke("com.apple.coredevice.feature.startmediastream",
                                       "com.apple.coredevice.action.mediastreamstart", input,
                                       output, 30000, err);
        if (r != remote::CallResult::Ok) {
            if (r == remote::CallResult::TransportError && err.empty()) {
                err = "在持有的连接上发 startmediastream 失败（链路断了）";
            }
            return nullptr;
        }
    } else if (!device.feature("com.apple.coredevice.displayservice",
                               "com.apple.coredevice.feature.startmediastream",
                               "com.apple.coredevice.action.mediastreamstart", input, output, err,
                               verbose, 30000)) {
        return nullptr;
    }

    Started started;
    started.answer = std::move(output);
    // 会话号就在请求体的 typed 包装里，从 input 读回来即可，不必让
    // build_start_request 多带一个出参。
    started.session_uuid =
        input.at("options").at("avcMediaStreamOptionClientSessionID").at("uuid").data;

    // answer 里设备侧的发送端口在 connection.sender.port，payload type 在
    // connection.streamConfig.RxPayloadType。
    const auto *connection = started.answer.find("connection");
    if (connection != nullptr) {
        // 这条流的 PT 是协商出来的，不是常量 100。RTCP 与视频共用一个 UDP 端口，
        // 拆包器只能靠 PT 区分二者，所以这个值必须交给它。取低 7 位，因为 RTP 头里
        // 的 payload type 字段就只有 7 位。
        if (const auto *sc = connection->find("streamConfig"); sc != nullptr) {
            started.payload_type =
                static_cast<uint8_t>(sc->at("RxPayloadType").as_int_or(100) & 0x7F);
            // 两个 SSRC 是回 RTCP 的两张"名字"：Local 指设备自己那条流（报告块里指认的
            // 那一位），Remote 是它给我们这一端分配的（我们当发送者填的那一位）。
            started.local_ssrc = static_cast<uint32_t>(sc->at("LocalSSRC").as_int_or(0));
            started.remote_ssrc = static_cast<uint32_t>(sc->at("RemoteSSRC").as_int_or(0));
        }
        if (const auto *sender = connection->find("sender"); sender != nullptr) {
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
    return next_packet(packet, peer_port, timeout_ms, err);
}

bool StreamSession::next_packet(std::vector<uint8_t> &packet, uint16_t &peer_port,
                                int timeout_ms, std::string &err) {
    return socket_->recv(packet, peer_port, timeout_ms, err);
}

bool StreamSession::send_rtp(const std::vector<uint8_t> &payload, uint16_t peer_port,
                             std::string &err) {
    return socket_->send(payload, peer_port, err);
}

bool StreamSession::stop(remote::Device &device, std::string &err, bool verbose) const {
    // feature() 每次调用都新开一条连接，正是这里要的：不能复用起流那条。
    auto input = xpc::make_dict();
    xpc::dict_set(input, "stopAll", xpc::make_bool(true));
    xpc::Value output;
    return device.feature("com.apple.coredevice.displayservice",
                          "com.apple.coredevice.feature.stopmediastream",
                          "com.apple.coredevice.action.mediastreamstop", input, output, err,
                          verbose, 10000);
}

uint16_t StreamSession::receiver_port() const { return socket_->local_port(); }

xpc::Value StreamSession::status(remote::Device &device, std::string &err, bool verbose) {
    auto input = xpc::make_dict();
    xpc::Value output;
    if (!device.feature("com.apple.coredevice.displayservice",
                        "com.apple.coredevice.feature.getmediastreamserverstatus",
                        "com.apple.coredevice.action.mediastreamstatus", input, output, err,
                        verbose, 10000)) {
        return xpc::Value {};
    }
    return output;
}

StreamSession::ServerState StreamSession::probe(remote::Device &device,
                                                const std::vector<uint8_t> &session_uuid,
                                                std::string &err, bool verbose) {
    const xpc::Value output = status(device, err, verbose);
    if (output.type == xpc::Type::Null) {
        return ServerState::Unknown;
    }
    // 实测回复形状：{sessions: [{connection: {options:
    // {avcMediaStreamOptionClientSessionID: {uuid: ...}}, streamConfig: {...}}}],
    // running: false, runDurationSeconds: 0}。
    const auto *sessions = output.find("sessions");
    if (sessions == nullptr || !sessions->is_array()) {
        err = "getmediastreamserverstatus 的回复里没有 sessions 数组";
        return ServerState::Unknown;
    }
    for (const auto &s : sessions->array) {
        // 一层层用 find 走，少一层就是 nullptr：这条会话条目里没有 uuid 不代表
        // 整个回复不可信，但也不能拿别的会话的 uuid 当我们自己的。
        const auto *options = s.find("connection");
        if (options == nullptr) {
            continue;
        }
        const auto *wrapped = options->at("options").find("avcMediaStreamOptionClientSessionID");
        if (wrapped == nullptr) {
            continue;
        }
        const auto *uuid = wrapped->find("uuid");
        if (uuid != nullptr && uuid->data == session_uuid) {
            return ServerState::Alive;
        }
    }
    return ServerState::Ended;
}

}  // namespace scrctl::media
