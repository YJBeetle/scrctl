#include "i18n/Translation.h"
#include "media/RecordingMuxer.h"

#include "media/RecordingVideoConfig.h"

#include <array>
#include <cstring>
#include <limits>
#include <utility>

#ifdef SCRCTL_HAVE_LIBAVFORMAT
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/version.h>
}
#endif

namespace scrctl::media {
namespace {
[[maybe_unused]] constexpr const char* unavailable = SCRCTL_N_("Container recording requires libavformat and libavcodec");

#ifdef SCRCTL_HAVE_LIBAVFORMAT
std::string libav_error(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, text, sizeof(text));
    return text;
}

struct ContextDeleter {
    void operator()(AVCodecContext* context) const { avcodec_free_context(&context); }
};

// 参数检查与 codecpar 导出使用同一份原始字节；不经临时文件或手写 hvcC 转换。
bool video_parameters(AVCodecParameters* output, const RecordingMuxer::Options& options,
                      std::string& error) {
    const AVCodec* codec = avcodec_find_decoder_by_name("hevc");
    if (codec == nullptr) {
        error = SCRCTL_TR("libavcodec has no software HEVC decoder");
        return false;
    }
    std::unique_ptr<AVCodecContext, ContextDeleter> context(avcodec_alloc_context3(codec));
    if (context == nullptr) {
        error = SCRCTL_TR("Cannot allocate the HEVC recording context");
        return false;
    }
    context->thread_count = 1;
    context->err_recognition = AV_EF_EXPLODE;
    const std::array<std::span<const uint8_t>, 3> parameters{
        options.vps, options.sps, options.pps};
    const std::size_t size = options.vps.size() + options.sps.size() + options.pps.size() + 12;
    context->extradata = static_cast<uint8_t*>(av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE));
    if (context->extradata == nullptr) {
        error = SCRCTL_TR("Cannot allocate HEVC recording extradata");
        return false;
    }
    context->extradata_size = static_cast<int>(size);
    auto* target = context->extradata;
    constexpr std::array<uint8_t, 4> prefix{0, 0, 0, 1};
    for (const auto nal : parameters) {
        std::memcpy(target, prefix.data(), prefix.size());
        target += prefix.size();
        std::memcpy(target, nal.data(), nal.size());
        target += nal.size();
    }
    const int opened = avcodec_open2(context.get(), codec, nullptr);
    if (opened < 0) {
        error = SCRCTL_TR("Cannot open the HEVC recording configuration: ") + libav_error(opened);
        return false;
    }
    const int copied = avcodec_parameters_from_context(output, context.get());
    if (copied < 0) {
        error = SCRCTL_TR("Cannot export HEVC recording parameters: ") + libav_error(copied);
        return false;
    }
    return true;
}

bool audio_parameters(AVCodecParameters* output, const RecordingMuxer::Audio& audio,
                      std::string& error) {
    output->codec_type = AVMEDIA_TYPE_AUDIO;
    output->codec_id = AV_CODEC_ID_AAC;
#ifdef AV_PROFILE_AAC_ELD
    output->profile = AV_PROFILE_AAC_ELD;
#else
    output->profile = FF_PROFILE_AAC_ELD;
#endif
    output->sample_rate = audio.sample_rate;
    output->frame_size = audio.frame_samples;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 24, 100)
    av_channel_layout_default(&output->ch_layout, audio.channels);
#else
    output->channels = audio.channels;
    output->channel_layout = AV_CH_LAYOUT_STEREO;
#endif
    // 与现有 AAC-ELD 解码器使用的已验证 ASC 相同：48 kHz、双声道，
    // frameLengthFlag=1 对应 480，0 对应 512。没有 ADTS 或 SBR。
    const std::array<uint8_t, 4> asc{
        0xf8, 0xe6, static_cast<uint8_t>(audio.frame_samples == 480 ? 0x50 : 0x40), 0x00};
    output->extradata = static_cast<uint8_t*>(av_mallocz(asc.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (output->extradata == nullptr) {
        error = SCRCTL_TR("Cannot allocate AAC-ELD recording extradata");
        return false;
    }
    std::memcpy(output->extradata, asc.data(), asc.size());
    output->extradata_size = static_cast<int>(asc.size());
    return true;
}
#endif
}  // namespace

struct RecordingMuxer::Impl {
    std::string first_error;
    bool closed = false;

    bool fail(std::string reason, std::string& error) {
        if (first_error.empty()) first_error = std::move(reason);
        error = first_error;
        return false;
    }

#ifdef SCRCTL_HAVE_LIBAVFORMAT
    AVFormatContext* context = nullptr;
    AVPacket* packet = nullptr;
    AVStream* video = nullptr;
    AVStream* audio = nullptr;
    Format format = Format::Mp4;
    bool header_written = false;
    std::optional<int64_t> video_dts, audio_dts;

    void remember(std::string reason) {
        if (first_error.empty()) first_error = std::move(reason);
    }

    void check_io(const char* operation) {
        if (context != nullptr && context->pb != nullptr && context->pb->error < 0) {
            remember(std::string(operation) + ": " + libav_error(context->pb->error));
        }
    }

    bool write(std::span<const uint8_t> bytes, Timing timing, AVStream* stream,
               std::optional<int64_t>& last_dts, bool is_video, bool keyframe,
               std::string& error) {
        error.clear();
        if (!first_error.empty()) return fail(first_error, error);
        if (closed) {
            error = SCRCTL_TR("The recording file has already been closed");
            return false;
        }
        if (stream == nullptr) return fail(SCRCTL_TR("This recording has no audio track"), error);
        if (bytes.empty() || bytes.size() > kMaxRecordingMuxerPacketBytes) {
            return fail(SCRCTL_TR("Recording access unit is empty or exceeds the 16 MiB packet budget"), error);
        }
        if (is_video) {
            const bool annex_b = bytes.size() >= 4 && bytes[0] == 0 && bytes[1] == 0 &&
                (bytes[2] == 1 || (bytes[2] == 0 && bytes[3] == 1));
            if (!annex_b) return fail(SCRCTL_TR("HEVC recording requires an Annex-B access unit"), error);
            if (timing.pts_us != timing.dts_us) {
                return fail(SCRCTL_TR("This HEVC configuration requires equal recording PTS and DTS"), error);
            }
            if (format == Format::Mp4 && timing.duration_us == 0) {
                return fail(SCRCTL_TR("MP4 recording requires a known positive HEVC access unit duration"), error);
            }
        }
        if (timing.pts_us == AV_NOPTS_VALUE || timing.dts_us == AV_NOPTS_VALUE ||
            timing.duration_us < 0 || (!is_video && timing.duration_us == 0) ||
            timing.pts_us > std::numeric_limits<int64_t>::max() - timing.duration_us ||
            timing.dts_us > std::numeric_limits<int64_t>::max() - timing.duration_us) {
            return fail(SCRCTL_TR("Recording timestamps or duration are invalid or overflow"), error);
        }
        if (format == Format::Matroska && (timing.pts_us < 0 || timing.dts_us < 0)) {
            return fail(SCRCTL_TR("Matroska recording requires a nonnegative common origin for both tracks"), error);
        }
        av_packet_unref(packet);
        const int allocated = av_new_packet(packet, static_cast<int>(bytes.size()));
        if (allocated < 0) return fail(SCRCTL_TR("Cannot allocate a recording packet: ") + libav_error(allocated), error);
        std::memcpy(packet->data, bytes.data(), bytes.size());
        packet->stream_index = stream->index;
        packet->pts = timing.pts_us;
        packet->dts = timing.dts_us;
        packet->duration = timing.duration_us;
        packet->pos = -1;
        if (keyframe) packet->flags |= AV_PKT_FLAG_KEY;
        // write_header 可改变 time_base；必须使用当前流的实际值，不能保留先验微秒。
        av_packet_rescale_ts(packet, AVRational{1, 1000000}, stream->time_base);
        if (packet->pts == AV_NOPTS_VALUE || packet->dts == AV_NOPTS_VALUE ||
            packet->pts == std::numeric_limits<int64_t>::max() ||
            packet->dts == std::numeric_limits<int64_t>::max() || packet->duration < 0 ||
            (timing.duration_us != 0 && packet->duration == 0) ||
            packet->pts > std::numeric_limits<int64_t>::max() - packet->duration ||
            packet->dts > std::numeric_limits<int64_t>::max() - packet->duration) {
            av_packet_unref(packet);
            return fail(SCRCTL_TR("Recording timestamps cannot be represented in the container time base"), error);
        }
        if (last_dts.has_value() && packet->dts <= *last_dts) {
            av_packet_unref(packet);
            return fail(SCRCTL_TR("Recording DTS must increase within each track and its container time base"), error);
        }
        // movenc 的 sample duration 使用有符号 32 位差值；超限应明确失败，
        // 不能让未知时长或极大时间间隔在写 trailer 时触发库内部断言。
        if (format == Format::Mp4 &&
            (packet->duration > std::numeric_limits<int>::max() ||
             (last_dts.has_value() &&
              *last_dts <= std::numeric_limits<int64_t>::max() - std::numeric_limits<int>::max() &&
              packet->dts > *last_dts + std::numeric_limits<int>::max()))) {
            av_packet_unref(packet);
            return fail(SCRCTL_TR("Recording sample duration or DTS interval exceeds the MP4 range"), error);
        }
        const int64_t written_dts = packet->dts;
        const int written = av_interleaved_write_frame(context, packet);
        av_packet_unref(packet);
        if (written < 0) remember(SCRCTL_TR("Cannot write a recording packet: ") + libav_error(written));
        check_io(SCRCTL_TR("Cannot write the recording file"));
        if (!first_error.empty()) return fail(first_error, error);
        last_dts = written_dts;
        return true;
    }
#endif

    bool finish(std::string& error) {
#ifdef SCRCTL_HAVE_LIBAVFORMAT
        if (!closed) {
            if (context != nullptr && header_written) {
                const int trailer = av_write_trailer(context);
                if (trailer < 0) remember(SCRCTL_TR("Cannot finish the recording container: ") + libav_error(trailer));
                check_io(SCRCTL_TR("Cannot finish the recording file"));
            }
            if (context != nullptr && context->pb != nullptr) {
                avio_flush(context->pb);
                check_io(SCRCTL_TR("Cannot flush the recording file"));
                const int closed_io = avio_closep(&context->pb);
                if (closed_io < 0) remember(SCRCTL_TR("Cannot close the recording file: ") + libav_error(closed_io));
            }
            av_packet_free(&packet);
            avformat_free_context(context);
            context = nullptr;
            video = audio = nullptr;
            closed = true;
        }
#else
        closed = true;
        if (first_error.empty()) first_error = SCRCTL_TR(unavailable);
#endif
        error = first_error;
        return first_error.empty();
    }
};

RecordingMuxer::RecordingMuxer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

RecordingMuxer::~RecordingMuxer() {
    std::string ignored;
    impl_->finish(ignored);
}

bool RecordingMuxer::available() noexcept {
#ifdef SCRCTL_HAVE_LIBAVFORMAT
    return true;
#else
    return false;
#endif
}

std::unique_ptr<RecordingMuxer> RecordingMuxer::open(const Options& options, std::string& error) {
    error.clear();
#ifndef SCRCTL_HAVE_LIBAVFORMAT
    (void)options;
    error = SCRCTL_TR(unavailable);
    return nullptr;
#else
    if (options.path.empty() || options.path.find('\0') != std::string::npos) {
        error = SCRCTL_TR("Recording requires a nonempty local file path without NUL bytes");
        return nullptr;
    }
    const char* format = nullptr;
    switch (options.format) {
        case Format::Mp4: format = "mp4"; break;
        case Format::Matroska: format = "matroska"; break;
        default: error = SCRCTL_TR("Unsupported recording container format"); return nullptr;
    }
    if (options.audio.has_value() &&
        (options.audio->sample_rate != 48000 || options.audio->channels != 2 ||
         (options.audio->frame_samples != 480 && options.audio->frame_samples != 512))) {
        error = SCRCTL_TR("AAC-ELD recording supports only 48 kHz stereo with 480 or 512 samples per packet");
        return nullptr;
    }
    const auto checked = inspect_recording_video_config(options.vps, options.sps, options.pps);
    if (!checked.permits_equal_dts_pts()) {
        error = checked.status == RecordingVideoConfig::Status::Reorder
            ? SCRCTL_TR("Recording HEVC with frame reordering is not supported")
            : SCRCTL_TR("Cannot validate the HEVC recording configuration: ") + checked.error;
        return nullptr;
    }
    auto result = std::unique_ptr<RecordingMuxer>(new RecordingMuxer(std::make_unique<Impl>()));
    auto& impl = *result->impl_;
    impl.format = options.format;
    const int allocated = avformat_alloc_output_context2(&impl.context, nullptr, format, options.path.c_str());
    if (allocated < 0 || impl.context == nullptr) {
        impl.fail(SCRCTL_TR("Cannot allocate the recording container: ") + libav_error(allocated), error);
        return nullptr;
    }
#ifdef AVFMT_AVOID_NEG_TS_DISABLED
    impl.context->avoid_negative_ts = AVFMT_AVOID_NEG_TS_DISABLED;
#else
    // FFmpeg 5.0 的公开字段以 0 表示 disabled，尚未提供同名常量。
    impl.context->avoid_negative_ts = 0;
#endif
    // 静止视频不会阻塞持续音频直到下一幅图像；这里只使用 libavformat 的有界交错。
    impl.context->max_interleave_delta = 100000;
    impl.video = avformat_new_stream(impl.context, nullptr);
    if (impl.video == nullptr) {
        impl.fail(SCRCTL_TR("Cannot allocate the HEVC recording track"), error);
        return nullptr;
    }
    if (!video_parameters(impl.video->codecpar, options, error)) {
        impl.fail(error, error);
        return nullptr;
    }
    impl.video->codecpar->codec_tag = options.format == Format::Mp4 ? MKTAG('h', 'v', 'c', '1') : 0;
    impl.video->time_base = AVRational{1, 1000000};
    impl.video->avg_frame_rate = impl.video->r_frame_rate = AVRational{0, 1};
    if (options.audio.has_value()) {
        impl.audio = avformat_new_stream(impl.context, nullptr);
        if (impl.audio == nullptr) {
            impl.fail(SCRCTL_TR("Cannot allocate the AAC-ELD recording track"), error);
            return nullptr;
        }
        if (!audio_parameters(impl.audio->codecpar, *options.audio, error)) {
            impl.fail(error, error);
            return nullptr;
        }
        impl.audio->time_base = AVRational{1, 1000000};
    }
    impl.packet = av_packet_alloc();
    if (impl.packet == nullptr) {
        impl.fail(SCRCTL_TR("Cannot allocate the recording packet"), error);
        return nullptr;
    }
    AVDictionary* io_options = nullptr;
    const int limited = av_dict_set(&io_options, "protocol_whitelist", "file", 0);
    if (limited < 0) {
        av_dict_free(&io_options);
        impl.fail(SCRCTL_TR("Cannot restrict recording output to local files: ") + libav_error(limited), error);
        return nullptr;
    }
    const int opened = avio_open2(&impl.context->pb, options.path.c_str(), AVIO_FLAG_WRITE, nullptr, &io_options);
    av_dict_free(&io_options);
    if (opened < 0) {
        impl.fail(SCRCTL_TR("Cannot open the recording file: ") + libav_error(opened), error);
        return nullptr;
    }
    // 普通 MP4 保留完整 AAC roll sample groups；不通过分片或关闭 edit list
    // 绕过尾帧/priming 约束。未知视频时长在 write 前明确拒绝，由调用方处理。
    const int header = avformat_write_header(impl.context, nullptr);
    if (header < 0) impl.remember(SCRCTL_TR("Cannot write the recording header: ") + libav_error(header));
    impl.header_written = header >= 0;
    // 部分协议使用缓冲写入。显式 flush 才能在 open 阶段识别真正的头部 I/O 失败。
    avio_flush(impl.context->pb);
    impl.check_io(SCRCTL_TR("Cannot write the recording header"));
    if (!impl.first_error.empty()) {
        error = impl.first_error;
        return nullptr;
    }
    return result;
#endif
}

bool RecordingMuxer::write_video(std::span<const uint8_t> bytes, Timing timing,
                                 bool keyframe, std::string& error) {
#ifdef SCRCTL_HAVE_LIBAVFORMAT
    return impl_->write(bytes, timing, impl_->video, impl_->video_dts, true, keyframe, error);
#else
    (void)bytes; (void)timing; (void)keyframe;
    return impl_->fail(SCRCTL_TR(unavailable), error);
#endif
}

bool RecordingMuxer::write_audio(std::span<const uint8_t> bytes, Timing timing, std::string& error) {
#ifdef SCRCTL_HAVE_LIBAVFORMAT
    return impl_->write(bytes, timing, impl_->audio, impl_->audio_dts, false, true, error);
#else
    (void)bytes; (void)timing;
    return impl_->fail(SCRCTL_TR(unavailable), error);
#endif
}

bool RecordingMuxer::finish(std::string& error) { return impl_->finish(error); }

const std::string& RecordingMuxer::error() const noexcept { return impl_->first_error; }

}  // namespace scrctl::media
