#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

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

/// `--test-degrade` 的时刻表：把 "4,8,12" 这种逗号分隔的**秒**换成相对起点的毫秒。
///
/// 为什么产品里会有一个测试开关：上面那本状态账里"跑着跑着解不出画面"这一格，在真机上
/// **打不响**——要画面复杂到编码器交出超过解码后端上限的帧，或者连续三次重起都拿不到
/// 关键帧。而连着三轮审查的修复（切换、序号、回收）全在这一格上，没有触发器就只能一直
/// 交"离线判据 + 代码论证"。所以给一个时刻表，让**同一段状态机**在真机上跑起来：它不改
/// 状态机本身，只是把 `video_dead` 那一个入参顶成真。
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

/// 解析 `--test-degrade` 的规格。格式错就返回 false 并把原因写进 `err`——这条旗标的
/// 用途是打判据，静默忽略一个写错的规格等于让人对着一个从没生效的开关读日志。
inline bool parse_degrade_marks(std::string_view spec, std::vector<uint64_t> &out,
                                std::string &err) {
    out.clear();
    const std::string text(spec);
    const char *p = text.c_str();
    while (*p != '\0') {
        char *end = nullptr;
        const double secs = std::strtod(p, &end);
        if (end == p) {
            err = std::string("有一段不是数字：\"") + p + "\"";
            return false;
        }
        if (secs < 0) {
            err = "时刻不能是负数";
            return false;
        }
        const auto ms = static_cast<uint64_t>(secs * 1000.0);
        if (!out.empty() && ms < out.back()) {
            err = "时刻要按升序给（这一段比前一个早）";
            return false;
        }
        out.push_back(ms);
        p = end;
        if (*p == ',') {
            ++p;
            if (*p == '\0') {
                err = "尾巴上多了个逗号";
                return false;
            }
            continue;
        }
        if (*p != '\0') {
            err = std::string("有认不出的字符：\"") + p + "\"";
            return false;
        }
    }
    if (out.empty()) {
        err = "时刻表是空的（至少要给一个时刻）";
        return false;
    }
    return true;
}

}  // namespace scrctl::app
