#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "http2/Framing.h"
#include "net/TcpStream.h"
#include "xpc/XpcValue.h"

namespace scrctl::remote {

/// 我方在 RSD 上申报的身份。
///
/// UUID 不是可以随手生成的装饰：iOS 27.2 起设备只保留「每条隧道一个 RSD 连接」，
/// 新连接一来就把旧的换掉，并且**记住被换掉那个 peer 的 UUID**。若来客的 UUID
/// 与记忆不符，设备会把整台机器重新 attach 一遍——已公布的服务监听全部关闭、
/// 端口拒绝连接。所以同一隧道内跨进程、跨次运行必须用同一个 UUID，UUID 一旦
/// 定了就得持久化保存。
struct PeerIdentity {
    std::array<uint8_t, 16> uuid{};
    /// 0x0100000000000006 = 现代（非 legacy）RemoteXPC 客户。不给这个值，
    /// 设备会回 "Invalid or missing remote device connection version flags"。
    uint64_t version_flags = 0x0100000000000006ULL;
    uint64_t messaging_protocol_version = 7;
};

/// 把 8-4-4-4-12 形式的 UUID 文本转成 XPC 里那种 16 字节大端布局。
/// 文本形态的 UUID 前两位对应第一个字节，所以是「按数字对折半字节」依次左移，
/// 而不是按字符串下标切段——搞反会让设备认为我们是一个从未见过的 peer。
[[nodiscard]] std::optional<std::array<uint8_t, 16>> parse_uuid_text(std::string_view text);

/// 隧道内一条 RemoteXPC 控制通道：HTTP/2 帧 + 帧里装的 XPC 消息。
///
/// 生命周期与一条 TCP 连接一致。设备上的每个 DDI 服务在 RSD 表里都有自己的端口，
/// 起服务时要另开一条 TCP 连接、再各走一遍这里的握手，所以这个类要能廉价地
/// 反复构造。
class Channel {
public:
    /// 在已建立的连接上跑完 HTTP/2 层握手（建流 + 等对端 SETTINGS 并 ACK）。
    ///
    /// 帧顺序是有约束的，不是随便排：设备侧 RemoteServiceDiscovery 会校验顺序，
    /// 主通道 (stream 1) 的 HEADERS 必须先于终止帧、回信通道 (stream 3) 的
    /// HEADERS 必须先于它的 INIT_HANDSHAKE 帧，否则直接被 xpc_connection_cancel()
    /// 拆掉。顺序照 Apple 自家工具抓包的结果来。
    static std::optional<Channel> open(net::TcpStream &socket, std::string &err,
                                       bool verbose = false);

    /// 申报身份并读回 peer_info。**只有 RSD 控制通道需要这一步**，服务连接上
    /// 设备不期望它，发了会被当成一次普通请求处理。
    bool announce_device(const PeerIdentity &identity, std::string &err);

    /// 发一个请求。`want_reply` 置起 WANTING_REPLY 标志。
    bool send_request(const xpc::Value &body, bool want_reply, std::string &err);

    /// 取回下一条带字典载荷的消息（心跳、空载荷帧、控制帧会被跳过）。
    bool receive(xpc::Value &out, int timeout_ms, std::string &err);

    /// 一次往返。
    bool call(const xpc::Value &request, xpc::Value &reply, int timeout_ms, std::string &err);

    /// 设备在握手时自报的身份：Model / OSVersion / Udid / Properties 等。
    [[nodiscard]] const xpc::Value *peer_info() const {
        return peer_info_.has_value() ? &*peer_info_ : nullptr;
    }

    [[nodiscard]] net::TcpStream &socket() { return socket_; }

    /// 主通道 / 回信通道的流号。1 发请求，3 收异步回信（服务连接上设备只用 1）。
    static constexpr uint32_t kRootStream = 1;
    static constexpr uint32_t kReplyStream = 3;

    /// 公开只为 std::optional::emplace 能构造它——optional 的内部实现不在本类
    /// 作用域里，私有构造函数它调不动。这样造出来的实例还没握手，别直接用，
    /// 要可用的通道请走 open()。
    explicit Channel(net::TcpStream &socket) : socket_(socket) {}

private:
    bool start(std::string &err);
    bool send_data(uint32_t stream_id, std::span<const uint8_t> payload, std::string &err);
    /// 收一批字节并处理其中的完整帧；返回 false 表示超时、GOAWAY 或连接已终止。
    bool pump(int timeout_ms, std::string &err);
    bool handle_frame(const http2::Frame &f, std::string &err);
    /// 收一条文件传输：先在设备推来的那条流上表态接受，再读满 size 字节。
    bool receive_file(uint32_t stream_id, uint64_t size,
                      std::chrono::steady_clock::time_point deadline, std::vector<uint8_t> &out,
                      std::string &err);
    /// 递归收集字典/数组里所有 FileTransfer 占位，顺序即流号顺序。
    static void collect_files(const xpc::Value &v, std::vector<xpc::Value *> &out);
    /// 把回信字典里所有 FileTransfer 占位换成真字节。
    bool materialize_files(xpc::Value &reply, std::chrono::steady_clock::time_point deadline,
                           std::string &err);
    /// 从各流的缓冲里取一条完整回信，并把随信推来的文件字节填进去。
    /// 取不到时返回 false 且 err 为空（表示"还得继续等"），err 非空才表示真的坏了。
    bool take_message(xpc::Value &out, std::chrono::steady_clock::time_point deadline,
                      std::string &err);
    void replenish_inbound_window(uint32_t stream_id);
    /// 写一帧并在 verbose 下打出原始字节。协议对不上时，唯一有用的证据就是
    /// 「我们究竟往线上写了什么」，靠推断排错在这里的性价比极低。
    bool send_bytes(std::span<const uint8_t> data, std::string &err);

    net::TcpStream &socket_;
    std::vector<uint8_t> rx_;  ///< 还没凑成一帧的原始字节
    /// 按流号分开缓冲：一条消息可能被拆成多个 DATA 帧，不同流的消息混在一个缓冲里
    /// 拼，顺序一错就整条解不出来。
    std::map<uint32_t, std::vector<uint8_t>> pending_;
    /// 偶数号流（设备侧发起）上跑的不是 XPC 消息而是文件裸字节，不能混进
    /// pending_ 里当消息解——那样每个字节都会被拿去当帧头/magic 校验一次。
    std::map<uint32_t, std::vector<uint8_t>> raw_;
    std::optional<xpc::Value> peer_info_;
    uint64_t next_message_id_ = 0;

    // 流控：我方发出去的量受对端授权，对端发进来的量由我们授予。
    uint32_t peer_initial_window_ = http2::kDefaultInitialWindowSize;
    int64_t outbound_connection_ = http2::kDefaultInitialWindowSize;
    std::map<uint32_t, int64_t> outbound_streams_;
    uint32_t peer_max_frame_ = static_cast<uint32_t>(http2::kDefaultMaxFrameSize);
    uint64_t consumed_connection_ = 0;
    std::map<uint32_t, uint64_t> consumed_per_stream_;

    /// 对端 SETTINGS 的原始条目。留着是因为「收到非 ACK 的 SETTINGS」这件事本身
    /// 是握手能不能往下走的信号，而空 SETTINGS 也是合法值，不能用「条目非空」
    /// 来判断收到了。
    std::vector<std::pair<uint16_t, uint32_t>> peer_settings_;
    bool settings_received_ = false;
    bool terminated_ = false;
    bool verbose_ = false;
};

}  // namespace scrctl::remote
