#include "socket_pair.h"
#include "transport/TlsChannel.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <future>
#include <thread>

namespace {
int failures = 0;
void check(bool ok, const char *what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++failures;
}

void io_lifecycle() {
    using namespace scrctl::transport;
    Socket a, b;
    std::string err;
    if (!make_test_socket_pair(a, b, err)) {
        check(false, err.c_str());
        return;
    }
    bool timed_out = false;
    err = "old error";
    check(!b.wait_readable(0, err, &timed_out) && timed_out && err.empty(),
          "poll timeout clears old error");
    check(a.write_all("hello", 5, err), "write bytes");
    timed_out = true;
    check(b.wait_readable(1000, err, &timed_out) && !timed_out, "data clears timeout flag");
    char text[5]{};
    check(b.read_exact(text, 5, err) && std::string(text, 5) == "hello", "exact read");
    check(b.set_read_timeout(50, err), "set native read timeout");
    check(!b.read_exact(text, 1, err) && !err.empty(), "recv timeout reports error");
    b.close();
    timed_out = true;
    check(!b.wait_readable(0, err, &timed_out) && !timed_out, "closed handle is not a timeout");
    // 主动 RST 后，普通 send 和 OpenSSL BIO 都应返回错误；Windows 不使用 SIGPIPE。
    Socket reset_peer;
    if (!make_test_socket_pair(a, reset_peer, err)) {
        check(false, err.c_str());
        return;
    }
    linger immediate{1, 0};
    ::setsockopt(reset_peer.fd(), SOL_SOCKET, SO_LINGER, reinterpret_cast<const char *>(&immediate),
                 sizeof(immediate));
    reset_peer.close();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    check(!a.write_all("x", 1, err) && !err.empty(),
          "RST makes send fail without terminating process");
    BIO *bio = BIO_new_socket(static_cast<int>(a.fd()), BIO_NOCLOSE);
    check(bio && BIO_write(bio, "x", 1) <= 0, "OpenSSL socket BIO reports closed peer");
    BIO_free(bio);
}
void interruption() {
    using namespace scrctl::transport;
    Socket a, b;
    std::string err;
    if (!make_test_socket_pair(a, b, err)) {
        check(false, err.c_str());
        return;
    }
    std::promise<void> started;
    auto ready = started.get_future();
    auto read = std::async(std::launch::async, [&] {
        char byte = 0;
        std::string reason;
        started.set_value();
        return !b.read_exact(&byte, 1, reason) && !reason.empty();
    });
    ready.wait();
    b.interrupt();
    check(read.wait_for(std::chrono::seconds(1)) == std::future_status::ready,
          "shutdown interrupts blocking recv");
    check(read.get(), "interrupted read reports failure");
    check(b.valid(), "interrupt retains handle ownership");
    const auto handle = b.release();
    check(!b.valid(), "release invalidates old owner");
    Socket next(handle), moved(std::move(next));
    check(moved.valid() && !next.valid(), "move transfers native handle");
}
constexpr std::array<unsigned char, 32> psk{1, 2, 3, 4};
unsigned server_psk(SSL *, const char *identity, unsigned char *out, unsigned capacity) {
    if (!identity || *identity || capacity < psk.size())
        return 0;
    std::copy(psk.begin(), psk.end(), out);
    return psk.size();
}
void tls_psk() {
    using namespace scrctl::transport;
    Socket client, server;
    std::string err;
    if (!make_test_socket_pair(client, server, err)) {
        check(false, err.c_str());
        return;
    }
    client.set_read_timeout(2000, err);
    server.set_read_timeout(2000, err);
    auto peer = std::async(std::launch::async, [&] {
        SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
        if (!ctx)
            return false;
        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
        SSL_CTX_set_max_proto_version(ctx, TLS1_2_VERSION);
        SSL_CTX_set_cipher_list(ctx, "PSK-AES128-GCM-SHA256");
        SSL_CTX_set_psk_server_callback(ctx, server_psk);
        SSL *ssl = SSL_new(ctx);
        bool ok =
            ssl && SSL_set_fd(ssl, static_cast<int>(server.fd())) == 1 && SSL_accept(ssl) == 1;
        char text[4]{};
        ok = ok && SSL_read(ssl, text, 4) == 4 && std::string(text, 4) == "ping";
        ok = ok && SSL_write(ssl, "pong", 4) == 4;
        if (ssl) {
            SSL_shutdown(ssl);
            SSL_free(ssl);
        }
        SSL_CTX_free(ctx);
        return ok;
    });
    TlsChannel channel;
    const bool connected = channel.handshake_psk(client, {psk.begin(), psk.end()}, err);
    check(connected, "native Windows socket completes PSK TLS handshake");
    if (connected) {
        char reply[4]{};
        check(SSL_write(channel.handle(), "ping", 4) == 4 &&
                  SSL_read(channel.handle(), reply, 4) == 4 && std::string(reply, 4) == "pong",
              "TLS data roundtrip");
    } else {
        client.interrupt();
        server.interrupt();
    }
    check(peer.get(), "TLS peer authenticated empty identity and received bytes");
}
} // namespace
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    io_lifecycle();
    interruption();
    tls_psk();
    return failures ? 1 : 0;
}
