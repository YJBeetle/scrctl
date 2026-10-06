#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>

namespace scrctl::net {

// 进程内唯一的 lwIP 执行上下文。任务不能阻塞隧道 I/O 或等待应用消费数据。
// 同步调用只等待一个短的核心操作；TCP
// 的连接、发送空间和接收等待在应用线程进行。
class LwipRuntime {
public:
  static LwipRuntime &instance();
  template <typename Fn> auto call(Fn fn) -> decltype(fn()) {
    if (std::this_thread::get_id() == worker_.get_id())
      return fn();
    auto task =
        std::make_shared<std::packaged_task<decltype(fn())()>>(std::move(fn));
    auto result = task->get_future();
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [this] { return tasks_.size() < 1024; });
      tasks_.emplace_back([task] { (*task)(); });
    }
    cv_.notify_all();
    return result.get();
  }
  unsigned char close_arg_id() const { return close_arg_id_; }

private:
  LwipRuntime();
  ~LwipRuntime();
  void run(std::promise<void> ready);
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> tasks_;
  bool stopping_ = false;
  unsigned char close_arg_id_ = 0;
  std::thread worker_;
};
} // namespace scrctl::net
