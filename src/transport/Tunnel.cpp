#include "Tunnel.h"

#include <cstring>

#include "json/Json.h"

namespace scrctl::transport {
namespace {

constexpr std::string_view kMagic = "CDTunnel";
constexpr size_t kControlHeaderLen = 10;  // magic(8) + u16 长度
constexpr size_t kIpv6HeaderLen = 40;
constexpr uint16_t kRequestedMtu = 16000;  // 与参考实现的 TCP 隧道一致

void put_be16(uint8_t *p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}

uint16_t get_be16(const uint8_t *p) {
    return static_cast<uint16_t>(uint16_t(p[0]) << 8 | p[1]);
}

}  // namespace

PacketTunnel::PacketTunnel(PacketTunnel &&) noexcept = default;
PacketTunnel &PacketTunnel::operator=(PacketTunnel &&) noexcept = default;

bool PacketTunnel::write_all(const void *data, size_t len, std::string &err) {
    if (tls_.handle() != nullptr) {
        const int n = SSL_write(tls_.handle(), data, static_cast<int>(len));
        if (n != static_cast<int>(len)) {
            return err = "隧道 TLS 写失败", false;
        }
        return true;
    }
    return sock_.write_all(data, len, err);
}

bool PacketTunnel::read_all(void *data, size_t len, std::string &err) {
    if (tls_.handle() != nullptr) {
        auto *p = static_cast<uint8_t *>(data);
        size_t got = 0;
        while (got < len) {
            const int n = SSL_read(tls_.handle(), p + got, static_cast<int>(len - got));
            if (n <= 0) {
                return err = "隧道 TLS 读失败", false;
            }
            got += static_cast<size_t>(n);
        }
        return true;
    }
    return sock_.read_exact(data, len, err);
}

std::optional<PacketTunnel> PacketTunnel::establish(uint32_t device_id, uint16_t proxy_port,
                                                   const PemIdentity &identity, bool use_tls,
                                                   std::string &err) {
    auto mux = Usbmux::open(err);
    if (!mux) {
        return std::nullopt;
    }
    auto sock = mux->connect(device_id, proxy_port, err);
    if (!sock) {
        return std::nullopt;
    }

    PacketTunnel t;
    t.sock_ = std::move(*sock);
    if (use_tls && !t.tls_.handshake(t.sock_, identity, err)) {
        return std::nullopt;
    }

    json::Value req;
    req.kind = json::Kind::Object_;
    req.object["type"] = [] {
        json::Value v;
        v.kind = json::Kind::String;
        v.string = "clientHandshakeRequest";
        return v;
    }();
    req.object["mtu"] = [] {
        json::Value v;
        v.kind = json::Kind::Int;
        v.integer = kRequestedMtu;
        return v;
    }();
    const std::string body = json::write(req);

    std::vector<uint8_t> frame(kControlHeaderLen + body.size());
    std::memcpy(frame.data(), kMagic.data(), kMagic.size());
    put_be16(frame.data() + 8, static_cast<uint16_t>(body.size()));
    std::memcpy(frame.data() + kControlHeaderLen, body.data(), body.size());
    if (!t.write_all(frame.data(), frame.size(), err)) {
        return std::nullopt;
    }

    uint8_t hdr[kControlHeaderLen];
    if (!t.read_all(hdr, kControlHeaderLen, err)) {
        return std::nullopt;
    }
    if (std::memcmp(hdr, kMagic.data(), kMagic.size()) != 0) {
        err = "隧道握手回复的 magic 不对";
        return std::nullopt;
    }
    const uint16_t payload_len = get_be16(hdr + 8);
    std::vector<uint8_t> payload(payload_len);
    if (!payload.empty() && !t.read_all(payload.data(), payload.size(), err)) {
        return std::nullopt;
    }

    auto parsed = json::parse(std::string_view(
        reinterpret_cast<const char *>(payload.data()), payload.size()));
    if (!parsed) {
        err = "隧道握手回复不是合法 JSON";
        return std::nullopt;
    }
    const auto *cp = parsed->find("clientParameters");
    const json::Value *cp_mtu = nullptr;
    if (cp != nullptr) {
        t.params_.client_address = cp->find("address") ? cp->find("address")->as_string_or("")
                                                       : "";
        // MTU 在 clientParameters 里，不在顶层——顶层取会拿到 0。
        cp_mtu = cp->find("mtu");
    }
    t.params_.server_address =
        parsed->find("serverAddress") ? parsed->find("serverAddress")->as_string_or("") : "";
    t.params_.rsd_port =
        static_cast<uint16_t>(parsed->find("serverRSDPort") ? parsed->find("serverRSDPort")->as_int_or(0) : 0);
    const auto *top_mtu = parsed->find("mtu");
    t.params_.mtu = static_cast<uint16_t>(
        cp_mtu != nullptr ? cp_mtu->as_int_or(0) : (top_mtu != nullptr ? top_mtu->as_int_or(0) : 0));

    if (t.params_.client_address.empty() || t.params_.server_address.empty() ||
        t.params_.rsd_port == 0) {
        err = "隧道握手回复缺少必要字段";
        return std::nullopt;
    }
    return t;
}

bool PacketTunnel::wait_readable(int ms, std::string &err) {
    // TLS 记录可能已被 SSL_read 拆进内部缓冲，此时 fd 上无可读事件，
    // 必须先问 SSL_pending，否则会白等超时。
    if (tls_.handle() != nullptr && SSL_pending(tls_.handle()) > 0) {
        return true;
    }
    return sock_.wait_readable(ms, err);
}

bool PacketTunnel::send_ipv6(const uint8_t *packet, size_t len, std::string &err) {
    if (!sock_.valid()) {
        err = "隧道未连接";
        return false;
    }
    // 一次一个包。合并写会破坏 CoreDeviceProxy 的读边界，导致隧道死亡。
    return write_all(packet, len, err);
}

bool PacketTunnel::recv_ipv6(std::vector<uint8_t> &out, std::string &err) {
    if (!sock_.valid()) {
        err = "隧道未连接";
        return false;
    }
    uint8_t hdr[kIpv6HeaderLen];
    if (!read_all(hdr, kIpv6HeaderLen, err)) {
        return false;
    }
    if ((hdr[0] >> 4) != 6) {
        err = "隧道内不是 IPv6 包: version nibble=" + std::to_string(hdr[0] >> 4);
        return false;
    }
    const size_t total = kIpv6HeaderLen + get_be16(hdr + 4);
    out.resize(total);
    std::memcpy(out.data(), hdr, kIpv6HeaderLen);
    return read_all(out.data() + kIpv6HeaderLen, total - kIpv6HeaderLen, err);
}

}  // namespace scrctl::transport
