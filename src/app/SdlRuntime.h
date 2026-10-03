#pragma once

#include <SDL.h>
#include <csignal>

namespace scrctl::app {

/// 应用独占的 SDL 和退出信号运行期；不支持并发或嵌套 run。
/// 声明在媒体源和窗口之前，确保它们先析构；初始化仍可等到起流成功之后。
class SdlRuntime final {
  public:
    SdlRuntime() = default;
    SdlRuntime(const SdlRuntime &) = delete;
    SdlRuntime &operator=(const SdlRuntime &) = delete;
    ~SdlRuntime();

    bool initialize(Uint32 flags);
    [[nodiscard]] bool stop_requested() const;

  private:
    using SignalHandler = void (*)(int);
    bool attempted_ = false;
    SignalHandler previous_int_ = SIG_ERR;
    SignalHandler previous_term_ = SIG_ERR;
};

} // namespace scrctl::app
