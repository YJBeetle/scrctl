#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "net/Stack.h"

namespace scrctl::net {

/// 隧道之上的最小 IPv6 + TCP 客户端。
///
/// 为什么不用 lwIP：本项目的需求窄到点对点、静态地址、无 ND/ARP、单连接、
/// 纯客户端，而 lwIP 要么 FetchContent 走 https（受限）、要么 vendor 数百
/// 文件难核验来源。手写这块的失败模式也更好判定——一旦走到 RSD 握手，
/// 校验和或序号有任何错就只会挂起，成功则返回 85 个服务的表，是硬证据。
///
/// 不支持：分段重组、窗口缩放、选择性确认、并发连接、服务端监听。
class TcpStream : public TcpEndpoint {
public:
    /// 地址来自隧道握手协商出的那一对，由 Stack 持有；这里只管一条连接。
    explicit TcpStream(Stack &stack);
    ~TcpStream() override;

    /// 完成三次握手。peer_port 是隧道内端口（如 RSD 端口）。
    bool connect(uint16_t peer_port, std::string &err);

    /// 发送全部字节（内部按 MSS 切分）。
    bool send(std::string_view data, std::string &err);

    /// 读若干字节，最多等 timeout_ms。返回 false 表示超时、对端关闭或出错。
    bool recv(std::vector<uint8_t> &out, int timeout_ms, std::string &err);

    void close();

    [[nodiscard]] bool connected() const { return established_; }

    /// 收到的"序号不连续"的段数与字节数。本实现不重排也不缓存：落在期望序号
    /// 之外的数据报会被丢掉（同时回一个重复 ACK 催对端重传）。排查"HTTP/2 说
    /// 帧长过大"这类症状时第一个要看的数就是这里——那是字节流缺了一段的表现。
    [[nodiscard]] uint64_t dropped_segments() const { return dropped_segments_; }
    [[nodiscard]] uint64_t dropped_bytes() const { return dropped_bytes_; }

private:
    struct Segment {
        uint8_t flags = 0;
        uint32_t seq = 0;
        uint32_t ack = 0;
        std::vector<uint8_t> payload;
    };

    bool send_segment(uint8_t flags, const std::vector<uint8_t> &payload, std::string &err);
    /// 复用层把目的端口属于自己的段交进来（泵线程调用）。
    void on_segment(const uint8_t *l4, std::size_t len) override;
    /// 处理一个属于本连接的段。**必须持有 m_ 调用**（泵线程派发时已经持着）。
    bool handle_segment(const uint8_t *l4, std::size_t len, std::string &err);
    /// 等某个条件成立，或到时间/隧道断掉。返回 true 表示条件成立。
    template <typename Pred>
    bool wait_for(Pred ready, int timeout_ms, std::string &err);

    Stack &stack_;

    /// 连接状态。入站段来自泵线程、出站与读取来自应用线程，两边都会碰这些字段，
    /// 所以全部由 m_ 保护；cv_ 在状态变化时通知（数据到达、握手完成、对端关闭）。
    mutable std::mutex m_;
    std::condition_variable cv_;

    uint16_t sport_ = 0;
    uint16_t dport_ = 0;
    uint32_t snd_nxt_ = 0;
    uint32_t rcv_nxt_ = 0;
    bool established_ = false;
    bool peer_closed_ = false;
    bool sent_fin_ = false;

    std::vector<uint8_t> rx_;
    size_t rx_pos_ = 0;
    uint64_t dropped_segments_ = 0;
    uint64_t dropped_bytes_ = 0;
    /// 处理段时可能要从 on_segment（无返回值）里往外传错误：复用层只负责分发，
    /// 真正的失败要由正在等这条连接的人看到。
    std::string pending_err_;
};

}  // namespace scrctl::net
