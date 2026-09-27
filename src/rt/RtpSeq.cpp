#include "rt/RtpSeq.h"

namespace scrctl::rt {

RtpSeq::Verdict RtpSeq::observe(const uint16_t seq, uint32_t *lost) {
    if (!have_) {
        have_ = true;
        high_ = seq;
        return Verdict::kFirst;
    }
    // 差值按 mod 2^16 解释：`uint16_t(seq - high_)` 落在 (0, 32768) 才是"在水位前面"，
    // 等于 0 是重复、大于等于 32768 是迟到。直接拿 int 比大小会在跨 65535 那一圈上把
    // 一个正常往前走的包判成倒退。
    const unsigned forward = static_cast<unsigned>(static_cast<uint16_t>(seq - high_));
    if (forward == 0 || forward >= 32768) {
        return Verdict::kLate;  // 水位不动：迟到的东西不欠账
    }
    if (lost != nullptr && forward > 1) {
        *lost = forward - 1;
    }
    high_ = seq;
    return forward > 1 ? Verdict::kGap : Verdict::kInOrder;
}

}  // namespace scrctl::rt
