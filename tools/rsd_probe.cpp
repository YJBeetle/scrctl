// M2.4 探针：验证隧道内的用户态 IPv6+TCP 栈能完成三次握手。
//
// 判据很硬：TCP 校验和（IPv6 伪头）或序号只要有一点错，设备就静默丢弃
// 我们的 SYN，表现为握手超时。所以"连上了"本身就是正确性证明。
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "net/TcpStream.h"
#include "transport/Lockdown.h"
#include "transport/Tunnel.h"
#include "transport/Usbmux.h"

namespace {

std::string mask(std::string_view s, size_t keep = 4) {
    if (s.size() <= keep) {
        return std::string(s);
    }
    return "****" + std::string(s.substr(s.size() - keep));
}

}  // namespace

int main() {
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

    std::printf("\n在隧道内建立 TCP 连接（M2.4 判据）...\n");
    scrctl::net::TcpStream stream(*tunnel, p.client_address, p.server_address);
    if (!stream.connect(p.rsd_port, err)) {
        std::fprintf(stderr, "  TCP 握手失败: %s\n", err.c_str());
        return 1;
    }
    std::printf("  TCP 三次握手完成，连接已建立\n");

    // 再验证一次双向数据：发点东西，看对方是否继续 ACK 且我们仍能读到回复。
    // 用 HTTP/2 的客户端前置签名，RSD 会把它当协议错误回一段数据。
    std::printf("\n验证双向数据...\n");
    constexpr std::string_view kH2Preface =
        "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    if (!stream.send(kH2Preface, err)) {
        std::fprintf(stderr, "  发送失败: %s\n", err.c_str());
        return 1;
    }
    std::printf("  已发 %zu 字节\n", kH2Preface.size());

    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    std::vector<uint8_t> got;
    err.clear();
    if (stream.recv(got, 3000, err)) {
        std::printf("  收到 %zu 字节回复: %s\n", got.size(),
                    std::string(got.begin(),
                               got.begin() + static_cast<long>(std::min<size_t>(got.size(), 60)))
                        .c_str());
    } else {
        std::printf("  暂无回复（%s）—— 握手已完成，数据面留给 M2.5\n", err.c_str());
    }

    std::printf("\nM2.4 验证完成：隧道内 TCP 可用\n");
    stream.close();
    return 0;
}
