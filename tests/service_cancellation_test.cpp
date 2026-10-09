#include "MemoryTunnel.h"
#include "http2/Framing.h"
#include "remote/Device.h"
#include "remote/Pasteboard.h"
#include "remote/Rsd.h"
#include "net/LwipRuntime.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <stdexcept>
#include <thread>
extern "C" {
#include "lwip/tcp.h"
}

using namespace scrctl;
using namespace std::chrono_literals;
namespace {
unsigned checks = 0;
void require(bool ok, const char *what) {
    if (!ok) throw std::runtime_error(what);
    ++checks;
}
template<class Fn> void until(Fn condition) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!condition()) {
        require(std::chrono::steady_clock::now() < deadline, "peer observation deadline");
        std::this_thread::sleep_for(2ms);
    }
}
struct Peer {
    tcp_pcb *pcb = nullptr;
    std::vector<uint8_t> bytes;
    bool credit = true;
    static err_t receive(void *arg, tcp_pcb *pcb, pbuf *p, err_t) {
        auto &self = *static_cast<Peer *>(arg);
        if (!p) return ERR_OK;
        const auto n = self.bytes.size();
        self.bytes.resize(n + p->tot_len);
        pbuf_copy_partial(p, self.bytes.data() + n, p->tot_len, 0);
        if (self.credit) tcp_recved(pcb, p->tot_len);
        pbuf_free(p);
        return ERR_OK;
    }
    static void error(void *arg, err_t) { static_cast<Peer *>(arg)->pcb = nullptr; }
};
struct Server {
    tcp_pcb *listener = nullptr;
    std::vector<std::unique_ptr<Peer>> peers;
    explicit Server(net::Stack &stack) {
        net::LwipRuntime::instance().call([&] {
            listener = tcp_new_ip_type(IPADDR_TYPE_V6);
            require(listener, "listener allocation");
            ip_addr_t ip{};
            ipaddr_aton(stack.local_text().c_str(), &ip);
            tcp_bind_netif(listener, stack.network_interface());
            require(tcp_bind(listener, &ip, 12345) == ERR_OK, "listener bind");
            listener = tcp_listen(listener);
            require(listener, "listen");
            tcp_arg(listener, this);
            tcp_accept(listener, accept);
        });
    }
    ~Server() {
        net::LwipRuntime::instance().call([&] {
            for (auto &peer : peers) if (peer->pcb) {
                tcp_arg(peer->pcb, nullptr);
                tcp_err(peer->pcb, nullptr);
                tcp_abort(peer->pcb);
            }
            if (listener) tcp_close(listener);
        });
    }
    static err_t accept(void *arg, tcp_pcb *pcb, err_t) {
        auto &self = *static_cast<Server *>(arg);
        auto peer = std::make_unique<Peer>();
        peer->pcb = pcb;
        tcp_arg(pcb, peer.get());
        tcp_recv(pcb, Peer::receive);
        tcp_err(pcb, Peer::error);
        tcp_nagle_disable(pcb);
        self.peers.push_back(std::move(peer));
        return ERR_OK;
    }
    size_t size(size_t n) {
        return net::LwipRuntime::instance().call([&] {
            return peers.size() > n ? peers[n]->bytes.size() : 0;
        });
    }
    void send(size_t n, std::span<const uint8_t> bytes) {
        net::LwipRuntime::instance().call([&] {
            require(bytes.size() < 65536 && peers.at(n)->pcb, "peer send precondition");
            require(tcp_write(peers[n]->pcb, bytes.data(), static_cast<u16_t>(bytes.size()),
                              TCP_WRITE_FLAG_COPY) == ERR_OK, "peer send");
            require(tcp_output(peers[n]->pcb) == ERR_OK, "peer output");
        });
    }
    void stop_credit(size_t n) {
        net::LwipRuntime::instance().call([&] { peers.at(n)->credit = false; });
    }
};
struct Link {
    MemoryTunnel a, b;
    net::Stack client{a, "fd00:82::1", "fd00:82::2"};
    net::Stack server{b, "fd00:82::2", "fd00:82::1"};
    std::unique_ptr<Server> peer;
    Link() {
        a.deliver = [&](auto packet) { b.inject(std::move(packet)); };
        b.deliver = [&](auto packet) { a.inject(std::move(packet)); };
        std::string err;
        require(client.start_pump(err) && server.start_pump(err), "linked stacks start");
        peer = std::make_unique<Server>(server);
    }
};
remote::ServiceInfo info(bool xpc = true) {
    remote::ServiceInfo out;
    out.name = "test.pasteboard";
    out.port = 12345;
    out.uses_remote_xpc = xpc;
    return out;
}
std::unique_ptr<remote::ServiceConnection> open(Link &link, std::stop_token token = {}) {
    const auto index = net::LwipRuntime::instance().call([&] { return link.peer->peers.size(); });
    auto opening = std::async(std::launch::async, [&] {
        std::string err;
        auto conn = remote::ServiceConnection::open(link.client, info(), err, false, token);
        if (!conn) throw std::runtime_error(err);
        return conn;
    });
    until([&] { return link.peer->size(index) >= http2::kClientPrefaceSize; });
    link.peer->send(index, http2::settings_frame({}));
    require(opening.wait_for(2s) == std::future_status::ready, "XPC open completes");
    return opening.get();
}
void stopped_before_connect() {
    MemoryTunnel tunnel;
    std::atomic<unsigned> packets{0};
    tunnel.deliver = [&](auto) { ++packets; };
    net::Stack stack(tunnel, "fd00:83::1", "fd00:83::2");
    std::string err;
    require(stack.start_pump(err), "unpaired stack start");
    net::TcpStream tcp(stack);
    tcp.close();
    require(!tcp.connect(12345, err) && !err.empty(), "closed endpoint cannot reconnect");
    require(packets == 0, "close before connect emits no packet");
    std::stop_source cancelled;
    cancelled.request_stop();
    require(!remote::ServiceConnection::open(stack, info(), err, false, cancelled.get_token()) &&
            err == "Service connection cancelled", "pre-cancel rejects service before connect");
    remote::Rsd rsd(stack, {});
    require(!rsd.connect_service("not present", err, false, cancelled.get_token()) &&
            err == "Service connection cancelled", "RSD pre-cancel precedes directory lookup");
    remote::Device device;
    require(!device.connect("not present", err, false, cancelled.get_token()) &&
            err == "Service connection cancelled", "Device pre-cancel precedes session lookup");
    require(!remote::Pasteboard::set_text(device, "中文🙂", err, false, cancelled.get_token()) &&
            err == "Clipboard request cancelled", "SET pre-cancel precedes device contact");
    std::string out = "preserved";
    require(!remote::Pasteboard::get_text(device, out, err, false, cancelled.get_token()) &&
            err == "Clipboard request cancelled" && out == "preserved", "PULL pre-cancel preserves output");
    require(!remote::Pasteboard::set_text(device, "x", err, false, {}, -1) &&
            err == "Clipboard reply timeout must not be negative", "SET rejects negative timeout");
    require(!remote::Pasteboard::get_text(device, out, err, false, {}, -1) &&
            err == "Clipboard reply timeout must not be negative", "PULL rejects negative timeout");
    require(!remote::Pasteboard::set_text(device, "x", err) && err == "Session not established",
            "default SET preserves session failure");
}
void cancel_connect() {
    MemoryTunnel tunnel;
    std::atomic<unsigned> syns{0};
    tunnel.deliver = [&](auto packet) {
        if (packet.size() >= 54 && packet[6] == 6 && (packet[53] & 2)) ++syns;
    };
    net::Stack stack(tunnel, "fd00:84::1", "fd00:84::2");
    std::string err;
    require(stack.start_pump(err), "pending-connect stack start");
    std::stop_source stop;
    auto pending = std::async(std::launch::async, [&] {
        std::string why;
        const auto conn = remote::ServiceConnection::open(stack, info(), why, false, stop.get_token());
        return !conn && why == "Service connection cancelled";
    });
    until([&] { return syns.load() != 0; });
    stop.request_stop();
    require(pending.wait_for(1s) == std::future_status::ready && pending.get(),
            "cancel wakes real pending TCP connect without 15s timeout");
}
void cancel_handshake() {
    Link link;
    std::stop_source stop;
    auto pending = std::async(std::launch::async, [&] {
        std::string why;
        const auto conn = remote::ServiceConnection::open(link.client, info(), why, false, stop.get_token());
        return !conn && why == "Service connection cancelled";
    });
    until([&] { return link.peer->size(0) >= http2::kClientPrefaceSize; });
    stop.request_stop();
    require(pending.wait_for(1s) == std::future_status::ready && pending.get(),
            "cancel wakes real HTTP/2 SETTINGS wait");
}
void cancel_reply_and_isolation() {
    Link link;
    std::stop_source stop;
    auto cancelled = open(link, stop.get_token());
    auto healthy = open(link);
    const auto before = link.peer->size(0);
    auto pending = std::async(std::launch::async, [&] {
        std::string why;
        xpc::Value reply;
        return !cancelled->call(remote::Pasteboard::build_set("中文🙂\ntext"), reply, 20000, why) &&
               why == "Service connection cancelled";
    });
    until([&] { return link.peer->size(0) > before; });
    stop.request_stop();
    require(pending.wait_for(1s) == std::future_status::ready && pending.get(),
            "cancel wakes real 20s reply wait");
    std::string err;
    require(healthy->send_only(remote::Pasteboard::build_pull(), err),
            "other XPC endpoint still sends after cancellation");
    auto body = xpc::make_dict();
    xpc::dict_set(body, "ok", xpc::make_bool(true));
    link.peer->send(1, http2::data_frame(1, xpc::encode_message(
        xpc::kFlagAlwaysSet | xpc::kFlagDataPresent, 1, &body)));
    xpc::Value reply;
    require(healthy->wait_message(reply, 1000, err) == remote::Channel::Wait::Message &&
            reply.at("ok").boolean, "other XPC endpoint still receives after cancellation");
    const auto size = link.peer->size(0);
    require(!cancelled->send_only(remote::Pasteboard::build_pull(), err) &&
            err == "Service connection cancelled", "cancelled service rejects new request");
    require(link.peer->size(0) == size, "cancelled service emits no new request bytes");
}
void cancel_buffered_reply() {
    Link link;
    std::stop_source stop;
    auto conn = open(link, stop.get_token());
    auto body = xpc::make_dict();
    xpc::dict_set(body, "late", xpc::make_bool(true));
    link.peer->send(0, http2::data_frame(1, xpc::encode_message(
        xpc::kFlagAlwaysSet | xpc::kFlagDataPresent, 1, &body)));
    std::string err;
    // service 读入 TCP 数据再解析，确保取消时 Channel 自己已有完整回复。
    require(conn->service(1000, err) && conn->service(0, err), "buffer complete reply before stop");
    stop.request_stop();
    xpc::Value reply;
    require(conn->wait_message(reply, 20000, err) == remote::Channel::Wait::Broken &&
            err == "Service connection cancelled", "cancelled service does not return buffered success");
}
void cancel_flow_control() {
    Link link;
    std::stop_source stop;
    auto conn = open(link, stop.get_token());
    const auto before = link.peer->size(0);
    auto pending = std::async(std::launch::async, [&] {
        std::string why;
        xpc::Value reply;
        return !conn->call(remote::Pasteboard::build_set(std::string(1u << 20, 's')),
                          reply, 20000, why) && why == "Service connection cancelled";
    });
    // 默认 HTTP/2 stream window 为 65535；不给 WINDOW_UPDATE，正文在该窗口耗尽后等待。
    until([&] { return link.peer->size(0) >= before + 65000; });
    require(pending.wait_for(50ms) == std::future_status::timeout, "HTTP/2 flow control is actually waiting");
    stop.request_stop();
    require(pending.wait_for(1s) == std::future_status::ready && pending.get(),
            "cancel wakes HTTP/2 flow-control wait");
}
void cancel_tcp_send() {
    Link link;
    std::stop_source stop;
    std::string err;
    auto conn = remote::ServiceConnection::open(link.client, info(false), err, false, stop.get_token());
    require(conn != nullptr, "plain service open");
    until([&] { return net::LwipRuntime::instance().call([&] { return !link.peer->peers.empty(); }); });
    link.peer->stop_credit(0);
    auto pending = std::async(std::launch::async, [&] {
        std::string why;
        return !conn->tcp().send(std::string(2u << 20, 'x'), why) && !why.empty();
    });
    until([&] { return link.peer->size(0) >= 400000; });
    require(pending.wait_for(50ms) == std::future_status::timeout, "TCP send buffer is actually blocked");
    stop.request_stop();
    require(pending.wait_for(1s) == std::future_status::ready && pending.get(),
            "connection lifetime callback wakes blocked TCP sender");
}
void destroy_registration() {
    Link link;
    for (unsigned n = 0; n != 8; ++n) {
        std::stop_source stop;
        std::string err;
        auto conn = remote::ServiceConnection::open(link.client, info(false), err, false, stop.get_token());
        require(conn != nullptr, "lifetime service open");
        auto cancelling = std::async(std::launch::async, [&] { stop.request_stop(); });
        conn.reset();
        require(cancelling.wait_for(1s) == std::future_status::ready, "callback destruction rendezvous completes");
        cancelling.get();
        require(!stop.request_stop(), "registration removed before repeated cancellation");
    }
}
} // namespace
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        stopped_before_connect();
        cancel_connect();
        cancel_handshake();
        cancel_reply_and_isolation();
        cancel_buffered_reply();
        cancel_flow_control();
        cancel_tcp_send();
        destroy_registration();
        std::printf("PASS: %u checks, service cancellation and endpoint isolation\n", checks);
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
