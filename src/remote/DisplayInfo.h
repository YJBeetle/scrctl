#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
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
/// 第一条就返回；要跟着转屏改尺寸得常驻订阅，那是下面的 `DisplayWatcher`。
std::optional<DisplayInfo> fetch_display_info(Device &device, std::string &err,
                                              bool verbose = false);

/// 常驻的显示几何订阅：后台挂着一条连接，转屏时把最新的朝向与尺寸递出来。
///
/// 为什么必须有它：镜像帧**永远不跟着转**（实测界面 rot270 时码流仍是 1136x2464 竖幅），
/// 转正这件事只有设备知道该怎么转，而它知道的方式是**推**。一次性订阅拿完第一条就收工
/// （`fetch_display_info`），之后设备转屏我们一无所知——画面会在窗口打开那一刻定死，
/// 横屏 App 会一直躺倒到下次重启。
///
/// 为什么独占一条连接：`Channel::take_message` 不校验消息 id，一条连接上同时挂两个
/// 要回信的请求就会互相拿错回信。所以订阅期间这条连接不接别的调用，它就是在那儿读。
///
/// 生命周期：它持有 `Device&`，所以**必须比 Device 先析构**。
class DisplayWatcher {
public:
    /// 最近一次推送里那块屏的状态。
    struct State {
        int width = 0;
        int height = 0;
        /// `rot0` / `rot90` / `rot180` / `rot270`。
        std::string orientation;
        /// 每收到一条**与上一次不同**的推送加一。调用方拿它当"要不要动"的判据，
        /// 而不是自己比较字段——设备会把同样的内容重发，比较字段就会把重发当成变化，
        /// 于是窗口每秒被重建一次。
        uint64_t seq = 0;
    };

    /// 订上并起后台线程。**订不上返回 nullptr**（err 给原因），调用方退回
    /// "起流前问一次"那一档就行——跟着转屏走是增强，不是必需。
    static std::unique_ptr<DisplayWatcher> start(Device &device, uint64_t display_id,
                                                 std::string &err, bool verbose = false);

    ~DisplayWatcher();

    DisplayWatcher(const DisplayWatcher &) = delete;
    DisplayWatcher &operator=(const DisplayWatcher &) = delete;

    [[nodiscard]] State latest() const;
    /// 后台线程还活着。连续重订失败之后它会自己收手（设备已经不在了），这时
    /// `latest()` 停在最后拿到的值——调用方应当按"没有这个功能"继续跑。
    [[nodiscard]] bool alive() const { return alive_.load(); }

private:
    DisplayWatcher(Device &device, uint64_t display_id, bool verbose);

    void loop();
    bool resubscribe(std::string &err);
    void publish(const xpc::Value &element);
    /// 从一条推送里挑出我们镜像的那块屏：先按 id 找，对不上退回主屏。
    const Display *pick(const DisplayInfo &info) const;

    Device &device_;
    uint64_t display_id_;
    bool verbose_;
    std::unique_ptr<ServiceConnection> conn_;
    std::thread worker_;
    mutable std::mutex mu_;
    State latest_;
    std::atomic<bool> stop_ { false };
    std::atomic<bool> alive_ { false };
};

}  // namespace scrctl::remote
