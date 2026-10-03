#include "transport/TcpConnect.h"

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace scrctl::transport {

std::optional<Socket> connect_tcp(const std::string &host, uint16_t port, int timeout_ms,
                                  std::string &err) {
    err.clear();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    const std::string service = std::to_string(port);
    addrinfo *first = nullptr;
    const int rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &first);
    if (rc != 0 || first == nullptr) {
        err = "解析地址 " + host + " 失败: " + (rc == 0 ? "没有结果" : gai_strerror(rc));
        return std::nullopt;
    }
    std::unique_ptr<addrinfo, void (*)(addrinfo *)> addresses(first, ::freeaddrinfo);

    // 逐个地址试：一个主机名同时有 A 和 AAAA、或者 v6 先回来但这台机器没 v6 路由，
    // 都是局域网里会遇到的事，只试第一个会变成"偶尔连不上"。
    std::string last_error;
    for (const addrinfo *ai = first; ai != nullptr; ai = ai->ai_next) {
        Socket socket(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
        if (!socket.valid()) {
            last_error = std::strerror(errno);
            continue;
        }
        // Socket 从创建起拥有 fd，所有失败分支自动关闭，并统一处理 SIGPIPE。
        const int fd = socket.fd();
        int flags = 0;
        const bool nonblocking = timeout_ms > 0;
        if (nonblocking) {
            flags = ::fcntl(fd, F_GETFL, 0);
            if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
                last_error = "设置非阻塞失败：" + std::string(std::strerror(errno));
                continue;
            }
        }
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
            if (!(errno == EINPROGRESS && nonblocking)) {
                last_error = std::strerror(errno);
                continue;
            }
            pollfd pfd{fd, POLLOUT, 0};
            // EINTR 后只等待剩余时间，避免重试一次就重置整个连接超时。
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(timeout_ms);
            int ready = 0;
            do {
                const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()).count();
                if (remaining <= 0) {
                    ready = 0;
                    break;
                }
                ready = ::poll(&pfd, 1, static_cast<int>(remaining));
            } while (ready < 0 && errno == EINTR);
            if (ready <= 0) {
                last_error = ready == 0 ? "连接超时" + std::to_string(timeout_ms) + "ms"
                                        : "等待连接失败：" + std::string(std::strerror(errno));
                continue;
            }
            int so_error = 0;
            socklen_t len = sizeof(so_error);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) != 0 || so_error != 0) {
                last_error = std::strerror(so_error == 0 ? errno : so_error);
                continue;
            }
        }
        // 交出去的 Socket read_exact/write_all 按阻塞 fd 工作，恢复失败就不交付它。
        if (nonblocking && ::fcntl(fd, F_SETFL, flags) != 0) {
            last_error = "恢复阻塞模式失败：" + std::string(std::strerror(errno));
            continue;
        }
        return socket;
    }
    err = "连不上 " + host + ":" + service + (last_error.empty() ? std::string() : "：" + last_error);
    return std::nullopt;
}

}  // namespace scrctl::transport
