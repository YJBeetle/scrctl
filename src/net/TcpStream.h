#pragma once
#include "net/ByteStream.h"
#include "net/Stack.h"
#include <memory>
namespace scrctl::net {
// 同步字节接口适配 lwIP raw TCP。协议状态只在核心线程访问；应用线程等待
// 独立的接收队列、连接结果与发送空间，不占用核心线程或隧道线程。
class TcpStream : public ByteStream {
public:
  explicit TcpStream(Stack &stack);
  ~TcpStream() override;
  TcpStream(const TcpStream &) = delete;
  TcpStream &operator=(const TcpStream &) = delete;
  bool connect(uint16_t peer_port, std::string &err);
  bool send(std::string_view data, std::string &err) override;
  bool recv(std::vector<uint8_t> &out, int timeout_ms, std::string &err,
            bool *timed_out = nullptr) override;
  void close();
  bool connected() const;

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};
} // namespace scrctl::net
