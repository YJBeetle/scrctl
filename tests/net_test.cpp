// IPv6 上层校验和自检。
//
// 这块值得单独钉：IPv6 下 TCP/UDP 的校验和要带伪头，算错的表现是**对方静默
// 丢弃**——不报错、不回 RST，只是永远握不上手。没有对照向量的话，这种错只能
// 靠接上真机去猜，而真机上的排查成本是每次几分钟。
#include <arpa/inet.h>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include "net/Stack.h"
#include "net/UdpSocket.h"

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

    std::printf("\n== 金标准：苹果真的被设备收进的那个 RTCP 包 ==\n");
    // 来源：Xcode DeviceHub 镜像时的抓包（utun 已被 remoted 解封装，内层明文），
    // 客户端 fda4:4c2:5901::2:49637 -> 设备 fda4:4c2:5901::1:56179 的一个 32 字节 RCTL。
    //
    // 为什么要钉这一条：设备的内核计数器显示我们发往它媒体 socket 的 UDP
    // **一个都没进去**（`udp_connection_summary ... pkts in: 0`，见 docs §13），
    // 于是"是不是我们的 IPv6/UDP 头或校验和算错、被内核静默丢掉"成了头号嫌疑。
    // 这个怀疑不能靠读代码排除——校验和算错的表现恰恰就是"对方静默丢弃"。
    // 拿一个**同类流量、同一条隧道、设备确实收进了**的真实包当对照向量。
    //
    // **但这一条只测到了 `l4_checksum` 这个零件，没测到真正上线的字节**：它用的是
    // 本文件里自己写的 `udp_sum()` 助手（拼装是对的），而产品代码走的是
    // `UdpSocket::send()`——根因恰恰在后者。所以"结果：一致"当时被误读成
    // "我们这侧的封装划掉嫌疑了"，那是过头的结论。教训记在这里：对照向量要对的是
    // **即将发出去的那一串字节**，不是参与计算的某个函数。见下面那条回归测。
    {
        const auto a_src = addr("fda4:4c2:5901::2");
        const auto a_dst = addr("fda4:4c2:5901::1");
        const std::vector<uint8_t> a_payload = {
            0x80, 0xcc, 0x00, 0x07, 0x00, 0x65, 0xbb, 0xd8, 0x52, 0x43, 0x54, 0x4c,
            0x85, 0x00, 0x00, 0x04, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06,
            0x90, 0x64, 0x00, 0x00, 0x00, 0x48, 0x00, 0x00};
        const uint16_t ours = udp_sum(a_src, a_dst, 49637, 56179, a_payload);
        check(ours == 0xB212,
              "我们对苹果那个包算出的校验和 == 抓包里的 0xb212（" + std::to_string(ours) + "）");
        // 回验方向也要过：把抓到的校验和塞回头字段整段重算得 0，
        // 说明我们的算法与苹果的收发两端是同一套。
        std::vector<uint8_t> wire;
        put16(wire, 49637);
        put16(wire, 56179);
        put16(wire, static_cast<uint16_t>(8 + a_payload.size()));
        put16(wire, 0xB212);
        wire.insert(wire.end(), a_payload.begin(), a_payload.end());
        check(scrctl::net::l4_checksum(a_src.data(), a_dst.data(), wire.data(), wire.size(), 17) ==
                  0,
              "用苹果抓包里的校验和回验得 0");
        // 同一个向量现在改走产品代码的拼装：结果必须一模一样。
        // （曾经的版本在这里会给出完全不同的字节，而这条测当时不存在。）
        const auto via_product = scrctl::net::build_udp_datagram(49637, 56179, a_src.data(),
                                                                 a_dst.data(), a_payload);
        check(via_product == wire, "产品代码拼出的苹果那个包 == 抓包字节（逐字节相等）");
    }

    std::printf("\n== 回归：我们自己发出去的 UDP 字节 ==\n");
    // 20 秒断流的根因就在这一段拼装里，而它躲过了上面所有校验和测试。
    // 错法是：头缓冲区按"头+载荷"的长度申请（内容是填零），载荷却又 append 到尾巴上。
    // 于是三件事同时发生：
    //   1) 声明在长度字段里的那 N 字节载荷**全是 0**——真正的载荷跑到了缓冲区尾巴；
    //   2) UDP 长度字段比缓冲区真实长度少 N，IPv6 头里的 payload length 又是按真实
    //      长度写的，两者不一致；
    //   3) 校验和按**两倍的长度**算，而内核只核对长度字段那么多字节 → 校验失败。
    // 内核丢弃一个校验和错的 UDP 数据报是**静默**的：不回错、不计进 socket 的
    // `pkts in`，所以设备侧唯一的表现就是"一个都没收到"——排查时手里的包形状
    // 全都对，因为错的从来不是形状，是缓冲区。
    {
        const auto s = addr("fd00::2");
        const auto d = addr("fd00::1");
        const std::vector<uint8_t> payload = {'A', 'B', 'C', 'D'};
        const auto dgram = scrctl::net::build_udp_datagram(0x1234, 0x5678, s.data(), d.data(),
                                                           payload);
        const uint16_t declared = static_cast<uint16_t>(dgram[4] << 8 | dgram[5]);
        check(dgram.size() == payload.size() + 8, "数据报长度 = 8 + 载荷长（不是两倍）");
        check(declared == dgram.size(), "UDP 长度字段 == 缓冲区真实字节数");
        check(std::equal(payload.begin(), payload.end(), dgram.begin() + 8),
              "载荷紧跟在 8 字节头之后（不是零、也没重复一遍）");
        // 内核是按长度字段裁完字节再核对的，所以自校验也必须用这个长度。
        check(scrctl::net::l4_checksum(s.data(), d.data(), dgram.data(), declared, 17) == 0,
              "按内核会核对的那段字节自校验得 0");
        // 偶数长度的载荷走完上面三步；奇数长度会碰上半字节进位，单独测一个。
        const std::vector<uint8_t> odd = {'X', 'Y', 'Z'};
        const auto odd_dgram =
            scrctl::net::build_udp_datagram(0x1234, 0x5678, s.data(), d.data(), odd);
        check(odd_dgram.size() == 11 && std::equal(odd.begin(), odd.end(), odd_dgram.begin() + 8),
              "奇数长度载荷：长度 11、载荷在第 9 字节起");
        check(scrctl::net::l4_checksum(s.data(), d.data(), odd_dgram.data(), odd_dgram[4] << 8 |
                                                               odd_dgram[5],
                                       17) == 0,
              "奇数长度载荷按声明长度自校验得 0");
    }

    std::printf("\n%s (失败 %d 项)\n", Failures == 0 ? "全部通过" : "存在失败", Failures);
    return Failures == 0 ? 0 : 1;
}
