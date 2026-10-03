#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "transport/Usbmux.h"

namespace scrctl::transport {

/// 连一个普通的 TCP 服务端点（IPv4/IPv6 字面量或主机名）。
///
/// 这一条不属于 `Usbmux`：Wi-Fi 那条路上，与设备打交道的第一跳是局域网里的一个
/// 普通 TCP 端口，没有 usbmuxd 参与。分开写，也是为了让"哪些字节是走系统协议栈
/// 出去的、哪些是走我们自己那个隧道内用户态栈出去的"这件事在文件层次上看得出来。
///
/// `timeout_ms` 按每个候选地址限制 connect 等待，不包含 DNS 解析。给 <= 0 表示不超时（不建议：设备睡眠时 ARP 会静默
/// 丢包，无限等就变成一个看不出原因的卡死）。
std::optional<Socket> connect_tcp(const std::string &host, uint16_t port, int timeout_ms,
                                  std::string &err);

}  // namespace scrctl::transport
