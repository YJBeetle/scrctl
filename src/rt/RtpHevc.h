#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace scrctl::rt {

/// CoreDevice 视频流的 RTP 载荷拆包。
///
/// 载荷是 RFC 7798 的 HEVC RTP 格式。每个包的 RTP 头之后是一个 8 字节的 RTP
/// 扩展头（X=1，profile 0x9011，长度 1 个 32 位字；内容语义未记录，按它自己声明
/// 的长度跳开即可），再往后才是 HEVC 载荷：
///
/// ```text
///   2 字节 NAL 头，type = (b0 >> 1) & 0x3F
///     48  聚合包：后面是 (2 字节大端长度 + NAL)*
///     49  分片：再跟 1 字节 FU 头（S=0x80 E=0x40 TU=0x3F），然后是分片数据
///     其它 单一 NAL，剩下整个载荷就是它
/// ```
///
/// 那 8 字节一度被记成"苹果在 RTP 载荷前多塞的私有子头"，于是代码里按扩展头长度
/// 跳了一遍、又固定多跳 8 字节——每个 NAL 都从中间开始，解出来的类型全是 63、92
/// 这种不存在的值。手工构造的测试包是 X=0 + 8 字节子头，跟这个错误假设正好自洽，
/// 所以测试全绿而真机全废。现在测试也照真机的样子构造（X=1 + 扩展头）。
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
    std::size_t payload_offset = 0;  ///< 跳掉 RTP 固定头、CSRC 与扩展头之后的起点
};

/// 校验 RTP 版本与最小长度。失败时不要碰返回值。
[[nodiscard]] bool parse_rtp_header(std::span<const uint8_t> datagram, PacketInfo &out);

}  // namespace scrctl::rt
