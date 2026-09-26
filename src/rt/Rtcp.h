#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace scrctl::rt {

/// RFC 3550 那几种 RTCP 包的字节拼装。
///
/// 为什么要有这一层：设备的媒体会话有一条 **RTCP 空闲计时器**——它从起流那刻开始倒数
/// 我们自己在 `startmediastream` 里报的 `timeout`，只有在到期之前收到过我们的 RTCP 才
/// 会归零。实测对账（同一台设备、同一套探针，只改发包的拼装）：
///   什么都不发        -> 19.97s / 19.99s 死，设备侧 socket `pkts in: 0`
///   每秒一个 RR       -> 40.2s 还在，设备侧 socket `pkts in: 41`
/// 所以"回 RTCP"不是可选的礼貌，而是这条流的**续命动作**，产品路径必须发。
///
/// RTCP 公共头里的长度是 **16 位**（§6.1：V/RC 1 字节 + PT 1 字节 + length 2 字节）。
/// 这个坑在探针里踩过一次、文档里也专门记着"拿自己组装的包去证明设备不理之前，先核对
/// 它的字节数"，结果又用 `put32(v, 7)` 写了一遍：整包从第 3 字节起错位两字节，于是
/// "设备不理我们"这个结论的证据基础又是一个畸形包。所以这几个函数各自钉了字节数的测。
///
/// 只放标准包。AVConference 那种厂商私有的 RTCP APP（PT=204 "RCTL"）留在探针里
/// （`tools/rr_keepalive_probe.cpp`），产品不发它——RR 已经足够复位计时器。

/// RR（§6.4.2）：V=2、RC=1、PT=201、长度 7（= 头之后还有 7 个字），加发送者 SSRC 与
/// 一个 24 字节报告块，一共 32 字节。
///
/// `sender_ssrc` 要填**设备在 answer 里分配给我们这一端的那个 SSRC**（`RemoteSSRC`），
/// 不是我们自己编的：这两个名字是从设备的视角起的（`Local` = 设备自己发的那条流，实测
/// 等于 RTP 头里的 SSRC），填错了就是一条"从没收过包的源"发来的报告。
/// `media_ssrc` 是被指认的那条流，填设备的 `LocalSSRC`。
/// `ext_high` 是扩展最高序号（RFC 3550 的 cumulative-ish 那一位），照实报收到的值。
[[nodiscard]] std::vector<uint8_t> build_rr(uint32_t sender_ssrc, uint32_t media_ssrc,
                                            uint32_t ext_high);

/// SR（§6.4.1）：RC=0 那一档，28 字节。`packets`/`octets` 是我们**发出去**的 RTP 计数
/// ——这条流我们不发媒体，所以如实报 0。
[[nodiscard]] std::vector<uint8_t> build_sr(uint32_t sender_ssrc, uint32_t packets,
                                            uint32_t octets);

/// SDES（PT=202）带一个**空 CNAME**，12 字节——这正是苹果客户端那个复合包 RR+SDES 的
/// 后半段。带上它不是为了续命（裸 RR 就够），是因为照抄对手的包形状比自创便宜。
[[nodiscard]] std::vector<uint8_t> build_sdes(uint32_t sender_ssrc);

/// 同上，但 CNAME 有实义内容。留着的理由是"空 CNAME 才有效"这个假设也值得能测。
[[nodiscard]] std::vector<uint8_t> build_sdes_cname(uint32_t sender_ssrc,
                                                    std::string_view cname);

/// PLI（RFC 4585 §6.3.1，PT=206 / FMT=1）：12 字节，头 + 发送者 SSRC + 被指认的媒体 SSRC。
/// 作用是"我丢了参考帧，请发一个新的 IDR"。
[[nodiscard]] std::vector<uint8_t> build_pli(uint32_t sender_ssrc, uint32_t media_ssrc);

/// FIR（RFC 5104 §4.6，PT=206 / FMT=4）：24 字节。`fir_seq` 是这一路的**序号**（每次请求
/// 加一，同一序号重复发不会让接收端再产一个 IDR——这是它和 PLI 的实际分工）；
/// `target_ssrc` 要重新强制同步的那条流。后面 8 字节的 FCI 序号我们一个 RTP 都没发过，
/// 按规范如实留 0。
[[nodiscard]] std::vector<uint8_t> build_fir(uint32_t sender_ssrc, uint8_t fir_seq,
                                             uint32_t target_ssrc);

/// 一个 UDP 数据报是不是**设备发来的那条 RTCP SR 心跳**。
///
/// 认的是设备那一条：它的 SR 带 RC=1（首字节 `0x81`），而 `build_sr()` 发出去的是 RC=0
/// （首字节 `0x80`）——两个字节串不一样，别拿这个判据去认自己发的包。
///
/// 为什么要单独判：设备的 RTCP SR 和视频**共用同一个 UDP 端口**，而它的 PT=200 超出
/// RTP 的 7 位字段（`0xc8 & 0x7f` 正好是 72），照 PT 判会被当成"视频里混了 PT=72"。
/// SR 是这条流的心跳（画面完全静止也每秒一个），所以"心跳在不在"是判断会话死活最便宜
/// 的信号——拆包器必须能把它和视频分开，两边计数才对得上账。
[[nodiscard]] bool is_rtcp_sr(std::span<const uint8_t> datagram);

}  // namespace scrctl::rt
