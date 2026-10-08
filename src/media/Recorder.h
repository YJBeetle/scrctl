#pragma once

#include "bitstream/AnnexB.h"
#include "media/RecordingMuxer.h"
#include "rt/Rtcp.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace scrctl::media {

/// 单一会话的 MP4/MKV 录制消费者；拥有一个 worker，串行使用时钟、参数检查和封装。
/// 泵只转交已经组好且完成来源/传输完整性检查的编码包，不等待磁盘或队列空间。
class Recorder {
public:
    enum class Track { Video, Audio };
    struct Options {
        std::string path;
        RecordingMuxer::Format format = RecordingMuxer::Format::Matroska;
        bool include_audio = false;
        /// ingress、待 SR 包及写入中的原编码共用的预算，保守预留编码复制
        /// 和参数检查的临时空间；不是进程 RSS 或库内部缓存的硬上限。
        /// 可调低以缩短故障等待或减少缓存；首版不能超过这些默认上限。
        std::size_t encoded_budget = 16u * 1024u * 1024u;
        std::chrono::milliseconds clock_wait{5000};
        std::chrono::microseconds final_extrapolation{1500000};
    };

    /// 创建 worker；文件在首 IDR 配置和所选轨道的首包时钟批准后才打开。
    /// 首版支持无重排 HEVC；音频仅 48 kHz/双声道/480-sample AAC-ELD。
    /// MP4 用下一批准视频点的 PTS 差作为前包时长，最后包显示 100 ms。
    /// 这个末帧时长是展示规则，不是源端结束时间；MKV 保留未知时长 0。
    [[nodiscard]] static std::unique_ptr<Recorder> start(const Options&, std::string& error);
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    /// 16 字节会话 UUID 原文，每轨只 begin 一次。source 的缺失与显式 0 不同；
    /// 缺失时只由首媒体绑定，SR 不能绑定来源。真实重建/epoch变化必须 fail。
    [[nodiscard]] bool begin_track(Track, std::span<const uint8_t> session,
                                    std::optional<uint32_t> source);
    /// 参数是本 AU 所属 epoch 的原始 VPS/SPS/PPS，不含起始码。完整 IDR
    /// （19/20）之前不录其它 AU，仅保留媒体参考；起录后参数原文不变，
    /// 展开的 ticks 严格递增。
    [[nodiscard]] bool video(std::span<const uint8_t> session, uint32_t source, int64_t ticks,
                              const std::vector<Nal>& nals,
                              const Nal& vps, const Nal& sps, const Nal& pps);
    [[nodiscard]] bool audio(std::span<const uint8_t> session, uint32_t source,
                              uint32_t timestamp, std::span<const uint8_t> payload);
    [[nodiscard]] bool sender_report(Track, std::span<const uint8_t> session,
                                      const rt::SenderReport&);

    /// 不入编码队列，立即锁存首错并封闭入口；已开始的写入尽力完成/收尾。
    /// foreign 数据应由泵过滤；确认缺片、重建、参数/时钟异常则结束整个录制。
    void fail(std::string_view reason);
    /// 入口 bool 仅表示完成有界复制和 admission，不表示已写入；异步错误在此读取。
    [[nodiscard]] std::string error() const;

    /// 唯一 owner 串行调用：先 join 两个生产者，再 seal/join 本 worker、做 Final 映射并关闭。
    /// 正常情况下每个所选轨道都须有媒体和至少两条可信 SR；重复调用同一结果。
    /// 调用后误传数据返回 false，不修改已完成的结果。析构仅作兜底。
    [[nodiscard]] bool finish(std::string& error);

private:
    struct Impl;
    explicit Recorder(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};

}  // namespace scrctl::media
