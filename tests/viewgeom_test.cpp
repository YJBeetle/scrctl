// 窗口坐标 -> 屏幕归一化坐标 的换算自检。
//
// 这段几何只有在这里才测得到：它藏在 SDL 事件处理里，而事件处理需要真窗口和
// 真鼠标。偏偏它是最容易静默错掉的一层——分母用错（编码帧尺寸 vs 逻辑显示尺寸）
// 或者忘了裁剪偏移，画面照样动，只是点哪儿都偏一点，然后花一小时怀疑设备。
#include <cmath>
#include <cstdio>
#include <string>

#include "app/ViewGeom.h"

namespace {

int Failures = 0;

void check(bool ok, const std::string &what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) {
        ++Failures;
    }
}

using scrctl::app::Crop;
using scrctl::app::display_fraction;
using scrctl::app::display_fraction_from_logical;
using scrctl::app::fit_window;
using scrctl::app::window_to_fraction;

bool near(double v, double want, double tol = 1e-6) { return std::fabs(v - want) < tol; }

/// 没裁剪：窗口就是整块屏幕。
void test_full_display() {
    std::printf("\n== 未裁剪 ==\n");
    const Crop c{0, 0, 1125, 2436, 1125, 2436};
    double fx = 0, fy = 0;
    display_fraction(0, 0, 1125, 2436, c, fx, fy);
    check(near(fx, 0.0) && near(fy, 0.0), "左上角 -> (0,0)");
    display_fraction(1124, 2435, 1125, 2436, c, fx, fy);
    check(fx > 0.998 && fy > 0.998, "右下角 -> 接近 (1,1)");
    display_fraction(562, 1218, 1125, 2436, c, fx, fy);
    check(near(fx, 0.499556, 1e-3) && near(fy, 0.5), "窗口正中 -> 屏幕正中");
}

/// 真机那对数字：编码 1136x2464，逻辑显示 1125x2436。
/// 除数用错了就会偏出 ~1%，这里钉住它。
void test_ctu_padding_not_in_the_denominator() {
    std::printf("\n== 分母是逻辑显示，不是编码帧 ==\n");
    const Crop c{0, 0, 1125, 2436, 1125, 2436};
    double fx = 0, fy = 0;
    display_fraction(1125 / 2, 2436 / 2, 1125, 2436, c, fx, fy);
    check(near(fx, 0.5, 0.001) && near(fy, 0.5, 0.001), "正中仍是正中");
    // 如果误用 1136/2464 当除数，右下角会停在 0.990/0.988 而不是接近 1。
    display_fraction(1124, 2435, 1125, 2436, c, fx, fy);
    check(fx > 0.99 && fy > 0.99, "右下角不被 CTU 填充吃掉（>0.99）");
}

/// 裁了显示区的一块：窗口正中应该落在屏幕的偏上位置，而不是 0.5。
void test_cropped_viewport() {
    std::printf("\n== 裁剪过的视口 ==\n");
    // 只看屏幕上半部：x 0..1125, y 0..1218
    const Crop top{0, 0, 1125, 1218, 1125, 2436};
    double fx = 0, fy = 0;
    display_fraction(1125 / 2, 1218 / 2, 1125, 1218, top, fx, fy);
    check(near(fx, 0.5, 0.001) && near(fy, 0.25, 0.001), "上半部视口的正中 -> 屏幕 (0.5,0.25)");

    // 只看右下角那一块
    const Crop corner{562, 1218, 563, 1218, 1125, 2436};
    display_fraction(0, 0, 563, 1218, corner, fx, fy);
    check(near(fx, 0.4996, 0.001) && near(fy, 0.5, 0.001), "右下视口的左上角 -> 屏幕正中附近");
    display_fraction(562, 1217, 563, 1218, corner, fx, fy);
    check(fx > 0.99 && fy > 0.99, "右下视口的右下角 -> 屏幕右下角");
}

/// 窗口必须放得进屏幕，且比例不能变——这次用户报的就是"窗口比例不对"。
void test_fit_window() {
    std::printf("\n== 窗口缩进屏幕 ==\n");
    int w = 0, h = 0;
    // 真机数字：裁剪框 1125x2436，MacBook 屏幕 1680x1050 点
    fit_window(1125, 2436, 1680, 990, 1.0, false, w, h);
    check(h <= 990 && w <= 1680, "放得进屏幕: " + std::to_string(w) + "x" + std::to_string(h));
    const double want = 1125.0 / 2436.0;
    check(std::fabs(static_cast<double>(w) / h - want) < 0.01,
          "比例不变: " + std::to_string(static_cast<double>(w) / h) + " vs " + std::to_string(want));

    // 屏幕够大就不该缩
    fit_window(1125, 2436, 4000, 4000, 1.0, false, w, h);
    check(w == 1125 && h == 2436, "屏幕够大时原样: " + std::to_string(w) + "x" + std::to_string(h));

    // 用户显式给了 --scale 就不能擅自改他的数
    fit_window(1125, 2436, 1680, 990, 0.5, true, w, h);
    check(w == 562 && h == 1218, "--scale 0.5 照收: " + std::to_string(w) + "x" + std::to_string(h));

    // 退化输入不能算出 0 尺寸的窗口（SDL 建不出窗口，症状是"闪退"）
    fit_window(1125, 2436, 0, 0, 1.0, false, w, h);
    check(w >= 1 && h >= 1, "拿不到屏幕尺寸时不缩成 0");
    fit_window(0, 0, 1680, 990, 1.0, false, w, h);
    check(w >= 1 && h >= 1, "裁剪框为 0 也不炸");
}

/// SDL 把窗口坐标换算成逻辑坐标之后剩下的那一半：加裁剪偏移、除以显示尺寸。
void test_logical_to_fraction() {
    std::printf("\n== 逻辑坐标 -> 归一化 ==\n");
    const Crop full{0, 0, 1125, 2436, 1125, 2436};
    double fx = 0, fy = 0;
    display_fraction_from_logical(562.5, 1218, full, fx, fy);
    check(near(fx, 0.5, 1e-3) && near(fy, 0.5, 1e-3), "未裁剪时逻辑正中 -> 屏幕正中");

    // 裁剪框有偏移时，同样的逻辑坐标要算到屏幕的不同位置上——漏了偏移就是"点哪儿都偏"
    const Crop off{100, 200, 1025, 2236, 1125, 2436};
    display_fraction_from_logical(0, 0, off, fx, fy);
    check(near(fx, 100.0 / 1125, 1e-6) && near(fy, 200.0 / 2436, 1e-6),
          "裁剪偏移要加进去");

    const Crop empty{};
    display_fraction_from_logical(10, 10, empty, fx, fy);
    check(std::isfinite(fx) && std::isfinite(fy), "空裁剪框不出 NaN");
}

/// 鼠标 -> 归一化坐标。上一版这里错了：SDL_RenderWindowToLogical 要的是绘制面
/// 像素，而 SDL2 的鼠标事件给的是窗口点，Retina 下差 2 倍，点击整体偏到左上。
void test_window_to_fraction() {
    std::printf("\n== 鼠标坐标 -> 归一化 ==\n");
    const Crop c{0, 0, 1125, 2436, 1125, 2436};
    double fx = 0, fy = 0;

    // Retina：窗口 457x990 点，绘制面 914x1976 像素。
    window_to_fraction(0, 0, 457, 990, 914, 1976, c, fx, fy);
    check(near(fx, 0.0, 0.01) && near(fy, 0.0, 0.01), "左上角 -> (0,0)");
    window_to_fraction(456, 989, 457, 990, 914, 1976, c, fx, fy);
    check(fx > 0.98 && fy > 0.98, "右下角 -> 接近 (1,1)，不被 2 倍缩放砍半");
    window_to_fraction(228, 495, 457, 990, 914, 1976, c, fx, fy);
    check(near(fx, 0.5, 0.02) && near(fy, 0.5, 0.02), "窗口正中 -> 屏幕正中");

    // 非 Retina（1:1）必须给出同样的答案，否则就是哪里混了单位
    window_to_fraction(228, 495, 457, 990, 457, 990, c, fx, fy);
    check(near(fx, 0.5, 0.02) && near(fy, 0.5, 0.02), "1:1 显示器上正中仍是正中");

    // 窗口被拉宽：画面等比居中、左右各留一条边。留边的存在必须反映到换算里，
    // 否则点左边的黑边会被当成画面左边缘（用户看到的就是"点哪儿都往右错一点"）。
    const Crop wide{0, 0, 1125, 2436, 1125, 2436};
    window_to_fraction(600, 495, 1200, 990, 1200, 1976, wide, fx, fy);
    check(near(fx, 0.5, 0.02) && near(fy, 0.5, 0.02), "拉宽后窗口正中依然是画面正中");
    window_to_fraction(20, 495, 1200, 990, 1200, 1976, wide, fx, fy);
    check(fx < 0.0, "点在左侧黑边里要算成负数（出界），而不是被夹到画面左边缘");
    // 留边宽度 = (1200 - 1125*0.811)/2 ≈ 144 像素；画面左边缘应当落在
    // 窗口 x ≈ 144/1200*1200 = 144 点附近，而不是 0。
    window_to_fraction(144, 495, 1200, 990, 1200, 1976, wide, fx, fy);
    check(near(fx, 0.0, 0.03), "画面左边缘对应的窗口位置约在 x=144");

    // 裁剪框带偏移：同一个鼠标位置要算到整块屏幕的不同地方
    const Crop off{100, 200, 1025, 2236, 1125, 2436};
    window_to_fraction(0, 0, 1025, 2236, 1025, 2236, off, fx, fy);
    check(near(fx, 100.0 / 1125, 1e-3) && near(fy, 200.0 / 2436, 1e-3), "裁剪偏移要加进去");
}

void test_degenerate_window() {
    std::printf("\n== 退化输入 ==\n");
    const Crop c{0, 0, 1125, 2436, 1125, 2436};
    double fx = -1, fy = -1;
    display_fraction(10, 10, 0, 0, c, fx, fy);
    check(std::isfinite(fx) && std::isfinite(fy), "窗口尺寸为 0 不炸也不出 NaN");
    const Crop none{};
    display_fraction(10, 10, 100, 100, none, fx, fy);
    check(std::isfinite(fx) && std::isfinite(fy), "全零的裁剪框也不出 NaN");
}

}  // namespace

int main() {
    test_full_display();
    test_ctu_padding_not_in_the_denominator();
    test_fit_window();
    test_window_to_fraction();
    test_logical_to_fraction();
    test_cropped_viewport();
    test_degenerate_window();
    std::printf("\n%s (失败 %d 项)\n", Failures == 0 ? "全部通过" : "存在失败", Failures);
    return Failures == 0 ? 0 : 1;
}
