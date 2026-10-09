#include "i18n/Translation.h"
#include "media/RecordingVideoConfig.h"
#include "media/RecordingMuxer.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
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

using Parameters = std::array<std::span<const uint8_t>, 3>;

std::optional<RecordingVideoConfig> validate_parameters(const Parameters& parameters,
                                                       std::size_t& parameter_bytes) {
    const auto& vps = parameters[0];
    const auto& sps = parameters[1];
    parameter_bytes = 0;
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

    return std::nullopt;
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

struct PacketDeleter {
    void operator()(AVPacket* packet) const { av_packet_free(&packet); }
};

using SyntaxFilter = std::unique_ptr<AVBSFContext, FilterDeleter>;

std::string codec_error(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, text, sizeof(text));
    return text;
}

void write_parameters(uint8_t* output, const Parameters& parameters) {
    static constexpr std::array<uint8_t, 4> start_code{0, 0, 0, 1};
    for (const auto nal : parameters) {
        std::memcpy(output, start_code.data(), start_code.size());
        output += start_code.size();
        std::memcpy(output, nal.data(), nal.size());
        output += nal.size();
    }
}

SyntaxFilter make_syntax_filter(const Parameters& parameters, std::size_t parameter_bytes,
                                Status& status, std::string& error) {
    const AVBitStreamFilter* filter = av_bsf_get_by_name("hevc_metadata");
    if (filter == nullptr) {
        status = Status::Unsupported;
        error = SCRCTL_TR("libavcodec has no HEVC parameter syntax filter");
        return nullptr;
    }
    AVBSFContext* allocated_filter = nullptr;
    const int allocated = av_bsf_alloc(filter, &allocated_filter);
    SyntaxFilter syntax(allocated_filter);
    if (allocated < 0 || syntax == nullptr) {
        status = Status::Unsupported;
        error = SCRCTL_TR("Cannot allocate the HEVC parameter syntax filter");
        return nullptr;
    }
    syntax->par_in->codec_type = AVMEDIA_TYPE_VIDEO;
    syntax->par_in->codec_id = AV_CODEC_ID_HEVC;
    const std::size_t bytes = parameter_bytes + parameters.size() * 4;
    syntax->par_in->extradata = static_cast<uint8_t*>(av_mallocz(bytes + AV_INPUT_BUFFER_PADDING_SIZE));
    if (syntax->par_in->extradata == nullptr) {
        status = Status::Unsupported;
        error = SCRCTL_TR("Cannot allocate HEVC configuration extradata");
        return nullptr;
    }
    syntax->par_in->extradata_size = static_cast<int>(bytes);
    write_parameters(syntax->par_in->extradata, parameters);
    const int checked = av_bsf_init(syntax.get());
    if (checked < 0) {
        status = checked == AVERROR(ENOMEM) ? Status::Unsupported : Status::Invalid;
        error = SCRCTL_TR("Invalid HEVC recording parameter syntax: ") + codec_error(checked);
        return nullptr;
    }
    return syntax;
}
#endif
}  // namespace

RecordingVideoConfig inspect_recording_video_config(const Nal& vps, const Nal& sps,
                                                      const Nal& pps) {
    const Parameters parameters{vps, sps, pps};
    std::size_t parameter_bytes = 0;
    if (const auto invalid = validate_parameters(parameters, parameter_bytes)) return *invalid;

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
    write_parameters(context->extradata, parameters);
    // 软件 decoder 在部分坏 PPS 路径会记录错误却返回成功，不能单凭 open
    // 证明三条参数语法完整。先经公开 hevc_metadata BSF 检查 extradata，
    // 不设置修改选项、不提交图像、不访问 CBS 私有对象，也不解析日志。
    Status syntax_status = Status::Invalid;
    std::string syntax_error;
    auto syntax = make_syntax_filter(parameters, parameter_bytes, syntax_status, syntax_error);
    if (syntax == nullptr) return failure(syntax_status, std::move(syntax_error));
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

bool recording_idr_checks_available(std::string& error) {
#ifndef SCRCTL_HAVE_LIBAV
    error = SCRCTL_TR("HEVC recording configuration checks require libavcodec");
    return false;
#else
    if (avcodec_find_decoder_by_name("hevc") == nullptr) {
        error = SCRCTL_TR("libavcodec has no software HEVC decoder");
        return false;
    }
    if (av_bsf_get_by_name("hevc_metadata") == nullptr) {
        error = SCRCTL_TR("libavcodec has no HEVC parameter syntax filter");
        return false;
    }
    error.clear();
    return true;
#endif
}

RecordingIdrSyntax inspect_recording_idr(const std::vector<Nal>& nals,
                                         const RecordingVideoConfig& config) {
    using IdStatus = RecordingIdrSyntax::Status;
    const auto rejected = [](IdStatus status, std::string error) {
        return RecordingIdrSyntax{status, std::move(error)};
    };
    if (config.status == Status::Unsupported) {
        return rejected(IdStatus::Unsupported, config.error.empty()
            ? SCRCTL_TR("HEVC IDR checks require a supported current configuration") : config.error);
    }
    if ((config.status != Status::NoReorder && config.status != Status::Reorder) ||
        config.width <= 0 || config.height <= 0 || config.reorder_depth < 0) {
        return rejected(IdStatus::Invalid, SCRCTL_TR("HEVC IDR checks require a valid current configuration"));
    }
    const Parameters parameters{config.vps, config.sps, config.pps};
    std::size_t parameter_bytes = 0;
    if (const auto invalid = validate_parameters(parameters, parameter_bytes)) {
        return rejected(invalid->status == Status::Unsupported ? IdStatus::Unsupported : IdStatus::Invalid,
                        invalid->error);
    }
    if (nals.empty() || nals.size() > kMaxRecordingIdrNals) {
        return rejected(IdStatus::Invalid, SCRCTL_TR("HEVC IDR access unit is empty or exceeds the NAL-count budget"));
    }
    std::size_t packet_bytes = 0;
    uint8_t picture_type = 0;
    bool have_slice = false;
    const std::array<const Nal*, 3> parameter_nals{&config.vps, &config.sps, &config.pps};
    for (const auto& nal : nals) {
        if (nal.size() < 3 || nal.size() > kMaxRecordingMuxerPacketBytes - 4 ||
            packet_bytes > kMaxRecordingMuxerPacketBytes - 4 - nal.size()) {
            return rejected(IdStatus::Invalid, SCRCTL_TR("Recording HEVC AU is incomplete or exceeds the packet budget"));
        }
        packet_bytes += nal.size() + 4;
        const uint8_t type = (nal[0] >> 1) & 63;
        if (!valid_header(nal, type) || !single_nal(nal)) {
            return rejected(IdStatus::Invalid, SCRCTL_TR("HEVC IDR access unit has an invalid raw NAL header"));
        }
        if ((nal[0] & 1) != 0 || (nal[1] >> 3) != 0 || (nal[1] & 7) != 1) {
            return rejected(IdStatus::Unsupported, SCRCTL_TR("Recording supports only HEVC base layer 0 and temporal ID 0"));
        }
        if (have_slice && ((type >= 32 && type <= 35) || type == 39)) {
            return rejected(IdStatus::Invalid, SCRCTL_TR("HEVC IDR access unit has a next-picture prefix after its slices"));
        }
        if (type >= 32 && type <= 34 && nal != *parameter_nals[type - 32]) {
            return rejected(IdStatus::Invalid, SCRCTL_TR("HEVC IDR parameter sets do not match the current configuration"));
        }
        if (type > 31) continue;
        if ((type != 19 && type != 20) || (have_slice && picture_type != type)) {
            return rejected(IdStatus::Invalid, SCRCTL_TR("HEVC IDR access unit contains a non-IDR or mixed VCL type"));
        }
        // 与 AnnexBParser 使用相同的固定 first-slice bit；其余语法由公开 BSF 检查。
        const bool first_slice = (nal[2] & 0x80) != 0;
        if (first_slice == have_slice) {
            return rejected(IdStatus::Invalid, SCRCTL_TR("HEVC IDR access unit is missing its first slice or contains a second picture"));
        }
        picture_type = type;
        have_slice = true;
    }
    if (!have_slice) {
        return rejected(IdStatus::Invalid, SCRCTL_TR("HEVC IDR access unit has no IDR slice"));
    }
#ifndef SCRCTL_HAVE_LIBAV
    return rejected(IdStatus::Unsupported, SCRCTL_TR("HEVC recording configuration checks require libavcodec"));
#else
    Status syntax_status = Status::Invalid;
    std::string syntax_error;
    auto syntax = make_syntax_filter(parameters, parameter_bytes, syntax_status, syntax_error);
    if (syntax == nullptr) {
        return rejected(syntax_status == Status::Unsupported ? IdStatus::Unsupported : IdStatus::Invalid,
                        std::move(syntax_error));
    }
    static_assert(kMaxRecordingMuxerPacketBytes <= static_cast<std::size_t>(std::numeric_limits<int>::max()));
    std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc());
    if (packet == nullptr || av_new_packet(packet.get(), static_cast<int>(packet_bytes)) < 0) {
        return rejected(IdStatus::Unsupported, SCRCTL_TR("Cannot allocate the bounded HEVC IDR syntax packet"));
    }
    auto* output = packet->data;
    for (const auto& nal : nals) {
        static constexpr std::array<uint8_t, 4> start_code{0, 0, 0, 1};
        std::memcpy(output, start_code.data(), start_code.size());
        output += start_code.size();
        std::memcpy(output, nal.data(), nal.size());
        output += nal.size();
    }
    int checked = av_bsf_send_packet(syntax.get(), packet.get());
    if (checked >= 0) checked = av_bsf_receive_packet(syntax.get(), packet.get());
    if (checked < 0) {
        return rejected(checked == AVERROR(ENOMEM) ? IdStatus::Unsupported : IdStatus::Invalid,
                        SCRCTL_TR("Invalid HEVC IDR slice syntax: ") + codec_error(checked));
    }
    // 输出可能重写EPB或参数；只检查返回状态，不把此packet交给录制器。
    return RecordingIdrSyntax{IdStatus::Valid, {}};
#endif
}

}  // namespace scrctl::media
