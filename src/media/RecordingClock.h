#pragma once

#include "rt/Rtcp.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string_view>

namespace scrctl::media {

/// 单个录制 epoch 内的 SR 时间映射。音视频各持有一个实例，使用同一 NTP
/// 参考值；返回带小数的微秒，调用方减去一次确定的录制原点后才量化。
/// 共同参考应取本次首个可信 SR 附近，避免遥远参考引入额外微秒舍入。
///
/// 此类只处理已解析的时钟锚点，不负责媒体缓存、跨轨排序、容器或线程。
/// 调用方必须确认来源和 epoch，并提供当前媒体的展开 ticks。它不能从
/// 长时间失联后的 32 位值猜出周期，也不能证明两轨的 NTP 实际同步。
class RecordingClock {
public:
    struct Limits {
        std::chrono::microseconds boundary_extrapolation{1'500'000};
        std::chrono::microseconds report_gap{5'000'000};
        std::size_t anchors = 8;
    };
    enum class ReportResult { Accepted, Duplicate, Error };
    enum class State { Ready, Pending, Error };
    enum class Mode { Running, Final };
    struct Interval {
        State state = State::Pending;
        long double begin_us = 0;
        long double end_us = 0;
    };

    RecordingClock(uint32_t source, uint64_t common_ntp_reference);
    RecordingClock(uint32_t source, uint64_t common_ntp_reference, Limits limits);

    /// 同源 SR 使用最近周期展开；NTP/ticks 必须前进，完全重复的时钟锚点忽略。
    /// packet/octet 计数可以合法不变或回绕，不参与时钟映射。
    /// 尚未保留过的旧锚点严格拒绝；此首版不插入乱序 SR 来修改已使用的区间。
    /// 错误锁存至实例销毁；重建会话应建立新实例，不能拼接旧 epoch。
    ReportResult add_report(const rt::SenderReport& report, int64_t media_reference);

    /// 至少两个 SR 才能确定频率。运行中不外推最新 SR 之后的数据，而是返回
    /// Pending 等待右锚点；结束时才允许有预算的尾部外推。起始边界也有预算。
    /// Final 不再等待未来报告；锚点不足时直接返回 Error。
    /// begin/end 均为展开 ticks，允许相等以映射单点，不改变输入的呈现顺序。
    [[nodiscard]] Interval map_interval(int64_t begin, int64_t end,
                                        Mode mode = Mode::Running) const;

    /// 所有较早样本已消费后释放锚点；保留该位置左侧的一点和至少末尾两点。
    /// keep_recent 可保留更多近期历史，供完整 AU 晚于其采样时刻到达时使用；
    /// 它不增加 anchors 总预算，也不承诺固定秒数的采样历史。
    /// 调用方不能释放仍有待定样本需要的锚点。释放后的旧时间不会重新外推。
    void discard_before(int64_t earliest_pending_ticks, std::size_t keep_recent = 2);

    [[nodiscard]] std::size_t anchor_count() const noexcept { return anchors_.size(); }
    [[nodiscard]] std::string_view error() const noexcept { return error_; }

private:
    struct Anchor { uint64_t ntp; int64_t ticks; long double time_us; };
    struct Point { State state; long double time_us = 0; };
    [[nodiscard]] Point map_point(int64_t ticks, Mode mode) const;
    ReportResult fail(std::string_view reason);

    uint32_t source_;
    uint64_t reference_;
    Limits limits_;
    std::deque<Anchor> anchors_;
    bool pruned_ = false;
    std::string_view error_;
};

}  // namespace scrctl::media
