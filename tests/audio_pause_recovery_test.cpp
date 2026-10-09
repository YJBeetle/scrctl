#include "app/AudioOut.h"
#include "media/AudioPump.h"
#include "rt/Rtcp.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <span>
#include <vector>

namespace {
using Bytes = std::vector<uint8_t>;
using Pump = scrctl::media::AudioPump;
using Session = scrctl::media::StreamSession;
using namespace std::chrono_literals;
constexpr int kRate = 48000;
constexpr int kPacket = 480;
constexpr int kCallback = 1024;
constexpr uint32_t kSource = 7;
int failures = 0;
int checks = 0;

bool check(bool condition, const char *message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
    return condition;
}

struct Source {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<Bytes> packets;
    std::size_t submitted = 0;
    std::size_t fetched = 0;
    std::size_t completed = 0;
    unsigned sessions = 0;
    uint16_t sequence = 0;
    SDL_AudioSpec audio{};

    bool send(Bytes packet) {
        std::unique_lock lock(mutex);
        const auto target = ++submitted;
        packets.push_back(std::move(packet));
        ready.notify_all();
        // 下一次接收说明上一个数据报已经经过真实 loop/push/observe。
        return ready.wait_for(lock, 3s, [&] { return completed >= target; });
    }

    bool pcm() {
        Bytes packet(14, 0);
        packet[0] = 0x80;
        packet[1] = 101;
        packet[2] = static_cast<uint8_t>(sequence >> 8);
        packet[3] = static_cast<uint8_t>(sequence);
        ++sequence;
        packet[11] = kSource;
        return send(std::move(packet));
    }

    std::vector<int16_t> render() const {
        std::vector<int16_t> pcm(kCallback * 2, 123);
        audio.callback(audio.userdata, reinterpret_cast<Uint8 *>(pcm.data()),
                       static_cast<int>(pcm.size() * sizeof(int16_t)));
        return pcm;
    }
} source;

class Decoder final : public scrctl::AudioDecoder {
public:
    bool decode(std::span<const uint8_t>, std::vector<int16_t> &pcm,
                std::string &error) override {
        pcm.resize(kPacket * 2);
        for (int i = 0; i < kPacket; ++i, ++frame_) {
            const auto value = static_cast<int16_t>(12000 * std::sin(
                2 * 3.14159265358979323846 * 440 * static_cast<double>(frame_) / kRate));
            pcm[i * 2] = pcm[i * 2 + 1] = value;
        }
        error.clear();
        return true;
    }
    const char *backend_name() const override { return "controlled-pcm"; }
private:
    std::size_t frame_ = 0;
};

void pause_recovery() {
    scrctl::remote::Device device;
    Pump::Options options;
    options.target_backlog_ms = 200;
    std::string error;
    auto pump = Pump::start(device, options, error);
    if (!check(pump != nullptr && error.empty(), "real PCM pipeline starts")) return;
    scrctl::app::AudioOut output;
    if (!check(output.open(*pump, error), "controlled soundcard opens the real AudioOut callback")) return;
    const auto produce = [] { return check(source.pcm(), "RTP passed the real receive and PCM push path"); };

    for (int i = 0; i < 21; ++i) if (!produce()) return;
    source.render();
    // 消费时钟稍快，先使真实重采样器处于非零补偿，验证恢复会撤销旧速率。
    for (int i = 0; i < 250; ++i) {
        if (!produce() || !produce()) return;
        source.render();
    }
    const auto warm = pump->stats().clock;
    check(warm.active && warm.compensation_ppm > 0 && warm.added_frames > 0,
          "warmup uses the real libswresample clock correction");
    check(pump->stats().decoded == 521, "warmup packets are decoded by the production receive loop");
    while (pump->buffered_frames() != 0) source.render();
    for (int i = 0; i < 6; ++i) source.render();
    if (!produce()) return;
    const auto before = pump->stats().clock;
    check(before.compensation_ppm == warm.compensation_ppm &&
              before.compensation_updates == warm.compensation_updates,
          "a 128 ms empty PCM gap keeps the existing compensation between updates");
    while (pump->buffered_frames() != 0) source.render();
    const auto delivered = output.delivered();
    const auto silence = output.underrun_silence();
    const auto underruns = output.underrun_callbacks();
    const auto decoded = pump->stats().decoded;

    // 通过生产消费入口推进31秒无PCM，避免真的等待31秒。匹配的SR仍然到达。
    constexpr int empty_callbacks = kRate * 31 / kCallback;
    for (int i = 0; i < empty_callbacks; ++i) {
        const auto pcm = source.render();
        if (!check(std::all_of(pcm.begin(), pcm.end(), [](int16_t x) { return x == 0; }),
                   "empty playback callback actually outputs silence")) return;
        if (i % 48 == 0 && !check(source.send(scrctl::rt::build_sr(kSource, 0, 0)),
                                  "RTCP SR continues during the PCM pause")) return;
    }
    check(pump->stats().decoded == decoded && pump->stats().other_payload > 0 &&
              pump->buffered_frames() == 0 && output.delivered() == delivered,
          "control packets do not turn the empty PCM stream into active audio");
    check(output.underrun_silence() == silence + static_cast<uint64_t>(empty_callbacks) * kCallback &&
              output.underrun_callbacks() == underruns + empty_callbacks,
          "all real underrun silence and callback totals are retained");

    if (!produce()) return;
    const auto resumed = pump->stats().clock;
    check(resumed.active && resumed.compensation_ppm == 0 &&
              resumed.compensation_updates == before.compensation_updates,
          "first resumed PCM clears stale compensation without erasing update totals");
    check(resumed.average_frames < kRate / 20 &&
              resumed.average_frames <= static_cast<double>(pump->buffered_frames() + kCallback),
          "resume estimate describes the small live queue instead of 31 seconds of silence");
    check(resumed.added_frames >= before.added_frames && resumed.removed_frames >= before.removed_frames,
          "converter lifetime totals survive the pause reanchor");
    const auto after_pause_silence = output.underrun_silence();
    const auto resumed_pcm = source.render();
    check(output.delivered() > delivered &&
              std::any_of(resumed_pcm.begin(), resumed_pcm.end(), [](int16_t x) { return x != 0; }),
          "resumed audio is consumed immediately without repeating startup preroll");
    check(output.underrun_silence() >= after_pause_silence,
          "resume preserves cumulative playback underrun accounting");

    // 等速的480帧输入与1024帧回调，仅调度边界；水位/补偿完全由生产代码计算。
    std::size_t input_frames = 0;
    std::size_t consumed_frames = 0;
    for (int i = 0; i < 1600; ++i) {
        if (!produce()) return;
        input_frames += kPacket;
        while (consumed_frames + kCallback <= input_frames) {
            source.render();
            consumed_frames += kCallback;
        }
    }
    const auto final = pump->stats();
    check(source.sessions == 1 && final.restarts == 0 && final.clock_failed == 0,
          "pause recovery does not recreate the device session or fail resampling");
    check(final.clock.average_frames > kRate / 10 && final.clock.average_frames < kRate * 3 / 10 &&
              pump->buffered_frames() < kRate / 2 && final.dropped_stale == 0 && final.steered == 0,
          "continuous PCM returns near the 200 ms target without hard trimming");
    std::printf("pause recovery: initial compensation=%d ppm, first resume mean=%.1f frames, "
                "final mean=%.1f frames, queue=%zu frames, underrun silence=%llu frames\n",
                before.compensation_ppm, resumed.average_frames, final.clock.average_frames,
                pump->buffered_frames(), static_cast<unsigned long long>(output.underrun_silence()));
    output.close();
    pump->stop();
}
} // namespace

namespace scrctl {
std::unique_ptr<AudioDecoder> create_audio_decoder(int, int, int, std::string &error) {
    error.clear();
    return std::make_unique<Decoder>();
}
} // namespace scrctl

namespace scrctl::media {
std::unique_ptr<StreamSession> StreamSession::start(remote::Device &, const Request &,
        std::string &error, bool, remote::ServiceConnection *, StartStatus *status) {
    Started started;
    started.payload_type = 101;
    started.sender_port = 24680;
    started.local_ssrc = kSource;
    started.remote_ssrc = 123;
    started.has_local_ssrc = started.has_remote_ssrc = true;
    started.session_uuid.assign(16, 1);
    auto config = xpc::make_dict();
    xpc::dict_set(config, "AudioStreamMode", xpc::make_int64(10));
    auto connection = xpc::make_dict();
    xpc::dict_set(connection, "streamConfig", std::move(config));
    started.answer = xpc::make_dict();
    xpc::dict_set(started.answer, "connection", std::move(connection));
    ++source.sessions;
    error.clear();
    if (status) *status = StartStatus::Started;
    return std::unique_ptr<StreamSession>(new StreamSession(nullptr, std::move(started)));
}
StreamSession::~StreamSession() = default;
uint16_t StreamSession::receiver_port() const { return 49152; }
bool StreamSession::next_packet(Bytes &packet, uint16_t &port, int timeout, std::string &error) {
    std::unique_lock lock(source.mutex);
    source.completed = source.fetched;
    source.ready.notify_all();
    if (!source.ready.wait_for(lock, std::chrono::milliseconds(timeout), [] { return !source.packets.empty(); })) {
        error = "controlled timeout";
        return false;
    }
    packet = std::move(source.packets.front());
    source.packets.pop_front();
    ++source.fetched;
    port = 24680;
    error.clear();
    return true;
}
bool StreamSession::send_rtp(const Bytes &, uint16_t, std::string &error) { error.clear(); return true; }
bool StreamSession::stop(remote::Device &, std::string &error, bool) const { error.clear(); return true; }
StreamSession::ServerState StreamSession::probe(remote::Device &, const Bytes &, std::string &error, bool) {
    error.clear();
    return ServerState::Alive;
}
} // namespace scrctl::media

extern "C" SDL_AudioDeviceID SDLCALL SDL_OpenAudioDevice(const char *, int, const SDL_AudioSpec *want,
        SDL_AudioSpec *have, int) {
    source.audio = *want;
    *have = *want;
    have->size = kCallback * 2 * sizeof(int16_t);
    return 1;
}
extern "C" void SDLCALL SDL_CloseAudioDevice(SDL_AudioDeviceID) {}
extern "C" void SDLCALL SDL_PauseAudioDevice(SDL_AudioDeviceID, int) {}
extern "C" const char *SDLCALL SDL_GetCurrentAudioDriver() { return "controlled-callback"; }

int main() {
    pause_recovery();
    std::printf("audio_pause_recovery: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
