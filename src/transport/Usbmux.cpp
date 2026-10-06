#include "i18n/Translation.h"
#include "Usbmux.h"

#include "transport/TcpConnect.h"
#ifndef _WIN32
#include <sys/un.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cstring>

namespace scrctl::transport {
namespace {

constexpr uint32_t kProtoVersion = 1;
constexpr uint32_t kMsgPlist = 8;
constexpr size_t kHeaderLen = 16;
/// 一帧负载的上限。对方声明的长度必须先跟它比再去申请内存。
constexpr uint32_t kMaxPayload = 16u << 20;
constexpr uint16_t kLockdownPort = 62078;
constexpr const char *kClientName = "scrctl";

void put_u32_le(uint8_t *p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

uint32_t get_u32_le(const uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

plist::Value base_request(std::string_view type) {
    plist::Value v = plist::Value::Dict();
    v.set("MessageType", plist::Value::Str(std::string(type)));
    v.set("ClientVersionString", plist::Value::Str(kClientName));
    v.set("ProgName", plist::Value::Str(kClientName));
    return v;
}

DeviceRecord parse_record(const plist::Value &rec) {
    DeviceRecord d;
    if (const auto *id = rec.find("DeviceID")) {
        d.device_id = static_cast<uint32_t>(id->as_int_or(0));
    }
    if (const auto *props = rec.find("Properties")) {
        if (const auto *s = props->find("SerialNumber")) {
            d.udid = s->as_string_or("");
        }
        if (const auto *c = props->find("ConnectionType")) {
            d.connection_type = c->as_string_or("");
        }
        if (const auto *pid = props->find("ProductID")) {
            d.product_id = static_cast<uint32_t>(pid->as_int_or(0));
        }
    }
    return d;
}

} // namespace

// ------------------------------------------------------------ Usbmux ------

std::string Usbmux::socket_path() {
#ifdef _WIN32
    return "127.0.0.1:27015";
#else
    return "/var/run/usbmuxd";
#endif
}

std::optional<Usbmux> Usbmux::open(std::string &err) {
#ifdef _WIN32
    // Apple Mobile Device Service 在 Windows 提供本地 TCP usbmux 服务。
    auto socket = connect_tcp("127.0.0.1", 27015, 3000, err);
    if (!socket) return std::nullopt;
    Usbmux mux;
    mux.sock_ = std::move(*socket);
    return mux;
#else
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        err = std::string(SCRCTL_TR("socket failed: ")) + std::strerror(errno);
        return std::nullopt;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path().c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        err = SCRCTL_TR("Connect to ") + socket_path() + SCRCTL_TR(" failed: ") + std::strerror(errno);
        ::close(fd);
        return std::nullopt;
    }
    Usbmux mux;
    mux.sock_.reset(fd);
    return mux;
#endif
}

Usbmux::Usbmux(Usbmux &&other) noexcept : sock_(std::move(other.sock_)), tag_(other.tag_) {
    other.tag_ = 0;
}

Usbmux &Usbmux::operator=(Usbmux &&other) noexcept {
    if (this != &other) {
        sock_ = std::move(other.sock_);
        tag_ = other.tag_;
        other.tag_ = 0;
    }
    return *this;
}

bool Usbmux::round_trip(const plist::Value &request, plist::Value &reply, std::string &err) {
    std::string body = plist::write(request);
    body.push_back('\0');

    uint8_t hdr[kHeaderLen];
    put_u32_le(hdr, static_cast<uint32_t>(kHeaderLen + body.size()));
    put_u32_le(hdr + 4, kProtoVersion);
    put_u32_le(hdr + 8, kMsgPlist);
    put_u32_le(hdr + 12, tag_);
    if (!sock_.write_all(hdr, kHeaderLen, err)) {
        return false;
    }
    if (!sock_.write_all(body.data(), body.size(), err)) {
        return false;
    }
    ++tag_;

    uint8_t rhdr[kHeaderLen];
    if (!sock_.read_exact(rhdr, kHeaderLen, err)) {
        return false;
    }
    const uint32_t total = get_u32_le(rhdr);
    // 上限必须有，且不能只判下界：total 是对方给的 32 位数，只挡 `< kHeaderLen`
    // 的话，一个 0xFFFFFFFF 就会让我们先去申请 4GB 内存——那是 DoS，不是解析失败。
    // usbmuxd 的回复实际是 KB 级（设备列表、配对记录），16MB 已经宽到没边。
    if (total < kHeaderLen || total - kHeaderLen > kMaxPayload) {
        err = SCRCTL_TR("Invalid mux frame length: ") + std::to_string(total);
        return false;
    }
    std::vector<uint8_t> payload(total - kHeaderLen);
    if (!payload.empty() && !sock_.read_exact(payload.data(), payload.size(), err)) {
        return false;
    }
    while (!payload.empty() && payload.back() == 0) {
        payload.pop_back();
    }
    auto parsed = plist::parse(std::string_view(
        reinterpret_cast<const char *>(payload.data()), payload.size()));
    if (!parsed) {
        err = SCRCTL_TR("mux response is not a valid plist");
        return false;
    }
    reply = std::move(*parsed);
    return true;
}

bool Usbmux::list_devices(std::vector<DeviceRecord> &out, std::string &err) {
    plist::Value reply;
    if (!round_trip(base_request("ListDevices"), reply, err)) {
        return false;
    }
    const auto *list = reply.find("DeviceList");
    if (list == nullptr) {
        err = SCRCTL_TR("ListDevices response missing DeviceList");
        return false;
    }
    for (const auto &rec : list->array) {
        out.push_back(parse_record(rec));
    }
    return true;
}

std::optional<Socket> Usbmux::connect(uint32_t device_id, uint16_t port, std::string &err) {
    plist::Value req = base_request("Connect");
    req.set("DeviceID", plist::Value::Int(device_id));
    // 必须网络序：传主机序会得到 Number=3 (CONNREFUSED)。
    req.set("PortNumber", plist::Value::Int(htons(port)));

    plist::Value reply;
    if (!round_trip(req, reply, err)) {
        return std::nullopt;
    }
    const auto *number_val = reply.find("Number");
    const int number = number_val != nullptr ? static_cast<int>(number_val->as_int_or(-1)) : -1;
    if (number != 0) {
        err = SCRCTL_TR("Connect failed, usbmuxd returned Number=") + std::to_string(number);
        return std::nullopt;
    }
    // 此后同一 socket 即到 device:port 的透明通道，把 fd 交出去。
    Socket tunnel;
    tunnel.reset(sock_.release());
    return tunnel;
}

bool Usbmux::read_pair_record(std::string_view udid, std::vector<uint8_t> &out,
                              std::string &err) {
    plist::Value req = base_request("ReadPairRecord");
    req.set("PairRecordID", plist::Value::Str(std::string(udid)));

    plist::Value reply;
    if (!round_trip(req, reply, err)) {
        return false;
    }
    const auto *data = reply.find("PairRecordData");
    if (data == nullptr || data->data.empty()) {
        const auto *e = reply.find("MessageType");
        err = SCRCTL_TR("ReadPairRecord response missing PairRecordData (response ") +
              std::string(e ? e->as_string_or("?") : "?") + SCRCTL_TR("); device may not trust this computer yet");
        return false;
    }
    out = data->data;
    return true;
}

std::optional<Socket> connect_lockdown(uint32_t device_id, std::string &err) {
    auto mux = Usbmux::open(err);
    if (!mux) {
        return std::nullopt;
    }
    return mux->connect(device_id, kLockdownPort, err);
}

}  // namespace scrctl::transport
