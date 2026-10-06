#pragma once
#include "transport/PacketIo.h"
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>

// 内存包隧道保留 PacketIo 的阻塞/关闭语义，用于验证生产线程和同步端点。
class MemoryTunnel : public scrctl::transport::PacketIo {
public:
  std::function<void(std::vector<uint8_t>)> deliver;
  void inject(std::vector<uint8_t> packet) {
    {
      std::lock_guard lock(mutex_);
      if (closed_)
        return;
      incoming_.push_back(std::move(packet));
    }
    cv_.notify_all();
  }
  bool send_ipv6(const uint8_t *data, size_t length,
                 std::string &err) override {
    {
      std::lock_guard lock(mutex_);
      if (closed_)
        return err = "内存隧道已关闭", false;
    }
    if (deliver)
      deliver(std::vector<uint8_t>(data, data + length));
    return true;
  }
  bool recv_ipv6(std::vector<uint8_t> &out, std::string &err) override {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [&] { return closed_ || !incoming_.empty(); });
    if (closed_)
      return err = "内存隧道已关闭", false;
    out = std::move(incoming_.front());
    incoming_.pop_front();
    return true;
  }
  bool wait_readable(int ms, std::string &err, bool *timed_out) override {
    std::unique_lock lock(mutex_);
    if (timed_out)
      *timed_out = false;
    if (!cv_.wait_for(lock, std::chrono::milliseconds(ms),
                      [&] { return closed_ || !incoming_.empty(); })) {
      if (timed_out) {
        *timed_out = true;
        err.clear();
      } else
        err = "内存隧道超时";
      return false;
    }
    return closed_ ? (err = "内存隧道已关闭", false) : true;
  }
  uint16_t mtu() const override { return 16000; }
  void shutdown() override {
    {
      std::lock_guard lock(mutex_);
      closed_ = true;
    }
    cv_.notify_all();
  }

private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool closed_ = false;
  std::deque<std::vector<uint8_t>> incoming_;
};
