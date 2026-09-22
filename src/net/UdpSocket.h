#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "net/Stack.h"

namespace scrctl::net {

/// 隧道内的一个 UDP 端点。
///
/// 存在的唯一理由：CoreDevice 的媒体流是设备**反向推**到我们隧道地址上的某个
/// UDP 端口的（startmediastream 的 receiverIP / receiverPort 就是干这个的）。
/// 所以必须能在用户态栈里绑一个端口并收数据报。
///
/// 不支持：组播、分片、校验和为 0 的收包（IPv6 里 0 表示"没算"，按规范要丢）。
class UdpSocket : public UdpEndpoint {
public:
    UdpSocket(Stack &stack, uint16_t local_port) : stack_(stack), local_port_(local_port) {}
    ~UdpSocket() override;

    /// 登记到复用层。之后落到这个端口的数据报就会进 recv 队列。
    bool bind(std::string &err);

    /// 发给隧道对端的某个端口。
    bool send(const std::vector<uint8_t> &payload, uint16_t peer_port, std::string &err);

    /// 取一个已到达的数据报；队列空时按 timeout 驱动复用层收包。
    bool recv(std::vector<uint8_t> &payload, uint16_t &peer_port, int timeout_ms,
              std::string &err);

    [[nodiscard]] uint16_t local_port() const { return local_port_; }
    [[nodiscard]] std::size_t buffered() const { return queue_.size(); }

    void on_datagram(const uint8_t *l4, std::size_t len) override;

private:
    struct Packet {
        uint16_t peer_port = 0;
        std::vector<uint8_t> payload;
    };

    Stack &stack_;
    uint16_t local_port_;
    bool bound_ = false;
    std::deque<Packet> queue_;
    uint64_t dropped_ = 0;  ///< 校验和错或长度不合法的包数，排查时要知道有没有在丢
};

}  // namespace scrctl::net
