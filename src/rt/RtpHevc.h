#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace scrctl::rt {

/// CoreDevice 视频流的 RTP 载荷拆包。
///
/// 载荷是 RFC 7798 的 HEVC RTP 格式，但前面多了一层苹果自己的 8 字节子头
/// （语义未记录；实测每个包都是 8 字节，跳过即可）。RTP 头之后：
///
/// ```text
///   2 字节 NAL 头，type = (b0 >> 1) & 0x3F
///     48  聚合包：后面是 (2 字节大端长度 + NAL)*
///     49  分片：再跟 1 字节 FU 头（S=0x80 E=0x40 TU=0x3F），然后是分片数据
///     其它 单一 NAL，剩下整个载荷就是它
/// ```
///
/// **没有 DONL 字段。** 这一条值得单独说：S=1 的分片头后面那 2 字节看起来极像
/// RFC 7798 里可选的 DONL（同一帧里几个分片的这两字节值还相同），按规范把它
/// 跳过后，画面能解出 NAL 结构、类型全对，但解码器一帧都不出——因为每个分片
/// 都少了 2 字节真实码流。留着它才正常。规范里"允许"的字段，这条流里没有。
class HevcRtpDepacketizer {
public:
    struct Stats {
        uint64_t packets = 0;
        uint64_t nals = 0;
        /// 因分片不完整而丢弃的 NAL 数（丢包或中途 reset）。
        uint64_t dropped_fragments = 0;
        uint64_t malformed = 0;
    };

    /// 吃一个 UDP 数据报，把里面完整的 NAL 以 Annex-B（4 字节起始码）追加到 out。
    /// 返回 false 表示这个包连 RTP 头都不是（err 给出原因）。
    bool push(std::span<const uint8_t> datagram, std::vector<uint8_t> &out, std::string &err);

    /// 丢掉未完成的分片。切换显示、停止流、或（M2.7 里）决定放弃当前帧时用。
    void reset();

    [[nodiscard]] const Stats &stats() const { return stats_; }
    /// 是否有还没收完的分片。
    [[nodiscard]] bool mid_fragment() const { return !partial_.empty(); }
    [[nodiscard]] uint32_t fragment_timestamp() const { return partial_ts_; }

private:
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
    std::size_t payload_offset = 0;  ///< 跳掉 RTP 头与苹果子头之后的起点
};

/// 校验 RTP 版本与最小长度。失败时不要碰返回值。
[[nodiscard]] bool parse_rtp_header(std::span<const uint8_t> datagram, PacketInfo &out);

}  // namespace scrctl::rt
