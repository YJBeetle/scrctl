#include "net/UdpSocket.h"

#include <chrono>

namespace scrctl::net {
namespace {

constexpr std::size_t kUdpHeaderLen = 8;
constexpr uint8_t kNextHeaderUdp = 17;

uint16_t get16(const uint8_t *p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
void put16(uint8_t *p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

UdpSocket::~UdpSocket() {
    if (bound_) {
        stack_.detach_udp(local_port_);
    }
}

bool UdpSocket::bind(std::string &err) {
    if (local_port_ == 0) {
        err = "未指定本地端口";
        return false;
    }
    if (!stack_.addresses_ok()) {
        err = "隧道地址不是合法 IPv6";
        return false;
    }
    stack_.attach_udp(local_port_, this);
    bound_ = true;
    return true;
}

bool UdpSocket::send(const std::vector<uint8_t> &payload, uint16_t peer_port, std::string &err) {
    if (!bound_) {
        err = "套接字没绑定就发";
        return false;
    }
    std::vector<uint8_t> dgram(kUdpHeaderLen + payload.size());
    put16(dgram.data(), local_port_);
    put16(dgram.data() + 2, peer_port);
    put16(dgram.data() + 4, static_cast<uint16_t>(dgram.size()));
    put16(dgram.data() + 6, 0);  // 校验和先置零，算完回填
    dgram.insert(dgram.end(), payload.begin(), payload.end());
    const uint16_t sum =
        l4_checksum(stack_.local_addr().data(), stack_.peer_addr().data(), dgram.data(),
                    dgram.size(), kNextHeaderUdp);
    // IPv6 里 UDP 校验和为 0 的含义是"没算"，对端会直接丢包，所以算出 0 也要
    // 按规范改写成 0xFFFF（它等价于全一的补码）。
    put16(dgram.data() + 6, sum == 0 ? 0xFFFF : sum);
    return stack_.send(stack_.wrap(dgram, kNextHeaderUdp), err);
}

void UdpSocket::on_datagram(const uint8_t *l4, std::size_t len) {
    if (len < kUdpHeaderLen) {
        ++dropped_;
        return;
    }
    const std::size_t declared = get16(l4 + 4);
    if (declared < kUdpHeaderLen || declared > len) {
        ++dropped_;
        return;
    }
    const uint16_t wire_sum = get16(l4 + 6);
    if (wire_sum == 0) {
        ++dropped_;  // IPv6 下 0 表示未计算，按规范丢弃
        return;
    }
    std::vector<uint8_t> copy(l4, l4 + declared);
    const uint16_t check = l4_checksum(stack_.peer_addr().data(), stack_.local_addr().data(),
                                       copy.data(), copy.size(), kNextHeaderUdp);
    if (check != 0) {
        ++dropped_;
        return;
    }
    queue_.push_back(Packet{get16(l4 + 0),
                            std::vector<uint8_t>(l4 + kUdpHeaderLen, l4 + declared)});
}

bool UdpSocket::recv(std::vector<uint8_t> &payload, uint16_t &peer_port, int timeout_ms,
                     std::string &err) {
    const auto deadline = now_ms() + timeout_ms;
    for (;;) {
        if (!queue_.empty()) {
            peer_port = queue_.front().peer_port;
            payload = std::move(queue_.front().payload);
            queue_.pop_front();
            return true;
        }
        const auto left = deadline - now_ms();
        if (left <= 0) {
            err = "收数据报超时";
            return false;
        }
        if (!stack_.pump(static_cast<int>(left), err)) {
            if (!queue_.empty()) {
                continue;  // 断开前落地的最后一个数据报还是要交出去
            }
            return false;
        }
    }
}

}  // namespace scrctl::net
