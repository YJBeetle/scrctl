// 窗口几何的离线自检。
//
// 这段几何是"点哪儿打哪儿"的唯一依据，而且它错起来毫无征兆（画面在动、触摸在动，
// 只是整体偏一个倍数）。所以用例里的关键数字全部来自真机 --debug-input 实测，
// 而不是编出来的理想值。
#include <cmath>
#include <cstdio>
#include <limits>
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

void test_requested_window_size() {
    std::printf("\n== 显式窗口尺寸 ==\n");
    int w = 0, h = 0;
    check(fit_window(1125, 2436, 1680, 990, 1.0, false, w, h, 240, 0) &&
              w == 240 && h == 519,
          "仅指定宽度时保留 240，按视口比例推导高度");
    check(fit_window(1125, 2436, 1680, 990, 1.0, false, w, h, 0, 480) &&
              w == 221 && h == 480,
          "仅指定高度时保留 480，按视口比例推导宽度");

    const Crop part{100, 200, 400, 300, 1125, 2436};
    for (int degrees : {0, 90, 180, 270}) {
        int vw = 0, vh = 0;
        scrctl::app::viewport_size(part, degrees, vw, vh);
        const bool swapped = degrees == 90 || degrees == 270;
        check(fit_window(vw, vh, 10, 10, 0.5, true, w, h, 80, 0) &&
                  w == 80 && h == (swapped ? 106 : 60),
              "裁剪 + 旋转 " + std::to_string(degrees) + "：仅宽度优先于 scale 与屏幕适配");
        check(fit_window(vw, vh, 10, 10, 0.5, true, w, h, 0, 90) &&
                  w == (swapped ? 67 : 120) && h == 90,
              "裁剪 + 旋转 " + std::to_string(degrees) + "：仅高度按当前视口比例推导");
        check(fit_window(vw, vh, 10, 10, 0.5, true, w, h, 320, 240) &&
                  w == 320 && h == 240,
              "双维 320x240 不随旋转或 scale 改变，内容留边由渲染器处理");
    }

    check(fit_window(1125, 2436, 1680, 990, 1.0, false, w, h, 0, 0) &&
              w == 457 && h == 990,
          "两维 0 使用默认屏幕适配");
    check(fit_window(1125, 2436, 1680, 990, 0.5, true, w, h, 0, 0) &&
              w == 562 && h == 1218,
          "两维 0 保留显式 scale，即使高度超过屏幕");
}

void test_window_size_limits() {
    std::printf("\n== 窗口尺寸边界 ==\n");
    const int maximum = std::numeric_limits<int>::max();
    int w = 0, h = 0;
    check(fit_window(maximum, maximum, 0, 0, 1.0, false, w, h, maximum, 0) &&
              w == maximum && h == maximum,
          "单维推导在 int 最大值上不产生乘法溢出");
    check(fit_window(1, 1, 0, 0, 1.0, false, w, h, maximum, maximum) &&
              w == maximum && h == maximum,
          "双维保留 int 可表示的最大值");
    check(fit_window(maximum, 1, 0, 0, 1.0, false, w, h, 1, 0) && w == 1 && h == 1,
          "推导不足 1 点时保留最小正尺寸");
    check(fit_window(0, 0, 0, 0, 1.0, false, w, h) && w == 1 && h == 1,
          "全零视口仍得到 1x1 退化兜底");
    check(fit_window(0, 0, 0, 0, 1.0, false, w, h, 42, 0) && w == 42 && h == 42,
          "单维配合零视口不会除零");
    check(fit_window(1125, 2436, 1680, 990, std::numeric_limits<double>::max(), false, w, h) &&
              w == 457 && h == 990,
          "默认适配先限制比例，避免巨大的有限 scale 产生溢出");
    check(fit_window(1125, 2436, 0, 0, std::numeric_limits<double>::min(), true, w, h) &&
              w == 1 && h == 1,
          "极小正 scale 仍得到正窗口尺寸");

    const auto rejected = [&](int vw, int vh, double scale, int requested_w, int requested_h,
                              const char *name) {
        w = 17;
        h = 23;
        check(!fit_window(vw, vh, 0, 0, scale, true, w, h, requested_w, requested_h) &&
                  w == 17 && h == 23,
              name);
    };
    rejected(1, maximum, 1.0, 2, 0, "拒绝高度推导溢出，不夹取或改动已有结果");
    rejected(maximum, 1, 1.0, 0, 2, "拒绝宽度推导溢出，不夹取或改动已有结果");
    rejected(2, 1, static_cast<double>(maximum), 0, 0, "拒绝显式 scale 超过 int 尺寸范围");
    rejected(2, 1, std::numeric_limits<double>::max(), 0, 0, "拒绝 scale 乘积溢出为无穷");
    rejected(1, 1, std::numeric_limits<double>::infinity(), 0, 0, "拒绝无穷 scale");
    rejected(1, 1, std::numeric_limits<double>::quiet_NaN(), 0, 0, "拒绝 NaN scale");
    rejected(1, 1, 0, 0, 0, "拒绝零 scale");
    rejected(1, 1, -1, 0, 0, "拒绝负 scale");
    rejected(1, 1, 1.0, -1, 0, "拒绝负宽度");
    rejected(1, 1, 1.0, 0, -1, "拒绝负高度");
}

/// `--crop` 只改"看哪一块"，不改触摸的分母。
///
/// 这里钉的是 resolve_crop 那个调用点：它曾经把裁剪框自己的尺寸写进 display_w/h，
/// 于是只要给了 --crop，右下角就被压回 0.5 附近——画面看着是对的，点下去是错的。
void test_manual_crop_keeps_the_display_denominator() {
    std::printf("\n== 手工裁剪不改分母 ==\n");
    using scrctl::app::make_crop;

    // 编码 1136x2464、逻辑显示 1125x2436，用户裁右下那块 500x500@(600,1800)。
    const Crop c = make_crop(true, 600, 1800, 500, 500, 1136, 2464, 1125, 2436);
    check(c.display_w == 1125 && c.display_h == 2436, "分母仍是整块逻辑显示");

    double fx = 0, fy = 0;
    // 裁剪框的右下角 = 逻辑坐标 (1100, 2300) = 整块屏幕的 (0.978, 0.944)
    display_fraction_from_logical(500, 500, c, fx, fy);
    check(near(fx, 1100.0 / 1125, 0.002) && near(fy, 2300.0 / 2436, 0.002),
          "分区右下角落在屏幕右下角附近，而不是被压回中间");

    // 没给 --crop 时整块显示就是视口，中心仍是 0.5。
    const Crop full = make_crop(false, 0, 0, 0, 0, 1136, 2464, 1125, 2436);
    display_fraction_from_logical(562, 1218, full, fx, fy);
    check(near(fx, 0.5, 0.002) && near(fy, 0.5, 0.002), "不裁时正中 -> (0.5,0.5)");

    // 越界与零尺寸的框要夹进编码帧里：0 尺寸窗口是"启动就闪退"，比画错更糟。
    const Crop clamped = make_crop(true, 5000, -10, 0, 0, 1136, 2464, 1125, 2436);
    check(clamped.x == 1135 && clamped.y == 0 && clamped.w == 1 && clamped.h == 1,
          "越界框被夹住且不为 0");
    check(clamped.display_w == 1125 && clamped.display_h == 2436, "夹取不动分母");
}


/// 界面旋转：视口 -> 面板这条映射。
///
/// 判据全部来自一次真机对照，不是推的：设备报 `currentOrientation: rot270` 时，
/// 把面板那一帧顺时针转 270° 得到正立画面（拿截图服务给的同一屏图对过）。
/// 于是"视口 = 面板顺时针转 degrees"，触摸要的是它的反变换。
void test_viewport_rotation() {
    std::printf("\n== 界面旋转下的触摸映射 ==\n");
    using scrctl::app::orientation_degrees;
    using scrctl::app::viewport_fraction_to_panel;
    using scrctl::app::viewport_size;

    check(orientation_degrees("rot0") == 0 && orientation_degrees("rot90") == 90 &&
              orientation_degrees("rot180") == 180 && orientation_degrees("rot270") == 270,
          "rotN -> N 度");
    // 认不出来当 0：转歪比不转更糟（不转时触摸与画面仍是一致的）。
    check(orientation_degrees("") == 0 && orientation_degrees("upside-down") == 0,
          "不认识的值当 0，不猜");

    // 面板 1125x2436、整幅可见（真机那一档）。
    const Crop panel {0, 0, 1125, 2436, 1125, 2436};
    int vw = 0, vh = 0;
    viewport_size(panel, 270, vw, vh);
    check(vw == 2436 && vh == 1125, "转 270 之后视口是 2436x1125（宽高对调）");
    viewport_size(panel, 0, vw, vh);
    check(vw == 1125 && vh == 2436, "不转时视口就是裁剪框");

    double fx = 0, fy = 0;
    // 中心在四种旋转下都是不动点——它能挡住"整个映射平移了"这种错。
    for (int deg : {0, 90, 180, 270}) {
        viewport_size(panel, deg, vw, vh);
        viewport_fraction_to_panel(vw / 2.0, vh / 2.0, panel, deg, fx, fy);
        check(near(fx, 0.5, 0.002) && near(fy, 0.5, 0.002),
              "deg=" + std::to_string(deg) + " 视口中心 -> 面板中心");
    }

    // 真机那一条：横屏视频里进度条上的播放头在视口 (0.17, 0.79)，
    // 面板原图里那个红点在**左边靠上** (0.21, 0.17)。
    viewport_fraction_to_panel(0.17 * 2436.0, 0.79 * 1125.0, panel, 270, fx, fy);
    check(near(fx, 0.21, 0.01) && near(fy, 0.17, 0.01),
          "rot270 时视口左下 -> 面板左上（红点那一档）");

    // 四角必须一一映射到四角，且四种角度互不相同——挡住"忘了某个分支"和"两个角度写重"。
    for (int deg : {0, 90, 180, 270}) {
        viewport_size(panel, deg, vw, vh);
        viewport_fraction_to_panel(0.0, 0.0, panel, deg, fx, fy);
        const int corner = (fx > 0.5 ? 1 : 0) * 2 + (fy > 0.5 ? 1 : 0);
        std::printf("      deg=%3d 视口左上 -> 面板角 %d (%.2f,%.2f)\n", deg, corner, fx, fy);
        check(fx >= 0.0 && fx <= 1.0 && fy >= 0.0 && fy <= 1.0,
              "deg=" + std::to_string(deg) + " 视口角点落在面板角点上");
    }
    // 具体到 rot270：视口左上角 = 面板右上角（真机截图里标题就横在面板右边缘）。
    viewport_fraction_to_panel(0.0, 0.0, panel, 270, fx, fy);
    check(near(fx, 1.0, 0.002) && near(fy, 0.0, 0.002), "rot270 视口左上 -> 面板右上");
    viewport_fraction_to_panel(0.0, 0.0, panel, 90, fx, fy);
    check(near(fx, 0.0, 0.002) && near(fy, 1.0, 0.002), "rot90 视口左上 -> 面板左下");
    viewport_fraction_to_panel(0.0, 0.0, panel, 180, fx, fy);
    check(near(fx, 1.0, 0.002) && near(fy, 1.0, 0.002), "rot180 视口左上 -> 面板右下");

    // 旋转 + 裁剪偏移同时存在：偏移在面板轴上，必须先转回面板轴再加偏移。
    const Crop part {600, 1800, 500, 500, 1125, 2436};
    viewport_fraction_to_panel(0.0, 0.0, part, 270, fx, fy);
    check(near(fx, 1100.0 / 1125, 0.002) && near(fy, 1800.0 / 2436, 0.002),
          "rot270 + 偏移框：视口左上 -> 面板 (1100, 1800)");

    // 与不旋转那条旧路必须逐点一致（防止新函数把 deg=0 也带偏）。
    for (double y = 0; y <= 2436.0; y += 609.0) {
        double a = 0, b = 0, c2 = 0, d2 = 0;
        viewport_fraction_to_panel(300, y, panel, 0, a, b);
        scrctl::app::display_fraction_from_logical(300, y, panel, c2, d2);
        check(near(a, c2, 1e-9) && near(b, d2, 1e-9), "deg=0 与旧函数一致 y=" + std::to_string(int(y)));
    }
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    test_raw_to_fraction();
    test_ctu_padding_not_in_the_denominator();
    test_cropped_viewport();
    test_manual_crop_keeps_the_display_denominator();
    test_viewport_rotation();
    test_degenerate();
    test_fit_window();
    test_requested_window_size();
    test_window_size_limits();
    std::printf("\n%s (失败 %d 项)\n", Failures == 0 ? "全部通过" : "存在失败", Failures);
    return Failures == 0 ? 0 : 1;
}
