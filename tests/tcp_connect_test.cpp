#include "transport/TcpConnect.h"

#include <cstdio>
#include <thread>
#include <chrono>
#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

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
    std::string err;
    if (!scrctl::transport::initialize_sockets(err)) return 1;
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
    // 占住端口但不 listen，连接应失败；平台可能拒绝或丢弃 SYN，不依赖外网。
#ifdef _WIN32
    // 第一次 Winsock 地址查询可能初始化常驻句柄，先预热再比较失败重试的资源数。
    scrctl::transport::connect_tcp("127.0.0.1", port, 20, err);
    DWORD before = 0;
    check(GetProcessHandleCount(GetCurrentProcess(), &before), "handle baseline");
#else
    const int next_fd = ::dup(server.fd());
    check(next_fd >= 0, "descriptor baseline");
    if (next_fd >= 0)
        ::close(next_fd);
#endif
    for (int i = 0; i < 32; ++i) {
        check(!scrctl::transport::connect_tcp("127.0.0.1", port, 20, err) && !err.empty(),
              "failed connections report an error");
    }
#ifdef _WIN32
    DWORD after = 0;
    check(GetProcessHandleCount(GetCurrentProcess(), &after) && after == before,
          "repeated failures do not leak handles");
#else
    const int after = ::dup(server.fd());
    check(after == next_fd, "repeated failures do not leak descriptors");
    if (after >= 0)
        ::close(after);
#endif
    if (::listen(server.fd(), 4) != 0)
        return 1;
    for (int timeout : {0, 1000}) {
        err = "old error";
        auto client = scrctl::transport::connect_tcp("localhost", port, timeout, err);
        check(client.has_value() && err.empty(), "hostname connection succeeds and clears error");
        if (!client)
            continue;
#ifndef _WIN32
        const int flags = ::fcntl(client->fd(), F_GETFL, 0);
        check(flags >= 0 && (flags & O_NONBLOCK) == 0, "returned socket is blocking");
#endif
        Socket peer(::accept(server.fd(), nullptr, nullptr));
        check(peer.valid(), "server accepts connection");
        if (!peer.valid())
            continue;
        char received = 0;
        // 延迟发送后，read_exact 仍应阻塞等待，不能带着连接阶段的非阻塞模式交付。
        client->set_read_timeout(1000, err);
        std::thread delayed([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            std::string reason;
            peer.write_all("y", 1, reason);
        });
        check(client->read_exact(&received, 1, err) && received == 'y', "returned socket blocks for data");
        delayed.join();
        check(client->write_all("x", 1, err) && peer.read_exact(&received, 1, err) &&
                  received == 'x',
              "connected socket transfers bytes");
    }
    return failures ? 1 : 0;
}
