#include "app/SdlRuntime.h"

#include <atomic>

namespace scrctl::app {
namespace {
// atomic_flag 保证无锁，可在信号处理器内使用，也允许信号由其他线程接收。
std::atomic_flag g_stop_requested = ATOMIC_FLAG_INIT;

void on_stop_signal(int signal) {
    g_stop_requested.test_and_set(std::memory_order_relaxed);
#ifdef _WIN32
    // Windows CRT 在调用后恢复默认动作；退出清理期间再次收到信号仍只请求停止。
    std::signal(signal, on_stop_signal);
#else
    (void)signal;
#endif
}
} // namespace

bool SdlRuntime::initialize(Uint32 flags) {
    if (attempted_) {
        SDL_SetError("SDL runtime already initialized");
        return false;
    }
    g_stop_requested.clear(std::memory_order_relaxed);
    // 先保存调用者的处理器。SDL_Init 可能安装自己的处理器，SDL_Quit 也可能恢复它们。
    previous_int_ = std::signal(SIGINT, on_stop_signal);
    previous_term_ = std::signal(SIGTERM, on_stop_signal);
    attempted_ = true;
    // 使用普通 main，不依赖 SDL 的平台入口包装器。
    SDL_SetMainReady();
    const bool initialized = SDL_Init(flags) == 0;
    // 必须在 SDL 后重新安装，否则退出请求可能被 SDL 吞掉。
    std::signal(SIGINT, on_stop_signal);
    std::signal(SIGTERM, on_stop_signal);
    return initialized;
}

SdlRuntime::~SdlRuntime() {
    if (!attempted_) {
        return;
    }
    // 失败的 SDL_Init 也可能已经初始化了部分子系统。
    SDL_Quit();
    if (previous_int_ != SIG_ERR) {
        std::signal(SIGINT, previous_int_);
    }
    if (previous_term_ != SIG_ERR) {
        std::signal(SIGTERM, previous_term_);
    }
}

bool SdlRuntime::stop_requested() const { return g_stop_requested.test(std::memory_order_relaxed); }

} // namespace scrctl::app
