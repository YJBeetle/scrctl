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
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(m_);
        auto reject = [&] {
            ++dropped_;
            return;
        };
        if (len < kUdpHeaderLen) {
            reject();
            return;
        }
        const std::size_t declared = get16(l4 + 4);
        if (declared < kUdpHeaderLen || declared > len) {
            reject();
            return;
        }
        const uint16_t wire_sum = get16(l4 + 6);
        if (wire_sum == 0) {
            reject();  // IPv6 下 0 表示未计算，按规范丢弃
            return;
        }
        std::vector<uint8_t> copy(l4, l4 + declared);
        const uint16_t check = l4_checksum(stack_.peer_addr().data(), stack_.local_addr().data(),
                                           copy.data(), copy.size(), kNextHeaderUdp);
        if (check != 0) {
            reject();
            return;
        }
        if (queue_.size() >= kMaxQueue) {
            // 丢最老的、留下这个新的：消费者要的是"现在"而不是三秒前。
            // 这里不能 pop 完就 return——那等于一次丢两个包（最老的和新到的都没
            // 进队列），而 RTP 侧的序号缺口又会被上层当成网络丢包去重起会话。
            queue_.pop_front();
            ++dropped_;
        }
        queue_.emplace_back(Packet{get16(l4 + 0),
                                   std::vector<uint8_t>(l4 + kUdpHeaderLen, l4 + declared)});
        queued = true;
    }
    if (queued) {
        cv_.notify_all();
    }
}

bool UdpSocket::recv(std::vector<uint8_t> &payload, uint16_t &peer_port, int timeout_ms,
                     std::string &err) {
    std::unique_lock<std::mutex> lock(m_);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        if (!queue_.empty()) {
            peer_port = queue_.front().peer_port;
            payload = std::move(queue_.front().payload);
            queue_.pop_front();
            return true;
        }
        // 泵线程退出后再等也不会有包进来，立刻把原因交出去。
        const std::string why = stack_.pump_error();
        if (!why.empty()) {
            err = why;
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            err = "收数据报超时";
            return false;
        }
        cv_.wait_for(lock, std::chrono::milliseconds(50));
    }
}

std::size_t UdpSocket::buffered() const {
    std::lock_guard<std::mutex> lock(m_);
    return queue_.size();
}

std::size_t UdpSocket::dropped() const {
    std::lock_guard<std::mutex> lock(m_);
    return dropped_;
}

}  // namespace scrctl::net
