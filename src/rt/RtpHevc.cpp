#include "rt/RtpHevc.h"

#include <cstring>

namespace scrctl::rt {
namespace {

constexpr std::size_t kRtpHeaderLen = 12;
/// 苹果在 RTP 载荷前多塞的子头，实测固定 8 字节，语义未记录。
constexpr std::size_t kAppleSubHeaderLen = 8;
constexpr uint8_t kTypeAgg16 = 48;
constexpr uint8_t kTypeFragment = 49;

uint16_t be16(const uint8_t *p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t *p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

void append_start_code(std::vector<uint8_t> &out) {
    static const uint8_t kStartCode[4] = {0, 0, 0, 1};
    out.insert(out.end(), kStartCode, kStartCode + 4);
}

void append_annexb(std::vector<uint8_t> &out, const uint8_t *nal, std::size_t len) {
    append_start_code(out);
    out.insert(out.end(), nal, nal + len);
}

}  // namespace

bool parse_rtp_header(std::span<const uint8_t> d, PacketInfo &out) {
    if (d.size() < kRtpHeaderLen) {
        return false;
    }
    if ((d[0] >> 6) != 2) {
        return false;  // 只认 RTP v2
    }
    out.marker = (d[1] & 0x80) != 0;
    out.payload_type = static_cast<uint8_t>(d[1] & 0x7F);
    out.sequence = be16(d.data() + 2);
    out.timestamp = be32(d.data() + 4);
    out.ssrc = be32(d.data() + 8);

    std::size_t pos = kRtpHeaderLen;
    const std::size_t csrc = static_cast<std::size_t>(d[0] & 0x0F) * 4;
    if (csrc > 0) {
        if (d.size() < pos + csrc) {
            return false;
        }
        pos += csrc;
    }
    // 有扩展头时必须按它的长度跳，否则后面整段偏移都错，且错得毫无征兆。
    if ((d[0] & 0x10) != 0) {
        if (d.size() < pos + 4) {
            return false;
        }
        const std::size_t ext = (static_cast<std::size_t>(be16(d.data() + pos + 2)) + 1) * 4;
        if (d.size() < pos + ext) {
            return false;
        }
        pos += ext;
    }
    if (d.size() < pos + kAppleSubHeaderLen + 2) {
        return false;
    }
    out.payload_offset = pos + kAppleSubHeaderLen;
    return true;
}

void HevcRtpDepacketizer::reset() {
    if (!partial_.empty()) {
        ++stats_.dropped_fragments;
    }
    partial_.clear();
    partial_ts_ = 0;
    partial_type_ = 0;
}

bool HevcRtpDepacketizer::push(std::span<const uint8_t> datagram, std::vector<uint8_t> &out,
                               std::string &err) {
    PacketInfo info;
    if (!parse_rtp_header(datagram, info)) {
        ++stats_.malformed;
        err = "不是 RTP 包";
        return false;
    }
    ++stats_.packets;
    std::span<const uint8_t> body = datagram.subspan(info.payload_offset);

    while (body.size() >= 2) {
        const uint8_t *nal_header = body.data();
        const uint8_t type = static_cast<uint8_t>((body[0] >> 1) & 0x3F);
        body = body.subspan(2);

        if (type == kTypeAgg16) {
            // 聚合包：(2 字节大端长度 + 完整 NAL)*
            while (body.size() >= 2) {
                const std::size_t len = be16(body.data());
                body = body.subspan(2);
                if (len == 0 || len > body.size()) {
                    break;  // 剩下的放不下，当尾部噪声丢掉
                }
                append_annexb(out, body.data(), len);
                body = body.subspan(len);
                ++stats_.nals;
            }
            break;
        }

        if (type == kTypeFragment) {
            if (body.empty()) {
                ++stats_.malformed;
                break;
            }
            const uint8_t fu = body[0];
            const bool start = (fu & 0x80) != 0;
            const bool end = (fu & 0x40) != 0;
            const uint8_t inner_type = static_cast<uint8_t>(fu & 0x3F);
            body = body.subspan(1);

            if (start) {
                if (!partial_.empty()) {
                    // 上一片没等到结尾就来了新片的开头：说明中间丢了包。
                    ++stats_.dropped_fragments;
                }
                partial_.clear();
                partial_type_ = inner_type;
                partial_ts_ = info.timestamp;
            } else if (partial_.empty()) {
                // 中间/结尾分片先到、起始分片没到：这个 NAL 拼不出来，丢掉。
                ++stats_.dropped_fragments;
                break;
            }
            // 这里**不跳 DONL**：见头文件里那段说明，跳了每个分片都少 2 字节真码流。
            partial_.insert(partial_.end(), body.begin(), body.end());
            if (end) {
                // 重组：起始分片声明的 TU 还原成 2 字节 NAL 头，后面接所有分片数据
                // （含本片——上面已经把本片载荷插进 partial_ 了）。
                const uint8_t header[2] = {static_cast<uint8_t>((partial_type_ << 1) & 0x7E),
                                           0x01};
                append_start_code(out);
                out.insert(out.end(), header, header + 2);
                out.insert(out.end(), partial_.begin(), partial_.end());
                ++stats_.nals;
                partial_.clear();
            }
            break;
        }

        // 单一 NAL：剩下整个都是它。
        {
            std::vector<uint8_t> nal(2);
            nal[0] = nal_header[0];
            nal[1] = nal_header[1];
            nal.insert(nal.end(), body.begin(), body.end());
            append_annexb(out, nal.data(), nal.size());
            ++stats_.nals;
        }
        break;
    }
    return true;
}

}  // namespace scrctl::rt
