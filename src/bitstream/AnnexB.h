#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace scrctl {

/// 一个 NAL，包含两字节 HEVC NAL 头，字节保持与 Annex-B 起始码之后的载荷一致。
/// 保留 emulation prevention byte（EPB），便于将原始 NAL 传给解码器。
using Nal = std::vector<uint8_t>;

enum class NalType : uint8_t {
    Vps = 32,
    Sps = 33,
    Pps = 34,
    SeiPrefix = 39,
};

/// 流式 Annex-B -> Access Unit 解析器。
///
/// 根据 slice segment header 的 first_slice_segment_in_pic_flag 判断新图像，
/// 同一图像中的后续 slice 继续合并；不能仅凭 NAL 类型划分多 slice 的 AU。
class AnnexBParser {
public:
    using AuHandler = std::function<void(std::vector<Nal> &&au, bool keyframe)>;

    explicit AnnexBParser(AuHandler on_au) : on_au_(std::move(on_au)) {}

    void feed(const uint8_t *data, std::size_t len);
    void flush();

    const Nal &vps() const { return vps_; }
    const Nal &sps() const { return sps_; }
    const Nal &pps() const { return pps_; }
    bool has_parameter_sets() const { return !vps_.empty() && !sps_.empty() && !pps_.empty(); }

private:
    void emit_range(std::size_t end);
    void on_nal(std::vector<uint8_t> &&raw);
    void close_au();

    AuHandler on_au_;

    std::vector<uint8_t> pending_;  ///< 尚未定界的字节
    std::size_t nal_begin_ = 0;     ///< 当前 NAL 负载起点（起始码之后）
    std::size_t scan_from_ = 0;     ///< pending_ 中下一个待检查的起始码位置

    std::vector<Nal> cur_au_;
    bool au_open_ = false;
    /// cur_au_ 是否包含 VCL NAL。仅在包含图像数据时提交 AU，
    /// VPS/SPS/PPS 前缀继续保留，与随后到达的图像组成同一 AU。
    bool au_has_vcl_ = false;
    bool au_keyframe_ = false;

    Nal vps_, sps_, pps_;
};

/// 将完整的 00 00 03 xx（xx<=0x03）转换为 00 00 xx，保留截断或不符合该模式的字节。
/// 返回的 RBSP 仅用于读取语法元素，例如 SPS 的 conformance window 和 profile/tier。
/// 本项目的解码样本及 hvcC 参数集使用原始 NAL，保留 EPB；不要用 RBSP 替换这些输入。
std::vector<uint8_t> unescape_nal(const uint8_t *data, std::size_t len);

}  // namespace scrctl
