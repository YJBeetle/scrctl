#include "i18n/Translation.h"
// 当前 Apple 构建的 AAC-ELD 后端，输入与输出约定见 AudioDecoder.h。
#include "decode/AudioDecoder.h"

#include <AudioToolbox/AudioToolbox.h>

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace scrctl {
namespace {

[[nodiscard]] constexpr UInt32 fourcc(const char *s) {
    return (static_cast<UInt32>(static_cast<unsigned char>(s[0])) << 24) |
           (static_cast<UInt32>(static_cast<unsigned char>(s[1])) << 16) |
           (static_cast<UInt32>(static_cast<unsigned char>(s[2])) << 8) |
           static_cast<UInt32>(static_cast<unsigned char>(s[3]));
}

/// 格式化 OSStatus；四个字节均可打印时附加四字符码，便于诊断 AudioToolbox 错误。
std::string osstatus_text(OSStatus st) {
    const auto u = static_cast<UInt32>(st);
    const char raw[4] = {static_cast<char>((u >> 24) & 0xFF), static_cast<char>((u >> 16) & 0xFF),
                         static_cast<char>((u >> 8) & 0xFF), static_cast<char>(u & 0xFF)};
    std::string text = "0x" + std::to_string(static_cast<unsigned long long>(u));
    bool printable = true;
    for (const char c : raw) {
        if (std::isprint(static_cast<unsigned char>(c)) == 0) {
            printable = false;
        }
    }
    if (printable) {
        text += std::string(" ('") + raw + "')";
    }
    return text;
}

/// 一次 FillComplexBuffer 调用的输入状态。frame 内存由调用方拥有，Input 仅借用，
/// 生命周期覆盖该同步调用。回调首次提供一包，后续请求返回零包，不重复提交载荷；
/// packet description 存在 Input 内，和本次回调上下文具有相同生命周期。
struct Input {
    const uint8_t *data = nullptr;
    UInt32 size = 0;
    bool handed = false;
    AudioStreamPacketDescription desc {};
};

OSStatus input_proc(AudioConverterRef, UInt32 *io_number_packets, AudioBufferList *io_data,
                    AudioStreamPacketDescription **out_packet_desc, void *userdata) {
    auto *in = static_cast<Input *>(userdata);
    if (in->handed) {
        *io_number_packets = 0;
        io_data->mBuffers[0].mDataByteSize = 0;
        return noErr;
    }
    in->handed = true;
    io_data->mNumberBuffers = 1;
    io_data->mBuffers[0].mNumberChannels = 0;
    io_data->mBuffers[0].mData = const_cast<uint8_t *>(in->data);
    io_data->mBuffers[0].mDataByteSize = in->size;
    std::memset(&in->desc, 0, sizeof(in->desc));
    in->desc.mDataByteSize = in->size;
    *out_packet_desc = &in->desc;
    *io_number_packets = 1;
    return noErr;
}

/// 实例拥有 AudioConverter 和复用的输出缓冲。同一实例的 decode 与销毁需要串行。
/// PCM 在 FillComplexBuffer 成功后才追加到调用方 vector，返回后不借用 out_。
class AudioToolboxEldDecoder final : public AudioDecoder {
public:
    AudioToolboxEldDecoder(AudioConverterRef conv, int channels, int frame_length)
        : conv_(conv), channels_(channels), frame_length_(frame_length) {
        // 输出缓冲按两帧交织 PCM 的字节数预留，供转换器使用；每次调用请求的
        // 输出采样数仍为 frame_length，实际追加数量以返回的 frames 为准。
        out_.resize(static_cast<std::size_t>(frame_length) * 2 *
                    static_cast<std::size_t>(channels) * sizeof(int16_t));
    }

    ~AudioToolboxEldDecoder() override {
        if (conv_ != nullptr) {
            AudioConverterDispose(conv_);
        }
    }

    AudioToolboxEldDecoder(const AudioToolboxEldDecoder &) = delete;
    AudioToolboxEldDecoder &operator=(const AudioToolboxEldDecoder &) = delete;

    bool decode(std::span<const uint8_t> frame, std::vector<int16_t> &pcm,
                std::string &err) override {
        if (frame.empty()) {
            return true;
        }
        Input in;
        in.data = frame.data();
        in.size = static_cast<UInt32>(frame.size());

        // 当前目标为交织 PCM，只需一个 AudioBuffer 槽。按尾部数组布局分配
        // AudioBufferList；其数据指针借用实例拥有的 out_，不是额外分配的样本。
        auto *list = static_cast<AudioBufferList *>(
            std::malloc(offsetof(AudioBufferList, mBuffers) + sizeof(AudioBuffer)));
        if (list == nullptr) {
            err = SCRCTL_TR("Failed to allocate AudioBufferList");
            return false;
        }
        list->mNumberBuffers = 1;
        list->mBuffers[0].mNumberChannels = static_cast<UInt32>(channels_);
        list->mBuffers[0].mData = out_.data();
        list->mBuffers[0].mDataByteSize = static_cast<UInt32>(out_.size());

        UInt32 frames = static_cast<UInt32>(frame_length_);
        const OSStatus st =
            AudioConverterFillComplexBuffer(conv_, input_proc, &in, &frames, list, nullptr);
        if (st != noErr) {
            std::free(list);
            err = SCRCTL_TR("AudioConverterFillComplexBuffer failed: ") + osstatus_text(st);
            return false;
        }
        const std::size_t samples =
            static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels_);
        const auto *src = static_cast<const int16_t *>(list->mBuffers[0].mData);
        pcm.insert(pcm.end(), src, src + samples);
        std::free(list);
        return true;
    }

    [[nodiscard]] const char *backend_name() const override { return "AudioToolbox/aac-eld"; }

private:
    AudioConverterRef conv_ = nullptr;
    int channels_ = 0;
    int frame_length_ = 0;
    std::vector<uint8_t> out_;
};

}  // namespace

std::unique_ptr<AudioDecoder> create_audio_decoder(int sample_rate, int channels,
                                                   int frame_length, std::string &err) {
    AudioStreamBasicDescription src {};
    src.mSampleRate = static_cast<Float64>(sample_rate);
    // 'aace' 是 kAudioFormatMPEG4AAC_ELD；格式码指定 ELD，协商的
    // frame_length 指定每包每声道采样数。已有设备配置为 480，不能仅据此区分编码。
    src.mFormatID = fourcc("aace");
    src.mFramesPerPacket = static_cast<UInt32>(frame_length);
    src.mChannelsPerFrame = static_cast<UInt32>(channels);
    // mBytesPerPacket 保持 0 表示变长包。当前一包一帧的设备载荷长度并不固定，
    // 实际字节数由 input_proc 的 AudioStreamPacketDescription 提供。

    AudioStreamBasicDescription dst {};
    dst.mSampleRate = static_cast<Float64>(sample_rate);
    dst.mFormatID = fourcc("lpcm");
    dst.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    dst.mBytesPerPacket = static_cast<UInt32>(channels) * 2;
    dst.mFramesPerPacket = 1;
    dst.mBytesPerFrame = static_cast<UInt32>(channels) * 2;
    dst.mChannelsPerFrame = static_cast<UInt32>(channels);
    dst.mBitsPerChannel = 16;

    AudioConverterRef conv = nullptr;
    const OSStatus st = AudioConverterNew(&src, &dst, &conv);
    if (st != noErr) {
        err = SCRCTL_TR("AudioConverterNew failed: ") + osstatus_text(st);
        return nullptr;
    }
    // 当前适配器不设置 kAudioConverterDecompressionMagicCookie，以上述 ASBD
    // 创建转换器。已有 macOS/设备样本在未设置 cookie 时解出 1406/1406 帧，
    // 每帧每声道 480 个采样；同次设置 cookie 的尝试返回 '!dat'。这仅说明已测
    // 配置可按当前路径解码，不代表其他 ELD 配置或平台都不需要 cookie（docs §17.1）。
    return std::make_unique<AudioToolboxEldDecoder>(conv, channels, frame_length);
}

}  // namespace scrctl
