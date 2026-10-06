#pragma once
#include "net/Stack.h"
#include <memory>
namespace scrctl::net {
// lwIP UDP 的同步数据报接口；接收队列按包数与总字节数限制，满时丢最老的数据。
class UdpSocket {
public:
  UdpSocket(Stack &stack, uint16_t local_port);
  ~UdpSocket();
  UdpSocket(const UdpSocket &) = delete;
  UdpSocket &operator=(const UdpSocket &) = delete;
  bool bind(std::string &err);
  bool send(const std::vector<uint8_t> &payload, uint16_t peer_port,
            std::string &err);
  bool recv(std::vector<uint8_t> &payload, uint16_t &peer_port, int timeout_ms,
            std::string &err);
  uint16_t local_port() const;
  size_t buffered() const;
  size_t dropped() const;

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};
} // namespace scrctl::net
