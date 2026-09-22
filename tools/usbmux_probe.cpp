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

    // 配对记录含私钥与证书：只报长度和魔数，内容一个字节都不打。
    std::printf("\nReadPairRecord:\n");
    {
        std::vector<uint8_t> rec;
        err.clear();
        if (!mux->read_pair_record(devices[0].udid, rec, err)) {
            std::fprintf(stderr, "  失败: %s\n", err.c_str());
        } else {
            std::string magic(rec.size() >= 8 ? reinterpret_cast<const char *>(rec.data()) : "");
            std::printf("  取回 %zu 字节，魔数=%s%s\n", rec.size(), magic.substr(0, 8).c_str(),
                        magic.starts_with("bplist") ? "  (bplist -> 后续需要二进制 plist 解析)"
                                                    : "");
            // 只列键名/类型/长度。值里有私钥和证书，一个都不打。
            auto parsed = scrctl::plist::parse(std::string_view(
                reinterpret_cast<const char *>(rec.data()), rec.size()));
            if (!parsed) {
                std::fprintf(stderr, "  记录无法按 XML plist 解析\n");
            } else {
                std::printf("  字段（仅键名与长度）:\n");
                for (const auto &k : parsed->keys) {
                    const auto *v = parsed->find(k);
                    const char *type = "?";
                    size_t len = 0;
                    switch (v->kind) {
                        case scrctl::plist::Kind::String: type = "string"; len = v->string.size(); break;
                        case scrctl::plist::Kind::Data:   type = "data";   len = v->data.size();   break;
                        case scrctl::plist::Kind::Int:    type = "integer"; break;
                        case scrctl::plist::Kind::Bool:   type = "bool"; break;
                        case scrctl::plist::Kind::Array:  type = "array"; len = v->array.size(); break;
                        case scrctl::plist::Kind::Dict:   type = "dict";  len = v->keys.size();  break;
                        default: break;
                    }
                    std::printf("    %-22s %-8s %zu\n", k.c_str(), type, len);
                }
            }
        }
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

    {
        scrctl::plist::Value req = scrctl::plist::Value::Dict();
        req.set("Label", scrctl::plist::Value::Str("scrctl-probe"));
        req.set("Request", scrctl::plist::Value::Str("QueryType"));
        scrctl::plist::Value reply;
        err.clear();
        if (!lockdown_query(*sock, req, reply, err)) {
            std::fprintf(stderr, "  QueryType 失败: %s\n", err.c_str());
        } else {
            std::string line = "  QueryType -> ";
            for (const auto &k : reply.keys) {
                const auto *v = reply.find(k);
                line += k + "=" + (v && v->is_string() ? v->as_string_or("")
                                                       : std::string("(非字符串)")) + " ";
            }
            std::printf("%s\n", line.c_str());
        }
    }

    // 关键问题：起 CoreDeviceProxy 到底需不需要先建立配对 session。
    // 不需要的话，M2 就能省掉整套 TLS 配对流程。
    std::printf("\n不经 session 直接 StartService(CoreDeviceProxy):\n");
    {
        scrctl::plist::Value req = scrctl::plist::Value::Dict();
        req.set("Label", scrctl::plist::Value::Str("scrctl-probe"));
        req.set("Request", scrctl::plist::Value::Str("StartService"));
        req.set("Service",
                scrctl::plist::Value::Str("com.apple.internal.devicecompute.CoreDeviceProxy"));
        scrctl::plist::Value reply;
        err.clear();
        if (!lockdown_query(*sock, req, reply, err)) {
            std::fprintf(stderr, "  失败: %s\n", err.c_str());
        } else {
            std::printf("  回复键: ");
            for (const auto &k : reply.keys) {
                std::printf("%s ", k.c_str());
            }
            std::printf("\n");
            if (const auto *e = reply.find("Error")) {
                std::printf("  Error=%s\n", e->as_string_or("?").c_str());
            }
            if (const auto *svc = reply.find("Service")) {
                std::printf("  Service=%s\n", svc->as_string_or("?").c_str());
            }
            if (const auto *p = reply.find("Port")) {
                std::printf("  Port=%lld\n", static_cast<long long>(p->as_int_or(0)));
            }
        }
    }

    std::printf("\nM2.1 通路验证完成\n");
    return 0;
}
