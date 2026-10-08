#include "i18n/Translation.h"
#include "media/AudioPump.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <span>
#include <thread>
#include <utility>

#include "remote/Device.h"
#include "rt/Rtcp.h"
#include "rt/RtpHevc.h"
#include "rt/RtpSeq.h"

namespace scrctl::media {
namespace {

/// 每秒发送一个 RR，延长设备的 RTCP 空闲租期。音频会话独立计时，
/// 不能依赖视频 RR 续期，验证见 docs/coredevice.md §13。
constexpr uint64_t kRtcpPeriodMs = 1000;

/// 每轮收包的等待上限，兼顾 CPU 占用、停止响应和 RTCP 调度。
constexpr int kPollTimeoutMs = 50;

/// 环形缓冲容量至少为半秒，用于容纳短暂接收、消费抖动。
/// 实际容量随目标水位增大，确保预滚阈值可达，见 compute_waterline()。
constexpr std::size_t kMinRingCapacityFrames = 24000;

/// 限制最大缓冲时长，控制实时播放延迟和命令行参数导致的内存分配。
constexpr int kMaxTargetBacklogMs = 1000;

/// 音频包静默超过此时长后查询设备会话状态。
/// 实测静音内容仍约每秒发送 100 个包，故无音频包可作为查询理由，
/// 但不能据此直接认定会话结束。视频重建也可能使音频会话结束，
/// 400 ms 查询阈值用于缩短这种中断；对照见 docs/coredevice.md §17.2。
constexpr uint64_t kQuietProbeMs = 400;

uint64_t now_ms() {
    using clock = std::chrono::steady_clock;
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch())
            .count());
}

}  // namespace

AudioPump::Waterline AudioPump::compute_waterline(const Options &options) {
    Waterline w;
    int ms = options.target_backlog_ms;
    if (ms > kMaxTargetBacklogMs) {
        w.clamped_to_ms = kMaxTargetBacklogMs;
        ms = kMaxTargetBacklogMs;
    }
    if (ms < 0) {
        ms = 0;  // 0 = 不攒，取多少给多少；负数没有意义
    }
    const std::size_t rate =
        options.sample_rate > 0 ? static_cast<std::size_t>(options.sample_rate) : 48000;
    w.target_frames = static_cast<std::size_t>(ms) * rate / 1000;
    // 容量大于两倍水位，使 read() 的积压调节先于环满丢弃生效。
    w.capacity_frames =
        std::max<std::size_t>(kMinRingCapacityFrames, w.target_frames * 4 + w.target_frames / 4);
    // 保持预滚水位可达的约束，避免容量下限修改后无法开始播放。
    if (w.target_frames >= w.capacity_frames) {
        w.capacity_frames = w.target_frames * 4 + 1;
    }
    return w;
}

AudioPump::ReadTrim AudioPump::compute_read_trim(std::size_t buffered,
                                                std::size_t requested,
                                                std::size_t target,
                                                int frame_length, bool first_read) {
    if (requested == 0 || buffered <= requested) {
        return {};
    }
    const std::size_t remaining = buffered - requested;
    if (remaining <= target) {
        return {};
    }
    const std::size_t excess = remaining - target;
    if (first_read) {
        return {excess, true};
    }
    const std::size_t packet_frames = frame_length > 0 ? static_cast<std::size_t>(frame_length) : 480;
    // 等价于 remaining > 2 * target + packet_frames，但分步相减避免极值相加溢出。
    if (excess > target && excess - target > packet_frames) {
        return {excess, false};
    }
    return {};
}

std::unique_ptr<AudioPump> AudioPump::start(remote::Device &device, const Options &options,
                                           std::string &err, bool verbose) {
    // 起流前计算并提示被限制的配置，与构造函数采用相同纯函数。
    const Waterline w = compute_waterline(options);
    if (w.clamped_to_ms != 0) {
        std::printf(SCRCTL_TR("Clamped --audio-buffer %d to %d ms and adjusted buffer capacity\n"),
                    options.target_backlog_ms, w.clamped_to_ms);
    }
    auto pump = std::unique_ptr<AudioPump>(new AudioPump(device, options));
    pump->verbose_ = verbose;
    if (!pump->start_session(err)) {
        return nullptr;
    }
    std::string clock_error;
    pump->regulator_ = AudioRegulator::create(options.sample_rate, options.channels,
                                              pump->target_frames_, clock_error);
    if (pump->regulator_ != nullptr) {
        pump->stats_.clock = pump->regulator_->stats();
    } else if (!clock_error.empty()) {
        pump->report_clock_failure(clock_error);
    }
    pump->worker_ = std::thread([raw = pump.get()] { raw->loop(); });
    return pump;
}

AudioPump::~AudioPump() { stop(); }

bool AudioPump::validate_stream_mode(const StreamSession::Started &started,
                                     const Options &options, std::string &err) {
    const auto *mode = started.answer.at("connection").at("streamConfig").find("AudioStreamMode");
    uint64_t value = 0;
    if (mode != nullptr && mode->type == xpc::Type::UInt64) {
        value = mode->uint64;
    } else if (mode != nullptr && mode->type == xpc::Type::Int64 && mode->int64 >= 0) {
        value = static_cast<uint64_t>(mode->int64);
    } else {
        err = SCRCTL_TR("Audio routing was not confirmed: answer has no valid AudioStreamMode");
        return false;
    }
    const uint64_t expected = options.audio_dup ? 8 : 10;
    if (value != expected) {
        err = options.audio_dup
            ? SCRCTL_TR("Phone-and-computer audio routing was not accepted (--audio-dup requires AudioStreamMode 8)")
            : SCRCTL_TR("Computer-only audio routing was not accepted (requires AudioStreamMode 10); not falling back to --audio-dup");
        return false;
    }
    err.clear();
    return true;
}

void AudioPump::reject_negotiation(std::string &err) {
    negotiation_invalid_ = true;
    char lease_note[384];
    std::snprintf(lease_note, sizeof(lease_note), SCRCTL_TR(
        ". Audio keepalive has stopped; wait for session expiry (about %u seconds), "
        "then resume the phone's player if needed"), options_.lease_seconds);
    err += lease_note;
    // 只停音频本地收包和续期，让已接受的设备音频会话到期；stopAll 会中断视频。
}

bool AudioPump::start_session(std::string &err) {
    // 先确认解码器可用再起流，避免构建缺少后端时占用设备媒体会话。
    if (decoder_ == nullptr) {
        decoder_ = create_audio_decoder(options_.sample_rate, options_.channels,
                                       options_.frame_length, err);
        if (decoder_ == nullptr) {
            return false;
        }
    }

    StreamSession::Request request;
    request.audio = true;
    request.offer.audio_dup = options_.audio_dup;
    request.timeout_seconds = options_.lease_seconds;
    request.client_session_uuid = options_.client_session_uuid;
    StreamSession::StartStatus start_status = StreamSession::StartStatus::NotConfirmed;
    auto session = StreamSession::start(device_, request, err, verbose_, nullptr, &start_status);
    if (session == nullptr) {
        if (start_status == StreamSession::StartStatus::AcceptedInvalidAnswer) {
            reject_negotiation(err);
        }
        return false;
    }
    const auto &started = session->started();
    if (!validate_stream_mode(started, options_, err)) {
        reject_negotiation(err);
        // 会话已被设备接受，但路由不符。停止本地收包，不发 RR，让音频租期到期；
        // 不能调用 stopAll，因为视频已在运行。也不能为获得声音而静默改成双端播放。
        return false;
    }
    if (!started.has_remote_ssrc || !started.has_local_ssrc) {
        // 缺少 SSRC 不影响当前收包和解码，但 RR 无法正确指向设备媒体源，
        // 会话可能无法续期。输出协商值以便排查。
        std::fprintf(stderr,
                     SCRCTL_TR("Audio answer missing SSRC (RemoteSSRC=%u LocalSSRC=%u); session renewal may fail\n"),
                     started.remote_ssrc, started.local_ssrc);
    }
    session_ = std::move(session);
    if (regulator_ != nullptr) {
        regulator_->reset();
        std::lock_guard<std::mutex> lock(mutex_);
        clock_silence_pending_ = 0;
        clock_discard_pending_ = 0;
        stats_.clock = regulator_->stats();
    }
    publish_live();
    return true;
}

/// 会话变更后更新公开快照。backend_name_ 也在相同锁内写入，
/// 与 backend_name() 的并发读取同步。
void AudioPump::publish_live() {
    std::lock_guard<std::mutex> lock(live_mutex_);
    Live live;
    if (session_ != nullptr) {
        const auto &started = session_->started();
        live.receiver_port = session_->receiver_port();
        live.payload_type = started.payload_type;
        live.sender_port = started.sender_port;
        live.remote_ssrc = started.remote_ssrc;
        live.local_ssrc = started.local_ssrc;
    }
    if (decoder_ != nullptr) {
        backend_name_ = decoder_->backend_name();
    }
    live_ = live;
}

void AudioPump::clear_live() {
    std::lock_guard<std::mutex> lock(live_mutex_);
    live_ = {};
}

void AudioPump::stop() {
    stopping_.store(true);
    if (worker_.joinable()) {
        worker_.join();
    }
    // 此处不调用 session_->stop()：当前 stopmediastream 只支持
    // stopAll，会同时结束视频会话。FramePump 停止时已负责 stopAll；
    // 单独运行音频时依赖自身 RTCP 空闲租期释放设备会话。
    session_.reset();
    clear_live();
}

void AudioPump::push(const std::vector<int16_t> &pcm) {
    std::vector<int16_t> adjusted;
    std::string clock_error;
    const bool regulated = regulator_ != nullptr && regulator_->process(pcm, adjusted, clock_error);
    if (!clock_error.empty()) {
        report_clock_failure(clock_error);
    }
    // 库暂时不可用时播放原始 PCM，不让补偿错误结束解码或设备会话。
    const auto &output = regulated ? adjusted : pcm;
    const std::size_t channels =
        options_.channels > 0 ? static_cast<std::size_t>(options_.channels) : 1;
    const std::size_t frames = output.size() / channels;
    AudioRegulator::Observation observation;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ring_.empty()) {
            ring_.assign(capacity_frames_ * channels, 0);
        }
        // 短 burst 的音频仍会在后续收包间隙消费，不能因暂时超过两倍目标
        // 就硬裁。仅环满时丢弃旧数据；正常水位由软补偿逐渐回到固定目标。
        std::size_t from = 0;
        if (frames >= capacity_frames_) {
            const std::size_t drop = used_ + frames - capacity_frames_;
            stats_.dropped_stale += drop;
            clock_discard_pending_ += drop;
            used_ = 0;
            read_ = write_;
            from = frames - capacity_frames_;
        } else if (used_ + frames > capacity_frames_) {
            const std::size_t drop = used_ + frames - capacity_frames_;
            read_ = (read_ + drop) % capacity_frames_;
            used_ -= drop;
            stats_.dropped_stale += drop;
            clock_discard_pending_ += drop;
        }
        for (std::size_t f = from; f < frames; ++f) {
            const std::size_t slot = ((write_ + (f - from)) % capacity_frames_) * channels;
            for (std::size_t c = 0; c < channels; ++c) {
                ring_[slot + c] = output[f * channels + c];
            }
        }
        write_ = (write_ + (frames - from)) % capacity_frames_;
        used_ += frames - from;
        observation.buffered_frames = used_;
        observation.playing = read_started_;
        observation.inserted_silence = std::exchange(clock_silence_pending_, 0);
        observation.discarded_frames = std::exchange(clock_discard_pending_, 0);
    }
    if (regulated && !regulator_->observe(observation, clock_error)) {
        report_clock_failure(clock_error);
    }
    if (regulator_ != nullptr) {
        const auto clock_stats = regulator_->stats();
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.clock = clock_stats;
    }
}

void AudioPump::report_clock_failure(const std::string &err) {
    std::uint64_t failures;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        failures = ++stats_.clock_failed;
    }
    if (failures <= 3) {
        std::fprintf(stderr, SCRCTL_TR("Audio clock compensation failed: %s; continuing playback\n"),
                     err.c_str());
    }
}

std::size_t AudioPump::read(int16_t *dst, std::size_t frames) {
    const std::size_t channels =
        options_.channels > 0 ? static_cast<std::size_t>(options_.channels) : 1;
    std::lock_guard<std::mutex> lock(mutex_);
    if (frames == 0) {
        return 0;
    }
    if (ring_.empty()) {
        if (read_started_) {
            clock_silence_pending_ += frames;
        }
        return 0;
    }
    // 本次将播放的 frames 不是额外积压；将它也算入目标会在正常节拍下反复
    // 丢掉少量样本。软补偿只裁启动积压；直接 PCM 路径还保留大积压保护。
    const auto trim = compute_read_trim(used_, frames, target_frames_, options_.frame_length,
                                       !read_started_);
    if (trim.frames > 0 && (trim.startup || regulator_ == nullptr)) {
        read_ = (read_ + trim.frames) % capacity_frames_;
        used_ -= trim.frames;
        clock_discard_pending_ += trim.frames;
        if (trim.startup) {
            stats_.startup_trimmed += trim.frames;
        } else {
            stats_.steered += trim.frames;
        }
    }
    const std::size_t take = frames < used_ ? frames : used_;
    for (std::size_t f = 0; f < take; ++f) {
        const std::size_t slot = ((read_ + f) % capacity_frames_) * channels;
        for (std::size_t c = 0; c < channels; ++c) {
            dst[f * channels + c] = ring_[slot + c];
        }
    }
    read_ = (read_ + take) % capacity_frames_;
    used_ -= take;
    if (take > 0) {
        read_started_ = true;
    }
    if (read_started_) {
        clock_silence_pending_ += frames - take;
    }
    return take;
}

std::size_t AudioPump::buffered_frames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return used_;
}

AudioPump::Stats AudioPump::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

std::uint16_t AudioPump::receiver_port() const {
    std::lock_guard<std::mutex> lock(live_mutex_);
    return live_.receiver_port;
}

std::uint8_t AudioPump::payload_type() const {
    std::lock_guard<std::mutex> lock(live_mutex_);
    return live_.payload_type;
}

std::string AudioPump::backend_name() const {
    std::lock_guard<std::mutex> lock(live_mutex_);
    return backend_name_.empty() ? std::string("none") : backend_name_;
}

void AudioPump::loop() {
    std::vector<uint8_t> datagram;
    std::string err;
    uint64_t next_rtcp_ms = now_ms() + kRtcpPeriodMs;
    uint64_t last_packet_ms = now_ms();
    uint64_t last_probe_ms = 0;
    scrctl::rt::RtpSeq seq;
    /// RTP 序号统计按会话重置；重置前将旧会话丢包值计入 lost_carry，
    /// 供整个 AudioPump 生命周期的累计统计使用。
    uint64_t lost_carry = 0;
    uint64_t decode_failures_logged = 0;

    while (!stopping_.load()) {
        const uint64_t now = now_ms();

        if (session_ == nullptr) {
            std::string rerr;
            if (start_session(rerr)) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++stats_.restarts;
                }
                std::printf(SCRCTL_TR("Audio session recreated, receive port=%u\n"), session_->receiver_port());
                last_packet_ms = now_ms();
                next_rtcp_ms = last_packet_ms + kRtcpPeriodMs;
                // 新会话的序号空间与上一条无关，不重置会把第一包判成大片缺口。
                lost_carry += seq.lost();
                seq.reset();
                continue;
            }
            if (negotiation_invalid_) {
                std::fprintf(stderr, SCRCTL_TR("Audio negotiation failed during recovery; disabling audio: %s\n"),
                             rerr.c_str());
                break;
            }
            std::fprintf(stderr, SCRCTL_TR("Failed to recreate audio session: %s (retry in 1 second)\n"), rerr.c_str());
            // 使用可取消的条件变量等待，停止请求无需等待退避期结束。
            for (int i = 0; i < 10 && !stopping_.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }

        // 保活使用每轮检查的单调时钟，不依赖收包超时；错过周期时从当前
        // 时间重新调度，不连续补发过期 RR。
        if (now >= next_rtcp_ms) {
            next_rtcp_ms = now + kRtcpPeriodMs;
            const auto rr = scrctl::rt::build_rr(session_->started().remote_ssrc,
                                                 session_->started().local_ssrc, seq.high());
            std::string serr;
            const bool ok = session_->send_rtp(rr, session_->started().sender_port, serr);
            uint64_t failed_after = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (ok) {
                    ++stats_.rtcp_sent;
                } else {
                    failed_after = ++stats_.rtcp_failed;
                }
            }
            // 每个会话只输出第一次发送失败，后续由会话恢复策略处理。
            if (failed_after == 1) {
                std::fprintf(stderr, SCRCTL_TR("Failed to send audio RTCP keepalive: %s\n"), serr.c_str());
            }
        }

        uint16_t peer_port = 0;
        if (!session_->next_packet(datagram, peer_port, kPollTimeoutMs, err)) {
            const uint64_t quiet = now_ms() - last_packet_ms;
            if (quiet >= kQuietProbeMs && now_ms() >= last_probe_ms + kRtcpPeriodMs) {
                last_probe_ms = now_ms();
                std::string perr;
                const auto state = StreamSession::probe(device_, session_->started().session_uuid,
                                                        perr, verbose_);
                if (state == StreamSession::ServerState::Ended) {
                    std::fprintf(stderr, SCRCTL_TR("Audio: device session ended (%llu ms without audio); recreating session\n"),
                                 static_cast<unsigned long long>(quiet));
                    session_.reset();
                    clear_live();
                }
            }
            continue;
        }
        last_packet_ms = now_ms();

        scrctl::rt::PacketInfo info;
        if (!scrctl::rt::parse_rtp_header(std::span<const uint8_t>(datagram), info)) {
            continue;
        }
        // 先按载荷类型过滤，再统计 RTP 序号。RTCP 头的第 3–4 字节是
        // 长度而非 RTP 序号，将 SR 计入序号空间会产生虚假的缺口。
        if (info.payload_type != session_->started().payload_type) {
            std::lock_guard<std::mutex> lock(mutex_);
            ++stats_.other_payload;
            continue;
        }
        // 使用共享 rt::RtpSeq 处理序号回绕和乱序；最高序号不随迟到包回退。
        const auto verdict = seq.observe(info.sequence);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++stats_.packets;
            if (verdict == scrctl::rt::RtpSeq::Verdict::kGap) {
                ++stats_.seq_gaps;
            } else if (verdict == scrctl::rt::RtpSeq::Verdict::kLate) {
                ++stats_.out_of_order;
            }
            // 使用当前未补齐缺口数，允许迟到包减少丢包读数；旧会话值已保存在
            // lost_carry 中。
            stats_.seq_lost = lost_carry + seq.lost();
        }
        // 当前 ELD 每包承载一帧（10 ms），不做重组或重传，只统计序号缺口。
        const auto payload = std::span<const uint8_t>(datagram).subspan(
            info.payload_offset, info.payload_size);
        std::vector<int16_t> pcm;
        std::string derr;
        if (!decoder_->decode(payload, pcm, derr)) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++stats_.decode_failed;
            }
            // 解码错误只输出前几次，避免每包重复日志。
            if (decode_failures_logged < 3) {
                ++decode_failures_logged;
                std::fprintf(stderr, SCRCTL_TR("Audio decode failed (%zu-byte payload): %s\n"), payload.size(),
                             derr.c_str());
            }
            continue;
        }
        if (!pcm.empty()) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++stats_.decoded;
            }
            push(pcm);
        }
    }
}

}  // namespace scrctl::media
