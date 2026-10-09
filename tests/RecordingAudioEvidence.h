#pragma once

// Test evidence from the actual container and AAC decoder. FFmpeg 6.1's MKV
// muxer omits AAC DefaultDuration/BlockDuration even with codecpar.frame_size;
// packet.duration alone therefore cannot prove or disprove the frame length.
#ifdef SCRCTL_HAVE_LIBAVFORMAT
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/parseutils.h>
#include <libavutil/version.h>
}

#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace recording_test {
inline std::string versions() {
    std::ostringstream out;
    for (const auto value : {avcodec_version(), avformat_version(), avutil_version()})
        out << (value >> 16) << '.' << ((value >> 8) & 255u) << '.' << (value & 255u) << ' ';
    return out.str();
}
inline std::string av_error(int value) {
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(value, text, sizeof(text));
    return text;
}
inline bool matroska(const AVFormatContext& input) {
    return std::string_view(input.iformat->name).starts_with("matroska");
}
struct AudioPacketEvidence {
    std::string format, file, error, asc;
    int64_t pts_us = AV_NOPTS_VALUE, dts_us = AV_NOPTS_VALUE, duration_us = 0;
    int samples = 0, sample_rate = 0, channels = 0;
    bool is_matroska = false, decoded = false, configuration_valid = false;
    bool frame_valid() const {
        return configuration_valid && decoded && samples == 480 && sample_rate == 48000 && channels == 2;
    }
    bool duration_valid() const {
        return frame_valid() && (duration_us == 10000 || (is_matroska && duration_us == 0));
    }
    std::string diagnostic() const {
        std::ostringstream out;
        out << "format=" << format << " file=" << file << " versions=" << versions()
            << "PTS/DTS/duration_us=" << pts_us << '/' << dts_us << '/' << duration_us
            << " decoded=" << decoded << " samples/rate/channels=" << samples << '/'
            << sample_rate << '/' << channels << " ASC=" << asc
            << " configuration_valid=" << configuration_valid << " error=" << error;
        return out.str();
    }
};
inline AudioPacketEvidence inspect_audio_packet(const AVFormatContext& input, const AVPacket& packet) {
    AudioPacketEvidence result;
    result.format = input.iformat->name;
    result.file = input.url ? input.url : "";
    result.is_matroska = matroska(input);
    const auto* stream = input.streams[packet.stream_index];
    const auto* parameters = stream->codecpar;
    constexpr char hex[] = "0123456789abcdef";
    for (int i = 0; i < parameters->extradata_size; ++i) {
        result.asc += hex[parameters->extradata[i] >> 4];
        result.asc += hex[parameters->extradata[i] & 15];
    }
    result.configuration_valid = parameters->codec_id == AV_CODEC_ID_AAC &&
                                 parameters->sample_rate == 48000 && result.asc == "f8e65000";
    result.pts_us = av_rescale_q(packet.pts, stream->time_base, AVRational{1, 1000000});
    result.dts_us = av_rescale_q(packet.dts, stream->time_base, AVRational{1, 1000000});
    result.duration_us = av_rescale_q(packet.duration, stream->time_base, AVRational{1, 1000000});
    const auto* codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    struct ContextDelete { void operator()(AVCodecContext* value) const { avcodec_free_context(&value); } };
    struct FrameDelete { void operator()(AVFrame* value) const { av_frame_free(&value); } };
    std::unique_ptr<AVCodecContext, ContextDelete> context(avcodec_alloc_context3(codec));
    std::unique_ptr<AVFrame, FrameDelete> frame(av_frame_alloc());
    if (!codec || !context || !frame) { result.error = "AAC decoder allocation unavailable"; return result; }
    int status = avcodec_parameters_to_context(context.get(), stream->codecpar);
    context->thread_count = 1;
    context->err_recognition = AV_EF_EXPLODE;
    if (status >= 0) status = avcodec_open2(context.get(), codec, nullptr);
    if (status >= 0) status = avcodec_send_packet(context.get(), &packet);
    if (status >= 0) status = avcodec_receive_frame(context.get(), frame.get());
    if (status < 0) { result.error = av_error(status); return result; }
    result.samples = frame->nb_samples;
    result.sample_rate = frame->sample_rate;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 24, 100)
    result.channels = frame->ch_layout.nb_channels;
#else
    result.channels = frame->channels;
#endif
    // A single encoded packet must yield exactly one complete audio frame.
    status = avcodec_receive_frame(context.get(), frame.get());
    result.decoded = status == AVERROR(EAGAIN);
    if (!result.decoded) result.error = status >= 0 ? "multiple AAC frames in one packet" : av_error(status);
    return result;
}
struct AudioTailEvidence {
    std::string format, file;
    bool is_matroska = false;
    int64_t container_us = AV_NOPTS_VALUE, stream_us = AV_NOPTS_VALUE;
    std::optional<int64_t> tagged_us;
    bool valid(int64_t expected_end_us) const {
        if (is_matroska)
            return container_us == expected_end_us && tagged_us && *tagged_us == expected_end_us;
        return stream_us == expected_end_us;
    }
    std::string diagnostic(int64_t expected_end_us) const {
        std::ostringstream out;
        out << "format=" << format << " file=" << file << " versions=" << versions()
            << "container/stream/tagged_end_us=" << container_us << '/' << stream_us << '/';
        if (tagged_us) out << *tagged_us; else out << "missing";
        out << " expected_end_us=" << expected_end_us;
        return out.str();
    }
};
inline AudioTailEvidence inspect_audio_tail(const AVFormatContext& input, unsigned stream_index) {
    AudioTailEvidence result;
    result.format = input.iformat->name;
    result.file = input.url ? input.url : "";
    result.is_matroska = matroska(input);
    result.container_us = input.duration;
    const auto* stream = input.streams[stream_index];
    if (stream->duration != AV_NOPTS_VALUE)
        result.stream_us = av_rescale_q(stream->duration, stream->time_base, AVRational{1, 1000000});
    const auto* tag = av_dict_get(stream->metadata, "DURATION", nullptr, 0);
    int64_t tagged = 0;
    if (tag && av_parse_time(&tagged, tag->value, 1) >= 0) result.tagged_us = tagged;
    return result;
}
}  // namespace recording_test
#endif
