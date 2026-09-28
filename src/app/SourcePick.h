#pragma once

#include <cstdint>

namespace scrctl::app {

/// 取每一帧之前，画面该从哪条路来。
///
/// 为什么把它抽成纯函数：运行中降级是一本四格状态账（起流就降级 / 跑着降级 / 泵回升 /
/// 截图源起失败过），第一版写在 `LiveSource::next()` 里时把"起流就降级"那一格判成了
/// "回升"——那一格泵根本不存在（`pump_ == nullptr`），而它在 iOS 18 上是**常态**，错了
/// 就是兜底路一帧都出不来。四格状态机值得一个能离线跑全组合的判据，而不是等真机凑现场。
enum class SourcePick {
    kStayStream,  ///< 继续等媒体泵（含"两条路都没有"，调用方自己判空）
    kStayShot,    ///< 继续用截图源
    kToShot,      ///< 切去截图源：调用方负责起它，起失败要记下来别再撞
    kToStream,    ///< 泵回升了：调用方负责释放截图源
};

/// 截图源起失败之后过多久再试一次。
///
/// 一次失败不该判永久（审查 P2）：截图 RPC 可能只是暂时不通，而媒体后端仍解不出画面，
/// 判永久就等于本次会话一路停在旧画面上。30 秒与 FramePump 降级期的重试同节奏——
/// 每帧都撞会把渲染线程泡在建连接的来回里，60 秒又让"爬回来"慢得看不出在救。
inline constexpr uint64_t kShotRetryMs = 30000;

/// `has_pump` 媒体泵存在（起流成功过）；`video_dead` 泵自报当前解不出画面
/// （`FramePump::video_unusable`：连续重起仍拿不到关键帧）；`has_shot` 截图源活着；
/// `ms_since_shot_fail` 距上次截图源起失败过了多少毫秒，从没失败过传 `UINT64_MAX`
/// （冷却期内不重试，冷却一过就再试）。
inline SourcePick pick_picture_source(bool has_pump, bool video_dead, bool has_shot,
                                      uint64_t ms_since_shot_fail) {
    if (has_shot) {
        // 起流就降级的那一格泵不存在：`!video_dead` 在那里是"没有泵"而不是"泵回升"，
        // 必须留在截图路上。这一格就是第一版写错的地方。
        return (has_pump && !video_dead) ? SourcePick::kToStream : SourcePick::kStayShot;
    }
    if (has_pump && video_dead && ms_since_shot_fail >= kShotRetryMs) {
        return SourcePick::kToShot;
    }
    return SourcePick::kStayStream;
}

}  // namespace scrctl::app
