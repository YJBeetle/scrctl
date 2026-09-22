// M2.2 探针：建立 lockdown 配对 session（含双向 TLS），并起 CoreDeviceProxy。
//
// 需要真机插着 USB 且已「信任此电脑」。SessionID 只打末 4 位。
#include <cstdio>
#include <string>
#include <vector>

#include "plist/Plist.h"
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

bool get_value(scrctl::transport::Lockdown &ld, std::string_view key, std::string &out,
               std::string &err) {
    scrctl::plist::Value req = scrctl::plist::Value::Dict();
    req.set("Request", scrctl::plist::Value::Str("GetValue"));
    req.set("Key", scrctl::plist::Value::Str(std::string(key)));
    scrctl::plist::Value reply;
    if (!ld.request(req, reply, err)) {
        return false;
    }
    const auto *v = reply.find("Value");
    if (v == nullptr) {
        out.clear();
        return true;
    }
    out = v->is_string() ? v->as_string_or("") : std::string("(非字符串)");
    return true;
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
    const auto &d = devices[0];
    std::printf("设备 DeviceID=%u 类型=%s UDID=%s\n", d.device_id, d.connection_type.c_str(),
                mask(d.udid).c_str());

    std::printf("\n建立 lockdown session（StartSession + 双向 TLS）...\n");
    auto ld = scrctl::transport::Lockdown::establish(d.device_id, d.udid, err);
    if (!ld) {
        std::fprintf(stderr, "  失败: %s\n", err.c_str());
        return 1;
    }
    std::printf("  OK  TLS=%s  SessionID=%s\n", ld->secure() ? "已建立" : "未启用",
                mask(ld->session_id()).c_str());

    std::printf("\n读几个无副作用的值:\n");
    for (const char *key : {"ProductVersion", "DeviceName", "AuthenticationPolicy"}) {
        std::string val;
        err.clear();
        if (!get_value(*ld, key, val, err)) {
            std::fprintf(stderr, "  %s 失败: %s\n", key, err.c_str());
            continue;
        }
        std::printf("  %-22s = %s\n", key, val.c_str());
    }

    std::printf("\n起 CoreDeviceProxy（这是 M2.3 隧道的入口）:\n");
    err.clear();
    uint16_t port = 0;
    bool service_tls = false;
    {
        scrctl::plist::Value req = scrctl::plist::Value::Dict();
        req.set("Request", scrctl::plist::Value::Str("StartService"));
        req.set("Service",
                scrctl::plist::Value::Str("com.apple.internal.devicecompute.CoreDeviceProxy"));
        scrctl::plist::Value reply;
        if (!ld->request(req, reply, err)) {
            std::fprintf(stderr, "  失败: %s\n", err.c_str());
            return 1;
        }
        std::printf("  回复字段:\n");
        for (const auto &k : reply.keys) {
            const auto *v = reply.find(k);
            std::string shown;
            if (v != nullptr) {
                switch (v->kind) {
                    case scrctl::plist::Kind::String: shown = "\"" + v->as_string_or("") + "\""; break;
                    case scrctl::plist::Kind::Int:    shown = std::to_string(v->as_int_or(0)); break;
                    case scrctl::plist::Kind::Bool:   shown = v->boolean ? "true" : "false"; break;
                    default: shown = "(其他)"; break;
                }
            }
            std::printf("    %-24s = %s\n", k.c_str(), shown.c_str());
        }
        // 直接用这份回复的 Port，不再调第二次 StartService——端口是一次性的。
        const auto *pv = reply.find("Port");
        port = static_cast<uint16_t>(pv != nullptr ? pv->as_int_or(0) : 0);
        const auto *sv = reply.find("EnableServiceSSL");
        service_tls = sv != nullptr && sv->as_bool_or(false);
    }
    if (port == 0) {
        std::fprintf(stderr, "  没拿到 Port\n");
        return 1;
    }
    std::printf("  OK  端口 = %u\n", port);

    std::printf("\n隧道握手（M2.3）:\n");
    err.clear();
    auto tunnel = scrctl::transport::PacketTunnel::establish(d.device_id, port, ld->identity(),
                                                             service_tls, err);
    if (!tunnel) {
        std::fprintf(stderr, "  失败: %s\n", err.c_str());
        return 1;
    }
    const auto &p = tunnel->params();
    std::printf("  本机隧道地址 = %s\n", p.client_address.c_str());
    std::printf("  设备隧道地址 = %s\n", p.server_address.c_str());
    std::printf("  隧道内 RSD 端口 = %u\n", p.rsd_port);
    std::printf("  MTU = %u\n", p.mtu);
    std::printf("\nM2.3 握手验证完成：下一步挂 IPv6+TCP 栈连 RSD\n");
    return 0;
}
