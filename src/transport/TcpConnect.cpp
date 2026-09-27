#include "transport/TcpConnect.h"

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace scrctl::transport {

std::optional<Socket> connect_tcp(const std::string &host, uint16_t port, int timeout_ms,
                                  std::string &err) {
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
        const int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            last_error = std::strerror(errno);
            continue;
        }
#ifdef SO_NOSIGPIPE
        // 对端半关闭之后一次 write 就会把整个进程带走（SIGPIPE）。usbmux 那条路
        // 上从没遇到，是因为 usbmuxd 总是干净地关掉连接；局域网里设备睡觉、路由器
        // 重启都会给一个 RST，所以这里必须开。
        const int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        // 非阻塞 + poll：connect 卡住的原因（ARP 丢包、设备睡眠、防火墙静默丢）
        // 从现象上都看不出区别，所以一定要能超时。
        const int flags = ::fcntl(fd, F_GETFL, 0);
        bool nonblocking = false;
        if (timeout_ms > 0 && flags >= 0) {
            nonblocking = ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
        }
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
            if (!(errno == EINPROGRESS && nonblocking)) {
                last_error = std::strerror(errno);
                ::close(fd);
                continue;
            }
            pollfd pfd{fd, POLLOUT, 0};
            const int ready = ::poll(&pfd, 1, timeout_ms);
            int so_error = 0;
            socklen_t len = sizeof(so_error);
            if (ready <= 0 ||
                ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) != 0 || so_error != 0) {
                last_error =
                    ready <= 0 ? std::string("连接超时") + std::to_string(timeout_ms) + "ms"
                               : std::strerror(so_error == 0 ? errno : so_error);
                ::close(fd);
                continue;
            }
        }
        if (nonblocking) {
            ::fcntl(fd, F_SETFL, flags);
        }
        return Socket(fd);
    }
    err = "连不上 " + host + ":" + service + (last_error.empty() ? std::string() : "：" + last_error);
    return std::nullopt;
}

}  // namespace scrctl::transport
