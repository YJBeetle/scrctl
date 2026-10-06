#pragma once

#include "i18n/Translation.h"

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
/// 当前项目只接入 macOS AudioToolbox。已测试的 FFmpeg 原生 AAC 解码器不能正确
/// 处理这份设备 AAC-ELD 配置；这不表示 AAC-ELD 是 Apple 私有格式，或其他实现
/// 都不支持。FDK AAC 提供 ELD 解码，但尚未验证本项目的设备载荷和配置。
/// 当前测试样本和结果见 docs/ROADMAP.md；扩展后端前需用同一份 dump 比较 PCM。
#if defined(__APPLE__)
inline constexpr bool kHaveAudioDecoder = true;
#else
inline constexpr bool kHaveAudioDecoder = false;
#endif

inline constexpr const char *kNoAudioDecoderMessage =
    SCRCTL_N_(
        "AAC-ELD decoding is unavailable on this platform. This build uses AudioToolbox "
        "on macOS; the tested native FFmpeg AAC decoder does not support ELD. Video can "
        "continue without audio.");

}  // namespace scrctl
