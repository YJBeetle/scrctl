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
/// 不做的事：邻居发现、路由、分片重组、多播。隧道的对端只有一个，地址是握手时
/// 协商好的两个，这些能力在这里没有用武之地。ICMPv6 原本也在这一列，现在为了
/// "客户端->设备的 UDP 到底通不通"这个问题开了一个最小的口子：能发回音请求、
/// 能把收到的 ICMP 记下来（见 `send_echo_request()` 的说明），仅此而已。
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

    /// 发一个 ICMPv6 回音请求（type=128）给隧道对端。
    ///
    /// 为什么要有这一位：判断"客户端->设备的 UDP 到底通不通"时，金丝雀
    /// （往一个确定没人监听的端口发包）的**没有回信**这一步是二义的——可能是
    /// "我们的包根本没到设备内核"，也可能只是"设备不在隧道里生成 ICMP"。
    /// 回音请求能把这两种分开：它是设备**内核**直接答的（不需要任何 App 配合），
    /// 而且答话走的是"我们发出去的同一个包通道"的反方向。
    ///
    /// 实测（两臂都跑过）：回音 **5/5 有应答**——所以隧道对非 TCP 流量是投递的，
    /// 我们手搓的 IPv6 头、伪头校验和、`wrap()` 全都没问题（设备内核要是不认，
    /// 连 ICMP 都不会回）。金丝雀那侧在修好 `build_udp_datagram` 之前**一条 ICMP
    /// 都没有**，修好后立刻 3/3 收到 `type=1 code=4`。当时按上面那条二分支读成
    /// "我们的 UDP 没进内核（隧道不投递）"，那是**错的**：真实成因是数据报自己坏了，
    /// 而内核丢一个校验和错的 UDP 包是静默的、不回 ICMP。
    bool send_echo_request(uint16_t ident, uint16_t seq, std::string &err);

    /// 给本栈**所有出站包**的 IPv6 头填一个 20 位流标签（默认 0 = 不发流标签）。
    ///
    /// 这是为"客户端->设备的 UDP 一个都没到"那一条准备的开关：苹果客户端发出的
    /// RTCP 包，其 IPv6 头的流标签是**非零**的（抓包实测 `0xd0d00`，而设备发给我们
    /// 的那些是 `0x20d00`——两边各有一个随流生成的标签，符合 RFC 6438 的"每流随机"），
    /// 而我们 `wrap()` 里写死 0。
    ///
    /// **已判掉**：`--flow-label` 打一随机非零标签后，设备侧依旧是 `pkts in: 0`、
    /// 照旧 20 秒死。留着这一位是因为它是真实差异（迟早要按 RFC 6438 补上），不是保活手段。
    void set_flow_label(uint32_t label) { flow_label_ = label & 0xFFFFF; }

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

    /// 隧道里收到过多少个 ICMPv6 包，以及最后一条的一行摘要（没有则空串）。
    ///
    /// 为什么要有这一位：20 秒断流的根因是设备侧 `lastReceivedPacketTime:nan`
    /// ——我们发往设备的 UDP 数据报**一个都没落到它的媒体 socket 上**。这件事当时有两种
    /// 看起来完全不同的成因，而它们在媒体面上长得一模一样：我们的包没被隧道投递出去，
    /// 或者到了但那个端口上没人收。区别只在**设备的内核会不会回一个 ICMPv6**：
    /// 打给一个确定没人监听的端口，内核该回 `type=1 code=4`（端口不可达）；打给活着的
    /// socket 则不该有回信。所以"能不能看见 ICMP"就是这两种成因的分界线，而在此之前
    /// 整个栈对 next_header=58 是**直接丢弃且不计数**的，等于把唯一的线索扔了。
    ///
    /// 实际收的线比这两种都更窄，而这一点当初没想到：**校验和错的 UDP 包在进 UDP 层
    /// 之前就被丢**，所以连"端口不可达"都不会有——金丝雀在修好数据报拼装之前三条全哑，
    /// 修好之后三条全部拿到 `type=1 code=4` 且回带的内层四元组就是我们的发包
    /// （`58250->47891`）。于是这一位真正的用法是：**有 code=4 = 包是好的、只是没人听；
    /// 什么都没有 = 包本身就是坏的**。
    [[nodiscard]] uint64_t icmp_seen() const { return icmp_seen_; }
    /// 其中是**回音应答**（type=129）的个数——这是"隧道投不投递非 TCP 流量"的正信号。
    [[nodiscard]] uint64_t echo_replies() const { return echo_replies_; }
    [[nodiscard]] std::string icmp_last() const;

private:
    void pump_loop();
    /// 读一个入站包并分发。只在泵线程里跑。
    bool pump_once(int timeout_ms, std::string &err);
    /// 记下一条 ICMPv6（含错误消息回带的内层四元组）。见 `icmp_seen()` 的说明。
    void observe_icmpv6(const uint8_t *icmp, size_t len);

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
    uint32_t flow_label_ = 0;
    uint64_t icmp_seen_ = 0;
    uint64_t echo_replies_ = 0;
    mutable std::mutex icmp_mu_;
    std::string icmp_last_;
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
