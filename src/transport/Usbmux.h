#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "plist/Plist.h"
#include "transport/Socket.h"

namespace scrctl::transport {

struct DeviceRecord {
    uint32_t device_id = 0;
    std::string udid;              ///< Properties.SerialNumber
    std::string connection_type;   ///< "USB" / "Network"
    uint32_t product_id = 0;
    [[nodiscard]] bool is_usb() const { return connection_type == "USB"; }
};

/// usbmux 客户端：macOS / Linux 使用 AF_UNIX，Windows 使用 AMDS 本地 TCP。
///
/// 帧格式（已有 macOS 设备记录）：
///   [len:u32 LE，含自身][version:u32=1][message:u32][tag:u32][payload]
/// plist 协议下 message 恒为 8(PLIST)，payload 是 XML plist 再加一个 NUL。
///
/// 连接约束：
///   - Connect 不能在发过 Listen 的连接上用，否则回 Number=5(NOT_CONNECTED)。
///     所以本类走 ListDevices + Connect 的同一条连接。
///   - PortNumber 必须是网络序（htons）。传主机序会回 Number=3(CONNREFUSED)。
class Usbmux {
public:
    Usbmux() = default;
    Usbmux(Usbmux &&) noexcept;
    Usbmux &operator=(Usbmux &&) noexcept;
    Usbmux(const Usbmux &) = delete;
    Usbmux &operator=(const Usbmux &) = delete;

    /// 连接 usbmux 服务：macOS/Linux 为 /var/run/usbmuxd，Windows 为 127.0.0.1:27015。
    static std::optional<Usbmux> open(std::string &err);

    bool list_devices(std::vector<DeviceRecord> &out, std::string &err);

    /// 把 socket 转发到 device_id:port。成功后本对象失效，返回的 Socket
    /// 就是到该端口的透明通道（lockdown 即接在这上面）。
    std::optional<Socket> connect(uint32_t device_id, uint16_t port, std::string &err);

    /// 读配对记录（bplist 字节）。
    ///
    /// 必须走 usbmuxd 而不是直接读文件：macOS 上 /var/db/lockdown 是 root-only，
    /// 而 usbmuxd 有权限代读。返回的 blob 里含私钥与证书，**绝不可写进日志**。
    bool read_pair_record(std::string_view udid, std::vector<uint8_t> &out, std::string &err);

    [[nodiscard]] static std::string socket_path();

private:
    bool round_trip(const plist::Value &request, plist::Value &reply, std::string &err);

    Socket sock_;
    uint32_t tag_ = 1;
};

/// 新建 usbmux 连接并转发到 lockdown 端口 (62078)。
/// 注意不能在发过 Listen 的连接上 Connect，所以这里每次新开。
std::optional<Socket> connect_lockdown(uint32_t device_id, std::string &err);

}  // namespace scrctl::transport
