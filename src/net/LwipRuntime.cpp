#include "net/LwipRuntime.h"
extern "C" {
#include "lwip/init.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
}
namespace scrctl::net {
LwipRuntime &LwipRuntime::instance() {
  static LwipRuntime runtime;
  return runtime;
}
LwipRuntime::LwipRuntime() {
  std::promise<void> ready;
  auto initialized = ready.get_future();
  worker_ = std::thread(&LwipRuntime::run, this, std::move(ready));
  initialized.get();
}
LwipRuntime::~LwipRuntime() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  cv_.notify_all();
  worker_.join();
}
void LwipRuntime::run(std::promise<void> ready) {
  lwip_init();
  close_arg_id_ = tcp_ext_arg_alloc_id();
  ready.set_value();
  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock lock(mutex_);
      cv_.wait_for(lock, std::chrono::milliseconds(10),
                   [this] { return stopping_ || !tasks_.empty(); });
      if (stopping_ && tasks_.empty())
        break;
      if (!tasks_.empty()) {
        task = std::move(tasks_.front());
        tasks_.pop_front();
      }
    }
    cv_.notify_all();
    if (task)
      task();
    sys_check_timeouts();
  }
}
} // namespace scrctl::net
