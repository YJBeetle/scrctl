#pragma once

#include <SDL.h>
#include <csignal>
#include <string>

namespace scrctl::app {

/// 应用独占的 SDL 和退出信号运行期；不支持并发或嵌套 run。
/// 声明在媒体源和窗口之前，确保它们先析构。应用在起流前初始化，避免本机
/// 输出已确定不可用时仍切换手机音频路由。
class SdlRuntime final {
  public:
    SdlRuntime() = default;
    SdlRuntime(const SdlRuntime &) = delete;
    SdlRuntime &operator=(const SdlRuntime &) = delete;
    ~SdlRuntime();

    bool initialize(Uint32 flags);
    /// 返回是否可以建立音频会话。显式禁用播放时不初始化声卡；播放必需但
    /// SDL 音频初始化失败时返回 false 并填 err。关闭音频时不访问音频后端。
    /// 调用前必须已成功 initialize；实际声卡的打开仍由 AudioOut 完成。
    bool prepare_audio(bool requested, bool playback, std::string &err);
    [[nodiscard]] bool stop_requested() const;

  private:
    using SignalHandler = void (*)(int);
    bool attempted_ = false;
    bool initialized_ = false;
    SignalHandler previous_int_ = SIG_ERR;
    SignalHandler previous_term_ = SIG_ERR;
};

} // namespace scrctl::app
