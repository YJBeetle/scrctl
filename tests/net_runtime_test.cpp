#include "MemoryTunnel.h"
#include "net/LwipRuntime.h"
#include "net/Stack.h"
#include "net/TcpStream.h"
#include "net/UdpSocket.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
extern "C" {
#include "lwip/tcp.h"
}
using namespace scrctl::net;
using namespace std::chrono_literals;
namespace {
void require(bool good, const char *what) {
  if (!good)
    throw std::runtime_error(what);
}
struct Peer {
  tcp_pcb *pcb = nullptr;
  std::vector<uint8_t> bytes;
  bool closed = false;
  bool credit = true;
  static err_t receive(void *arg, tcp_pcb *pcb, pbuf *p, err_t) {
    auto &self = *static_cast<Peer *>(arg);
    if (!p) {
      self.closed = true;
      return ERR_OK;
    }
    const auto n = self.bytes.size();
    self.bytes.resize(n + p->tot_len);
    pbuf_copy_partial(p, self.bytes.data() + n, p->tot_len, 0);
    if (self.credit)
      tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
  }
  static void error(void *arg, err_t) {
    static_cast<Peer *>(arg)->pcb = nullptr;
  }
};
struct Server {
  Stack &stack;
  tcp_pcb *listener = nullptr;
  std::vector<std::unique_ptr<Peer>> peers;
  explicit Server(Stack &s) : stack(s) {
    LwipRuntime::instance().call([&] {
      listener = tcp_new_ip_type(IPADDR_TYPE_V6);
      require(listener, "listener allocation");
      ip_addr_t ip{};
      ipaddr_aton(stack.local_text().c_str(), &ip);
      tcp_bind_netif(listener, stack.network_interface());
      require(tcp_bind(listener, &ip, 12345) == ERR_OK, "listen bind");
      listener = tcp_listen(listener);
      require(listener, "listen");
      tcp_arg(listener, this);
      tcp_accept(listener, accept);
    });
  }
  ~Server() {
    LwipRuntime::instance().call([&] {
      for (auto &peer : peers)
        if (peer->pcb) {
          tcp_arg(peer->pcb, nullptr);
          tcp_err(peer->pcb, nullptr);
          tcp_abort(peer->pcb);
        }
      if (listener)
        tcp_close(listener);
    });
  }
  static err_t accept(void *arg, tcp_pcb *pcb, err_t) {
    auto &self = *static_cast<Server *>(arg);
    auto peer = std::make_unique<Peer>();
    peer->pcb = pcb;
    tcp_arg(pcb, peer.get());
    tcp_recv(pcb, Peer::receive);
    tcp_err(pcb, Peer::error);
    tcp_nagle_disable(pcb);
    self.peers.push_back(std::move(peer));
    return ERR_OK;
  }
  size_t size(size_t n) {
    return LwipRuntime::instance().call(
        [&] { return peers.at(n)->bytes.size(); });
  }
  void send(size_t n, const char *text, size_t length) {
    LwipRuntime::instance().call([&] {
      require(tcp_write(peers.at(n)->pcb, text, static_cast<u16_t>(length),
                        TCP_WRITE_FLAG_COPY) == ERR_OK,
              "peer send");
      tcp_output(peers.at(n)->pcb);
    });
  }
};
template <typename Fn> void until(Fn condition) {
  const auto until = std::chrono::steady_clock::now() + 10s;
  while (!condition()) {
    require(std::chrono::steady_clock::now() < until, "test deadline");
    std::this_thread::sleep_for(10ms);
  }
}
void icmp_quotes() {
  // 经 MemoryTunnel 注入完整 IPv6/ICMPv6 包，验证生产隧道线程的诊断。
  // 引用空载荷 UDP 时，ICMPv6 头 8 + IPv6 头 40 + UDP 头 8 已足够读取端口。
  MemoryTunnel tunnel;
  Stack stack(tunnel, "fd00:4::1", "fd00:4::2");
  std::string err;
  require(stack.start_pump(err), "ICMP diagnostic stack start");
  const auto diagnose = [&](size_t payload_size, size_t quote_size) {
    std::vector<uint8_t> udp(8 + payload_size, 0);
    udp[0] = 0xcf;
    udp[1] = 0x08; // 源端口 53000。
    udp[2] = 0x30;
    udp[3] = 0x39; // 目的端口 12345。
    udp[5] = static_cast<uint8_t>(udp.size());
    const auto udp_sum = l4_checksum(stack.local_addr().data(),
                                    stack.peer_addr().data(), udp.data(),
                                    udp.size(), 17);
    udp[6] = static_cast<uint8_t>(udp_sum >> 8);
    udp[7] = static_cast<uint8_t>(udp_sum);
    const auto original = stack.wrap(udp, 17);
    require(quote_size <= original.size(), "ICMP quote fixture length");
    std::vector<uint8_t> icmp(8, 0);
    icmp[0] = 1;
    icmp[1] = 4; // ICMPv6 目的端口不可达，随后引用触发报文。
    icmp.insert(icmp.end(), original.begin(), original.begin() + quote_size);
    const auto sum = l4_checksum(stack.peer_addr().data(),
                                stack.local_addr().data(), icmp.data(),
                                icmp.size(), 58);
    icmp[2] = static_cast<uint8_t>(sum >> 8);
    icmp[3] = static_cast<uint8_t>(sum);
    auto incoming = stack.wrap(icmp, 58);
    std::copy(stack.peer_addr().begin(), stack.peer_addr().end(), incoming.begin() + 8);
    std::copy(stack.local_addr().begin(), stack.local_addr().end(), incoming.begin() + 24);
    const auto before = stack.icmp_seen();
    tunnel.inject(std::move(incoming));
    until([&] { return stack.icmp_seen() > before; });
    return stack.icmp_last();
  };
  require(diagnose(0, 48).find("53000->12345") != std::string::npos,
          "minimal complete UDP quote reports ports");
  require(diagnose(8, 56).find("53000->12345") != std::string::npos,
          "UDP quote with payload reports ports");
  // UDP 头缺少一个字节或内层 IPv6 头未完整时，不越界读取、不拼出端口。
  require(diagnose(0, 47).find("53000->12345") == std::string::npos,
          "truncated UDP header does not report ports");
  require(diagnose(0, 39).find("53000->12345") == std::string::npos,
          "truncated IPv6 header does not report ports");
}
void exercise() {
  MemoryTunnel a, b;
  std::atomic<bool> drop_syn{true}, drop_data{true};
  std::atomic<unsigned> dropped{0};
  a.deliver = [&](auto packet) {
    if (packet.size() >= 60 && packet[6] == 6) {
      const auto flags = packet[53];
      const auto payload = packet.size() - 40 - (packet[52] >> 4) * 4;
      if ((flags & 2) && drop_syn.exchange(false)) {
        ++dropped;
        return;
      }
      if (payload && drop_data.exchange(false)) {
        ++dropped;
        return;
      }
    }
    b.inject(std::move(packet));
  };
  b.deliver = [&](auto packet) { a.inject(std::move(packet)); };
  Stack client(a, "fd00:1::1", "fd00:1::2"),
      server(b, "fd00:1::2", "fd00:1::1");
  std::string err;
  require(client.start_pump(err), err.c_str());
  require(server.start_pump(err), err.c_str());
  Server peer(server);
  TcpStream first(client), second(client);
  require(first.connect(12345, err), "SYN retransmission / connect");
  require(second.connect(12345, err), "second connection");
  std::string bytes(200000, 'x');
  for (size_t n = 0; n < bytes.size(); ++n)
    bytes[n] = char(n % 251);
  require(first.send(bytes, err), "TCP send buffer pressure");
  until([&] { return peer.size(0) == bytes.size(); });
  require(dropped == 2, "SYN/data drop injection");
  require(LwipRuntime::instance().call([&] {
    return std::equal(bytes.begin(), bytes.end(), peer.peers[0]->bytes.begin(),
                      [](char a, uint8_t b) { return uint8_t(a) == b; });
  }),
          "TCP loss recovery content");
  require(second.send("isolated", err), "second send");
  until([&] { return peer.size(1) == 8; });
  peer.send(0, "hello", 5);
  std::vector<uint8_t> received;
  bool timeout = true;
  require(first.recv(received, 2000, err, &timeout) && !timeout &&
              received == std::vector<uint8_t>({'h', 'e', 'l', 'l', 'o'}),
          "TCP receive");
  require(!first.recv(received, 0, err, &timeout) && timeout && err.empty(),
          "TCP timeout");
  UdpSocket udp_a(client, 60000), udp_b(server, 60001),
      duplicate(client, 60000);
  require(udp_a.bind(err) && udp_b.bind(err), "UDP bind");
  require(!duplicate.bind(err), "duplicate UDP bind rejected");
  const std::vector<uint8_t> datagram{1, 128, 255, 4, 5};
  require(udp_a.send(datagram, 60001, err), "UDP send");
  uint16_t port = 0;
  require(udp_b.recv(received, port, 2000, err) && port == 60000 &&
              received == datagram,
          "UDP real lwIP packet");
  require(!udp_a.send(std::vector<uint8_t>(16000), 60001, err),
          "UDP rejects oversized MTU");
  auto pending_tcp = std::async(std::launch::async, [&] {
    std::vector<uint8_t> bytes;
    std::string why;
    bool timed_out = false;
    return !first.recv(bytes, 30000, why, &timed_out) && !timed_out &&
           !why.empty();
  });
  auto pending_udp = std::async(std::launch::async, [&] {
    std::vector<uint8_t> bytes;
    uint16_t port;
    std::string why;
    return !udp_a.recv(bytes, port, 30000, why) && !why.empty();
  });
  a.shutdown();
  require(pending_tcp.wait_for(2s) == std::future_status::ready &&
              pending_tcp.get(),
          "disconnect wakes TCP");
  require(pending_udp.wait_for(2s) == std::future_status::ready &&
              pending_udp.get(),
          "disconnect wakes UDP");
  client.stop_pump();
  require(!client.start_pump(err), "stopped tunnel cannot restart");
}
void pending_connect() {
  MemoryTunnel tunnel;
  Stack stack(tunnel, "fd00:2::1", "fd00:2::2");
  std::string err;
  require(stack.start_pump(err), "stack start");
  TcpStream stream(stack);
  auto waiting = std::async(std::launch::async, [&] {
    std::string why;
    return !stream.connect(12345, why) && !why.empty();
  });
  std::this_thread::sleep_for(50ms);
  stack.stop_pump();
  require(waiting.wait_for(2s) == std::future_status::ready && waiting.get(),
          "stop wakes pending connect");
}
void close_and_cancel() {
  // 每轮销毁整个 netif，仍处于 FIN_WAIT/TIME_WAIT 的 PCB 不能留下旧 Stack
  // 指针。
  for (unsigned round = 0; round < 8; ++round) {
    MemoryTunnel a, b;
    a.deliver = [&](auto packet) { b.inject(std::move(packet)); };
    b.deliver = [&](auto packet) { a.inject(std::move(packet)); };
    Stack client(a, "fd00:3::1", "fd00:3::2"),
        server(b, "fd00:3::2", "fd00:3::1");
    std::string err;
    require(client.start_pump(err) && server.start_pump(err),
            "lifecycle start");
    Server peer(server);
    TcpStream stream(client);
        require(stream.connect(12345, err), "lifecycle connect");
        until([&] {
            return LwipRuntime::instance().call([&] { return !peer.peers.empty(); });
        });
    if (round % 2 == 0) {
      peer.send(0, "before EOF", 10);
      LwipRuntime::instance().call([&] {
        server.close_tcp(peer.peers[0]->pcb);
        peer.peers[0]->pcb = nullptr;
      });
      std::vector<uint8_t> bytes;
      require(stream.recv(bytes, 2000, err) &&
                  std::string(bytes.begin(), bytes.end()) == "before EOF",
              "buffered bytes survive remote FIN");
      bool timeout = true;
      require(!stream.recv(bytes, 2000, err, &timeout) && !timeout &&
                  !err.empty(),
              "remote FIN is not a timeout");
      stream.close();
    } else {
      LwipRuntime::instance().call([&] { peer.peers[0]->credit = false; });
      auto sending = std::async(std::launch::async, [&] {
        std::string why;
        return !stream.send(std::string(2u << 20, 's'), why) && !why.empty();
      });
      // 对端不归还窗口；大于窗口+发送缓冲的写操作必须等待。
      until([&] { return peer.size(0) >= 400000; });
      require(sending.wait_for(100ms) == std::future_status::timeout,
              "slow peer applies backpressure");
      client.stop_pump();
      require(sending.wait_for(2s) == std::future_status::ready &&
                  sending.get(),
              "stop wakes blocked sender");
    }
  }
}
} // namespace
int main() {
  try {
    icmp_quotes();
    exercise();
    pending_connect();
    close_and_cancel();
    std::cout << "PASS: production lwIP runtime, loss, backpressure, "
                 "multi-netif, UDP and "
                 "cancellation\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
}
