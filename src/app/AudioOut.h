#pragma once

#include "media/AudioPump.h"
#include <SDL.h>
#include <atomic>
#include <string>

namespace scrctl::app {

/// SDL 音频输出适配层，按声卡回调节奏从 AudioPump 获取 PCM。
/// 会话建立、解码、预缓冲和 RR 由媒体层负责；播放到声卡属于应用层职责。
/// 无窗口的自动化客户端可单独使用 AudioPump。
class AudioOut {
  public:
    AudioOut() = default;
    AudioOut(const AudioOut &) = delete;
    AudioOut &operator=(const AudioOut &) = delete;
    ~AudioOut() { close(); }

    /// 打开默认音频设备。预缓冲阈值由 AudioPump::preroll_frames() 提供，
    /// 避免两处重复换算缓冲时长。采样率、声道数和格式必须与 PCM 一致，
    /// 本层不做重采样。首次等待预缓冲可减少起始欠载，代价是相应的启动延迟。
    /// 仅录音轨的 AudioPump 在打开声卡前拒绝播放，不改变它的接收或录制状态。
    bool open(scrctl::media::AudioPump &pump, std::string &err);

    /// 关闭音频设备。必须先于 SDL_Quit，确保回调和设备不会访问已释放的 SDL 上下文。
    void close();

    [[nodiscard]] bool dev_open() const { return dev_ != 0; }
    [[nodiscard]] uint64_t delivered() const { return delivered_.load(); }
    [[nodiscard]] uint64_t silence() const { return silence_.load(); }
    [[nodiscard]] uint64_t preroll_silence() const { return preroll_silence_.load(); }
    [[nodiscard]] uint64_t underrun_silence() const { return underrun_silence_.load(); }
    /// 返回输出不足、需要补静音的回调次数；不是传输丢包数。
    [[nodiscard]] uint64_t underrun_callbacks() const { return underrun_callbacks_.load(); }

  private:
    static void fill(void *userdata, Uint8 *stream, int len);

    scrctl::media::AudioPump *pump_ = nullptr;
    SDL_AudioDeviceID dev_ = 0;
    int channels_ = 2;
    std::size_t preroll_ = 0;
    std::atomic<bool> started_{false};
    std::atomic<uint64_t> delivered_{0};
    std::atomic<uint64_t> silence_{0};
    std::atomic<uint64_t> preroll_silence_{0};
    std::atomic<uint64_t> underrun_silence_{0};
    std::atomic<uint64_t> underrun_callbacks_{0};
};

} // namespace scrctl::app
