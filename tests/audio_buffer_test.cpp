#include "media/AudioPump.h"

#include <algorithm>
#include <cstdio>
#include <limits>

namespace {
int checks = 0;
int failures = 0;

void check(bool condition, const char *message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", message);
    }
}

using Pump = scrctl::media::AudioPump;

struct Counts {
    std::size_t startup_trim = 0;
    std::size_t runtime_trim = 0;
    std::size_t runtime_silence = 0;
};

Counts perfect_clocks(std::size_t initial_backlog) {
    // 同一采样时钟：生产方每 480 帧交付一包，输出方每 1024 帧读取一次。
    // 二者节拍不同，但速率相同；不应被当成漂移而持续裁掉音频。
    std::size_t buffered = initial_backlog;
    std::size_t next_packet = 0;
    bool started = false;
    Counts result;
    for (std::size_t clock = 0; clock < 48000 * 30; clock += 1024) {
        while (next_packet <= clock) {
            buffered += 480;
            next_packet += 480;
        }
        if (!started && buffered < 2400) {
            continue;
        }
        const auto trim = Pump::compute_read_trim(buffered, 1024, 2400, 480, !started);
        buffered -= trim.frames;
        (trim.startup ? result.startup_trim : result.runtime_trim) += trim.frames;
        const auto got = std::min<std::size_t>(buffered, 1024);
        buffered -= got;
        result.runtime_silence += 1024 - got;
        started |= got != 0;
    }
    return result;
}
} // namespace

int main() {
    check(Pump::compute_read_trim(2400 + 1024, 1024, 2400, 480, false).frames == 0,
          "the callback's own frames are not extra backlog");
    check(Pump::compute_read_trim(5000, 1024, 2400, 480, false).frames == 0,
          "ordinary callback and packet phase difference preserves PCM");
    check(Pump::compute_read_trim(6304, 1024, 2400, 480, false).frames == 0,
          "runtime trim has a full packet of headroom beyond twice the target");
    const auto runtime = Pump::compute_read_trim(6305, 1024, 2400, 480, false);
    check(runtime.frames == 2881 && !runtime.startup && 6305 - runtime.frames - 1024 == 2400,
          "excessive runtime backlog is trimmed while retaining the output and target");
    const auto startup = Pump::compute_read_trim(12000, 1024, 2400, 480, true);
    check(startup.frames == 8576 && startup.startup && 12000 - startup.frames - 1024 == 2400,
          "startup trim is separate and leaves the requested reserve after output");
    check(Pump::compute_read_trim(2400, 1024, 2400, 480, true).frames == 0,
          "normal preroll does not trim samples at startup");
    check(Pump::compute_read_trim(500, 1024, 2400, 480, false).frames == 0,
          "underflow does not underflow the trim calculation");
    check(Pump::compute_read_trim(0, 1024, 0, 480, true).frames == 0,
          "empty buffer cannot create trim");
    check(Pump::compute_read_trim(12000, 0, 2400, 480, true).frames == 0,
          "zero-length read does not alter playback");
    check(Pump::compute_read_trim(6304, 1024, 2400, 0, false).frames == 0,
          "missing frame length retains a conservative packet headroom");
    check(Pump::compute_read_trim(4096, 1024, 0, 480, true).frames == 3072,
          "zero target retains the requested output when trimming startup backlog");
    check(Pump::compute_read_trim(std::numeric_limits<std::size_t>::max(), 1,
                                  std::numeric_limits<std::size_t>::max() / 2 + 1,
                                  480, false).frames == 0,
          "large target does not overflow the runtime threshold");
    const auto normal = perfect_clocks(0);
    check(normal.startup_trim == 0 && normal.runtime_trim == 0 && normal.runtime_silence == 0,
          "perfect equal-rate clocks preserve 30 seconds without trim or underflow");
    const auto late_start = perfect_clocks(12000);
    check(late_start.startup_trim > 0 && late_start.runtime_trim == 0 &&
              late_start.runtime_silence == 0,
          "late consumer startup is classified once and does not create repeated drift trim");
    std::printf("audio_buffer: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
