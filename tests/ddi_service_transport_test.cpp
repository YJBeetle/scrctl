#include "ddi/ServiceTransport.h"
#include "transport/TcpConnect.h"
#include "net/ByteStream.h"
#include <atomic>
#include <cstdio>
#include <thread>
#include <deque>

namespace {
namespace ddi = scrctl::ddi;
using Socket = scrctl::transport::Socket;
using V = scrctl::plist::Value;
using Ms = std::chrono::milliseconds;
int failures = 0;
void check(bool ok, const char *what) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failures; }
bool pair(Socket &client, Socket &peer) {
    std::string error;
    if (!scrctl::transport::initialize_sockets(error)) return false;
    Socket server(::socket(AF_INET, SOCK_STREAM, 0));
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof(address);
    if (!server.valid() || ::bind(server.fd(), reinterpret_cast<sockaddr *>(&address), length) ||
        ::getsockname(server.fd(), reinterpret_cast<sockaddr *>(&address), &length) || ::listen(server.fd(), 1)) return false;
    auto connected = scrctl::transport::connect_tcp("127.0.0.1", ntohs(address.sin_port), 1000, error);
    if (!connected) return false;
    client = std::move(*connected); peer.reset(::accept(server.fd(), nullptr, nullptr));
    return peer.valid();
}
std::string frame(const V &value) {
    auto body = scrctl::plist::write(value); const auto length = static_cast<uint32_t>(body.size());
    std::string data{char(length >> 24), char(length >> 16), char(length >> 8), char(length)};
    return data + body;
}
void fragmentation() {
    Socket client, peer; check(pair(client, peer), "Create local byte-protocol connection");
    if (!peer.valid()) return;
    auto adapter = ddi::detail::service_transport_with_socket(std::move(client), {}); auto service = adapter->service();
    auto first = V::Dict(); first.set("Status", V::Str("ReceiveBytesAck"));
    auto second = V::Dict(); second.set("Status", V::Str("Complete"));
    auto expected = frame(first) + frame(second);
    bool peer_ok = false;
    std::thread writer([&] {
        std::string error; std::vector<uint8_t> request;
        if (!peer.read_len_prefixed_be(request, error)) return;
        auto parsed = scrctl::plist::parse(std::string_view(reinterpret_cast<const char *>(request.data()), request.size()));
        if (!parsed || !parsed->find("Command") || parsed->find("Command")->string != "ReceiveBytes") return;
        // Split every length-header byte; combine the second frame into a single write.
        for (size_t i = 0; i < 4; ++i) {
            if (!peer.write_all(expected.data() + i, 1, error)) return;
            std::this_thread::sleep_for(Ms(2));
        }
        peer_ok = peer.write_all(expected.data() + 4, expected.size() - 4, error);
    });
    auto request = V::Dict(); request.set("Command", V::Str("ReceiveBytes")); std::string error;
    auto response1 = service.request(request, Ms(2000), error);
    auto response2 = service.receive(Ms(2000), error);
    writer.join();
    check(peer_ok && response1 && response1->find("Status")->string == "ReceiveBytesAck" &&
        response2 && response2->find("Status")->string == "Complete", "Fragmented BE32 headers and coalesced replies retain independent frame boundaries");
    const std::string raw("image\0tail", 10);
    check(service.send_bytes(reinterpret_cast<const uint8_t *>(raw.data()), raw.size(), Ms(1000), error), "Upload accepts binary raw bytes without plist framing");
    char actual[10]; check(peer.read_exact(actual, sizeof(actual), error) && std::string(actual, 10) == raw, "Raw upload remains byte-for-byte identical");
}
void bounded(bool cancel) {
    Socket client, peer; check(pair(client, peer), "Create silent-peer connection"); if (!peer.valid()) return;
    std::atomic<bool> cancelled{false};
    auto adapter = ddi::detail::service_transport_with_socket(std::move(client), [&] { return cancelled.load(); });
    auto service = adapter->service(); std::string error;
    std::thread trigger;
    if (cancel) trigger = std::thread([&] { std::this_thread::sleep_for(Ms(40)); cancelled = true; });
    const auto start = std::chrono::steady_clock::now();
    auto response = service.receive(cancel ? Ms(1000) : Ms(50), error);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    if (trigger.joinable()) trigger.join();
    check(!response && elapsed < Ms(500) && error.find(cancel ? "cancelled" : "timed out") != std::string::npos,
        cancel ? "Cancellation interrupts a blocked receive and joins watcher before teardown" : "Deadline interrupts a silent peer rather than waiting for TCP EOF");
}
void invalid_frame() {
    Socket client, peer; check(pair(client, peer), "Create invalid-frame connection"); if (!peer.valid()) return;
    auto adapter = ddi::detail::service_transport_with_socket(std::move(client), {}); auto service = adapter->service();
    std::string error; const uint8_t header[]{0xff,0xff,0xff,0xff}; peer.write_all(header, 4, error);
    check(!service.receive(Ms(1000), error) && error.find("frame length") != std::string::npos,
        "Oversized peer length is rejected before allocation");
}
void tcp_fragments() {
    class Stream final : public scrctl::net::ByteStream {
    public:
        std::deque<std::vector<uint8_t>> packets;
        bool send(std::string_view, std::string &) override { return true; }
        bool recv(std::vector<uint8_t> &out, int, std::string &error, bool *timed_out) override {
            if (timed_out) *timed_out = false;
            if (packets.empty()) { error = "fake stream exhausted"; return false; }
            out = std::move(packets.front()); packets.pop_front(); return true;
        }
    } stream;
    auto first = V::Dict(); first.set("Status", V::Str("ReceiveBytesAck"));
    auto second = V::Dict(); second.set("Status", V::Str("Complete"));
    const auto body = frame(first) + frame(second);
    for (size_t offset = 0; offset < body.size(); offset += 3)
        stream.packets.emplace_back(body.begin() + offset, body.begin() + std::min(offset + 3, body.size()));
    auto adapter = ddi::detail::service_transport_with_byte_stream(stream, [] {}, {}); auto service = adapter->service();
    std::string error;
    auto a = service.receive(Ms(1000), error), b = service.receive(Ms(1000), error);
    check(a && b && a->find("Status")->string == "ReceiveBytesAck" && b->find("Status")->string == "Complete" && stream.packets.empty(),
        "TCP recv chunks can split all headers and bodies while preserving leftover bytes for the next frame");
}
}
int main() {
    fragmentation(); bounded(false); bounded(true); invalid_frame(); tcp_fragments();
    return failures ? 1 : 0;
}
