#pragma once

#include <algorithm>
#include <cstdint>

namespace scrctl::app {

/// 计算当前统计窗口的秒数，并将 last_ms 更新为 now_ms。
/// 每类计数器必须使用独立时钟，时间窗口与计数基线同步更新。画面切到截图
/// 时，后台视频仍累计数据；共用截图时钟会在切回后把长时间增量除以约一秒。
/// last_ms==0 返回 1.0 作为未初始化值；正常源在建立时初始化时钟。
inline double settle_window(uint64_t now_ms, uint64_t &last_ms) {
    const double secs = last_ms == 0
                            ? 1.0
                            : std::max(0.001, static_cast<double>(now_ms - last_ms) / 1000.0);
    last_ms = now_ms;
    return secs;
}

/// 对累计计数计算增量并更新基线。换源和重建会话可能归零，因此先检查
/// 计数回退，避免无符号减法下溢。
inline uint64_t counter_delta(uint64_t total, uint64_t &base) {
    const uint64_t delta = total >= base ? total - base : 0;
    base = total;
    return delta;
}

}  // namespace scrctl::app
