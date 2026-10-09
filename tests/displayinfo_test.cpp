// `displayinfoupdates` 解场的离线自检。
//
// 为什么必须有：产品路径拿这条推送替掉一张硬编码的裁剪表，而"裁错了"的真机表现是
// 边缘点不准 + 右边/下边一条垃圾边——从日志上看不出是**字段解错了**还是映射算错了。
// 所以这一层必须能离开设备单独判。
//
// 夹具的**键名与结构**照真机抓来的那一条搭（iPhone14,4 / iOS 27.0，
// display_info_probe 打的整条推送）。两处诚实交代：
// 1. 每个字段的 **XPC 类型**照实测填（几何是 Double、displayId 是 UInt64、
//    preferredUIScale 是 Int64）——类型是用 display_info_probe 打出来的，不是猜的。
//    第一版只按整数取值，于是尺寸全军覆没，而 describe 打出来 1125 与 1125.0 同形。
// 2. 产品路径不读的字段（bounds / frame / availableModes / physicalSize）在这里留空
//    数组，它们的作用只是"确实存在但解析器不去碰"。
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "remote/DisplayInfo.h"
#include "xpc/XpcValue.h"

namespace {

int Failures = 0;
int Checks = 0;

void check(bool ok, const std::string &what) {
    ++Checks;
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) {
        ++Failures;
    }
}

using namespace scrctl::xpc;

/// `[w, h]`。**实测这些几何值是 Double 不是整数**（CG 的 CGFloat），所以夹具默认走
/// Double——这正是第一版解析器栽的地方：按整数取，`size` 一个都拿不到。
Value pair(int w, int h) {
    auto a = make_array();
    array_push(a, make_double(static_cast<double>(w)));
    array_push(a, make_double(static_cast<double>(h)));
    return a;
}

/// 同一对值以整数形式再摆一次，用来验"三种类型都认"。
Value ipair(int w, int h) {
    auto a = make_array();
    array_push(a, make_int64(w));
    array_push(a, make_uint64(static_cast<uint64_t>(h)));
    return a;
}

/// 只给公开解析入口构造一条记录；边界测试不访问内部转换函数。
Value numeric_element(Value id, Value width, Value height) {
    auto size = make_array();
    array_push(size, std::move(width));
    array_push(size, std::move(height));
    auto current = make_dict();
    dict_set(current, "size", std::move(size));
    auto display = make_dict();
    dict_set(display, "displayId", std::move(id));
    dict_set(display, "primary", make_bool(true));
    dict_set(display, "currentMode", std::move(current));
    auto displays = make_array();
    array_push(displays, std::move(display));
    auto element = make_dict();
    dict_set(element, "displays", std::move(displays));
    return element;
}

Value mode(int w, int h, int ui_scale, int refresh) {
    auto m = make_dict();
    dict_set(m, "preferredUIScale", make_int64(ui_scale));  // 实测 Int64
    dict_set(m, "hdrMode", make_string("standard"));
    dict_set(m, "bitDepth", make_int64(8));  // 实测 Int64
    dict_set(m, "colorGamut", make_string("displayP3"));
    dict_set(m, "refreshRate", make_double(static_cast<double>(refresh)));  // 实测 Double
    dict_set(m, "size", pair(w, h));
    return m;
}

/// 真机那条主屏记录：面板 nativeSize 是 1080x2340，而**可见区在 currentMode.size**。
/// 这两个数不相等正是最容易认错的地方，所以两个都放进夹具。
Value primary_display() {
    auto d = make_dict();
    dict_set(d, "external", make_bool(false));
    dict_set(d, "primary", make_bool(true));
    dict_set(d, "framebufferMaskIdentifier",
             make_string("51C7C897-7BB5-4A78-A961-D382E90D9C5F"));
    dict_set(d, "deviceName", make_string("primary"));
    dict_set(d, "nativeSize", pair(1080, 2340));
    dict_set(d, "currentMode", mode(1125, 2436, 3, 60));
    dict_set(d, "physicalSize", make_array());
    dict_set(d, "nativeOrientation", make_string("rot0"));
    auto type = make_dict();
    dict_set(type, "integrated", make_dict());
    dict_set(d, "type", std::move(type));
    dict_set(d, "currentOrientation", make_string("rot0"));
    dict_set(d, "name", make_string("LCD"));
    dict_set(d, "displayId", make_uint64(1));  // 实测 UInt64
    dict_set(d, "bounds", make_array());
    dict_set(d, "chromeIdentifier", make_string("com.apple.dt.devicekit.chrome.phone3"));
    dict_set(d, "logicalScale", pair(1, 1));
    dict_set(d, "frame", make_array());
    dict_set(d, "availableModes", make_array());
    dict_set(d, "pointScale", make_uint64(3));
    return d;
}

Value wireless_display(uint64_t id) {
    auto d = make_dict();
    dict_set(d, "external", make_bool(true));
    dict_set(d, "primary", make_bool(false));
    dict_set(d, "deviceName", make_string("wireless0"));
    dict_set(d, "nativeSize", pair(1136, 2448));
    dict_set(d, "currentMode", mode(1136, 2448, 2, 60));
    dict_set(d, "currentOrientation", make_string("rot0"));
    dict_set(d, "name", make_string("Wireless"));
    dict_set(d, "displayId", make_uint64(id));
    dict_set(d, "pointScale", make_uint64(1));
    return d;
}

/// 真机一次订阅推过来的整条 element 的形状（值全部来自那次实测）。
Value real_element() {
    auto displays = make_array();
    array_push(displays, primary_display());
    array_push(displays, wireless_display(2));
    // 外接屏在这一台设备上一直是"在册但没插"：尺寸全零。它必须能解析出来而不出错，
    // 只是不能拿它的尺寸当可见区。
    auto dead = make_dict();
    dict_set(dead, "external", make_bool(true));
    dict_set(dead, "primary", make_bool(false));
    dict_set(dead, "currentMode", mode(0, 0, 1, 0));
    dict_set(dead, "displayId", make_uint64(3));
    array_push(displays, std::move(dead));
    // 设备还会塞一条没有 displayId 的（实测里 orientation 那一层之外的杂项），
    // 没有 id 就无从对应，必须跳过而不是把整包判死。
    auto anonymous = make_dict();
    dict_set(anonymous, "name", make_string("no-id"));
    array_push(displays, std::move(anonymous));

    auto orientation = make_dict();
    dict_set(orientation, "currentDeviceOrientationLocked", make_bool(false));
    dict_set(orientation, "currentDeviceNonFlatOrientation", make_string("portrait"));
    dict_set(orientation, "currentDeviceOrientation", make_string("portrait"));

    auto e = make_dict();
    dict_set(e, "current", make_bool(true));
    dict_set(e, "orientation", std::move(orientation));
    dict_set(e, "displays", std::move(displays));
    dict_set(e, "backlightState", make_string("activeOn"));
    return e;
}

}  // namespace

int main() {
    using scrctl::remote::parse_display_info;

    std::printf("== 真机抓来的那一条 ==\n");
    std::string err;
    const auto info = parse_display_info(real_element(), err);
    check(info != std::nullopt, "整条解得开（" + err + "）");
    if (info == std::nullopt) {
        std::printf("\n存在失败（解不开，后面无法继续）\n");
        return 1;
    }
    // 没有 displayId 的那条被跳过，其余三条都留下。
    check(info->displays.size() == 3, "displays 三条（无 id 的那条被跳过）");
    check(info->device_orientation == "portrait", "设备朝向 portrait");

    const auto *p = info->primary();
    check(p != nullptr && p->id == 1, "primary() 找到 1 号屏");
    if (p != nullptr) {
        // 这一条是整个替换的目的：可见区来自 currentMode.size，不是 nativeSize。
        check(p->width == 1125 && p->height == 2436,
              "主屏可见区 1125x2436（不是 nativeSize 的 1080x2340）");
        check(p->name == "LCD" && p->orientation == "rot0", "名字与自身朝向");
        check(!p->external, "主屏不是外接");
    }
    const auto *w = info->find(2);
    check(w != nullptr && w->width == 1136 && w->height == 2448, "按 id 能找到无线屏");
    check(w != nullptr && w->external, "无线屏标成外接");
    const auto *dead = info->find(3);
    check(dead != nullptr && dead->width == 0 && dead->height == 0,
          "在册没插的外接屏解出零尺寸（不是报错）");
    check(info->find(99) == nullptr, "没有的 id 返回 nullptr");

    std::printf("\n== 数值字段：Double 才是实测形状，整数是兜底 ==\n");
    {
        auto one_size = [](Value size) {
            auto displays = make_array();
            auto d = make_dict();
            dict_set(d, "primary", make_bool(true));
            dict_set(d, "displayId", make_uint64(7));
            dict_set(d, "name", make_string("X"));
            auto m = make_dict();
            dict_set(m, "size", std::move(size));
            dict_set(d, "currentMode", std::move(m));
            array_push(displays, std::move(d));
            auto e = make_dict();
            dict_set(e, "displays", std::move(displays));
            std::string e2;
            const auto got = parse_display_info(e, e2);
            const auto *f = got == std::nullopt ? nullptr : got->find(7);
            return f != nullptr && f->width == 500 && f->height == 900;
        };
        // 这一档钉的是本轮真机上的事故：几何字段其实全是 Double（CG 的 CGFloat），
        // 而第一版解析器只认整数，于是"问到设备了"却拿到 0x0，静默退回兜底表——
        // 日志上看不出原因，因为 describe 打出来 1125 和 1125.0 长得一模一样。
        auto dbl = make_array();
        array_push(dbl, make_double(500.0));
        array_push(dbl, make_double(900.0));
        check(one_size(std::move(dbl)), "size 是 Double（实测形状）时解得出 500x900");

        check(one_size(ipair(500, 900)), "size 是 int64 / uint64 时也解得出");

        // 浮点要四舍五入而不是截断：这一路是从 CG 的浮点几何换算来的。
        auto jitter = make_array();
        array_push(jitter, make_double(499.9999));
        array_push(jitter, make_double(900.0002));
        check(one_size(std::move(jitter)), "499.9999 / 900.0002 落到 500 x 900");

        // displayId 实测是 UInt64；Int64 也认，理由同上——猜错类型是静默失败。
        auto displays = make_array();
        auto d = make_dict();
        dict_set(d, "primary", make_bool(true));
        dict_set(d, "displayId", make_int64(7));
        dict_set(d, "currentMode", mode(500, 900, 2, 60));
        array_push(displays, std::move(d));
        auto e = make_dict();
        dict_set(e, "displays", std::move(displays));
        std::string e3;
        const auto got = parse_display_info(e, e3);
        check(got != std::nullopt && got->find(7) != nullptr, "displayId 是 int64 时也认");
    }

    std::printf("\n== 数字转换有界且保持最近整点 ==\n");
    {
        const auto check_size = [](Value width, Value height, int expected_width,
                                   int expected_height, const std::string &label) {
            std::string error;
            const auto got = parse_display_info(
                numeric_element(make_uint64(7), std::move(width), std::move(height)), error);
            const auto *display = got ? got->find(7) : nullptr;
            check(display != nullptr && got->displays.size() == 1 &&
                      display->width == expected_width && display->height == expected_height,
                  label + (display ? " (actual " + std::to_string(display->width) + "x" +
                                     std::to_string(display->height) + ")" : " (missing display)"));
        };
        const auto check_id = [](Value id, uint64_t expected, const std::string &label) {
            std::string error;
            const auto got = parse_display_info(
                numeric_element(std::move(id), make_int64(500), make_int64(900)), error);
            check(got != std::nullopt && got->displays.size() == 1 &&
                      got->find(expected) != nullptr, label);
        };
        const auto reject_id = [](Value id, const std::string &label) {
            std::string error;
            const auto got = parse_display_info(
                numeric_element(std::move(id), make_int64(500), make_int64(900)), error);
            check(got == std::nullopt && !error.empty(), label);
        };
        constexpr int minimum_int = std::numeric_limits<int>::min();
        constexpr int maximum_int = std::numeric_limits<int>::max();
        constexpr int64_t minimum_i64 = std::numeric_limits<int64_t>::min();
        constexpr int64_t maximum_i64 = std::numeric_limits<int64_t>::max();
        constexpr uint64_t maximum_u64 = std::numeric_limits<uint64_t>::max();
        const double upper_i64 = std::ldexp(1.0, std::numeric_limits<int64_t>::digits);
        const double lower_i64 = static_cast<double>(minimum_i64);
        const double infinity = std::numeric_limits<double>::infinity();
        const double nan = std::numeric_limits<double>::quiet_NaN();

        check_size(make_int64(minimum_int), make_uint64(static_cast<uint64_t>(maximum_int)),
                   minimum_int, maximum_int, "int 两个边界不被缩窄或夹紧（负尺寸仍由上层筛选）");
        check_size(make_double(static_cast<double>(maximum_int)),
                   make_double(static_cast<double>(minimum_int)), maximum_int, minimum_int,
                   "Double 精确 int 上下界保持原值");
        check_size(make_double(static_cast<double>(maximum_int) + 0.49),
                   make_double(static_cast<double>(minimum_int) - 0.49), maximum_int, minimum_int,
                   "Double 靠近 int 边界但最近整点仍在范围内");
        check_size(make_double(499.5), make_double(900.49), 500, 900,
                   "正半点远离零，半点以下取最近整点");
        check_size(make_double(-499.5), make_double(-900.49), -500, -900,
                   "负半点远离零，负半点以下取最近整点");
        check_size(make_double(-0.49), make_double(-0.5), 0, -1,
                   "负零附近维持原来的最近整点语义");

        struct InvalidNumber { const char *name; Value value; };
        const std::vector<InvalidNumber> invalid_sizes{
            {"NaN", make_double(nan)},
            {"+Inf", make_double(infinity)},
            {"-Inf", make_double(-infinity)},
            {"+DBL_MAX", make_double(std::numeric_limits<double>::max())},
            {"-DBL_MAX", make_double(-std::numeric_limits<double>::max())},
            {"Double 2^63", make_double(upper_i64)},
            {"Double低于LLONG_MIN", make_double(std::nextafter(lower_i64, -infinity))},
            {"Double小于2^63但超int", make_double(std::nextafter(upper_i64, 0.0))},
            {"UInt64 LLONG_MAX+1", make_uint64(static_cast<uint64_t>(maximum_i64) + 1)},
            {"UInt64最大值", make_uint64(maximum_u64)},
            {"Int64最大值", make_int64(maximum_i64)},
            {"Int64最小值", make_int64(minimum_i64)},
            {"Int64 INT_MAX+1", make_int64(static_cast<int64_t>(maximum_int) + 1)},
            {"Int64 INT_MIN-1", make_int64(static_cast<int64_t>(minimum_int) - 1)},
            {"Int64 2^32+500不得绕成500", make_int64((int64_t{1} << 32) + 500)},
            {"Int64 -2^32+500不得绕成500", make_int64(-(int64_t{1} << 32) + 500)},
            {"UInt64 INT_MAX+1", make_uint64(static_cast<uint64_t>(maximum_int) + 1)},
            {"Double INT_MAX+0.5", make_double(static_cast<double>(maximum_int) + 0.5)},
            {"Double INT_MIN-0.5", make_double(static_cast<double>(minimum_int) - 0.5)},
        };
        for (const auto &invalid : invalid_sizes) {
            check_size(invalid.value, make_int64(900), 0, 0,
                       std::string(invalid.name) + "：非法宽度整体保留0x0");
            check_size(make_int64(500), invalid.value, 0, 0,
                       std::string(invalid.name) + "：非法高度不留下部分宽度");
        }

        check_id(make_int64(maximum_i64), static_cast<uint64_t>(maximum_i64),
                 "displayId Int64最大值精确保留");
        check_id(make_uint64(static_cast<uint64_t>(maximum_i64)),
                 static_cast<uint64_t>(maximum_i64), "displayId UInt64恰为LLONG_MAX仍接受");
        // 2^63 下一个 Double 是 2^63-1024，不能把 LLONG_MAX 转 Double 作为闭上界。
        check_id(make_double(std::nextafter(upper_i64, 0.0)),
                 static_cast<uint64_t>(maximum_i64) - 1023, "displayId接受2^63之前的可表示Double");
        check_id(make_double(1.5), 2, "displayId Double正半点保持最近整点");
        check_id(make_double(-0.49), 0, "displayId Double负零附近仍落到0");
        reject_id(make_double(nan), "displayId NaN跳过，且不会浮点转整数越界");
        reject_id(make_double(infinity), "displayId +Inf跳过");
        reject_id(make_double(-infinity), "displayId -Inf跳过");
        reject_id(make_double(std::numeric_limits<double>::max()), "displayId DBL_MAX跳过");
        reject_id(make_double(upper_i64), "displayId Double 2^63排他上界跳过");
        reject_id(make_double(static_cast<double>(maximum_i64)),
                  "displayId LLONG_MAX向Double取整后为2^63，不能错误接受");
        reject_id(make_double(std::nextafter(lower_i64, -infinity)),
                  "displayId低于LLONG_MIN的Double跳过");
        reject_id(make_double(lower_i64), "displayId恰为LLONG_MIN安全转换后因负值跳过");
        reject_id(make_int64(minimum_i64), "displayId Int64最小值因负值跳过");
        reject_id(make_uint64(static_cast<uint64_t>(maximum_i64) + 1),
                  "displayId UInt64超LLONG_MAX明确跳过");
        reject_id(make_uint64(maximum_u64), "displayId UInt64最大值明确跳过");
        reject_id(make_double(-0.5), "displayId负半点取-1后仍按原规则跳过");

        auto mixed = numeric_element(make_double(nan), make_int64(500), make_int64(900));
        auto mixed_list = mixed.at("displays");
        array_push(mixed_list, primary_display());
        dict_set(mixed, "displays", std::move(mixed_list));
        std::string mixed_error;
        const auto mixed_info = parse_display_info(mixed, mixed_error);
        check(mixed_info && mixed_info->displays.size() == 1 && mixed_info->find(1) != nullptr,
              "非法数值id仅丢弃自身，不让同推送的合法屏幕失效");
    }

    std::printf("\n== 解不开的时候要如实失败 ==\n");
    {
        std::string e;
        const auto empty = parse_display_info(make_dict(), e);
        check(empty == std::nullopt && !e.empty(), "没有 displays 键 -> 失败并给原因");

        auto no_id = make_dict();
        auto displays = make_array();
        auto d = make_dict();
        dict_set(d, "name", make_string("只有名字"));
        array_push(displays, std::move(d));
        dict_set(no_id, "displays", std::move(displays));
        std::string e2;
        check(parse_display_info(no_id, e2) == std::nullopt, "所有条目都没有 displayId -> 失败");

        // 有条目、但当前模式缺 size：这一条**不能**失败（设备给得出 id 就给得出别的），
        // 尺寸留零让上层退回旧行为。失败与"没这个字段"要分得开。
        auto no_mode = make_dict();
        auto list2 = make_array();
        auto one = make_dict();
        dict_set(one, "displayId", make_uint64(1));
        dict_set(one, "primary", make_bool(true));
        array_push(list2, std::move(one));
        dict_set(no_mode, "displays", std::move(list2));
        std::string e3;
        const auto partial = parse_display_info(no_mode, e3);
        check(partial != std::nullopt && partial->primary() != nullptr &&
                  partial->primary()->width == 0,
              "缺 currentMode.size -> 解得开但尺寸为 0（上层据此退回旧表）");
    }

    std::printf("\n%s (检查 %d 项，失败 %d 项)\n", Failures == 0 ? "全部通过" : "存在失败", Checks, Failures);
    return Failures == 0 ? 0 : 1;
}
