#pragma once

#include "bitstream/AnnexB.h"

#include <cstddef>
#include <string>

namespace scrctl::media {

/// 首版检查只接受基础 layer 0、单 temporal layer、各一条原始 VPS/SPS/PPS。
/// 预算为三条参数的合计字节数，不含随后添加的 Annex-B 起始码。
inline constexpr std::size_t kMaxRecordingVideoParameterBytes = 1024 * 1024;

struct RecordingVideoConfig {
    enum class Status { NoReorder, Reorder, Invalid, Unsupported };
    Status status = Status::Invalid;
    int width = 0;
    int height = 0;
    /// 未能确认配置时保持 -1，不能把未初始化解码器的默认 0 当成证明。
    int reorder_depth = -1;
    /// 仅有效配置保留参数；包含原始 EPB，不用解析后的 RBSP 替换编码内容。
    Nal vps, sps, pps;
    std::string error;

    [[nodiscard]] bool permits_equal_dts_pts() const noexcept {
        return status == Status::NoReorder;
    }
};

/// 公开 hevc_metadata BSF 先检查参数语法；fresh libavcodec 软件 HEVC context
/// 仅通过同一原始 extradata 导出尺寸、像素格式和重排序深度。不提交图像，
/// 不读取 parser、显示解码器或 CBS 私有状态。无 libav、软件 HEVC 或该 BSF
/// 支持时明确返回 Unsupported。
///
/// 结论只属于返回的这一套参数和调用者当前 epoch。参数原文或 epoch 改变时
/// 必须重新检查，不能复用旧结果。NoReorder 只确认配置声明的重排序限额，
/// 不验证未来 AU 的引用或完整性；录制仍须拒绝倒退 PTS，并保持解码顺序。
[[nodiscard]] RecordingVideoConfig inspect_recording_video_config(
    const Nal& vps, const Nal& sps, const Nal& pps);

}  // namespace scrctl::media
