#include "transport/TcpConnect.h"

#include <arpa/inet.h>
#include <cstdio>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
int failures = 0;
void check(bool ok, const char *what) {
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}
} // namespace

int main() {
    using scrctl::transport::Socket;
    Socket server(::socket(AF_INET, SOCK_STREAM, 0));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (!server.valid() ||
        ::bind(server.fd(), reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        std::perror("bind loopback");
        return 1;
    }
    socklen_t size = sizeof(address);
    if (::getsockname(server.fd(), reinterpret_cast<sockaddr *>(&address), &size) != 0) {
        return 1;
    }
    const uint16_t port = ntohs(address.sin_port);
    std::string err;
    // 占住端口但不 listen，连接应失败；平台可能拒绝或丢弃 SYN，不依赖外网。
    const int next_fd = ::dup(server.fd());
    check(next_fd >= 0, "descriptor baseline");
    if (next_fd >= 0)
        ::close(next_fd);
    for (int i = 0; i < 32; ++i) {
        check(!scrctl::transport::connect_tcp("127.0.0.1", port, 20, err) && !err.empty(),
              "failed connections report an error");
    }
    const int after = ::dup(server.fd());
    check(after == next_fd, "repeated failures do not leak descriptors");
    if (after >= 0)
        ::close(after);
    if (::listen(server.fd(), 4) != 0)
        return 1;
    for (int timeout : {0, 1000}) {
        err = "old error";
        auto client = scrctl::transport::connect_tcp("localhost", port, timeout, err);
        check(client.has_value() && err.empty(), "hostname connection succeeds and clears error");
        if (!client)
            continue;
        const int flags = ::fcntl(client->fd(), F_GETFL, 0);
        check(flags >= 0 && (flags & O_NONBLOCK) == 0, "returned socket is blocking");
        Socket peer(::accept(server.fd(), nullptr, nullptr));
        check(peer.valid(), "server accepts connection");
        if (!peer.valid())
            continue;
        char received = 0;
        check(client->write_all("x", 1, err) && peer.read_exact(&received, 1, err) &&
                  received == 'x',
              "connected socket transfers bytes");
    }
    return failures ? 1 : 0;
}
