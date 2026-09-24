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

/// 鼠标原始坐标 -> 整块屏幕的 0..1。**触摸对不对全靠这一条**。
///
/// 传进来的必须已经是**逻辑坐标**（单位 = 裁剪框像素）。SDL2 在设了
/// `SDL_RenderSetLogicalSize` 之后，鼠标事件给的就是逻辑坐标 —— 实测右下角
/// 原始值 (1121,2431) 对上逻辑尺寸 1125x2436，全程 y 铺满 0..2434。
/// 这里曾经多除了一次"窗口点数/绘制面像素"，结果整体差 2.46 倍。
inline void display_fraction_from_logical(double lx, double ly, const Crop &c, double &fx,
                                          double &fy) {
    fx = (c.x + lx) / (c.display_w > 0 ? c.display_w : 1);
    fy = (c.y + ly) / (c.display_h > 0 ? c.display_h : 1);
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
