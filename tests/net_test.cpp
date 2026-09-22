// IPv6 上层校验和自检。
//
// 这块值得单独钉：IPv6 下 TCP/UDP 的校验和要带伪头，算错的表现是**对方静默
// 丢弃**——不报错、不回 RST，只是永远握不上手。没有对照向量的话，这种错只能
// 靠接上真机去猜，而真机上的排查成本是每次几分钟。
#include <arpa/inet.h>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include "net/Stack.h"

namespace {

int Failures = 0;

void check(bool ok, const std::string &what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) {
        ++Failures;
    }
}

std::vector<uint8_t> addr(const char *text) {
    std::vector<uint8_t> a(16);
    inet_pton(AF_INET6, text, a.data());
    return a;
}

void put16(std::vector<uint8_t> &v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}

void put32(std::vector<uint8_t> &v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x >> 24));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}

/// 一个 UDP 段（含 8 字节头，校验和字段置零），返回算出的校验和。
uint16_t udp_sum(const std::vector<uint8_t> &src, const std::vector<uint8_t> &dst,
                 uint16_t sport, uint16_t dport, const std::vector<uint8_t> &payload) {
    std::vector<uint8_t> dgram;
    put16(dgram, sport);
    put16(dgram, dport);
    put16(dgram, static_cast<uint16_t>(8 + payload.size()));
    put16(dgram, 0);
    dgram.insert(dgram.end(), payload.begin(), payload.end());
    return scrctl::net::l4_checksum(src.data(), dst.data(), dgram.data(), dgram.size(), 17);
}

std::vector<uint8_t> tcp_seg(uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack,
                             uint8_t flags, const std::vector<uint8_t> &options) {
    std::vector<uint8_t> seg;
    put16(seg, sport);
    put16(seg, dport);
    put32(seg, seq);
    put32(seg, ack);
    seg.push_back(static_cast<uint8_t>((5 + options.size() / 4) << 4));
    seg.push_back(flags);
    put16(seg, 65535);
    put16(seg, 0);  // 校验和位置零
    put16(seg, 0);
    seg.insert(seg.end(), options.begin(), options.end());
    return seg;
}

uint16_t tcp_sum(const std::vector<uint8_t> &src, const std::vector<uint8_t> &dst,
                 const std::vector<uint8_t> &seg) {
    return scrctl::net::l4_checksum(src.data(), dst.data(), seg.data(), seg.size(), 6);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto src = addr("fd12::2");
    const auto dst = addr("fd12::1");

    std::printf("== 对照向量（另一套独立实现按 RFC 1071/2460 算出）==\n");
    check(udp_sum(src, dst, 5000, 4000, {}) == 0xE28D, "UDP 空载荷");
    check(udp_sum(src, dst, 5000, 4000, {'h', 'i'}) == 0x7A20, "UDP 2 字节载荷");
    // 59 字节：奇数长度，走"末字节补零"那条分支
    std::vector<uint8_t> odd;
    for (uint8_t i = 1; i <= 59; ++i) {
        odd.push_back(i);
    }
    check(udp_sum(src, dst, 49152, 5004, odd) == 0xAA49, "UDP 奇数长度载荷");

    const auto bare = tcp_seg(49152, 5004, 7, 0, 0x12, {});
    check(bare.size() == 20, "无选项的 TCP 头 20 字节");
    check(tcp_sum(src, dst, bare) == 0xE216, "TCP SYN");
    const auto with_mss = tcp_seg(49152, 5004, 7, 0, 0x12, {0x02, 0x04, 0x3E, 0x94});
    check(with_mss.size() == 24, "带 MSS 选项的 TCP 头 24 字节");
    check(tcp_sum(src, dst, with_mss) == 0x917A, "TCP SYN + MSS 选项");

    std::printf("\n== 校验和的自反性 ==\n");
    // 把算出来的和填回字段再整段重算，结果必须是 0——这是接收端的判据。
    {
        std::vector<uint8_t> dgram;
        const uint16_t s = udp_sum(src, dst, 5000, 4000, odd);
        put16(dgram, 5000);
        put16(dgram, 4000);
        put16(dgram, static_cast<uint16_t>(8 + odd.size()));
        put16(dgram, s);
        dgram.insert(dgram.end(), odd.begin(), odd.end());
        check(scrctl::net::l4_checksum(dst.data(), src.data(), dgram.data(), dgram.size(), 17) ==
                  0,
              "接收端重算得 0");
        dgram[9] ^= 0x01;  // 改一个载荷字节
        check(scrctl::net::l4_checksum(dst.data(), src.data(), dgram.data(), dgram.size(), 17) !=
                  0,
              "载荷被改后重算不为 0");
    }

    std::printf("\n%s (失败 %d 项)\n", Failures == 0 ? "全部通过" : "存在失败", Failures);
    return Failures == 0 ? 0 : 1;
}
