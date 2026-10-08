#include "media/RecordingClock.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

namespace {
using Clock = scrctl::media::RecordingClock;
using Report = scrctl::rt::SenderReport;
int checks = 0;
int failures = 0;
constexpr uint64_t reference = uint64_t{0xee5fe7ad} << 32;

void check(bool value, const char* message) {
    ++checks;
    if (!value) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

Report report(uint64_t ntp, int64_t ticks, uint32_t source = 7) {
    return {source, static_cast<uint32_t>(ntp >> 32), static_cast<uint32_t>(ntp),
            static_cast<uint32_t>(ticks), 0, 0};
}

void add(Clock& clock, uint64_t ntp, int64_t ticks, uint32_t source = 7) {
    check(clock.add_report(report(ntp, ticks, source), ticks) == Clock::ReportResult::Accepted,
          "valid sender report accepted");
}

void ready(const Clock& clock, int64_t begin, int64_t end, int64_t begin_us, int64_t end_us,
           Clock::Mode mode = Clock::Mode::Running) {
    const auto value = clock.map_interval(begin, end, mode);
    check(value.state == Clock::State::Ready, "interval is ready");
    check(std::llround(value.begin_us) == begin_us && std::llround(value.end_us) == end_us,
          "mapped endpoints retain their expected common-clock times");
}

void wait_for_right_report() {
    Clock clock(7, reference);
    check(clock.map_interval(1000, 1400).state == Clock::State::Pending,
          "no reports cannot infer the sampling frequency");
    check(clock.map_interval(1000, 1400, Clock::Mode::Final).state == Clock::State::Error,
          "EOF without clock reports cannot wait for future anchors");
    add(clock, reference, 1000);
    check(clock.map_interval(1000, 1400).state == Clock::State::Pending,
          "one report still cannot infer the frequency");
    check(clock.map_interval(1000, 1400, Clock::Mode::Final).state == Clock::State::Error,
          "EOF with one report still has no trustworthy frequency");
    add(clock, reference + (uint64_t{1} << 32), 25000);
    ready(clock, 1000, 7000, 0, 250000);
    ready(clock, 25000, 25000, 1000000, 1000000);
    check(clock.map_interval(24999, 25001).state == Clock::State::Pending,
          "running sample ending after the latest report waits, even when its start is covered");
    ready(clock, 25000, 37000, 1000000, 1500000, Clock::Mode::Final);
    ready(clock, -35000, -11000, -1500000, -500000);
    ready(clock, 25000, 61000, 1000000, 2500000, Clock::Mode::Final);
    check(clock.map_interval(-35001, 1000).state == Clock::State::Error,
          "initial extrapolation over budget fails");
    check(clock.map_interval(25000, 61001, Clock::Mode::Final).state == Clock::State::Error,
          "final extrapolation over budget fails");
    check(clock.map_interval(25000, 61001).state == Clock::State::Pending,
          "running tail stays pending instead of consuming the EOF extrapolation budget");
    check(clock.map_interval(7000, 1000).state == Clock::State::Error,
          "reversed interval rejected without changing time order");
    add(clock, reference + (uint64_t{2} << 32), 49000);
    ready(clock, 25001, 37000, 1000042, 1500000);
    ready(clock, 16000, 18000, 625000, 708333);
    ready(clock, 7000, 10000, 250000, 375000);
}

void common_origin_and_variable_intervals() {
    Clock video(7, reference);
    Clock audio(9, reference);
    add(video, reference, 1000);
    add(video, reference + (uint64_t{1} << 32), 25000);
    add(audio, reference + (uint64_t{1} << 30), 9000, 9);
    add(audio, reference + (uint64_t{5} << 30), 57000, 9);
    ready(video, 1000, 2200, 0, 50000);
    ready(audio, 9000, 9480, 250000, 260000);
    // 独立生成的视频呈现间隔为 50、25、75 ms，不用帧号 / 帧率计算期望。
    constexpr std::array<int64_t, 4> ticks{1000, 2200, 2800, 4600};
    constexpr std::array<int64_t, 4> times{0, 50000, 75000, 150000};
    for (std::size_t i = 0; i + 1 < ticks.size(); ++i) {
        ready(video, ticks[i], ticks[i + 1], times[i], times[i + 1]);
    }
    // 参考原点和样本均含小数微秒。先分别取整会把相差一 tick 的样本从
    // 正确的 42 us 变成 41 us；时钟保留小数，消费者减原点后才量化。
    const auto origin = video.map_interval(1001, 1001);
    const auto sample = video.map_interval(1002, 1002);
    check(origin.state == Clock::State::Ready && sample.state == Clock::State::Ready,
          "fractional origin and sample are both mapped");
    check(std::llround(sample.begin_us - origin.begin_us) == 42,
          "quantization happens after subtracting the shared recording origin");
    Clock earlier(9, reference);
    add(earlier, reference - (uint64_t{1} << 30), 9000, 9);
    add(earlier, reference + (uint64_t{3} << 30), 57000, 9);
    ready(earlier, 9000, 9480, -250000, -240000);
}

void source_and_report_validation() {
    Clock zero(0, reference);
    add(zero, reference, 0, 0);
    add(zero, reference + (uint64_t{1} << 32), 48000, 0);
    ready(zero, 0, 480, 0, 10000);
    auto duplicate = report(reference + (uint64_t{1} << 32), 48000, 0);
    duplicate.packet_count = 0xffffffff;
    check(zero.add_report(duplicate, 48000) == Clock::ReportResult::Duplicate,
          "identical clock anchor does not need a new queue slot");
    // 计数从接近回绕值回到零，同时 octet 不变，仍是合法时钟锚点。
    add(zero, reference + (uint64_t{2} << 32), 96000, 0);
    check(zero.add_report(report(reference, 0, 0), 96000) == Clock::ReportResult::Duplicate,
          "a delayed duplicate of a retained old report is ignored");
    check(zero.anchor_count() == 3, "duplicate leaves anchor count unchanged");

    Clock foreign(7, reference);
    check(foreign.add_report(report(reference, 1000, 8), 1000) == Clock::ReportResult::Error,
          "another source cannot establish this recording clock");
    check(!foreign.error().empty() && foreign.anchor_count() == 0, "source error is explicit");
    check(foreign.add_report(report(reference, 1000), 1000) == Clock::ReportResult::Error,
          "invalid epoch remains failed rather than silently restarting");

    Clock absent(7, reference);
    check(absent.add_report(report(0, 1000), 1000) == Clock::ReportResult::Error,
          "zero NTP is an unavailable clock");
    Clock ambiguous(7, reference);
    check(ambiguous.add_report(report(reference, 0x80000000LL), 0) == Clock::ReportResult::Error,
          "half RTP cycle is ambiguous");
    Clock ntp_ambiguous(7, reference);
    check(ntp_ambiguous.add_report(report(reference + (uint64_t{1} << 63), 0), 0) ==
              Clock::ReportResult::Error, "half NTP cycle is ambiguous");

    for (int kind = 0; kind < 4; ++kind) {
        Clock broken(7, reference);
        add(broken, reference, 1000);
        const uint64_t ntp = kind == 0 ? reference : kind == 1 ? reference - 1 :
            reference + (uint64_t{1} << 32);
        const int64_t ticks = kind == 2 ? 1000 : kind == 3 ? 999 : 25000;
        check(broken.add_report(report(ntp, ticks), ticks) == Clock::ReportResult::Error,
              "NTP and RTP must both advance in the same epoch");
        check(broken.anchor_count() == 1, "invalid report is not retained");
    }
    Clock gap(7, reference);
    add(gap, reference, 1000);
    check(gap.add_report(report(reference + (uint64_t{6} << 32), 145000), 145000) ==
              Clock::ReportResult::Error, "sender clock gap over budget rejected");
    Clock edge(7, reference);
    add(edge, reference, 1000);
    add(edge, reference + (uint64_t{5} << 32), 121000);
    ready(edge, 1000, 121000, 0, 5000000);
}

void wraps_and_signed_extremes() {
    constexpr uint64_t wrap_ref = (uint64_t{0xffffffff} << 32) | (uint64_t{1} << 31);
    Clock wrap(7, wrap_ref);
    add(wrap, wrap_ref, 0xfffff000LL);
    add(wrap, uint64_t{1} << 31, 0xfffff000LL + 24000);
    ready(wrap, 0xfffff000LL + 12000, 0xfffff000LL + 24000, 500000, 1000000);
    for (const auto base : {std::numeric_limits<int64_t>::min(),
                           std::numeric_limits<int64_t>::max() - 48000}) {
        Clock clock(7, reference);
        add(clock, reference, base);
        add(clock, reference + (uint64_t{1} << 32), base + 24000);
        ready(clock, base + 12000, base + 24000, 500000, 1000000);
    }
    Clock clock(7, reference);
    add(clock, reference, 1000);
    add(clock, reference + (uint64_t{1} << 32), 25000);
    check(clock.map_interval(std::numeric_limits<int64_t>::min(), 0).state == Clock::State::Error,
          "extreme old ticks rejected without signed subtraction overflow");
    check(clock.map_interval(0, std::numeric_limits<int64_t>::max(), Clock::Mode::Final).state ==
              Clock::State::Error, "extreme tail rejected without signed subtraction overflow");
}

void retention_and_limits() {
    Clock::Limits limits;
    limits.anchors = 3;
    Clock clock(7, reference, limits);
    add(clock, reference, 1000);
    add(clock, reference + (uint64_t{1} << 32), 25000);
    add(clock, reference + (uint64_t{2} << 32), 49000);
    clock.discard_before(24000);
    check(clock.anchor_count() == 3, "pending interval keeps its left anchor");
    clock.discard_before(25000);
    check(clock.anchor_count() == 2, "consumed interval releases the oldest anchor");
    ready(clock, 25000, 37000, 1000000, 1500000);
    check(clock.map_interval(1000, 2000).state == Clock::State::Error,
          "pruned history cannot be remapped with a newer first interval");
    add(clock, reference + (uint64_t{3} << 32), 73000);
    clock.discard_before(73000);
    check(clock.anchor_count() == 2, "pruning always retains the last pair for EOF mapping");
    ready(clock, 73000, 85000, 3000000, 3500000, Clock::Mode::Final);
    add(clock, reference + (uint64_t{4} << 32), 97000);
    check(clock.add_report(report(reference + (uint64_t{5} << 32), 121000), 121000) ==
              Clock::ReportResult::Error, "anchor budget enforced before allocation");
    check(clock.anchor_count() == 3, "anchor count never exceeds its limit");
    for (int kind = 0; kind < 3; ++kind) {
        auto invalid = Clock::Limits{};
        if (kind == 0) invalid.anchors = 1;
        if (kind == 1) invalid.report_gap = std::chrono::microseconds{0};
        if (kind == 2) invalid.boundary_extrapolation = std::chrono::microseconds{-1};
        Clock bad(7, reference, invalid);
        check(!bad.error().empty(), "invalid limits rejected at construction");
    }
}
}  // namespace

int main() {
    wait_for_right_report();
    common_origin_and_variable_intervals();
    source_and_report_validation();
    wraps_and_signed_extremes();
    retention_and_limits();
    std::printf("Recording clock: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
