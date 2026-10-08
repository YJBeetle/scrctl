#include "rt/RtpTimestamp.h"

#include <array>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>

namespace {
using scrctl::rt::RtpTimestamp;
int checks = 0;
int failures = 0;

void check(bool ok, const char* message) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

void expect(RtpTimestamp& tracker, uint32_t raw, int64_t value, int64_t high,
            const char* message) {
    check(tracker.observe(raw) == value, message);
    check(tracker.high() == high, "high-water reference matches accepted observations");
}

void initial_and_reset() {
    RtpTimestamp tracker;
    check(!tracker.high(), "new tracker has no timestamp reference");
    expect(tracker, 0, 0, 0, "first zero timestamp is initialized");
    expect(tracker, 400, 400, 400, "forward timestamp retains its ticks");
    expect(tracker, 400, 400, 400, "duplicate timestamp retains its value");
    expect(tracker, 397, 397, 400, "small backward timestamp does not reset the epoch");
    expect(tracker, 0, 0, 400, "raw zero alone does not signal a new epoch");
    tracker.reset();
    check(!tracker.high(), "explicit epoch reset clears the high-water reference");
    expect(tracker, 0, 0, 0, "first zero in the new epoch starts at zero");
    tracker.reset();
    expect(tracker, 0xffffffffU, 4294967295LL, 4294967295LL,
           "first UINT32_MAX stays unsigned rather than becoming minus one");
    tracker.reset();
    expect(tracker, 0x89abcdefU, 2309737967LL, 2309737967LL,
           "first timestamp above INT32_MAX retains its full value");
}

void wraps_and_late_packets() {
    RtpTimestamp tracker;
    expect(tracker, 0xfffffff0U, 4294967280LL, 4294967280LL,
           "timestamp starts near the end of a cycle");
    expect(tracker, 0x10U, 4294967312LL, 4294967312LL,
           "forward wrap extends into the next cycle");
    expect(tracker, 0xfffffff8U, 4294967288LL, 4294967312LL,
           "late previous-cycle timestamp is not mistaken for another forward wrap");
    expect(tracker, 0x20U, 4294967328LL, 4294967328LL,
           "late packet does not move the next forward timestamp to an older cycle");
    expect(tracker, 0xfffffff8U, 4294967288LL, 4294967328LL,
           "duplicate late packet remains in its original nearby cycle");

    tracker.reset();
    expect(tracker, 0, 0, 0, "zero establishes the initial reference");
    expect(tracker, 0xffffffc0U, -64, 0,
           "timestamp just before an initial zero can be negative");
    expect(tracker, 0x20U, 32, 32, "negative late timestamp does not lower the zero high-water mark");

    // 累计前进跨过多个周期，单次距离均小于半周期；期望值直接来自已知
    // 的 64 位采样时间，而非重用被测工具的周期选择公式。
    tracker.reset();
    int64_t absolute = 4294967000LL;
    expect(tracker, static_cast<uint32_t>(absolute), absolute, absolute, "multi-cycle initial timestamp");
    for (int i = 0; i < 12; ++i) {
        absolute += 0x70000000LL;
        expect(tracker, static_cast<uint32_t>(absolute), absolute, absolute,
               "forward timestamps retain more than one wrap cycle");
        expect(tracker, static_cast<uint32_t>(absolute - 1000), absolute - 1000, absolute,
               "late timestamp retains its nearest cycle after several wraps");
    }
}

void presentation_reordering() {
    // 已知呈现时间为 0,4500,1500,3000,6000 ticks，但按解码顺序到达；
    // 同时跨过 32 位回绕。不能通过钳制或排序将这些时间戳改成单调输出。
    constexpr int64_t base = 4294964296LL;
    constexpr std::array<int64_t, 5> presentation = {0, 4500, 1500, 3000, 6000};
    RtpTimestamp tracker;
    int64_t high = base;
    for (const auto pts : presentation) {
        if (base + pts > high) high = base + pts;
        expect(tracker, static_cast<uint32_t>(base + pts), base + pts, high,
               "decode-order input preserves original presentation timestamps across a wrap");
    }
}

void half_cycle_ambiguity() {
    RtpTimestamp tracker;
    expect(tracker, 100, 100, 100, "ambiguity test establishes a nonzero reference");
    check(!tracker.observe(0x80000064U), "exact half-cycle timestamp is rejected");
    check(tracker.high() == 100, "half-cycle rejection does not change the high-water reference");
    expect(tracker, 101, 101, 101, "valid timestamp is accepted after ambiguous input");

    tracker.reset();
    expect(tracker, 0xfffffff0U, 4294967280LL, 4294967280LL,
           "second ambiguity case starts near a wrap");
    expect(tracker, 16, 4294967312LL, 4294967312LL, "second ambiguity case crosses the wrap");
    expect(tracker, 0xfffffff8U, 4294967288LL, 4294967312LL,
           "second ambiguity case receives a late timestamp");
    check(!tracker.observe(0x80000010U), "ambiguity is compared with the high-water, not the last late value");
    check(tracker.high() == 4294967312LL, "late and ambiguous input leave the newer high-water unchanged");
    expect(tracker, 17, 4294967313LL, 4294967313LL,
           "next timestamp uses the same cycle after ambiguity");

    check(RtpTimestamp::nearest(0x7fffffffU, 0) == 2147483647LL,
          "one tick short of the positive half-cycle is unambiguous");
    check(!RtpTimestamp::nearest(0x80000000U, 0), "pure expansion rejects exact half-cycle");
    check(RtpTimestamp::nearest(0x80000001U, 0) == -2147483647LL,
          "one tick beyond positive half-cycle selects the nearer negative value");
}

void signed_64_boundaries() {
    constexpr int64_t maximum = std::numeric_limits<int64_t>::max();
    constexpr int64_t minimum = std::numeric_limits<int64_t>::min();
    check(RtpTimestamp::nearest(0xffffffffU, maximum) == maximum,
          "INT64_MAX duplicate is representable");
    check(RtpTimestamp::nearest(0xfffffffeU, maximum) == maximum - 1,
          "timestamp one tick before INT64_MAX is representable");
    check(!RtpTimestamp::nearest(0, maximum), "forward result beyond INT64_MAX is rejected");
    check(RtpTimestamp::nearest(0, minimum) == minimum, "INT64_MIN duplicate is representable");
    check(RtpTimestamp::nearest(1, minimum) == minimum + 1,
          "timestamp one tick after INT64_MIN is representable");
    check(!RtpTimestamp::nearest(0xffffffffU, minimum),
          "backward result below INT64_MIN is rejected");
    check(!RtpTimestamp::nearest(0x7fffffffU, maximum),
          "half-cycle remains ambiguous at INT64_MAX");
    check(!RtpTimestamp::nearest(0x80000000U, minimum),
          "half-cycle remains ambiguous at INT64_MIN");
    check(RtpTimestamp::nearest(0, -1) == 0, "negative reference can advance through signed zero");
    check(RtpTimestamp::nearest(0xffffffffU, 0) == -1,
          "unsigned raw timestamp expands to the nearest negative signed value");
    check(RtpTimestamp::nearest(0x80000000U, -2147483648LL) == -2147483648LL,
          "negative reference low bits are interpreted modulo 2^32");
}

void known_absolute_timeline() {
    // 先生成真实的 64 位时间线，再截去高位模拟线上 RTP；穿插较早的采样
    // 和重复包。该期望来自生成的时间线，不采用 nearest() 的实现公式。
    std::mt19937 generator(0x61727470U);
    RtpTimestamp tracker;
    int64_t high = 4294967000LL;
    expect(tracker, static_cast<uint32_t>(high), high, high, "generated timeline establishes its reference");
    for (int i = 0; i < 1024; ++i) {
        high += 1 + (generator() & 0x3fffffffU);
        expect(tracker, static_cast<uint32_t>(high), high, high,
               "generated forward timeline retains all higher wrap cycles");
        const int64_t earlier = high - (generator() & 0x0fffffffU);
        expect(tracker, static_cast<uint32_t>(earlier), earlier, high,
               "generated out-of-order timestamps retain their original values");
        expect(tracker, static_cast<uint32_t>(high), high, high,
               "generated duplicate does not change the reference");
    }
    check(high > 100LL * (int64_t{1} << 32), "generated timeline exercises more than 100 wrap cycles");
}
}  // namespace

int main() {
    initial_and_reset();
    wraps_and_late_packets();
    presentation_reordering();
    half_cycle_ambiguity();
    signed_64_boundaries();
    known_absolute_timeline();
    std::printf("RTP timestamp: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
