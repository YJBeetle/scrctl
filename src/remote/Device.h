#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "net/Stack.h"
#include "remote/Rsd.h"
#include "transport/Lockdown.h"
#include "transport/Tunnel.h"
#include "transport/Usbmux.h"

namespace scrctl::remote {

/// 一台设备的一次完整会话：usbmuxd -> lockdown 配对 session -> 双向 TLS ->
/// CoreDeviceProxy -> CDTunnel -> RSD 目录。
///
/// 这条链在 scrctl 里只会有一个人写对的机会，所以放在库里而不是散在探针里——
/// 探针跑的必须就是产品要跑的那条路径，否则「探针通过」不能给实现背书。
class Device {
public:
    Device() = default;
    Device(Device &&) noexcept;
    Device &operator=(Device &&) noexcept;
    Device(const Device &) = delete;
    Device &operator=(const Device &) = delete;
    ~Device();

    /// 列出当前在连的设备。
    static std::vector<transport::DeviceRecord> list(std::string &err);

    /// 建立会话。udid 为空时：恰好一台就用它，多台则报错并把候选列出来——
    /// 和 scrcpy 不带 --serial 时的行为一致。
    ///
    /// verbose 会把每一环的进展打到 stderr。这条链上有好几处能静默阻塞
    /// （usbmuxd 的读没有超时），没有分阶段输出的话，卡住就只剩「什么也没印」。
    static std::optional<Device> establish(std::string_view udid, std::string &err,
                                           bool verbose = false);

    [[nodiscard]] Rsd &rsd() { return *rsd_; }
    [[nodiscard]] const Rsd &rsd() const { return *rsd_; }
    [[nodiscard]] const std::string &udid() const { return udid_; }
    /// USB 还是 Wi-Fi 之类，来自 usbmux 的连接类型。
    [[nodiscard]] const std::string &connection_type() const { return connection_type_; }

    /// 从 peer_info 的 Properties 里取一个字符串字段（型号、系统版本等）。
    [[nodiscard]] std::string property(std::string_view key) const;

    /// 开一个服务连接。
    std::unique_ptr<ServiceConnection> connect(std::string_view service_name, std::string &err,
                                              bool verbose = false);

    /// 一步到位地调一个 CoreDevice feature：开连接、调用、关连接。
    /// 每次调用一条新连接听着浪费，但设备的 DDI 服务就是按连接算会话的，
    /// 而且设备侧对「复用发起 start 的那条连接发 stop」有崩溃前科。
    bool feature(std::string_view service_name, std::string_view feature_identifier,
                 std::string_view action_identifier, const xpc::Value &input, xpc::Value &output,
                      std::string &err, bool verbose = false, int timeout_ms = 10000);

private:
    std::string udid_;
    std::string connection_type_;
    std::optional<transport::Lockdown> lockdown_;
    /// 隧道必须放堆上。Rsd 里存的是 `PacketTunnel*`，而 tunnel_ 一旦跟着 Device
    /// 一起被移动，那个指针就会指向旧地址——表现是连不通或读到垃圾，且不保证
    /// 每次都不出问题。unique_ptr 让对象的地址与 Device 的位置脱钩，移动才安全。
    std::unique_ptr<transport::PacketTunnel> tunnel_;
    std::unique_ptr<net::Stack> stack_;
    std::optional<Rsd> rsd_;
};

/// 把标识类字段打掉，只留尾几位用于人工比对。UDID / 序列号 / MAC 一旦被贴进
/// issue 或日志收集系统就是永久泄露，所以默认屏蔽、要明文得显式改代码。
[[nodiscard]] std::string mask(std::string_view value, std::size_t keep = 4);

}  // namespace scrctl::remote
