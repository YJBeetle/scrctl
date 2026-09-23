#include "net/TcpStream.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>

namespace scrctl::net {
namespace {

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

TcpStream::TcpStream(Stack &stack) : stack_(stack) {}

TcpStream::~TcpStream() {
    if (sport_ != 0) {
        stack_.detach_tcp(sport_);
    }
}

bool TcpStream::send_segment(uint8_t flags, const std::vector<uint8_t> &payload,
                             std::string &err) {
    if (!stack_.addresses_ok()) {
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

    put16(seg.data() + 16, l4_checksum(stack_.local_addr().data(), stack_.peer_addr().data(),
                                       seg.data(), seg.size(), 6));
    return stack_.send(stack_.wrap(seg, 6), err);
}

void TcpStream::on_segment(const uint8_t *l4, std::size_t len) {
    std::string err;
    {
        std::lock_guard<std::mutex> lock(m_);
        if (!handle_segment(l4, len, err)) {
            pending_err_ = err;
        }
    }
    cv_.notify_all();
}

bool TcpStream::handle_segment(const uint8_t *l4, std::size_t len, std::string &err) {
    if (len < 20) {
        return true;  // 太短，忽略
    }
    const uint16_t their_sport = get16(l4 + 0);
    const uint16_t their_dport = get16(l4 + 2);
    if (their_dport != sport_ || their_sport != dport_) {
        return true;  // 不属于本连接
    }
    const uint32_t seq = get32(l4 + 4);
    const uint32_t ack = get32(l4 + 8);
    const uint8_t data_off = static_cast<uint8_t>(l4[12] >> 4);
    const uint8_t flags = l4[13];
    const size_t tcp_hdr = static_cast<size_t>(data_off) * 4;
    if (tcp_hdr < 20 || tcp_hdr > len) {
        return true;
    }

    if ((flags & kRst) != 0) {
        peer_closed_ = true;
        return true;
    }

    // ACK 位没置的段一律不认（RFC 793），SYN 单独处理在下面的分支里。
    if ((flags & kAck) == 0 && (flags & kSyn) == 0) {
        return true;
    }

    if ((flags & kSyn) != 0 && (flags & kAck) != 0) {
        if (established_) {
            return true;  // 重复的 SYN-ACK，忽略
        }
        // SYN 自己占一个序号，漏掉这一步会让后续字节整体偏一位。
        rcv_nxt_ = seq + 1;
        snd_nxt_ = ack;
        established_ = true;
        std::vector<uint8_t> empty;
        return send_segment(kAck, empty, err);
    }

    const size_t payload_len = len - tcp_hdr;
    if (payload_len > 0) {
        // 只接受期望序号的数据；乱序暂不支持。
        if (seq == rcv_nxt_) {
            rx_.insert(rx_.end(), l4 + tcp_hdr, l4 + len);
            rcv_nxt_ += static_cast<uint32_t>(payload_len);
            std::vector<uint8_t> empty;
            if (!send_segment(kAck | kPsh, empty, err)) {
                return false;
            }
        } else {
            // 序号落在期望之外：不缓存、不重排，丢掉并回一个期望序号的 ACK
            // （等价于重复 ACK，催对端重传）。计数是为了让"HTTP/2 帧长过大"
            // 这种字节流缺段的症状能被立刻归因到这里，而不是留给人猜。
            ++dropped_segments_;
            dropped_bytes_ += payload_len;
            std::fprintf(stderr, "    !! TCP 段序号不连续：期望 %u 收到 %u 长度 %zu，已丢弃\n",
                         rcv_nxt_, seq, payload_len);
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

template <typename Pred>
bool TcpStream::wait_for(Pred ready, int timeout_ms, std::string &err) {
    std::unique_lock<std::mutex> lock(m_);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        if (ready()) {
            return true;
        }
        if (!pending_err_.empty()) {
            err = pending_err_;
            pending_err_.clear();
            return false;
        }
        // 每轮都看一眼隧道状态：泵线程退出后再不会有段进来了，不查就只剩干等
        // 满超时，而"隧道断了"和"这一时半会儿没数据"对调用方是两回事。
        const std::string why = stack_.pump_error();
        if (!why.empty()) {
            err = why;
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        cv_.wait_for(lock, std::chrono::milliseconds(50));
    }
}

bool TcpStream::connect(uint16_t peer_port, std::string &err) {
    // 先把寻址状态摆好再登记。登记那一刻起泵线程就可能往这儿派段，所以这些
    // 字段的初始化必须在登记之前、且不需要持锁（还没有别人知道这个端点）。
    dport_ = peer_port;
    sport_ = random_port();
    snd_nxt_ = random_seq();
    rcv_nxt_ = 0;
    established_ = false;
    peer_closed_ = false;
    rx_.clear();
    rx_pos_ = 0;
    pending_err_.clear();
    stack_.attach_tcp(sport_, this);

    const int64_t deadline = now_ms() + 15000;
    int64_t next_retry = now_ms() + 500;
    std::vector<uint8_t> no_payload;
    {
        std::lock_guard<std::mutex> lock(m_);
        if (!send_segment(kSyn, no_payload, err)) {
            return false;
        }
    }
    while (now_ms() < deadline) {
        const int slice = static_cast<int>(std::max<int64_t>(1, next_retry - now_ms()));
        if (wait_for([this] { return established_ || peer_closed_; }, slice, err)) {
            if (established_) {
                return true;
            }
            return err = "握手被 RST/关闭", false;
        }
        if (now_ms() >= next_retry) {
            next_retry = now_ms() + 500;
            std::lock_guard<std::mutex> lock(m_);
            if (!send_segment(kSyn, no_payload, err)) {
                return false;
            }
        }
    }
    return err = "TCP 握手超时（检查校验和与序号）", false;
}

bool TcpStream::send(std::string_view data, std::string &err) {
    std::lock_guard<std::mutex> lock(m_);
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

bool TcpStream::recv(std::vector<uint8_t> &out, int timeout_ms, std::string &err,
                     bool *timed_out) {
    if (timed_out != nullptr) {
        *timed_out = false;
    }
    out.clear();
    if (!wait_for([this] { return rx_pos_ < rx_.size() || peer_closed_; }, timeout_ms, err)) {
        if (err.empty()) {
            err = "读超时";
            if (timed_out != nullptr) {
                *timed_out = true;
                err.clear();  // 超时不是错误：调用方按自己的总 deadline 决定还要不要等
            }
        }
        return false;
    }
    if (rx_pos_ >= rx_.size()) {
        return err = peer_closed_ ? "对端已关闭" : "读超时", false;
    }
    out.assign(rx_.begin() + static_cast<long>(rx_pos_), rx_.end());
    rx_pos_ = rx_.size();
    return true;
}

void TcpStream::close() {
    std::lock_guard<std::mutex> lock(m_);
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
