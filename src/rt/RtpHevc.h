#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "rt/RtpSeq.h"

namespace scrctl::rt {

/// CoreDevice 视频流的 HEVC RTP 载荷拆包，按当前适配的无 DONL 形态处理。
///
/// 已观察的视频包带 X=1 的 RTP 扩展头：profile 0x9011，扩展数据长度为一个
/// 32 位字，连同扩展头自身共 8 字节。解析器按包内声明长度跳过扩展，不依赖此固定值。
/// 扩展之后的 HEVC 载荷按 RFC 7798 的类型标记分派：
///
/// ```text
///   2 字节 NAL 头，type = (b0 >> 1) & 0x3F
///     48  聚合包：后面是 (2 字节大端长度 + NAL)*
///     49  分片：再跟 1 字节 FU 头（S=0x80 E=0x40 TU=0x3F），然后是分片数据
///     其它 单一 NAL，剩下整个载荷就是它
/// ```
///
/// 当前设备流不含 DONL，FU 头后直接连接分片数据；该约束不能推广到所有 HEVC RTP 流。
class HevcRtpDepacketizer {
public:
    /// 仅处理指定的视频 PT，其他 PT 不进入 HEVC 载荷解析及视频序号统计。
    /// 当前设备的裸 RTCP 与 RTP 共用 UDP 端口，SR 的开头为 81 c8；若按 RTP
    /// 的 7 位 PT 解读，0xc8 会得到 72。上层需先区分 RTCP，避免非视频数据进入参考链。
    explicit HevcRtpDepacketizer(uint8_t video_payload_type = 100)
        : video_pt_(video_payload_type) {}

    struct Stats {
        uint64_t packets = 0;
        uint64_t nals = 0;
        /// 因分片不完整而丢弃的 NAL 数（丢包或中途 reset）。
        uint64_t dropped_fragments = 0;
        uint64_t malformed = 0;
        /// seq_gaps 统计向前跳过序号的事件数，一次事件可以涉及多个缺失包。
        /// seq_lost 是 RtpSeq 当前未补齐的缺口数，窗口内迟到补齐会使其减少。
        /// reordered 包含迟到包和重复包；不能只凭前两项是否相等判断乱序。
        /// 分片缺失可能使图像不完整或破坏参考链，上层据此判断是否需要请求刷新或重建会话。
        uint64_t seq_gaps = 0;
        uint64_t seq_lost = 0;
        uint64_t reordered = 0;
        /// 非视频 PT（RTCP 等）被跳过的包数。
        uint64_t other_payload = 0;
    };

    /// 处理一个 UDP 数据报，将重组完成的 NAL 以 Annex-B 四字节起始码追加到 out。
    /// false 表示 RTP 头解析失败，err 给出原因；true 不保证本包产生 NAL。
    /// 分片或聚合载荷异常可能仅计入统计或丢弃。
    bool push(std::span<const uint8_t> datagram, std::vector<uint8_t> &out, std::string &err);

    /// 丢弃未完成的分片，用于切换显示、停止流或放弃当前图像。
    /// 不清空序号跟踪和累计统计。
    void reset();

    [[nodiscard]] const Stats &stats() const { return stats_; }
    /// 按模 2^16 顺序跟踪的视频包最高序号，尚未收到视频包时为 0。
    /// 迟到包不使其回退；此值为 16 位序号，不包含回绕次数。
    [[nodiscard]] uint16_t last_sequence() const { return seq_.high(); }
    /// 是否有还没收完的分片。
    [[nodiscard]] bool mid_fragment() const { return !partial_.empty(); }
    [[nodiscard]] uint32_t fragment_timestamp() const { return partial_ts_; }

private:
    uint8_t video_pt_;
    /// 统一跟踪序号进展、缺口和迟到补齐，统计规则见 RtpSeq。
    RtpSeq seq_;
    std::vector<uint8_t> partial_;
    uint32_t partial_ts_ = 0;
    uint8_t partial_type_ = 0;
    Stats stats_;
};

/// RTP 头的解析结果，测试与上层统计都要用。
struct PacketInfo {
    bool marker = false;
    uint8_t payload_type = 0;
    uint16_t sequence = 0;
    uint32_t timestamp = 0;
    uint32_t ssrc = 0;
    std::size_t payload_offset = 0;  ///< 跳掉 RTP 固定头、CSRC 与扩展头之后的起点
};

/// 校验 RTP v2、12 字节固定头、CSRC/扩展声明长度，以及至少 2 字节剩余载荷。
/// 失败时 out 可能只被部分填写，调用方不得使用。
[[nodiscard]] bool parse_rtp_header(std::span<const uint8_t> datagram, PacketInfo &out);

}  // namespace scrctl::rt
