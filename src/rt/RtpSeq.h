#pragma once

#include <cstdint>
#include <vector>

namespace scrctl::rt {

/// RTP 序号跟踪，供视频拆包器和音频接收共用。
/// 按模 2^16 比较当前包与已观察到的最高序号，区分连续、缺口、迟到及重复。
/// 迟到包只补齐仍在重排窗口内的缺口，不使最高序号回退。
class RtpSeq {
public:
    enum class Verdict {
        kFirst,    ///< 这一轮第一个包：没有可比的水位，只建水位
        kInOrder,  ///< 正好接在水位后面（forward == 1）
        kGap,      ///< 往前跳了一格以上，中间那几个序号没到
        kLate,     ///< 不超过水位：迟到的旧包，或者重复包
    };

    /// 距最高序号不足 1024 的缺口可由迟到包补齐；达到此距离后删除待补记录，
    /// 但保留在 lost() 中。该窗口是当前统计与内存策略，不是协议规定的乱序上限。
    /// 一次跳过的缺口数达到窗口大小时，不为该次跳跃登记待补记录。
    static constexpr uint32_t kReorderWindow = 1024;

    Verdict observe(uint16_t seq);

    /// 新会话开始时清空序号和统计，避免把不同会话的序号差计为缺口。
    /// 需要跨会话累计时，调用方应在 reset 前保存 lost()，例如 AudioPump 的 lost_carry。
    void reset() {
        have_ = false;
        high_ = 0;
        pending_.clear();
        detected_ = 0;
        filled_ = 0;
    }

    /// 按模 2^16 顺序跟踪的最高序号，迟到包不使其回退；不包含回绕次数。
    [[nodiscard]] uint16_t high() const { return high_; }

    /// 累计检测的缺口数减去窗口内迟到补齐数，包含已过重排窗口的缺口。
    /// kGap 是事件判定，一次事件可能跨越多个序号；gaps_detected() 是缺口序号总数。
    /// 此统计依赖当前的重排窗口和半序号空间比较规则。
    [[nodiscard]] uint64_t lost() const { return detected_ - filled_; }

    /// 累计向前跳过的缺口序号数，只增不减；与 lost() 的差是窗口内补齐数。
    [[nodiscard]] uint64_t gaps_detected() const { return detected_; }

private:
    /// 登记可由迟到包补齐的缺口序号，按检测顺序保存。
    /// observe 推进最高序号后会 prune，使保留的缺口距离小于 kReorderWindow。
    void remember_holes(uint16_t from, uint32_t count);
    /// 删除距离最高序号达到 kReorderWindow 的待补记录，不减少 lost()。
    void prune();

    bool have_ = false;
    uint16_t high_ = 0;
    std::vector<uint16_t> pending_;
    uint64_t detected_ = 0;
    uint64_t filled_ = 0;
};

}  // namespace scrctl::rt
