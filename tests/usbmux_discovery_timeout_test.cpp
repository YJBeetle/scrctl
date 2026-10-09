#include "transport/Usbmux.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <thread>
#ifndef _WIN32
#include <cstdlib>
#include <sys/un.h>
#include <unistd.h>
#endif

using namespace std::chrono_literals;
using namespace scrctl::transport;
namespace {
using Clock = std::chrono::steady_clock;
int failures = 0;
void check(bool ok, const char *what) {
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

uint32_t get_u32(const uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
void put_u32(uint8_t *p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(value >> (8 * i));
}

// 无系统服务、固定 TCP 端口或 socketpair；两端都走真实 connect/accept。
class Server {
public:
    detail::UsbmuxDiscoveryEndpoint endpoint;
    std::atomic<bool> stopped{false};
    std::atomic<bool> valid_request{false};
    std::atomic<size_t> response_bytes{0};
    Socket listener;
    Socket peer;
    using Handler = std::function<void(Server &)>;

    explicit Server(Handler handler, bool listen = true) {
        std::string err;
        if (!initialize_sockets(err)) throw std::runtime_error(err);
#ifdef _WIN32
        listener.reset(::socket(AF_INET, SOCK_STREAM, 0));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#else
        // sun_path 在 macOS 只有 104 字节，使用短的私有临时目录。
        char directory[] = "/tmp/scrctl-mux-XXXXXX";
        if (!::mkdtemp(directory)) throw std::runtime_error("mkdtemp failed");
        directory_ = directory;
        endpoint.path = directory_ + "/socket";
        listener.reset(::socket(AF_UNIX, SOCK_STREAM, 0));
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, endpoint.path.c_str(), endpoint.path.size() + 1);
#endif
        if (!listener.valid() || ::bind(listener.fd(), reinterpret_cast<sockaddr *>(&address), sizeof(address)))
            throw std::runtime_error("private bind failed");
#ifdef _WIN32
        socklen_t size = sizeof(address);
        if (::getsockname(listener.fd(), reinterpret_cast<sockaddr *>(&address), &size))
            throw std::runtime_error("getsockname failed");
        endpoint.port = ntohs(address.sin_port);
#endif
        if (!listen) return;
        if (::listen(listener.fd(), 4)) throw std::runtime_error("listen failed");
        worker_ = std::thread([this, handler = std::move(handler)] {
            while (!stopped) {
                if (wait_socket(listener.fd(), false, 10) > 0) break;
            }
            if (stopped) return;
            peer.reset(::accept(listener.fd(), nullptr, nullptr));
            if (!peer.valid()) return;
            std::string err;
            peer.set_read_timeout(500, err);
            std::array<uint8_t, 16> header{};
            if (!peer.read_exact(header.data(), header.size(), err)) return;
            const uint32_t total = get_u32(header.data());
            if (total < 16 || total > 4096 || get_u32(header.data() + 4) != 1 ||
                get_u32(header.data() + 8) != 8 || get_u32(header.data() + 12) != 1) return;
            std::string body(total - 16, '\0');
            if (!peer.read_exact(body.data(), body.size(), err)) return;
            while (!body.empty() && body.back() == '\0') body.pop_back();
            auto request = scrctl::plist::parse(body);
            const auto *type = request ? request->find("MessageType") : nullptr;
            valid_request = type && type->as_string_or() == "ListDevices";
            if (valid_request) handler(*this);
        });
    }
    ~Server() {
        stopped = true;
        if (worker_.joinable()) worker_.join();
        peer.close();
        listener.close();
#ifndef _WIN32
        ::unlink(endpoint.path.c_str());
        ::rmdir(directory_.c_str());
#endif
    }
    bool pause(std::chrono::milliseconds delay) {
        const auto until = Clock::now() + delay;
        while (!stopped && Clock::now() < until) std::this_thread::sleep_for(1ms);
        return !stopped;
    }
    bool send(const void *data, size_t length) {
        std::string err;
        if (stopped || !peer.write_all(data, length, err)) return false;
        response_bytes += length;
        return true;
    }
private:
    std::thread worker_;
#ifndef _WIN32
    std::string directory_;
#endif
};

std::vector<uint8_t> response() {
    using scrctl::plist::Value;
    auto properties = Value::Dict();
    properties.set("SerialNumber", Value::Str("private-test-device"));
    properties.set("ConnectionType", Value::Str("USB"));
    properties.set("ProductID", Value::Int(123));
    auto record = Value::Dict();
    record.set("DeviceID", Value::Int(7));
    record.set("Properties", std::move(properties));
    auto devices = Value::Array();
    devices.push(std::move(record));
    auto reply = Value::Dict();
    reply.set("DeviceList", std::move(devices));
    auto xml = scrctl::plist::write(reply);
    xml.push_back('\0');
    std::vector<uint8_t> bytes(16 + xml.size());
    put_u32(bytes.data(), static_cast<uint32_t>(bytes.size()));
    put_u32(bytes.data() + 4, 1);
    put_u32(bytes.data() + 8, 8);
    put_u32(bytes.data() + 12, 1);
    std::memcpy(bytes.data() + 16, xml.data(), xml.size());
    return bytes;
}

struct Attempt {
    UsbmuxDiscoveryStatus status;
    std::vector<DeviceRecord> records;
    std::string error;
    std::chrono::milliseconds elapsed;
};
Attempt enumerate(Server &server, std::chrono::milliseconds budget = 1000ms,
                  const std::function<bool()> &cancel = {}) {
    Attempt result;
    result.records.push_back({99, "stale", "USB", 0});
    result.error = "stale error";
    const auto start = Clock::now();
    result.status = detail::list_usbmux_devices_at(
        server.endpoint, result.records, budget, cancel, result.error);
    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
    return result;
}
void timeout_case(const char *name, const Server::Handler &handler, bool expect_progress) {
    Server server(handler);
    const auto result = enumerate(server);
    std::printf("%s: status=%d elapsed_ms=%lld response_bytes=%zu\n", name,
                int(result.status), static_cast<long long>(result.elapsed.count()), server.response_bytes.load());
    check(server.valid_request, "server received actual ListDevices request");
    check(result.status == UsbmuxDiscoveryStatus::timed_out && result.records.empty(),
          "incomplete enumeration times out without partial/stale records");
    // 允许慢 CI 调度余量；进展必须不能重置或按 header/payload 分别再给 1 秒。
    check(result.elapsed >= 900ms && result.elapsed < 1400ms, "one absolute 1s transaction budget");
    if (expect_progress) check(server.response_bytes > 0, "timeout case received real partial progress");
}
void cancellation_case(const char *name, const Server::Handler &handler,
                       std::chrono::milliseconds budget = 1000ms, bool expect_cancel = true) {
    Server server(handler);
    std::atomic<bool> cancelled{false};
    std::atomic<bool> enumeration_finished{false};
    bool cancellation_sent = false; // worker writes; main reads only after join.
    Clock::time_point requested;
    const auto fixture_start = Clock::now();
    std::thread canceller([&] {
        const auto handshake_deadline = Clock::now() + 500ms;
        while (!server.valid_request && !enumeration_finished && Clock::now() < handshake_deadline)
            std::this_thread::sleep_for(1ms);
        if (!server.valid_request || enumeration_finished) return;
        const auto cancel_at = Clock::now() + 85ms;
        while (!enumeration_finished && Clock::now() < cancel_at) std::this_thread::sleep_for(1ms);
        if (enumeration_finished) return;
        requested = Clock::now();
        cancellation_sent = true;
        cancelled.store(true, std::memory_order_release);
    });
    const auto result = enumerate(server, budget, [&] { return cancelled.load(std::memory_order_acquire); });
    const auto finished = Clock::now();
    enumeration_finished.store(true, std::memory_order_release);
    canceller.join();
    check(Clock::now() - fixture_start < 750ms, "canceller fixture cannot wait indefinitely for a request");
    if (expect_cancel) {
        check(server.valid_request, "cancellation case received an actual ListDevices request");
        check(cancellation_sent, "cancellation case actually issued cancellation");
        check(result.status == UsbmuxDiscoveryStatus::cancelled && result.records.empty(), "cancellation stops socket I/O");
    } else {
        check(!server.valid_request && !cancellation_sent && result.status == UsbmuxDiscoveryStatus::timed_out,
              "early enumeration exit releases canceller without issuing cancellation");
    }
    if (cancellation_sent) {
        const auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(finished - requested);
        std::printf("%s: status=%d elapsed_ms=%lld cancel_latency_ms=%lld cancellation_sent=1\n", name, int(result.status),
                    static_cast<long long>(result.elapsed.count()), static_cast<long long>(latency.count()));
        check(latency >= 0ms && latency <= 80ms, "50ms cancellation polling slice with 30ms scheduling tolerance");
    } else {
        std::printf("%s: status=%d elapsed_ms=%lld cancel_latency_ms=not-requested cancellation_sent=0\n", name,
                    int(result.status), static_cast<long long>(result.elapsed.count()));
    }
}
} // namespace

int main() {
    try {
#ifdef _WIN32
        std::puts("endpoint: private loopback TCP");
#else
        std::puts("endpoint: private AF_UNIX");
#endif
        const auto reply = response();
        {
            Server server([&](Server &s) { s.send(reply.data(), reply.size()); });
            const auto result = enumerate(server);
            check(result.status == UsbmuxDiscoveryStatus::complete && result.error.empty() &&
                  result.records.size() == 1 && result.records[0].device_id == 7 &&
                  result.records[0].udid == "private-test-device" && result.records[0].is_usb() &&
                  result.records[0].product_id == 123, "valid reply preserves device record fields");
            std::printf("complete: elapsed_ms=%lld\n", static_cast<long long>(result.elapsed.count()));
        }
        const auto silence = [](Server &s) { while (s.pause(10ms)) {} };
        timeout_case("no_reply", silence, false);
        timeout_case("partial_header", [&](Server &s) { s.send(reply.data(), 8); silence(s); }, true);
        timeout_case("trickle_header", [&](Server &s) {
            for (size_t i = 0; i < 16 && !s.stopped; ++i) {
                if (!s.send(reply.data() + i, 1) || !s.pause(90ms)) break;
            }
        }, true);
        timeout_case("trickle_payload", [&](Server &s) {
            if (!s.send(reply.data(), 16)) return;
            for (size_t i = 16; i < reply.size() && !s.stopped; ++i) {
                if (!s.send(reply.data() + i, 1) || !s.pause(10ms)) break;
            }
        }, true);
        timeout_case("shared_header_payload_budget", [&](Server &s) {
            if (!s.pause(700ms) || !s.send(reply.data(), 16) || !s.pause(600ms)) return;
            s.send(reply.data() + 16, reply.size() - 16);
        }, true);
        cancellation_case("cancel_header", silence);
        cancellation_case("cancel_payload", [&](Server &s) { s.send(reply.data(), 16); silence(s); });
        cancellation_case("early_enumeration_exit", silence, 0ms, false);
        {
            Server server(silence);
            const auto result = enumerate(server, 1000ms, [] { return true; });
            check(result.status == UsbmuxDiscoveryStatus::cancelled && result.records.empty() &&
                  result.elapsed < 50ms && !server.valid_request, "pre-cancellation never contacts listener");
            const auto expired = enumerate(server, 0ms);
            check(expired.status == UsbmuxDiscoveryStatus::timed_out && expired.records.empty() &&
                  expired.elapsed < 50ms && !server.valid_request, "zero budget never contacts listener");
        }
        {
            Server server(silence, false);
            const auto result = enumerate(server);
            std::printf("closed_endpoint: status=%d error=\"%s\" error_bytes=%zu elapsed_ms=%lld request_received=%d records=%zu\n",
                        int(result.status), result.error.c_str(), result.error.size(),
                        static_cast<long long>(result.elapsed.count()), int(server.valid_request.load()), result.records.size());
            // Windows 的非阻塞 loopback 拒绝可能晚于预算；SO_ERROR 先到为 unavailable，
            // 绝对 deadline 先到为 timed_out。两者都必须保留各自的错误语义和耗时上限。
            const bool refused = result.status == UsbmuxDiscoveryStatus::unavailable && !result.error.empty();
            const bool expired = result.status == UsbmuxDiscoveryStatus::timed_out && result.error.empty() &&
                                 result.elapsed >= 900ms;
            check(refused || expired, "closed endpoint reports connection refusal or deadline expiry");
            check(result.records.empty() && !server.valid_request, "closed endpoint returns no devices or accepted request");
            check(result.elapsed < 1400ms, "closed endpoint remains within the absolute 1s budget plus scheduling tolerance");
        }
        {
            Server server([](Server &s) { s.peer.close(); });
            const auto result = enumerate(server);
            std::printf("peer_eof: status=%d error=\"%s\" error_bytes=%zu elapsed_ms=%lld request_received=%d records=%zu\n",
                        int(result.status), result.error.c_str(), result.error.size(),
                        static_cast<long long>(result.elapsed.count()), int(server.valid_request.load()), result.records.size());
            check(server.valid_request, "EOF peer received an actual ListDevices request before closing");
            check(result.status == UsbmuxDiscoveryStatus::unavailable && !result.error.empty() &&
                  result.records.empty(), "connected peer EOF reports an unavailable source with an error");
            check(result.elapsed < 1400ms, "connected peer EOF is bounded by the discovery budget");
        }
        {
            Server server([](Server &s) {
                std::array<uint8_t, 16> header{};
                put_u32(header.data(), 0xffffffff);
                s.send(header.data(), header.size());
            });
            const auto result = enumerate(server);
            check(result.status == UsbmuxDiscoveryStatus::unavailable && !result.error.empty() &&
                  result.records.empty(), "oversize reply is rejected before payload allocation");
        }
#ifndef _WIN32
        std::puts("connect backlog note: AF_UNIX EAGAIN is an immediate source failure; no portable pending-backlog assertion");
#endif
        std::printf("usbmux discovery: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
        return failures ? 1 : 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
