#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace scrctl {

/// 一路 AAC-ELD 的解码器：吃"一包一帧"的裸 ELD 载荷，吐交织的 s16 PCM。
class AudioDecoder {
public:
    virtual ~AudioDecoder() = default;

    /// 解一帧，样本**追加**进 pcm。一帧固定 `frame_length` 个采样/声道，所以
    /// "pcm 涨了多少"本身就是"后端认不认这份配置"的读数。
    ///
    /// 返回 false = 这一帧解不出来。调用方丢掉它继续，别停：音频上缺一帧只是
    /// "咔"一下，停下来是整段没声。
    virtual bool decode(std::span<const uint8_t> frame, std::vector<int16_t> &pcm,
                        std::string &err) = 0;

    [[nodiscard]] virtual const char *backend_name() const = 0;
};

/// 建 AAC-ELD 解码器。没有可用后端时返回 nullptr 并把原因写进 err——调用方必须
/// 能处理空，因为**非 Apple 平台就是没有**（见 kHaveAudioDecoder）。
std::unique_ptr<AudioDecoder> create_audio_decoder(int sample_rate, int channels,
                                                   int frame_length, std::string &err);

/// 这个构建里到底有没有音频后端。与视频那边同一个道理：没有就要在起流之前说清楚，
/// 而不是等第一帧音频到了才发现手里是空指针。
///
/// 为什么 Apple 之外会是 false：设备那条音频腿发的是苹果专有的 AAC-ELD，而自由
/// 实现里没有一个能解它——libavcodec 的原生 aac 解码器明确不支持 ELD（真机 dump
/// 实测：按规范拼的 AudioSpecificConfig 连 `avcodec_open2` 都过不去，报 "AAC data
/// resilience is not implemented"；换成苹果自己那份 cookie 打得开，1404 帧只出得来
/// 109 帧、每帧 512 采样、峰值顶满，是解歪了的样子）。libav 里那个 AudioToolbox 壳
/// `aac_at` 同一条 dump 也只出 109/1406。所以 macOS 直接对 AudioToolbox 的
/// AudioConverter 说话，别处只能承认这一路是缺的。数字与判据在 docs §17.1。
#if defined(__APPLE__)
inline constexpr bool kHaveAudioDecoder = true;
#else
inline constexpr bool kHaveAudioDecoder = false;
#endif

inline constexpr const char *kNoAudioDecoderMessage =
    "这个平台上没有 AAC-ELD 解码后端：设备音频是苹果专有的 AAC-ELD，而 macOS 之外"
    "没有能解它的自由实现（libavcodec 的原生 aac 解码器不支持 ELD，实测）。"
    "所以音频在非 Apple 平台上是缺的，不是没接好。";

}  // namespace scrctl
