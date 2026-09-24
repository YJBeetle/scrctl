#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "media/MediaOffer.h"
#include "net/UdpSocket.h"
#include "remote/Device.h"

namespace scrctl::media {

/// 起一条设备屏幕的视频流。
///
/// 流程上有一个反直觉的地方：**必须先绑好 UDP 端口再发 startmediastream**。
/// 设备一返回 answer 就开始往 receiverIP:receiverPort 推 RTP，晚绑一微秒就丢掉
/// 开头几个包，而开头恰好是 VPS/SPS/PPS 和第一个关键帧——丢了后面整段都解不出来。
class StreamSession {
public:
    struct Request {
        uint16_t receiver_port = 0;  ///< 0 = 随机挑一个
        uint32_t display_id = 1;
        uint32_t timeout_seconds = 20;
        /// 申报给设备的主机能力位掩码。观测值是 140，而设备自己回
        /// `supportedFeatures: 972`——差着的位里可能藏着更高档的编码器配置，
        /// 所以这个数要能改，别焊死在常量上。
        uint64_t client_supported_features = 140;
        Offer offer;
    };

    struct Started {
        /// 设备侧发源的端口（answer 里带；answer 没带则为 0，表示任意源都收）。
        uint16_t sender_port = 0;
        /// 协商出来的视频 payload type（answer 的 streamConfig.RxPayloadType）。
        /// 拆包器只认这个 PT，其余（同端口到达的 RTCP）跳过。
        uint8_t payload_type = 100;
        /// answer 原文，供上层记录协商结果。
        scrctl::xpc::Value answer;
        /// 我们这次起流用的会话号（`avcMediaStreamOptionClientSessionID`），
        /// 16 字节的 XPC UUID 原文。stopmediastream 要拿它来指认是哪条会话。
        std::vector<uint8_t> session_uuid;
    };

    /// 在已经建好的会话（含隧道与 RSD 目录）上起流。
    /// 失败时 err 带上设备说的人话（CoreDevice.error 里的 NSLocalizedDescription）。
    static std::unique_ptr<StreamSession> start(scrctl::remote::Device &device,
                                                const Request &request, std::string &err,
                                                bool verbose = false);

    ~StreamSession();

    /// 取一个 RTP 包（UDP 数据报原文）。
    bool next_packet(std::vector<uint8_t> &packet, int timeout_ms, std::string &err);
    /// 同上，并带出对端端口——回 RTCP 时要发给"包是从哪个端口来的"，
    /// 而不是猜一个。
    bool next_packet(std::vector<uint8_t> &packet, uint16_t &peer_port, int timeout_ms,
                     std::string &err);

    /// 往隧道对端的某个端口发一个数据报（RTCP 反馈用）。
    bool send_rtp(const std::vector<uint8_t> &payload, uint16_t peer_port, std::string &err);

    /// 停掉这条流。**必须另开一条连接**：设备侧对"复用发起 start 的那条连接发
    /// stop"有崩溃前科。
    ///
    /// 入参形状是设备自己教的：四种形状都回 "Expected to find key stopAll."，
    /// 带上 `stopAll: Bool` 就成功，回 `{serverInfo: {running: false...},
    /// stoppedStreams: [<u32>]}`——那个 u32 就是 offer 里的 session_id，不是
    /// 起流时那个 UUID。`stopAll` 给整数会被 Swift Codable 拒（"Expected to
    /// decode Bool but found a OS_xpc_uint64"）。
    bool stop(scrctl::remote::Device &device, std::string &err, bool verbose = false) const;

    [[nodiscard]] uint16_t receiver_port() const;
    [[nodiscard]] const Started &started() const { return started_; }

    /// 设备侧媒体流服务的状态。**收不到包有两种完全不同的原因，必须分开**：
    /// 静止画面上编码器本来就不发（流好着），以及设备把流结束掉了（流死了）。
    /// 只看"多久没包"把它们混成一个，结果就是静止画面每 3 秒被无谓地重起一次。
    enum class ServerState {
        /// 我们这条会话还在设备的 sessions 列表里。
        Alive,
        /// 已经不在了——设备结束了流，必须重起才能再收到画面。
        Ended,
        /// 问不到，或回复形状不认识。按"未知"处理，别当成 Ended。
        Unknown,
    };

    /// 问一次 getmediastreamserverstatus，看 `session_uuid` 还在不在设备的会话
    /// 列表里。另开一条连接，理由同 stop()。
    [[nodiscard]] static ServerState probe(remote::Device &device,
                                           const std::vector<uint8_t> &session_uuid,
                                           std::string &err, bool verbose = false);

private:
    StreamSession(std::unique_ptr<scrctl::net::UdpSocket> sock, Started started)
        : socket_(std::move(sock)), started_(std::move(started)) {}

    std::unique_ptr<scrctl::net::UdpSocket> socket_;
    Started started_;
};

/// 组装 startmediastream 的 CoreDevice.input。单独拆出来是为了能离线比对：
/// 请求体里任何一个字段错了，设备的反应都是"不回话"或一个语义模糊的错误码。
[[nodiscard]] scrctl::xpc::Value build_start_request(const std::string &receiver_ip,
                                                     uint16_t receiver_port,
                                                     const std::string &sender_ip,
                                                     const std::vector<uint8_t> &offer_bplist,
                                                     uint32_t display_id,
                                                     uint32_t timeout_seconds,
                                                     uint64_t client_supported_features);

}  // namespace scrctl::media
