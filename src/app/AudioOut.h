#pragma once

#include "media/AudioPump.h"
#include <SDL.h>
#include <atomic>
#include <string>

namespace scrctl::app {

/// 音频的 SDL 出口：按系统要的节拍从 AudioPump 里取 PCM。
///
/// 为什么这一层在 app 而不在 media：拿到 PCM 之后怎么放（声卡 / 写文件 / 丢掉的）是
/// 客户端的事，而"起流、解码、攒缓冲、回 RR"对任何客户端都一样——控制单元那条路就
/// 只要泵不要声卡。
class AudioOut {
  public:
    AudioOut() = default;
    AudioOut(const AudioOut &) = delete;
    AudioOut &operator=(const AudioOut &) = delete;
    ~AudioOut() { close(); }

    /// 打开默认输出设备。水位由泵自己定（`AudioPump::preroll_frames()`），这里不再
    /// 从外面传毫秒数——否则"攒多久"这件事会有两处换算，而它们会分家。
    ///
    /// 协商是**逐项对死**的（见下面的判据）：这一层没有重采样器，所以只有"系统给的
    /// 就是我们送的"这一种情况能开口放。
    ///
    /// 为什么要 preroll：一开口就取，第一个回调必然赶上"缓冲里才两三个包"的时刻，
    /// 于是起始十几毫秒全是补静音的接缝，听感是一声咔。攒 50ms 再放就把它压成
    /// 起始延迟——这也是 scrcpy 那个默认值存在的原因。
    bool open(scrctl::media::AudioPump &pump, std::string &err);

    /// 关设备。**必须在 SDL_Quit 之前**调到——之后音频子系统已经拆了，
    /// 再 SDL_CloseAudioDevice 就是对着已释放的上下文操作。
    void close();

    [[nodiscard]] bool dev_open() const { return dev_ != 0; }
    [[nodiscard]] uint64_t delivered() const { return delivered_.load(); }
    [[nodiscard]] uint64_t silence() const { return silence_.load(); }

  private:
    static void fill(void *userdata, Uint8 *stream, int len);

    scrctl::media::AudioPump *pump_ = nullptr;
    SDL_AudioDeviceID dev_ = 0;
    int channels_ = 2;
    std::size_t preroll_ = 0;
    std::atomic<bool> started_{false};
    std::atomic<uint64_t> delivered_{0};
    std::atomic<uint64_t> silence_{0};
};

} // namespace scrctl::app
