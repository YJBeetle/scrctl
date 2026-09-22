#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "transport/Tunnel.h"

namespace scrctl::net {

/// 隧道之上的最小 IPv6 + TCP 客户端。
///
/// 为什么不用 lwIP：本项目的需求窄到点对点、静态地址、无 ND/ARP、单连接、
/// 纯客户端，而 lwIP 要么 FetchContent 走 https（受限）、要么 vendor 数百
/// 文件难核验来源。手写这块的失败模式也更好判定——一旦走到 RSD 握手，
/// 校验和或序号有任何错就只会挂起，成功则返回 85 个服务的表，是硬证据。
///
/// 不支持：分段重组、窗口缩放、选择性确认、并发连接、服务端监听。
class TcpStream {
public:
    TcpStream(scrctl::transport::PacketTunnel &tunnel, std::string local_ip, std::string peer_ip);

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
    /// 收并处理一个包；返回 false 表示超时或连接终止。
    bool pump_one(int timeout_ms, std::string &err);
    bool parse_and_queue(const std::vector<uint8_t> &packet, std::string &err);

    scrctl::transport::PacketTunnel &tunnel_;
    std::string local_ip_;
    std::string peer_ip_;
    std::vector<uint8_t> local_addr_;  // 16 字节
    std::vector<uint8_t> peer_addr_;   // 16 字节

    uint16_t sport_ = 0;
    uint16_t dport_ = 0;
    uint32_t snd_nxt_ = 0;
    uint32_t rcv_nxt_ = 0;
    bool established_ = false;
    bool peer_closed_ = false;
    bool sent_fin_ = false;

    std::vector<uint8_t> rx_;
    size_t rx_pos_ = 0;
};

}  // namespace scrctl::net
