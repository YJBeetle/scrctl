#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "decode/AudioDecoder.h"
#include "media/StreamSession.h"
#include "remote/Device.h"

namespace scrctl::media {

/// 独立接收并解码音频，使用环形缓冲向调用方提供 PCM。
/// 音频工作线程与视频帧循环分离，避免静止画面阻塞音频接收。
class AudioPump {
public:
    struct Options {
        /// 音频会话的 avcMediaStreamOptionClientSessionID（16 字节 XPC UUID）。
        /// 为空时由设备生成。
        ///
        /// 应用默认不与视频共用 UUID。设备按 ClientSessionID 查询会话，
        /// 共用时任一会话存活都可能让视频状态查询返回存活，掩盖视频已结束。
        /// 音频和视频分别发送 RR 续期，无需为此共用 UUID。
        std::vector<uint8_t> client_session_uuid;
        /// 会话的 RTCP 空闲租期（秒），周期 RR 延长该期限。
        std::uint32_t lease_seconds = 20;
        int sample_rate = 48000;
        int channels = 2;
        /// ELD 每帧采样数，也是 ASBD.mFramesPerPacket。ELD 使用 480，
        /// AAC-LC 的 1024 不适用；错误配置会改变解码输出长度，见 docs/coredevice.md §17.1。
        int frame_length = 480;
        /// 目标缓冲时长，也是开始播放前需要积累的音频量。
        ///
        /// 默认 50 ms，上限 1000 ms。超出上限时限制到上限并告知调用方。
        ///
        /// 目标水位限制实时播放延迟。启动时消费方可能晚于接收线程准备好，
        /// 额外积累的音频不会在生产、消费速率相等时自动减少；read() 通过
        /// 仅在初次读取或积压明显超出目标时裁去旧采样。正常包到达与音频回调
        /// 的节拍差不应触发逐回调丢样本；长期时钟漂移尚未用重采样补偿。
        ///
        /// 延迟开放消费端的验证见 docs/coredevice.md §17.2：read() 应将
        /// 超过两倍目标的积压在一次调用内降至目标附近，而后维持稳定水位。
        int target_backlog_ms = 50;
    };

    /// 同时计算目标水位与环容量。容量必须大于预滚水位，避免开始播放的
    /// 条件永远无法达到；容量留出四倍目标的余量，使 read() 的水位调节
    /// 先于环满时的旧帧丢弃生效。
    struct Waterline {
        std::size_t target_frames = 0;
        std::size_t capacity_frames = 0;
        /// 非零表示请求时长被限制到此值，调用方应提示实际采用的配置。
        int clamped_to_ms = 0;
    };

    /// 根据选项计算水位及容量，不访问设备，可在 tests/media_test 离线验证。
    [[nodiscard]] static Waterline compute_waterline(const Options &options);

    struct ReadTrim {
        std::size_t frames = 0;
        bool startup = false;
    };

    /// 以本次读取后的剩余帧数判断积压。初次输出可裁去启动期间积累的旧数据；
    /// 后续只处理超过两倍目标加一个编码帧的大积压，正常回调不跳过样本。
    /// 纯函数供离线节拍和缓冲策略验证，不访问设备或修改缓冲。
    [[nodiscard]] static ReadTrim compute_read_trim(std::size_t buffered,
                                                   std::size_t requested,
                                                   std::size_t target,
                                                   int frame_length, bool first_read);

    struct Stats {
        /// 载荷类型与音频配置匹配的 RTP 包数，包括解码失败的包。
        std::uint64_t packets = 0;
        /// 输出非空 PCM 的包数，与 packets 的差值表示未输出 PCM 的包数。
        std::uint64_t decoded = 0;
        std::uint64_t decode_failed = 0;
        /// 同端口收到的其他载荷类型，包括 RTCP SR。
        std::uint64_t other_payload = 0;
        /// 序号向前跳跃的事件数，一次事件可以跨过多个包。
        std::uint64_t seq_gaps = 0;
        /// 当前未补齐的序号数量，迟到的包可减少此值。
        /// 缺口与迟到、重复分别统计，避免将乱序直接等同于永久丢包。
        std::uint64_t seq_lost = 0;
        /// 序号低于或等于已见最高值的包数，包括迟到和重复。
        std::uint64_t out_of_order = 0;
        std::uint64_t rtcp_sent = 0;
        std::uint64_t rtcp_failed = 0;
        /// 缓冲满时丢弃的最旧音频帧数；一帧是 channels 个采样点。
        std::uint64_t dropped_stale = 0;
        /// 首次输出前裁去的旧音频帧数，不计入运行时的积压调整。
        std::uint64_t startup_trimmed = 0;
        /// 开始输出后，为控制过大积压而跳过的音频帧数。
        std::uint64_t steered = 0;
        std::uint64_t restarts = 0;
    };

    ~AudioPump();

    AudioPump(const AudioPump &) = delete;
    AudioPump &operator=(const AudioPump &) = delete;

    /// 建立音频会话并启动线程。失败返回 nullptr 并设置原因，调用方可继续
    /// 提供不带音频的视频镜像。
    static std::unique_ptr<AudioPump> start(remote::Device &device, const Options &options,
                                            std::string &err, bool verbose = false);

    /// 向 dst 写入最多 frames 帧交织 s16 PCM，返回实际帧数，不足部分
    /// 由调用方补静音。dst 必须容纳 frames * channels() 个 int16_t。
    /// 接口单位为音频帧，不能将单声道采样数或字节数直接作为 frames。
    std::size_t read(int16_t *dst, std::size_t frames);

    /// 协商及解码使用的采样率与声道数，播放设备应采用同样配置。
    [[nodiscard]] int sample_rate() const { return options_.sample_rate; }
    [[nodiscard]] int channels() const { return options_.channels; }

    /// 当前缓冲音频帧数，供播放端判断是否达到预滚水位。
    [[nodiscard]] std::size_t buffered_frames() const;

    /// 开始播放前的预滚帧数，由目标毫秒数统一换算，避免调用方重复换算。
    [[nodiscard]] std::size_t preroll_frames() const { return target_frames_; }

    [[nodiscard]] Stats stats() const;

    /// 当前会话的信息快照。session_ 由工作线程重建，外部只访问 live_
    /// 副本，避免跨线程读取正在 reset 的 unique_ptr。
    [[nodiscard]] std::uint16_t receiver_port() const;
    [[nodiscard]] std::uint8_t payload_type() const;

    /// 返回实际解码后端名，便于区分会话、解码及播放缓冲的问题。
    [[nodiscard]] std::string backend_name() const;

    /// 停止工作线程并释放本地会话。设备停止策略见 stop()；析构也会调用。
    void stop();

private:
    AudioPump(remote::Device &device, Options options)
        : device_(device), options_(std::move(options)) {
        const Waterline w = compute_waterline(options_);
        target_frames_ = w.target_frames;
        capacity_frames_ = w.capacity_frames;
    }

    /// 供其他线程读取的当前会话快照，字段均为可复制标量。
    struct Live {
        std::uint16_t receiver_port = 0;
        std::uint8_t payload_type = 0;
        std::uint16_t sender_port = 0;
        std::uint32_t remote_ssrc = 0;
        std::uint32_t local_ssrc = 0;
    };

    bool start_session(std::string &err);
    void publish_live();
    void clear_live();
    void loop();
    void push(const std::vector<int16_t> &pcm);

    remote::Device &device_;
    Options options_;
    bool verbose_ = false;
    std::unique_ptr<AudioDecoder> decoder_;
    /// 仅工作线程及 join 后的 stop() 访问 session_。对外信息经 live_
    /// 快照提供，不能将重建时会被 reset 的指针交给其他线程。
    std::unique_ptr<StreamSession> session_;
    std::thread worker_;
    std::atomic<bool> stopping_ { false };

    mutable std::mutex live_mutex_;
    Live live_;
    /// 解码器创建后后端名保持不变，重建只更换媒体会话。
    std::string backend_name_;

    mutable std::mutex mutex_;
    std::vector<int16_t> ring_;
    std::size_t write_ = 0;
    std::size_t read_ = 0;
    std::size_t used_ = 0;
    bool read_started_ = false;
    std::size_t target_frames_ = 0;
    /// 环容量（音频帧），与目标水位由 compute_waterline() 一起计算。
    std::size_t capacity_frames_ = 0;
    Stats stats_;
};

}  // namespace scrctl::media
