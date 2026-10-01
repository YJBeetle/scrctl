#pragma once

#include <algorithm>
#include <cstdint>

namespace scrctl::app {

/// `--stats` 一段窗口的分母（秒），并把尺推到 `now_ms`。
///
/// **一本账一把尺**：分母必须是"这本账上次结算到现在"，不能是"上次打印任何东西到
/// 现在"。共用一把尺是什么形状，本仓库真有过（审查 P2）：`last_stats_ms_` 被两个画面源
/// 分支各自推进，而媒体泵的计数基线只在媒体分支里更新。截图兜底那 30 秒里泵仍在后台
/// 收包计数，切回实时流后第一段就把 30 秒的增量除以约 1 秒——收包/AU/解码/音频速率
/// 一起虚高到真实值的几十倍，而打印出来的分母写着 1.0s，看起来完全自洽。
/// 隧道内 TCP 那一行早就因为同一个理由自带 `last_tcp_ms_`。
///
/// `last_ms == 0` 是"还没起表"：返回 1.0。正常路径不该走到——源建好时就把尺置成当下
/// 的时刻，那样第一次结算的分母是真实经过的时间，而不是一个假数。
inline double settle_window(uint64_t now_ms, uint64_t &last_ms) {
    const double secs = last_ms == 0
                            ? 1.0
                            : std::max(0.001, static_cast<double>(now_ms - last_ms) / 1000.0);
    last_ms = now_ms;
    return secs;
}

/// 只增累计数做差，带下溢守卫，并把基线推到当前。
///
/// 守卫不是多余的：换源与重起会话都会让计数从 0 重数，无符号做差就下溢成一个天文数字
/// ——真机上打出过 `画面 18156244167036960768.00/s`。
inline uint64_t counter_delta(uint64_t total, uint64_t &base) {
    const uint64_t delta = total >= base ? total - base : 0;
    base = total;
    return delta;
}

}  // namespace scrctl::app
