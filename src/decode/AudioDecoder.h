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
    /// frame 内存由调用方拥有，需保持到调用返回；输出 PCM 由 pcm 自身拥有。
    /// 当前后端不提供实例内并发保护，decode 与销毁需由调用方串行执行。
    /// frame_length 按每声道采样数定义；实际输出数量以 pcm 的增量为准。
    /// 返回 true 不保证追加了 PCM，例如空输入可以成功但无输出；核验后端时需同时检查
    /// 采样数量和内容。返回 false 表示本次解码失败，err 提供原因，后续处理由调用方决定。
    /// 同一份设备载荷的后端对照见 [CoreDevice §17.1](../../docs/coredevice.md)。
    virtual bool decode(std::span<const uint8_t> frame, std::vector<int16_t> &pcm,
                        std::string &err) = 0;

    [[nodiscard]] virtual const char *backend_name() const = 0;
};

/// 按协商的采样率、声道数和每声道 frame_length 创建 AAC-ELD 解码器。
/// 后端不可用或初始化失败时返回 nullptr，并通过 err 提供原因，调用方需处理空结果。
/// Apple 平台使用 AudioToolbox；其他平台在启用 libav 时使用 FFmpeg 原生 AAC
/// 解码器。FFmpeg 适配器仅接受已验证的 48000 Hz、双声道、480 或 512 帧长配置。
std::unique_ptr<AudioDecoder> create_audio_decoder(int sample_rate, int channels,
                                                   int frame_length, std::string &err);

/// 本构建是否编入音频解码后端，可用于在起流前判断能力；true 不保证初始化成功。
///
/// 它只描述后端是否存在，不代表任意 AAC-ELD 配置都受支持。新增配置需要核对
/// AudioSpecificConfig，并以同一份载荷检查解码后的采样数与内容。
#if defined(__APPLE__) || defined(SCRCTL_HAVE_LIBAV)
inline constexpr bool kHaveAudioDecoder = true;
#else
inline constexpr bool kHaveAudioDecoder = false;
#endif

inline constexpr const char *kNoAudioDecoderMessage =
    SCRCTL_N_(
        "AAC-ELD decoding is unavailable in this build. Video can continue without audio.");

}  // namespace scrctl
