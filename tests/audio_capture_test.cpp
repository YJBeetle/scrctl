#include "app/AudioOut.h"
#include "media/AudioPump.h"
#include "media/Recorder.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <mutex>
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
    std::vector<std::pair<Bytes, uint16_t>> sent;
    std::vector<Bytes> decoded;
    bool local_present = true, end_once = false, send_ok = true;
    uint32_t local = 0;
    std::atomic<unsigned> factories{0}, regulators{0}, pcm_calls{0}, opens{0}, stops{0};
    bool decoder_available = false;
    void reset() {
        std::lock_guard lock(mutex);
        packets.clear(); starts.clear(); sent.clear(); decoded.clear();
        local_present = true; end_once = false; send_ok = true; local = 0;
        factories = 0; regulators = 0; pcm_calls = 0; opens = 0; stops = 0;
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
    check(options.decode_pcm, "existing callers decode PCM by default");
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
    check(script.stops == 0, "stopping the audio pump never sends device stopAll");
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
std::unique_ptr<Recorder> recorder_at(const std::filesystem::path& path, std::size_t budget = 16u * 1024u * 1024u) {
    Recorder::Options options;
    options.path = path.string(); options.include_audio = true; options.encoded_budget = budget;
    std::string error; auto recorder = Recorder::start(options, error);
    check(recorder != nullptr && error.empty() && recorder->includes_audio(),
          "actual AV Recorder starts with an immutable audio consumer for capture-only");
    if (recorder) check(recorder->begin_track(Recorder::Track::Video, video_session, 0), "actual video track is admitted");
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
void verify_file(const std::filesystem::path& path) {
    AVFormatContext* input = nullptr;
    check(avformat_open_input(&input, path.string().c_str(), nullptr, nullptr) >= 0 && input,
          "capture-only output opens as a real Matroska container");
    if (!input) return;
    check(input->nb_streams == 2, "capture-only preserves both HEVC and AAC tracks");
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
            check(stream->codecpar->codec_id == AV_CODEC_ID_HEVC && bytes == expected,
                  "capture-only audio leaves recorded HEVC bytes unchanged");
            check(pts == static_cast<int64_t>(video) * 100000, "video retains its independent SR-derived 24kHz clock");
            ++video;
        }
        av_packet_unref(packet);
    }
    check(status == AVERROR_EOF && audio == 3 && video == 3,
          "late, duplicate and foreign packets do not create extra container samples");
    av_packet_free(&packet); avformat_close_input(&input);
}
void successful_capture(const Directory& directory, bool source_known, bool duplicate_audio,
                        bool send_failure = false) {
    script.reset(); script.local_present = source_known; script.local = source_known ? 0u : 9u;
    script.send_ok = !send_failure;
    const uint32_t source = script.local;
    const auto path = directory.path / (send_failure ? "rr-failure.mkv" :
                                      (source_known ? "negotiated-zero.mkv" : "first-media.mkv"));
    auto recorder = recorder_at(path); if (!recorder) return; seed_video(*recorder);
    scrctl::remote::Device device; std::string error;
    Pump::Options options; options.decode_pcm = false; options.audio_dup = duplicate_audio; options.recorder = recorder.get();
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
    check(script.sent_count() == sent && script.stops == 0, "stop joins capture worker and stops RR without device stopAll");
    check(recorder->finish(error) && error.empty(), "capture-only actual AV recording finishes after producer join");
    verify_file(path);
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
    Started started; started.sender_port = 24680; started.payload_type = 101;
    started.local_ssrc = script.local; started.has_local_ssrc = script.local_present;
    started.remote_ssrc = 123; started.has_remote_ssrc = true;
    started.session_uuid.assign(16, static_cast<uint8_t>(script.starts.size()));
    auto config = xpc::make_dict(); xpc::dict_set(config, "AudioStreamMode", xpc::make_int64(request.offer.audio_dup ? 8 : 10));
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
bool StreamSession::stop(remote::Device&, std::string& error, bool) const { ++script.stops; error.clear(); return true; }
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
#ifdef SCRCTL_HAVE_LIBAVFORMAT
    const Directory directory;
    missing_audio_consumer(directory);
    successful_capture(directory, true, false);
    successful_capture(directory, false, true);
    successful_capture(directory, true, true, true);
    capture_errors(directory);
#else
    std::printf("SKIP: actual container capture needs libavformat; startup/default PCM gates still ran\n");
#endif
    std::printf("audio_capture: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
