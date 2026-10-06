#pragma once

#include <algorithm>
#include <string_view>

namespace scrctl::app {

/// 设备可见区及其在完整编码帧中的位置。HEVC 编码可能包含 CTU 对齐填充；
/// iPhone 13 mini 实测编码为 1136x2464，可见区为 1125x2436，
/// 右侧 11 px 和底部 28 px 不属于显示内容。
struct Crop {
    int x = 0, y = 0, w = 0, h = 0;
    /// 设备整屏尺寸，用作触摸归一化分母；编码尺寸包含填充，不能作为触摸分母。
    int display_w = 0, display_h = 0;
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

/// 将设备 currentOrientation 转换为顺时针转正角度。
/// 实测 rot270 对应顺时针 270 度，与截图服务的正立图一致。
/// 未知值按 0 度处理，保持画面与触摸映射一致。
[[nodiscard]] inline int orientation_degrees(std::string_view orientation) {
    if (orientation == "rot90") {
        return 90;
    }
    if (orientation == "rot180") {
        return 180;
    }
    if (orientation == "rot270") {
        return 270;
    }
    return 0;
}

/// 将旋转后的视口逻辑像素转换为设备整屏归一化坐标：
/// 1. lx/ly 是 SDL logical size 空间坐标，不重复换算窗口点数或输出像素。
/// 2. 90/270 度时视口宽高交换，用 c.h/c.w 归一化。
/// 3. 先逆旋转回面板坐标，再加裁剪偏移，最后除以设备整屏尺寸。
/// 下面的映射取“面板顺时针旋转 degrees 得到视口”的逆变换。
inline void viewport_fraction_to_panel(double lx, double ly, const Crop &c, int degrees,
                                       double &fx, double &fy) {
    const bool swapped = degrees == 90 || degrees == 270;
    // 转 90/270 之后视口是"宽高对调"的：面板 1125x2436 -> 视口 2436x1125。
    const double vw = swapped ? static_cast<double>(c.h) : static_cast<double>(c.w);
    const double vh = swapped ? static_cast<double>(c.w) : static_cast<double>(c.h);
    const double u = lx / (vw > 0 ? vw : 1);
    const double v = ly / (vh > 0 ? vh : 1);
    double pu = u, pv = v;  // 裁剪框内、面板轴上的归一化坐标
    switch (degrees) {
        case 90: pu = v; pv = 1.0 - u; break;
        case 180: pu = 1.0 - u; pv = 1.0 - v; break;
        case 270: pu = 1.0 - v; pv = u; break;
        default: break;
    }
    fx = (c.x + pu * c.w) / (c.display_w > 0 ? c.display_w : 1);
    fy = (c.y + pv * c.h) / (c.display_h > 0 ? c.display_h : 1);
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

/// 按 scale 计算窗口尺寸。默认比例超过屏幕时等比缩小；
/// 用户显式指定 --scale 时保留其比例。手机显示通常高于电脑可用屏幕区域。
inline void fit_window(int crop_w, int crop_h, int avail_w, int avail_h, double scale,
                       bool scale_given, int &win_w, int &win_h) {
    win_w = static_cast<int>(crop_w * scale);
    win_h = static_cast<int>(crop_h * scale);
    if (!scale_given && crop_w > 0 && crop_h > 0 && avail_w > 0 && avail_h > 0) {
        const double fit = std::min(1.0, std::min(static_cast<double>(avail_w) / win_w,
                                                  static_cast<double>(avail_h) / win_h));
        if (fit < 1.0) {
            win_w = static_cast<int>(crop_w * fit);
            win_h = static_cast<int>(crop_h * fit);
        }
    }
    // 所有分支都保证窗口尺寸大于零，避免 SDL 创建失败。
    win_w = std::max(1, win_w);
    win_h = std::max(1, win_h);
}

}  // namespace scrctl::app
