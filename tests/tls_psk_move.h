#pragma once
#include "socket_pair.h"
#include "transport/TlsChannel.h"
#include <algorithm>
#include <array>
#include <future>
#include <memory>

namespace scrctl_test {
inline constexpr std::array<unsigned char, 32> move_psk{1, 2, 3, 4};
inline unsigned move_server_psk(SSL *, const char *identity, unsigned char *out,
                                unsigned capacity) {
    if (!identity || *identity || capacity < move_psk.size())
        return 0;
    std::copy(move_psk.begin(), move_psk.end(), out);
    return static_cast<unsigned>(move_psk.size());
}
inline bool accept_psk(scrctl::transport::Socket &socket) {
    auto context = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>(
        SSL_CTX_new(TLS_server_method()), SSL_CTX_free);
    if (!context || SSL_CTX_set_min_proto_version(context.get(), TLS1_2_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(context.get(), TLS1_2_VERSION) != 1 ||
        SSL_CTX_set_cipher_list(context.get(), "PSK-AES128-GCM-SHA256") != 1)
        return false;
    SSL_CTX_set_psk_server_callback(context.get(), move_server_psk);
    auto ssl = std::unique_ptr<SSL, decltype(&SSL_free)>(SSL_new(context.get()), SSL_free);
    char bytes[4]{};
    const bool ok = ssl && SSL_set_fd(ssl.get(), static_cast<int>(socket.fd())) == 1 &&
                    SSL_accept(ssl.get()) == 1 && SSL_read(ssl.get(), bytes, 4) == 4 &&
                    std::string(bytes, 4) == "ping" && SSL_write(ssl.get(), "pong", 4) == 4;
    if (ssl)
        SSL_shutdown(ssl.get());
    return ok;
}
inline bool exchange(SSL *ssl) {
    char reply[4]{};
    return SSL_write(ssl, "ping", 4) == 4 && SSL_read(ssl, reply, 4) == 4 &&
           std::string(reply, 4) == "pong";
}

// 销毁移动来源后，先验证现有 TLS 收发，再通过 SSL_clear 开始新的握手。
// 第二次握手强制重新调用 PSK 回调，不会仅因第一次握手成功而漏掉 ex_data 悬空指针。
template <typename Check> void tls_psk_moves(Check check) {
    using namespace scrctl::transport;
    for (bool assignment : {false, true}) {
        Socket client, server;
        std::string err;
        if (!make_test_socket_pair(client, server, err)) {
            check(false, err.c_str());
            return;
        }
        client.set_read_timeout(2000, err);
        server.set_read_timeout(2000, err);
        auto peer = std::async(std::launch::async, [&] { return accept_psk(server); });
        auto original = std::make_unique<TlsChannel>();
        if (!original->handshake_psk(client, {move_psk.begin(), move_psk.end()}, err)) {
            check(false, err.c_str());
            client.interrupt();
            server.interrupt();
            peer.get();
            continue;
        }
        std::unique_ptr<TlsChannel> moved;
        if (assignment) {
            moved = std::make_unique<TlsChannel>();
            *moved = std::move(*original);
        } else {
            moved = std::make_unique<TlsChannel>(std::move(*original));
        }
        original.reset();
        const bool first = exchange(moved->handle());
        check(first, assignment ? "move assignment retains established TLS I/O"
                                : "move construction retains established TLS I/O");
        if (!first) {
            client.interrupt();
            server.interrupt();
        }
        check(peer.get(), "PSK peer authenticates and exchanges bytes");
        SSL_shutdown(moved->handle());
        Socket next_client, next_server;
        if (!make_test_socket_pair(next_client, next_server, err)) {
            check(false, err.c_str());
            continue;
        }
        next_client.set_read_timeout(2000, err);
        next_server.set_read_timeout(2000, err);
        auto next_peer = std::async(std::launch::async, [&] { return accept_psk(next_server); });
        const bool connected =
            SSL_clear(moved->handle()) == 1 && SSL_set_session(moved->handle(), nullptr) == 1 &&
            SSL_set_fd(moved->handle(), static_cast<int>(next_client.fd())) == 1 &&
            SSL_connect(moved->handle()) == 1;
        const bool second = connected && exchange(moved->handle());
        check(second, assignment
                          ? "PSK callback survives move assignment and source destruction"
                          : "PSK callback survives move construction and source destruction");
        if (!second) {
            next_client.interrupt();
            next_server.interrupt();
        }
        check(next_peer.get(), "second handshake authenticates the original PSK");
        // TlsChannel 不拥有 socket，必须在新的 socket 被析构之前释放 TLS。
        moved.reset();
    }
}
} // namespace scrctl_test
