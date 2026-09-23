#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "remote/Device.h"
#include "xpc/XpcValue.h"

namespace scrctl::hid {

/// 触摸注入。
///
/// 走的是 `com.apple.coredevice.hid.universalhidservice`：设备把自己的 HID 面
/// （真正的触摸屏、手势面、侧键组…）注册在 dtuhidd 上，宿主往某个面的
/// `_ServiceID` 上**投原始 HID 报告**即可，坐标是归一化的 0..65535，所以不依赖
/// 屏幕分辨率——同一份代码在 1125x2436 与 1290x2796 上不用改。
///
/// 没有走 `hid.indigo` 的 digitizer feature：那条路要 Apple 的 Mercury 对端事件
/// 外壳，实测设备收到 dispatch 后立刻 "Resetting gesture state then canceling"，
/// 不进任何 handler。indigo 上确定能用的是 button（硬件按键）。
///
/// **必须有一条在跑的视频流。** 这是认证门，不是巧合：没有媒体会话时 dtuhidd 把
/// 我们的 HID 面标成 `authenticated: NO / eventSource: externalAccessory`，
/// backboardd 会把每一个 digitizer 事件丢掉（"ignoring digitizer event for
/// display <main> from unsupported service"）。起一条 `startmediastream` 就会把
/// 这两个标志翻成 YES，报告一路走到 UIKit 变成真的 `UIEventTypeTouches`；流本身
/// 的载荷可以不看。

/// 设备注册的静态 HID 面。
inline constexpr uint64_t kSurfaceMainTouchscreen = 257;   ///< 真数（0x101）
inline constexpr uint64_t kSurfaceTouchGesture = 1281;     ///< 触控板式指针（0x501）

/// mainTouchscreen 报告里的接触状态字节。
inline constexpr uint8_t kStateContact = 0xC2;  ///< 在该位置保持接触
inline constexpr uint8_t kStateRelease = 0x02;  ///< 抬起

/// 主机侧单调时钟，作为 48 位时间戳塞进报告尾部。
///
/// 要保住的性质只有两条：单调、以及帧间差值真实（手势/惯性靠差值算速度）。
/// 绝对值不需要和设备的墙钟对齐——参考客户端抓下来的这个字段是 1.07e9 这个量级，
/// 明显不是"开机至今"，而它的报告照样能画出来。这里取"本进程第一次取时间戳"
/// 以来的纳秒，量级与之一致。
[[nodiscard]] uint64_t report_timestamp();

/// 58 字节的 mainTouchscreen 报告（报告号 0x09）。
///
/// ```text
///  0     0x09  报告号
///  1-2   0x01 0x05
///  3     状态：0xC2 接触 / 0x02 抬起
///  4-5   X  (UInt16 LE, 0..65535)
///  6-7   Y  (UInt16 LE, 0..65535)
///  8-39  32 字节 0
///  40-43 0x02 0x00 0x00 0x00
///  44-49 时间戳（6 字节 LE）
///  50-57 8 字节 0
/// ```
///
/// 一次点击 = 同一坐标上一个 CONTACT 加一个 RELEASE；一次拖动 = 一串推进坐标的
/// CONTACT，最后跟一个 RELEASE。**没有**单独的 begin/end 操作码，每个 CONTACT
/// 都是"此刻在此处接触着"。
[[nodiscard]] std::vector<uint8_t> touchscreen_report(uint8_t state, uint16_t x, uint16_t y,
                                                      uint64_t timestamp = report_timestamp());

/// 归一化坐标（0.0..1.0）-> 报告里的 UInt16。越界值夹住而不是取模：取模会把
/// 屏幕外的点变成屏幕内的点击，那是比"停在边上"更糟的结果。
[[nodiscard]] uint16_t normalize(double v);

/// 一条已打开的 universalhidservice 连接。
class Service {
public:
    /// 目录里没有这个服务时返回 nullptr（DDI 版本差异，调用方该给出人话）。
    static std::unique_ptr<Service> open(scrctl::remote::Device &device, std::string &err,
                                        bool verbose = false);

    struct Surface {
        uint64_t service_id = 0;
        std::string name;
    };

    /// 枚举设备当前注册的 HID 面。
    bool surfaces(std::vector<Surface> &out, std::string &err);

    /// connectedServices 的**原文**。面的认证状态这类属性长什么样，事先猜不到，
    /// 所以留一条把整份字典交出来的路。
    bool raw_connected_services(scrctl::xpc::Value &reply, std::string &err);

    /// 投递一个原始报告（首字节是 HID 报告号）。默认不等回信——设备对 send 不安
    /// 回，等就成了每个点一次往返，注入延迟立刻可见。
    ///
    /// 传 `reply` 就改走一次往返并把回信带回来。"发送成功但屏幕没反应"的时候，
    /// 这是唯一能把 dtuhidd 的真实态度拿到的办法——它拒绝一个畸形请求时未必会
    /// 留下别的痕迹，所以两种结果都有信息量。
    bool send_report(uint64_t service_id, std::span<const uint8_t> report, std::string &err,
                     scrctl::xpc::Value *reply = nullptr);

    /// 在 mainTouchscreen 上放一个接触或抬起。x/y 是 0..1 的归一化屏幕坐标。
    bool touch(uint64_t service_id, double x, double y, bool down, std::string &err);

    /// 一次点击：按下、停 hold_ms、抬起。
    bool tap(double x, double y, int hold_ms, std::string &err);

    /// 一条折线：从 (x0,y0) 按点序接触移动，最后抬起。点与点之间睡 step_ms，
    /// 让设备侧能算出速度——瞬移式的拖动会被当成抖动。
    bool stroke(const std::vector<std::pair<double, double>> &points, int step_ms,
                std::string &err);

private:
    explicit Service(std::unique_ptr<scrctl::remote::ServiceConnection> conn)
        : conn_(std::move(conn)) {}

    std::unique_ptr<scrctl::remote::ServiceConnection> conn_;
};

}  // namespace scrctl::hid
