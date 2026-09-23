#pragma once

#include <algorithm>

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

/// 窗口里的一次点击 -> 整块屏幕的 0..1 归一化坐标。
///
/// 换算必须带上裁剪偏移：窗口看到的是显示区，而触摸面的 0..1 是相对整块屏幕的。
/// 只在"没裁剪"时两者才重合；裁过之后要把窗口坐标先还原成屏幕像素再除以显示
/// 尺寸，否则点哪儿都偏。
///
/// 分母用窗口尺寸而不是 drawable 像素尺寸：SDL2 在 macOS 的高 DPI 下给的鼠标
/// 坐标已经是逻辑点，与窗口尺寸同一坐标系，再乘一次 backing scale 就会翻倍。
inline void display_fraction(int wx, int wy, int win_w, int win_h, const Crop &c, double &fx,
                             double &fy) {
    const double safe_w = win_w > 0 ? win_w : 1;
    const double safe_h = win_h > 0 ? win_h : 1;
    const double sx = c.x + static_cast<double>(wx) * c.w / safe_w;
    const double sy = c.y + static_cast<double>(wy) * c.h / safe_h;
    fx = sx / (c.display_w > 0 ? c.display_w : 1);
    fy = sy / (c.display_h > 0 ? c.display_h : 1);
}

/// 已经过 SDL_RenderWindowToLogical 的坐标（单位就是裁剪框的像素）-> 0..1。
/// 留边与 Retina 缩放都由 SDL 算过了，这里只剩"加上裁剪偏移、除以显示尺寸"。
inline void display_fraction_from_logical(double lx, double ly, const Crop &c, double &fx,
                                          double &fy) {
    fx = (c.x + lx) / (c.display_w > 0 ? c.display_w : 1);
    fy = (c.y + ly) / (c.display_h > 0 ? c.display_h : 1);
}

/// 鼠标位置 -> 整块屏幕的 0..1 归一化坐标。**这条是触摸对不对的唯一依据**。
///
/// 三个坐标系必须一次换算到底，任何一级用错单位都会让点击整体偏移：
///
/// - SDL2 的鼠标事件给的是**窗口逻辑点**（= SDL_GetWindowSize 的单位）；
/// - 绘制面是**像素**，Retina 下是点数的两倍（= SDL_GetRendererOutputSize）；
/// - 设了 logical size 之后还有一层**等比留边**（窗口被拉成别的比例时出现）。
///
/// 这里自己算而不用 SDL_RenderWindowToLogical：那个函数要的是像素，喂点数会
/// 静默偏一半，而且不写出来就没法单测——上一版正是栽在这上面。
inline void window_to_fraction(int wx, int wy, int win_pts_w, int win_pts_h, int out_px_w,
                               int out_px_h, const Crop &c, double &fx, double &fy) {
    const double scale_x = out_px_w / static_cast<double>(std::max(1, win_pts_w));
    const double scale_y = out_px_h / static_cast<double>(std::max(1, win_pts_h));
    const double px = wx * scale_x;
    const double py = wy * scale_y;

    const double lw = c.w > 0 ? c.w : 1;
    const double lh = c.h > 0 ? c.h : 1;
    const double s = std::min(out_px_w / lw, out_px_h / lh);
    const double off_x = (out_px_w - lw * s) / 2.0;
    const double off_y = (out_px_h - lh * s) / 2.0;

    display_fraction_from_logical((px - off_x) / s, (py - off_y) / s, c, fx, fy);
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
