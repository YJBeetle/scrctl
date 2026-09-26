#include "rt/Rtcp.h"

namespace scrctl::rt {
namespace {

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

/// SDES 的公共部分：头 + SSRC + 那个 CNAME 块 + END，长度字段按实际字数写。
std::vector<uint8_t> sdes_with_cname(uint32_t sender_ssrc, std::string_view cname) {
    std::vector<uint8_t> body;
    put32(body, sender_ssrc);
    body.push_back(1);  // CNAME
    body.push_back(static_cast<uint8_t>(cname.size()));
    body.insert(body.end(), cname.begin(), cname.end());
    body.push_back(0);  // END
    while (body.size() % 4 != 0) {
        body.push_back(0);
    }
    std::vector<uint8_t> v;
    v.push_back(0x81);  // V=2, SC=1
    v.push_back(202);
    put16(v, static_cast<uint16_t>(body.size() / 4));
    v.insert(v.end(), body.begin(), body.end());
    return v;
}

}  // namespace

std::vector<uint8_t> build_rr(uint32_t sender_ssrc, uint32_t media_ssrc, uint32_t ext_high) {
    std::vector<uint8_t> v;
    v.push_back(0x81);  // V=2, RC=1
    v.push_back(201);   // PT = RR
    put16(v, 7);        // 长度以 4 字节为单位，不含第一个字
    put32(v, sender_ssrc);
    put32(v, media_ssrc);
    put32(v, 0);          // 分数丢包 1 + 累积丢包 3：这条流我们不重传，报 0
    put32(v, ext_high);   // 扩展最高序号
    put32(v, 0);          // 抖动
    put32(v, 0);          // LSR / DLSR：老实报 0，不假装算过
    put32(v, 0);
    return v;
}

std::vector<uint8_t> build_sr(uint32_t sender_ssrc, uint32_t packets, uint32_t octets) {
    std::vector<uint8_t> v;
    v.push_back(0x80);  // V=2, RC=0
    v.push_back(200);   // PT = SR
    put16(v, 6);        // 头之后还有 6 个字
    put32(v, sender_ssrc);
    put32(v, 0);        // NTP 时间戳高位：不假装算过
    put32(v, 0);        // NTP 低位
    put32(v, 0);        // RTP 时间戳
    put32(v, packets);
    put32(v, octets);
    return v;
}

std::vector<uint8_t> build_sdes(uint32_t sender_ssrc) { return sdes_with_cname(sender_ssrc, {}); }

std::vector<uint8_t> build_sdes_cname(uint32_t sender_ssrc, std::string_view cname) {
    return sdes_with_cname(sender_ssrc, cname);
}

bool is_rtcp_sr(std::span<const uint8_t> datagram) {
    return datagram.size() >= 28 && datagram[0] == 0x81 && datagram[1] == 0xc8;
}

}  // namespace scrctl::rt
