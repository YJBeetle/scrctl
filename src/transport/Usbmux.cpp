#include "Usbmux.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace scrctl::transport {
namespace {

constexpr uint32_t kProtoVersion = 1;
constexpr uint32_t kMsgPlist = 8;
constexpr size_t kHeaderLen = 16;
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

}  // namespace

// ------------------------------------------------------------ Socket ------

Socket::Socket(Socket &&other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

Socket &Socket::operator=(Socket &&other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

Socket::~Socket() { close(); }

void Socket::reset(int fd) {
    close();
    fd_ = fd;
}

void Socket::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool Socket::write_all(const void *data, size_t len, std::string &err) {
    const auto *p = static_cast<const uint8_t *>(data);
    size_t left = len;
    while (left > 0) {
        const ssize_t n = ::send(fd_, p, left, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            err = std::string("send 失败: ") + std::strerror(errno);
            return false;
        }
        if (n == 0) {
            err = "对端关闭了写方向";
            return false;
        }
        p += static_cast<size_t>(n);
        left -= static_cast<size_t>(n);
    }
    return true;
}

bool Socket::read_exact(void *data, size_t len, std::string &err) {
    if (len == 0) {
        return true;
    }
    auto *p = static_cast<uint8_t *>(data);
    size_t got = 0;
    while (got < len) {
        const ssize_t n = ::recv(fd_, p + got, len - got, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            err = std::string("recv 失败: ") + std::strerror(errno);
            return false;
        }
        if (n == 0) {
            err = "对端关闭（只读到 " + std::to_string(got) + "/" + std::to_string(len) + "）";
            return false;
        }
        got += static_cast<size_t>(n);
    }
    return true;
}

bool Socket::wait_readable(int ms, std::string &err) {
    if (fd_ < 0) {
        err = "socket 已关闭";
        return false;
    }
    pollfd pfd{fd_, POLLIN, 0};
    for (;;) {
        const int n = ::poll(&pfd, 1, ms);
        if (n > 0) {
            return true;
        }
        if (n == 0) {
            err = "等待超时";
            return false;
        }
        if (errno != EINTR) {
            err = std::string("poll 失败: ") + std::strerror(errno);
            return false;
        }
    }
}

bool Socket::read_len_prefixed_be(std::vector<uint8_t> &out, std::string &err) {
    uint8_t hdr[4];
    if (!read_exact(hdr, 4, err)) {
        return false;
    }
    const uint32_t len = uint32_t(hdr[0]) << 24 | uint32_t(hdr[1]) << 16 | uint32_t(hdr[2]) << 8 |
                         hdr[3];
    if (len < 4 || len > (32u << 20)) {
        err = "lockdown 帧长度异常: " + std::to_string(len);
        return false;
    }
    out.resize(len);
    return read_exact(out.data(), len, err);
}

bool Socket::write_len_prefixed_be(std::string_view payload, std::string &err) {
    uint8_t hdr[4];
    const uint32_t n = static_cast<uint32_t>(payload.size());
    hdr[0] = static_cast<uint8_t>(n >> 24);
    hdr[1] = static_cast<uint8_t>(n >> 16);
    hdr[2] = static_cast<uint8_t>(n >> 8);
    hdr[3] = static_cast<uint8_t>(n);
    if (!write_all(hdr, 4, err)) {
        return false;
    }
    return write_all(payload.data(), payload.size(), err);
}

// ------------------------------------------------------------ Usbmux ------

std::string Usbmux::socket_path() { return "/var/run/usbmuxd"; }

std::optional<Usbmux> Usbmux::open(std::string &err) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        err = std::string("socket 失败: ") + std::strerror(errno);
        return std::nullopt;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path().c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        err = "连接 " + socket_path() + " 失败: " + std::strerror(errno);
        ::close(fd);
        return std::nullopt;
    }
    Usbmux mux;
    mux.sock_.reset(fd);
    return mux;
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
    if (total < kHeaderLen) {
        err = "mux 帧长度异常: " + std::to_string(total);
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
        err = "mux 回复不是合法 plist";
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
        err = "ListDevices 回复里没有 DeviceList";
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
        err = "Connect 失败，usbmuxd 返回 Number=" + std::to_string(number);
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
        err = "ReadPairRecord 没有返回 PairRecordData（回复 " +
              std::string(e ? e->as_string_or("?") : "?") + "）；设备可能尚未信任本机";
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
