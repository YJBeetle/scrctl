#include "media/RecordingClock.h"
#include "rt/RtpTimestamp.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <optional>

namespace scrctl::media {
namespace {
constexpr uint64_t ntp_half_cycle = uint64_t{1} << 63;
constexpr long double q32_to_us = 1'000'000.0L / 4'294'967'296.0L;

// NTP 32 位秒字段同样会回绕。只使用离共同参考不到半周期的差，先做
// 无符号减法再转换，避免把绝对 Q32 大数转成浮点后丢掉低位精度。
std::optional<long double> relative_us(uint64_t ntp, uint64_t reference) {
    const auto forward = ntp - reference;
    if (forward == ntp_half_cycle) return std::nullopt;
    if (forward < ntp_half_cycle) return static_cast<long double>(forward) * q32_to_us;
    return -static_cast<long double>(reference - ntp) * q32_to_us;
}

long double tick_distance(int64_t value, int64_t reference) {
    // 无符号差保留包括跨越 signed 零点在内的完整距离，不计算溢出的有符号差。
    if (value >= reference) {
        return static_cast<long double>(static_cast<uint64_t>(value) -
                                        static_cast<uint64_t>(reference));
    }
    return -static_cast<long double>(static_cast<uint64_t>(reference) -
                                     static_cast<uint64_t>(value));
}
}  // namespace

RecordingClock::RecordingClock(uint32_t source, uint64_t reference)
    : RecordingClock(source, reference, Limits{}) {}

RecordingClock::RecordingClock(uint32_t source, uint64_t reference, Limits limits)
    : source_(source), reference_(reference), limits_(limits) {
    if (limits_.boundary_extrapolation.count() < 0 || limits_.report_gap.count() <= 0 ||
        limits_.anchors < 2) {
        error_ = "Invalid recording clock limits";
    }
}

RecordingClock::ReportResult RecordingClock::fail(std::string_view reason) {
    if (error_.empty()) error_ = reason;
    return ReportResult::Error;
}

RecordingClock::ReportResult RecordingClock::add_report(const rt::SenderReport& report,
                                                        int64_t media_reference) {
    if (!error_.empty()) return ReportResult::Error;
    if (report.ssrc != source_) return fail("Sender report belongs to another media source");
    const uint64_t ntp = (uint64_t{report.ntp_seconds} << 32) | report.ntp_fraction;
    if (ntp == 0) return fail("Sender report has no NTP clock");
    const auto ticks = rt::RtpTimestamp::nearest(report.rtp_timestamp, media_reference);
    const auto time = relative_us(ntp, reference_);
    if (!ticks || !time) return fail("Sender report clock has an ambiguous wrap cycle");
    if (std::any_of(anchors_.begin(), anchors_.end(), [&](const Anchor& anchor) {
            return ntp == anchor.ntp && *ticks == anchor.ticks;
        })) {
        return ReportResult::Duplicate;
    }
    if (!anchors_.empty()) {
        const auto& previous = anchors_.back();
        const auto ntp_delta = ntp - previous.ntp;
        if (ntp_delta == 0 || ntp_delta >= ntp_half_cycle || *ticks <= previous.ticks ||
            *time <= previous.time_us) {
            return fail("Sender report clocks do not advance together");
        }
        if (static_cast<long double>(ntp_delta) * q32_to_us > limits_.report_gap.count()) {
            return fail("Sender report clock gap exceeds the recording budget");
        }
    }
    if (anchors_.size() >= limits_.anchors) return fail("Recording clock anchor limit reached");
    anchors_.push_back({ntp, *ticks, *time});
    return ReportResult::Accepted;
}

RecordingClock::Point RecordingClock::map_point(int64_t ticks, Mode mode) const {
    auto right = std::upper_bound(anchors_.begin(), anchors_.end(), ticks,
                                 [](int64_t value, const Anchor& a) { return value < a.ticks; });
    const bool before = ticks < anchors_.front().ticks;
    const bool after = ticks > anchors_.back().ticks;
    if (before && pruned_) return {State::Error};
    if (after && mode == Mode::Running) return {State::Pending};
    if (right == anchors_.begin()) ++right;
    if (right == anchors_.end()) --right;
    const auto& b = *right;
    const auto& a = *std::prev(right);
    const auto duration = static_cast<long double>(b.ntp - a.ntp) * q32_to_us;
    const auto mapped = a.time_us + tick_distance(ticks, a.ticks) *
                                   duration / tick_distance(b.ticks, a.ticks);
    if (before || after) {
        const auto& edge = before ? anchors_.front() : anchors_.back();
        if (std::abs(mapped - edge.time_us) > limits_.boundary_extrapolation.count()) {
            return {State::Error};
        }
    }
    return {State::Ready, mapped};
}

RecordingClock::Interval RecordingClock::map_interval(int64_t begin, int64_t end,
                                                      Mode mode) const {
    if (!error_.empty() || end < begin) return {State::Error};
    if (anchors_.size() < 2) return {mode == Mode::Final ? State::Error : State::Pending};
    const auto first = map_point(begin, mode);
    const auto last = map_point(end, mode);
    if (first.state == State::Error || last.state == State::Error) return {State::Error};
    if (first.state == State::Pending || last.state == State::Pending) return {};
    const auto valid = [](long double value) {
        return std::isfinite(value) && value > std::numeric_limits<int64_t>::min() &&
               value < std::numeric_limits<int64_t>::max();
    };
    if (!valid(first.time_us) || !valid(last.time_us)) return {State::Error};
    return {State::Ready, first.time_us, last.time_us};
}

void RecordingClock::discard_before(int64_t earliest_pending_ticks, std::size_t keep_recent) {
    const auto retained = std::max(std::size_t{2}, keep_recent);
    while (anchors_.size() > retained && anchors_[1].ticks <= earliest_pending_ticks) {
        anchors_.pop_front();
        pruned_ = true;
    }
}

}  // namespace scrctl::media
