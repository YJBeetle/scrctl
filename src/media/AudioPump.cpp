#include "media/AudioPump.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <span>
#include <thread>

#include "remote/Device.h"
#include "rt/Rtcp.h"
#include "rt/RtpHevc.h"

namespace scrctl::media {
namespace {

/// 一秒钟一个 RR。这个节拍是从视频腿搬过来的同一条实测结论：设备的
/// `RTCPTimeoutInterval` 是"距离上次收到我们 RTCP 多久"的空闲计时器，1Hz 的裸 RR
/// 就能复位（docs §13）。音频腿是一条**独立的会话**，它自己也在倒数，所以这条腿也得
/// 自己发——不能指望视频腿那份 RTCP 顺带把它救活。
constexpr uint64_t kRtcpPeriodMs = 1000;

/// 每轮 `next_packet` 的等待上限。太短白烧 CPU（音频只有约 100 包/秒），太长会让
/// 停止响应和 RTCP 节拍一起抖。
constexpr int kPollTimeoutMs = 50;

/// 环形缓冲容量（帧）= 0.5 秒。它是"设备推得比我们取得快时能囤多久"的上限，而囤下来的
/// 每一毫秒都是延迟。0.5 秒够跨过一次已知抖动（停+起重起约 300ms），又不至于把音画
/// 差做成半秒。
constexpr std::size_t kRingCapacityFrames = 24000;

/// 多久没收到任何音频包就去问一次设备"这条会话还在不在"。
///
/// 音频这一侧不需要像视频那样先分辨"画面静止"与"流死了"：实测承载的声音全零的那
/// 28 秒里包照样按 100 个/秒不停地到（docs §17.2 ②），所以"没包"本身就是死讯。既然
/// 一包 10ms、一秒该来 100 个，400ms 的空档就不是抖动而是死讯的征兆——这一档因此取
/// 400，不像视频腿那样要留到两三个心跳的量级。
///
/// 但仍然**只拿它当去问一句的理由，不当直接重起的依据**：问一句实测 10~300ms，而
/// 猜错一次重起的代价是白停白起一条流、外加等第一个包。
///
/// 这一档从 2000 降到 400 是因为量到了一次真实的连带伤害：**视频腿每一次重起都会把
/// 音频会话一起带走**，而 2000 那档让音频掉了约 2.4 秒才自己爬起来。改到 400 之后
/// 实测掉声 672~689ms（两臂各一次）。机制与那两条臂见 docs §17.2 ⑤——要点是这跟
/// 我们发不发 `stopAll` 无关（一个 stop 都不发、直接起第二条视频会话，音频照样断），
/// 所以别指望"少发一次 stop"能救回来，只能靠发现得快。
constexpr uint64_t kQuietProbeMs = 400;

uint64_t now_ms() {
    using clock = std::chrono::steady_clock;
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch())
            .count());
}

}  // namespace

std::unique_ptr<AudioPump> AudioPump::start(remote::Device &device, const Options &options,
                                           std::string &err, bool verbose) {
    auto pump = std::unique_ptr<AudioPump>(new AudioPump(device, options));
    pump->verbose_ = verbose;
    if (!pump->start_session(err)) {
        return nullptr;
    }
    pump->worker_ = std::thread([raw = pump.get()] { raw->loop(); });
    return pump;
}

AudioPump::~AudioPump() { stop(); }

bool AudioPump::start_session(std::string &err) {
    // 解码器先建再起流：反过来的话，建解码器失败会把一条已经建好的设备会话留在那儿
    // 空转到租期结束，而这条路径是可用的（非 Apple 平台没有后端）。
    if (decoder_ == nullptr) {
        decoder_ = create_audio_decoder(options_.sample_rate, options_.channels,
                                       options_.frame_length, err);
        if (decoder_ == nullptr) {
            return false;
        }
    }

    StreamSession::Request request;
    request.audio = true;
    request.timeout_seconds = options_.lease_seconds;
    request.client_session_uuid = options_.client_session_uuid;
    auto session = StreamSession::start(device_, request, err, verbose_);
    if (session == nullptr) {
        return false;
    }
    const auto &started = session->started();
    if (started.remote_ssrc == 0 || started.local_ssrc == 0) {
        // 不致命：收流与解码都不依赖这两个数，只有回 RTCP 要。但它们为 0 说明 answer 的
        // 形状和预期不符，而这时发出去的 RR 指认不到任何流——租期就会到点。把数打出来，
        // 免得它变成"声音每 20 秒卡一次"这种查不出来源的症状。
        std::fprintf(stderr,
                     "音频腿 answer 里没给出 SSRC（RemoteSSRC=%u LocalSSRC=%u），"
                     "这条会话可能到租期就被摘掉\n",
                     started.remote_ssrc, started.local_ssrc);
    }
    session_ = std::move(session);
    publish_live();
    return true;
}

/// 把当前会话的那几个标量抄成一份对外可见的快照。只在会话换了之后调。
///
/// 锁放在**最上面**而不是只包住最后那次赋值：`backend_name_` 也是这份快照的一部分，
/// 它在锁外写就等于给 `backend_name()` 留了一个"读到半条字符串"的窗口。
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
    // 这里**故意不调** `session_->stop()`。`stopmediastream` 唯一的入参形状是
    // `{stopAll: true}`，而它停的是设备上**所有**会话——音频腿退房会把视频腿一起掐掉，
    // 现场表现是画面突然开始"设备已结束这条流，重起媒体会话"（docs §13 那条副作用）。
    // 视频腿的 `~FramePump` 本来就会发一次 stopAll，那一下已经把两条腿都停了。真出现
    // "只有音频腿"的用法时，这条会话最多活到它自己报的 20 秒租期——代价是电与一个端口，
    // 比误杀视频流便宜。
    session_.reset();
    clear_live();
}

void AudioPump::push(const std::vector<int16_t> &pcm) {
    const std::size_t channels =
        options_.channels > 0 ? static_cast<std::size_t>(options_.channels) : 1;
    const std::size_t frames = pcm.size() / channels;
    if (frames == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (ring_.empty()) {
        ring_.assign(kRingCapacityFrames * channels, 0);
    }
    // 满了丢**最旧**的：这是实时流，攒着的旧声音放出来只会越来越对不上画面。
    std::size_t from = 0;
    if (frames >= kRingCapacityFrames) {
        // 一次喂进来的比整个环还大（正常走不到：ELD 一帧 480）。这一支存在的理由是
        // 守住"used_ 不超过容量"这条不变式——不单独处理的话下面那句 `used_ += `
        // 会把它推成一个大于容量的数，之后每次 read 都在一个假水位上算取多少，
        // 症状是"没声"而不是"这一帧丢了"。
        stats_.dropped_stale += used_ + frames - kRingCapacityFrames;
        used_ = 0;
        read_ = write_;
        from = frames - kRingCapacityFrames;
    } else if (used_ + frames > kRingCapacityFrames) {
        const std::size_t drop = used_ + frames - kRingCapacityFrames;
        read_ = (read_ + drop) % kRingCapacityFrames;
        used_ -= drop;
        stats_.dropped_stale += drop;
    }
    for (std::size_t f = from; f < frames; ++f) {
        const std::size_t slot = ((write_ + (f - from)) % kRingCapacityFrames) * channels;
        for (std::size_t c = 0; c < channels; ++c) {
            ring_[slot + c] = pcm[f * channels + c];
        }
    }
    write_ = (write_ + (frames - from)) % kRingCapacityFrames;
    used_ += frames - from;
}

std::size_t AudioPump::read(int16_t *dst, std::size_t frames) {
    const std::size_t channels =
        options_.channels > 0 ? static_cast<std::size_t>(options_.channels) : 1;
    std::lock_guard<std::mutex> lock(mutex_);
    if (ring_.empty() || frames == 0) {
        return 0;
    }
    // 水位导向：高出目标就多跳过几帧最旧的，把囤着的东西排掉。分两档，因为"差一点"
    // 与"差一截"的正确修法不一样：
    //
    //   * 差一截（超过两倍目标）：**一次砍回目标**。这时候囤着的东西多半是"窗口还没
    //     开、声音先攒了半秒"那种没人听过的旧内容，整段丢掉是对的，而慢慢调速要花
    //     二十几秒（每秒只能悄悄排掉 800 帧），那二十几秒里播放速率是偏快的、
    //     听感是变调。实测这一档把收敛从 27 秒压到一次调用。
    //   * 差一点（两倍以内，也就是漂移那种量级）：每次悄悄跳几帧。跳 8 帧是 21ms
    //     回调的 0.8%，等于把速率调快千分之几——听不出，但它能把每小时几十毫秒的
    //     漂移持续排掉而永远不必做一次明显的剪切。
    std::size_t skip = 0;
    if (used_ > target_frames_ && used_ >= frames) {
        // `used_ >= frames` 那一判不是啰嗦：不够给的时候 `used_ - frames` 会下溢成
        // 一个巨大值，`skip` 反而被放行到上限。
        const std::size_t excess = used_ - target_frames_;
        skip = used_ > target_frames_ * 2 ? excess : std::min<std::size_t>(excess / 20, 8);
        skip = std::min(skip, used_ - frames);  // 别把这次要给的帧也算进跳过里
    }
    if (skip > 0) {
        read_ = (read_ + skip) % kRingCapacityFrames;
        used_ -= skip;
        stats_.steered += skip;
    }
    const std::size_t take = frames < used_ ? frames : used_;
    for (std::size_t f = 0; f < take; ++f) {
        const std::size_t slot = ((read_ + f) % kRingCapacityFrames) * channels;
        for (std::size_t c = 0; c < channels; ++c) {
            dst[f * channels + c] = ring_[slot + c];
        }
    }
    read_ = (read_ + take) % kRingCapacityFrames;
    used_ -= take;
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
    bool have_seq = false;
    uint16_t last_seq = 0;
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
                std::printf("音频会话已重起，收流端口=%u\n", session_->receiver_port());
                last_packet_ms = now_ms();
                next_rtcp_ms = last_packet_ms + kRtcpPeriodMs;
                have_seq = false;
                continue;
            }
            std::fprintf(stderr, "重起音频会话失败: %s（1 秒后再试）\n", rerr.c_str());
            // 这 1 秒要睡得能响应停止：Ctrl-C 之后还硬睡，用户看到的就是"按了没反应"。
            for (int i = 0; i < 10 && !stopping_.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }

        // 保活包：挂在墙上时钟而不是"取包超时"那条分支上（包连着来时 50ms 超时永远轮不到）。
        // 追不上节拍就重新对齐，不要在一个停顿之后连发一串补账的 RR。
        if (now >= next_rtcp_ms) {
            next_rtcp_ms = now + kRtcpPeriodMs;
            const auto rr = scrctl::rt::build_rr(session_->started().remote_ssrc,
                                                 session_->started().local_ssrc, last_seq);
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
            // 只报第一条：发不出去一般是会话已经没了，而上面的静默判据一会就会把它重起。
            if (failed_after == 1) {
                std::fprintf(stderr, "音频腿 RTCP 保活包发送失败: %s\n", serr.c_str());
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
                    std::fprintf(stderr, "音频：设备已结束这条会话（静默 %llu ms），重起\n",
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
        // PT 过滤必须在序号记账**之前**：设备的 RTCP SR 和视频/音频共用同一个 UDP 端口，
        // 而 RTCP 头的第 3-4 字节是长度不是序号。把它当 RTP 记进序号序列，每来一个 SR
        // 就造出两次"缺口"，而且跳的是 SR 长度那个小数（实测每 2 秒 4 次、累计"真丢"
        // 两万多）——这条流一秒正好 100.75 个包、一个都没少，账却全错在 SR 上。
        if (info.payload_type != session_->started().payload_type) {
            std::lock_guard<std::mutex> lock(mutex_);
            ++stats_.other_payload;
            continue;
        }
        if (have_seq) {
            // 序号差按 mod 2^16 解释：`uint16_t(a - b)` 落在 (0, 32768) 才是"往前"，
            // 否则是迟到。直接比大小在跨越 65535 那一圈时会把一个正常的包判成倒退。
            const unsigned jump = static_cast<unsigned>(
                static_cast<uint16_t>(info.sequence - last_seq));
            std::lock_guard<std::mutex> lock(mutex_);
            if (jump > 1 && jump < 32768) {
                ++stats_.seq_gaps;
                stats_.seq_lost += jump - 1;
            } else if (jump == 0 || jump >= 32768) {
                ++stats_.out_of_order;
            }
        }
        have_seq = true;
        last_seq = info.sequence;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++stats_.packets;
        }
        // 分片重组与丢包重传对音频没有意义：ELD 一包就是一帧（10ms），丢一包就是少 10ms
        // 声音，补不出来。所以这里只数缺口、不追包。
        const auto payload = std::span<const uint8_t>(datagram).subspan(info.payload_offset);
        std::vector<int16_t> pcm;
        std::string derr;
        if (!decoder_->decode(payload, pcm, derr)) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++stats_.decode_failed;
            }
            // 只报前几条：解码器一旦不认这份参数就会每个包都失败，全打会把真正要看的
            // 那几行日志盖掉。
            if (decode_failures_logged < 3) {
                ++decode_failures_logged;
                std::fprintf(stderr, "音频包解不出（%zu 字节）: %s\n", payload.size(),
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
