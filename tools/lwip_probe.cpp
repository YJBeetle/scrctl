// 独立验证 lwIP 的 IPv6 包接口与 TCP 行为；生产仍使用现有协议栈。
#include "net/ByteStream.h"
#include "remote/Rsd.h"
#include "transport/Lockdown.h"
#include "transport/TcpConnect.h"
#include "wifi/PairVerify.h"
#include "wifi/RemotePairing.h"
#include <charconv>
extern "C" {
#include "lwip/init.h"
#include "lwip/ip6.h"
#include "lwip/netif.h"
#include "lwip/sys.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "lwip/udp.h"
}
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace scrctl;
using Bytes = std::vector<uint8_t>;
bool virtual_time = true;
u32_t clock_ms = 0;
const auto epoch = std::chrono::steady_clock::now();
void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
void check(err_t code, const char *operation) {
    if (code != ERR_OK)
        throw std::runtime_error(std::string(operation) + ": lwIP error " + std::to_string(code));
}

// 输出只排队，不在 lwIP 回调里再次调用 input 或执行阻塞 TLS 写入。
// 这也使丢包和重排注入发生在真实的 IPv6 包边界上。
class Engine {
  public:
    struct Interface {
        netif nic{};
        Engine *owner;
    };
    struct Packet {
        Interface *source;
        Bytes bytes;
    };
    std::vector<std::unique_ptr<Interface>> interfaces;
    std::deque<Packet> output;
    std::function<bool(std::string &)> step;
    Engine() { lwip_init(); }
    ~Engine() {
        for (auto &i : interfaces)
            netif_remove(&i->nic);
    }
    Interface &add(const std::string &address, uint16_t mtu) {
        ip6_addr_t ip{};
        require(ip6addr_aton(address.c_str(), &ip), "invalid IPv6 address");
        auto i = std::make_unique<Interface>();
        i->owner = this;
        require(netif_add_noaddr(&i->nic, i.get(), initialize, ip6_input), "netif_add");
        netif_ip6_addr_set(&i->nic, 0, &ip);
        netif_ip6_addr_set_state(&i->nic, 0, IP6_ADDR_PREFERRED);
        i->nic.mtu = mtu;
        netif_set_link_up(&i->nic);
        netif_set_up(&i->nic);
        netif_set_default(&i->nic);
        interfaces.push_back(std::move(i));
        return *interfaces.back();
    }
    void input(Interface &i, const Bytes &bytes) {
        require(!bytes.empty() && bytes.size() <= 65535, "invalid packet size");
        pbuf *p = pbuf_alloc(PBUF_RAW, static_cast<u16_t>(bytes.size()), PBUF_RAM);
        require(p, "pbuf allocation failed");
        const auto copied = pbuf_take(p, bytes.data(), static_cast<u16_t>(bytes.size()));
        if (copied != ERR_OK) {
            pbuf_free(p);
            check(copied, "pbuf_take");
        }
        const auto status = i.nic.input(p, &i.nic);
        if (status != ERR_OK)
            pbuf_free(p);
        check(status, "IPv6 input");
    }
    bool poll(std::string &err) {
        sys_check_timeouts();
        return step && step(err);
    }

  private:
    static err_t initialize(netif *n) {
        n->name[0] = 'c';
        n->name[1] = 'd';
        n->output_ip6 = emit;
        return ERR_OK;
    }
    static err_t emit(netif *n, pbuf *p, const ip6_addr_t *) {
        auto &i = *static_cast<Interface *>(n->state);
        Bytes bytes(p->tot_len);
        if (pbuf_copy_partial(p, bytes.data(), p->tot_len, 0) != p->tot_len)
            return ERR_BUF;
        i.owner->output.push_back({&i, std::move(bytes)});
        return ERR_OK;
    }
};

class Connection : public net::ByteStream {
    Engine &engine_;
    tcp_pcb *pcb_ = nullptr;
    bool established_ = false;
    std::string failure_;
    static Connection &self(void *p) { return *static_cast<Connection *>(p); }
    void callbacks() {
        tcp_arg(pcb_, this);
        tcp_recv(pcb_, received);
        tcp_err(pcb_, failed);
        tcp_sent(pcb_, sent);
        tcp_nagle_disable(pcb_);
    }
    static err_t opened(void *p, tcp_pcb *, err_t code) {
        if (code == ERR_OK)
            self(p).established_ = true;
        return code;
    }
    static err_t received(void *p, tcp_pcb *pcb, pbuf *buffer, err_t code) {
        auto &c = self(p);
        if (!buffer) {
            c.eof = true;
            return ERR_OK;
        }
        if (code != ERR_OK)
            return code;
        // 暂不归还窗口时 lwIP 会保留该 pbuf 并重试，避免无限积累应用缓冲。
        if (c.inbound.size() + buffer->tot_len > (16u << 20))
            return ERR_MEM;
        const size_t offset = c.inbound.size();
        c.inbound.resize(offset + buffer->tot_len);
        pbuf_copy_partial(buffer, c.inbound.data() + offset, buffer->tot_len, 0);
        tcp_recved(pcb, buffer->tot_len);
        pbuf_free(buffer);
        return ERR_OK;
    }
    static void failed(void *p, err_t code) {
        auto &c = self(p);
        c.pcb_ = nullptr; // error 回调发生前 lwIP 已释放 PCB。
        c.established_ = false;
        c.failure_ = "TCP terminated: lwIP error " + std::to_string(code);
    }
    static err_t sent(void *p, tcp_pcb *, u16_t length) {
        self(p).acknowledged += length;
        return ERR_OK;
    }

  public:
    Bytes inbound;
    bool eof = false;
    size_t acknowledged = 0;
    explicit Connection(Engine &engine) : engine_(engine) {}
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;
    ~Connection() { abort(); }
    void adopt(tcp_pcb *pcb) {
        pcb_ = pcb;
        established_ = true;
        callbacks();
    }
    bool connect(Engine::Interface &i, const std::string &peer, uint16_t port, std::string &err) {
        ip_addr_t ip{};
        if (!ipaddr_aton(peer.c_str(), &ip))
            return err = "invalid peer address", false;
        pcb_ = tcp_new_ip_type(IPADDR_TYPE_V6);
        if (!pcb_)
            return err = "TCP allocation failed", false;
        callbacks();
        tcp_bind_netif(pcb_, &i.nic);
        ip_addr_t local{};
        ip_addr_copy_from_ip6(local, *netif_ip6_addr(&i.nic, 0));
        check(tcp_bind(pcb_, &local, 0), "TCP bind");
        check(tcp_connect(pcb_, &ip, port, opened), "TCP connect");
        const auto deadline = sys_now() + 15000;
        while (!established_) {
            if (!failure_.empty())
                return err = failure_, false;
            if (static_cast<int32_t>(sys_now() - deadline) >= 0)
                return err = "TCP connect timed out", false;
            if (!engine_.poll(err))
                return false;
        }
        return true;
    }
    bool send(std::string_view data, std::string &err) override {
        err.clear();
        const auto deadline = sys_now() + 15000;
        size_t offset = 0;
        while (offset < data.size()) {
            if (!pcb_ || !established_ || !failure_.empty())
                return err = failure_.empty() ? "TCP is not connected" : failure_, false;
            const auto count = static_cast<u16_t>(
                std::min<size_t>({data.size() - offset, tcp_sndbuf(pcb_), 16384}));
            if (count) {
                const auto status =
                    tcp_write(pcb_, data.data() + offset, count, TCP_WRITE_FLAG_COPY);
                if (status == ERR_OK)
                    offset += count;
                else if (status != ERR_MEM)
                    return err = "TCP write failed", false;
            }
            check(tcp_output(pcb_), "TCP output");
            if (offset == data.size())
                return true;
            if (static_cast<int32_t>(sys_now() - deadline) >= 0)
                return err = "TCP send timed out", false;
            if (!engine_.poll(err))
                return false;
        }
        return true;
    }
    bool recv(Bytes &out, int timeout_ms, std::string &err, bool *timed_out = nullptr) override {
        out.clear();
        err.clear();
        if (timed_out)
            *timed_out = false;
        const auto deadline = sys_now() + std::max(timeout_ms, 0);
        while (inbound.empty()) {
            if (!failure_.empty() || eof)
                return err = eof ? "peer closed" : failure_, false;
            if (static_cast<int32_t>(sys_now() - deadline) >= 0) {
                if (timed_out)
                    *timed_out = true;
                else
                    err = "read timed out";
                return false;
            }
            if (!engine_.poll(err))
                return false;
        }
        out.swap(inbound);
        return true;
    }
    bool graceful_close() {
        if (!pcb_)
            return true;
        tcp_arg(pcb_, nullptr);
        tcp_recv(pcb_, nullptr);
        tcp_err(pcb_, nullptr);
        tcp_sent(pcb_, nullptr);
        const auto status = tcp_close(pcb_);
        if (status != ERR_OK) {
            callbacks();
            return false;
        }
        // 成功 close 后不再访问 PCB；协议的 FIN/重传仍由 lwIP 定时器管理。
        pcb_ = nullptr;
        established_ = false;
        return true;
    }
    void abort() {
        if (!pcb_)
            return;
        tcp_arg(pcb_, nullptr);
        tcp_err(pcb_, nullptr);
        tcp_abort(pcb_);
        pcb_ = nullptr;
        established_ = false;
    }
    bool broken() const { return !failure_.empty(); }
};

uint32_t seq(const Bytes &p) {
    return (uint32_t(p[44]) << 24) | (uint32_t(p[45]) << 16) | (uint32_t(p[46]) << 8) | p[47];
}
size_t payload(const Bytes &p) {
    return p.size() >= 60 && p[6] == 6 ? p.size() - 40 - (p[52] >> 4) * 4 : 0;
}

class Laboratory {
  public:
    Engine engine;
    Engine::Interface &client = engine.add("fd00::1", 1500);
    Engine::Interface &server = engine.add("fd00::2", 1500);
    tcp_pcb *listener = nullptr;
    std::vector<std::unique_ptr<Connection>> accepted;
    bool drop_syn = false, drop_data = false, reorder = false, drop_fin = false;
    unsigned syn_drops = 0, data_drops = 0, fin_drops = 0, reordered = 0;
    Bytes held;
    uint32_t lost_seq = 0;
    unsigned retransmissions = 0;
    Laboratory() {
        engine.step = [this](std::string &) {
            clock_ms += 25;
            size_t budget = 10000;
            while (!engine.output.empty()) {
                require(budget-- != 0, "packet dispatch did not settle");
                auto packet = std::move(engine.output.front());
                engine.output.pop_front();
                if (packet.bytes.size() >= 48 && packet.bytes[6] == 17) {
                    engine.input(packet.source == &client ? server : client, packet.bytes);
                    continue;
                }
                if (packet.bytes.size() < 60 || packet.bytes[6] != 6)
                    continue;
                auto &p = packet.bytes;
                const auto flags = p[53];
                if (packet.source == &client) {
                    if (drop_syn && (flags & 2)) {
                        drop_syn = false;
                        ++syn_drops;
                        continue;
                    }
                    if (drop_data && payload(p)) {
                        drop_data = false;
                        lost_seq = seq(p);
                        ++data_drops;
                        continue;
                    }
                    if (data_drops && payload(p) && seq(p) == lost_seq)
                        ++retransmissions;
                    if (drop_fin && (flags & 1)) {
                        drop_fin = false;
                        ++fin_drops;
                        continue;
                    }
                    engine.input(server, p);
                } else {
                    if (reorder && payload(p)) {
                        if (held.empty()) {
                            held = std::move(p);
                            continue;
                        }
                        engine.input(client, p);
                        engine.input(client, held);
                        held.clear();
                        reorder = false;
                        ++reordered;
                    } else
                        engine.input(client, p);
                }
            }
            return true;
        };
        listener = tcp_new_ip_type(IPADDR_TYPE_V6);
        require(listener, "listener allocation");
        ip_addr_t ip{};
        ipaddr_aton("fd00::2", &ip);
        check(tcp_bind(listener, &ip, 12345), "server bind");
        tcp_bind_netif(listener, &server.nic);
        listener = tcp_listen(listener);
        require(listener, "server listen");
        tcp_arg(listener, this);
        tcp_accept(listener, on_accept);
    }
    ~Laboratory() {
        accepted.clear();
        if (listener)
            tcp_close(listener);
    }
    static err_t on_accept(void *arg, tcp_pcb *pcb, err_t error) {
        if (error != ERR_OK)
            return error;
        auto &lab = *static_cast<Laboratory *>(arg);
        auto c = std::make_unique<Connection>(lab.engine);
        c->adopt(pcb);
        lab.accepted.push_back(std::move(c));
        return ERR_OK;
    }
    template <typename Predicate> void until(Predicate ready) {
        std::string err;
        for (unsigned n = 0; n < 1000; ++n) {
            if (ready())
                return;
            if (!engine.poll(err))
                throw std::runtime_error(err);
        }
        throw std::runtime_error("offline TCP condition timed out");
    }
};

class Datagram {
    udp_pcb *pcb_ = nullptr;
    static void received(void *arg, udp_pcb *, pbuf *p, const ip_addr_t *, u16_t) {
        auto &bytes = static_cast<Datagram *>(arg)->bytes;
        bytes.resize(p->tot_len);
        pbuf_copy_partial(p, bytes.data(), p->tot_len, 0);
        pbuf_free(p);
    }

  public:
    Bytes bytes;
    Datagram(Engine::Interface &i, const char *address, uint16_t port) {
        pcb_ = udp_new_ip_type(IPADDR_TYPE_V6);
        require(pcb_, "UDP allocation");
        udp_bind_netif(pcb_, &i.nic);
        ip_addr_t ip{};
        require(ipaddr_aton(address, &ip), "UDP address");
        const auto status = udp_bind(pcb_, &ip, port);
        if (status != ERR_OK) {
            udp_remove(pcb_);
            pcb_ = nullptr;
            check(status, "UDP bind");
        }
        udp_recv(pcb_, received, this);
    }
    ~Datagram() {
        if (pcb_)
            udp_remove(pcb_);
    }
    Datagram(const Datagram &) = delete;
    Datagram &operator=(const Datagram &) = delete;
    void send(const char *address, uint16_t port, const Bytes &bytes) {
        require(bytes.size() <= 1200, "UDP test payload exceeds MTU");
        ip_addr_t ip{};
        require(ipaddr_aton(address, &ip), "UDP peer address");
        pbuf *p = pbuf_alloc(PBUF_TRANSPORT, static_cast<u16_t>(bytes.size()), PBUF_RAM);
        require(p, "UDP pbuf allocation");
        const auto copied = pbuf_take(p, bytes.data(), static_cast<u16_t>(bytes.size()));
        if (copied != ERR_OK) {
            pbuf_free(p);
            check(copied, "UDP copy");
        }
        const auto sent = udp_sendto(pcb_, p, &ip, port);
        pbuf_free(p);
        check(sent, "UDP send");
    }
};

int offline() {
    Laboratory lab;
    Connection a(lab.engine), b(lab.engine);
    std::string err;
    lab.drop_syn = true;
    if (!a.connect(lab.client, "fd00::2", 12345, err))
        throw std::runtime_error(err);
    require(lab.syn_drops == 1 && lab.accepted.size() == 1, "SYN retry");
    if (!b.connect(lab.client, "fd00::2", 12345, err))
        throw std::runtime_error(err);
    require(lab.accepted.size() == 2, "two concurrent connections");
    auto &peer_a = *lab.accepted[0];
    auto &peer_b = *lab.accepted[1];
    std::string bytes(200000, 'x');
    for (size_t n = 0; n < bytes.size(); ++n)
        bytes[n] = char(n % 251);
    lab.drop_data = true;
    if (!a.send(bytes, err))
        throw std::runtime_error(err);
    lab.until(
        [&] { return peer_a.inbound.size() == bytes.size() && a.acknowledged == bytes.size(); });
    require(lab.data_drops == 1 && lab.retransmissions > 0 &&
                std::equal(bytes.begin(), bytes.end(), peer_a.inbound.begin(),
                           [](char a, uint8_t b) { return uint8_t(a) == b; }),
            "loss recovery bytes");
    lab.reorder = true;
    if (!peer_a.send(bytes, err))
        throw std::runtime_error(err);
    lab.until([&] { return a.inbound.size() == bytes.size(); });
    require(lab.reordered == 1 && std::equal(bytes.begin(), bytes.end(), a.inbound.begin(),
                                             [](char a, uint8_t b) { return uint8_t(a) == b; }),
            "out-of-order bytes");
    // 单段丢失没有后续段生成重复 ACK，需要由 RTO 定时器重传。
    lab.drop_data = true;
    const auto retries_before = lab.retransmissions;
    const auto retry_started = clock_ms;
    if (!b.send("other connection", err))
        throw std::runtime_error(err);
    lab.until([&] { return peer_b.inbound.size() == 16; });
    require(lab.retransmissions > retries_before && clock_ms - retry_started >= 250,
            "single-segment retransmission timer");
    require(peer_a.inbound.size() == bytes.size(), "connection isolation");
    Bytes read;
    require(a.recv(read, 0, err) && read.size() == bytes.size(), "receive delivered bytes");
    bool timed_out = false;
    require(!a.recv(read, 50, err, &timed_out) && timed_out && err.empty(),
            "read timeout semantics");
    lab.drop_fin = true;
    require(a.graceful_close(), "client close");
    lab.until([&] { return peer_a.eof; });
    require(lab.fin_drops == 1, "FIN retry");
    require(peer_a.graceful_close(), "server close");
    peer_b.abort();
    lab.until([&] { return b.broken(); });
    timed_out = true;
    require(!b.recv(read, 50, err, &timed_out) && !timed_out && !err.empty(),
            "RST error semantics");
    Datagram udp_client(lab.client, "fd00::1", 60000), udp_server(lab.server, "fd00::2", 60001);
    const Bytes datagram{0, 1, 2, 3, 127, 128, 254, 255, 42};
    udp_client.send("fd00::2", 60001, datagram);
    lab.until([&] { return !udp_server.bytes.empty(); });
    require(udp_server.bytes == datagram, "UDP IPv6 payload/checksum");
    udp_server.send("fd00::1", 60000, udp_server.bytes);
    lab.until([&] { return !udp_client.bytes.empty(); });
    require(udp_client.bytes == datagram, "UDP IPv6 reply/checksum");
    std::printf("PASS: IPv6 netif, SYN/data/FIN retries, reordered bytes, send buffer pressure, "
                "two connections, timeout, RST and UDP\n");
    return 0;
}

struct Session {
    transport::PacketTunnel tunnel;
    remote::PeerIdentity identity;
};

Session wifi_session(const std::string &address) {
    std::string err;
    const auto dir = wifi::default_record_dir();
    const auto ids = wifi::list_record_udids(dir, err);
    require(ids.size() == 1, "live probe requires exactly one existing pairing record");
    auto record = wifi::load_record(wifi::record_path(dir, ids.front()), err);
    if (!record)
        throw std::runtime_error(err);
    auto control = transport::connect_tcp(address, wifi::kAdvertisedPortFallback, 5000, err);
    if (!control)
        throw std::runtime_error(err);
    wifi::SocketStream stream(*control);
    wifi::FramedCarrier carrier(stream);
    wifi::Rppairing channel(carrier);
    auto verified = wifi::pair_verify(channel, *record, err);
    if (verified.outcome != wifi::VerifyOutcome::Paired)
        throw std::runtime_error(verified.error);
    const auto listener = wifi::request_tcp_listener(channel, verified.shared_secret, err);
    if (!listener)
        throw std::runtime_error(err);
    control->close();
    auto socket = transport::connect_tcp(address, *listener, 5000, err);
    if (!socket)
        throw std::runtime_error(err);
    auto tunnel =
        transport::PacketTunnel::establish_psk(std::move(*socket), verified.shared_secret, err);
    if (!tunnel)
        throw std::runtime_error(err);
    const auto uuid = remote::parse_uuid_text(record->host_identifier);
    require(uuid.has_value(), "invalid pairing host identifier");
    remote::PeerIdentity identity;
    identity.uuid = *uuid;
    return {std::move(*tunnel), identity};
}

Session usb_session() {
    std::string err;
    auto mux = transport::Usbmux::open(err);
    if (!mux)
        throw std::runtime_error(err);
    std::vector<transport::DeviceRecord> devices;
    if (!mux->list_devices(devices, err))
        throw std::runtime_error(err);
    std::erase_if(devices, [](const auto &d) { return !d.is_usb(); });
    require(devices.size() == 1, "USB probe requires exactly one USB device");
    const auto &device = devices.front();
    auto lockdown = transport::Lockdown::establish(device.device_id, device.udid, err);
    if (!lockdown)
        throw std::runtime_error(err);
    auto proxy = lockdown->start_service("com.apple.internal.devicecompute.CoreDeviceProxy", err);
    if (!proxy)
        throw std::runtime_error(err);
    auto tunnel = transport::PacketTunnel::establish(
        device.device_id, proxy->port, lockdown->identity(), proxy->requires_tls, err);
    if (!tunnel)
        throw std::runtime_error(err);
    const auto uuid = remote::parse_uuid_text(lockdown->host_id());
    require(uuid.has_value(), "invalid lockdown host identifier");
    remote::PeerIdentity identity;
    identity.uuid = *uuid;
    return {std::move(*tunnel), identity};
}

uint16_t service_port(const xpc::Value &value) {
    int64_t port = value.as_int_or(0);
    if (value.is_string()) {
        const auto &s = value.string;
        const auto parsed = std::from_chars(s.data(), s.data() + s.size(), port);
        require(parsed.ec == std::errc{} && parsed.ptr == s.data() + s.size(), "invalid port text");
    }
    require(port > 0 && port <= 65535, "invalid screenshot port");
    return static_cast<uint16_t>(port);
}

int live(const std::string &address) {
    virtual_time = false;
    std::string err;
    auto session = address.empty() ? usb_session() : wifi_session(address);
    auto &tunnel = session.tunnel;
    Engine engine;
    const auto &params = tunnel.params();
    require(params.mtu >= 1280, "invalid tunnel MTU");
    auto &nic = engine.add(params.client_address, params.mtu);
    engine.step = [&](std::string &error) {
        while (!engine.output.empty()) {
            auto p = std::move(engine.output.front());
            engine.output.pop_front();
            if (!tunnel.send_ipv6(p.bytes.data(), p.bytes.size(), error))
                return false;
        }
        std::string wait_error;
        bool timed_out = false;
        if (tunnel.wait_readable(10, wait_error, &timed_out)) {
            Bytes p;
            if (!tunnel.recv_ipv6(p, error))
                return false;
            engine.input(nic, p);
        } else if (!timed_out) {
            error = wait_error;
            return false;
        }
        return true;
    };
    Connection rsd(engine);
    if (!rsd.connect(nic, params.server_address, params.rsd_port, err))
        throw std::runtime_error(err);
    auto remote = remote::Channel::open(rsd, err);
    if (!remote)
        throw std::runtime_error(err);
    if (!remote->announce_device(session.identity, err))
        throw std::runtime_error(err);
    const auto *services = remote->peer_info()->find("Services");
    require(services && services->is_dict(), "RSD services missing");
    std::printf("PASS: lwIP TCP + existing RemoteXPC RSD, services=%zu MTU=%u\n",
                services->dict.size(), params.mtu);
    const auto *entry = services->find("com.apple.coredevice.screencaptureservice");
    require(entry && entry->is_dict(), "screenshot service missing");
    const auto port = service_port(entry->at("Port"));
    require(entry->at("Properties").at("UsesRemoteXPC").as_bool_or(false) &&
                !entry->at("Properties").at("EncryptSocketData").as_bool_or(false),
            "unsupported service transport");
    Connection screenshot(engine);
    if (!screenshot.connect(nic, params.server_address, static_cast<uint16_t>(port), err))
        throw std::runtime_error(err);
    auto service = remote::Channel::open(screenshot, err);
    if (!service)
        throw std::runtime_error(err);
    auto input = xpc::make_dict();
    xpc::dict_set(input, "displayUniqueID", xpc::make_null());
    xpc::dict_set(input, "requestedFormat", xpc::make_string("png"));
    auto request =
        remote::core_device_request("com.apple.coredevice.feature.capturescreenshot",
                                    "com.apple.coredevice.action.capturescreenshot", input);
    xpc::Value reply;
    if (!service->call(request, reply, 15000, err))
        throw std::runtime_error(err);
    const auto *result = reply.find("CoreDevice.output");
    const auto *image = result ? result->find("image") : nullptr;
    const uint8_t signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    require(image && (image->type == xpc::Type::Data || image->type == xpc::Type::FileTransfer) &&
                image->data.size() >= 8 &&
                std::equal(std::begin(signature), std::end(signature), image->data.begin()),
            "PNG response missing");
    std::printf("PASS: second simultaneous lwIP connection, image=%s PNG bytes=%zu\n",
                image->type == xpc::Type::Data ? "inline" : "FileTransfer", image->data.size());
    return 0;
}
} // namespace

extern "C" u32_t sys_now(void) {
    return virtual_time ? clock_ms
                        : static_cast<u32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 std::chrono::steady_clock::now() - epoch)
                                                 .count());
}
extern "C" uint32_t scrctl_lwip_random(void) {
    static std::mt19937 random(std::random_device{}());
    return random();
}
int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    try {
        if (argc == 1)
            return offline();
        if (argc == 2 && std::string(argv[1]) == "--usb")
            return live("");
        if (argc == 3 && std::string(argv[1]) == "--wifi")
            return live(argv[2]);
        std::fprintf(stderr, "Usage: lwip_probe [--usb | --wifi ADDRESS]\n");
        return 2;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
