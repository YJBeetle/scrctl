#include "decode/FFmpegEldDecoder.h"

#include "i18n/Translation.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
#include <libavutil/version.h>
#include <libswresample/swresample.h>
}

#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

namespace scrctl {
namespace {

struct CodecDeleter {
    void operator()(AVCodecContext *ctx) const { avcodec_free_context(&ctx); }
};
struct PacketDeleter {
    void operator()(AVPacket *packet) const { av_packet_free(&packet); }
};
struct FrameDeleter {
    void operator()(AVFrame *frame) const { av_frame_free(&frame); }
};
struct ResamplerDeleter {
    void operator()(SwrContext *ctx) const { swr_free(&ctx); }
};
using CodecPtr = std::unique_ptr<AVCodecContext, CodecDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using ResamplerPtr = std::unique_ptr<SwrContext, ResamplerDeleter>;

std::string av_error(int status) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(status, buffer, sizeof(buffer));
    return buffer;
}

bool has_stereo_layout(const AVFrame *frame) {
    // FFmpeg 5.0 使用 channels/channel_layout；5.1 起采用 AVChannelLayout。
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 24, 100)
    const AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    return av_channel_layout_compare(&frame->ch_layout, &stereo) == 0;
#else
    return frame->channels == 2 && frame->channel_layout == AV_CH_LAYOUT_STEREO;
#endif
}

class FFmpegEldDecoder final : public AudioDecoder {
public:
    FFmpegEldDecoder(CodecPtr codec, PacketPtr packet, FramePtr frame, int frame_length)
        : codec_(std::move(codec)), packet_(std::move(packet)), frame_(std::move(frame)),
          frame_length_(frame_length) {}

    bool decode(std::span<const uint8_t> encoded, std::vector<int16_t> &pcm,
                std::string &err) override {
        err.clear();
        if (encoded.empty()) {
            return true;
        }
        if (encoded.size() > kMaxEldPacketBytes ||
            encoded.size() > static_cast<size_t>(std::numeric_limits<int>::max() -
                                                 AV_INPUT_BUFFER_PADDING_SIZE)) {
            err = SCRCTL_TR("AAC-ELD packet exceeds the single-datagram size limit");
            return false;
        }

        av_packet_unref(packet_.get());
        int status = av_new_packet(packet_.get(), static_cast<int>(encoded.size()));
        if (status < 0) {
            return fail(SCRCTL_TR("Failed to allocate FFmpeg AAC-ELD packet: ") +
                            av_error(status), err);
        }
        // av_new_packet 提供解码器所需的零填充尾部。输入 span 不必包含 padding，
        // packet 拥有载荷；调用返回后解码器不会借用调用方内存。
        std::memcpy(packet_->data, encoded.data(), encoded.size());
        status = avcodec_send_packet(codec_.get(), packet_.get());
        av_packet_unref(packet_.get());
        if (status < 0) {
            return fail(SCRCTL_TR("FFmpeg AAC-ELD rejected the packet: ") + av_error(status), err);
        }

        // 先收集本次输出，再一次性追加；解析或转换中途失败不会留下部分 PCM。
        std::vector<int16_t> decoded;
        for (;;) {
            av_frame_unref(frame_.get());
            status = avcodec_receive_frame(codec_.get(), frame_.get());
            if (status == AVERROR(EAGAIN)) {
                break;
            }
            if (status < 0) {
                return fail(SCRCTL_TR("FFmpeg AAC-ELD decode failed: ") + av_error(status), err);
            }
            if (frame_->sample_rate != 48000 || !has_stereo_layout(frame_.get()) ||
                frame_->nb_samples != frame_length_ || !decoded.empty()) {
                return fail(SCRCTL_TR("FFmpeg AAC-ELD returned an unexpected audio format"), err);
            }
            if (!convert(decoded, err)) {
                reset_after_error();
                return false;
            }
        }
        if (decoded.size() > pcm.max_size() - pcm.size()) {
            return fail(SCRCTL_TR("AAC-ELD PCM output exceeds the buffer size limit"), err);
        }
        pcm.insert(pcm.end(), decoded.begin(), decoded.end());
        return true;
    }

    [[nodiscard]] const char *backend_name() const override { return "libavcodec/aac-eld"; }

private:
    void reset_after_error() {
        avcodec_flush_buffers(codec_.get());
        av_frame_unref(frame_.get());
        av_packet_unref(packet_.get());
        converter_.reset();
    }

    bool fail(std::string message, std::string &err) {
        err = std::move(message);
        reset_after_error();
        return false;
    }

    bool convert(std::vector<int16_t> &decoded, std::string &err) {
        if (frame_->format < 0 || frame_->format >= AV_SAMPLE_FMT_NB ||
            frame_->extended_data == nullptr) {
            err = SCRCTL_TR("FFmpeg AAC-ELD returned an unexpected audio format");
            return false;
        }
        const auto format = static_cast<AVSampleFormat>(frame_->format);
        if (frame_->extended_data[0] == nullptr ||
            (av_sample_fmt_is_planar(format) && frame_->extended_data[1] == nullptr)) {
            err = SCRCTL_TR("FFmpeg AAC-ELD returned an unexpected audio format");
            return false;
        }
        if (!converter_ || input_format_ != format) {
            converter_.reset();
            SwrContext *raw = nullptr;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 24, 100)
            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            int status = swr_alloc_set_opts2(&raw, &stereo, AV_SAMPLE_FMT_S16, 48000,
                                             &frame_->ch_layout, format, 48000, 0, nullptr);
#else
            raw = swr_alloc_set_opts(nullptr, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, 48000,
                                     AV_CH_LAYOUT_STEREO, format, 48000, 0, nullptr);
            int status = raw != nullptr ? 0 : AVERROR(ENOMEM);
#endif
            converter_.reset(raw);
            if (status < 0 || !converter_) {
                err = SCRCTL_TR("Failed to configure AAC-ELD PCM conversion: ") + av_error(status);
                return false;
            }
            status = swr_init(converter_.get());
            if (status < 0) {
                err = SCRCTL_TR("Failed to configure AAC-ELD PCM conversion: ") + av_error(status);
                return false;
            }
            input_format_ = format;
        }

        const int capacity = swr_get_out_samples(converter_.get(), frame_->nb_samples);
        // 输入采样率与输出相同，每次转换应输出这一帧。仍按库给出的容量分配，
        // 并限制上限；不把负数或异常容量转换为 size_t 后直接分配。
        if (capacity < frame_length_ || capacity > 2048) {
            err = SCRCTL_TR("AAC-ELD PCM conversion returned an invalid sample count");
            return false;
        }
        decoded.resize(static_cast<size_t>(capacity) * 2);
        std::array<uint8_t *, 1> output = {
            reinterpret_cast<uint8_t *>(decoded.data()),
        };
        std::array<const uint8_t *, 2> input = {
            frame_->extended_data[0],
            av_sample_fmt_is_planar(format) ? frame_->extended_data[1] : nullptr,
        };
        const int samples = swr_convert(converter_.get(), output.data(), capacity,
                                        input.data(), frame_->nb_samples);
        if (samples < 0) {
            err = SCRCTL_TR("AAC-ELD PCM conversion failed: ") + av_error(samples);
            return false;
        }
        if (samples != frame_length_) {
            err = SCRCTL_TR("AAC-ELD PCM conversion returned an invalid sample count");
            return false;
        }
        decoded.resize(static_cast<size_t>(samples) * 2);
        return true;
    }

    CodecPtr codec_;
    PacketPtr packet_;
    FramePtr frame_;
    ResamplerPtr converter_;
    AVSampleFormat input_format_ = AV_SAMPLE_FMT_NONE;
    int frame_length_;
};

} // namespace

std::unique_ptr<AudioDecoder> create_ffmpeg_eld_decoder(int sample_rate, int channels,
                                                      int frame_length, std::string &err) {
    err.clear();
    if (sample_rate != 48000 || channels != 2 || (frame_length != 480 && frame_length != 512)) {
        err = SCRCTL_TR("FFmpeg AAC-ELD requires 48000 Hz, stereo, and 480 or 512 samples per frame");
        return nullptr;
    }
    const AVCodec *codec = avcodec_find_decoder_by_name("aac");
    if (codec == nullptr) {
        err = SCRCTL_TR("FFmpeg has no native AAC decoder");
        return nullptr;
    }
    CodecPtr ctx(avcodec_alloc_context3(codec));
    PacketPtr packet(av_packet_alloc());
    FramePtr frame(av_frame_alloc());
    if (!ctx || !packet || !frame) {
        err = SCRCTL_TR("Failed to allocate FFmpeg AAC-ELD decoder");
        return nullptr;
    }
    // AOT=39（escape 31 + 7）、48 kHz index=3、channelConfiguration=2，
    // frameLengthFlag 为 1 时是 480，为 0 时是 512。其余配置位为 0：
    // 无 error resilience、无 LD-SBR、ELDEXT_TERM、epConfig=0。不要沿用
    // F8 E6 28：它声明单声道、512 和 resilience flags=4，不能代表当前配置。
    std::array<uint8_t, 4> asc = {0xf8, 0xe6,
                                 static_cast<uint8_t>(frame_length == 480 ? 0x50 : 0x40), 0};
    ctx->extradata = static_cast<uint8_t *>(av_mallocz(asc.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (ctx->extradata == nullptr) {
        err = SCRCTL_TR("Failed to allocate FFmpeg AAC-ELD decoder");
        return nullptr;
    }
    ctx->extradata_size = static_cast<int>(asc.size());
    std::memcpy(ctx->extradata, asc.data(), asc.size());
    // libavcodec 的 ELD 输出路径使用上下文采样率；不能只提供 ASC 后期待它回填。
    ctx->sample_rate = sample_rate;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 24, 100)
    av_channel_layout_default(&ctx->ch_layout, channels);
#else
    ctx->channels = channels;
    ctx->channel_layout = AV_CH_LAYOUT_STEREO;
#endif
    ctx->thread_count = 1;
    const int status = avcodec_open2(ctx.get(), codec, nullptr);
    if (status < 0) {
        err = SCRCTL_TR("Failed to initialize FFmpeg AAC-ELD decoder: ") + av_error(status);
        return nullptr;
    }
    return std::make_unique<FFmpegEldDecoder>(std::move(ctx), std::move(packet), std::move(frame),
                                             frame_length);
}

} // namespace scrctl
