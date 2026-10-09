#include "app/ClipboardPasteJob.h"

#include <exception>
#include <utility>

namespace scrctl::app {

ClipboardPasteJob::ClipboardPasteJob(Operation operation) : operation_(std::move(operation)) {}

ClipboardPasteJob::~ClipboardPasteJob() { shutdown(); }

ClipboardPasteJob::StartStatus ClipboardPasteJob::start(uint64_t id, uint64_t generation,
                                                       const std::string &text) {
    std::lock_guard lock(mutex_);
    if (!accepting_ || !operation_) return StartStatus::Unavailable;
    if (occupied_) return StartStatus::Busy;
    if (text.size() > kMaxTextBytes) return StartStatus::TooLarge;

    try {
        std::string owned_text = text;
        cancel_requested_ = false;
        occupied_ = true;
        worker_ = std::jthread(
            [this, id, generation, text = std::move(owned_text)](std::stop_token token) mutable {
                run(id, generation, std::move(text), token);
            });
        // start 仍持有 mutex，cancel 无法在停止状态安装之前遗漏这一请求。
        stop_source_ = worker_.get_stop_source();
    } catch (...) {
        occupied_ = false;
        stop_source_ = std::stop_source{std::nostopstate};
        return StartStatus::LaunchFailed;
    }
    return StartStatus::Started;
}

void ClipboardPasteJob::run(uint64_t id, uint64_t generation, std::string text,
                           std::stop_token token) {
    OperationResult outcome;
    try {
        if (token.stop_requested()) {
            outcome.cancelled = true;
        } else {
            outcome = operation_(text, token);
        }
    } catch (const std::exception &error) {
        // 调用方显示本地化失败提示；底层异常的原始诊断按值保留。
        outcome.error = error.what();
    } catch (...) {
        // 无可读诊断时保留普通失败，由调用方使用统一提示。
    }

    std::lock_guard lock(mutex_);
    const bool cancelled = cancel_requested_ || token.stop_requested() || outcome.cancelled;
    completion_ = Completion{id, generation, outcome.success && !cancelled, cancelled,
                             std::move(outcome.error)};
}

std::optional<ClipboardPasteJob::Completion> ClipboardPasteJob::poll() {
    std::optional<Completion> result;
    {
        std::lock_guard lock(mutex_);
        if (!completion_) return std::nullopt;
        result = std::move(completion_);
        completion_.reset();
    }
    // operation 已返回，不持 job mutex 等待线程尾部，以便 cancel 的回调重入。
    if (worker_.joinable()) worker_.join();
    {
        std::lock_guard lock(mutex_);
        if (cancel_requested_) {
            result->cancelled = true;
            result->success = false;
        }
        occupied_ = false;
        cancel_requested_ = false;
        stop_source_ = std::stop_source{std::nostopstate};
    }
    return result;
}

void ClipboardPasteJob::cancel() {
    std::stop_source source{std::nostopstate};
    {
        std::lock_guard lock(mutex_);
        if (!occupied_) return;
        cancel_requested_ = true;
        if (completion_) {
            completion_->cancelled = true;
            completion_->success = false;
        }
        source = stop_source_;
    }
    source.request_stop();
}

bool ClipboardPasteJob::busy() const {
    std::lock_guard lock(mutex_);
    return occupied_;
}

void ClipboardPasteJob::shutdown() {
    std::stop_source source{std::nostopstate};
    {
        std::lock_guard lock(mutex_);
        accepting_ = false;
        if (occupied_) cancel_requested_ = true;
        source = stop_source_;
    }
    source.request_stop();
    if (worker_.joinable()) worker_.join();
    {
        std::lock_guard lock(mutex_);
        completion_.reset();
        occupied_ = false;
        cancel_requested_ = false;
        stop_source_ = std::stop_source{std::nostopstate};
    }
    // 线程已结束，先释放 operation 捕获的对象，再允许调用方销毁 Device。
    operation_ = {};
}

} // namespace scrctl::app
