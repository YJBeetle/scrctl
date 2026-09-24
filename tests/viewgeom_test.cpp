// 窗口几何的离线自检。
//
// 这段几何是"点哪儿打哪儿"的唯一依据，而且它错起来毫无征兆（画面在动、触摸在动，
// 只是整体偏一个倍数）。所以用例里的关键数字全部来自真机 --debug-input 实测，
// 而不是编出来的理想值。
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

bool near(double v, double want, double tol = 1e-6) { return std::fabs(v - want) < tol; }

using scrctl::app::Crop;
using scrctl::app::display_fraction_from_logical;
using scrctl::app::fit_window;

/// 真机实测：窗口 457x988 点 / 绘制面 914x1976 像素 / 逻辑 1125x2436。
/// 用户沿窗口四角描边，原始坐标 x 走 9..1208、y 走 15..2434 —— y 几乎铺满逻辑
/// 高度，证明 SDL 在设了 logical size 之后给的就是逻辑坐标。
void test_raw_to_fraction() {
    std::printf("\n== 原始坐标 -> 归一化（真机实测值）==\n");
    const Crop c{0, 0, 1125, 2436, 1125, 2436};
    double fx = 0, fy = 0;

    // 实测右下角
    display_fraction_from_logical(1121, 2431, c, fx, fy);
    check(near(fx, 0.996, 0.002) && near(fy, 0.998, 0.002), "右下角 (1121,2431) -> (0.996,0.998)");
    // 回归：这里曾经多除一次"窗口点数/绘制面像素"，右下角被算成 (2.456, 2.461)
    check(fx < 1.05 && fy < 1.05, "不再被放大 2.46 倍");

    // 实测左上角附近
    display_fraction_from_logical(9, 15, c, fx, fy);
    check(fx < 0.01 && fy < 0.01, "左上角 (9,15) -> 接近 (0,0)");

    // 描边时略微超出窗口右侧（实测到 1208 > 1125）：要如实报成 >1.0，
    // 而不是被折回画面里（折回去就是"点边缘却打在中间"）
    display_fraction_from_logical(1208, 1780, c, fx, fy);
    check(fx > 1.0 && fx < 1.1 && near(fy, 1780.0 / 2436, 0.002), "略微出界要如实报成 >1.0");

    display_fraction_from_logical(562, 1218, c, fx, fy);
    check(near(fx, 0.5, 0.002) && near(fy, 0.5, 0.002), "画面正中 -> (0.5,0.5)");
}

/// 真机那对数字：编码 1136x2464、逻辑显示 1125x2436。
/// 分母必须是显示尺寸而不是编码帧尺寸，否则右下角只能摸到 0.989。
void test_ctu_padding_not_in_the_denominator() {
    std::printf("\n== 分母是逻辑显示，不是编码帧 ==\n");
    const Crop c{0, 0, 1125, 2436, 1125, 2436};
    double fx = 0, fy = 0;
    display_fraction_from_logical(1124, 2435, c, fx, fy);
    check(fx > 0.99 && fy > 0.99, "右下角不被 CTU 填充吃掉");
    check(near(1124.0 / 1136, 0.989, 0.002), "反证：拿 1136 当分母就只有 0.989");
}

/// 裁了显示区的一块：视口正中应落在整块屏幕的偏上位置，而不是 0.5。
void test_cropped_viewport() {
    std::printf("\n== 裁剪过的视口 ==\n");
    const Crop top{0, 0, 1125, 1218, 1125, 2436};
    double fx = 0, fy = 0;
    display_fraction_from_logical(562, 609, top, fx, fy);
    check(near(fx, 0.5, 0.002) && near(fy, 0.25, 0.002), "上半部视口的正中 -> 屏幕 (0.5,0.25)");

    const Crop corner{562, 1218, 563, 1218, 1125, 2436};
    display_fraction_from_logical(0, 0, corner, fx, fy);
    check(near(fx, 0.4996, 0.002) && near(fy, 0.5, 0.002), "右下视口的左上角 -> 屏幕正中附近");
    display_fraction_from_logical(562, 1217, corner, fx, fy);
    check(fx > 0.99 && fy > 0.99, "右下视口的右下角 -> 屏幕右下角");
}

void test_degenerate() {
    std::printf("\n== 退化输入 ==\n");
    double fx = 0, fy = 0;
    const Crop none{};
    display_fraction_from_logical(10, 10, none, fx, fy);
    check(std::isfinite(fx) && std::isfinite(fy), "全零的裁剪框也不出 NaN");

    int w = 0, h = 0;
    fit_window(1125, 2436, 0, 0, 1.0, false, w, h);
    check(w >= 1 && h >= 1, "拿不到屏幕尺寸时不缩成 0");
    fit_window(0, 0, 1680, 990, 1.0, false, w, h);
    check(w >= 1 && h >= 1, "裁剪框为 0 也不炸");
}

/// 窗口必须放得进屏幕，且比例不能变 —— 用户报"窗口比例不对"就是这里。
void test_fit_window() {
    std::printf("\n== 窗口缩进屏幕 ==\n");
    int w = 0, h = 0;
    fit_window(1125, 2436, 1680, 990, 1.0, false, w, h);
    check(h <= 990 && w <= 1680, "放得进屏幕: " + std::to_string(w) + "x" + std::to_string(h));
    const double want = 1125.0 / 2436.0;
    check(near(static_cast<double>(w) / h, want, 0.01), "比例不变");

    fit_window(1125, 2436, 4000, 4000, 1.0, false, w, h);
    check(w == 1125 && h == 2436, "屏幕够大时原样");

    fit_window(1125, 2436, 1680, 990, 0.5, true, w, h);
    check(w == 562 && h == 1218, "--scale 0.5 照收，不擅自改");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    test_raw_to_fraction();
    test_ctu_padding_not_in_the_denominator();
    test_cropped_viewport();
    test_degenerate();
    test_fit_window();
    std::printf("\n%s (失败 %d 项)\n", Failures == 0 ? "全部通过" : "存在失败", Failures);
    return Failures == 0 ? 0 : 1;
}
