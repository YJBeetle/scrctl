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
    test_cropped_viewport();
    test_degenerate_window();
    std::printf("\n%s (失败 %d 项)\n", Failures == 0 ? "全部通过" : "存在失败", Failures);
    return Failures == 0 ? 0 : 1;
}
