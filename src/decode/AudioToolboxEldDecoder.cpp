#include "i18n/Translation.h"
// AAC-ELD 的 AudioToolbox 后端。见 AudioDecoder.h 上那段"为什么只能走这里"。
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

/// OSStatus 常常本身就是一个四字符码（'!dat'、'fmt?'），打十进制没人看得懂。
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

/// 一次解码的输入状态。AudioConverter 是"我要数据你来给"的拉模型，所以喂一包就得
/// 给一个只认这一包的上下文，第二次被问要说"没了"。
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

class AudioToolboxEldDecoder final : public AudioDecoder {
public:
    AudioToolboxEldDecoder(AudioConverterRef conv, int channels, int frame_length)
        : conv_(conv), channels_(channels), frame_length_(frame_length) {
        // 一次最多可能出两帧（转换器攒着 priming 那一帧时会连着给），按两倍留。
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

        // AudioBufferList 的尾部数组必须变长分配：sizeof 它只含一个槽，
        // 而 mNumberBuffers 说的是里面有几个。
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
    // 'aace' = kAudioFormatMPEG4AAC。ELD 不靠 format ID 区分，靠的就是
    // mFramesPerPacket=480 这一位——1024 才是 LC。
    src.mFormatID = fourcc("aace");
    src.mFramesPerPacket = static_cast<UInt32>(frame_length);
    src.mChannelsPerFrame = static_cast<UInt32>(channels);
    // mBytesPerPacket 留 0 = 变长包。这条流一包一帧而每包尺寸都不同（实测 246~400
    // 字节），写成一个定值会让转换器按定长切。

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
    // 这里**不设** kAudioConverterDecompressionMagicCookie（'dmgc'）。试过：这台
    // macOS 上 SetProperty('dmgc') 无论塞规范拼的 ASC 还是苹果自己那份 cookie 都回
    // '!dat'，而不塞照样 1406/1406 全解出来、每帧正好 480 采样（docs §17.1）。
    // 也就是说 ELD 的档位信息 ASBD 里已经有了，多设一次只会把一个不重要的错误
    // 变成"看起来像失败"。
    return std::make_unique<AudioToolboxEldDecoder>(conv, channels, frame_length);
}

}  // namespace scrctl
