#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "transport/TlsChannel.h"
#include "transport/Usbmux.h"

namespace scrctl::transport {

/// CoreDeviceProxy 隧道握手后的参数。
struct TunnelParams {
    std::string client_address;  ///< 本机在隧道内的 IPv6
    std::string server_address;  ///< 设备在隧道内的 IPv6
    uint16_t rsd_port = 0;       ///< 隧道内 RSD 监听端口
    uint16_t mtu = 0;
};

/// CoreDeviceProxy 数据包隧道。
///
/// 帧格式（实测自参考实现）：
///   控制帧 = "CDTunnel"(8B magic) + u16 BE 长度 + JSON body
///   握手后线上是**裸 IPv6 包**：读时先取 40 字节 IPv6 头，用头内
///   bytes[4:5] 的 u16 BE payload 长度再取 body；写时不带任何帧头。
///
/// 一个必须遵守的约束：每个 IPv6 包要单独一次 write()，**不能合并写**。
/// CoreDeviceProxy 的转发路径对读边界敏感，合并突发会破坏包边界——
/// 实测 iOS 26.5/USB 上上传会塌到 ~1 MB/s 且隧道连接最终直接死掉。
class PacketTunnel {
public:
    PacketTunnel() = default;
    PacketTunnel(PacketTunnel &&) noexcept;
    PacketTunnel &operator=(PacketTunnel &&) noexcept;
    PacketTunnel(const PacketTunnel &) = delete;
    PacketTunnel &operator=(const PacketTunnel &) = delete;

    /// proxy 来自 lockdown StartService(CoreDeviceProxy)。
    ///
    /// 实测该服务带 EnableServiceSSL=true，即连上之后必须先按同一套配对证书
    /// 做 TLS 握手，再发 CDTunnel 控制帧；直接发明文会被对端立刻关闭连接。
    static std::optional<PacketTunnel> establish(uint32_t device_id, uint16_t proxy_port,
                                                 const PemIdentity &identity, bool use_tls,
                                                 std::string &err);

    [[nodiscard]] const TunnelParams &params() const { return params_; }
    [[nodiscard]] bool valid() const { return sock_.valid(); }

    /// 发一个完整 IPv6 包（一次 write，不合并）。
    bool send_ipv6(const uint8_t *packet, size_t len, std::string &err);
    /// 收一个完整 IPv6 包。
    bool recv_ipv6(std::vector<uint8_t> &out, std::string &err);

private:
    bool write_all(const void *data, size_t len, std::string &err);
    bool read_all(void *data, size_t len, std::string &err);

    Socket sock_;
    TlsChannel tls_;
    TunnelParams params_;
};

}  // namespace scrctl::transport
