#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace scrctl {

/// 一个 NAL，含 2 字节 HEVC NAL header，字节与 Annex-B 里起始码之后的部分
/// **原样一致**（也就是保留 emulation prevention byte）。
using Nal = std::vector<uint8_t>;

enum class NalType : uint8_t {
    Vps = 32,
    Sps = 33,
    Pps = 34,
    SeiPrefix = 39,
};

/// 流式 Annex-B -> Access Unit 解析器。
///
/// 帧边界靠 slice segment header 的第一个 bit（first_slice_segment_in_pic_flag）
/// 判定，而不是猜 NAL type —— 后者在有多个 slice 的帧上会错。
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
    /// cur_au_ 是否已含 VCL NAL。只有含 VCL 的 AU 才值得收尾——否则会把
    /// VPS/SPS/PPS 单独切成一个空 AU，真正的图像帧反倒丢了参数集前缀。
    bool au_has_vcl_ = false;
    bool au_keyframe_ = false;

    Nal vps_, sps_, pps_;
};

/// 去掉 NAL 中的 emulation prevention byte（00 00 03 xx, xx<=0x03 -> 00 00 xx），
/// 得到 RBSP。只在**读语法元素**时用：SPS 的 conformance window、profile/tier
/// 这些要按 RBSP 位流解析。
///
/// 不要把结果喂给解码器。长度前缀样本与 hvcC 里的参数集都要求原样保留 EPB
/// （与 avcC 同一套规则），去掉了就不是那段码流的字节了——去掉之后 RBSP 里还
/// 可能凭空出现 00 00 01，是否踩到取决于内容，所以错得是概率性的。
std::vector<uint8_t> unescape_nal(const uint8_t *data, std::size_t len);

}  // namespace scrctl
