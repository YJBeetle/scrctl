#include "i18n/Translation.h"
#include "net/TcpStream.h"
#include "net/LwipRuntime.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
extern "C" {
#include "lwip/tcp.h"
}
namespace scrctl::net {
struct TcpStream::Impl {
  Stack &stack;
  tcp_pcb *pcb = nullptr; // 只在核心线程访问。
  std::mutex mutex, send_mutex;
  std::condition_variable cv;
  bool started = false, established = false, eof = false, closed = false;
  uint64_t progress = 0;
  std::string failure;
  std::vector<uint8_t> received;
  explicit Impl(Stack &s) : stack(s) {}
  void fail(const std::string &reason) {
    auto *p = pcb;
    pcb = nullptr;
    {
      std::lock_guard lock(mutex);
      failure = reason;
      established = false;
      ++progress;
    }
    cv.notify_all();
    if (p) {
      tcp_arg(p, nullptr);
      tcp_err(p, nullptr);
      tcp_abort(p);
    }
  }
  static void error(void *arg, err_t code) {
    auto &self = *static_cast<Impl *>(arg);
    self.pcb = nullptr; // 此时 lwIP 已释放 PCB。
    self.fail(SCRCTL_TR("TCP connection terminated (lwIP ") + std::to_string(code) + SCRCTL_TR(")"));
  }
  static err_t connected(void *arg, tcp_pcb *, err_t code) {
    auto &self = *static_cast<Impl *>(arg);
    {
      std::lock_guard lock(self.mutex);
      self.established = code == ERR_OK;
      ++self.progress;
    }
    self.cv.notify_all();
    return code;
  }
  static err_t receive(void *arg, tcp_pcb *, pbuf *p, err_t code) {
    auto &self = *static_cast<Impl *>(arg);
    if (code != ERR_OK)
      return code;
    {
      std::lock_guard lock(self.mutex);
      if (!p)
        self.eof = true;
      else {
        if (self.received.size() + p->tot_len > (4u << 20))
          return ERR_MEM;
        try {
          const auto offset = self.received.size();
          self.received.resize(offset + p->tot_len);
          pbuf_copy_partial(p, self.received.data() + offset, p->tot_len, 0);
        } catch (const std::bad_alloc &) {
          return ERR_MEM;
        }
        self.stack.note_tcp_recv(p->tot_len);
        pbuf_free(p);
        // 应用取走字节后再 tcp_recved；慢读者会收紧 TCP 接收窗口。
      }
      ++self.progress;
    }
    self.cv.notify_all();
    return ERR_OK;
  }
  static err_t sent(void *arg, tcp_pcb *, u16_t) {
    auto &self = *static_cast<Impl *>(arg);
    {
      std::lock_guard lock(self.mutex);
      ++self.progress;
    }
    self.cv.notify_all();
    return ERR_OK;
  }
};
TcpStream::TcpStream(Stack &stack) : impl_(std::make_shared<Impl>(stack)) {}
TcpStream::~TcpStream() { close(); }
bool TcpStream::connect(uint16_t port, std::string &err) {
  auto s = impl_;
  err.clear();
  const auto code = LwipRuntime::instance().call([&]() -> err_t {
    auto *nic = s->stack.interface();
    if (!nic || !port || s->started)
      return ERR_ARG;
    s->started = true;
    s->pcb = tcp_new_ip_type(IPADDR_TYPE_V6);
    if (!s->pcb)
      return ERR_MEM;
    s->stack.attach_endpoint(s.get(),
                             [s](const std::string &why) { s->fail(why); });
    tcp_arg(s->pcb, s.get());
    tcp_recv(s->pcb, Impl::receive);
    tcp_err(s->pcb, Impl::error);
    tcp_sent(s->pcb, Impl::sent);
    tcp_nagle_disable(s->pcb);
    tcp_bind_netif(s->pcb, nic);
    ip_addr_t local{}, peer{};
    ipaddr_aton(s->stack.local_text().c_str(), &local);
    ipaddr_aton(s->stack.peer_text().c_str(), &peer);
    auto result = tcp_bind(s->pcb, &local, 0);
    if (result == ERR_OK)
      result = tcp_connect(s->pcb, &peer, port, Impl::connected);
    if (result != ERR_OK)
      s->fail(SCRCTL_TR("TCP connect failed (lwIP ") + std::to_string(result) + SCRCTL_TR(")"));
    return result;
  });
  if (code != ERR_OK)
    return err = SCRCTL_TR("Cannot create TCP connection (lwIP ") + std::to_string(code) + SCRCTL_TR(")"),
           false;
  std::unique_lock lock(s->mutex);
  const bool ready = s->cv.wait_for(lock, std::chrono::seconds(15), [&] {
    return s->established || s->closed || !s->failure.empty();
  });
  if (ready && s->established)
    return true;
  err = s->failure.empty() ? SCRCTL_TR("TCP connect timed out or connection closed") : s->failure;
  lock.unlock();
  LwipRuntime::instance().call([s, why = err] { s->fail(why); });
  return false;
}
bool TcpStream::send(std::string_view data, std::string &err) {
  auto s = impl_;
  std::lock_guard serial(s->send_mutex);
  err.clear();
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(15);
  size_t offset = 0;
  while (offset < data.size()) {
    uint64_t progress;
    {
      std::lock_guard lock(s->mutex);
      progress = s->progress;
    }
    const auto result = LwipRuntime::instance().call([&] {
      {
        std::lock_guard lock(s->mutex);
        if (s->closed || !s->established || !s->failure.empty() || !s->pcb)
          return std::pair<size_t, err_t>{0, ERR_CONN};
      }
      const auto count = static_cast<u16_t>(
          std::min<size_t>({data.size() - offset, tcp_sndbuf(s->pcb), 16384}));
      err_t code = count ? tcp_write(s->pcb, data.data() + offset, count,
                                     TCP_WRITE_FLAG_COPY)
                         : ERR_MEM;
      const size_t accepted = code == ERR_OK ? count : 0;
      const auto output = tcp_output(s->pcb);
      if (output != ERR_OK && output != ERR_MEM)
        code = output;
      return std::pair<size_t, err_t>{accepted, code};
    });
    offset += result.first;
    if (result.second != ERR_OK && result.second != ERR_MEM) {
      std::lock_guard lock(s->mutex);
      return err = s->failure.empty() ? SCRCTL_TR("TCP send failed (connection closed or network error)")
                                      : s->failure,
             false;
    }
    if (offset == data.size())
      return true;
    std::unique_lock lock(s->mutex);
    if (std::chrono::steady_clock::now() >= deadline) {
      err = SCRCTL_TR("TCP send timed out");
      lock.unlock();
      LwipRuntime::instance().call([s, why = err] { s->fail(why); });
      return false;
    }
    s->cv.wait_until(lock,
                     std::min(deadline, std::chrono::steady_clock::now() +
                                            std::chrono::milliseconds(50)),
                     [&] {
                       return s->progress != progress || s->closed ||
                              !s->failure.empty();
                     });
  }
  return connected() ? true : (err = SCRCTL_TR("TCP connection not established"), false);
}
bool TcpStream::recv(std::vector<uint8_t> &out, int timeout_ms,
                     std::string &err, bool *timed_out) {
  auto s = impl_;
  out.clear();
  err.clear();
  if (timed_out)
    *timed_out = false;
  std::unique_lock lock(s->mutex);
  if (!s->cv.wait_for(lock, std::chrono::milliseconds(std::max(timeout_ms, 0)),
                      [&] {
                        return !s->received.empty() || s->eof || s->closed ||
                               !s->failure.empty();
                      })) {
    if (timed_out)
      *timed_out = true;
    else
      err = SCRCTL_TR("TCP read timed out");
    return false;
  }
  if (s->received.empty())
    return err = s->failure.empty() ? SCRCTL_TR("TCP peer or local endpoint closed") : s->failure,
           false;
  out.swap(s->received);
  lock.unlock();
  LwipRuntime::instance().call([s, count = out.size()] {
    if (!s->pcb)
      return;
    size_t remaining = count;
    while (remaining) {
      const auto n = static_cast<u16_t>(std::min<size_t>(remaining, 65535));
      tcp_recved(s->pcb, n);
      remaining -= n;
    }
  });
  return true;
}
void TcpStream::close() {
  auto s = impl_;
  LwipRuntime::instance().call([s] {
    {
      std::lock_guard lock(s->mutex);
      s->closed = true;
      s->established = false;
      ++s->progress;
    }
    s->cv.notify_all();
    if (s->pcb) {
      auto *p = s->pcb;
      s->pcb = nullptr;
      s->stack.close_tcp(p);
    }
    s->stack.detach_endpoint(s.get());
  });
}
bool TcpStream::connected() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->established && !impl_->closed && impl_->failure.empty();
}
} // namespace scrctl::net
