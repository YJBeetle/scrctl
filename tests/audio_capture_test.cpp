#include "app/AudioOut.h"
#include "media/AudioPump.h"
#include "media/Recorder.h"
#include "RecordingAudioEvidence.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

#ifdef SCRCTL_HAVE_LIBAVFORMAT
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}
#endif

// 编译真实 AudioPump 接收循环、AudioOut 和 core/Recorder。仅在本可执行文件中
// 替换已存在的设备会话、解码/补偿工厂与 SDL 开声卡边界，不访问网络或声卡。
namespace {
using Pump = scrctl::media::AudioPump;
using Session = scrctl::media::StreamSession;
using Recorder = scrctl::media::Recorder;
using Bytes = std::vector<uint8_t>;
using namespace std::chrono_literals;
int checks = 0, failures = 0;
void check(bool condition, const char* message) {
    ++checks;
    if (!condition) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
struct Script {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<Bytes> packets;
    std::vector<Session::Request> starts;
    std::deque<Session::StartStatus> start_results;
    std::optional<int64_t> route_mode;
    std::vector<std::pair<Bytes, uint16_t>> sent;
    std::vector<Bytes> decoded;
    bool local_present = true, end_once = false, send_ok = true, stop_ok = true;
    uint32_t local = 0;
    std::atomic<unsigned> factories{0}, regulators{0}, pcm_calls{0}, opens{0}, stops{0}, receivers{0};
    bool stop_before_join = false;
    int stop_delay_ms = 0;
    bool decoder_available = false;
    void reset() {
        std::lock_guard lock(mutex);
        packets.clear(); starts.clear(); sent.clear(); decoded.clear(); start_results.clear(); route_mode.reset();
        local_present = true; end_once = false; send_ok = true; stop_ok = true; local = 0;
        factories = 0; regulators = 0; pcm_calls = 0; opens = 0; stops = 0; receivers = 0;
        stop_before_join = false;
        stop_delay_ms = 0;
        decoder_available = false;
    }
    void enqueue(Bytes packet) {
        { std::lock_guard lock(mutex); packets.push_back(std::move(packet)); }
        ready.notify_one();
    }
    std::size_t start_count() { std::lock_guard lock(mutex); return starts.size(); }
    std::size_t sent_count() { std::lock_guard lock(mutex); return sent.size(); }
} script;
template<class Predicate> bool wait(Predicate predicate, std::chrono::milliseconds limit = 2000ms) {
    const auto until = std::chrono::steady_clock::now() + limit;
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(5ms);
    } while (std::chrono::steady_clock::now() < until);
    return predicate();
}
void put32(Bytes& packet, std::size_t offset, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        packet[offset + i] = static_cast<uint8_t>(value >> (24 - 8 * i));
}
#ifdef SCRCTL_HAVE_LIBAVFORMAT
uint32_t get32(const Bytes& packet, std::size_t offset) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value = (value << 8) | packet[offset + i];
    return value;
}
#endif
const Bytes silence{0x00, 0x68, 0x34, 0x00};
Bytes rtp(uint16_t sequence, uint32_t ticks, uint32_t source, const Bytes& payload = silence,
          bool decorated = false, uint8_t type = 101) {
    Bytes packet(decorated ? 24u : 12u, 0);
    packet[0] = decorated ? 0xb1 : 0x80; // V2 + padding + extension + one CSRC
    packet[1] = static_cast<uint8_t>(type | (decorated ? 0x80 : 0));
    packet[2] = static_cast<uint8_t>(sequence >> 8); packet[3] = static_cast<uint8_t>(sequence);
    put32(packet, 4, ticks); put32(packet, 8, source);
    if (decorated) { put32(packet, 12, 77); packet[16] = 0xbe; packet[17] = 0xde; packet[19] = 1; }
    packet.insert(packet.end(), payload.begin(), payload.end());
    if (decorated) packet.insert(packet.end(), {0, 0, 3});
    return packet;
}
#ifdef SCRCTL_HAVE_LIBAVFORMAT
Bytes sender_report(uint32_t source, uint32_t ticks, uint32_t seconds) {
    auto packet = scrctl::rt::build_sr(source, 0, 0);
    put32(packet, 8, seconds); put32(packet, 12, 0); put32(packet, 16, ticks);
    return packet;
}
#endif
class Decoder final : public scrctl::AudioDecoder {
public:
    bool decode(std::span<const uint8_t> frame, std::vector<int16_t>& pcm, std::string& error) override {
        ++script.pcm_calls;
        { std::lock_guard lock(script.mutex); script.decoded.emplace_back(frame.begin(), frame.end()); }
        if (!frame.empty() && frame.front() == 0xff) { error = "controlled decode failure"; return false; }
        pcm.insert(pcm.end(), 960, 17); error.clear(); return true;
    }
    const char* backend_name() const override { return "controlled"; }
};
#ifdef SCRCTL_HAVE_LIBAVFORMAT
void no_pcm(const Pump& pump) {
    const auto stats = pump.stats();
    check(!pump.decoding_enabled() && pump.backend_name() == "none", "capture-only has no decoder backend");
    check(script.factories == 0 && script.regulators == 0 && script.pcm_calls == 0,
          "capture-only never creates a decoder or regulator or decodes a packet");
    check(stats.decoded == 0 && stats.decode_failed == 0 && stats.clock_failed == 0 &&
          !stats.clock.active && stats.clock.compensation_updates == 0,
          "intentional decode bypass is not reported as a failure or active PCM clock");
    check(pump.buffered_frames() == 0 && pump.preroll_frames() == 0 &&
          stats.dropped_stale == 0 && stats.startup_trimmed == 0 && stats.steered == 0,
          "capture-only has no playback buffering or trim statistics");
}
#endif
void gates_and_default() {
    script.reset(); scrctl::remote::Device device; std::string error;
    Pump::Options options;
    check(options.decode_pcm && !options.stop_device_on_exit,
          "existing callers decode PCM and do not own stopAll by default");
    check(!Pump::start(device, options, error) && error == "controlled decoder unavailable" &&
          script.factories == 1 && script.start_count() == 0,
          "default decoder failure occurs before any device audio routing request");
    script.reset(); options.decode_pcm = false;
    options.target_backlog_ms = 2000;
    const auto waterline = Pump::compute_waterline(options);
    check(waterline.target_frames == 0 && waterline.capacity_frames == 0 && waterline.clamped_to_ms == 0,
          "capture-only does not reserve or clamp a playback buffer");
    check(!Pump::start(device, options, error) && error.find("requires a container recording") != std::string::npos &&
          script.factories == 0 && script.start_count() == 0,
          "capture-only without a consumer fails before decoder or session construction");

    script.reset(); script.decoder_available = true; options = {};
    auto pump = Pump::start(device, options, error);
    check(pump != nullptr && script.factories == 1 && script.regulators == 1, "default playback still initializes its PCM pipeline");
    if (!pump) return;
    scrctl::app::AudioOut output;
    check(!output.open(*pump, error) && error == "controlled audio device unavailable" && script.opens == 1,
          "PCM-enabled playback reaches the existing SDL audio-device boundary");
    script.enqueue(rtp(20, 0, 0, silence, true));
    script.enqueue(rtp(21, 480, 0, Bytes{0xff, 0x00}));
    check(wait([&] { const auto s = pump->stats(); return s.decoded == 1 && s.decode_failed == 1; }),
          "default loop decodes valid audio and preserves decoder-failure accounting");
    std::array<int16_t, 960> pcm{};
    check(pump->read(pcm.data(), 480) == 480 && pcm.front() == 17 && pcm.back() == 17,
          "default playback still delivers interleaved PCM");
    pump->stop();
    { std::lock_guard lock(script.mutex);
      check(script.decoded.size() == 2 && script.decoded.front() == silence,
            "real RTP parser strips marker, CSRC, extension and padding before decoding"); }
    check(script.stops == 0 && pump->terminal_error().empty(),
          "default audio stop never sends device stopAll or makes ordinary decode failure terminal");
}

void audio_only_cleanup() {
    for (const bool stop_ok : {true, false}) {
        script.reset(); script.decoder_available = true; script.stop_ok = stop_ok;
        scrctl::remote::Device device; std::string error;
        Pump::Options options; options.stop_device_on_exit = true;
        auto pump = Pump::start(device, options, error);
        check(pump != nullptr, "audio-only owner starts the existing PCM receive loop");
        if (!pump) continue;
        check(wait([] { return script.receivers != 0; }), "audio-only fixture enters the real worker's receive boundary");
        scrctl::app::AudioOut output;
        check(!output.open(*pump, error) && error == "controlled audio device unavailable",
              "audio-only playback failure retains the original SDL device error");
        script.enqueue(rtp(20, 0, 0));
        check(wait([&] { return pump->stats().decoded == 1; }), "audio-only loop remains owned until explicit final cleanup");
        pump->stop();
        check(error == "controlled audio device unavailable", "cleanup does not overwrite the caller's playback error");
        check(script.stops == 1 && script.receivers == 0 && !script.stop_before_join,
              "audio-only owner joins receive before the sole stopAll request");
        check(pump->receiver_port() == 0 && pump->payload_type() == 0,
              "final cleanup clears the cross-thread session snapshot");
        check(pump->terminal_error() == (stop_ok ? "" : "controlled stop failure"),
              "stopAll failure is a readable sticky terminal error instead of a clean success");
        const auto sent = script.sent_count();
        pump->stop(); pump.reset();
        check(script.stops == 1 && script.sent_count() == sent,
              "repeated stop and destructor neither repeat stopAll nor continue RR");
    }
}
void initial_rejection_cleanup() {
    for (const bool owner : {false, true}) {
        script.reset(); script.decoder_available = true;
        script.start_results.push_back(Session::StartStatus::AcceptedInvalidAnswer);
        scrctl::remote::Device device; std::string error;
        Pump::Options options; options.stop_device_on_exit = owner;
        check(!Pump::start(device, options, error) && script.start_count() == 1,
              "accepted-invalid initial answer cannot publish an audio pump or retry startup");
        check(error.find("controlled accepted-invalid answer") == 0 &&
              error.find("wait for session expiry") != std::string::npos && script.stops == 0,
              "initial accepted-invalid reply has no confirmed handle and truthfully relies on lease expiry");
        check(script.sent_count() == 0 && script.regulators == 0,
              "initial rejection starts neither RR worker nor PCM regulator");

        script.reset(); script.decoder_available = true; script.route_mode = 8;
        check(!Pump::start(device, options, error) &&
              error.find("Computer-only audio routing was not accepted") == 0,
              "valid session with rejected routing retains the explicit negotiation error");
        check(script.stops == (owner ? 1u : 0u) && !script.stop_before_join && script.sent_count() == 0,
              "only audio-only owner cleans up a confirmed but rejected route without any keepalive");
        check(error.find(owner ? "cleanup will request the device to stop streaming" : "wait for session expiry") != std::string::npos,
              "rejected routing diagnostic distinguishes stopAll ownership from video-safe expiry");
    }
}
void audio_only_recovery() {
    script.reset(); script.decoder_available = true; script.end_once = true;
    script.start_results = {Session::StartStatus::Started, Session::StartStatus::NotConfirmed,
                            Session::StartStatus::Started};
    scrctl::remote::Device device; std::string error;
    Pump::Options options; options.stop_device_on_exit = true;
    auto pump = Pump::start(device, options, error);
    check(pump != nullptr, "audio-only ordinary-recovery fixture starts");
    if (pump) {
        check(wait([] { return script.start_count() >= 2; }), "quiet state triggers a controlled unconfirmed recovery attempt");
        check(pump->terminal_error().empty(), "ordinary recovery failure and read timeout are not terminal audio errors");
        check(wait([&] { return pump->stats().restarts >= 1; }, 2500ms),
              "ordinary NotConfirmed recovery retries after the existing backoff and recovers");
        check(script.start_count() == 3 && pump->terminal_error().empty(),
              "successful recovery keeps terminal status clear without repeated routing requests");
        script.enqueue(rtp(30, 0, 0));
        check(wait([&] { return pump->stats().decoded == 1; }), "recovered audio-only session resumes the actual PCM loop");
        pump->stop(); pump.reset();
        check(script.stops == 1 && !script.stop_before_join,
              "successful audio-only recovery still stops device exactly once after join");
    }
    for (const bool owner : {false, true}) {
        script.reset(); script.decoder_available = true; script.end_once = true;
        script.stop_ok = false;
        script.start_results = {Session::StartStatus::Started, Session::StartStatus::AcceptedInvalidAnswer};
        options.stop_device_on_exit = owner;
        pump = Pump::start(device, options, error);
        check(pump != nullptr, "terminal-recovery fixture first establishes an actual confirmed audio session");
        if (!pump) continue;
        check(wait([&] { return !pump->terminal_error().empty(); }),
              "accepted-invalid recovery latches a readable terminal error");
        const auto first = pump->terminal_error();
        check(first.find("controlled accepted-invalid answer") == 0 && pump->receiver_port() == 0 &&
              pump->stats().restarts == 0, "invalid recovery clears live session data and never publishes a replacement");
        const auto starts = script.start_count(), sent = script.sent_count();
        std::this_thread::sleep_for(1100ms);
        check(starts == 2 && script.start_count() == starts && script.sent_count() == sent,
              "accepted-invalid recovery stops RR and does not retry after the normal one-second backoff");
        check(first.find(owner ? "cleanup will request the device to stop streaming" : "wait for session expiry") != std::string::npos,
              "recovery cleanup ownership uses a previously confirmed handle only when allowed");
        pump->stop(); pump->stop();
        check(script.stops == (owner ? 1u : 0u) && script.receivers == 0 && !script.stop_before_join,
              "only audio-only owner performs one stopAll after terminal recovery worker exits");
        check(pump->terminal_error() == first, "failed stopAll and repeated clear do not overwrite the negotiation first error");
        pump.reset();
        check(script.stops == (owner ? 1u : 0u), "destructor preserves stopAll idempotence after terminal recovery");
    }
}

#ifdef SCRCTL_HAVE_LIBAVFORMAT
const std::array<uint8_t, 16> video_session{1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
Bytes hex(std::string_view text) {
    Bytes bytes;
    for (std::size_t i = 0; i < text.size(); i += 2) {
        unsigned value = 0; std::sscanf(text.data() + i, "%2x", &value);
        bytes.push_back(static_cast<uint8_t>(value));
    }
    return bytes;
}
// 已有 recorder_test 的固定 FFmpeg/x265 64x64、bframes=0 样例，不要求运行时编码器。
const auto vps = hex("40010c01ffff01600000030090000003000003001eba0240");
const auto sps = hex("42010101600000030090000003000003001ea020810596e92930bc05a020000003002000000303c1");
const auto pps = hex("4401c073c089");
const auto idr = hex("2801ac21800e7ffeebf349ac");
struct Directory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("scrctl-audio-capture-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Directory() { std::filesystem::create_directory(path); }
    ~Directory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
std::unique_ptr<Recorder> recorder_at(const std::filesystem::path& path,
                                    std::size_t budget = 16u * 1024u * 1024u,
                                    bool include_video = true,
                                    std::chrono::milliseconds clock_wait = 5000ms) {
    Recorder::Options options;
    options.path = path.string(); options.include_audio = true; options.encoded_budget = budget;
    options.include_video = include_video;
    options.clock_wait = clock_wait;
    std::string error; auto recorder = Recorder::start(options, error);
    check(recorder != nullptr && error.empty() && recorder->includes_audio(),
          "actual Recorder starts with an immutable audio consumer for capture-only");
    if (recorder && include_video)
        check(recorder->begin_track(Recorder::Track::Video, video_session, 0), "actual video track is admitted");
    return recorder;
}
void seed_video(Recorder& recorder) {
    for (const int64_t ticks : {0, 2400, 4800})
        check(recorder.video(video_session, 0, ticks, {idr}, vps, sps, pps), "real recorder admits fixed complete HEVC AU");
    for (const auto ticks : {0u, 24000u}) {
        scrctl::rt::SenderReport sr;
        sr.ssrc = 0; sr.rtp_timestamp = ticks; sr.ntp_seconds = ticks == 0 ? 1000u : 1001u;
        check(recorder.sender_report(Recorder::Track::Video, video_session, sr), "real recorder admits video SR anchor");
    }
}
void missing_audio_consumer(const Directory& directory) {
    script.reset();
    Recorder::Options config; config.path = (directory.path / "video-only.mkv").string();
    std::string error; auto recorder = Recorder::start(config, error);
    check(recorder != nullptr && !recorder->includes_audio(), "actual video-only Recorder has no audio consumer");
    if (!recorder) return;
    scrctl::remote::Device device; Pump::Options options;
    options.decode_pcm = false; options.recorder = recorder.get();
    check(!Pump::start(device, options, error) &&
          error == "Audio capture without PCM decoding requires a container recording with an audio track" &&
          script.factories == 0 && script.regulators == 0 && script.start_count() == 0,
          "video-only Recorder cannot trigger capture-only decoder construction or device routing");
    check(recorder->error().empty(), "rejecting an audio request does not poison the video Recorder first error");
    check(recorder->begin_track(Recorder::Track::Video, video_session, 0),
          "video-only recording remains usable after the rejected audio request");
    seed_video(*recorder);
    check(recorder->finish(error) && error.empty() && !recorder->includes_audio(),
          "the unchanged video-only Recorder completes normally and keeps its selection");
}
void verify_file(const std::filesystem::path& path, bool include_video = true) {
    AVFormatContext* input = nullptr;
    check(avformat_open_input(&input, path.string().c_str(), nullptr, nullptr) >= 0 && input,
          "capture-only output opens as a real Matroska container");
    if (!input) return;
    check(input->nb_streams == (include_video ? 2u : 1u),
          "capture-only preserves exactly the selected recording tracks");
    AVPacket* packet = av_packet_alloc();
    check(packet != nullptr, "demux packet allocation succeeds");
    unsigned video = 0, audio = 0; int status = AVERROR_EOF;
    if (packet) while ((status = av_read_frame(input, packet)) >= 0) {
        const auto* stream = input->streams[packet->stream_index];
        const auto pts = av_rescale_q(packet->pts, stream->time_base, AVRational{1, 1000000});
        const Bytes bytes(packet->data, packet->data + packet->size);
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            check(stream->codecpar->codec_id == AV_CODEC_ID_AAC && bytes == silence,
                  "original stripped AAC bytes reach the container without decode or reencode");
            check(pts == static_cast<int64_t>(audio) * 10000 && packet->pts == packet->dts,
                  "audio SR mapping preserves the shared origin and 10ms PTS increments");
            ++audio;
        } else {
            Bytes expected{0,0,0,static_cast<uint8_t>(idr.size())}; expected.insert(expected.end(), idr.begin(), idr.end());
            check(include_video && stream->codecpar->codec_id == AV_CODEC_ID_HEVC && bytes == expected,
                  "capture-only audio leaves recorded HEVC bytes unchanged");
            check(pts == static_cast<int64_t>(video) * 100000, "video retains its independent SR-derived 24kHz clock");
            ++video;
        }
        av_packet_unref(packet);
    }
    check(status == AVERROR_EOF && audio == 3 && video == (include_video ? 3u : 0u),
          "late, duplicate and foreign packets do not create extra container samples");
    av_packet_free(&packet); avformat_close_input(&input);
}
void slow_audio_cleanup(const Directory& directory) {
    for (const unsigned scenario : {0u, 1u, 2u}) {
        const bool prior_recording_error = scenario == 2;
        const bool stop_ok = scenario == 0;
        script.reset(); script.stop_ok = stop_ok;
        script.stop_delay_ms = 600;
        // 保留真实循环的 50ms poll 边界，为 CI 调度留出余量；设备 RPC 仍
        // 明显超过缩短后的时钟预算，若 seal 在 RPC 之后，合法尾包会超时。
        const auto path = directory.path / ("slow-stop-" + std::to_string(scenario) + ".mkv");
        auto recorder = recorder_at(path, 16u * 1024u * 1024u, false, 250ms);
        if (!recorder) continue;
        scrctl::remote::Device device; std::string error;
        Pump::Options options;
        options.decode_pcm = false; options.stop_device_on_exit = true; options.recorder = recorder.get();
        auto pump = Pump::start(device, options, error);
        check(pump != nullptr, "slow cleanup fixture starts the actual audio-only receive loop");
        if (!pump) continue;
        script.enqueue(rtp(20, 0, 0));
        script.enqueue(sender_report(0, 0, 1000));
        script.enqueue(sender_report(0, 48000, 1001));
        // 采样点和编码区间都晚于最后 SR，只能在 Final 有界外推中写入。
        script.enqueue(rtp(21, 48480, 0));
        check(wait([&] { const auto s = pump->stats(); return s.packets == 2 && s.other_payload == 2; }),
              "real audio loop admits a valid tail beyond the last trusted sender report");
        pump->stop_receiving();
        pump->stop_receiving();
        check(script.stops == 0 && script.receivers == 0 && pump->receiver_port() == 49152,
              "receive stop joins without device RPC and retains the confirmed cleanup handle");
        const auto sent = script.sent_count();
        const std::string first_error = "controlled capture first error";
        if (prior_recording_error) recorder->fail(first_error);
        const bool recording_ok = recorder->finish(error);
        check(prior_recording_error ? !recording_ok && error == first_error : recording_ok && error.empty(),
              "Recorder seals before slow device cleanup, preserving a valid Final tail or its first error");
        const auto stop_started = std::chrono::steady_clock::now();
        pump->stop();
        check(std::chrono::steady_clock::now() - stop_started >= 500ms,
              "the actual cleanup boundary stays blocked longer than the 250ms recording budget");
        const auto cleanup_error = pump->terminal_error();
        check(cleanup_error == (stop_ok ? "" : "controlled stop failure"),
              "slow stop failure remains a readable cleanup error after recording has finished");
        const bool overall_ok = recording_ok && cleanup_error.empty();
        check(overall_ok == (scenario == 0),
              "cleanup failure cannot be reported as an overall successful exit");
        if (prior_recording_error)
            check(recorder->error() == first_error, "slow cleanup never overwrites the Recorder's prior capture error");
        else
            check(recorder->error().empty(), "a completed recording remains valid after slow or failed stopAll");
        check(script.stops == 1 && script.receivers == 0 && !script.stop_before_join &&
              script.sent_count() == sent && pump->receiver_port() == 0,
              "slow cleanup sends one stopAll only after producer join and leaves no RR or live session");
        pump->stop_receiving(); pump->stop(); pump.reset();
        check(script.stops == 1 && script.sent_count() == sent,
              "repeated receive stop, cleanup and destruction are idempotent after a slow RPC");
        check(prior_recording_error ? !recorder->finish(error) && error == first_error :
                                     recorder->finish(error) && error.empty(),
              "repeated Recorder finish retains its completed result independently of cleanup status");
        if (prior_recording_error) continue;
        AVFormatContext* input = nullptr;
        check(avformat_open_input(&input, path.string().c_str(), nullptr, nullptr) >= 0 && input,
              "Final tail output opens as an actual audio-only container after slow cleanup");
        if (!input) continue;
        check(input->nb_streams == 1 && input->streams[0]->codecpar->codec_id == AV_CODEC_ID_AAC,
              "Final tail output has only the selected AAC track");
        AVPacket* packet = av_packet_alloc();
        check(packet != nullptr, "Final tail demux packet allocation succeeds");
        unsigned packets = 0;
        int status = AVERROR_EOF;
        if (packet) while ((status = av_read_frame(input, packet)) >= 0) {
            const auto* stream = input->streams[packet->stream_index];
            const auto pts = av_rescale_q(packet->pts, stream->time_base, AVRational{1, 1000000});
            const auto evidence = recording_test::inspect_audio_packet(*input, *packet);
            check(Bytes(packet->data, packet->data + packet->size) == silence,
                  "SR-approved first packet and Final-only tail retain original AAC bytes");
            check(packet->pts == packet->dts && pts == (packets == 0 ? 0 : 1010000),
                  "SR-approved first packet and Final-only tail retain exact PTS/DTS across the gap");
            check(evidence.frame_valid(), "Final-only tail actually decodes to 480 samples at 48kHz stereo");
            check(evidence.duration_valid(), "Final-only tail has 10ms duration or MKV's missing field with actual AAC proof");
            if (Bytes(packet->data, packet->data + packet->size) != silence ||
                !evidence.frame_valid() || !evidence.duration_valid() || packet->pts != packet->dts ||
                pts != (packets == 0 ? 0 : 1010000))
                std::fprintf(stderr, "AUDIO: %s\n", evidence.diagnostic().c_str());
            ++packets; av_packet_unref(packet);
        }
        check(status == AVERROR_EOF && packets == 2,
              "slow or failed stopAll cannot discard the valid tail packet sealed before its RPC");
        const auto tail = recording_test::inspect_audio_tail(*input, 0);
        check(tail.valid(1020000), "Final-only audio tail ends at 1020ms after its 1010ms packet PTS");
        if (!tail.valid(1020000))
            std::fprintf(stderr, "TAIL: %s\n", tail.diagnostic(1020000).c_str());
        av_packet_free(&packet); avformat_close_input(&input);
    }
}
void successful_capture(const Directory& directory, bool source_known, bool duplicate_audio,
                        bool send_failure = false, bool include_video = true) {
    script.reset(); script.local_present = source_known; script.local = source_known ? 0u : 9u;
    script.send_ok = !send_failure;
    const uint32_t source = script.local;
    const auto path = directory.path / (!include_video ? "audio-only.mkv" : send_failure ? "rr-failure.mkv" :
                                      (source_known ? "negotiated-zero.mkv" : "first-media.mkv"));
    auto recorder = recorder_at(path, 16u * 1024u * 1024u, include_video); if (!recorder) return;
    if (include_video) seed_video(*recorder);
    scrctl::remote::Device device; std::string error;
    Pump::Options options; options.decode_pcm = false; options.audio_dup = duplicate_audio; options.recorder = recorder.get();
    options.stop_device_on_exit = !include_video;
    auto pump = Pump::start(device, options, error);
    check(pump != nullptr && error.empty(), "capture-only starts even when the PCM decoder factory is unavailable");
    if (!pump) return;
    scrctl::app::AudioOut output;
    check(!output.open(*pump, error) && script.opens == 0 && error.find("PCM decoding is disabled") != std::string::npos,
          "capture-only playback request fails before opening SDL and preserves recording");
    script.enqueue(sender_report(55, 0, 2000)); // SR 不可为缺失 LocalSSRC 绑定来源。
    script.enqueue(rtp(40000, 0, 777, silence, false, 100));
    script.enqueue(rtp(65535, 0, source)); script.enqueue(rtp(0, 480, source, silence, true));
    script.enqueue(rtp(0, 480, source)); script.enqueue(rtp(65535, 0, source));
    script.enqueue(rtp(10000, 90000, 88));
    script.enqueue(sender_report(source, 0, 1000)); script.enqueue(sender_report(source, 48000, 1001));
    script.enqueue(rtp(1, 960, source));
    check(wait([&] { const auto s = pump->stats(); return s.packets == 5 && s.other_payload == 4; }),
          "real loop filters PT/SSRC and distinguishes SR, wraparound, duplicate and late packets");
    if (source_known) {
        check(wait([&] { return script.sent_count() > 0; }), "capture-only continues independent RTCP keepalive");
        std::lock_guard lock(script.mutex);
        if (!script.sent.empty()) {
            const auto& [rr, port] = script.sent.front();
            check(rr.size() == 32 && rr[1] == 201 && port == 24680 && get32(rr, 4) == 123 &&
                  get32(rr, 8) == 0 && get32(rr, 16) == 65537,
                  "capture-only RR preserves negotiated identities and port with the extended wrap sequence");
        }
    }
    pump->stop();
    const auto stats = pump->stats();
    if (source_known)
        check(send_failure ? stats.rtcp_failed >= 1 && stats.rtcp_sent == 0 :
                             stats.rtcp_sent >= 1 && stats.rtcp_failed == 0,
              "capture-only retains separate RTCP success and send-failure accounting");
    check(stats.seq_gaps == 0 && stats.seq_lost == 0 && stats.out_of_order == 2,
          "late and duplicate accounting is retained without recording repeated AAC");
    no_pcm(*pump);
    std::array<int16_t, 8> pcm; pcm.fill(29);
    check(pump->read(pcm.data(), 4) == 0 && pcm.front() == 29 && pcm.back() == 29,
          "capture-only read returns no PCM and leaves caller storage untouched");
    { std::lock_guard lock(script.mutex);
      check(script.starts.size() == 1 && script.starts.front().audio &&
            script.starts.front().offer.audio_dup == duplicate_audio && script.starts.front().timeout_seconds == 20,
            "PCM demand does not alter audio route or lease request"); }
    const auto sent = script.sent_count(); std::this_thread::sleep_for(60ms);
    check(script.sent_count() == sent && script.stops == (include_video ? 0u : 1u) && !script.stop_before_join,
          "stop joins capture worker and stops RR; only sole audio owner sends one stopAll");
    check(recorder->finish(error) && error.empty(), "capture-only actual recording finishes after producer join");
    verify_file(path, include_video);
    if (!include_video) {
        pump->stop(); pump.reset();
        check(script.stops == 1 && script.sent_count() == sent,
              "audio-only capture cleanup and destruction keep stopAll idempotent after Recorder seal");
    }
}
void capture_errors(const Directory& directory) {
    for (const bool budget_failure : {false, true}) {
        script.reset(); auto recorder = recorder_at(directory.path / (budget_failure ? "budget.mkv" : "gap.mkv"),
                                                    budget_failure ? 512u : 16u * 1024u * 1024u);
        if (!recorder) continue;
        scrctl::remote::Device device; std::string error; Pump::Options options;
        options.decode_pcm = false; options.recorder = recorder.get();
        auto pump = Pump::start(device, options, error); check(pump != nullptr, "failure fixture starts actual capture loop");
        if (!pump) continue;
        if (budget_failure) script.enqueue(rtp(10, 0, 0, Bytes(300, 0x42)));
        else {
            script.enqueue(rtp(10, 0, 0)); script.enqueue(rtp(12, 960, 0));
            script.enqueue(rtp(11, 480, 0)); script.enqueue(rtp(12, 960, 0));
        }
        check(wait([&] { return !recorder->error().empty() && pump->stats().packets == (budget_failure ? 1u : 4u); }),
              "encoded admission or transport loss still seals the actual recorder in capture-only mode");
        pump->stop(); no_pcm(*pump);
        const auto first = recorder->error();
        check(first == (budget_failure ? "Recorder input exceeds its encoded-byte or event budget" :
                                         "Audio packet loss ended container recording"),
              "capture-only preserves the first concrete recording error");
        if (!budget_failure) {
            const auto stats = pump->stats();
            check(stats.seq_gaps == 1 && stats.out_of_order == 2 && stats.seq_lost == 0,
                  "a late gap fill updates loss statistics but cannot repair a failed recording");
        }
        check(!recorder->finish(error) && error == first, "finish preserves the original capture error over missing clocks");
        check(!recorder->finish(error) && error == first, "repeated finish returns the same recording failure");
    }
    script.reset(); script.end_once = true;
    auto recorder = recorder_at(directory.path / "restart.mkv"); if (!recorder) return;
    scrctl::remote::Device device; std::string error; Pump::Options options;
    options.decode_pcm = false; options.audio_dup = true; options.recorder = recorder.get();
    auto pump = Pump::start(device, options, error); check(pump != nullptr, "restart fixture starts capture-only session");
    if (!pump) return;
    script.enqueue(rtp(65000, 0, 0));
    check(wait([&] { return pump->stats().restarts == 1; }), "real quiet timer detects ended audio and restarts capture-only session");
    script.enqueue(rtp(100, 0, 0));
    check(wait([&] { return pump->stats().packets == 2; }), "new capture epoch accepts its independent sequence space");
    pump->stop(); no_pcm(*pump);
    { std::lock_guard lock(script.mutex);
      check(script.starts.size() == 2 && script.starts[0].offer.audio_dup && script.starts[1].offer.audio_dup,
            "audio recovery preserves phone-and-computer routing without constructing PCM"); }
    check(pump->stats().seq_gaps == 0 && pump->stats().seq_lost == 0 && script.stops == 0,
          "restart clears old audio sequence state and never stops the video session");
    const auto first = recorder->error();
    check(first == "Device ended the recorded audio session" && !recorder->finish(error) && error == first,
          "session-end first error survives subsequent recreate and recording finalization");
}
#endif
} // namespace

namespace scrctl {
std::unique_ptr<AudioDecoder> create_audio_decoder(int, int, int, std::string& error) {
    ++script.factories;
    if (!script.decoder_available) { error = "controlled decoder unavailable"; return nullptr; }
    error.clear(); return std::make_unique<Decoder>();
}
} // namespace scrctl
namespace scrctl::media {
std::unique_ptr<StreamSession> StreamSession::start(remote::Device&, const Request& request, std::string& error,
        bool, remote::ServiceConnection*, StartStatus* status) {
    std::lock_guard lock(script.mutex); script.starts.push_back(request);
    const auto result = script.start_results.empty() ? StartStatus::Started : script.start_results.front();
    if (!script.start_results.empty()) script.start_results.pop_front();
    if (status) *status = result;
    if (result != StartStatus::Started) {
        error = result == StartStatus::AcceptedInvalidAnswer ? "controlled accepted-invalid answer" : "controlled unconfirmed request";
        return nullptr;
    }
    Started started; started.sender_port = 24680; started.payload_type = 101;
    started.local_ssrc = script.local; started.has_local_ssrc = script.local_present;
    started.remote_ssrc = 123; started.has_remote_ssrc = true;
    started.session_uuid.assign(16, static_cast<uint8_t>(script.starts.size()));
    auto config = xpc::make_dict(); xpc::dict_set(config, "AudioStreamMode", xpc::make_int64(script.route_mode.value_or(request.offer.audio_dup ? 8 : 10)));
    auto connection = xpc::make_dict(); xpc::dict_set(connection, "streamConfig", std::move(config));
    started.answer = xpc::make_dict(); xpc::dict_set(started.answer, "connection", std::move(connection));
    error.clear(); if (status) *status = StartStatus::Started;
    return std::unique_ptr<StreamSession>(new StreamSession(nullptr, std::move(started)));
}
StreamSession::~StreamSession() = default;
uint16_t StreamSession::receiver_port() const { return 49152; }
bool StreamSession::next_packet(Bytes& packet, int timeout, std::string& error) {
    uint16_t port = 0; return next_packet(packet, port, timeout, error);
}
bool StreamSession::next_packet(Bytes& packet, uint16_t& port, int timeout, std::string& error) {
    struct Receiving {
        Receiving() { ++script.receivers; }
        ~Receiving() { --script.receivers; }
    } receiving;
    std::unique_lock lock(script.mutex);
    if (!script.ready.wait_for(lock, std::chrono::milliseconds(timeout), [] { return !script.packets.empty(); })) {
        error = "controlled timeout"; return false;
    }
    packet = std::move(script.packets.front()); script.packets.pop_front(); port = 24680; error.clear(); return true;
}
bool StreamSession::send_rtp(const Bytes& payload, uint16_t port, std::string& error) {
    std::lock_guard lock(script.mutex); script.sent.emplace_back(payload, port);
    error = script.send_ok ? "" : "controlled send failure"; return script.send_ok;
}
bool StreamSession::stop(remote::Device&, std::string& error, bool) const {
    int delay_ms = 0;
    bool result = false;
    {
        std::lock_guard lock(script.mutex); ++script.stops;
        if (script.receivers != 0) script.stop_before_join = true;
        delay_ms = script.stop_delay_ms; result = script.stop_ok;
    }
    if (delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    error = result ? "" : "controlled stop failure";
    return result;
}
StreamSession::ServerState StreamSession::probe(remote::Device&, const Bytes&, std::string& error, bool) {
    std::lock_guard lock(script.mutex); error.clear();
    if (script.end_once) { script.end_once = false; return ServerState::Ended; }
    return ServerState::Alive;
}
// 无 PCM 时连工厂也不应调用；默认路径可用既有“无软补偿、直接 PCM”返回值。
struct AudioRegulator::Impl {};
AudioRegulator::AudioRegulator(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
AudioRegulator::~AudioRegulator() = default;
std::unique_ptr<AudioRegulator> AudioRegulator::create(int, int, std::size_t, std::string& error) {
    ++script.regulators; error.clear(); return nullptr;
}
bool AudioRegulator::process(std::span<const int16_t>, std::vector<int16_t>&, std::string&) { return false; }
bool AudioRegulator::observe(const Observation&, std::string&) { return false; }
void AudioRegulator::reset() {}
AudioRegulator::Stats AudioRegulator::stats() const { return {}; }
} // namespace scrctl::media
extern "C" SDL_AudioDeviceID SDLCALL SDL_OpenAudioDevice(const char*, int, const SDL_AudioSpec*, SDL_AudioSpec*, int) {
    ++script.opens; SDL_SetError("controlled audio device unavailable"); return 0;
}
int main() {
    gates_and_default();
    audio_only_cleanup();
    initial_rejection_cleanup();
    audio_only_recovery();
#ifdef SCRCTL_HAVE_LIBAVFORMAT
    const Directory directory;
    missing_audio_consumer(directory);
    successful_capture(directory, true, false);
    successful_capture(directory, false, true);
    successful_capture(directory, true, true, true);
    successful_capture(directory, true, false, false, false);
    slow_audio_cleanup(directory);
    capture_errors(directory);
#else
    std::printf("SKIP: actual container capture needs libavformat; startup/default PCM gates still ran\n");
#endif
    std::printf("audio_capture: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
