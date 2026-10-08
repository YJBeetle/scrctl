#pragma once

#include "remote/Device.h"
#include "remote/Discovery.h"
#include <functional>
#include <optional>
#include <string>

namespace scrctl::app {

/// wifi="auto" 只扫描本次 mDNS 广播，按唯一已配对设备尝试所有可用地址与 SRV 端口。
/// 显式地址使用 wifi_port；无线模式只使用已有完整且有设备身份的记录，不创建配对。
/// wifi 为空时优先匹配的 usbmux 设备，缺少候选才尝试自动无线；USB 协议失败直接返回。
/// serial 始终是原始 USB/配对记录 UDID，不能用净化后的文件名或广播 identifier 替代。
std::optional<remote::Device> open_device(const std::string &serial, const std::string &wifi,
                                          std::string &err,
                                          uint16_t wifi_port = wifi::kAdvertisedPortFallback);

namespace detail {
struct WirelessSelection {
    std::string udid;
    std::vector<remote::DiscoveryCandidate> candidates;
};

/// 只选择 ready 的 RemotePairing 候选；多个原始 UDID 要求明确 serial。
/// 普通 IPv4/IPv6 先于 link-local；IPv6 link-local 的接口 scope 不丢失。
std::optional<WirelessSelection> select_paired_wireless_device(
    const std::vector<remote::DiscoveredDevice> &devices, std::string_view serial, std::string &error);
std::optional<wifi::PairRecord> select_pairing_record(const std::vector<wifi::PairRecord> &records,
                                                    std::string_view serial, std::string &error);
bool has_usbmux_match(const std::vector<transport::DeviceRecord> &devices, std::string_view serial);

using WifiConnector = std::function<std::optional<remote::Device>(
    const std::string &, const wifi::PairRecord &, uint16_t, std::string &)>;
/// 依次连接选择结果，某一候选失败不丢弃其余候选；全部失败时保留各次错误。
std::optional<remote::Device> connect_wireless_candidates(const WirelessSelection &selection,
                                                        const wifi::PairRecord &record,
                                                        std::string &error,
                                                        const WifiConnector &connect);
} // namespace detail

} // namespace scrctl::app
