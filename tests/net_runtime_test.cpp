#include "MemoryTunnel.h"
#include "net/LwipRuntime.h"
#include "net/Stack.h"
#include "net/TcpStream.h"
#include "net/UdpSocket.h"
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
