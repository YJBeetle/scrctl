#include "media/AudioRegulator.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace {
using Regulator = scrctl::media::AudioRegulator;
constexpr int kRate = 48000;
constexpr int kChannels = 2;
constexpr int kPacket = 480;
constexpr int kCallback = 1024;
constexpr int kTarget = 2400;
constexpr double kPi = 3.14159265358979323846;
int checks = 0;
int failures = 0;

void check(bool condition, const char *message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", message);
    }
}

std::vector<int16_t> wave(std::size_t first, std::size_t frames) {
    std::vector<int16_t> pcm(frames * kChannels);
    for (std::size_t i = 0; i < frames; ++i) {
        const double time = static_cast<double>(first + i) / kRate;
        pcm[i * 2] = static_cast<int16_t>(16000 * std::sin(2 * kPi * 440 * time));
        pcm[i * 2 + 1] = static_cast<int16_t>(12000 * std::sin(2 * kPi * 660 * time + 0.3));
    }
    return pcm;
}

struct Simulation {
    std::size_t underrun_frames = 0;
    std::size_t trimmed_frames = 0;
    std::size_t peak_queue = 0;
    std::size_t tail_low = std::numeric_limits<std::size_t>::max();
    std::size_t tail_high = 0;
    int largest_step = 0;
    std::size_t sample_comparisons = 0;
    Regulator::Stats clock;
    bool ok = true;
};

Simulation simulate(double seconds, int input_ppm, bool burst, bool regulated,
                    bool legacy_trim = false, bool late_release = false) {
    Simulation result;
    std::string error;
    auto clock = regulated ? Regulator::create(kRate, kChannels, kTarget, error) : nullptr;
    if (regulated && !clock) {
        result.ok = false;
        return result;
    }
    std::deque<int16_t> queue;
    std::size_t packet_index = 0;
    std::size_t callback_index = 0;
    bool playing = false;
    bool have_last = false;
    int16_t last[2]{};
    std::uint64_t pending_silence = 0;
    std::uint64_t pending_discard = 0;
    const double input_rate = kRate * (1.0 + input_ppm / 1000000.0);
    auto arrival = [=](std::size_t index) {
        const double normal = index * kPacket / input_rate;
        // 设备已捕获的 140ms PCM 一起到达，之后恢复正常交付节拍。
        // 这描述网络积压释放，不能把突发中的最后几包当成多余旧音频。
        if (normal >= 5.0 && normal < 5.14) {
            if (burst) return 5.0;
            if (late_release) return 5.14;
        }
        return normal;
    };
    for (;;) {
        const double input_time = arrival(packet_index);
        const double output_time = callback_index * static_cast<double>(kCallback) / kRate;
        if (std::min(input_time, output_time) >= seconds) {
            break;
        }
        if (input_time <= output_time) {
            auto input = wave(packet_index * kPacket, kPacket);
            ++packet_index;
            std::vector<int16_t> output;
            if (clock) {
                if (!clock->process(input, output, error)) {
                    result.ok = false;
                    break;
                }
            } else {
                output = std::move(input);
            }
            queue.insert(queue.end(), output.begin(), output.end());
            result.peak_queue = std::max(result.peak_queue, queue.size() / 2);
            if (clock) {
                const Regulator::Observation observation {
                    queue.size() / 2, playing, pending_silence, pending_discard,
                };
                pending_silence = pending_discard = 0;
                if (!clock->observe(observation, error)) {
                    result.ok = false;
                    break;
                }
            }
        } else {
            ++callback_index;
            if (!playing && queue.size() / 2 < kTarget) {
                continue;
            }
            playing = true;
            // 已提交的窄修复仍会对超过约 2×target 的运行积压硬裁。
            // 此对照模型检查 burst 后的实际欠载和波形，不能只检查计数公式。
            const auto buffered = queue.size() / 2;
            if (legacy_trim && buffered > kCallback + 2 * kTarget + kPacket) {
                const auto drop = buffered - kCallback - kTarget;
                queue.erase(queue.begin(), queue.begin() + static_cast<std::ptrdiff_t>(drop * 2));
                result.trimmed_frames += drop;
                pending_discard += drop;
            }
            const auto take = std::min<std::size_t>(queue.size() / 2, kCallback);
            for (std::size_t frame = 0; frame < take; ++frame) {
                for (int channel = 0; channel < 2; ++channel) {
                    const auto current = queue.front();
                    queue.pop_front();
                    if (have_last) {
                        result.largest_step = std::max(result.largest_step,
                                                       std::abs(current - last[channel]));
                        ++result.sample_comparisons;
                    }
                    last[channel] = current;
                }
                have_last = true;
            }
            result.underrun_frames += kCallback - take;
            pending_silence += kCallback - take;
            if (take < kCallback) {
                have_last = false;
            }
            if (output_time >= seconds / 2) {
                result.tail_low = std::min(result.tail_low, queue.size() / 2);
                result.tail_high = std::max(result.tail_high, queue.size() / 2);
            }
        }
    }
    if (!error.empty()) {
        std::printf("simulation error: %s\n", error.c_str());
    }
    if (clock) {
        result.clock = clock->stats();
    }
    std::printf("simulation: %.0fs input=%+dppm burst=%d late=%d clock=%d trim=%zu underrun=%zu "
                "tail=%zu..%zu mean=%.1f compensation=%dppm max-step=%d\n",
                 seconds, input_ppm, burst, late_release, regulated, result.trimmed_frames,
                 result.underrun_frames, result.tail_low, result.tail_high,
                 result.clock.average_frames, result.clock.compensation_ppm, result.largest_step);
    return result;
}
} // namespace

int main() {
    std::string error = "old error";
    check(!Regulator::create(kRate, 2, 0, error) && error.empty(),
          "zero buffer selects direct playback without an error");
    check(!Regulator::create(0, 2, kTarget, error) && !error.empty(),
          "invalid sample rate reports an initialization error");
    check(!Regulator::create(kRate, 99, kTarget, error) && !error.empty(),
          "unsupported channel count is bounded before calling the library");
    check(!Regulator::create(kRate, 2, kRate + 1, error) && !error.empty(),
          "excessive latency is rejected without an oversized allocation");

    auto regulator = Regulator::create(kRate, 2, kTarget, error);
    check(regulator && error.empty() && regulator->stats().active,
          "the real resampler initializes for stereo PCM");
    if (!regulator) {
        return 1;
    }
    std::vector<int16_t> warmup;
    check(regulator->process(wave(12000, kPacket), warmup, error),
          "a previous PCM block primes the real resampling filter");
    std::vector<int16_t> output {123, 456};
    const std::vector<int16_t> odd {1, 2, 3};
    check(!regulator->process(odd, output, error) && !error.empty() &&
              output == std::vector<int16_t>({123, 456}) && !regulator->stats().active,
          "partial stereo input reports an error without publishing partial PCM");
    const auto oversized = wave(0, kRate + 1);
    check(!regulator->process(oversized, output, error) && !error.empty() &&
              output == std::vector<int16_t>({123, 456}),
          "oversized PCM preserves caller output on conversion rejection");
    const auto first = wave(0, kPacket);
    check(regulator->process(first, output, error) && error.empty() &&
              output.size() / 2 > 440 && output.size() / 2 <= kPacket,
          "valid conversion continues after rejected blocks using real PCM");
    auto after_error_reference = Regulator::create(kRate, 2, kTarget, error);
    std::vector<int16_t> after_error_pcm;
    check(after_error_reference && after_error_reference->process(first, after_error_pcm, error) &&
              output == after_error_pcm,
          "automatic recovery discards old filter history and matches a fresh converter");
    check(regulator->process({}, output, error) && error.empty() && output.empty(),
          "empty input does not flush delayed samples into playback");

    // reset 必须真实释放并重建滤波器；新会话首包应与全新实例逐点一致，
    // 而不能仍带旧会话的滤波历史或补偿比例。
    regulator->reset();
    auto fresh = Regulator::create(kRate, 2, kTarget, error);
    std::vector<int16_t> rebuilt;
    std::vector<int16_t> reference;
    const auto new_session = wave(24000, kPacket);
    check(!regulator->stats().active && regulator->stats().compensation_ppm == 0,
          "reset disables the old converter and its estimated compensation");
    check(regulator->process(new_session, rebuilt, error) && fresh &&
              fresh->process(new_session, reference, error) && rebuilt == reference &&
              regulator->stats().active,
          "rebuilt filter produces the same PCM as a fresh session");

    const auto normal = simulate(30, 0, false, true);
    check(normal.ok && normal.underrun_frames == 0 && normal.trimmed_frames == 0,
          "equal clocks play continuously across unequal packet and callback sizes");
    check(normal.sample_comparisons > 1000000 && normal.largest_step < 2000,
          "converted sine waves have continuous PCM across real resampling boundaries");
    check(std::abs(normal.clock.average_frames - kTarget) < 300,
          "equal clocks converge near the configured mean waterline");

    const auto old_burst = simulate(15, 0, true, false, true);
    const auto smooth_burst = simulate(15, 0, true, true);
    check(old_burst.trimmed_frames > 0 && old_burst.underrun_frames > 0,
          "a short backlog release reproduces hard trim followed by missing audio");
    check(smooth_burst.ok && smooth_burst.trimmed_frames == 0 &&
              smooth_burst.underrun_frames == 0 && smooth_burst.peak_queue < kRate / 2,
          "the same burst preserves playback and fits the existing bounded ring");
    check(smooth_burst.largest_step < 2000,
          "burst recovery does not cut the middle of the PCM waveform");

    const auto delayed = simulate(30, 0, false, true, false, true);
    check(delayed.ok && delayed.underrun_frames > 0 && delayed.underrun_frames < 6000 &&
              delayed.trimmed_frames == 0 && delayed.tail_low > 0,
          "a gap longer than the target inserts finite silence then resumes continuous playback");
    check(std::abs(delayed.clock.average_frames - kTarget) < 300 && delayed.tail_high < 4000,
          "silence accounting recovers the fixed target without permanently growing latency");

    const auto fast_raw = simulate(120, 2000, false, false);
    const auto slow_raw = simulate(120, -2000, false, false);
    const auto fast = simulate(120, 2000, false, true);
    const auto slow = simulate(120, -2000, false, true);
    check(fast_raw.tail_high > 10000 && slow_raw.underrun_frames > 0,
          "uncompensated independent clocks accumulate latency or exhaust the buffer");
    check(fast.ok && slow.ok && fast.underrun_frames == 0 && slow.underrun_frames == 0 &&
              fast.tail_high < 5000 && slow.tail_high < 5000,
          "positive and negative clock offsets stay bounded without audio gaps");
    check(fast.clock.compensation_ppm < -1000 && slow.clock.compensation_ppm > 1000 &&
              std::abs(fast.clock.compensation_ppm) <= 20000 &&
              std::abs(slow.clock.compensation_ppm) <= 20000,
          "library compensation follows both clock directions with a bounded rate");
    check(fast.largest_step < 2000 && slow.largest_step < 2000,
          "clock correction changes duration smoothly rather than skipping sample blocks");
    std::printf("audio_regulator: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
