#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
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
/// 数据报由复用层的泵线程放进来（on_datagram），消费方在 recv 上等条件变量。
/// 端点自己不读隧道——一条字节流上两个读者会互相偷包。
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

    /// 取一个已到达的数据报；队列空时最多等 timeout_ms。
    bool recv(std::vector<uint8_t> &payload, uint16_t &peer_port, int timeout_ms,
              std::string &err);

    [[nodiscard]] uint16_t local_port() const { return local_port_; }
    [[nodiscard]] std::size_t buffered() const;
    /// 队列满时丢掉的数据报数。镜像卡住不收时先看这个。
    [[nodiscard]] std::size_t dropped() const;

    void on_datagram(const uint8_t *l4, std::size_t len) override;

private:
    struct Packet {
        uint16_t peer_port = 0;
        std::vector<uint8_t> payload;
    };

    /// 队列上限。到顶就丢最老的：宁可让消费方看到几段缺口，也不能让一个不读
    /// 的接收端把内存吃光——那条隧道上还有别的连接要靠泵线程活着。
    static constexpr std::size_t kMaxQueue = 4096;

    Stack &stack_;
    uint16_t local_port_;
    bool bound_ = false;

    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<Packet> queue_;
    std::size_t dropped_ = 0;  ///< 校验和错、长度不合法或队列满的包数
};

}  // namespace scrctl::net
