#include "media/AudioRegulator.h"

#include "i18n/Translation.h"

#include <algorithm>
#include <cmath>
#include <utility>

#if defined(SCRCTL_HAVE_LIBAV)
extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/version.h>
#include <libswresample/swresample.h>
}
#endif

namespace scrctl::media {
namespace {
#if defined(SCRCTL_HAVE_LIBAV)
constexpr int kMaxSampleRate = 384000;
constexpr int kMaxChannels = 8;
constexpr double kAverageSeconds = 1.28;
constexpr int kCompensationSeconds = 4;
constexpr int kMaxCompensationPercent = 2;

std::string av_error(int status) {
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(status, text, sizeof(text));
    return text;
}
#endif
} // namespace

struct AudioRegulator::Impl {
    int sample_rate;
    int channels;
    std::size_t target;
    Stats stats;
    bool averaging = false;
    std::size_t interval_frames = 0;
    std::size_t pending_input = 0;
    std::int64_t pending_delta = 0;
#if defined(SCRCTL_HAVE_LIBAV)
    struct SwrContext *swr = nullptr;

    ~Impl() { swr_free(&swr); }

    bool initialize(std::string &err) {
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 24, 100)
        AVChannelLayout layout{};
        av_channel_layout_default(&layout, channels);
        int status = swr_alloc_set_opts2(&swr, &layout, AV_SAMPLE_FMT_S16, sample_rate,
                                         &layout, AV_SAMPLE_FMT_S16, sample_rate, 0, nullptr);
        av_channel_layout_uninit(&layout);
#else
        const auto layout = av_get_default_channel_layout(channels);
        swr = swr_alloc_set_opts(nullptr, layout, AV_SAMPLE_FMT_S16, sample_rate,
                                 layout, AV_SAMPLE_FMT_S16, sample_rate, 0, nullptr);
        int status = swr != nullptr ? 0 : AVERROR(ENOMEM);
#endif
        // 从第一包起就启用重采样。若仅做同采样率格式转换，首次补偿可能
        // 隐式 swr_init，清掉滤波状态；提前设置可保持 PCM 连续。
        if (status >= 0) {
            status = av_opt_set_int(swr, "flags", SWR_FLAG_RESAMPLE, 0);
        }
        if (status >= 0) {
            status = swr_init(swr);
        }
        if (status < 0) {
            err = SCRCTL_TR("Failed to initialize audio clock resampling: ") + av_error(status);
            swr_free(&swr);
            stats.active = false;
            return false;
        }
        stats.active = true;
        return true;
    }
#endif

    Impl(int rate, int count, std::size_t waterline)
        : sample_rate(rate), channels(count), target(waterline) {}

    void clear() {
#if defined(SCRCTL_HAVE_LIBAV)
        swr_free(&swr);
#endif
        averaging = false;
        interval_frames = 0;
        pending_input = 0;
        pending_delta = 0;
        stats.active = false;
        stats.average_frames = 0;
        stats.compensation_ppm = 0;
    }
};

AudioRegulator::AudioRegulator(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
AudioRegulator::~AudioRegulator() = default;

std::unique_ptr<AudioRegulator> AudioRegulator::create(int sample_rate, int channels,
                                                     std::size_t target_frames,
                                                     std::string &err) {
    err.clear();
#if !defined(SCRCTL_HAVE_LIBAV)
    (void) sample_rate;
    (void) channels;
    (void) target_frames;
    return nullptr;
#else
    if (target_frames == 0) {
        return nullptr;
    }
    if (sample_rate <= 0 || sample_rate > kMaxSampleRate || channels <= 0 ||
        channels > kMaxChannels || target_frames > static_cast<std::size_t>(sample_rate)) {
        err = SCRCTL_TR("Audio clock resampling requires a valid PCM format and at most one second of buffering");
        return nullptr;
    }
    auto impl = std::make_unique<Impl>(sample_rate, channels, target_frames);
    if (!impl->initialize(err)) {
        return nullptr;
    }
    return std::unique_ptr<AudioRegulator>(new AudioRegulator(std::move(impl)));
#endif
}

bool AudioRegulator::process(std::span<const int16_t> input, std::vector<int16_t> &output,
                             std::string &err) {
    err.clear();
    if (input.empty()) {
        output.clear();
        return true;
    }
#if !defined(SCRCTL_HAVE_LIBAV)
    (void) output;
    err = SCRCTL_TR("Audio clock resampling is unavailable in this build");
    return false;
#else
    auto &p = *impl_;
    const auto channels = static_cast<std::size_t>(p.channels);
    const auto frames = input.size() / channels;
    if (input.size() % channels != 0 || frames > static_cast<std::size_t>(p.sample_rate)) {
        err = SCRCTL_TR("Audio clock resampling received an invalid PCM block");
        p.clear();
        return false;
    }
    if (p.swr == nullptr && !p.initialize(err)) {
        return false;
    }
    // 上次 observe 可能改变补偿；必须在其后重新查询容量，不能缓存旧上限。
    const int capacity = swr_get_out_samples(p.swr, static_cast<int>(frames));
    const int max_capacity = 2 * p.sample_rate + 4096;
    if (capacity < 0 || capacity > max_capacity ||
        static_cast<std::size_t>(capacity) > output.max_size() / channels) {
        err = SCRCTL_TR("Audio clock resampling returned an invalid sample count");
        p.clear();
        return false;
    }
    std::vector<int16_t> converted(static_cast<std::size_t>(capacity) * channels);
    uint8_t *dst = reinterpret_cast<uint8_t *>(converted.data());
    const uint8_t *src = reinterpret_cast<const uint8_t *>(input.data());
    const int count = swr_convert(p.swr, &dst, capacity, &src, static_cast<int>(frames));
    if (count < 0 || count > capacity) {
        err = SCRCTL_TR("Audio clock resampling failed: ") +
              (count < 0 ? av_error(count) : SCRCTL_TR("invalid sample count"));
        p.clear();
        return false;
    }
    converted.resize(static_cast<std::size_t>(count) * channels);
    output.swap(converted);
    p.pending_input += frames;
    const auto delta = static_cast<std::int64_t>(count) - static_cast<std::int64_t>(frames);
    p.pending_delta += delta;
    if (delta > 0) {
        p.stats.added_frames += static_cast<std::uint64_t>(delta);
    } else {
        p.stats.removed_frames += static_cast<std::uint64_t>(-delta);
    }
    return true;
#endif
}

bool AudioRegulator::observe(const Observation &observation, std::string &err) {
    err.clear();
#if !defined(SCRCTL_HAVE_LIBAV)
    (void) observation;
    return true;
#else
    auto &p = *impl_;
    const auto input = std::exchange(p.pending_input, 0);
    const auto delta = std::exchange(p.pending_delta, 0);
    if (!observation.playing || input == 0 || p.swr == nullptr) {
        // 消费端还没开始时不估计时钟偏差，也不把 startup trim 算成漂移。
        p.averaging = false;
        p.interval_frames = 0;
        p.stats.average_frames = static_cast<double>(observation.buffered_frames);
        return true;
    }
    const double buffered = static_cast<double>(observation.buffered_frames);
    if (!p.averaging) {
        p.stats.average_frames = buffered;
        p.averaging = true;
    } else {
        const double changed = static_cast<double>(delta) +
                               static_cast<double>(observation.inserted_silence) -
                               static_cast<double>(observation.discarded_frames);
        // 重采样、补零和明确丢弃改变了播放时间线，立即修正估计；只有自然
        // 收包/消费节拍造成的水位变化才平滑。按输入时长加权，避免帧长影响均值。
        double mean = std::max(0.0, p.stats.average_frames + changed);
        const double weight = std::min(1.0, static_cast<double>(input) /
                                           (p.sample_rate * kAverageSeconds));
        p.stats.average_frames = mean + weight * (buffered - mean);
    }
    p.interval_frames += input;
    if (p.interval_frames < static_cast<std::size_t>(p.sample_rate)) {
        return true;
    }
    p.interval_frames %= static_cast<std::size_t>(p.sample_rate);
    double difference = static_cast<double>(p.target) - p.stats.average_frames;
    const double deadzone = p.sample_rate * (p.stats.compensation_ppm != 0 ? 0.001 : 0.004);
    if (std::abs(difference) < deadzone ||
        (difference < 0 && observation.buffered_frames < p.target)) {
        difference = 0;
    }
    const int distance = p.sample_rate * kCompensationSeconds;
    const int limit = distance * kMaxCompensationPercent / 100;
    const int correction = static_cast<int>(std::clamp(difference, -static_cast<double>(limit),
                                                      static_cast<double>(limit)));
    const int status = swr_set_compensation(p.swr, correction, distance);
    if (status < 0) {
        err = SCRCTL_TR("Failed to adjust the audio clock: ") + av_error(status);
        p.clear();
        return false;
    }
    p.stats.compensation_ppm = static_cast<int>(
        static_cast<std::int64_t>(correction) * 1000000 / distance);
    ++p.stats.compensation_updates;
    return true;
#endif
}

void AudioRegulator::reset() { impl_->clear(); }
AudioRegulator::Stats AudioRegulator::stats() const { return impl_->stats; }

} // namespace scrctl::media
