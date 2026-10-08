#pragma once

#include "media/AudioPump.h"
#include "media/FramePump.h"
#include "media/ScreenshotSource.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace scrctl::app {

/// 根据实时源的值快照打印统计，独立维护各类速率的时间和计数基线。
/// 不持有设备、媒体泵或声卡，采集快照和媒体生命周期由 LiveSource 管理。
class LiveStats {
  public:
    struct Tcp {
        uint64_t now_ms = 0;
        uint64_t recv_bytes = 0;
        bool net_debug = false;
        uint64_t bad_checksums = 0;
        uint64_t icmp_seen = 0;
        uint64_t echo_replies = 0;
    };
    struct Screenshot {
        media::ScreenshotSource::Stats counters;
        uint64_t now_ms = 0;
    };
    struct Video {
        media::FramePump::Stats counters;
        uint64_t now_ms = 0;
    };
    struct Audio {
        /// 结算时刻沿用本次视频快照的 now_ms，音频窗口仍独立维护。
        media::AudioPump::Stats counters;
        /// 同一次采样用于速率和下次基线，避免漏掉两次读取之间的交付。
        uint64_t delivered = 0;
        uint64_t silence = 0;
        uint64_t preroll_silence = 0;
        uint64_t underrun_silence = 0;
        uint64_t underrun_callbacks = 0;
        std::size_t buffered_frames = 0;
        bool output_open = false;
        std::string output_driver;
    };
    struct Snapshot {
        std::optional<Tcp> tcp;
        std::optional<Screenshot> screenshot;
        std::optional<Video> video;
        std::optional<Audio> audio;
    };

    /// 媒体泵建立后设置各自的首个统计窗口，与获取首帧和声卡打开分别计时。
    void video_started(uint64_t now_ms);
    void audio_started(uint64_t now_ms);
    /// 每次安装新截图源时重置时间与帧基线，不结算后台视频和音频。
    void reset_screenshot(uint64_t now_ms);
    /// 先打印隧道；有截图时只打印截图，否则打印视频及其音频。
    void print(const Snapshot &snapshot);

  private:
    uint64_t last_packets_ = 0;
    uint64_t last_dev_packets_ = 0;
    uint64_t last_dev_change_ms_ = 0;
    double last_dev_rate_ = 0;
    /// 上次设备速率计算使用的 SR 更新间隔，输出时与本地采样窗口分别标明。
    uint64_t last_dev_span_ms_ = 0;
    uint64_t last_aus_ = 0;
    uint64_t last_decoded_ = 0;
    uint64_t last_audio_packets_ = 0;
    uint64_t last_audio_decoded_ = 0;
    uint64_t last_audio_delivered_ = 0;
    /// 视频和音频分别维护计数与时间基线，截图另有独立状态。切换画面源时，
    /// 后台媒体和音频仍累计数据，不能用截图的打印时钟计算它们的增量。
    /// 各源建立时初始化时钟，首次统计使用真实经过的时间。
    uint64_t last_stream_ms_ = 0;
    uint64_t last_audio_ms_ = 0;
    /// 隧道 TCP 的统计时钟，不随画面来源切换。
    uint64_t last_tcp_ms_ = 0;
    uint64_t last_tcp_recv_ = 0;
    uint64_t last_screenshot_ms_ = 0;
    uint64_t last_screenshot_frames_ = 0;
};

} // namespace scrctl::app
