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

/// 音频腿：起流、收包、解码、攒进一个环形缓冲，等人来取 PCM。
///
/// 为什么必须自己一个线程，而不是让主循环顺手取一下：SDL 的音频回调是按固定节拍
/// 被系统叫醒的，它不管画面有没有在动。而镜像的主循环是**帧驱动**的——屏幕静止时
/// 设备一个视频包都不发，主循环就一直停在等帧上。音频挂在主循环上，结果就是
/// "没人动屏幕 → 声音也停了"，那是个比没接音频更糟的假象。
class AudioPump {
public:
    struct Options {
        /// 本腿的 `avcMediaStreamOptionClientSessionID`（16 字节 XPC UUID 原文）。
        /// 空 = 设备协商时自己生成一个。
        ///
        /// **产品路径留空，不要跟视频腿共用**，虽然苹果是两条腿同一个 UUID（抓包里两次
        /// start 的都是同一个）。原因是 `StreamSession::probe()` 认的就是这个 UUID：设备
        /// 的会话表按 ClientSessionID 查，共用时"查得到"只等于**至少有一条腿还活着**，
        /// 于是视频腿那套"设备已结束这条流"的判据会被音频腿掩护掉——画面冻住而泵以为
        /// 流好着。租期靠每条腿各自的 RR 已经能续住（docs §13 实测），不需要共用。
        std::vector<uint8_t> client_session_uuid;
        /// 这条会话的租期（秒）。理由与视频腿完全一样：设备那个
        /// `RTCPTimeoutInterval` 是"距离上次收到我们 RTCP 多久"的空闲计时器。
        std::uint32_t lease_seconds = 20;
        int sample_rate = 48000;
        int channels = 2;
        /// ELD 一帧的采样数。它同时是 ASBD 的 mFramesPerPacket——480 才是 ELD，
        /// 1024 是 LC，写错这一位解码器会给出两倍的采样数（docs §17.1）。
        int frame_length = 480;
    };

    struct Stats {
        /// PT 对上音频腿的那个数的 RTP 包（不管解不解得开）。
        std::uint64_t packets = 0;
        /// 解出了非空 PCM 的包数。`packets - decoded` 就是解码器哑掉的量。
        std::uint64_t decoded = 0;
        std::uint64_t decode_failed = 0;
        /// 同端口上收到的非音频载荷（设备的 RTCP SR）。
        std::uint64_t other_payload = 0;
        /// 序号**往前跳**的事件数（一次事件可能缺好几个包，看 seq_lost）。
        std::uint64_t seq_gaps = 0;
        /// 按序号算出真正没到的包数。
        ///
        /// 为什么和 seq_gaps 分开数，也为什么"迟到"不能并进缺口里：这条流实测每
        /// 10ms 一个包、20 秒 2015 个（=100.75/s，一秒不多一秒不少），而相邻序号
        /// 不等式检查每 2 秒就报 4 次——那**不是**丢包，是包在隧道里换了个顺序到。
        /// 把换序算成丢失会把"要不要为此做重传/缓冲"这个决定引到错误方向上。
        std::uint64_t seq_lost = 0;
        /// 比已见过的最大序号还晚到的包数（迟到或重复）。
        std::uint64_t out_of_order = 0;
        std::uint64_t rtcp_sent = 0;
        std::uint64_t rtcp_failed = 0;
        /// 缓冲满时被丢掉的**最旧帧**数（不是包数：一帧 = 一个声道采样点组）。
        std::uint64_t dropped_stale = 0;
        std::uint64_t restarts = 0;
    };

    ~AudioPump();

    AudioPump(const AudioPump &) = delete;
    AudioPump &operator=(const AudioPump &) = delete;

    /// 起音频腿并开线程。失败返回 nullptr 并给原因——**不致命**：没有音频的镜像
    /// 仍然是可用的镜像，调用方打一行说明就该继续跑。
    static std::unique_ptr<AudioPump> start(remote::Device &device, const Options &options,
                                            std::string &err, bool verbose = false);

    /// 取 `frames` 帧（一帧 = channels 个交织 s16）到 dst，返回实际取到的帧数。
    /// 不够就取多少给多少——调用方（音频回调）自己补静音并数一下欠载。
    ///
    /// **dst 必须容得下 `frames * channels()` 个 int16**。这里的单位是"帧"而不是
    /// "采样数"，因为调用方（SDL 的回调）手里那个数就是帧；两者按 channels 换算，
    /// 换错一次就是往缓冲区后面多写 channels 倍字节，而现场表现为"别的线程过一会儿
    /// 崩在 malloc 里"（探针就这么栽过一次）。
    std::size_t read(int16_t *dst, std::size_t frames);

    /// 协商/解码用的采样率与声道数。出口（声卡、文件）要按这两个数开设备。
    [[nodiscard]] int sample_rate() const { return options_.sample_rate; }
    [[nodiscard]] int channels() const { return options_.channels; }

    /// 缓冲里现在攒了多少帧。回调用它决定"先攒够再开始放"，否则起播的头几百毫秒
    /// 会一直在欠载与补静音之间跳。
    [[nodiscard]] std::size_t buffered_frames() const;

    [[nodiscard]] Stats stats() const;
    [[nodiscard]] std::uint16_t receiver_port() const;
    [[nodiscard]] std::uint8_t payload_type() const;

    /// 实际在用的解码后端名。为什么要在意外面看得见它：这条路上"没声"的原因至少有
    /// 三个（没起流、后端不认这份参数、缓冲一直被取空），而三者症状一模一样。
    [[nodiscard]] const char *backend_name() const {
        return decoder_ != nullptr ? decoder_->backend_name() : "none";
    }

    /// 停线程并停掉设备侧那条会话。析构也会做，但显式调一次能让"先停流再退进程"
    /// 这件事发生在调用点，而不是等到栈展开。
    void stop();

private:
    AudioPump(remote::Device &device, Options options)
        : device_(device), options_(std::move(options)) {}

    bool start_session(std::string &err);
    void loop();
    void push(const std::vector<int16_t> &pcm);

    remote::Device &device_;
    Options options_;
    bool verbose_ = false;
    std::unique_ptr<AudioDecoder> decoder_;
    std::unique_ptr<StreamSession> session_;
    std::thread worker_;
    std::atomic<bool> stopping_ { false };

    mutable std::mutex mutex_;
    std::vector<int16_t> ring_;
    std::size_t write_ = 0;
    std::size_t read_ = 0;
    std::size_t used_ = 0;
    Stats stats_;
};

}  // namespace scrctl::media
