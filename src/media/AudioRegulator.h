#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace scrctl::media {

/// 解码后、入播放缓冲前的 PCM 时钟补偿。解码器保持原有的每包帧数契约，
/// 此层通过 libswresample 轻微改变输出数量，使两个独立的采样时钟长期同步。
/// 实例由生产线程串行访问；消费线程只通过 AudioPump 的缓冲快照传递信息。
class AudioRegulator {
public:
    struct Observation {
        /// 入环后的瞬时水位，单位为音频帧（每声道一个采样点）。
        std::size_t buffered_frames = 0;
        bool playing = false;
        /// 自上次观察后，由消费端补的静音及缓冲明确丢弃的 PCM。
        /// 两者改变播放时间线，不能当成自然收包抖动一起平滑。
        std::uint64_t inserted_silence = 0;
        std::uint64_t discarded_frames = 0;
    };

    struct Stats {
        bool active = false;
        double average_frames = 0;
        /// 正数表示延展 PCM、负数表示压缩 PCM；不是硬件采样率。
        int compensation_ppm = 0;
        std::uint64_t compensation_updates = 0;
        /// 转换输出相对输入的数量变化，包含初始滤波延迟；不是硬裁丢弃计数。
        std::uint64_t added_frames = 0;
        std::uint64_t removed_frames = 0;
    };

    ~AudioRegulator();
    AudioRegulator(const AudioRegulator &) = delete;
    AudioRegulator &operator=(const AudioRegulator &) = delete;

    /// 无 libav 的构建、或 target_frames=0 时返回 nullptr 且 err 为空，调用方
    /// 可以保持直接 PCM 播放。参数或库初始化失败也返回 nullptr，但提供原因。
    static std::unique_ptr<AudioRegulator> create(int sample_rate, int channels,
                                                 std::size_t target_frames,
                                                 std::string &err);

    /// 将交织 s16 PCM 转换为同格式。成功时替换 output；失败不修改 output，
    /// 调用方可以播放原输入，下一包重新初始化状态。空输入不冲洗滤波器、不产生尾音。
    /// 每次输入最多一秒；输入、输出不得使用同一个 vector 的存储。
    bool process(std::span<const int16_t> input, std::vector<int16_t> &output,
                 std::string &err);

    /// 在 process 的输出入环后观察实际水位。自然波动平滑约 1.3 秒，约每秒
    /// 将偏差摊到四秒重采样，并限制速率与小偏差死区。目标水位始终不变。
    /// 返回 false 表示库拒绝补偿；状态已清空，下一包会尝试重新初始化。
    bool observe(const Observation &observation, std::string &err);

    /// 媒体会话重建时清除滤波器和估计，不把旧会话的补偿带入新会话。
    /// 下次 process 会初始化；累计统计保留。
    void reset();

    [[nodiscard]] Stats stats() const;

private:
    struct Impl;
    explicit AudioRegulator(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace scrctl::media
