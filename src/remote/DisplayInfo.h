#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "remote/Rsd.h"
#include "xpc/XpcValue.h"

namespace scrctl::remote {

class Device;

/// 设备自己报出来的显示几何（`com.apple.coredevice.feature.displayinfoupdates`）。
///
/// 为什么必须有它而不是接着量码流尺寸反推：镜像帧的尺寸是**编码分辨率**，它比真正的
/// 显示分辨率大一圈（HEVC 按 CU 对齐填充），而且这一圈多大没有写在协议里。此前我们
/// 把 iPhone 上量到的那一档（1136x2464 -> 1125x2436）硬编码成一张表，表里没有的机型
/// 就整幅当可见区——裁剪偏了直接表现是边缘点不准与右边/下边一条垃圾边。
///
/// 这条 feature 给的是权威值，而且实测一次订阅就推一条完整的过来（真机 iPhone14,4 /
/// iOS 27.0）：
///
/// ```text
/// {current:true,
///  orientation:{currentDeviceOrientationLocked:false,
///               currentDeviceOrientation:"portrait", ...},
///  displays:[{displayId:1, primary:true, external:false, name:"LCD",
///             currentMode:{size:[1125,2436], preferredUIScale:3, refreshRate:60, ...},
///             nativeSize:[1080,2340], bounds:[[0,0],[1125,2436]],
///             currentOrientation:"rot0", pointScale:3, ...},
///            {displayId:2, external:true, deviceName:"wireless0", ...}, ...],
///  backlightState:"activeOn"}
/// ```
///
/// 只解**用到的**那几个字段：`displays[]` 里还有色彩、刷新率、物理尺寸、可用模式表
/// 等十来项，产品路径上一个都不读，读了解码面就要跟着它们一起维护。
struct Display {
    /// `startmediastream` 里送的 `displayId` 就是它：主屏在这台设备上是 1。
    uint64_t id = 0;
    bool primary = false;
    bool external = false;
    /// 设备给的名字（"LCD" / "Wireless"），只用来打日志。
    std::string name;
    /// `currentMode.size`：**当前这一档模式的像素尺寸**，也就是画面里真正可见的那一块。
    /// 注意别拿 `nativeSize` 当它——这台设备上 nativeSize 是 1080x2340，而可见区是
    /// 1125x2436，两者不是一回事。
    int width = 0;
    int height = 0;
    /// `currentOrientation`：`rot0` / `rot90` / `rot180` / `rot270`。
    std::string orientation;
};

struct DisplayInfo {
    std::vector<Display> displays;
    /// `orientation.currentDeviceOrientation`（"portrait" / "landscapeLeft" / ...）。
    /// 带着它只为了重起流时日志能说清"几何为什么变了"，产品路径不据它分支。
    std::string device_orientation;

    /// 按 `startmediastream` 用的那个 display id 找。找不到返回 nullptr——
    /// 这是常态而不是错误：外接屏、无线屏的 id 是设备分配的，我们不一定对得上。
    [[nodiscard]] const Display *find(uint64_t id) const;
    /// 主屏（`primary:true`）。多屏设备上这是"我们默认镜像的那一块"。
    [[nodiscard]] const Display *primary() const;
};

/// 从一条推送的 element 解出几何。形状不对返回 nullopt 并给原因。
///
/// 单独拆出来是为了离线自检：真机上"裁错了"的表现是边缘点不准，从日志根本看不出
/// 是解字段解错了还是映射算错了，所以这一层必须能拿录下来的 element 判。
[[nodiscard]] std::optional<DisplayInfo> parse_display_info(const xpc::Value &element,
                                                            std::string &err);

/// 开一条 deviceinfo 连接、订阅一次、拿到第一条推送就收工。
///
/// 为什么不常驻着订阅：这条 feature 是"推"模型，挂着不动它就不再发（实测一次推送之后
/// 就是 21 秒静默直到我们超时）。第一版需要的只是**起流之前**把几何问出来，所以拿完
/// 第一条就返回；要跟着转屏改尺寸得另做一套（订阅独占一条连接，而 `Channel::take_message`
/// 不看消息 id，一条连接上不能有两个要回信的请求——见 Rsd.h）。
std::optional<DisplayInfo> fetch_display_info(Device &device, std::string &err,
                                              bool verbose = false);

}  // namespace scrctl::remote
