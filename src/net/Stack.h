#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
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
/// 这个唯一入口就是 `start_pump()` 起的那一个线程。它读隧道、分发，端点只从自己
/// 的队列取数据并等待——端点不再自己驱动复用层。之前是端点各自 pump，实测后果是
/// 「镜像在跑时打开 HID 服务」直接失败：收流线程与握手线程同时在同一条隧道上读，
/// 一方把另一方的 TCP 字节当成自己的包读走了。
///
/// 不做的事：邻居发现、路由、分片重组、ICMPv6、多播。隧道的对端只有一个，
/// 地址是握手时协商好的两个，这些能力在这里没有用武之地。
class Stack {
public:
    Stack(transport::PacketTunnel &tunnel, std::string local_ip_text, std::string peer_ip_text);
    /// 停泵线程并等它退出。端点必须在复用层之前析构（Device 的成员顺序保证了这点）。
    ~Stack();

    [[nodiscard]] bool addresses_ok() const { return addresses_valid_; }
    [[nodiscard]] const std::array<uint8_t, 16> &local_addr() const { return local_addr_; }
    [[nodiscard]] const std::array<uint8_t, 16> &peer_addr() const { return peer_addr_; }
    [[nodiscard]] const std::string &local_text() const { return local_text_; }
    [[nodiscard]] const std::string &peer_text() const { return peer_text_; }

    /// 起泵线程。整个栈的生命周期里只该调一次，且要在任何端点登记之前。
    bool start_pump(std::string &err);
    void stop_pump();
    [[nodiscard]] bool pumping() const { return pumping_; }

    /// 泵线程退出的原因（通常是隧道断了）。端点等不到数据时把它转给调用方，
    /// 否则"隧道已经死了"在调用方看来和"再等等就好"没有区别。
    [[nodiscard]] std::string pump_error() const;

    /// 发一个完整的 IPv6 包。**每包一次 write**：把多个包合并成一次写会破坏
    /// CoreDeviceProxy 转发路径的读边界，实测会把隧道打死。
    /// 写要串行：泵线程替连接发 ACK 的同时，应用线程可能在发请求。
    bool send(const std::vector<uint8_t> &ipv6_packet, std::string &err);

    void attach_tcp(uint16_t local_port, TcpEndpoint *ep);
    void detach_tcp(uint16_t local_port);
    void attach_udp(uint16_t local_port, UdpEndpoint *ep);
    void detach_udp(uint16_t local_port);

    /// 组一个 IPv6 头（上层负责 L4 头与校验和）。
    std::vector<uint8_t> wrap(const std::vector<uint8_t> &l4, uint8_t next_header) const;

    /// 收到的 L4 段里校验和不对的个数。**这是"字节被改了"而不是"字节丢了"的判据**：
    /// 不校验的话损坏的 TCP 载荷会被当成正常数据交给上层，症状是上层在莫名其妙的
    /// 位置报"帧长过大"，看起来像它自己的 bug。
    [[nodiscard]] uint64_t bad_checksums() const { return bad_checksums_; }

private:
    void pump_loop();
    /// 读一个入站包并分发。只在泵线程里跑。
    bool pump_once(int timeout_ms, std::string &err);

    transport::PacketTunnel &tunnel_;
    std::string local_text_;
    std::string peer_text_;
    std::array<uint8_t, 16> local_addr_{};
    std::array<uint8_t, 16> peer_addr_{};
    bool addresses_valid_ = false;

    /// 分发表：泵线程读，端点构造/析构时写。
    mutable std::mutex ep_mu_;
    std::map<uint16_t, TcpEndpoint *> tcp_;
    std::map<uint16_t, UdpEndpoint *> udp_;

    uint64_t bad_checksums_ = 0;
    std::mutex write_mu_;
    std::thread pump_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> pumping_{false};
    mutable std::mutex err_mu_;
    std::string pump_err_;
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
