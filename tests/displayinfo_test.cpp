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
#include <cstdio>
#include <string>
#include <vector>

#include "remote/DisplayInfo.h"
#include "xpc/XpcValue.h"

namespace {

int Failures = 0;

void check(bool ok, const std::string &what) {
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

        auto both = make_array();
        array_push(both, make_int64(500));
        array_push(both, make_uint64(900));
        check(one_size(std::move(both)), "size 是 int64 / uint64 时也解得出");

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

    std::printf("\n%s (失败 %d 项)\n", Failures == 0 ? "全部通过" : "存在失败", Failures);
    return Failures == 0 ? 0 : 1;
}
