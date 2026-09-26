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
        /// 发往设备的 `timeout` 键：**这条会话的租期长度（秒）**，不是"等 answer 的超时"。
        ///
        /// 设备把它原样抄进 answer 的 `RTCPTimeoutInterval`，然后按"距离上次收到我们
        /// RTCP 多久"倒数，到点就把这条会话从设备表里摘掉。**它会复位**——这一点是
        /// 2026-09-27 才定下来的，此前这里的注释写着"中途不看我们发了什么：喂画面、回
        /// 各种形状的 RTCP、查状态都改不了那个时刻"，那句**观测上成立而结论是错的**：
        /// 那二十多臂发的 UDP 数据报一个都没到设备（`build_udp_datagram` 把长度字段和
        /// 校验和都算错了范围，内核静默丢弃），所以"回 RTCP 不续命"测的其实是
        /// "什么都没回"。修好之后同一台设备：什么都不发 +19.97s 死，每秒一个 RR 活过
        /// 40 秒，设备侧 socket 的 `pkts in` 从 0 变成 41。完整推导在 FramePump.cpp 的
        /// kSessionLeaseSeconds 上面。
        ///
        /// 所以这一位的真实含义变了：它不是"多久必须换一次会话"，而是**"我们回 RTCP 的
        /// 容忍空档"**。报 20 并要求自己每秒发一个 RR，是苹果的做法（抓包里它的
        /// `timeout` 就是 20），好处是进程被 SIGKILL 或崩掉时设备只被占住 20 秒——一台
        /// 设备一次只容一条流，Xcode 的 DeviceHub 共用这一格。
        ///
        /// 默认值留着 3600 是给**不回 RTCP 的调用方**的（各探针、一次性抓帧的工具）：
        /// 对它们来说租期就是唯一的时间边界，报 20 会让每次跑都断在 20 秒。真正在跑的
        /// 产品路径（`media/FramePump`）显式报 20 并按秒回 RR。用这个默认值又想活得更久，
        /// 就得自己发 RTCP——别把默认值当保活。
        ///
        /// **`nullopt` = 整个键都不发**。留着它不是为了用，是为了把一条测过的事实钉在
        /// 可执行的探针上：这个键在 CoreDevice 的 feature 层是**必填**的，不发就起不了流
        /// （`code 4865 / "Expected to find key timeout."`，两臂都撞在这上面）。
        ///
        /// 关于"那苹果为什么不断"，这一栏走过三次弯路，都记在这儿以免再走：
        ///   ① 曾根据设备会话表里苹果条目没有 `timeout`/`type` 键，推断"苹果不走这个
        ///      feature"。**错了**——抓它的请求原文，走的就是 `mediastreamstart`，`timeout`
        ///      也报 20。会话表是回显，回显是子集不是同射。
        ///   ② 曾把剩下的差异归到"苹果那条连接是 HTTP/2 而我们和 p3 是裸 XPC"。**也错了**：
        ///      我们的服务连接同样是 HTTP/2 + DATA 帧装 XPC（remote/RemoteXpc.cpp），
        ///      前置三帧 `0x1/0x201/0x400001` 和苹果的逐字节一致。
        ///   ③ 曾把"我们 20 秒死而苹果 74 秒不断"读成设备侧有个我们没控住的结构性差异
        ///      （音频腿、sessionEventChannel、LTRP、VRAE、宿主连接、流标签……八项逐一
        ///      判完，全带数）。**根因在我们自己发出去的字节里**：那 74 秒里苹果每秒有
        ///      RTCP 落到设备的 socket 上，而我们一个都没到。
        /// 苹果的实测事实留着（同一台 iPhone 13 mini / iOS 27.0，一次 95.8 秒抓包）：报
        /// 20 秒租期，视频 RTP 从起流一路发到抓包结束共 74 秒**一秒空档都没有**、SSRC 与
        /// 两端端口全程不变、`startmediastream` 整场只有两次（音频一次 + 视频一次，不是
        /// 重试），而那条 carrying 两次起流的连接从头到尾没有 FIN/RST。
        /// （后来两条也都判完了：握着起流那条连接并持续 `service()`、在 `deviceinfo` 上挂
        /// `displayinfoupdates` 订阅、在 `universalhidservice` 上附着，各自一臂全部照旧
        /// 死在 19.97~20.00 秒；RCTL 的字段语义另外量出了我们填错的一位。见 docs §13。）
        std::optional<uint32_t> timeout_seconds = 3600;
        /// 发往设备的 `sessionEventChannel` 键：**一个 XPC UUID**，不是端点句柄。
        ///
        /// 这是从设备侧抓包（utun7 上隧道已解封装，请求原文是明文）里挖出来的**我们和
        /// Xcode DeviceHub 之间唯一差的那个键**：苹果的 `startmediastream` 请求里
        /// `timeout` 也是 20、`type` 也是 audio/video，键集合和我们一样，只多了这个。
        ///
        /// 试过一臂：填一个我们这边没人登记的 UUID——**19.961 秒死**，所以"有个 UUID
        /// 就够了"不成立。至于苹果那边这个 UUID 换来了什么，抓包给出的答案不是"续命"：
        /// 全场唯一那条 `XPCSideChannel.uniqueIdentifier` + `sideChannelStatus` 推送落在
        /// 设备端口 54583 = `com.apple.coredevice.deviceinfo` 上，而它的 feature 列表里有
        /// `displayinfoupdates`——那是**显示器/方向/背光状态订阅**，和媒体会话的租期无关。
        /// 所以这一位目前解释不了苹果的 74 秒不断，别把它当保活手段。
        ///
        /// 而且"苹果那边一定接了个对端"这个前提也是猜的：拿它那两条腿的两个 UUID 的
        /// 16 字节原文在整场抓包里搜，**各只出现 1 次**，就是各自那条起流请求，
        /// 之后设备与客户端都没再提过（对比 `ClientSessionID` 出现 6 次）。
        /// 也就是说苹果自己也没在这个窗口里给它接上对端——我们填的悬空 UUID
        /// 恰好就是苹果的形状，不是我们漏了一步。
        std::optional<std::vector<uint8_t>> session_event_channel;
        /// 申报给设备的主机能力位掩码。观测值是 140，而设备自己回
        /// `supportedFeatures: 972`——差着的位里可能藏着更高档的编码器配置，
        /// 所以这个数要能改，别焊死在常量上。
        uint64_t client_supported_features = 140;
        /// 起**音频腿**：`type:"audio"`、options 里不带那两个显示键、offer 走 mode 6。
        bool audio = false;
        /// 共享的 `avcMediaStreamOptionClientSessionID`（16 字节 XPC UUID 原文）。
        /// 空 = 本腿自己生成一个。苹果是两条腿用同一个，所以要做那组实验必须能传进来。
        std::vector<uint8_t> client_session_uuid;
        Offer offer;
        /// 非空时**原样**当作 `negotiatorOffer` 的字节发出去，绕开 `build_negotiator_offer`。
        ///
        /// 为什么要有：offer 是"bplist 套 zlib 套 protobuf"三层、几十个字段，而"逐字段
        /// 比对我们的和苹果的"这件事一直做不干净——每次都有人说"这一位大概无所谓"。
        /// 手里正好有苹果当场发出去的那 482 字节原文（抓包解出来的），最省事的判据就是
        /// 把它**一个字节都不改地**发一遍：设备如果对 offer 里的某一位有反应，这样一定
        /// 会反应出来；如果这样仍然 20 秒死，那 offer 这一整层就可以判掉，不用再猜字段。
        std::vector<uint8_t> raw_offer;
    };

    struct Started {
        /// 设备侧发源的端口（answer 里带；answer 没带则为 0，表示任意源都收）。
        uint16_t sender_port = 0;
        /// 协商出来的视频 payload type（answer 的 streamConfig.RxPayloadType）。
        /// 拆包器只认这个 PT，其余（同端口到达的 RTCP）跳过。
        uint8_t payload_type = 100;
        /// answer 的 `streamConfig.LocalSSRC` —— **设备自己那条流的 SSRC**，也就是它推
        /// 过来的每个 RTP 头里那个数（实测对上过）。名字是从**设备的视角**起的。
        /// 我们发 RTCP 时要拿它当报告块里"指认哪条流"的那一位。
        uint32_t local_ssrc = 0;
        /// answer 的 `streamConfig.RemoteSSRC` —— 设备**给我们这一端分配的** SSRC。
        /// 我们发出的 RTCP 的发送者 SSRC 必须填这个，不能自己编：填错了就等于"一个从没
        /// 收过包的源发来的报告"，设备按 SSRC 配对时对不上号。
        /// （这两个名字曾经被反着理解，于是所有 RTCP 臂都填错了人。）
        uint32_t remote_ssrc = 0;
        /// answer 原文，供上层记录协商结果。
        scrctl::xpc::Value answer;
        /// 我们这次起流用的会话号（`avcMediaStreamOptionClientSessionID`），
        /// 16 字节的 XPC UUID 原文。stopmediastream 要拿它来指认是哪条会话。
        std::vector<uint8_t> session_uuid;
    };

    /// 在已经建好的会话（含隧道与 RSD 目录）上起流。
    /// 失败时 err 带上设备说的人话（CoreDevice.error 里的 NSLocalizedDescription）。
    ///
    /// `on_conn` 是给"起流用哪条连接"留的口子：默认另开一条、调用完就丢（那是
    /// Device::feature 的语义），传了它就在这条**由调用方持有**的连接上起流，并且
    /// **不关掉它**。
    ///
    /// 这条口子的实测后果要写清楚，因为它是反直觉的（三臂 A/B/C，见 docs §13）：
    ///   * 握着不放、之后**不再**在这条连接上发任何东西 —— 连接能活过整场（30 秒实测没断），
    ///     但会话照旧死在自己报的那个秒数上，所以"有宿主连接"不延长租期；
    ///   * 握着它、又在**同一条**连接上发第二次请求（哪怕只是一个只读的
    ///     `getmediastreamserverstatus`）—— 连接和媒体会话**同时**在 +241ms 被设备带走，
    ///     两次独立复现。视频包冻在 61–65 个、一个 SR 都没收到。
    /// 所以规则是：**这条连接只能用来起流，任何后续请求都要另开连接**（和 `stop()` 那条
    /// "必须另开"的已知规则同族）。传了 `on_conn` 又不守这条规则，等于自己把流戳死。
    static std::unique_ptr<StreamSession> start(scrctl::remote::Device &device,
                                                const Request &request, std::string &err,
                                                bool verbose = false,
                                                scrctl::remote::ServiceConnection *on_conn =
                                                    nullptr);

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

    /// 设备回的状态原文（整个 `getmediastreamserverstatus` 的输出）。单独开这一条是
    /// 为了"判活"之外的用途：`probe()` 只回一个三值枚举，而当我们想查"设备到底有没有
    /// 收到我们发过去的 RTCP"时，需要的恰恰是它自己报的那些计数器——那种问题没法用
    /// 枚举回答。取不到时返回 Null。
    [[nodiscard]] static scrctl::xpc::Value status(remote::Device &device, std::string &err,
                                                   bool verbose = false);

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
                                                     std::optional<uint32_t> timeout_seconds,
                                                     uint64_t client_supported_features,
                                                     const std::vector<uint8_t> &event_channel_uuid,
                                                     bool audio = false,
                                                     const std::vector<uint8_t> &shared_client_session_uuid = {});

}  // namespace scrctl::media
