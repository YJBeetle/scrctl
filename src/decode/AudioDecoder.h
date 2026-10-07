#pragma once

#include "i18n/Translation.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace scrctl {

/// AAC-ELD 解码接口。输入为已去除 RTP 头的单帧裸 ELD 载荷，输出为交织的有符号
/// 16 位 PCM。已测设备采用一包一帧，编码配置见
/// [CoreDevice §17](../../docs/coredevice.md#17-音频腿的编码鉴定实测iphone144--ios-270--usb)。
class AudioDecoder {
public:
    virtual ~AudioDecoder() = default;

    /// 解码一帧，将输出采样追加到 pcm，不清空已有内容。
    /// frame_length 按每声道采样数定义；实际输出数量以 pcm 的增量为准。
    /// 返回 true 不保证追加了 PCM，例如空输入可以成功但无输出；核验后端时需同时检查
    /// 采样数量和内容。返回 false 表示本次解码失败，err 提供原因，后续处理由调用方决定。
    /// 同一份设备载荷的后端对照见 [CoreDevice §17.1](../../docs/coredevice.md#171-音频解码后端只能选-audiotoolbox实测同一份-dump-四路对照)。
    virtual bool decode(std::span<const uint8_t> frame, std::vector<int16_t> &pcm,
                        std::string &err) = 0;

    [[nodiscard]] virtual const char *backend_name() const = 0;
};

/// 按协商的采样率、声道数和每声道 frame_length 创建 AAC-ELD 解码器。
/// 后端不可用或初始化失败时返回 nullptr，并通过 err 提供原因，调用方需处理空结果。
/// 当前项目仅在 Apple 平台接入 AudioToolbox；其他平台的本构建未接入音频解码后端。
std::unique_ptr<AudioDecoder> create_audio_decoder(int sample_rate, int channels,
                                                   int frame_length, std::string &err);

/// 本构建是否编入音频解码后端，可用于在起流前判断能力；true 不保证初始化成功。
///
/// 当前后端为 macOS AudioToolbox。已测试的 FFmpeg 原生 AAC 解码器不能正确处理
/// 本项目测试设备的 AAC-ELD 配置，不能由此推断所有 FFmpeg 版本或其他实现都不支持 ELD。
/// AAC-ELD 并非 Apple 私有格式；FDK AAC 提供 ELD 解码，但尚未验证本项目的设备载荷和配置。
/// 测试样本与对照见 [CoreDevice §17.1](../../docs/coredevice.md#171-音频解码后端只能选-audiotoolbox实测同一份-dump-四路对照)，
/// 后端扩展计划见 [ROADMAP](../../docs/ROADMAP.md)。接入新后端前需用同一份 dump 比较 PCM。
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
