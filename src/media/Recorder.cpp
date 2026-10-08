#include "media/Recorder.h"

#include "media/RecordingClock.h"
#include "media/RecordingMuxer.h"
#include "media/RecordingVideoConfig.h"
#include "rt/RtpTimestamp.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

namespace scrctl::media {
namespace {
using Time = std::chrono::steady_clock;
using Session = std::array<uint8_t, 16>;
constexpr std::size_t max_ingress_events = 1024;
constexpr std::size_t max_media_events = 8192;
constexpr std::size_t max_early_reports = 8;
constexpr std::size_t recent_clock_anchors = 7;
constexpr std::size_t max_encoded_budget = 16u * 1024u * 1024u;

std::size_t index(Recorder::Track track) { return track == Recorder::Track::Video ? 0 : 1; }
bool valid_track(Recorder::Track track) {
    return track == Recorder::Track::Video || track == Recorder::Track::Audio;
}
uint64_t ntp(const rt::SenderReport& report) {
    return (uint64_t{report.ntp_seconds} << 32) | report.ntp_fraction;
}
bool same_clock(const rt::SenderReport& a, const rt::SenderReport& b) {
    return a.ssrc == b.ssrc && ntp(a) == ntp(b) && a.rtp_timestamp == b.rtp_timestamp;
}
}  // namespace

struct Recorder::Impl {
    enum class Kind { Begin, Video, Audio, Report };
    struct Event {
        Kind kind = Kind::Begin;
        Track track = Track::Video;
        Session session{};
        std::optional<uint32_t> source;
        int64_t ticks = 0;
        uint32_t timestamp = 0;
        std::vector<uint8_t> bytes;
        Nal vps, sps, pps;
        rt::SenderReport report;
        Time::time_point received;
        std::size_t cost = 0;
        bool media_owned = false;
        bool vcl = false, idr = false, keyframe = false;
    };
    struct Packet {
        std::vector<uint8_t> bytes;
        int64_t ticks = 0, end_ticks = 0;
        bool keyframe = false;
        Time::time_point received;
        std::size_t cost = 0;
    };
    struct Stream {
        bool begun = false;
        Session session{};
        std::optional<uint32_t> source;
        std::optional<int64_t> media_high, last_media, report_high;
        rt::RtpTimestamp timestamp;
        std::deque<rt::SenderReport> early_reports;
        std::unique_ptr<RecordingClock> clock;
        std::deque<Packet> pending;
        bool wrote_media = false;
    };

    Options options;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::deque<Event> ingress;
    std::size_t owned_bytes = 0, media_events = 0;
    bool sealed = false, completed = false;
    std::string first_error;
    std::thread worker;

    // 以下状态只由 worker 使用；finish 只在 join 后读错误结果。
    std::array<Stream, 2> streams;
    std::optional<uint64_t> common_reference;
    std::optional<long double> origin;
    std::optional<RecordingVideoConfig> video_config;
    std::size_t configuration_cost = 0;
    std::unique_ptr<RecordingMuxer> muxer;
    Time::time_point startup_deadline;

    explicit Impl(Options value)
        : options(std::move(value)), startup_deadline(Time::now() + options.clock_wait) {}

    bool failed() const {
        std::lock_guard lock(mutex);
        return !first_error.empty();
    }
    void fail(std::string_view reason) {
        {
            std::lock_guard lock(mutex);
            if (completed) return;
            if (first_error.empty()) first_error = reason.empty() ? "Recording failed" : reason;
            sealed = true;
        }
        changed.notify_all();
    }
    void fail_locked(std::string_view reason) {
        if (first_error.empty()) first_error = reason;
        sealed = true;
        changed.notify_all();
    }
    void release(std::size_t bytes, bool media_complete) {
        std::lock_guard lock(mutex);
        owned_bytes -= bytes;
        if (media_complete) --media_events;
    }

    template<class Copy>
    bool admit(std::size_t cost, bool media, Copy copy) {
        std::lock_guard lock(mutex);
        if (sealed || !first_error.empty()) return false;
        if (cost > options.encoded_budget - owned_bytes ||
            ingress.size() >= max_ingress_events || (media && media_events >= max_media_events)) {
            fail_locked("Recorder input exceeds its encoded-byte or event budget");
            return false;
        }
        // 先预留再复制，两个生产者不能同时持有未计入预算的大编码副本。
        owned_bytes += cost;
        if (media) ++media_events;
        try {
            Event event;
            event.received = Time::now();
            event.cost = cost;
            event.media_owned = media;
            copy(event);
            ingress.push_back(std::move(event));
        } catch (...) {
            owned_bytes -= cost;
            if (media) --media_events;
            fail_locked("Cannot allocate the Recorder input buffer");
            return false;
        }
        changed.notify_one();
        return true;
    }

    bool session_valid(Stream& stream, const Event& event) {
        if (!stream.begun || stream.session != event.session) {
            fail("Recording media belongs to a missing or changed session");
            return false;
        }
        return true;
    }

    void prune(Stream& stream) {
        if (!stream.clock || !origin) return;
        // FU/AU 的完成可晚于同源 SR；空 pending 不代表之后不会收到较早采样。
        // 留最近七个锚点，为新 SR 在八点预算内留一槽。pending 仍保护所需左点；
        // 这是有界历史，不承诺任意延迟或固定秒数的样本都能被恢复。
        if (!stream.pending.empty())
            stream.clock->discard_before(stream.pending.front().ticks, recent_clock_anchors);
        else if (stream.report_high)
            stream.clock->discard_before(*stream.report_high, recent_clock_anchors);
    }

    void accept_report(Stream& stream, const rt::SenderReport& report) {
        if (!stream.source || report.ssrc != *stream.source) return;
        if (!stream.media_high) {
            const auto duplicate = std::any_of(stream.early_reports.begin(), stream.early_reports.end(),
                [&](const auto& known) { return same_clock(known, report); });
            if (duplicate) return;
            if (stream.early_reports.size() >= max_early_reports) {
                fail("Recording early sender-report budget exceeded");
                return;
            }
            stream.early_reports.push_back(report);
            return;
        }
        const int64_t reference = stream.report_high
            ? std::max(*stream.media_high, *stream.report_high) : *stream.media_high;
        const auto ticks = rt::RtpTimestamp::nearest(report.rtp_timestamp, reference);
        if (ntp(report) == 0 || !ticks) {
            fail("Recording sender report has no usable NTP/RTP clock");
            return;
        }
        if (!common_reference) common_reference = ntp(report);
        if (!stream.clock) {
            RecordingClock::Limits limits;
            limits.boundary_extrapolation = options.final_extrapolation;
            stream.clock = std::make_unique<RecordingClock>(*stream.source, *common_reference, limits);
        }
        prune(stream);
        const auto result = stream.clock->add_report(report, reference);
        if (result == RecordingClock::ReportResult::Error) {
            fail(stream.clock->error());
            return;
        }
        if (result == RecordingClock::ReportResult::Accepted) stream.report_high = *ticks;
    }

    bool bind_media(Stream& stream, Event& event, int64_t ticks) {
        if (!session_valid(stream, event)) return false;
        if (!stream.source) stream.source = event.source;
        if (stream.source != event.source) {
            fail("Recording media source changed within the session");
            return false;
        }
        if (stream.last_media && ticks <= *stream.last_media) {
            fail("Recording media timestamps do not advance within the track");
            return false;
        }
        stream.last_media = ticks;
        if (!stream.media_high || ticks > *stream.media_high) stream.media_high = ticks;
        // 初媒体完成来源绑定后才允许早到 SR 进入时钟；不凭 SR 选设备来源。
        auto reports = std::move(stream.early_reports);
        stream.early_reports.clear();
        for (const auto& report : reports) {
            accept_report(stream, report);
            if (failed()) return false;
        }
        return true;
    }

    void process_video(Event& event) {
        auto& stream = streams[0];
        if (!session_valid(stream, event)) return;
        if (stream.source && stream.source != event.source) {
            fail("Recording media source changed within the session"); return;
        }
        if (video_config && (event.vps != video_config->vps || event.sps != video_config->sps ||
                             event.pps != video_config->pps)) {
            fail("Recording HEVC parameter sets changed"); return;
        }
        if (!event.vcl) return;
        if (!bind_media(stream, event, event.ticks)) return;
        if (!video_config && !event.idr) return;
        const bool first = !video_config;
        std::optional<RecordingVideoConfig> checked;
        if (first) {
            checked = inspect_recording_video_config(event.vps, event.sps, event.pps);
            if (!checked->permits_equal_dts_pts()) {
                fail(checked->error.empty() ? "Recording HEVC requires a no-reorder configuration" : checked->error);
                return;
            }
        }
        const std::size_t packet_cost = event.bytes.size() * 2;
        stream.pending.push_back({std::move(event.bytes), event.ticks, event.ticks,
                                  event.keyframe, event.received, packet_cost});
        event.cost -= packet_cost;
        event.media_owned = false;
        if (first) {
            configuration_cost = (event.vps.size() + event.sps.size() + event.pps.size()) * 3;
            video_config = std::move(checked);
            event.cost -= configuration_cost;
        }
    }

    void process(Event& event) {
        auto& stream = streams[index(event.track)];
        switch (event.kind) {
            case Kind::Begin:
                if (stream.begun) { fail("Recording track session was started again"); return; }
                stream.begun = true;
                stream.session = event.session;
                stream.source = event.source;
                break;
            case Kind::Video: process_video(event); break;
            case Kind::Audio: {
                if (!session_valid(stream, event)) return;
                const auto ticks = stream.timestamp.observe(event.timestamp);
                if (!ticks || *ticks > std::numeric_limits<int64_t>::max() - 480) {
                    fail("Recording audio RTP timestamp has an ambiguous wrap or overflows");
                    return;
                }
                if (!bind_media(stream, event, *ticks)) return;
                const std::size_t cost = event.bytes.size() * 2;
                stream.pending.push_back({std::move(event.bytes), *ticks, *ticks + 480,
                                          true, event.received, cost});
                event.cost -= cost;
                event.media_owned = false;
                break;
            }
            case Kind::Report:
                if (!session_valid(stream, event)) return;
                if (!stream.source) {
                    if (std::any_of(stream.early_reports.begin(), stream.early_reports.end(),
                        [&](const auto& known) { return same_clock(known, event.report); })) return;
                    if (stream.early_reports.size() >= max_early_reports) {
                        fail("Recording early sender-report budget exceeded"); return;
                    }
                    stream.early_reports.push_back(event.report);
                } else accept_report(stream, event.report);
                break;
        }
    }

    RecordingClock::Interval mapped(const Stream& stream, const Packet& packet,
                                     RecordingClock::Mode mode) const {
        if (!stream.clock) return {mode == RecordingClock::Mode::Final
                                  ? RecordingClock::State::Error : RecordingClock::State::Pending};
        return stream.clock->map_interval(packet.ticks, packet.end_ticks, mode);
    }

    bool establish_origin(RecordingClock::Mode mode) {
        if (origin) return true;
        std::optional<long double> earliest;
        for (std::size_t i = 0; i < (options.include_audio ? 2u : 1u); ++i) {
            const auto& stream = streams[i];
            if (stream.pending.empty()) {
                if (mode == RecordingClock::Mode::Final) fail("A selected recording track has no approved media");
                return false;
            }
            const auto time = mapped(stream, stream.pending.front(), mode);
            if (time.state == RecordingClock::State::Error) {
                fail("Cannot map the first recording packet from trusted sender reports"); return false;
            }
            if (time.state == RecordingClock::State::Pending) return false;
            earliest = earliest ? std::min(*earliest, time.begin_us) : time.begin_us;
        }
        origin = *earliest;
        RecordingMuxer::Options output;
        output.path = options.path;
        output.format = RecordingMuxer::Format::Matroska;
        output.vps = video_config->vps;
        output.sps = video_config->sps;
        output.pps = video_config->pps;
        if (options.include_audio) output.audio = RecordingMuxer::Audio{};
        std::string error;
        muxer = RecordingMuxer::open(output, error);
        if (!muxer) { fail(error); return false; }
        return true;
    }

    std::optional<int64_t> quantize(long double value) {
        const long double rounded = std::round(value);
        if (!std::isfinite(rounded) || rounded < 0 || rounded >= std::ldexp(1.0L, 63)) {
            fail("Recording time cannot be represented as nonnegative microseconds");
            return std::nullopt;
        }
        return static_cast<int64_t>(rounded);
    }

    void drive(RecordingClock::Mode mode) {
        if (failed() || !establish_origin(mode)) return;
        for (std::size_t i = 0; i < (options.include_audio ? 2u : 1u); ++i) {
            auto& stream = streams[i];
            while (!stream.pending.empty() && !failed()) {
                auto& packet = stream.pending.front();
                const auto time = mapped(stream, packet, mode);
                if (time.state == RecordingClock::State::Pending) break;
                if (time.state == RecordingClock::State::Error) {
                    fail("Recording clock cannot map a packet within its SR/extrapolation budget"); return;
                }
                const auto pts = quantize(time.begin_us - *origin);
                const auto end = quantize(time.end_us - *origin);
                if (!pts || !end) return;
                if (i == 1 && *end <= *pts) {
                    fail("Recording audio duration is not positive after clock mapping"); return;
                }
                const RecordingMuxer::Timing timing{*pts, *pts, *end - *pts};
                std::string error;
                const bool written = i == 0
                    ? muxer->write_video(packet.bytes, timing, packet.keyframe, error)
                    : muxer->write_audio(packet.bytes, timing, error);
                if (!written) { fail(error); return; }
                stream.wrote_media = true;
                const auto cost = packet.cost;
                stream.pending.pop_front();
                release(cost, true);
                prune(stream);
            }
        }
    }

    Time::time_point next_deadline() const {
        auto deadline = origin ? Time::time_point::max() : startup_deadline;
        for (const auto& stream : streams) {
            if (!stream.pending.empty())
                deadline = std::min(deadline, stream.pending.front().received + options.clock_wait);
        }
        return deadline;
    }

    void cleanup() {
        if (muxer) {
            std::string error;
            if (!muxer->finish(error)) fail(error);
            muxer.reset();
        }
        for (auto& stream : streams) {
            for (const auto& packet : stream.pending) release(packet.cost, true);
            stream.pending.clear();
        }
        video_config.reset();
        release(configuration_cost, false);
        configuration_cost = 0;
        std::deque<Event> abandoned;
        {
            std::lock_guard lock(mutex);
            abandoned.swap(ingress);
        }
        for (const auto& event : abandoned) release(event.cost, event.media_owned);
    }

    void loop() {
        try {
            for (;;) {
                Event event;
                bool have_event = false, final = false;
                {
                    std::unique_lock lock(mutex);
                    if (!first_error.empty()) break;
                    if (ingress.empty() && !sealed) {
                        const auto deadline = next_deadline();
                        if (deadline == Time::time_point::max()) changed.wait(lock, [&] {
                            return sealed || !first_error.empty() || !ingress.empty(); });
                        else changed.wait_until(lock, deadline, [&] {
                            return sealed || !first_error.empty() || !ingress.empty(); });
                    }
                    if (!first_error.empty()) break;
                    if (!ingress.empty()) {
                        event = std::move(ingress.front());
                        ingress.pop_front();
                        have_event = true;
                    } else final = sealed;
                }
                if (have_event) {
                    try { process(event); }
                    catch (...) {
                        const auto cost = event.cost;
                        const auto media = event.media_owned;
                        event = {};
                        release(cost, media);
                        throw;
                    }
                    const auto cost = event.cost;
                    const auto media = event.media_owned;
                    event = {};
                    release(cost, media);
                }
                drive(final ? RecordingClock::Mode::Final : RecordingClock::Mode::Running);
                if (failed()) break;
                if (final) {
                    for (std::size_t i = 0; i < (options.include_audio ? 2u : 1u); ++i) {
                        if (!streams[i].wrote_media) fail("A selected recording track did not produce a completed packet");
                    }
                    break;
                }
                if (Time::now() >= next_deadline()) {
                    fail("Recording waited too long for approved media or trusted sender reports");
                    break;
                }
            }
        } catch (...) { fail("Recording worker could not process its input"); }
        cleanup();
        std::lock_guard lock(mutex);
        completed = true;
    }
};

Recorder::Recorder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

std::unique_ptr<Recorder> Recorder::start(const Options& options, std::string& error) {
    error.clear();
    if (!RecordingMuxer::available()) {
        error = "MKV recording requires libavformat and libavcodec";
        return nullptr;
    }
    if (options.path.empty() || options.path.find('\0') != std::string::npos ||
        options.encoded_budget == 0 || options.encoded_budget > max_encoded_budget ||
        options.clock_wait.count() <= 0 || options.clock_wait > std::chrono::milliseconds{5000} ||
        options.final_extrapolation.count() < 0 ||
        options.final_extrapolation > std::chrono::microseconds{1500000}) {
        error = "Invalid Recorder path or bounded buffering/clock limits";
        return nullptr;
    }
    try {
        auto recorder = std::unique_ptr<Recorder>(new Recorder(std::make_unique<Impl>(options)));
        recorder->impl_->worker = std::thread([state = recorder->impl_.get()] { state->loop(); });
        return recorder;
    } catch (...) {
        error = "Cannot create the recording worker";
        return nullptr;
    }
}

Recorder::~Recorder() {
    std::string ignored;
    (void)finish(ignored);
}

bool Recorder::begin_track(Track track, std::span<const uint8_t> session,
                           std::optional<uint32_t> source) {
    if (!valid_track(track) || session.size() != 16 ||
        (track == Track::Audio && !impl_->options.include_audio)) {
        fail("Recording track selection or session UUID is invalid"); return false;
    }
    return impl_->admit(0, false, [&](Impl::Event& event) {
        event.kind = Impl::Kind::Begin;
        event.track = track;
        std::copy(session.begin(), session.end(), event.session.begin());
        event.source = source;
    });
}

bool Recorder::video(std::span<const uint8_t> session, uint32_t source, int64_t ticks,
                     const std::vector<Nal>& nals, const Nal& vps, const Nal& sps, const Nal& pps) {
    if (session.size() != 16) { fail("Recording session UUID is invalid"); return false; }
    std::size_t size = 0;
    for (const auto& nal : nals) {
        if (nal.size() < 2 || nal.size() > kMaxRecordingMuxerPacketBytes - 4 ||
            size > kMaxRecordingMuxerPacketBytes - 4 - nal.size()) {
            fail("Recording HEVC AU is incomplete or exceeds the packet budget"); return false;
        }
        // 单 base layer / temporal layer 配置不能混入另一层或非法 HEVC 头。
        if ((nal[0] & 0x81) != 0 || (nal[1] >> 3) != 0 || (nal[1] & 7) != 1) {
            fail("Recording HEVC AU has an invalid or unsupported layer header"); return false;
        }
        size += nal.size() + 4;
    }
    if (size == 0) { fail("Recording HEVC AU is empty"); return false; }
    std::size_t parameters = 0;
    for (const auto* nal : {&vps, &sps, &pps}) {
        if (nal->size() > kMaxRecordingVideoParameterBytes - parameters) {
            fail("Recording HEVC parameters exceed the configuration budget"); return false;
        }
        parameters += nal->size();
    }
    // 原编码 + 提交 mux 的副本；参数为检查结果和开 mux 预留临时副本。
    return impl_->admit(size * 2 + parameters * 3, true, [&](Impl::Event& event) {
        event.kind = Impl::Kind::Video;
        std::copy(session.begin(), session.end(), event.session.begin());
        event.source = source;
        event.ticks = ticks;
        event.bytes.reserve(size);
        for (const auto& nal : nals) {
            const uint8_t type = (nal[0] >> 1) & 63;
            event.vcl |= type <= 31;
            event.idr |= type == 19 || type == 20;
            event.keyframe |= type >= 16 && type <= 21;
            event.bytes.insert(event.bytes.end(), {0, 0, 0, 1});
            event.bytes.insert(event.bytes.end(), nal.begin(), nal.end());
        }
        event.vps = vps; event.sps = sps; event.pps = pps;
    });
}

bool Recorder::audio(std::span<const uint8_t> session, uint32_t source, uint32_t timestamp,
                     std::span<const uint8_t> payload) {
    if (!impl_->options.include_audio || session.size() != 16 || payload.empty() ||
        payload.size() > kMaxRecordingMuxerPacketBytes) {
        fail("Recording AAC-ELD track, session or payload is invalid"); return false;
    }
    return impl_->admit(payload.size() * 2, true, [&](Impl::Event& event) {
        event.kind = Impl::Kind::Audio;
        event.track = Track::Audio;
        std::copy(session.begin(), session.end(), event.session.begin());
        event.source = source;
        event.timestamp = timestamp;
        event.bytes.assign(payload.begin(), payload.end());
    });
}

bool Recorder::sender_report(Track track, std::span<const uint8_t> session,
                             const rt::SenderReport& report) {
    if (!valid_track(track) || session.size() != 16 ||
        (track == Track::Audio && !impl_->options.include_audio)) {
        fail("Recording sender-report track or session is invalid"); return false;
    }
    return impl_->admit(0, false, [&](Impl::Event& event) {
        event.kind = Impl::Kind::Report;
        event.track = track;
        std::copy(session.begin(), session.end(), event.session.begin());
        event.report = report;
    });
}

void Recorder::fail(std::string_view reason) { impl_->fail(reason); }

std::string Recorder::error() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->first_error;
}

bool Recorder::finish(std::string& error) {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->sealed = true;
    }
    impl_->changed.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
    error = this->error();
    return error.empty();
}

}  // namespace scrctl::media
