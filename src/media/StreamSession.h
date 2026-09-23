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
    };

    /// 在已经建好的会话（含隧道与 RSD 目录）上起流。
    /// 失败时 err 带上设备说的人话（CoreDevice.error 里的 NSLocalizedDescription）。
    static std::unique_ptr<StreamSession> start(scrctl::remote::Device &device,
                                                const Request &request, std::string &err,
                                                bool verbose = false);

    ~StreamSession();

    /// 取一个 RTP 包（UDP 数据报原文）。
    bool next_packet(std::vector<uint8_t> &packet, int timeout_ms, std::string &err);

    [[nodiscard]] uint16_t receiver_port() const;
    [[nodiscard]] const Started &started() const { return started_; }

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
                                                     uint32_t timeout_seconds);

}  // namespace scrctl::media
