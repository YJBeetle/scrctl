#pragma once

#include <cstdint>

namespace scrctl::probe {

// 时间由调用者传入，生产使用单调时钟，离线测试可以精确推进时间。
// 用差值比较避免“起点 + 时限”溢出；时限必须大于零。
class Deadline {
public:
    Deadline(uint64_t start_ms, uint64_t limit_ms) : start_ms_(start_ms), limit_ms_(limit_ms) {}

    bool expired(uint64_t now_ms) const { return now_ms - start_ms_ >= limit_ms_; }
    uint64_t remaining(uint64_t now_ms) const {
        const uint64_t elapsed = now_ms - start_ms_;
        return elapsed >= limit_ms_ ? 0 : limit_ms_ - elapsed;
    }

private:
    uint64_t start_ms_;
    uint64_t limit_ms_;
};

enum class QuietResult { Pending, Quiet, TimedOut };

// 等待数据报停止。新数据报只重置静默计时，不能延长总时限。
// Quiet 仅说明本地收包静默；设备会话是否结束仍须单独查询。
class QuietWait {
public:
    QuietWait(uint64_t start_ms, uint64_t packets, uint64_t quiet_ms, uint64_t limit_ms)
        : deadline_(start_ms, limit_ms), last_change_ms_(start_ms), packets_(packets),
          quiet_ms_(quiet_ms) {}

    QuietResult observe(uint64_t now_ms, uint64_t packets) {
        if (packets != packets_) {
            packets_ = packets;
            last_change_ms_ = now_ms;
        }
        // 截止时刻与静默达标重合时，不能把超时样本记为成功。
        if (deadline_.expired(now_ms)) {
            return QuietResult::TimedOut;
        }
        return quiet_for(now_ms) >= quiet_ms_ ? QuietResult::Quiet : QuietResult::Pending;
    }

    uint64_t quiet_for(uint64_t now_ms) const { return now_ms - last_change_ms_; }
    uint64_t remaining(uint64_t now_ms) const { return deadline_.remaining(now_ms); }

private:
    Deadline deadline_;
    uint64_t last_change_ms_;
    uint64_t packets_;
    uint64_t quiet_ms_;
};

}  // namespace scrctl::probe
