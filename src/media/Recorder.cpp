#include "i18n/Translation.h"
#include "media/Recorder.h"

#include "media/RecordingClock.h"
#include "media/RecordingVideoEncoder.h"
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
#include <map>
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
constexpr int64_t mp4_tail_duration_us = 100000;

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
    enum class Kind { Begin, Video, Pixels, Audio, Report };
    struct Event {
        Kind kind = Kind::Begin;
        Track track = Track::Video;
        Session session{};
        std::optional<uint32_t> source;
        int64_t ticks = 0;
        uint32_t timestamp = 0;
        std::vector<uint8_t> bytes;
        std::vector<Nal> nals;
        Nal vps, sps, pps;
        rt::SenderReport report;
        Time::time_point received;
        std::size_t cost = 0;
        bool media_owned = false;
        bool vcl = false, idr = false, keyframe = false;
        Frame frame;
        VideoGeometry geometry;
        std::size_t pixel_cost = 0;
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
    struct MappedVideo {
        Packet packet;
        int64_t pts = 0;
    };

    const Options options;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::deque<Event> ingress;
    std::size_t owned_bytes = 0, media_events = 0;
    std::size_t pixel_bytes = 0, pixel_events = 0;
    std::optional<int64_t> last_pixel_received, last_pixel_admitted;
    std::optional<Session> pixel_session;
    std::optional<uint32_t> pixel_source;
    bool sealed = false, completed = false;
    std::string first_error;
    std::thread worker;

    // 以下状态只由 worker 使用；finish 只在 join 后读错误结果。
    std::array<Stream, 2> streams;
    std::optional<uint64_t> common_reference;
    std::optional<long double> origin;
    // Header describes the first retained image, even if more configurations arrive
    // before sender reports permit opening the muxer. Subsequent in-band epochs
    // are separately retained and checked; neither changes clocks or sessions.
    std::optional<RecordingVideoConfig> video_config;
    std::optional<RecordingVideoConfig> active_video_config;
    std::size_t configuration_cost = 0, active_configuration_cost = 0;
    std::unique_ptr<RecordingMuxer> muxer;
    // 只保留一个完成 SR 映射的 MP4 尾包，原编码和事件仍占用 admission 预算。
    // 它等待下一视频点来决定展示间隔，既不等待 SR，也不需要保留旧时钟锚点。
    std::optional<MappedVideo> video_tail;
    std::unique_ptr<RecordingVideoEncoder> encoder;
    uint32_t encoder_width = 0, encoder_height = 0;
    Time::time_point last_pixels_at{};
    std::map<int64_t, Event> encoding_metadata;
    Time::time_point startup_deadline;

    explicit Impl(Options value)
        : options(std::move(value)), startup_deadline(Time::now() + options.clock_wait) {}

    bool selected(std::size_t i) const {
        return i == 0 ? options.include_video : options.include_audio;
    }

    bool failed() const {
        std::lock_guard lock(mutex);
        return !first_error.empty();
    }
    void fail(std::string_view reason) {
        {
            std::lock_guard lock(mutex);
            if (completed) return;
            if (first_error.empty()) first_error = reason.empty() ? SCRCTL_TR("Recording failed") : reason;
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
            fail_locked(SCRCTL_TR("Recorder input exceeds its encoded-byte or event budget"));
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
            fail_locked(SCRCTL_TR("Cannot allocate the Recorder input buffer"));
            return false;
        }
        changed.notify_one();
        return true;
    }

    void release_pixels(std::size_t bytes) {
        if (!bytes) return;
        std::lock_guard lock(mutex);
        pixel_bytes -= bytes;
        --pixel_events;
    }

    bool session_valid(Stream& stream, const Event& event) {
        if (!stream.begun || stream.session != event.session) {
            fail(SCRCTL_TR("Recording media belongs to a missing or changed session"));
            return false;
        }
        return true;
    }

    void prune(Stream& stream) {
        if (!stream.clock || !origin) return;
        // FU/AU 的完成可晚于同源 SR；空 pending 不代表之后不会收到较早采样。
        // 留最近七个锚点，为新 SR 在八点预算内留一槽。pending 仍保护所需左点；
        // 这是有界历史，不承诺任意延迟或固定秒数的样本都能被恢复。
        std::optional<int64_t> earliest;
        if (!stream.pending.empty()) earliest = stream.pending.front().ticks;
        if (&stream == &streams[0] && !encoding_metadata.empty()) {
            const auto encoding = encoding_metadata.begin()->first;
            earliest = earliest ? std::min(*earliest, encoding) : encoding;
        }
        if (earliest) stream.clock->discard_before(*earliest, recent_clock_anchors);
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
                fail(SCRCTL_TR("Recording early sender-report budget exceeded"));
                return;
            }
            stream.early_reports.push_back(report);
            return;
        }
        const int64_t reference = stream.report_high
            ? std::max(*stream.media_high, *stream.report_high) : *stream.media_high;
        const auto ticks = rt::RtpTimestamp::nearest(report.rtp_timestamp, reference);
        if (ntp(report) == 0 || !ticks) {
            fail(SCRCTL_TR("Recording sender report has no usable NTP/RTP clock"));
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
            fail(SCRCTL_TR("Recording media source changed within the session"));
            return false;
        }
        if (stream.last_media && ticks <= *stream.last_media) {
            fail(SCRCTL_TR("Recording media timestamps do not advance within the track"));
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
            fail(SCRCTL_TR("Recording media source changed within the session")); return;
        }
        if (!event.vcl) return; // parameter-only prefixes belong to the next image
        if (!bind_media(stream, event, event.ticks)) return;
        if (!video_config && !event.idr) return;
        const auto* current = active_video_config ? &*active_video_config
                                                 : video_config ? &*video_config : nullptr;
        const bool changed = current && (event.vps != current->vps ||
            event.sps != current->sps || event.pps != current->pps);
        const bool first = !video_config;
        std::optional<RecordingVideoConfig> checked;
        if (first || changed) {
            if (!event.idr) {
                fail(SCRCTL_TR("Recording HEVC configuration changes require an IDR")); return;
            }
            checked = inspect_recording_video_config(event.vps, event.sps, event.pps);
            if (!checked->permits_equal_dts_pts()) {
                fail(checked->error.empty() ? SCRCTL_TR("Recording HEVC requires a no-reorder configuration") : checked->error);
                return;
            }
            const auto syntax = inspect_recording_idr(event.nals, *checked);
            if (!syntax.valid()) { fail(syntax.error); return; }
        }
        // Encode no pixels: retain each NAL verbatim and prepend the new epoch's
        // parameters to its IDR. hev1/MKV carry these in-band; the initial header
        // remains fixed. Exclude duplicate parameter prefixes on transitions.
        std::size_t prefix_bytes = 0;
        if (changed) prefix_bytes = event.vps.size() + event.sps.size() + event.pps.size() + 12;
        std::size_t bytes = prefix_bytes;
        for (const auto& nal : event.nals) {
            const auto type = (nal[0] >> 1) & 63;
            if (changed && type >= 32 && type <= 34) continue;
            if (nal.size() > kMaxRecordingMuxerPacketBytes - 4 ||
                bytes > kMaxRecordingMuxerPacketBytes - 4 - nal.size()) {
                fail(SCRCTL_TR("Recording HEVC AU is incomplete or exceeds the packet budget")); return;
            }
            bytes += nal.size() + 4;
        }
        const auto packet_cost = bytes * 2;
        const auto retained_parameters = (first || changed)
            ? (event.vps.size() + event.sps.size() + event.pps.size()) * 3 : 0;
        if (packet_cost + retained_parameters > event.cost) {
            const auto extra = packet_cost + retained_parameters - event.cost;
            bool exceeded;
            {
                std::lock_guard lock(mutex);
                exceeded = extra > options.encoded_budget - owned_bytes;
                if (!exceeded) { owned_bytes += extra; event.cost += extra; }
            }
            if (exceeded) { fail(SCRCTL_TR("Recorder input exceeds its encoded-byte or event budget")); return; }
        }
        event.bytes.reserve(bytes);
        auto append = [&](const Nal& nal) {
            event.bytes.insert(event.bytes.end(), {0, 0, 0, 1});
            event.bytes.insert(event.bytes.end(), nal.begin(), nal.end());
        };
        if (changed) { append(event.vps); append(event.sps); append(event.pps); }
        for (const auto& nal : event.nals) {
            const auto type = (nal[0] >> 1) & 63;
            if (!(changed && type >= 32 && type <= 34)) append(nal);
        }
        event.nals.clear();
        stream.pending.push_back({std::move(event.bytes), event.ticks, event.ticks,
                                  event.keyframe, event.received, packet_cost});
        event.cost -= packet_cost;
        event.media_owned = false;
        if (first) {
            configuration_cost = retained_parameters;
            video_config = std::move(checked);
            event.cost -= configuration_cost;
        } else if (changed) {
            active_video_config.reset();
            release(active_configuration_cost, false);
            active_configuration_cost = retained_parameters;
            active_video_config = std::move(checked);
            event.cost -= active_configuration_cost;
        }
    }

    void encoded_packets(std::vector<RecordingVideoEncoder::Packet>& outputs) {
        for (auto& output : outputs) {
            const auto found = encoding_metadata.find(output.pts_ticks);
            if (found == encoding_metadata.end()) {
                fail(SCRCTL_TR("Recording encoder returned an unknown timestamp")); return;
            }
            Event event = std::move(found->second);
            encoding_metadata.erase(found);
            event.kind = Kind::Video;
            event.nals = std::move(output.nals);
            event.vps = std::move(output.vps); event.sps = std::move(output.sps); event.pps = std::move(output.pps);
            std::size_t bytes = 0;
            for (const auto& nal : event.nals) {
                bytes += nal.size() + 4;
                if (nal.size() < 2) { fail(SCRCTL_TR("Recording encoder returned an invalid NAL")); return; }
                const unsigned type = (nal[0] >> 1) & 63;
                event.vcl |= type <= 31; event.idr |= type == 19 || type == 20;
                event.keyframe |= type >= 16 && type <= 21;
            }
            const std::size_t cost = bytes * 2 + (event.vps.size() + event.sps.size() + event.pps.size()) * 3;
            {
                std::lock_guard lock(mutex);
                if (cost > options.encoded_budget - owned_bytes || media_events >= max_media_events) {
                    fail_locked(SCRCTL_TR("Recorder input exceeds its encoded-byte or event budget")); return;
                }
                owned_bytes += cost; ++media_events;
            }
            event.cost = cost; event.media_owned = true;
            try { process_video(event); }
            catch (...) { release(event.cost, event.media_owned); throw; }
            release(event.cost, event.media_owned);
            if (failed()) return;
        }
    }

    void process_pixels(Event& event) {
        auto& stream = streams[0];
        if (!session_valid(stream, event)) return;
        if (stream.source && stream.source != event.source) {
            fail(SCRCTL_TR("Recording media source changed within the session")); return;
        }
        if (!encoder) {
            const bool swapped = event.geometry.clockwise == 90 || event.geometry.clockwise == 270;
            const auto width = event.geometry.crop_width > 0 ? event.geometry.crop_width : event.frame.width;
            const auto height = event.geometry.crop_height > 0 ? event.geometry.crop_height : event.frame.height;
            RecordingVideoEncoder::Options config;
            config.width = (swapped ? height : width) + ((swapped ? height : width) & 1);
            config.height = (swapped ? width : height) + ((swapped ? width : height) & 1);
            std::string error;
            encoder_width = config.width; encoder_height = config.height;
            encoder = RecordingVideoEncoder::create(config, error);
            if (!encoder) { fail(error); return; }
        }
        if (encoding_metadata.size() >= 8 || encoding_metadata.contains(event.ticks)) {
            fail(SCRCTL_TR("Recording encoder pending-frame budget exceeded")); return;
        }
        Event metadata;
        metadata.session = event.session; metadata.source = event.source;
        metadata.ticks = event.ticks; metadata.received = event.received;
        encoding_metadata.emplace(event.ticks, std::move(metadata));
        const RecordingVideoEncoder::Crop crop{static_cast<uint32_t>(event.geometry.crop_x),
            static_cast<uint32_t>(event.geometry.crop_y), static_cast<uint32_t>(event.geometry.crop_width),
            static_cast<uint32_t>(event.geometry.crop_height)};
        std::vector<RecordingVideoEncoder::Packet> outputs;
        std::string error;
        if (!encoder->encode(event.frame, event.geometry.clockwise, crop, event.ticks, outputs, error)) {
            fail(error); return;
        }
        encoded_packets(outputs);
        last_pixels_at = event.received;
    }

    void process(Event& event) {
        auto& stream = streams[index(event.track)];
        switch (event.kind) {
            case Kind::Begin:
                if (stream.begun) { fail(SCRCTL_TR("Recording track session was started again")); return; }
                stream.begun = true;
                stream.session = event.session;
                stream.source = event.source;
                break;
            case Kind::Video: process_video(event); break;
            case Kind::Pixels: process_pixels(event); break;
            case Kind::Audio: {
                if (!session_valid(stream, event)) return;
                const auto ticks = stream.timestamp.observe(event.timestamp);
                if (!ticks || *ticks > std::numeric_limits<int64_t>::max() - 480) {
                    fail(SCRCTL_TR("Recording audio RTP timestamp has an ambiguous wrap or overflows"));
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
                // An asynchronous encoder needs drain when a static source stops
                // supplying frames. Reopen the same canvas before new activity.
                if (event.track == Track::Video && stream.source == event.report.ssrc &&
                    encoder && !encoding_metadata.empty() &&
                    event.received - last_pixels_at >= std::chrono::milliseconds(100)) {
                    std::vector<RecordingVideoEncoder::Packet> outputs;
                    std::string error;
                    if (!encoder->finish(outputs, error)) { fail(error); return; }
                    encoded_packets(outputs);
                    if (failed()) return;
                    if (!encoding_metadata.empty()) {
                        fail(SCRCTL_TR("Recording encoder did not return every submitted frame")); return;
                    }
                    RecordingVideoEncoder::Options config;
                    config.width = encoder_width; config.height = encoder_height;
                    encoder = RecordingVideoEncoder::create(config, error);
                    if (!encoder) { fail(error); return; }
                }
                if (!stream.source) {
                    if (std::any_of(stream.early_reports.begin(), stream.early_reports.end(),
                        [&](const auto& known) { return same_clock(known, event.report); })) return;
                    if (stream.early_reports.size() >= max_early_reports) {
                        fail(SCRCTL_TR("Recording early sender-report budget exceeded")); return;
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
        for (std::size_t i = 0; i < streams.size(); ++i) {
            if (!selected(i)) continue;
            const auto& stream = streams[i];
            if (stream.pending.empty()) {
                if (mode == RecordingClock::Mode::Final) fail(SCRCTL_TR("A selected recording track has no approved media"));
                return false;
            }
            const auto time = mapped(stream, stream.pending.front(), mode);
            if (time.state == RecordingClock::State::Error) {
                fail(SCRCTL_TR("Cannot map the first recording packet from trusted sender reports")); return false;
            }
            if (time.state == RecordingClock::State::Pending) return false;
            earliest = earliest ? std::min(*earliest, time.begin_us) : time.begin_us;
        }
        origin = *earliest;
        RecordingMuxer::Options output;
        output.path = options.path;
        output.format = options.format;
        output.video_orientation = options.video_orientation;
        output.include_video = options.include_video;
        if (options.include_video) {
            output.vps = video_config->vps;
            output.sps = video_config->sps;
            output.pps = video_config->pps;
        }
        if (options.include_audio) output.audio = RecordingMuxer::Audio{};
        std::string error;
        muxer = RecordingMuxer::open(output, error);
        if (!muxer) { fail(error); return false; }
        return true;
    }

    std::optional<int64_t> quantize(long double value) {
        const long double rounded = std::round(value);
        if (!std::isfinite(rounded) || rounded < 0 || rounded >= std::ldexp(1.0L, 63)) {
            fail(SCRCTL_TR("Recording time cannot be represented as nonnegative microseconds"));
            return std::nullopt;
        }
        return static_cast<int64_t>(rounded);
    }

    bool write_mp4_tail(int64_t duration) {
        const auto& tail = *video_tail;
        if (duration <= 0 || tail.pts > std::numeric_limits<int64_t>::max() - duration) {
            fail(SCRCTL_TR("MP4 recording video duration is not positive or overflows"));
            return false;
        }
        std::string error;
        if (!muxer->write_video(tail.packet.bytes, {tail.pts, tail.pts, duration},
                                tail.packet.keyframe, error)) {
            fail(error);
            return false;
        }
        streams[0].wrote_media = true;
        const auto cost = tail.packet.cost;
        video_tail.reset();
        release(cost, true);
        return true;
    }

    void drive(RecordingClock::Mode mode) {
        if (failed() || !establish_origin(mode)) return;
        for (std::size_t i = 0; i < streams.size(); ++i) {
            if (!selected(i)) continue;
            auto& stream = streams[i];
            while (!stream.pending.empty() && !failed()) {
                auto& packet = stream.pending.front();
                const auto time = mapped(stream, packet, mode);
                if (time.state == RecordingClock::State::Pending) break;
                if (time.state == RecordingClock::State::Error) {
                    fail(SCRCTL_TR("Recording clock cannot map a packet within its SR/extrapolation budget")); return;
                }
                const auto pts = quantize(time.begin_us - *origin);
                const auto end = quantize(time.end_us - *origin);
                if (!pts || !end) return;
                if (i == 1 && *end <= *pts) {
                    fail(SCRCTL_TR("Recording audio duration is not positive after clock mapping")); return;
                }
                if (i == 0 && options.format == RecordingMuxer::Format::Mp4) {
                    if (video_tail) {
                        if (*pts <= video_tail->pts) {
                            fail(SCRCTL_TR("MP4 recording video timestamps do not advance after clock mapping"));
                            return;
                        }
                        if (!write_mp4_tail(*pts - video_tail->pts)) return;
                    }
                    video_tail.emplace(MappedVideo{std::move(packet), *pts});
                    stream.pending.pop_front();
                    // 只转移所有权，不释放尾包的原编码或媒体事件预算。
                    prune(stream);
                    continue;
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
        // 音频可以在静止画面期间独立写入。只有正常 Final 才确定末帧展示规则；
        // 失败时仍由 cleanup 收回尾包预算，不把未完成录制伪装成成功。
        if (options.include_video && mode == RecordingClock::Mode::Final && video_tail && !failed())
            (void)write_mp4_tail(mp4_tail_duration_us);
    }

    Time::time_point next_deadline() const {
        auto deadline = origin ? Time::time_point::max() : startup_deadline;
        for (std::size_t i = 0; i < streams.size(); ++i) {
            if (!selected(i)) continue;
            const auto& stream = streams[i];
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
        if (video_tail) {
            const auto cost = video_tail->packet.cost;
            video_tail.reset();
            release(cost, true);
        }
        video_config.reset();
        active_video_config.reset();
        release(active_configuration_cost, false);
        active_configuration_cost = 0;
        release(configuration_cost, false);
        configuration_cost = 0;
        std::deque<Event> abandoned;
        {
            std::lock_guard lock(mutex);
            abandoned.swap(ingress);
        }
        for (const auto& event : abandoned) { release(event.cost, event.media_owned); release_pixels(event.pixel_cost); }
        encoder.reset(); encoding_metadata.clear();
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
                        const auto pixels = event.pixel_cost;
                        event = {};
                        release(cost, media); release_pixels(pixels);
                        throw;
                    }
                    const auto cost = event.cost;
                    const auto media = event.media_owned;
                    const auto pixels = event.pixel_cost;
                    event = {};
                    release(cost, media); release_pixels(pixels);
                }
                if (final && encoder) {
                    std::vector<RecordingVideoEncoder::Packet> outputs;
                    std::string error;
                    if (!encoder->finish(outputs, error)) fail(error);
                    else encoded_packets(outputs);
                    if (!failed() && !encoding_metadata.empty())
                        fail(SCRCTL_TR("Recording encoder did not return every submitted frame"));
                }
                drive(final ? RecordingClock::Mode::Final : RecordingClock::Mode::Running);
                if (failed()) break;
                if (final) {
                    for (std::size_t i = 0; i < streams.size(); ++i) {
                        if (!selected(i)) continue;
                        if (!streams[i].wrote_media) fail(SCRCTL_TR("A selected recording track did not produce a completed packet"));
                    }
                    break;
                }
                if (Time::now() >= next_deadline()) {
                    fail(SCRCTL_TR("Recording waited too long for approved media or trusted sender reports"));
                    break;
                }
            }
        } catch (...) { fail(SCRCTL_TR("Recording worker could not process its input")); }
        cleanup();
        std::lock_guard lock(mutex);
        completed = true;
    }
};

Recorder::Recorder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

bool Recorder::includes_video() const { return impl_->options.include_video; }
bool Recorder::includes_audio() const { return impl_->options.include_audio; }

std::unique_ptr<Recorder> Recorder::start(const Options& options, std::string& error) {
    if (!options.include_video && !options.include_audio) {
        error = SCRCTL_TR("Recording requires at least one selected track");
        return nullptr;
    }
    if (!options.include_video && options.video_orientation != 0) {
        error = SCRCTL_TR("Audio-only recording requires zero video orientation");
        return nullptr;
    }
    if (options.upright_video && (!options.include_video || options.video_orientation != 0)) {
        error = SCRCTL_TR("Upright recording requires video and zero static orientation"); return nullptr;
    }
    if (options.upright_max_fps > 60) {
        error = SCRCTL_TR("Invalid Recorder path or bounded buffering/clock limits"); return nullptr;
    }
    if (options.upright_video && !RecordingVideoEncoder::available(error)) return nullptr;
    if (!RecordingMuxer::validate_video_orientation(options.format, options.video_orientation, error))
        return nullptr;
    if (!RecordingMuxer::available()) {
        error = SCRCTL_TR("Container recording requires libavformat and libavcodec");
        return nullptr;
    }
    if (options.path.empty() || options.path.find('\0') != std::string::npos ||
        options.encoded_budget == 0 || options.encoded_budget > max_encoded_budget ||
        options.clock_wait.count() <= 0 || options.clock_wait > std::chrono::milliseconds{5000} ||
        options.final_extrapolation.count() < 0 ||
        options.final_extrapolation > std::chrono::microseconds{1500000}) {
        error = SCRCTL_TR("Invalid Recorder path or bounded buffering/clock limits");
        return nullptr;
    }
    try {
        auto recorder = std::unique_ptr<Recorder>(new Recorder(std::make_unique<Impl>(options)));
        if (options.upright_video && (options.upright_width || options.upright_height)) {
            RecordingVideoEncoder::Options encoder_options;
            encoder_options.width = options.upright_width;
            encoder_options.height = options.upright_height;
            recorder->impl_->encoder_width = encoder_options.width;
            recorder->impl_->encoder_height = encoder_options.height;
            recorder->impl_->encoder = RecordingVideoEncoder::create(encoder_options, error);
            if (!recorder->impl_->encoder) return nullptr;
            std::fprintf(stderr, SCRCTL_TR("Recording encoder: %s, canvas %ux%u\n"),
                recorder->impl_->encoder->backend_name(), options.upright_width, options.upright_height);
        }
        recorder->impl_->worker = std::thread([state = recorder->impl_.get()] { state->loop(); });
        return recorder;
    } catch (...) {
        error = SCRCTL_TR("Cannot create the recording worker");
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
        !impl_->selected(index(track))) {
        fail(SCRCTL_TR("Recording track selection or session UUID is invalid")); return false;
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
    if (impl_->options.upright_video) {
        fail(SCRCTL_TR("Upright recording accepts decoded frames only")); return false;
    }
    if (!impl_->options.include_video) {
        fail(SCRCTL_TR("This recording has no video track")); return false;
    }
    if (session.size() != 16) { fail(SCRCTL_TR("Recording session UUID is invalid")); return false; }
    if (nals.empty() || nals.size() > kMaxRecordingIdrNals) {
        fail(SCRCTL_TR("Recording HEVC AU is empty or exceeds the NAL-count budget")); return false;
    }
    std::size_t size = 0;
    for (const auto& nal : nals) {
        if (nal.size() < 2 || nal.size() > kMaxRecordingMuxerPacketBytes - 4 ||
            size > kMaxRecordingMuxerPacketBytes - 4 - nal.size()) {
            fail(SCRCTL_TR("Recording HEVC AU is incomplete or exceeds the packet budget")); return false;
        }
        // 单 base layer / temporal layer 配置不能混入另一层或非法 HEVC 头。
        if ((nal[0] & 0x81) != 0 || (nal[1] >> 3) != 0 || (nal[1] & 7) != 1) {
            fail(SCRCTL_TR("Recording HEVC AU has an invalid or unsupported layer header")); return false;
        }
        size += nal.size() + 4;
    }
    if (size == 0) { fail(SCRCTL_TR("Recording HEVC AU is empty")); return false; }
    std::size_t parameters = 0;
    for (const auto* nal : {&vps, &sps, &pps}) {
        if (nal->size() > kMaxRecordingVideoParameterBytes - parameters) {
            fail(SCRCTL_TR("Recording HEVC parameters exceed the configuration budget")); return false;
        }
        parameters += nal->size();
    }
    // 原编码 + 提交 mux 的副本；参数为检查结果和开 mux 预留临时副本。
    return impl_->admit(size * 2 + parameters * 3, true, [&](Impl::Event& event) {
        event.kind = Impl::Kind::Video;
        std::copy(session.begin(), session.end(), event.session.begin());
        event.source = source;
        event.ticks = ticks;
        event.nals = nals;
        for (const auto& nal : nals) {
            const uint8_t type = (nal[0] >> 1) & 63;
            event.vcl |= type <= 31;
            event.idr |= type == 19 || type == 20;
            event.keyframe |= type >= 16 && type <= 21;
        }
        event.vps = vps; event.sps = sps; event.pps = pps;
    });
}

bool Recorder::video_frame(std::span<const uint8_t> session, uint32_t source, int64_t ticks,
                           const Frame& frame, VideoGeometry geometry) {
    if (!impl_->options.upright_video || !impl_->options.include_video || session.size() != 16) {
        fail(SCRCTL_TR("Recording decoded-frame mode or session is invalid")); return false;
    }
    const uint64_t bytes = uint64_t(frame.row_pitch) * frame.height;
    if (!frame.width || !frame.height || frame.bytes_per_pixel != 4 ||
        uint64_t(frame.width) * 4 > frame.row_pitch || bytes > frame.pixels.size() ||
        !bytes || bytes > 64u * 1024u * 1024u || geometry.crop_x < 0 || geometry.crop_y < 0 ||
        geometry.crop_width < 0 || geometry.crop_height < 0 ||
        (geometry.clockwise != 0 && geometry.clockwise != 90 && geometry.clockwise != 180 && geometry.clockwise != 270)) {
        fail(SCRCTL_TR("Recording BGRA frame or orientation is invalid")); return false;
    }
    std::lock_guard lock(impl_->mutex);
    if (impl_->sealed || !impl_->first_error.empty()) return false;
    Session incoming_session;
    std::copy(session.begin(), session.end(), incoming_session.begin());
    if (impl_->pixel_session && *impl_->pixel_session != incoming_session) {
        impl_->fail_locked(SCRCTL_TR("Recording media belongs to a missing or changed session")); return false;
    }
    if (impl_->pixel_source && *impl_->pixel_source != source) {
        impl_->fail_locked(SCRCTL_TR("Recording media source changed within the session")); return false;
    }
    impl_->pixel_session = incoming_session; impl_->pixel_source = source;
    if (ticks < 0 || (impl_->last_pixel_received && ticks <= *impl_->last_pixel_received)) {
        impl_->fail_locked(SCRCTL_TR("Recording media timestamps do not advance within the track")); return false;
    }
    impl_->last_pixel_received = ticks;
    if (impl_->options.upright_max_fps && impl_->last_pixel_admitted &&
        ticks - *impl_->last_pixel_admitted < (24000 + impl_->options.upright_max_fps - 1) / impl_->options.upright_max_fps)
        return true; // configured rate sampling, not queue-overload dropping
    if (impl_->pixel_events >= 4 || bytes > 64u * 1024u * 1024u - impl_->pixel_bytes ||
        impl_->ingress.size() >= max_ingress_events) {
        impl_->fail_locked(SCRCTL_TR("Recorder decoded-frame budget exceeded")); return false;
    }
    ++impl_->pixel_events; impl_->pixel_bytes += bytes;
    impl_->last_pixel_admitted = ticks;
    try {
        Impl::Event event;
        event.kind = Impl::Kind::Pixels;
        std::copy(session.begin(), session.end(), event.session.begin());
        event.source = source; event.ticks = ticks; event.received = Time::now();
        event.geometry = geometry; event.pixel_cost = static_cast<std::size_t>(bytes);
        event.frame.width = frame.width; event.frame.height = frame.height;
        event.frame.row_pitch = frame.row_pitch; event.frame.bytes_per_pixel = 4;
        event.frame.pixels.assign(frame.pixels.begin(), frame.pixels.begin() + static_cast<std::size_t>(bytes));
        impl_->ingress.push_back(std::move(event));
    } catch (...) {
        --impl_->pixel_events; impl_->pixel_bytes -= bytes;
        impl_->fail_locked(SCRCTL_TR("Cannot allocate the Recorder input buffer")); return false;
    }
    impl_->changed.notify_one();
    return true;
}

bool Recorder::audio(std::span<const uint8_t> session, uint32_t source, uint32_t timestamp,
                     std::span<const uint8_t> payload) {
    if (!impl_->options.include_audio || session.size() != 16 || payload.empty() ||
        payload.size() > kMaxRecordingMuxerPacketBytes) {
        fail(SCRCTL_TR("Recording AAC-ELD track, session or payload is invalid")); return false;
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
        !impl_->selected(index(track))) {
        fail(SCRCTL_TR("Recording sender-report track or session is invalid")); return false;
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
