#pragma once

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

}  // namespace scrctl::app
