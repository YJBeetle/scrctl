// M2.5 探针：从 USB 一路走到 RSD 服务目录。
//
// 这条链的每一环都各有一种死法，所以逐环打印、逐环节止：
//   usbmuxd -> lockdown session -> 双向 TLS -> CoreDeviceProxy -> CDTunnel
//   -> 隧道内 IPv6/TCP -> HTTP/2 -> RemoteXPC -> peer_info + Services
// 判据很硬：TCP 校验和或序号有一点错，设备静默丢 SYN（表现为握手超时）；
// HTTP/2 帧顺序不对，设备回 GOAWAY 并写明原因；XPC 编码错了，回信解不出来。
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "http2/Framing.h"
#include "net/TcpStream.h"
#include "remote/RemoteXpc.h"
#include "transport/Lockdown.h"
#include "transport/Tunnel.h"
#include "transport/Usbmux.h"
#include "xpc/XpcValue.h"

namespace {

std::string mask(std::string_view s, size_t keep = 4) {
    if (s.size() <= keep) {
        return std::string(s);
    }
    return "****" + std::string(s.substr(s.size() - keep));
}

/// peer_info 里这几项是设备指纹级的标识，探针的意义只在「有没有、长什么样」，
/// 所以只留尾 2 位供比对，其余全部打掉。输出经常被贴进 issue 或提交说明里，
/// 这类字段一漏就是永久泄露。
bool sensitive(std::string_view key) {
    static constexpr std::string_view kParts[] = {"SerialNumber", "MacAddress", "UDID",
                                                 "UniqueDeviceID", "BootSessionUUID", "WiFi"};
    for (const auto &part : kParts) {
        if (key.find(part) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main() {
    // 探针常被外层脚本限时杀掉，stdout 重定向到文件是块缓冲的，
    // 不显式关掉缓冲就会把已经打出来的进展整段丢掉。
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string err;
    auto mux = scrctl::transport::Usbmux::open(err);
    if (!mux) {
        std::fprintf(stderr, "打开 usbmuxd 失败: %s\n", err.c_str());
        return 1;
    }
    std::vector<scrctl::transport::DeviceRecord> devices;
    if (!mux->list_devices(devices, err)) {
        std::fprintf(stderr, "list_devices 失败: %s\n", err.c_str());
        return 1;
    }
    if (devices.empty()) {
        std::fprintf(stderr, "没有设备\n");
        return 1;
    }
    const auto d = devices[0];
    std::printf("DeviceID=%u UDID=%s\n", d.device_id, mask(d.udid).c_str());

    auto ld = scrctl::transport::Lockdown::establish(d.device_id, d.udid, err);
    if (!ld) {
        std::fprintf(stderr, "lockdown 失败: %s\n", err.c_str());
        return 1;
    }
    std::printf("lockdown session OK\n");

    auto ep = ld->start_service("com.apple.internal.devicecompute.CoreDeviceProxy", err);
    if (!ep) {
        std::fprintf(stderr, "StartService 失败: %s\n", err.c_str());
        return 1;
    }
    std::printf("CoreDeviceProxy 端口=%u 需要TLS=%s\n", ep->port,
                ep->requires_tls ? "是" : "否");

    auto tunnel = scrctl::transport::PacketTunnel::establish(d.device_id, ep->port,
                                                            ld->identity(), ep->requires_tls, err);
    if (!tunnel) {
        std::fprintf(stderr, "隧道握手失败: %s\n", err.c_str());
        return 1;
    }
    const auto &p = tunnel->params();
    std::printf("隧道 OK: %s -> %s  RSD 端口=%u MTU=%u\n", p.client_address.c_str(),
                p.server_address.c_str(), p.rsd_port, p.mtu);

    std::printf("\n在隧道内建立 TCP 连接...\n");
    scrctl::net::TcpStream stream(*tunnel, p.client_address, p.server_address);
    if (!stream.connect(p.rsd_port, err)) {
        std::fprintf(stderr, "  TCP 握手失败: %s\n", err.c_str());
        return 1;
    }
    std::printf("  TCP 三次握手完成\n");

    // M2.5：把隧道 -> HTTP/2 -> RemoteXPC 这条链跑通，读到服务目录。
    // peer UUID 取自配对记录里的 HostID：设备上每条隧道只保留一个 RSD 连接，
    // 而且会记住被它换掉的那个 peer；UUID 一变设备就重新 attach 整台机器，
    // 把已公布的服务端口全部关掉，所以这个值必须稳定。
    std::printf("\nRemoteXPC 握手...\n");
    auto uuid = scrctl::remote::parse_uuid_text(ld->host_id());
    if (!uuid) {
        std::fprintf(stderr, "配对记录里的 HostID 不是合法 UUID\n");
        return 1;
    }
    scrctl::remote::PeerIdentity identity;
    identity.uuid = *uuid;
    std::printf("  peer UUID=%s\n", mask(ld->host_id()).c_str());

    auto channel = scrctl::remote::Channel::open(stream, identity, err, /*verbose=*/true);
    if (!channel) {
        std::fprintf(stderr, "  RemoteXPC 握手失败: %s\n", err.c_str());
        return 1;
    }
    const auto *info = channel->peer_info();
    if (info == nullptr) {
        std::fprintf(stderr, "  握手成功但没有 peer_info\n");
        return 1;
    }

    std::printf("  peer_info 顶层键: ");
    for (const auto &entry : info->dict) {
        std::printf("%s ", entry.key.c_str());
    }
    std::printf("\n");

    const auto *props = info->find("Properties");
    if (props != nullptr) {
        for (const auto &entry : props->dict) {
            std::string value = scrctl::xpc::describe(entry.value);
            if (sensitive(entry.key)) {
                value = mask(value, 2);
            }
            std::printf("    %-22s %s\n", entry.key.c_str(), value.c_str());
        }
    }

    const auto *services = info->find("Services");
    if (services == nullptr || !services->is_dict()) {
        std::fprintf(stderr, "  peer_info 里没有 Services\n");
        return 1;
    }
    std::printf("\n  服务共 %zu 个：\n", services->dict.size());
    for (const auto &entry : services->dict) {
        std::printf("    %-56s %s\n", entry.key.c_str(),
                    scrctl::xpc::describe(entry.value).substr(0, 150).c_str());
    }

    std::printf("\nM2.5 验证完成：隧道内已读到 RSD 服务目录\n");
    stream.close();
    return 0;
}
