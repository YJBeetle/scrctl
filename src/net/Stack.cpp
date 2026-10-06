#include "net/Stack.h"

#include "net/LwipRuntime.h"
#include <arpa/inet.h>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <utility>
extern "C" {
#include "lwip/ip6.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"
}

namespace scrctl::net {
namespace {

uint16_t get16(const uint8_t *p) {
  return static_cast<uint16_t>(p[0] << 8 | p[1]);
}
uint32_t fold_sum(uint32_t sum, const uint8_t *p, std::size_t n) {
  for (std::size_t i = 0; i + 1 < n; i += 2) {
    sum += get16(p + i);
  }
  if (n & 1) {
    sum += static_cast<uint32_t>(p[n - 1]) << 8;
  }
  return sum;
}

uint16_t finish_sum(uint32_t sum) {
  while (sum >> 16) {
    sum = (sum & 0xFFFF) + (sum >> 16);
  }
  return static_cast<uint16_t>(~sum & 0xFFFF);
}

} // namespace

uint16_t l4_checksum(const uint8_t src[16], const uint8_t dst[16],
                     const uint8_t *l4, std::size_t len, uint8_t next_header) {
  uint32_t sum = 0;
  sum = fold_sum(sum, src, 16);
  sum = fold_sum(sum, dst, 16);
  // 伪头：上层长度(4) + 3 字节零 + next header。与 L4 一起连续累加，才等价于
  // 分两段各算一半再相加。
  const uint8_t pseudo[8] = {
      static_cast<uint8_t>(len >> 24),
      static_cast<uint8_t>(len >> 16),
      static_cast<uint8_t>(len >> 8),
      static_cast<uint8_t>(len),
      0,
      0,
      0,
      next_header,
  };
  sum = fold_sum(sum, pseudo, sizeof(pseudo));
  sum = fold_sum(sum, l4, len);
  return finish_sum(sum);
}

struct Stack::Impl {
  netif nic{};
  bool registered = false;
  Stack *owner;
  std::mutex queue_mutex;
  std::deque<std::vector<uint8_t>> outgoing;
  size_t queued_bytes = 0;
  std::map<const void *, std::function<void(const std::string &)>> endpoints;
  std::set<tcp_pcb *> closing;
  static err_t init(netif *nic) {
    nic->name[0] = 's';
    nic->name[1] = 'c';
    nic->output_ip6 = output;
    return ERR_OK;
  }
  static err_t output(netif *nic, pbuf *p, const ip6_addr_t *) {
    auto &self = *static_cast<Impl *>(nic->state);
    try {
      std::vector<uint8_t> packet(p->tot_len);
      if (pbuf_copy_partial(p, packet.data(), p->tot_len, 0) != p->tot_len)
        return ERR_BUF;
      return self.owner->enqueue(std::move(packet)) ? ERR_OK : ERR_MEM;
    } catch (const std::bad_alloc &) {
      return ERR_MEM;
    }
  }
  struct Closing {
    Impl *owner;
    tcp_pcb *pcb;
    static void destroyed(u8_t, void *arg) {
      auto *record = static_cast<Closing *>(arg);
      record->owner->closing.erase(record->pcb);
      delete record;
    }
  };
};

Stack::Stack(transport::PacketIo &tunnel, std::string local, std::string peer)
    : tunnel_(tunnel), local_text_(std::move(local)),
      peer_text_(std::move(peer)), impl_(std::make_unique<Impl>()) {
  impl_->owner = this;
  addresses_valid_ =
      inet_pton(AF_INET6, local_text_.c_str(), local_addr_.data()) == 1 &&
      inet_pton(AF_INET6, peer_text_.c_str(), peer_addr_.data()) == 1;
}
Stack::~Stack() { stop_pump(); }
netif *Stack::interface() {
  return impl_->registered && pumping_ && !stopping_ ? &impl_->nic : nullptr;
}
void Stack::attach_endpoint(const void *key,
                            std::function<void(const std::string &)> fail) {
  impl_->endpoints.emplace(key, std::move(fail));
}
void Stack::detach_endpoint(const void *key) { impl_->endpoints.erase(key); }
void Stack::close_tcp(tcp_pcb *pcb) {
  const auto id = LwipRuntime::instance().close_arg_id();
  static const tcp_ext_arg_callbacks callbacks{Impl::Closing::destroyed,
                                               nullptr};
  auto record =
      std::make_unique<Impl::Closing>(Impl::Closing{impl_.get(), pcb});
  impl_->closing.insert(pcb);
  tcp_ext_arg_set(pcb, id, record.release());
  tcp_ext_arg_set_callbacks(pcb, id, &callbacks);
  tcp_arg(pcb, nullptr);
  tcp_recv(pcb, nullptr);
  tcp_err(pcb, nullptr);
  tcp_sent(pcb, nullptr);
  if (tcp_close(pcb) != ERR_OK)
    tcp_abort(pcb);
}
bool Stack::start_pump(std::string &err) {
  if (!addresses_valid_ || stopping_ || pumping_)
    return err = "隧道栈无法启动：地址无效或已启动/停止", false;
  const auto mtu = tunnel_.mtu();
  if (mtu < 1280)
    return err = "隧道 MTU 小于 IPv6 最小值", false;
  const bool registered = LwipRuntime::instance().call([&] {
    ip6_addr_t ip{};
    ip6addr_aton(local_text_.c_str(), &ip);
    if (!netif_add_noaddr(&impl_->nic, impl_.get(), Impl::init, ip6_input))
      return false;
    impl_->registered = true;
    impl_->nic.mtu = mtu;
    netif_ip6_addr_set(&impl_->nic, 0, &ip);
    netif_ip6_addr_set_state(&impl_->nic, 0, IP6_ADDR_PREFERRED);
    netif_set_up(&impl_->nic);
    netif_set_link_up(&impl_->nic);
    return true;
  });
  if (!registered)
    return err = "无法注册 lwIP 隧道接口", false;
  pumping_ = true;
  try {
    pump_ = std::thread(&Stack::pump_loop, this);
  } catch (const std::system_error &e) {
    err = "无法启动隧道线程：" + std::string(e.what());
    stop_pump();
    return false;
  }
  err.clear();
  return true;
}
void Stack::fail_endpoints(const std::string &reason) {
  LwipRuntime::instance().call([&] {
    for (auto &[key, fail] : impl_->endpoints)
      fail(reason);
  });
}
void Stack::stop_pump() {
  if (!impl_->registered)
    return;
  stopping_ = true;
  {
    std::lock_guard lock(err_mu_);
    if (pump_err_.empty())
      pump_err_ = "隧道已停止";
  }
  tunnel_.shutdown(); // 中断阻塞的 TLS/包读取；fd 在线程退出之后才释放。
  fail_endpoints(pump_error());
  if (pump_.joinable())
    pump_.join();
  pumping_ = false;
  LwipRuntime::instance().call([&] {
    while (!impl_->closing.empty())
      tcp_abort(*impl_->closing.begin());
    netif_set_down(&impl_->nic);
    netif_set_link_down(&impl_->nic);
    netif_remove(&impl_->nic);
    impl_->registered = false;
  });
  std::lock_guard lock(impl_->queue_mutex);
  impl_->outgoing.clear();
  impl_->queued_bytes = 0;
}
std::string Stack::pump_error() const {
  std::lock_guard lock(err_mu_);
  return pump_err_;
}
bool Stack::enqueue(std::vector<uint8_t> packet) {
  if (stopping_ || packet.size() < 40)
    return false;
  const uint32_t label = flow_label_;
  packet[1] = static_cast<uint8_t>((packet[1] & 0xf0) | (label >> 16));
  packet[2] = static_cast<uint8_t>(label >> 8);
  packet[3] = static_cast<uint8_t>(label);
  std::lock_guard lock(impl_->queue_mutex);
  constexpr size_t max_bytes = 4u << 20;
  if (impl_->queued_bytes + packet.size() > max_bytes)
    return false;
  const auto size = packet.size();
  impl_->outgoing.push_back(std::move(packet));
  impl_->queued_bytes += size;
  return true;
}
bool Stack::send(const std::vector<uint8_t> &packet, std::string &err) {
  if (!pumping_ || !enqueue(packet))
    return err = "隧道已停止或发送队列已满", false;
  return true;
}
void Stack::pump_loop() {
  std::string error;
  try {
    while (!stopping_) {
      // 限制每轮发包数量，持续出站时仍给入站 ACK / 媒体留出处理机会。
      for (unsigned n = 0; n < 64 && !stopping_; ++n) {
        std::vector<uint8_t> packet;
        {
          std::lock_guard lock(impl_->queue_mutex);
          if (impl_->outgoing.empty())
            break;
          packet = std::move(impl_->outgoing.front());
          impl_->outgoing.pop_front();
          impl_->queued_bytes -= packet.size();
        }
        if (!tunnel_.send_ipv6(packet.data(), packet.size(), error))
          break;
      }
      if (!error.empty())
        break;
      bool timed_out = false;
      if (!tunnel_.wait_readable(5, error, &timed_out)) {
        if (timed_out)
          continue;
        break;
      }
      std::vector<uint8_t> packet;
      if (!tunnel_.recv_ipv6(packet, error))
        break;
      if (packet.size() < 40 || packet.size() > 65535 || packet[0] >> 4 != 6)
        continue;
      // 诊断计数保留；有效包仍由 lwIP 处理扩展头、TCP、UDP 和 ICMP。
      const uint8_t protocol = packet[6];
      if ((protocol == 6 || protocol == 17) && packet.size() >= 48 &&
          l4_checksum(packet.data() + 8, packet.data() + 24, packet.data() + 40,
                      packet.size() - 40, protocol) != 0)
        ++bad_checksums_;
      if (protocol == 58 && packet.size() >= 44)
        observe_icmpv6(packet.data() + 40, packet.size() - 40);
      LwipRuntime::instance().call([&] {
        if (stopping_)
          return;
        auto *p =
            pbuf_alloc(PBUF_RAW, static_cast<u16_t>(packet.size()), PBUF_RAM);
        if (!p)
          return;
        pbuf_take(p, packet.data(), static_cast<u16_t>(packet.size()));
        if (ip6_input(p, &impl_->nic) != ERR_OK)
          pbuf_free(p);
      });
    }
  } catch (const std::exception &e) {
    error = "隧道处理失败：" + std::string(e.what());
  }
  if (!stopping_) {
    {
      std::lock_guard lock(err_mu_);
      pump_err_ = error.empty() ? "隧道读取失败" : error;
    }
    stopping_ = true;
    fail_endpoints(pump_error());
  }
  pumping_ = false;
}
std::vector<uint8_t> Stack::wrap(const std::vector<uint8_t> &l4,
                                 uint8_t protocol) const {
  std::vector<uint8_t> packet(40 + l4.size());
  packet[0] = 0x60;
  packet[4] = static_cast<uint8_t>(l4.size() >> 8);
  packet[5] = static_cast<uint8_t>(l4.size());
  packet[6] = protocol;
  packet[7] = 64;
  std::copy(local_addr_.begin(), local_addr_.end(), packet.begin() + 8);
  std::copy(peer_addr_.begin(), peer_addr_.end(), packet.begin() + 24);
  std::copy(l4.begin(), l4.end(), packet.begin() + 40);
  return packet;
}

std::string Stack::icmp_last() const {
  std::lock_guard<std::mutex> lock(icmp_mu_);
  return icmp_last_;
}

bool Stack::send_echo_request(uint16_t ident, uint16_t seq, std::string &err) {
  // ICMPv6 的报文格式：type / code / 校验和(2)，回音请求再跟 id / seq / 数据。
  // 算校验和时该字段置 0，伪头用 next header = 58。
  std::vector<uint8_t> msg = {128,
                              0,
                              0,
                              0,
                              static_cast<uint8_t>(ident >> 8),
                              static_cast<uint8_t>(ident),
                              static_cast<uint8_t>(seq >> 8),
                              static_cast<uint8_t>(seq)};
  const std::string tag = "scrctl-canary";
  msg.insert(msg.end(), tag.begin(), tag.end());
  const uint16_t sum = l4_checksum(local_addr_.data(), peer_addr_.data(),
                                   msg.data(), msg.size(), 58);
  msg[2] = static_cast<uint8_t>(sum >> 8);
  msg[3] = static_cast<uint8_t>(sum);
  return send(wrap(msg, 58), err);
}

/// 把 ICMPv6 头部（以及错误消息里带的那个内层 IPv6 包头）记下来。
///
/// 不校验它的 L4 校验和：上面那段只对 next=6/17
/// 验和，而这一位是"设备有没有答话"的
/// 存在性证据，验和失败也不该把它当成没发生。
void Stack::observe_icmpv6(const uint8_t *icmp, std::size_t len) {
  if (len < 4) {
    return;
  }
  const uint8_t type = icmp[0];
  const uint8_t code = icmp[1];
  // ICMPv6 的 type=1 码表和 ICMPv4 的**不一样**，别照抄：v4 的"端口不可达"是
  // code 3， v6 的是 code 4（照 v4
  // 抄会把金丝雀那条读成"地址不可达"，意思整个反了）。
  static constexpr const char *kCodes[] = {
      "没有路由到目的",    "与管理策略禁止通信", "超出源地址的范围",
      "地址不可达",        "端口不可达",         "源地址被入/出站策略禁止",
      "到目的的路由被拒绝"};
  std::string line =
      "ICMPv6 type=" + std::to_string(type) + " code=" + std::to_string(code);
  if (type == 1 && code < std::size(kCodes)) {
    line += std::string("（") + kCodes[code] + "）";
  } else if (type == 2) {
    line += "（包太大）";
  } else if (type == 3) {
    line += "（TTL 耗尽）";
  } else if (type == 4) {
    line += "（参数问题）";
  } else if (type == 128 || type == 129) {
    line += type == 128 ? "（回音请求）" : "（回音应答）";
  }
  // 错误消息（type 1..4）在第 8 字节之后回带触发它的那个包：内层 IPv6 头 40
  // 字节， 再往后是触发包 L4 头的前 8 字节——对 UDP 来说刚好是
  // 源端口/目的端口/长度/校验和。 这一串才是分界线："设备回过端口不可达的那个 4
  // 元组，是不是我们发 RTCP 的那个"。
  if (type <= 4 && len >= 8 + 40 + 8) {
    const uint8_t *inner = icmp + 8;
    const std::size_t rest = len - 8;
    const uint8_t inner_nh = inner[6];
    char src[64] = "?";
    char dst[64] = "?";
    inet_ntop(AF_INET6, inner + 8, src, sizeof(src));
    inet_ntop(AF_INET6, inner + 24, dst, sizeof(dst));
    line += " 内层 nh=" + std::to_string(inner_nh) + " " + src + " -> " + dst;
    if (inner_nh == 17 && rest >= 48 + 8) {
      const uint16_t sp = get16(inner + 40);
      const uint16_t dp = get16(inner + 42);
      line += " 端口 " + std::to_string(sp) + "->" + std::to_string(dp);
    }
  }
  {
    std::lock_guard<std::mutex> lock(icmp_mu_);
    ++icmp_seen_;
    if (type == 129) {
      ++echo_replies_;
    }
    icmp_last_ = line;
  }
  std::fprintf(stderr, "    <- %s\n", line.c_str());
}

} // namespace scrctl::net
