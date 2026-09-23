#include "AnnexB.h"

#include <algorithm>

namespace scrctl {
namespace {

constexpr uint8_t nal_type_of(const std::vector<uint8_t> &nal) {
    return nal.size() >= 2 ? static_cast<uint8_t>((nal[0] >> 1) & 0x3F) : 0xFF;
}

bool is_vcl(uint8_t type) { return type <= 31; }
bool is_irap(uint8_t type) { return type >= 16 && type <= 21; }
bool is_param_set(uint8_t type) { return type == 32 || type == 33 || type == 34; }

/// slice segment header 的第一个 bit 就是 first_slice_segment_in_pic_flag，
/// 紧跟在 2 字节 NAL header 之后。
bool starts_new_picture(const Nal &nal) {
    return nal.size() > 2 && (nal[2] & 0x80) != 0;
}

}  // namespace

std::vector<uint8_t> unescape_nal(const uint8_t *data, std::size_t len) {
    std::vector<uint8_t> out;
    out.reserve(len);
    for (std::size_t i = 0; i < len; ++i) {
        // 只有 00 00 03 后面跟着 0x00..0x03 时那个 03 才是插进来的防 emulation 字节；
        // 光看 00 00 03 就删，会在截断的 NAL 上删掉真实码流。
        if (i + 3 < len && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 3 &&
            data[i + 3] <= 3) {
            out.push_back(0);
            out.push_back(0);
            i += 2;  // 跳过 0x03
            continue;
        }
        out.push_back(data[i]);
    }
    return out;
}

void AnnexBParser::feed(const uint8_t *data, std::size_t len) {
    pending_.insert(pending_.end(), data, data + len);

    std::size_t i = scan_from_;
    while (i + 3 <= pending_.size()) {
        if (pending_[i] == 0 && pending_[i + 1] == 0 && pending_[i + 2] == 1) {
            std::size_t end = i;
            if (end > nal_begin_ && pending_[end - 1] == 0) {
                --end;  // 4 字节起始码，末尾那个 0 属于起始码
            }
            emit_range(end);
            nal_begin_ = i + 3;
            i = nal_begin_;
            continue;
        }
        ++i;
    }

    // 保留可能横跨 feed 边界的最后 2 字节（半个起始码）
    scan_from_ = std::max(nal_begin_, pending_.size() >= 2 ? pending_.size() - 2 : 0);

    if (nal_begin_ > 0) {
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<long>(nal_begin_));
        scan_from_ -= nal_begin_;
        nal_begin_ = 0;
    }
}

void AnnexBParser::flush() {
    emit_range(pending_.size());
    pending_.clear();
    scan_from_ = 0;
    nal_begin_ = 0;
    close_au();
}

void AnnexBParser::emit_range(std::size_t end) {
    if (end <= nal_begin_) {
        return;
    }
    std::vector<uint8_t> raw(pending_.begin() + static_cast<long>(nal_begin_),
                             pending_.begin() + static_cast<long>(end));
    on_nal(std::move(raw));
}

void AnnexBParser::on_nal(std::vector<uint8_t> &&nal) {
    if (nal.size() < 3) {
        return;
    }
    // 这里**不**去 emulation prevention 字节。曾经去过，代价很隐蔽：
    // VideoToolbox 的长度前缀样本要求 NAL 字节与 Annex-B 里起始码之后的原样一致
    // （含 00 00 03），hvcC 里的参数集同理（与 avcC 同一套规则），去掉之后解码器
    // 读到的是另一串东西。实测同一台机器、同一份 SPS，两份录屏一份"看起来正常"
    // 一份整片噪声——因为去掉 EPB 后 RBSP 里会出现 00 00 01，是否踩到取决于码流
    // 内容，所以是概率性出错。
    // 判类型与 first_slice_segment_in_pic_flag 都不受影响：EPB 需要前面有两个
    // 零字节，不可能出现在 NAL 头或 RBSP 的第一个字节上。
    const uint8_t type = nal_type_of(nal);

    if (type == static_cast<uint8_t>(NalType::Vps)) {
        vps_ = nal;
    } else if (type == static_cast<uint8_t>(NalType::Sps)) {
        sps_ = nal;
    } else if (type == static_cast<uint8_t>(NalType::Pps)) {
        pps_ = nal;
    }

    if (is_vcl(type)) {
        // 参数集是紧随其后的图像帧的前缀，必须留在同一个 AU 里。
        // 因此只有当前 AU 已经含图像数据时，新图像开始才意味着收尾；
        // 否则把已攒下的 VPS/SPS/PPS 带进这一帧。
        const bool new_pic = starts_new_picture(nal);
        if (new_pic) {
            if (au_has_vcl_) {
                close_au();
            }
        } else if (!au_open_) {
            return;  // 中途接入流，丢弃首个完整帧之前的残留 slice
        }
        au_open_ = true;
        au_has_vcl_ = true;
        if (is_irap(type)) {
            au_keyframe_ = true;
        }
        cur_au_.push_back(std::move(nal));
        return;
    }

    if (is_param_set(type)) {
        // 参数集是下一个 AU 的前缀：只有当前 AU 已有图像数据才收尾，
        // 否则继续把它们累积在同一组前缀里。
        if (au_has_vcl_) {
            close_au();
        }
        au_open_ = true;
        cur_au_.push_back(std::move(nal));
        return;
    }

    if (au_open_) {
        cur_au_.push_back(std::move(nal));
    }
}

void AnnexBParser::close_au() {
    if (au_has_vcl_ && !cur_au_.empty()) {
        on_au_(std::move(cur_au_), au_keyframe_);
    }
    cur_au_.clear();
    au_open_ = false;
    au_has_vcl_ = false;
    au_keyframe_ = false;
}

}  // namespace scrctl
