#pragma once

#include <cstdint>
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

    /// 读满 n 字节，带总超时。
    bool recv_exact(void *dst, size_t n, int timeout_ms, std::string &err);

    void close();

    [[nodiscard]] bool connected() const { return established_; }

private:
    struct Segment {
        uint8_t flags = 0;
        uint32_t seq = 0;
        uint32_t ack = 0;
        std::vector<uint8_t> payload;
    };

    bool send_segment(uint8_t flags, const std::vector<uint8_t> &payload, std::string &err);
    /// 驱动复用层收一个包；返回 false 表示超时或隧道终止。
    bool pump_one(int timeout_ms, std::string &err);
    void on_segment(const uint8_t *l4, std::size_t len) override;
    bool handle_segment(const uint8_t *l4, std::size_t len, std::string &err);

    Stack &stack_;

    uint16_t sport_ = 0;
    uint16_t dport_ = 0;
    uint32_t snd_nxt_ = 0;
    uint32_t rcv_nxt_ = 0;
    bool established_ = false;
    bool peer_closed_ = false;
    bool sent_fin_ = false;

    std::vector<uint8_t> rx_;
    size_t rx_pos_ = 0;
    /// 处理段时可能要从 on_segment（无返回值）里往外传错误：复用层只负责分发，
    /// 真正的失败要由正在等这条连接的人看到。
    std::string pending_err_;
};

}  // namespace scrctl::net
