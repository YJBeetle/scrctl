#include "TcpStream.h"

#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <random>

namespace scrctl::net {
namespace {

constexpr size_t kIpv6HeaderLen = 40;
constexpr uint8_t kNextHeaderTcp = 6;
constexpr uint8_t kHopLimit = 64;

// TCP 标志位
constexpr uint8_t kFin = 0x01;
constexpr uint8_t kSyn = 0x02;
constexpr uint8_t kRst = 0x04;
constexpr uint8_t kPsh = 0x08;
constexpr uint8_t kAck = 0x10;

void put16(uint8_t *p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}

void put32(uint8_t *p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

uint16_t get16(const uint8_t *p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
uint32_t get32(const uint8_t *p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

/// 序号比较，按回绕处理。
bool seq_lt(uint32_t a, uint32_t b) { return int32_t(a - b) < 0; }
bool seq_ge(uint32_t a, uint32_t b) { return !seq_lt(a, b); }

uint32_t fold_sum(uint32_t sum, const uint8_t *p, size_t n) {
    for (size_t i = 0; i + 1 < n; i += 2) {
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

/// IPv6 下 TCP 校验和必须用伪头，算错的表现是对方静默丢弃、连接永远握不上。
uint16_t tcp_checksum(const uint8_t src[16], const uint8_t dst[16],
                      const std::vector<uint8_t> &seg) {
    uint32_t sum = 0;
    sum = fold_sum(sum, src, 16);
    sum = fold_sum(sum, dst, 16);
    const uint32_t len = static_cast<uint32_t>(seg.size());
    const uint8_t pseudo[12] = {
        static_cast<uint8_t>(len >> 24), static_cast<uint8_t>(len >> 16),
        static_cast<uint8_t>(len >> 8),  static_cast<uint8_t>(len),
        0, 0, 0, kNextHeaderTcp,
    };
    // 上层的 4 字节长度 + 3 字节零 + next header，与段一起连续求和才等价于
    // 标准伪头；这里把 pseudo 当作 12 字节参与同一轮累加。
    sum = fold_sum(sum, pseudo, sizeof(pseudo));
    sum = fold_sum(sum, seg.data(), seg.size());
    return finish_sum(sum);
}

std::vector<uint8_t> build_ipv6(const std::vector<uint8_t> &src, const std::vector<uint8_t> &dst,
                                const std::vector<uint8_t> &payload) {
    std::vector<uint8_t> out(kIpv6HeaderLen + payload.size());
    // version(6) | traffic class(0) | flow label(0)
    put32(out.data(), 6u << 28);
    put16(out.data() + 4, static_cast<uint16_t>(payload.size()));
    out[6] = kNextHeaderTcp;
    out[7] = kHopLimit;
    std::memcpy(out.data() + 8, src.data(), 16);
    std::memcpy(out.data() + 24, dst.data(), 16);
    std::memcpy(out.data() + kIpv6HeaderLen, payload.data(), payload.size());
    return out;
}

uint16_t random_port() {
    static std::mt19937 rng{std::random_device{}()};
    return static_cast<uint16_t>(49152 + rng() % 16383);
}

uint32_t random_seq() {
    static std::mt19937 rng{std::random_device{}()};
    return rng();
}

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

TcpStream::TcpStream(scrctl::transport::PacketTunnel &tunnel, std::string local_ip,
                     std::string peer_ip)
    : tunnel_(tunnel), local_ip_(std::move(local_ip)), peer_ip_(std::move(peer_ip)) {
    local_addr_.resize(16);
    peer_addr_.resize(16);
    if (inet_pton(AF_INET6, local_ip_.c_str(), local_addr_.data()) != 1) {
        local_addr_.clear();
    }
    if (inet_pton(AF_INET6, peer_ip_.c_str(), peer_addr_.data()) != 1) {
        peer_addr_.clear();
    }
}

bool TcpStream::send_segment(uint8_t flags, const std::vector<uint8_t> &payload,
                             std::string &err) {
    if (local_addr_.empty() || peer_addr_.empty()) {
        return err = "隧道地址不是合法 IPv6", false;
    }
    // 20 字节基本头 + 4 字节 MSS 选项（仅 SYN 带）。
    const bool with_options = (flags & kSyn) != 0 && (flags & kAck) == 0;
    const size_t hdr_len = with_options ? 24 : 20;

    std::vector<uint8_t> seg(hdr_len + payload.size());
    put16(seg.data(), sport_);
    put16(seg.data() + 2, dport_);
    put32(seg.data() + 4, snd_nxt_);
    put32(seg.data() + 8, rcv_nxt_);
    seg[12] = static_cast<uint8_t>((hdr_len / 4) << 4);
    seg[13] = flags;
    put16(seg.data() + 14, 65535);  // 接收窗口
    put16(seg.data() + 16, 0);      // 校验和，先置零
    put16(seg.data() + 18, 0);
    if (with_options) {
        seg[20] = 2;  // MSS
        seg[21] = 4;
        put16(seg.data() + 22, 15940);  // 16000 MTU - 40 IPv6 - 20 TCP
    }
    std::memcpy(seg.data() + hdr_len, payload.data(), payload.size());

    put16(seg.data() + 16, tcp_checksum(local_addr_.data(), peer_addr_.data(), seg));

    const auto packet = build_ipv6(local_addr_, peer_addr_, seg);
    // 每个 IPv6 包单独一次写：CoreDeviceProxy 的转发路径对读边界敏感。
    return tunnel_.send_ipv6(packet.data(), packet.size(), err);
}

bool TcpStream::parse_and_queue(const std::vector<uint8_t> &packet, std::string &err) {
    if (packet.size() < kIpv6HeaderLen) {
        return true;  // 太短，忽略
    }
    if ((packet[0] >> 4) != 6) {
        return true;
    }
    if (packet[6] != kNextHeaderTcp) {
        return true;  // 非 TCP（如 ICMPv6），本实现不处理
    }
    const size_t l4 = kIpv6HeaderLen;
    if (packet.size() < l4 + 20) {
        return true;
    }
    const uint16_t their_sport = get16(packet.data() + l4 + 0);
    const uint16_t their_dport = get16(packet.data() + l4 + 2);
    if (their_dport != sport_ || their_sport != dport_) {
        return true;  // 不属于本连接
    }
    const uint32_t seq = get32(packet.data() + l4 + 4);
    const uint32_t ack = get32(packet.data() + l4 + 8);
    const uint8_t data_off = static_cast<uint8_t>(packet[l4 + 12] >> 4);
    const uint8_t flags = packet[l4 + 13];
    const size_t tcp_hdr = static_cast<size_t>(data_off) * 4;
    if (tcp_hdr < 20 || packet.size() < l4 + tcp_hdr) {
        return true;
    }

    if (flags & kRst) {
        established_ = false;
        peer_closed_ = true;
        return false;
    }

    // SYN-ACK：SYN 自身消耗一个序号，所以 rcv_nxt 是对方 seq+1；
    // 对方的 ack 就是我们可以开始发送的序号。必须在下面的数据处理之前
    // 返回，否则会把握手的序号推进当成载荷。
    if ((flags & kSyn) != 0 && (flags & kAck) != 0) {
        if (established_) {
            return true;  // 重复的 SYN-ACK，忽略
        }
        rcv_nxt_ = seq + 1;
        snd_nxt_ = ack;
        established_ = true;
        std::vector<uint8_t> empty;
        return send_segment(kAck, empty, err);
    }

    const size_t payload_len = packet.size() - l4 - tcp_hdr;
    if (payload_len > 0) {
        // 只接受期望序号的数据；乱序暂不支持。
        if (seq == rcv_nxt_) {
            rx_.insert(rx_.end(), packet.begin() + static_cast<long>(l4 + tcp_hdr),
                       packet.end());
            rcv_nxt_ += static_cast<uint32_t>(payload_len);
            std::vector<uint8_t> empty;
            if (!send_segment(kAck | kPsh, empty, err)) {
                return false;
            }
        } else {
            // 重复/乱序：回一个 ACK 提示对方。
            std::vector<uint8_t> empty;
            send_segment(kAck, empty, err);
        }
    }

    if ((flags & kFin) != 0) {
        if (seq_ge(rcv_nxt_, seq) || payload_len == 0) {
            rcv_nxt_ = seq + static_cast<uint32_t>(payload_len) + 1;
        }
        peer_closed_ = true;
    }
    return true;
}

bool TcpStream::pump_one(int timeout_ms, std::string &err) {
    std::string wait_err;
    if (!tunnel_.wait_readable(timeout_ms, wait_err)) {
        err = wait_err;
        return false;
    }
    std::vector<uint8_t> packet;
    if (!tunnel_.recv_ipv6(packet, err)) {
        return false;
    }
    return parse_and_queue(packet, err);
}

bool TcpStream::connect(uint16_t peer_port, std::string &err) {
    dport_ = peer_port;
    sport_ = random_port();
    snd_nxt_ = random_seq();
    rcv_nxt_ = 0;

    const int64_t deadline = now_ms() + 15000;
    int64_t next_retry = now_ms() + 500;
    std::vector<uint8_t> no_payload;
    if (!send_segment(kSyn, no_payload, err)) {
        return false;
    }
    while (now_ms() < deadline) {
        const int slice = static_cast<int>(std::max<int64_t>(1, next_retry - now_ms()));
        std::string pump_err;
        if (pump_one(slice, pump_err)) {
            if (established_) {
                return true;
            }
        } else if (peer_closed_) {
            return err = "握手被 RST/关闭", false;
        }
        if (now_ms() >= next_retry) {
            next_retry = now_ms() + 500;
            if (!send_segment(kSyn, no_payload, err)) {
                return false;
            }
        }
    }
    return err = "TCP 握手超时（检查校验和与序号）", false;
}

bool TcpStream::send(std::string_view data, std::string &err) {
    if (!established_) {
        return err = "连接未建立", false;
    }
    size_t off = 0;
    constexpr size_t kMss = 15940;
    while (off < data.size()) {
        const size_t n = std::min(kMss, data.size() - off);
        std::vector<uint8_t> chunk(data.begin() + static_cast<long>(off),
                                   data.begin() + static_cast<long>(off + n));
        if (!send_segment(kAck | kPsh, chunk, err)) {
            return false;
        }
        snd_nxt_ += static_cast<uint32_t>(n);
        off += n;
    }
    return true;
}

bool TcpStream::recv(std::vector<uint8_t> &out, int timeout_ms, std::string &err) {
    out.clear();
    const int64_t deadline = now_ms() + timeout_ms;
    for (;;) {
        if (rx_pos_ < rx_.size()) {
            out.assign(rx_.begin() + static_cast<long>(rx_pos_), rx_.end());
            rx_pos_ = rx_.size();
            return true;
        }
        if (peer_closed_) {
            return err = "对端已关闭", false;
        }
        const int64_t left = deadline - now_ms();
        if (left <= 0) {
            return err = "读超时", false;
        }
        std::string pump_err;
        if (!pump_one(static_cast<int>(left), pump_err) && !pump_err.empty() &&
            pump_err.find("超时") == std::string::npos) {
            err = pump_err;
            return false;
        }
    }
}

bool TcpStream::recv_exact(void *dst, size_t n, int timeout_ms, std::string &err) {
    auto *p = static_cast<uint8_t *>(dst);
    size_t got = 0;
    const int64_t deadline = now_ms() + timeout_ms;
    std::vector<uint8_t> chunk;
    while (got < n) {
        const int64_t left = deadline - now_ms();
        if (left <= 0) {
            return err = "读不满：超时", false;
        }
        if (!recv(chunk, static_cast<int>(left), err)) {
            return false;
        }
        const size_t take = std::min(chunk.size(), n - got);
        std::memcpy(p + got, chunk.data(), take);
        got += take;
        if (take < chunk.size()) {
            // 多余部分留在 rx 缓冲里，下次继续取。
            break;
        }
    }
    return got == n;
}

void TcpStream::close() {
    if (!established_ || sent_fin_) {
        return;
    }
    sent_fin_ = true;
    std::vector<uint8_t> empty;
    std::string err;
    send_segment(kFin | kAck, empty, err);
    established_ = false;
}

}  // namespace scrctl::net
