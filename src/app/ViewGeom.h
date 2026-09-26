#pragma once

#include <algorithm>
#include <string_view>

namespace scrctl::app {

/// 画面里"看得见的那一块"以及它在整块屏幕里的位置。
///
/// 设备编码分辨率比逻辑显示大（HEVC CTU 对齐填充）。实测 iPhone 13 mini 编码
/// 1136x2464、逻辑显示 1125x2436，右侧 11px 与底部 28px 是垃圾像素。
struct Crop {
    int x = 0, y = 0, w = 0, h = 0;
    /// 逻辑显示的尺寸。触摸报告的 0..1 是相对**整块屏幕**的，所以换算的除数是它，
    /// 不是编码帧的尺寸——用后者会把右/下方向偏出 1% 左右，边缘点击就错。
    int display_w = 0, display_h = 0;
};

/// 把命令行给的裁剪框落成一次会话用的 Crop。
///
/// `display_w/display_h` 是整块逻辑显示的尺寸，而 `--crop` 只决定"窗口里看哪一块"
/// ——**它不换分母**。这里曾经把分母写成裁剪框自己的尺寸，于是只要给了 `--crop`，
/// 每个触摸点都会被按比例往左上压，越靠右下偏得越多（画面看着对，点下去不对）。
inline Crop make_crop(bool crop_given, int x, int y, int w, int h, int coded_w, int coded_h,
                      int display_w, int display_h) {
    Crop c;
    if (crop_given) {
        c = {x, y, w, h, display_w, display_h};
    } else {
        c = {0, 0, display_w, display_h, display_w, display_h};
    }
    // 夹进编码帧之内：越界的框会算出 0 或负的窗口尺寸，而 0 尺寸窗口是启动即闪退。
    c.x = std::max(0, std::min(c.x, coded_w - 1));
    c.y = std::max(0, std::min(c.y, coded_h - 1));
    c.w = std::max(1, std::min(c.w, coded_w - c.x));
    c.h = std::max(1, std::min(c.h, coded_h - c.y));
    c.display_w = std::max(1, c.display_w);
    c.display_h = std::max(1, c.display_h);
    return c;
}

/// 设备给的界面旋转（`displays[].currentOrientation`）-> **顺时针**角度。
///
/// 规则是拿真机对出来的，不是推的：设备报 `rot270` 时，把面板那一帧**顺时针转 270°**
/// 得到的就是正立画面（对照过截图服务给的同一屏图，它给的是已经转正的 2436x1125）。
/// 所以这个数就是"要转正需要顺时针转多少"。
///
/// 认不出来的值一律当 0：**宁可不转，也不能把画面转歪**——转歪之后触摸与画面全都错，
/// 而不转只是画面躺着、触摸仍与画面一致。
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

/// 视口（画面上看到的那一块，可能已经被转正过）里的逻辑像素 -> 整块屏幕的 0..1。
///
/// **触摸对不对全靠这一条**，而它要同时处理三件事：
///
/// 1. `lx/ly` 必须是**逻辑坐标**（SDL2 在设了 `SDL_RenderSetLogicalSize` 之后给的就是）。
///    这里曾经多除一次"窗口点数/绘制面像素"，结果整体差 2.46 倍。
/// 2. 视口尺寸随旋转而换向：`degrees` 是 90/270 时视口是"宽高对调"的，
///    所以除数用 `c.h`/`c.w` 而不是 `c.w`/`c.h`。
/// 3. 裁剪框的偏移在**面板轴**上，因此必须先把视口归一化坐标按旋转映射回面板轴，
///    再加 `c.x/c.y`、再除以整块屏的尺寸。顺序反了就会在横屏 App 上点偏一整条边。
///
/// 映射表来自"视口 = 面板顺时针转 degrees"这一条实测，取它的反变换：
/// `0:(u,v) 90:(v,1-u) 180:(1-u,1-v) 270:(1-v,u)`。
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

/// 没旋转时的那条路（`degrees = 0`）。留着是因为它有自己的历史与测试，
/// 而文件回放那条路根本没有旋转可言。
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

/// 窗口尺寸：按 scale 缩放裁剪框，但**不许超过屏幕**。
///
/// 手机的逻辑显示（1125x2436）比笔记本屏幕（约 1680x1050 点）高出一倍多，直接
/// 按 1:1 建窗口会被窗口管理器裁掉——裁完内容填不满、比例也不对。这里等比缩到
/// 放得下为止；用户显式给了 --scale 就照他的，不擅自改。
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
    // 兜底放在所有分支之后：0 尺寸的窗口 SDL 建不出来，症状是"启动就闪退"，
    // 比画得难看难查得多。
    win_w = std::max(1, win_w);
    win_h = std::max(1, win_h);
}

}  // namespace scrctl::app
