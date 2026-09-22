#include "net/Stack.h"

#include <arpa/inet.h>
#include <cstring>

namespace scrctl::net {
namespace {

constexpr std::size_t kIpv6HeaderLen = 40;

uint16_t get16(const uint8_t *p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
uint32_t fold_sum(uint32_t sum, const uint8_t *p, std::size_t n) {
    for (std::size_t i = 0; i + 1 < n; i += 2) {
        sum += get16(p + i);
    }
    if (n & 1) {
        sum += static_cast<uint32_t>(p[n - 1]) << 8;
    }
    return sum;
}

uint16_t finish_sum(uint32_t sum) {
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return static_cast<uint16_t>(~sum & 0xFFFF);
}

}  // namespace

uint16_t l4_checksum(const uint8_t src[16], const uint8_t dst[16], const uint8_t *l4,
                     std::size_t len, uint8_t next_header) {
    uint32_t sum = 0;
    sum = fold_sum(sum, src, 16);
    sum = fold_sum(sum, dst, 16);
    // 伪头：上层长度(4) + 3 字节零 + next header。与 L4 一起连续累加，才等价于
    // 分两段各算一半再相加。
    const uint8_t pseudo[8] = {
        static_cast<uint8_t>(len >> 24), static_cast<uint8_t>(len >> 16),
        static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len),
        0, 0, 0, next_header,
    };
    sum = fold_sum(sum, pseudo, sizeof(pseudo));
    sum = fold_sum(sum, l4, len);
    return finish_sum(sum);
}

Stack::Stack(transport::PacketTunnel &tunnel, std::string local_ip_text, std::string peer_ip_text)
    : tunnel_(tunnel), local_text_(std::move(local_ip_text)), peer_text_(std::move(peer_ip_text)) {
    addresses_valid_ = inet_pton(AF_INET6, local_text_.c_str(), local_addr_.data()) == 1 &&
                       inet_pton(AF_INET6, peer_text_.c_str(), peer_addr_.data()) == 1;
}

bool Stack::send(const std::vector<uint8_t> &ipv6_packet, std::string &err) {
    // 每个 IPv6 包单独一次写。合并写会破坏 CoreDeviceProxy 转发路径的读边界，
    // 实测足以把整条隧道打死。
    return tunnel_.send_ipv6(ipv6_packet.data(), ipv6_packet.size(), err);
}

std::vector<uint8_t> Stack::wrap(const std::vector<uint8_t> &l4, uint8_t next_header) const {
    std::vector<uint8_t> out(kIpv6HeaderLen + l4.size());
    uint8_t *p = out.data();
    p[0] = 0x60;  // version 6，TC 与流标签全零
    const uint16_t payload = static_cast<uint16_t>(l4.size());
    p[4] = static_cast<uint8_t>(payload >> 8);
    p[5] = static_cast<uint8_t>(payload);
    p[6] = next_header;
    p[7] = 64;  // hop limit
    std::memcpy(p + 8, local_addr_.data(), 16);
    std::memcpy(p + 24, peer_addr_.data(), 16);
    std::memcpy(p + kIpv6HeaderLen, l4.data(), l4.size());
    return out;
}

void Stack::attach_tcp(uint16_t local_port, TcpEndpoint *ep) { tcp_[local_port] = ep; }
void Stack::detach_tcp(uint16_t local_port) { tcp_.erase(local_port); }
void Stack::attach_udp(uint16_t local_port, UdpEndpoint *ep) { udp_[local_port] = ep; }
void Stack::detach_udp(uint16_t local_port) { udp_.erase(local_port); }

bool Stack::pump(int timeout_ms, std::string &err) {
    std::string wait_err;
    if (!tunnel_.wait_readable(timeout_ms, wait_err)) {
        err = wait_err.empty() ? "等入站包超时" : wait_err;
        return false;
    }
    std::vector<uint8_t> packet;
    if (!tunnel_.recv_ipv6(packet, err)) {
        return false;
    }
    if (packet.size() < kIpv6HeaderLen + 4 || (packet[0] >> 4) != 6) {
        return true;  // 不是 IPv6 或太短，丢掉继续
    }
    const uint8_t next = packet[6];
    const std::size_t l4 = kIpv6HeaderLen;
    // 只认发给本机地址的包。隧道是对端唯一的，但源地址写错通常意味着我们
    // 把上一个包的边界读错了——那种情况下静默丢弃比交给错误的端点强。
    if (std::memcmp(packet.data() + 24, local_addr_.data(), 16) != 0) {
        return true;
    }
    if (next == 6) {
        const uint16_t dport = get16(packet.data() + l4 + 2);
        auto it = tcp_.find(dport);
        if (it != tcp_.end()) {
            it->second->on_segment(packet.data() + l4, packet.size() - l4);
        }
        return true;
    }
    if (next == 17) {
        const uint16_t dport = get16(packet.data() + l4 + 2);
        auto it = udp_.find(dport);
        if (it != udp_.end()) {
            it->second->on_datagram(packet.data() + l4, packet.size() - l4);
        }
        return true;
    }
    return true;  // ICMPv6 等：本实现不处理
}

}  // namespace scrctl::net
