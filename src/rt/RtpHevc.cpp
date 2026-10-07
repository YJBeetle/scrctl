#include "i18n/Translation.h"
#include "rt/RtpHevc.h"

#include <cstring>

namespace scrctl::rt {
namespace {

constexpr std::size_t kRtpHeaderLen = 12;
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
    // 扩展长度以 32 位字计，不含扩展头自身的 4 字节；按声明长度跳过整个扩展。
    // 不再额外跳过固定长度的私有头。
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
    if (d.size() < pos + 2) {
        return false;
    }
    out.payload_offset = pos;
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
        err = SCRCTL_TR("Not an RTP packet");
        return false;
    }
    if (info.payload_type != video_pt_) {
        // 非视频 PT 整包跳过，不参与当前视频流的序号统计。
        ++stats_.other_payload;
        return true;
    }
    ++stats_.packets;
    // 由 RtpSeq 按模 2^16 跟踪序号，迟到包不改变当前最高序号。
    switch (seq_.observe(info.sequence)) {
    case RtpSeq::Verdict::kGap:
        ++stats_.seq_gaps;
        break;
    case RtpSeq::Verdict::kLate:
        ++stats_.reordered;
        break;
    case RtpSeq::Verdict::kFirst:
    case RtpSeq::Verdict::kInOrder:
        break;
    }
    // 直接读取当前缺口数，保留窗口内迟到补齐带来的减少。
    stats_.seq_lost = seq_.lost();
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
                    break;  // 长度为零或超过剩余载荷，停止解析此聚合包
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
                    // 新起始分片到达时，前一个 NAL 仍未完成，计为丢弃。
                    ++stats_.dropped_fragments;
                }
                partial_.clear();
                partial_type_ = inner_type;
                partial_ts_ = info.timestamp;
            } else if (partial_.empty()) {
                // 没有已缓存的起始分片，无法重组中间或结尾分片。
                ++stats_.dropped_fragments;
                break;
            }
            // 当前适配不含 DONL，FU 头后全部字节均作为分片数据。
            partial_.insert(partial_.end(), body.begin(), body.end());
            if (end) {
                // 使用起始分片的 TU 构造两字节 NAL 头，随后连接全部缓存分片数据。
                // 当前实现写入 layer_id=0、temporal_id_plus1=1。
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
