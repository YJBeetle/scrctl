#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace scrctl::app {

/// 在取帧前选择实时流或截图。纯函数覆盖启动即截图、运行中降级、视频恢复
/// 和截图启动失败等状态，便于离线遍历全部组合。无媒体泵时不能误判为恢复。
enum class SourcePick {
    kStayStream,  ///< 继续等媒体泵（含"两条路都没有"，调用方自己判空）
    kStayShot,    ///< 继续用截图源
    kToShot,      ///< 切去截图源：调用方负责起它，起失败要记下来别再撞
    kToStream,    ///< 泵回升了：调用方负责释放截图源
};

/// 截图启动失败后的重试间隔。暂时的 RPC 失败不应永久停留在旧画面；
/// 使用 30 秒退避，避免每帧建立连接，也与视频降级期间的重试节奏一致。
inline constexpr uint64_t kShotRetryMs = 30000;

/// `has_pump` 媒体泵存在（起流成功过）；`video_dead` 泵自报当前解不出画面
/// （`FramePump::video_unusable`：连续重起仍拿不到关键帧）；`has_shot` 截图源活着；
/// `ms_since_shot_fail` 距上次截图源起失败过了多少毫秒，从没失败过传 `UINT64_MAX`
/// （冷却期内不重试，冷却一过就再试）。
inline SourcePick pick_picture_source(bool has_pump, bool video_dead, bool has_shot,
                                      uint64_t ms_since_shot_fail) {
    if (has_shot) {
        // 启动时已使用截图且不存在媒体泵，应继续截图，不能将 !video_dead 当作恢复。
        return (has_pump && !video_dead) ? SourcePick::kToStream : SourcePick::kStayShot;
    }
    if (has_pump && video_dead && ms_since_shot_fail >= kShotRetryMs) {
        return SourcePick::kToShot;
    }
    return SourcePick::kStayStream;
}

/// --test-degrade 将逗号分隔的秒数转换为相对启动时刻的毫秒。
/// 用于在真机强制触发运行中降级，验证生产状态机的切换、序号和回收路径。
/// 开关仅覆盖 video_dead 输入，不改变状态机规则。
inline bool degrade_forced(uint64_t now_ms, const std::vector<uint64_t> &marks) {
    std::size_t passed = 0;
    for (const uint64_t m : marks) {
        if (now_ms < m) {
            break;
        }
        ++passed;
    }
    // 从第一个时刻起交替：[m0,m1) 强制降级、[m1,m2) 放开、……没给时刻就永远不强制。
    return passed % 2 == 1;
}

/// 解析降级时刻表，格式错误时返回 false 并填写 err，避免无效开关被静默忽略。
inline bool parse_degrade_marks(std::string_view spec, std::vector<uint64_t> &out,
                                std::string &err) {
    out.clear();
    const std::string text(spec);
    const char *p = text.c_str();
    while (*p != '\0') {
        char *end = nullptr;
        const double secs = std::strtod(p, &end);
        if (end == p) {
            err = std::string("存在无效数字：\"") + p + "\"";
            return false;
        }
        // 拒绝非有限值和超出转换范围的值。strtod 可接受 nan / inf，NaN 的比较
        // 无法被普通负数判断捕获；从非有限或越界浮点值转换整数是未定义行为。
        if (!std::isfinite(secs)) {
            err = std::string("时刻不是有限数：\"") + std::string(p, static_cast<std::size_t>(end - p)) + "\"";
            return false;
        }
        if (secs < 0) {
            err = "时刻不能是负数";
            return false;
        }
        // 确认秒数换算为毫秒后可由 uint64_t 表示；不另加人为时间上限。
        // 下面的阈值向上取整到 2^64。
        constexpr double kMaxMs = static_cast<double>(UINT64_MAX);  // 向上取整到 2^64
        if (secs * 1000.0 >= kMaxMs) {
            err = std::string("时刻超过 uint64 毫秒范围：\"") + std::string(p, static_cast<std::size_t>(end - p)) + "\"";
            return false;
        }
        const auto ms = static_cast<uint64_t>(secs * 1000.0);
        if (!out.empty() && ms < out.back()) {
            err = "时刻必须按升序排列";
            return false;
        }
        out.push_back(ms);
        p = end;
        if (*p == ',') {
            ++p;
            if (*p == '\0') {
                err = "末尾不能包含逗号";
                return false;
            }
            continue;
        }
        if (*p != '\0') {
            err = std::string("存在无效字符：\"") + p + "\"";
            return false;
        }
    }
    if (out.empty()) {
        err = "时刻表至少需要一个时刻";
        return false;
    }
    return true;
}

}  // namespace scrctl::app
