// M2.1 探针：验证 usbmuxd 客户端能枚举设备并打通 lockdown。
//
// 需要真机插着 USB，所以是手工工具而不是 ctest 用例。
// 输出里的 UDID 只保留末 4 位，避免把设备标识打进日志。
#include <cstdio>
#include <string>
#include <vector>

#include "plist/Plist.h"
#include "transport/Usbmux.h"

namespace {

std::string mask(std::string_view s) {
    if (s.size() <= 4) {
        return std::string(s);
    }
    return std::string("****") + std::string(s.substr(s.size() - 4));
}

bool lockdown_query(scrctl::transport::Socket &sock, const scrctl::plist::Value &req,
                    scrctl::plist::Value &out, std::string &err) {
    if (!sock.write_len_prefixed_be(scrctl::plist::write(req), err)) {
        return false;
    }
    std::vector<uint8_t> frame;
    if (!sock.read_len_prefixed_be(frame, err)) {
        return false;
    }
    auto v = scrctl::plist::parse(std::string_view(
        reinterpret_cast<const char *>(frame.data()), frame.size()));
    if (!v) {
        err = "lockdown 回复不是合法 plist";
        return false;
    }
    out = std::move(*v);
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
    std::printf("已连接 %s\n", scrctl::transport::Usbmux::socket_path().c_str());

    std::vector<scrctl::transport::DeviceRecord> devices;
    if (!mux->list_devices(devices, err)) {
        std::fprintf(stderr, "list_devices 失败: %s\n", err.c_str());
        return 1;
    }
    std::printf("设备数: %zu\n", devices.size());
    for (const auto &d : devices) {
        std::printf("  DeviceID=%u 类型=%-7s ProductID=%u UDID=%s\n", d.device_id,
                    d.connection_type.c_str(), d.product_id, mask(d.udid).c_str());
    }
    if (devices.empty()) {
        std::fprintf(stderr, "没有设备，插上 USB 再试\n");
        return 1;
    }

    const auto usb = devices[0];
    std::printf("\nConnect 到 lockdown(62078) ...\n");
    auto mux2 = scrctl::transport::Usbmux::open(err);
    if (!mux2) {
        std::fprintf(stderr, "重开 usbmuxd 失败: %s\n", err.c_str());
        return 1;
    }
    auto sock = mux2->connect(usb.device_id, 62078, err);
    if (!sock) {
        std::fprintf(stderr, "Connect 失败: %s\n", err.c_str());
        return 1;
    }
    std::printf("  OK，socket 已转发到 lockdown\n");

    for (const char *req_name : {"QueryType", "ReadBUID"}) {
        scrctl::plist::Value req = scrctl::plist::Value::Dict();
        req.set("Label", scrctl::plist::Value::Str("scrctl-probe"));
        req.set("Request", scrctl::plist::Value::Str(req_name));
        scrctl::plist::Value reply;
        err.clear();
        if (!lockdown_query(*sock, req, reply, err)) {
            std::fprintf(stderr, "  %s 失败: %s\n", req_name, err.c_str());
            continue;
        }
        std::string line = std::string("  ") + req_name + " -> ";
        for (const auto &k : reply.keys) {
            const auto *v = reply.find(k);
            line += k + "=" + (v && v->is_string() ? v->as_string_or("")
                                                   : std::string("(非字符串)")) + " ";
        }
        std::printf("%s\n", line.c_str());
    }

    std::printf("\nM2.1 通路验证完成\n");
    return 0;
}
