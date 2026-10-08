#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace scrctl::app {

/// 与一次成功取帧一起保存的几何快照。display_w/h 使用设备 currentMode.size
/// 的面板轴；panel_degrees 来自 currentOrientation，不根据图像宽高猜测方向。
/// 截图像素已按该角度摆正，视频像素仍在面板轴上；二者的渲染角不能代替原始角。
/// 这是交付时保存的软件快照，截图 RPC 与方向订阅不是设备端原子采样。
struct FrameGeometry {
    int display_w = 0, display_h = 0;
    std::optional<int> panel_degrees = 0;
    bool screenshot = false;
};

/// 设备可见区及其在完整编码帧中的位置。HEVC 编码可能包含 CTU 对齐填充；
/// iPhone 13 mini 实测编码为 1136x2464，可见区为 1125x2436，
/// 右侧 11 px 和底部 28 px 不属于显示内容。
struct Crop {
    int x = 0, y = 0, w = 0, h = 0;
    /// 设备整屏尺寸，用作触摸归一化分母；编码尺寸包含填充，不能作为触摸分母。
    int display_w = 0, display_h = 0;
    /// 源像素相对面板已做的顺时针旋转；截图用设备原始角，视频为 0。
    int pixel_degrees = 0;
    /// 缺少截图方向或尺寸与快照矛盾时，继续显示图像，但不推测触摸坐标。
    bool input_valid = true;
};

/// 构造会话裁剪框。display_w/display_h 是设备整屏尺寸；--crop 仅指定
/// 显示哪一部分，不能改变触摸归一化分母，否则裁剪后坐标会按比例偏移。
inline Crop make_crop(bool crop_given, int x, int y, int w, int h, int coded_w, int coded_h,
                      int display_w, int display_h) {
    Crop c;
    if (crop_given) {
        c = {x, y, w, h, display_w, display_h};
    } else {
        c = {0, 0, display_w, display_h, display_w, display_h};
    }
    // 将裁剪框限制在编码帧内，保证窗口尺寸为正。
    c.x = std::max(0, std::min(c.x, coded_w - 1));
    c.y = std::max(0, std::min(c.y, coded_h - 1));
    c.w = std::max(1, std::min(c.w, coded_w - c.x));
    c.h = std::max(1, std::min(c.h, coded_h - c.y));
    c.display_w = std::max(1, c.display_w);
    c.display_h = std::max(1, c.display_h);
    return c;
}

/// --crop 始终使用当前源图像的像素坐标。截图默认显示整张 PNG，不以面板的
/// 竖幅尺寸裁它；触摸分母仍使用面板尺寸，先去掉源像素已经应用的旋转。
inline Crop make_frame_crop(bool crop_given, int x, int y, int w, int h, int coded_w,
                           int coded_h, const FrameGeometry &geometry) {
    if (!geometry.screenshot) {
        return make_crop(crop_given, x, y, w, h, coded_w, coded_h,
                         geometry.display_w > 0 ? geometry.display_w : coded_w,
                         geometry.display_h > 0 ? geometry.display_h : coded_h);
    }
    Crop c = make_crop(crop_given, x, y, w, h, coded_w, coded_h, coded_w, coded_h);
    const auto angle = geometry.panel_degrees;
    const bool known = angle && (*angle == 0 || *angle == 90 || *angle == 180 || *angle == 270);
    c.pixel_degrees = known ? *angle : 0;
    const bool swapped = c.pixel_degrees == 90 || c.pixel_degrees == 270;
    int panel_w = geometry.display_w, panel_h = geometry.display_h;
    if (panel_w <= 0 || panel_h <= 0) {
        // 已知原始角时，PNG 的可见区尺寸可逆变换为面板尺寸；它不能提供旋转方向。
        panel_w = swapped ? coded_h : coded_w;
        panel_h = swapped ? coded_w : coded_h;
    }
    c.display_w = std::max(1, panel_w);
    c.display_h = std::max(1, panel_h);
    c.input_valid = known && coded_w > 0 && coded_h > 0 &&
                    coded_w == (swapped ? panel_h : panel_w) &&
                    coded_h == (swapped ? panel_w : panel_h);
    return c;
}

/// 保留无法识别的方向，供截图触摸映射决定是否可用。
[[nodiscard]] inline std::optional<int> parse_orientation_degrees(std::string_view orientation) {
    if (orientation == "rot0") {
        return 0;
    }
    if (orientation == "rot90") {
        return 90;
    }
    if (orientation == "rot180") {
        return 180;
    }
    if (orientation == "rot270") {
        return 270;
    }
    return std::nullopt;
}

/// 将设备 currentOrientation 转换为顺时针转正角度。
/// 实测 rot270 对应顺时针 270 度，与截图服务的正立图一致。
/// 未知值按 0 度处理，保持画面与触摸映射一致。
[[nodiscard]] inline int orientation_degrees(std::string_view orientation) {
    return parse_orientation_degrees(orientation).value_or(0);
}

/// 将旋转后的视口逻辑像素转换为设备整屏归一化坐标：
/// 1. lx/ly 是 SDL logical size 空间坐标，不重复换算窗口点数或输出像素。
/// 2. 90/270 度时视口宽高交换，用 c.h/c.w 归一化。
/// 3. 先逆窗口旋转，再逆裁剪区域内的水平翻转，加源像素裁剪偏移，
///    最后逆截图已经应用的旋转；不能把翻转直接施加到设备整屏坐标上。
/// 未知截图方向时返回 false，不发布部分坐标；调用方须释放已有触摸。
inline bool viewport_fraction_to_panel(double lx, double ly, const Crop &c, int degrees,
                                       double &fx, double &fy, bool horizontal_flip = false) {
    if (!c.input_valid) {
        return false;
    }
    const bool swapped = degrees == 90 || degrees == 270;
    // 转 90/270 之后视口是"宽高对调"的：面板 1125x2436 -> 视口 2436x1125。
    const double vw = swapped ? static_cast<double>(c.h) : static_cast<double>(c.w);
    const double vh = swapped ? static_cast<double>(c.w) : static_cast<double>(c.h);
    const double u = lx / (vw > 0 ? vw : 1);
    const double v = ly / (vh > 0 ? vh : 1);
    double pu = u, pv = v;  // 裁剪框内、源像素轴上的归一化坐标
    switch (degrees) {
        case 90: pu = v; pv = 1.0 - u; break;
        case 180: pu = 1.0 - u; pv = 1.0 - v; break;
        case 270: pu = 1.0 - v; pv = u; break;
        default: break;
    }
    if (horizontal_flip) pu = 1.0 - pu;
    const bool pixel_swapped = c.pixel_degrees == 90 || c.pixel_degrees == 270;
    const int image_w = pixel_swapped ? c.display_h : c.display_w;
    const int image_h = pixel_swapped ? c.display_w : c.display_h;
    const double ix = (c.x + pu * c.w) / (image_w > 0 ? image_w : 1);
    const double iy = (c.y + pv * c.h) / (image_h > 0 ? image_h : 1);
    switch (c.pixel_degrees) {
        case 90: fx = iy; fy = 1.0 - ix; break;
        case 180: fx = 1.0 - ix; fy = 1.0 - iy; break;
        case 270: fx = 1.0 - iy; fy = ix; break;
        default: fx = ix; fy = iy; break;
    }
    return true;
}

/// 零旋转的坐标映射，供文件回放和对应测试使用。
inline void display_fraction_from_logical(double lx, double ly, const Crop &c, double &fx,
                                          double &fy) {
    viewport_fraction_to_panel(lx, ly, c, 0, fx, fy);
}

/// 视口尺寸（窗口与 logical size 要用它，旋转 90/270 时宽高对调）。
inline void viewport_size(const Crop &c, int degrees, int &width, int &height) {
    if (degrees == 90 || degrees == 270) {
        width = c.h;
        height = c.w;
    } else {
        width = c.w;
        height = c.h;
    }
}

/// 计算窗口点数；view_w/h 必须使用裁剪并旋转后的视口尺寸。
/// 两维均为 0 时按 scale 计算，默认比例再限制到可用屏幕；显式 --scale 不缩小。
/// 指定一维时保留该维，另一维按视口比例取整；指定两维时保留窗口尺寸，由 SDL 留边。
/// 结果最小为 1 点。无法用 int 表示时返回 false，不发布部分结果。
inline bool fit_window(int view_w, int view_h, int avail_w, int avail_h, double scale,
                       bool scale_given, int &win_w, int &win_h, int want_w = 0, int want_h = 0) {
    if (want_w < 0 || want_h < 0) {
        return false;
    }
    const int vw = std::max(1, view_w);
    const int vh = std::max(1, view_h);
    int width = want_w, height = want_h;
    if (want_w > 0 || want_h > 0) {
        if (want_h == 0) {
            // 两个正 int 的乘积可放入 int64_t；先扩宽再乘，避免中间结果溢出。
            const int64_t derived = std::max<int64_t>(1, int64_t(want_w) * vh / vw);
            if (derived > std::numeric_limits<int>::max()) {
                return false;
            }
            height = static_cast<int>(derived);
        } else if (want_w == 0) {
            const int64_t derived = std::max<int64_t>(1, int64_t(want_h) * vw / vh);
            if (derived > std::numeric_limits<int>::max()) {
                return false;
            }
            width = static_cast<int>(derived);
        }
    } else {
        if (!std::isfinite(scale) || scale <= 0) {
            return false;
        }
        double fit = scale;
        if (!scale_given && avail_w > 0 && avail_h > 0) {
            fit = std::min(fit, std::min(static_cast<double>(avail_w) / vw,
                                         static_cast<double>(avail_h) / vh));
        }
        const double scaled_w = vw * fit, scaled_h = vh * fit;
        if (scaled_w > std::numeric_limits<int>::max() ||
            scaled_h > std::numeric_limits<int>::max()) {
            return false;
        }
        width = std::max(1, static_cast<int>(scaled_w));
        height = std::max(1, static_cast<int>(scaled_h));
    }
    win_w = width;
    win_h = height;
    return true;
}

}  // namespace scrctl::app
