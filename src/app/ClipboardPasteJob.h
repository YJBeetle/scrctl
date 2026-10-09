#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

namespace scrctl::app {

/// 显式粘贴请求的单任务 worker。operation 负责 SET 和独立 PULL 的字节校验，
/// 本类只交付完成结果，不发送 HID，也不把文本交给其他线程继续借用。
class ClipboardPasteJob {
  public:
    struct OperationResult {
        bool success = false;
        bool cancelled = false;
        std::string error;
    };
    using Operation = std::function<OperationResult(const std::string &, std::stop_token)>;

    struct Completion {
        uint64_t id = 0;
        uint64_t generation = 0;
        bool success = false;
        bool cancelled = false;
        std::string error;
    };

    enum class StartStatus { Started, Busy, TooLarge, Unavailable, LaunchFailed };
    static constexpr std::size_t kMaxTextBytes = 1024 * 1024;

    explicit ClipboardPasteJob(Operation operation);
    ~ClipboardPasteJob();
    ClipboardPasteJob(const ClipboardPasteJob &) = delete;
    ClipboardPasteJob &operator=(const ClipboardPasteJob &) = delete;

    /// start、poll 和 shutdown 由主线程串行调用。成功启动前复制 UTF-8 字节，
    /// 之后调用者可立即修改或销毁原字符串。超过 1 MiB 不复制、不调用 operation。
    /// worker 结束但结果尚未被 poll 消费时仍为 Busy，不覆盖上一请求的结果。
    [[nodiscard]] StartStatus start(uint64_t id, uint64_t generation, const std::string &text);

    /// 没有完成结果时立即返回空；有结果时取走并 join 已完成 operation 的线程。
    /// join 仍可能短暂等待线程尾部收尾，不承诺严格零等待。
    [[nodiscard]] std::optional<Completion> poll();

    /// 可与 worker 并发调用。先锁存取消意图，离锁后 request_stop，允许同步
    /// stop_callback 重入 busy。已经完成但未 poll 的成功也会改成取消结果。
    void cancel();
    [[nodiscard]] bool busy() const;

    /// 永久停止接收任务，取消并 join，丢弃未消费结果，重复调用安全。
    /// operation 必须配合 stop_token 或自身有界返回；本类不强制中断网络调用。
    /// operation 借用 Device 时，调用方须在销毁 Device 前执行此方法。
    void shutdown();

  private:
    void run(uint64_t id, uint64_t generation, std::string text, std::stop_token token);

    Operation operation_;
    mutable std::mutex mutex_;
    std::stop_source stop_source_{std::nostopstate};
    std::optional<Completion> completion_;
    bool accepting_ = true;
    bool occupied_ = false;
    bool cancel_requested_ = false;
    // stop_source 是当前 jthread 停止状态的副本。取消时可先在锁内复制，
    // 再在锁外请求停止，不需要在回调期间访问 worker_。
    std::jthread worker_;
};

} // namespace scrctl::app
