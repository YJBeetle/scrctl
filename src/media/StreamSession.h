#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "media/MediaOffer.h"
#include "net/UdpSocket.h"
#include "remote/Device.h"

namespace scrctl::media {

/// 建立一条设备视频或音频媒体会话，收发该会话使用的 UDP 数据报。
///
/// UDP 端口先绑定，再调用 startmediastream。已测设备在起流回复后立即发送 RTP，
/// 提前绑定可以接住视频参数集和首个关键帧，避免在后续恢复之前无法解码。
/// 起流顺序见 [CoreDevice §11](../../docs/coredevice.md#11-屏幕视频流的线上细节实测iphone-144--ios-270--usb)。
class StreamSession {
public:
    struct Request {
        uint16_t receiver_port = 0;  ///< 0 表示从 49152..65151 中随机选择端口
        /// 视频目标的显示器 ID；音频请求不携带显示器 options。
        uint32_t display_id = 1;
        /// 发往设备的 timeout，单位为秒，是 RTCP 空闲超时，不是等待起流 RPC 回复的时限。
        /// 已测设备在 answer 的 RTCPTimeoutInterval 中回显此值；有效 RR 可重置计时器。
        /// UDP 发送修复后的续期对照见 [CoreDevice §13](../../docs/coredevice.md#修好之后第一次真测租期能续pli-有效fir-有害)。
        ///
        /// 默认 3600 供未实现周期 RTCP 的一次性调用方使用。产品 FramePump 和 AudioPump
        /// 显式使用 20 秒，并每秒发送 RR。默认值、保留起流连接或查询状态都不能替代 RTCP。
        /// 视频和音频可以同时存在，各自有会话与续期状态，见
        /// [CoreDevice §17.2](../../docs/coredevice.md#172-音频腿接进产品四个问题的真机读数toolsaudio_pump_probeasan-下跑)。
        ///
        /// nullopt 表示省略整个键，保留给协议探针使用。已测 CoreDevice feature 在省略时
        /// 返回 code 4865 / Expected to find key timeout，正常起流应提供此参数。
        std::optional<uint32_t> timeout_seconds = 3600;
        /// 可选的 sessionEventChannel，值为 16 字节 XPC UUID；未提供或为空时省略。
        /// 其事件通道的注册与对端语义尚未确认，不能把一个 UUID 本身当作保活机制。
        /// 抓包中显示几何推送属于 deviceinfo 的 displayinfoupdates，不能据此推断此字段
        /// 是否有对端；UUID 仅在抓包中出现一次也不能证明没有注册过程。
        /// 相关观察见 [CoreDevice §13](../../docs/coredevice.md#13-停流关键帧请求与恢复实测) 和
        /// [§16](../../docs/coredevice.md#16-displayinfoupdates设备自己报的显示几何实测iphone144--ios-270--usb)。
        std::optional<std::vector<uint8_t>> session_event_channel;
        /// 申报的客户端能力位掩码，默认 140 来自可用请求的观测。
        /// 设备回复的 supportedFeatures=972 是设备能力，不能直接当作客户端应申报的值。
        /// 已测更改未获得稳定的编码提升；各位的含义尚未完整确认，见
        /// [CoreDevice §11](../../docs/coredevice.md#11-屏幕视频流的线上细节实测iphone-144--ios-270--usb)。
        uint64_t client_supported_features = 140;
        /// 选择音频请求：type 为 audio，省略显示器 options，普通 offer 使用 mode 6。
        /// 视频请求的 type 为 video，普通 offer 使用 mode 5。
        bool audio = false;
        /// avcMediaStreamOptionClientSessionID 的 16 字节 XPC UUID 原文；为空时生成新 UUID。
        /// 参考客户端曾为音视频共用此值；已测独立 UUID 同样可以并行起流，产品分别使用。
        /// 若共享 UUID，probe() 的 Alive 只能表示至少一条同 UUID 会话存在，无法区分音视频。
        /// 验证范围见 [CoreDevice §17.2](../../docs/coredevice.md#172-音频腿接进产品四个问题的真机读数toolsaudio_pump_probeasan-下跑)。
        std::vector<uint8_t> client_session_uuid;
        /// 普通路径的 offer 参数。start() 会根据 audio 设置 is_audio，并为本次协商
        /// 生成 session_id 和 call_id，覆盖传入 Offer 中这三个字段。
        Offer offer;
        /// 非空时跳过 XML offer 构造，原样作为 negotiatorOffer 的 XPC Data。
        /// 供抓包重放和 binary / XML 格式对照，不修改样本中的 SSRC、CallID 或其他内容。
        /// 调用方负责选择与 audio 请求类型相符的样本。
        /// 发送 RTCP 应使用本次 answer 的 SSRC，不能使用普通构造路径另外生成的身份。
        /// 当前格式验证见 [BPLIST_COMPATIBILITY](../../docs/BPLIST_COMPATIBILITY.md#真机对照)。
        std::vector<uint8_t> raw_offer;
    };

    struct Started {
        /// answer 的 connection.sender.port，设备发送媒体使用的端口。
        /// 缺失时为 0，接收端允许任意源端口；反馈目的端口需依据协商结果或实际对端确定。
        uint16_t sender_port = 0;
        /// 协商的媒体 RTP payload type，来自 streamConfig.RxPayloadType 的低七位。
        /// 缺失时保留默认 100；音频也使用协商值。拆包前需区分同端口收到的裸 RTCP。
        uint8_t payload_type = 100;
        /// answer 的 streamConfig.LocalSSRC，名字采用设备视角。
        /// 已测值与设备发送的 RTP SSRC 一致；客户端 RTCP 报告块用它指明被报告的媒体源。
        uint32_t local_ssrc = 0;
        /// answer 的 streamConfig.RemoteSSRC，表示客户端侧 SSRC。
        /// 已测设备回显 offer 声明的 SSRC，不应理解为设备总会另行分配新值。
        /// 客户端发送 RTCP 时使用本次回复的此值，避免与报告的会话身份不匹配。
        uint32_t remote_ssrc = 0;
        /// 起流回复原文，供上层读取和记录完整协商结果。
        scrctl::xpc::Value answer;
        /// 本次请求中 ClientSessionID 的 16 字节 XPC UUID 原文，用于 probe() 匹配会话。
        /// 当前 stop() 使用 stopAll，不按此 UUID 筛选。
        std::vector<uint8_t> session_uuid;
    };

    /// 在已建立隧道并取得 RSD 目录的 Device 上起流，先绑定接收 UDP 端口。
    /// 失败时返回 nullptr，并通过 err 提供传输或 CoreDevice 错误信息。
    ///
    /// 默认通过 Device::feature 新建服务连接，调用结束后释放。on_conn 非空时借用
    /// 调用方持有的连接，不取得所有权，也不在调用结束后关闭它。
    /// 已测设备在同一起流连接上接收第二次请求时会关闭连接并结束媒体会话，因此后续
    /// status、stop 等请求使用新连接；保留连接本身不能替代周期 RTCP。
    /// 连接复用的观测范围见 [CoreDevice §13](../../docs/coredevice.md#13-停流关键帧请求与恢复实测)。
    static std::unique_ptr<StreamSession> start(scrctl::remote::Device &device,
                                                const Request &request, std::string &err,
                                                bool verbose = false,
                                                scrctl::remote::ServiceConnection *on_conn =
                                                    nullptr);

    ~StreamSession();

    /// 接收一个 UDP 数据报原文，可能是 RTP 或 RTCP；timeout_ms 为本次接收等待时限。
    /// 协议分类与媒体拆包由调用方完成。
    bool next_packet(std::vector<uint8_t> &packet, int timeout_ms, std::string &err);
    /// 同上，并返回该数据报的源端口，供调用方识别对端和确定反馈目的地。
    bool next_packet(std::vector<uint8_t> &packet, uint16_t &peer_port, int timeout_ms,
                     std::string &err);

    /// 向隧道对端指定端口发送原始 UDP 载荷，当前用于 RTCP 反馈。
    /// 不添加 RTP 头；调用方负责完整的 RTCP 编码、SSRC 和目的端口。
    bool send_rtp(const std::vector<uint8_t> &payload, uint16_t peer_port, std::string &err);

    /// 通过新服务连接发送 stopmediastream，当前入参固定为 stopAll: XPC Bool true。
    /// 该操作停止设备上所有媒体会话，包括音频和视频，不只停止本对象对应的会话。
    /// 已测设备要求 stopAll 为 Bool；回复 stoppedStreams 中的编号对应 offer 的
    /// u32 session_id，而非 ClientSessionID UUID。复用起流连接发送 stop 曾导致设备服务异常。
    /// 参数形状与作用范围见 [CoreDevice §13](../../docs/coredevice.md#13-停流关键帧请求与恢复实测)。
    bool stop(scrctl::remote::Device &device, std::string &err, bool verbose = false) const;

    [[nodiscard]] uint16_t receiver_port() const;
    [[nodiscard]] const Started &started() const { return started_; }

    /// 以设备 sessions 列表判断会话是否仍存在。
    /// 媒体暂时无包不等于会话结束，例如静止画面可能不产生新的视频帧；
    /// Alive 也不保证正在收包或已成功解码，上层需结合媒体与恢复状态判断。
    enum class ServerState {
        /// 设备 sessions 列表中存在匹配 ClientSessionID 的会话。
        Alive,
        /// 已识别的 sessions 列表中未找到匹配 UUID，由上层决定是否重新协商。
        Ended,
        /// 查询失败或回复缺少可识别的 sessions 列表；不能等同于 Ended。
        Unknown,
    };

    /// 通过新连接查询 getmediastreamserverstatus，并按 session_uuid 匹配 ClientSessionID。
    /// 多个媒体流共用 UUID 时只能判断该组是否仍有会话，不能确定某条流的存活状态。
    [[nodiscard]] static ServerState probe(remote::Device &device,
                                           const std::vector<uint8_t> &session_uuid,
                                           std::string &err, bool verbose = false);

    /// 通过新连接返回 getmediastreamserverstatus 的完整输出，失败时返回 Null。
    /// 除会话匹配外，回复中的设备计数器可用于检查 RTCP 是否实际到达等传输问题。
    [[nodiscard]] static scrctl::xpc::Value status(remote::Device &device, std::string &err,
                                                   bool verbose = false);

private:
    StreamSession(std::unique_ptr<scrctl::net::UdpSocket> sock, Started started)
        : socket_(std::move(sock)), started_(std::move(started)) {}

    std::unique_ptr<scrctl::net::UdpSocket> socket_;
    Started started_;
};

/// 组装 startmediastream 的 CoreDevice.input，不进行网络 I/O，供起流和离线协议校验使用。
/// options 的参数按 int / string / uuid 标签包装；会话与事件通道 UUID 使用 16 字节
/// XPC UUID 对象，不能用 UUID 文本代替。offer_bytes 原样放入 XPC Data，不转换 plist 格式。
/// audio 决定请求类型及是否省略显示器 options；空共享 UUID 会生成新 ClientSessionID，
/// 空事件通道 UUID 和 nullopt timeout 分别省略对应键。省略 timeout 在已测设备上被拒。
/// 类型约束和请求样本见 [CoreDevice §13、§14](../../docs/coredevice.md#14-让设备自己交代入参形状toolsfeature_schema_probe)。
[[nodiscard]] scrctl::xpc::Value build_start_request(const std::string &receiver_ip,
                                                     uint16_t receiver_port,
                                                     const std::string &sender_ip,
                                                     const std::vector<uint8_t> &offer_bytes,
                                                     uint32_t display_id,
                                                     std::optional<uint32_t> timeout_seconds,
                                                     uint64_t client_supported_features,
                                                     const std::vector<uint8_t> &event_channel_uuid,
                                                     bool audio = false,
                                                     const std::vector<uint8_t> &shared_client_session_uuid = {});

}  // namespace scrctl::media
