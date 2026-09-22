#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "transport/Tunnel.h"

namespace scrctl::net {

class TcpEndpoint;
class UdpEndpoint;

/// 一条包隧道之上的最小 IPv6 端点复用层。
///
/// 为什么非要有这一层：隧道是**点对点的一条字节流**，两个读者会互相偷包。而运行
/// 中同时需要「RSD 控制连接 + 显示服务连接 + HID 服务连接 + 设备反推过来的 RTP
/// UDP」，所以入站 IPv6 包必须有一个按协议和端口分发的唯一入口。
///
/// 不做的事：邻居发现、路由、分片重组、ICMPv6、多播。隧道的对端只有一个，
/// 地址是握手时协商好的两个，这些能力在这里没有用武之地。
class Stack {
public:
    Stack(transport::PacketTunnel &tunnel, std::string local_ip_text, std::string peer_ip_text);

    [[nodiscard]] bool addresses_ok() const { return addresses_valid_; }
    [[nodiscard]] const std::array<uint8_t, 16> &local_addr() const { return local_addr_; }
    [[nodiscard]] const std::array<uint8_t, 16> &peer_addr() const { return peer_addr_; }
    [[nodiscard]] const std::string &local_text() const { return local_text_; }
    [[nodiscard]] const std::string &peer_text() const { return peer_text_; }

    /// 发一个完整的 IPv6 包。**每包一次 write**：把多个包合并成一次写会破坏
    /// CoreDeviceProxy 转发路径的读边界，实测会把隧道打死。
    bool send(const std::vector<uint8_t> &ipv6_packet, std::string &err);

    /// 读一个入站包并按协议 + 端口分发。返回 false 表示超时或隧道已断。
    /// 不属于任何已登记端点的包被丢弃后仍算成功——分发本来就是尽力而为。
    bool pump(int timeout_ms, std::string &err);

    void attach_tcp(uint16_t local_port, TcpEndpoint *ep);
    void detach_tcp(uint16_t local_port);
    void attach_udp(uint16_t local_port, UdpEndpoint *ep);
    void detach_udp(uint16_t local_port);

    /// 组一个 IPv6 头（上层负责 L4 头与校验和）。
    std::vector<uint8_t> wrap(const std::vector<uint8_t> &l4, uint8_t next_header) const;

private:
    transport::PacketTunnel &tunnel_;
    std::string local_text_;
    std::string peer_text_;
    std::array<uint8_t, 16> local_addr_{};
    std::array<uint8_t, 16> peer_addr_{};
    bool addresses_valid_ = false;
    std::map<uint16_t, TcpEndpoint *> tcp_;
    std::map<uint16_t, UdpEndpoint *> udp_;
};

/// IPv6 下的 L4 校验和必须带伪头，且**不能省略**：TCP/UDP 在 IPv6 里写 0 表示
/// "没算"，对端会直接丢包。表现是"对方静默丢弃、连接永远握不上"，最难查。
/// `l4` 是从 L4 头开始、到载荷结束的完整字节（校验和字段本身须已置零）。
[[nodiscard]] uint16_t l4_checksum(const uint8_t src[16], const uint8_t dst[16],
                                   const uint8_t *l4, std::size_t len, uint8_t next_header);

/// 一个 TCP 端点（连接）在复用层里的登记身份。TcpStream 实现它。
class TcpEndpoint {
public:
    virtual ~TcpEndpoint() = default;
    /// 收到一个目的端口属于自己的 TCP 段。
    virtual void on_segment(const uint8_t *l4, std::size_t len) = 0;
};

/// 一个 UDP 端点。UdpSocket 实现它。
class UdpEndpoint {
public:
    virtual ~UdpEndpoint() = default;
    virtual void on_datagram(const uint8_t *l4, std::size_t len) = 0;
};

}  // namespace scrctl::net
