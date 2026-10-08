#include "i18n/Translation.h"
#include "media/RecordingVideoConfig.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <utility>

#ifdef SCRCTL_HAVE_LIBAV
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}
#endif

namespace scrctl::media {
namespace {
using Status = RecordingVideoConfig::Status;

RecordingVideoConfig failure(Status status, std::string error) {
    RecordingVideoConfig result;
    result.status = status;
    result.error = std::move(error);
    return result;
}

bool valid_header(std::span<const uint8_t> nal, uint8_t type) {
    return nal.size() >= 3 && (nal[0] & 0x80) == 0 && ((nal[0] >> 1) & 0x3f) == type &&
           (nal[1] & 7) != 0;
}

bool single_nal(std::span<const uint8_t> nal) {
    // 原始参数中的 00 00 01 必须经 EPB 转义。拒绝分隔符，防止调用者把
    // 多个 NAL 拼成一条输入；extradata 的 first SPS 结论不能代表多套配置。
    for (std::size_t i = 2; i < nal.size(); ++i) {
        if (nal[i - 2] == 0 && nal[i - 1] == 0 && nal[i] == 1) {
            return false;
        }
    }
    return true;
}

#ifdef SCRCTL_HAVE_LIBAV
static_assert(kMaxRecordingVideoParameterBytes + 12 <=
              static_cast<std::size_t>(std::numeric_limits<int>::max()) - AV_INPUT_BUFFER_PADDING_SIZE);

struct ContextDeleter {
    void operator()(AVCodecContext* context) const { avcodec_free_context(&context); }
};

struct FilterDeleter {
    void operator()(AVBSFContext* context) const { av_bsf_free(&context); }
};

std::string codec_error(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, text, sizeof(text));
    return text;
}
#endif
}  // namespace

RecordingVideoConfig inspect_recording_video_config(const Nal& vps, const Nal& sps,
                                                      const Nal& pps) {
    const std::array<std::span<const uint8_t>, 3> parameters{vps, sps, pps};
    std::size_t parameter_bytes = 0;
    for (std::size_t i = 0; i < parameters.size(); ++i) {
        const auto nal = parameters[i];
        if (nal.size() > kMaxRecordingVideoParameterBytes - parameter_bytes) {
            return failure(Status::Invalid, SCRCTL_TR("HEVC parameter sets exceed the 1 MiB recording budget"));
        }
        parameter_bytes += nal.size();
        if (!valid_header(nal, static_cast<uint8_t>(32 + i)) || !single_nal(nal)) {
            return failure(Status::Invalid,
                           SCRCTL_TR("Recording requires one complete raw VPS, SPS and PPS with valid NAL headers"));
        }
        const int layer = ((nal[0] & 1) << 5) | (nal[1] >> 3);
        if (layer != 0 || (nal[1] & 7) != 1) {
            return failure(Status::Unsupported,
                           SCRCTL_TR("Recording supports only HEVC base layer 0 and temporal ID 0"));
        }
    }
    if (vps.size() < 4) {
        return failure(Status::Invalid, SCRCTL_TR("HEVC VPS is truncated before its layer configuration"));
    }
    // 这几个固定字段在 RBSP 开头，不涉及 profile_tier_level 或 Exp-Golomb。
    // VPS: 4-bit ID、两条 base-layer 标志、6-bit max_layers、3-bit max_sub_layers。
    // SPS: 4-bit VPS ID、3-bit max_sub_layers、1-bit temporal nesting。
    const int vps_layers = ((vps[2] & 3) << 4) | (vps[3] >> 4);
    const int vps_sub_layers = (vps[3] >> 1) & 7;
    const int sps_sub_layers = (sps[2] >> 1) & 7;
    if (vps_layers != 0 || vps_sub_layers != 0 || sps_sub_layers != 0) {
        return failure(Status::Unsupported,
                       SCRCTL_TR("Recording requires single-layer HEVC VPS/SPS temporal configurations"));
    }

#ifndef SCRCTL_HAVE_LIBAV
    return failure(Status::Unsupported, SCRCTL_TR("HEVC recording configuration checks require libavcodec"));
#else
    const AVCodec* codec = avcodec_find_decoder_by_name("hevc");
    if (codec == nullptr) {
        return failure(Status::Unsupported, SCRCTL_TR("libavcodec has no software HEVC decoder"));
    }
    std::unique_ptr<AVCodecContext, ContextDeleter> context(avcodec_alloc_context3(codec));
    if (context == nullptr) {
        return failure(Status::Unsupported, SCRCTL_TR("Cannot allocate the HEVC configuration context"));
    }
    context->thread_count = 1;
    context->err_recognition = AV_EF_EXPLODE;
    // 三条起始码和 padding 在固定预算之外；总大小远小于 libav 的 int 上限。
    const std::size_t extradata_bytes = parameter_bytes + parameters.size() * 4;
    context->extradata = static_cast<uint8_t*>(
        av_mallocz(extradata_bytes + AV_INPUT_BUFFER_PADDING_SIZE));
    if (context->extradata == nullptr) {
        return failure(Status::Unsupported, SCRCTL_TR("Cannot allocate HEVC configuration extradata"));
    }
    context->extradata_size = static_cast<int>(extradata_bytes);
    auto* output = context->extradata;
    static constexpr std::array<uint8_t, 4> start_code{0, 0, 0, 1};
    for (const auto nal : parameters) {
        std::memcpy(output, start_code.data(), start_code.size());
        output += start_code.size();
        std::memcpy(output, nal.data(), nal.size());
        output += nal.size();
    }
    // 软件 decoder 在部分坏 PPS 路径会记录错误却返回成功，不能单凭 open
    // 证明三条参数语法完整。先经公开 hevc_metadata BSF 检查 extradata，
    // 不设置修改选项、不提交图像、不访问 CBS 私有对象，也不解析日志。
    const AVBitStreamFilter* filter = av_bsf_get_by_name("hevc_metadata");
    if (filter == nullptr) {
        return failure(Status::Unsupported, SCRCTL_TR("libavcodec has no HEVC parameter syntax filter"));
    }
    AVBSFContext* allocated_filter = nullptr;
    const int allocated = av_bsf_alloc(filter, &allocated_filter);
    std::unique_ptr<AVBSFContext, FilterDeleter> syntax(allocated_filter);
    if (allocated < 0 || syntax == nullptr) {
        return failure(Status::Unsupported, SCRCTL_TR("Cannot allocate the HEVC parameter syntax filter"));
    }
    const int copied = avcodec_parameters_from_context(syntax->par_in, context.get());
    if (copied < 0) {
        return failure(Status::Unsupported, SCRCTL_TR("Cannot copy HEVC parameter extradata: ") + codec_error(copied));
    }
    const int checked = av_bsf_init(syntax.get());
    if (checked < 0) {
        return failure(Status::Invalid, SCRCTL_TR("Invalid HEVC recording parameter syntax: ") + codec_error(checked));
    }
    // BSF 的输出可能重新序列化参数；只使用检查状态，仍保留调用者的原始字节。
    syntax.reset();
    const int opened = avcodec_open2(context.get(), codec, nullptr);
    if (opened < 0) {
        return failure(Status::Invalid, SCRCTL_TR("Invalid HEVC recording parameter sets: ") + codec_error(opened));
    }
    // 空或未解析配置也可能 open 成功且 has_b_frames 默认 0。必须先确认此 fresh
    // context 实际导出了尺寸和格式，不设置先验尺寸或 LOW_DELAY 去影响探测。
    if (context->width <= 0 || context->height <= 0 || context->pix_fmt == AV_PIX_FMT_NONE ||
        context->has_b_frames < 0) {
        return failure(Status::Invalid, SCRCTL_TR("HEVC decoder did not export a usable recording configuration"));
    }
    RecordingVideoConfig result;
    result.status = context->has_b_frames == 0 ? Status::NoReorder : Status::Reorder;
    result.width = context->width;
    result.height = context->height;
    result.reorder_depth = context->has_b_frames;
    result.vps = vps;
    result.sps = sps;
    result.pps = pps;
    return result;
#endif
}

}  // namespace scrctl::media
