#pragma once

#include "bitstream/AnnexB.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace scrctl::media {

/// 单个完整编码包的输入预算；调用方仍需为跨线程队列设独立字节/年龄限额。
inline constexpr std::size_t kMaxRecordingMuxerPacketBytes = 16u * 1024u * 1024u;

/// 只写入调用方给出的本地文件，不生成临时路径、重命名或删除旧文件。
/// 调用方须串行使用，负责跨线程队列、文件提交和完整 AU 的组帧。
class RecordingMuxer {
public:
    /// 普通 MP4 使用 hvc1，视频时长必须已知且为正，以免编辑列表隐藏尾帧。
    /// Matroska 首版只接受非负时间。调用方可选在两轨之前的共同原点，
    /// 保持相对偏移；本层不会自行平移时间或更改 AAC priming。
    enum class Format { Mp4, Matroska };

    struct Audio {
        int sample_rate = 48000;
        int channels = 2;
        int frame_samples = 480;
    };

    struct Options {
        std::string path;
        Format format = Format::Mp4;
        /// 相对编码像素的静态顺时针展示角度：0/90/180/270，不旋转或重编码图像。
        int video_orientation = 0;
        bool include_video = true;
        /// 原始参数 NAL，不含起始码。所选视频只接受已确认无重排的配置。
        /// 不选视频时参数必须空且方向必须为 0；至少选择视频或 audio 一轨。
        Nal vps, sps, pps;
        std::optional<Audio> audio;
    };

    struct Timing {
        /// 两轨共用调用方已确定的原点，单位为微秒；音频可以早于视频零点。
        int64_t pts_us = 0;
        int64_t dts_us = 0;
        /// Matroska 视频 0 表示未知；不能用固定帧率或上一帧间隔代替。
        /// MP4 视频和所有音频必须为正值。Matroska 解封装器可能推断
        /// 末帧时长，不把该值当作源端测量。
        int64_t duration_us = 0;
    };

    /// 只表示构建中有封装库；open 仍可能因格式、codec 或 I/O 不可用而失败。
    [[nodiscard]] static bool available() noexcept;
    /// 纯预检，不创建文件；旧版封装库不支持的非零 MKV 方向会明确拒绝。
    /// 0 不需要旋转元数据；本方法成功不代表 codec 或 I/O 可用。
    [[nodiscard]] static bool validate_video_orientation(
        Format format, int degrees, std::string& error);
    [[nodiscard]] static std::unique_ptr<RecordingMuxer> open(
        const Options& options, std::string& error);

    ~RecordingMuxer();
    RecordingMuxer(const RecordingMuxer&) = delete;
    RecordingMuxer& operator=(const RecordingMuxer&) = delete;

    /// 完整 Annex-B AU；当前只支持 DTS=PTS。调用方必须保证参数和会话 epoch
    /// 与 open 时相同；本层不解析图像引用、不重新组帧，也不替调用方建立时钟。
    [[nodiscard]] bool write_video(std::span<const uint8_t> access_unit, Timing timing,
                                   bool keyframe, std::string& error);
    /// 原始 AAC-ELD access unit，不含 RTP、ADTS 或 PCM；不改变采样数和 priming。
    [[nodiscard]] bool write_audio(std::span<const uint8_t> access_unit, Timing timing,
                                   std::string& error);

    /// 显式收尾并关闭文件，重复调用返回同一结果。写入失败后仍尝试 trailer、
    /// flush 和 close，保留最早错误；关闭之后的误写不会改变已经完成的结果。
    /// 析构只作兜底，调用方应检查本方法结果。
    [[nodiscard]] bool finish(std::string& error);
    [[nodiscard]] const std::string& error() const noexcept;

private:
    struct Impl;
    explicit RecordingMuxer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace scrctl::media
