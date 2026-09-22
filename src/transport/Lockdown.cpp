#include "Lockdown.h"

#include <cstring>

namespace scrctl::transport {
namespace {

constexpr uint16_t kLockdownPort = 62078;
constexpr const char *kLabel = "scrctl";

bool write_frame(SSL *ssl, Socket &sock, std::string_view body, std::string &err) {
    uint8_t hdr[4];
    const uint32_t n = static_cast<uint32_t>(body.size());
    hdr[0] = static_cast<uint8_t>(n >> 24);
    hdr[1] = static_cast<uint8_t>(n >> 16);
    hdr[2] = static_cast<uint8_t>(n >> 8);
    hdr[3] = static_cast<uint8_t>(n);
    if (ssl != nullptr) {
        if (SSL_write(ssl, hdr, 4) != 4) {
            return err = "TLS 写头失败", false;
        }
        if (SSL_write(ssl, body.data(), static_cast<int>(body.size())) !=
            static_cast<int>(body.size())) {
            return err = "TLS 写体失败", false;
        }
        return true;
    }
    return sock.write_len_prefixed_be(body, err);
}

bool read_frame(SSL *ssl, Socket &sock, std::vector<uint8_t> &out, std::string &err) {
    uint8_t hdr[4];
    if (ssl != nullptr) {
        if (SSL_read(ssl, hdr, 4) != 4) {
            return err = "TLS 读头失败", false;
        }
    } else if (!sock.read_exact(hdr, 4, err)) {
        return false;
    }
    const uint32_t len =
        uint32_t(hdr[0]) << 24 | uint32_t(hdr[1]) << 16 | uint32_t(hdr[2]) << 8 | hdr[3];
    if (len < 1 || len > (32u << 20)) {
        return err = "lockdown 帧长度异常: " + std::to_string(len), false;
    }
    out.resize(len);
    if (ssl == nullptr) {
        return sock.read_exact(out.data(), len, err);
    }
    size_t got = 0;
    while (got < len) {
        const int n = SSL_read(ssl, out.data() + got, static_cast<int>(len - got));
        if (n <= 0) {
            return err = "TLS 读体失败", false;
        }
        got += static_cast<size_t>(n);
    }
    return true;
}

}  // namespace

Lockdown::Lockdown() = default;
Lockdown::~Lockdown() = default;
Lockdown::Lockdown(Lockdown &&) noexcept = default;
Lockdown &Lockdown::operator=(Lockdown &&) noexcept = default;

bool Lockdown::request(const plist::Value &req, plist::Value &reply, std::string &err) {
    plist::Value full = req;
    if (full.find("Label") == nullptr) {
        full.set("Label", plist::Value::Str(kLabel));
    }
    SSL *ssl = tls_.handle();
    if (!write_frame(ssl, sock_, plist::write(full), err)) {
        return false;
    }
    std::vector<uint8_t> frame;
    if (!read_frame(ssl, sock_, frame, err)) {
        return false;
    }
    auto parsed = plist::parse(
        std::string_view(reinterpret_cast<const char *>(frame.data()), frame.size()));
    if (!parsed) {
        return err = "lockdown 回复不是合法 plist", false;
    }
    reply = std::move(*parsed);
    return true;
}

std::optional<Lockdown> Lockdown::establish(uint32_t device_id, std::string_view udid,
                                            std::string &err) {
    // 配对记录必须经 usbmuxd 读：/var/db/lockdown 是 root-only。
    // 且必须在 Connect 之前用另一条 mux 连接。
    auto rec_mux = Usbmux::open(err);
    if (!rec_mux) {
        return std::nullopt;
    }
    std::vector<uint8_t> record;
    if (!rec_mux->read_pair_record(udid, record, err)) {
        return std::nullopt;
    }
    auto parsed = plist::parse(
        std::string_view(reinterpret_cast<const char *>(record.data()), record.size()));
    if (!parsed) {
        err = "配对记录无法解析";
        return std::nullopt;
    }
    auto take = [&](std::string_view key) -> std::vector<uint8_t> {
        const auto *v = parsed->find(key);
        return v != nullptr ? v->data : std::vector<uint8_t>{};
    };

    Lockdown ld;
    if (const auto *h = parsed->find("HostID")) {
        ld.host_id_ = h->as_string_or("");
    }
    if (const auto *b = parsed->find("SystemBUID")) {
        ld.system_buid_ = b->as_string_or("");
    }
    ld.identity_ = PemIdentity{take("HostCertificate"), take("HostPrivateKey"),
                               take("RootCertificate")};
    if (ld.host_id_.empty() || !ld.identity_.complete()) {
        err = "配对记录缺少必要字段";
        return std::nullopt;
    }

    auto conn_mux = Usbmux::open(err);
    if (!conn_mux) {
        return std::nullopt;
    }
    auto sock = conn_mux->connect(device_id, kLockdownPort, err);
    if (!sock) {
        return std::nullopt;
    }
    ld.sock_ = std::move(*sock);

    plist::Value start = plist::Value::Dict();
    start.set("Request", plist::Value::Str("StartSession"));
    start.set("HostID", plist::Value::Str(ld.host_id_));
    start.set("SystemBUID", plist::Value::Str(ld.system_buid_));
    plist::Value reply;
    if (!ld.request(start, reply, err)) {
        return std::nullopt;
    }
    if (const auto *e = reply.find("Error"); e != nullptr) {
        err = "StartSession 被拒: " + e->as_string_or("?");
        return std::nullopt;
    }
    if (const auto *sid = reply.find("SessionID")) {
        ld.session_id_ = sid->as_string_or("");
    }
    const auto *want_ssl = reply.find("EnableSessionSSL");
    const bool need_tls = want_ssl != nullptr ? want_ssl->as_bool_or(true) : true;
    if (need_tls && !ld.tls_.handshake(ld.sock_, ld.identity_, err)) {
        return std::nullopt;
    }
    return ld;
}

std::optional<Lockdown::ServiceEndpoint> Lockdown::start_service(std::string_view name,
                                                                 std::string &err) {
    plist::Value req = plist::Value::Dict();
    req.set("Request", plist::Value::Str("StartService"));
    req.set("Service", plist::Value::Str(std::string(name)));
    plist::Value reply;
    if (!request(req, reply, err)) {
        return std::nullopt;
    }
    if (const auto *e = reply.find("Error"); e != nullptr) {
        err = "StartService(" + std::string(name) + ") 被拒: " + e->as_string_or("?");
        return std::nullopt;
    }
    const auto *port = reply.find("Port");
    if (port == nullptr) {
        err = "StartService 回复里没有 Port";
        return std::nullopt;
    }
    ServiceEndpoint ep;
    ep.port = static_cast<uint16_t>(port->as_int_or(0));
    const auto *ssl = reply.find("EnableServiceSSL");
    ep.requires_tls = ssl != nullptr && ssl->as_bool_or(false);
    return ep;
}

}  // namespace scrctl::transport
