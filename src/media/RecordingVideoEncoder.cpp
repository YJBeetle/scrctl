#include "media/RecordingVideoEncoder.h"
#include "media/RecordingVideoConfig.h"
#include "media/RecordingMuxer.h"
#include "i18n/Translation.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>

#ifdef SCRCTL_HAVE_LIBAV
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
#endif

namespace scrctl::media {
namespace {
constexpr uint64_t max_pixels = 16u * 1024u * 1024u;
constexpr const char* unavailable = SCRCTL_N_("Upright recording requires an available libx265 or hevc_videotoolbox encoder");
#ifdef SCRCTL_HAVE_LIBAV
const AVCodec* select_encoder(bool hardware = true) {
    if (hardware) {
        if (const auto* codec = avcodec_find_encoder_by_name("hevc_videotoolbox")) return codec;
    }
    const AVCodec* codec = avcodec_find_encoder_by_name("libx265");
    return codec ? codec : avcodec_find_encoder_by_name("hevc_videotoolbox");
}
std::string codec_error(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, text, sizeof(text));
    return text;
}
// Preserve raw NAL bytes. FFmpeg software encoders expose Annex-B while
// VideoToolbox commonly exposes hvcC/length-prefixed packets.
bool annex_b(std::span<const uint8_t> bytes, std::vector<Nal>& nals) {
    auto prefix = [&](std::size_t i) -> std::size_t {
        if (i + 3 <= bytes.size() && bytes[i] == 0 && bytes[i+1] == 0) {
            if (bytes[i+2] == 1) return 3;
            if (i + 4 <= bytes.size() && bytes[i+2] == 0 && bytes[i+3] == 1) return 4;
        }
        return 0;
    };
    const auto first = prefix(0);
    if (!first) return false;
    std::size_t begin = first;
    for (std::size_t i = begin; i < bytes.size();) {
        const auto size = prefix(i);
        if (!size) { ++i; continue; }
        if (i <= begin) return false;
        nals.emplace_back(bytes.begin()+begin, bytes.begin()+i);
        begin = i + size;
        i = begin;
    }
    if (begin >= bytes.size()) return false;
    nals.emplace_back(bytes.begin()+begin, bytes.end());
    return true;
}
#endif
}

struct RecordingVideoEncoder::Impl {
    Options options;
    std::string backend, first_error;
    bool ended = false;
    std::optional<int64_t> last_input, last_output;
    Nal vps, sps, pps;
    std::vector<uint8_t> rotated_pixels;
#ifdef SCRCTL_HAVE_LIBAV
    AVCodecContext* context = nullptr;
    AVFrame* image = nullptr;
    AVPacket* packet = nullptr;
    SwsContext* scale = nullptr;
    int length_bytes = 4;
    ~Impl() {
        sws_freeContext(scale);
        av_packet_free(&packet);
        av_frame_free(&image);
        avcodec_free_context(&context);
    }
    bool remember(const std::vector<Nal>& nals) {
        for (const auto& nal : nals) {
            if (nal.size() < 2) return false;
            switch ((nal[0] >> 1) & 63) {
                case 32: vps = nal; break;
                case 33: sps = nal; break;
                case 34: pps = nal; break;
                default: break;
            }
        }
        return true;
    }
    bool configuration() {
        if (!context->extradata_size) return true; // repeat headers may arrive with first IDR
        const std::span<const uint8_t> bytes(context->extradata, context->extradata_size);
        std::vector<Nal> nals;
        if (bytes[0] == 1) {
            if (bytes.size() < 23) return false;
            length_bytes = (bytes[21] & 3) + 1;
            std::size_t offset = 23;
            for (unsigned array = 0; array < bytes[22]; ++array) {
                if (offset + 3 > bytes.size()) return false;
                ++offset;
                const unsigned count = (unsigned(bytes[offset]) << 8) | bytes[offset+1];
                offset += 2;
                for (unsigned i = 0; i < count; ++i) {
                    if (offset + 2 > bytes.size()) return false;
                    const unsigned size = (unsigned(bytes[offset]) << 8) | bytes[offset+1];
                    offset += 2;
                    if (!size || size > bytes.size()-offset || nals.size() >= kMaxRecordingIdrNals) return false;
                    nals.emplace_back(bytes.begin()+offset,bytes.begin()+offset+size);
                    offset += size;
                }
            }
            if (offset != bytes.size()) return false;
        } else if (!annex_b(bytes,nals)) return false;
        return remember(nals);
    }
    bool receive(std::vector<Packet>& output, std::string& error) {
        for (;;) {
            const int result = avcodec_receive_packet(context, packet);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return true;
            if (result < 0) return fail(SCRCTL_TR("Cannot receive upright recording HEVC packet: ") + codec_error(result),error);
            if (packet->size <= 0 || static_cast<std::size_t>(packet->size) > kMaxRecordingMuxerPacketBytes ||
                packet->pts == AV_NOPTS_VALUE || packet->dts != packet->pts ||
                (last_output && packet->pts <= *last_output)) {
                av_packet_unref(packet);
                return fail(SCRCTL_TR("Upright recording encoder returned invalid or reordered timing"),error);
            }
            Packet encoded;
            encoded.pts_ticks = packet->pts;
            const std::span<const uint8_t> bytes(packet->data, packet->size);
            if (!annex_b(bytes,encoded.nals)) {
                encoded.nals.clear();
                for (std::size_t offset = 0; offset < bytes.size();) {
                    if (offset + length_bytes > bytes.size()) return fail(SCRCTL_TR("Upright recording encoder returned malformed HEVC"),error);
                    uint32_t size = 0;
                    for (int i = 0; i < length_bytes; ++i) size = (size << 8) | bytes[offset++];
                    if (size < 2 || size > bytes.size()-offset || encoded.nals.size() >= kMaxRecordingIdrNals)
                        return fail(SCRCTL_TR("Upright recording encoder returned malformed HEVC"),error);
                    encoded.nals.emplace_back(bytes.begin()+offset,bytes.begin()+offset+size);
                    offset += size;
                }
            }
            av_packet_unref(packet);
            if (encoded.nals.size() > kMaxRecordingIdrNals || !remember(encoded.nals) ||
                vps.empty() || sps.empty() || pps.empty())
                return fail(SCRCTL_TR("Upright recording encoder returned malformed HEVC"),error);
            if (!last_output) {
                const auto checked = inspect_recording_video_config(vps,sps,pps);
                if (!checked.permits_equal_dts_pts() || checked.width != static_cast<int>(options.width) ||
                    checked.height != static_cast<int>(options.height)) {
                    return fail(SCRCTL_TR("Upright recording encoder did not produce a fixed no-reorder canvas"),error);
                }
                const auto idr = inspect_recording_idr(encoded.nals,checked);
                if (!idr.valid()) return fail(idr.error,error);
            }
            last_output = encoded.pts_ticks;
            encoded.vps = vps; encoded.sps = sps; encoded.pps = pps;
            output.push_back(std::move(encoded));
        }
    }
#endif
    bool fail(std::string reason, std::string& error) {
        if (first_error.empty()) first_error = std::move(reason);
        error = first_error;
        return false;
    }
};

bool RecordingVideoEncoder::available(std::string& error) {
#ifdef SCRCTL_HAVE_LIBAV
    if (select_encoder()) { error.clear(); return true; }
#endif
    error = SCRCTL_TR(unavailable);
    return false;
}
std::unique_ptr<RecordingVideoEncoder> RecordingVideoEncoder::create(const Options& options, std::string& error) {
    error.clear();
    if (options.width < 2 || options.height < 2 || options.width % 2 || options.height % 2 ||
        options.width > 8192 || options.height > 8192 || uint64_t(options.width)*options.height > max_pixels ||
        options.clock_rate <= 0 || options.clock_rate > 1000000000 || options.bit_rate <= 0) {
        error = SCRCTL_TR("Invalid upright recording fixed canvas or clock"); return nullptr;
    }
    if (!available(error)) return nullptr;
#ifndef SCRCTL_HAVE_LIBAV
    return nullptr;
#else
    auto state = std::make_unique<Impl>();
    state->options = options;
    const auto* codec = select_encoder(options.prefer_hardware);
    state->backend = codec->name;
    state->context = avcodec_alloc_context3(codec);
    state->image = av_frame_alloc(); state->packet = av_packet_alloc();
    if (!state->context || !state->image || !state->packet) {
        error = SCRCTL_TR("Cannot allocate upright recording encoder"); return nullptr;
    }
    auto* context = state->context;
    context->width = static_cast<int>(options.width); context->height = static_cast<int>(options.height);
    context->pix_fmt = AV_PIX_FMT_YUV420P;
    context->time_base = AVRational{1,options.clock_rate};
    context->framerate = AVRational{30,1};
    context->bit_rate = options.bit_rate;
    context->max_b_frames = 0; context->gop_size = 120;
    context->thread_count = 0;
    context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    AVDictionary* settings = nullptr;
    if (state->backend == "libx265") {
        av_dict_set(&settings,"preset","ultrafast",0);
        av_dict_set(&settings,"tune","zerolatency",0);
        av_dict_set(&settings,"x265-params","bframes=0:repeat-headers=1:info=0:pools=4:frame-threads=1:log-level=error",0);
    } else {
        av_dict_set(&settings,"realtime","1",0);
        av_dict_set(&settings,"prio_speed","1",0);
    }
    const int opened = avcodec_open2(context,codec,&settings);
    av_dict_free(&settings);
    if (opened < 0) {
        if (options.prefer_hardware && state->backend == "hevc_videotoolbox" &&
            avcodec_find_encoder_by_name("libx265")) {
            auto software = options; software.prefer_hardware = false;
            state.reset();
            return create(software, error);
        }
        error = SCRCTL_TR("Cannot open upright recording encoder: ") + codec_error(opened); return nullptr;
    }
    if (!state->configuration()) { error = SCRCTL_TR("Upright recording encoder returned malformed HEVC"); return nullptr; }
    state->image->format = context->pix_fmt;
    state->image->width = context->width; state->image->height = context->height;
    const int allocated = av_frame_get_buffer(state->image,32);
    if (allocated < 0) { error = SCRCTL_TR("Cannot allocate upright recording canvas: ") + codec_error(allocated); return nullptr; }
    return std::unique_ptr<RecordingVideoEncoder>(new RecordingVideoEncoder(std::move(state)));
#endif
}
RecordingVideoEncoder::RecordingVideoEncoder(std::unique_ptr<Impl> state) : impl_(std::move(state)) {}
RecordingVideoEncoder::~RecordingVideoEncoder() = default;
const char* RecordingVideoEncoder::backend_name() const { return impl_->backend.c_str(); }

bool RecordingVideoEncoder::encode(const Frame& frame, int clockwise, Crop crop, int64_t ticks,
                                    std::vector<Packet>& output, std::string& error) {
    error.clear();
    if (!impl_->first_error.empty()) { error = impl_->first_error; return false; }
    if (impl_->ended) return impl_->fail(SCRCTL_TR("Upright recording encoder is already finished"),error);
    if (ticks < 0 || (impl_->last_input && ticks <= *impl_->last_input) ||
        (clockwise != 0 && clockwise != 90 && clockwise != 180 && clockwise != 270) ||
        frame.width == 0 || frame.height == 0 || frame.width > 8192 || frame.height > 8192 ||
        uint64_t(frame.width)*frame.height > max_pixels || frame.bytes_per_pixel != 4 ||
        uint64_t(frame.width)*4 > frame.row_pitch ||
        uint64_t(frame.row_pitch)*(frame.height-1)+uint64_t(frame.width)*4 > frame.pixels.size())
        return impl_->fail(SCRCTL_TR("Invalid upright recording frame, orientation or timestamp"),error);
    if (crop.width == 0 && crop.height == 0) { crop = {0,0,frame.width,frame.height}; }
    if (!crop.width || !crop.height || crop.x >= frame.width || crop.y >= frame.height ||
        crop.width > frame.width-crop.x || crop.height > frame.height-crop.y)
        return impl_->fail(SCRCTL_TR("Upright recording crop is outside the source frame"),error);
#ifndef SCRCTL_HAVE_LIBAV
    (void)output;
    return impl_->fail(SCRCTL_TR(unavailable),error);
#else
    const bool sideways = clockwise == 90 || clockwise == 270;
    const uint32_t width = sideways ? crop.height : crop.width;
    const uint32_t height = sideways ? crop.width : crop.height;
    auto& pixels = impl_->rotated_pixels;
    pixels.resize(uint64_t(width)*height*4);
    if (clockwise == 0) {
        for (uint32_t y = 0; y < height; ++y) {
            std::memcpy(pixels.data()+uint64_t(y)*width*4,
                frame.pixels.data()+uint64_t(crop.y+y)*frame.row_pitch+uint64_t(crop.x)*4,
                uint64_t(width)*4);
        }
    } else {
        // Tiled transpose keeps both source and destination cache lines hot;
        // a full-width scatter per source row is too slow for phone resolution.
        constexpr uint32_t tile = 16;
        for (uint32_t ty = 0; ty < crop.height; ty += tile) {
            for (uint32_t tx = 0; tx < crop.width; tx += tile) {
                for (uint32_t y = ty; y < std::min(ty+tile,crop.height); ++y) {
                    for (uint32_t x = tx; x < std::min(tx+tile,crop.width); ++x) {
                        uint32_t dx = x, dy = y;
                        if (clockwise == 90) { dx = crop.height-1-y; dy = x; }
                        else if (clockwise == 180) { dx = crop.width-1-x; dy = crop.height-1-y; }
                        else { dx = y; dy = crop.width-1-x; }
                        std::memcpy(pixels.data()+(uint64_t(dy)*width+dx)*4,
                            frame.pixels.data()+uint64_t(crop.y+y)*frame.row_pitch+uint64_t(crop.x+x)*4,4);
                    }
                }
            }
        }
    }
    const auto& options = impl_->options;
    uint32_t fit_width, fit_height;
    if (uint64_t(width)*options.height > uint64_t(height)*options.width) {
        fit_width = options.width;
        fit_height = static_cast<uint32_t>(uint64_t(height)*options.width/width) & ~1u;
    } else {
        fit_height = options.height;
        fit_width = static_cast<uint32_t>(uint64_t(width)*options.height/height) & ~1u;
    }
    if (fit_width < 2 || fit_height < 2) return impl_->fail(SCRCTL_TR("Upright recording content is too narrow for the canvas"),error);
    const uint32_t offset_x = ((options.width-fit_width)/2) & ~1u;
    const uint32_t offset_y = ((options.height-fit_height)/2) & ~1u;
    const int writable = av_frame_make_writable(impl_->image);
    if (writable < 0) return impl_->fail(SCRCTL_TR("Cannot write upright recording canvas: ") + codec_error(writable),error);
    for (int plane = 0; plane < 3; ++plane) {
        const uint32_t rows = plane == 0 ? options.height : options.height/2;
        const uint32_t columns = plane == 0 ? options.width : options.width/2;
        for (uint32_t y = 0; y < rows; ++y) std::memset(impl_->image->data[plane]+y*impl_->image->linesize[plane],
                                                        plane == 0 ? 16 : 128,columns);
    }
    impl_->scale = sws_getCachedContext(impl_->scale,width,height,AV_PIX_FMT_BGRA,
        fit_width,fit_height,AV_PIX_FMT_YUV420P,SWS_BILINEAR,nullptr,nullptr,nullptr);
    if (!impl_->scale) return impl_->fail(SCRCTL_TR("Cannot convert upright recording pixels"),error);
    const uint8_t* source[4]{pixels.data(),nullptr,nullptr,nullptr};
    const int stride[4]{static_cast<int>(width*4),0,0,0};
    uint8_t* destination[4]{impl_->image->data[0]+offset_y*impl_->image->linesize[0]+offset_x,
        impl_->image->data[1]+(offset_y/2)*impl_->image->linesize[1]+offset_x/2,
        impl_->image->data[2]+(offset_y/2)*impl_->image->linesize[2]+offset_x/2,nullptr};
    if (sws_scale(impl_->scale,source,stride,0,height,destination,impl_->image->linesize) != static_cast<int>(fit_height))
        return impl_->fail(SCRCTL_TR("Cannot convert upright recording pixels"),error);
    impl_->image->pts = ticks;
    const int sent = avcodec_send_frame(impl_->context,impl_->image);
    if (sent < 0) return impl_->fail(SCRCTL_TR("Cannot send upright recording frame: ") + codec_error(sent),error);
    impl_->last_input = ticks;
    return impl_->receive(output,error);
#endif
}
bool RecordingVideoEncoder::finish(std::vector<Packet>& output, std::string& error) {
    error.clear();
    if (!impl_->first_error.empty()) { error = impl_->first_error; return false; }
    if (impl_->ended) return true;
    impl_->ended = true;
#ifdef SCRCTL_HAVE_LIBAV
    const int sent = avcodec_send_frame(impl_->context,nullptr);
    if (sent < 0 && sent != AVERROR_EOF) return impl_->fail(SCRCTL_TR("Cannot finish upright recording encoder: ") + codec_error(sent),error);
    return impl_->receive(output,error);
#else
    (void)output;
    return impl_->fail(SCRCTL_TR(unavailable),error);
#endif
}
} // namespace scrctl::media
