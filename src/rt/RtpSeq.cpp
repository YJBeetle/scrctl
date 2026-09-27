#include "rt/RtpSeq.h"

#include <vector>

namespace scrctl::rt {

void RtpSeq::remember_holes(const uint16_t from, const uint32_t count) {
    if (count >= kReorderWindow) {
        // 一次跳掉整个重排窗口：这不是"乱序还没到"，更像是换了 SSRC 或换了会话。
        // 账照样记进 `detected_`（它确实没到），只是不登记这一千多个洞——它们不可能被
        // 补齐，留着只会让迟到包的查找变慢。
        return;
    }
    if (pending_.capacity() < kReorderWindow) {
        pending_.reserve(kReorderWindow);  // 一次跳几百号是实测会有的，别每次 realloc
    }
    for (uint32_t i = 0; i < count; ++i) {
        pending_.push_back(static_cast<uint16_t>(from + i));
    }
}

void RtpSeq::prune() {
    // 距离按 mod 2^16 算：`uint16_t(high_ - s)` 就是 s 落后水位多少个号。表里的洞都是
    // 在水位**之前**登记的，这个差只会长大到被判死，不会绕回小值——`observe` 一次最多
    // 把水位往前推 32767（再多就当迟到了），而判死发生在 1024，走不完一圈。
    std::erase_if(pending_, [this](const uint16_t s) {
        return static_cast<uint32_t>(static_cast<uint16_t>(high_ - s)) >= kReorderWindow;
    });
}

RtpSeq::Verdict RtpSeq::observe(const uint16_t seq) {
    if (!have_) {
        have_ = true;
        high_ = seq;
        return Verdict::kFirst;
    }
    // 差值按 mod 2^16 解释：`uint16_t(seq - high_)` 落在 (0, 32768) 才是"在水位前面"，
    // 等于 0 是重复、大于等于 32768 是迟到。直接拿 int 比大小会在跨 65535 那一圈上把
    // 一个正常往前走的包判成倒退。
    const unsigned forward = static_cast<unsigned>(static_cast<uint16_t>(seq - high_));
    if (forward >= 32768) {
        // 迟到。只有它正好占着一个还没判死的洞时才冲销一次丢失——所以这里是查欠账表
        // 而不是无脑 `++filled_`：重复到达的包（水位上的那一个、或已经补过的洞）不能把
        // `lost()` 往下冲，否则丢包数会被网络抖动改小，那就失去意义了。
        filled_ += std::erase(pending_, seq);
        return Verdict::kLate;  // 水位不动：迟到的东西不欠账
    }
    if (forward == 0) {
        return Verdict::kLate;  // 和水位同一个号：重复包，不是补齐
    }
    // 水位往前走：先把它身后那几个没到的登记成洞，再推进、再判死。
    if (forward > 1) {
        remember_holes(static_cast<uint16_t>(high_ + 1), forward - 1);
        detected_ += forward - 1;
    }
    high_ = seq;
    prune();
    return forward > 1 ? Verdict::kGap : Verdict::kInOrder;
}

}  // namespace scrctl::rt
