#include "app/ClipboardPasteJob.h"

#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {

using scrctl::app::ClipboardPasteJob;
using Status = ClipboardPasteJob::StartStatus;
using Outcome = ClipboardPasteJob::OperationResult;
using Completion = ClipboardPasteJob::Completion;
using namespace std::chrono_literals;

int checks = 0;
int failures = 0;

void check(bool ok, const char *message) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

std::optional<Completion> wait_result(ClipboardPasteJob &job) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto result = job.poll()) return result;
        std::this_thread::sleep_for(1ms);
    }
    check(false, "operation completes within the test deadline");
    return std::nullopt;
}

// 在真实 worker 退出线程时通知测试，晚于 operation 返回与 completion 发布。
// 这样可检验未消费完成结果的 Busy，不读取生产私有状态或增加测试 accessor。
struct ExitSignal {
    std::shared_ptr<std::promise<void>> signal;
    ~ExitSignal() {
        if (signal) signal->set_value();
    }
};
thread_local ExitSignal worker_exit;

struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool released = false;
    bool stop_observed = false;

    bool wait_entered() {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, 2s, [&] { return entered; });
    }
    bool wait_stopped() {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, 2s, [&] { return stop_observed; });
    }
    void release() {
        std::lock_guard lock(mutex);
        released = true;
        cv.notify_all();
    }
};

void inactive_and_rejected_jobs() {
    std::atomic<int> calls{0};
    ClipboardPasteJob job([&](const std::string &, std::stop_token) {
        ++calls;
        return Outcome{true, false, {}};
    });
    check(!job.busy() && !job.poll(), "a new job has no work or completion");
    job.cancel();
    check(!job.busy(), "cancelling an idle job does not make it busy");
    const std::string oversized(ClipboardPasteJob::kMaxTextBytes + 1, 'x');
    check(job.start(1, 1, oversized) == Status::TooLarge,
          "the byte limit rejects max plus one");
    check(calls.load() == 0 && !job.busy() && !job.poll(),
          "rejected text neither runs the operation nor occupies a result");
    check(job.start(2, 3, "ok") == Status::Started, "idle cancellation does not poison a later job");
    const auto result = wait_result(job);
    check(result && result->success && !result->cancelled && result->id == 2 &&
              result->generation == 3,
          "a later request retains its identity and succeeds");
    check(calls.load() == 1 && !job.busy(), "poll consumes the single result and permits reuse");
    job.shutdown(); job.shutdown(); job.cancel();
    check(job.start(4, 5, "ignored") == Status::Unavailable && !job.busy() && !job.poll(),
          "shutdown is permanent and idempotent");

    ClipboardPasteJob missing({});
    check(missing.start(1, 1, "text") == Status::Unavailable && !missing.busy(),
          "an absent operation cannot start a worker");
}

void byte_ownership_and_busy_completion() {
    Gate gate;
    std::string observed;
    const std::string expected = std::string("中文 café 😀\r\n") + std::string("\0", 1) + "tail";
    std::string caller_text = expected;
    auto exited = std::make_shared<std::promise<void>>();
    auto exit_future = exited->get_future();
    ClipboardPasteJob job([&](const std::string &text, std::stop_token) {
        worker_exit.signal = exited;
        std::unique_lock lock(gate.mutex);
        gate.entered = true; gate.cv.notify_all();
        gate.cv.wait(lock, [&] { return gate.released; });
        observed = text;
        return Outcome{true, false, {}};
    });
    check(job.start(42, 91, caller_text) == Status::Started, "start a real blocked worker");
    caller_text.assign("caller changed and destroyed its original bytes");
    check(gate.wait_entered(), "the owned text reaches the worker");
    check(job.busy() && job.start(43, 92, "replacement") == Status::Busy,
          "an operation in progress cannot be replaced");
    check(!job.poll(), "poll does not join an operation still waiting for input");
    gate.release();
    check(exit_future.wait_for(2s) == std::future_status::ready, "worker has actually exited");
    check(job.busy() && job.start(43, 92, "replacement") == Status::Busy,
          "an exited worker's unconsumed completion is still busy");
    const auto result = job.poll();
    check(result && result->id == 42 && result->generation == 91 && result->success &&
              !result->cancelled && result->error.empty(),
          "poll returns the original request rather than a rejected replacement");
    check(observed == expected, "Unicode, newlines and embedded NUL retain exact owned bytes");
    check(!job.busy() && !job.poll(), "completion is returned only once");
}

void exact_limit_and_operation_outcomes() {
    std::string seen;
    ClipboardPasteJob job([&](const std::string &text, std::stop_token) {
        seen = text;
        if (text == "failure") return Outcome{false, false, "original SET failure"};
        if (text == "cancelled") return Outcome{true, true, "operation cancelled"};
        if (text == "exception") throw std::runtime_error("original exception");
        if (text == "unknown exception") throw 7;
        return Outcome{true, false, {}};
    });
    std::string limit = "中文😀";
    limit.append(ClipboardPasteJob::kMaxTextBytes - limit.size(), 'x');
    check(job.start(1, 2, limit) == Status::Started, "exactly one MiB of UTF-8 bytes is accepted");
    auto result = wait_result(job);
    check(result && result->success && seen == limit, "the exact limit is not truncated or transcoded");
    for (const std::string text : {"failure", "cancelled", "exception", "unknown exception", ""}) {
        check(job.start(5, 6, text) == Status::Started, "finished jobs can run again");
        result = wait_result(job);
        if (text == "failure")
            check(result && !result->success && !result->cancelled &&
                      result->error == "original SET failure", "an operation failure preserves its error");
        else if (text == "cancelled")
            check(result && !result->success && result->cancelled &&
                      result->error == "operation cancelled", "an operation's cancellation overrides success");
        else if (text == "exception")
            check(result && !result->success && !result->cancelled &&
                      result->error == "original exception", "an exception becomes a failed owned result");
        else if (text == "unknown exception")
            check(result && !result->success && !result->cancelled && result->error.empty(),
                  "an unknown exception is a safe failure with no invented diagnostic");
        else
            check(result && result->success && seen.empty(), "empty bytes are passed through without special mutation");
    }
}

void cancellation_wakes_operation_and_allows_callback_reentry() {
    Gate gate;
    ClipboardPasteJob *current = nullptr;
    std::atomic<bool> callback_saw_busy{false};
    ClipboardPasteJob job([&](const std::string &, std::stop_token token) {
        std::stop_callback callback(token, [&] {
            callback_saw_busy = current->busy();
            std::lock_guard lock(gate.mutex);
            gate.stop_observed = true;
            gate.cv.notify_all();
        });
        std::unique_lock lock(gate.mutex);
        gate.entered = true; gate.cv.notify_all();
        gate.cv.wait(lock, [&] { return gate.stop_observed || gate.released; });
        return Outcome{true, false, "returned after stop"};
    });
    current = &job;
    check(job.start(9, 12, "cancel") == Status::Started && gate.wait_entered(),
          "the cancellable operation starts and registers its callback");
    job.cancel();
    check(callback_saw_busy.load() && gate.wait_stopped(),
          "cancel invokes a callback that can reenter busy without deadlock");
    const auto result = wait_result(job);
    check(result && result->id == 9 && result->generation == 12 && !result->success &&
              result->cancelled && result->error == "returned after stop",
          "cancel marks completion even if the operation returns success");
    check(!job.busy(), "a consumed cancellation permits a new request");
}

void cancel_after_worker_exit() {
    auto exited = std::make_shared<std::promise<void>>();
    auto exit_future = exited->get_future();
    ClipboardPasteJob job([&](const std::string &, std::stop_token) {
        worker_exit.signal = exited;
        return Outcome{true, false, {}};
    });
    check(job.start(11, 13, "done") == Status::Started, "start an immediately completing operation");
    check(exit_future.wait_for(2s) == std::future_status::ready,
          "completion is published before the observed thread exit");
    job.cancel(); job.cancel();
    check(job.start(12, 14, "replace") == Status::Busy, "cancel does not overwrite an unconsumed result");
    const auto result = job.poll();
    check(result && !result->success && result->cancelled && result->id == 11 &&
              result->generation == 13,
          "cancelling an already published success prevents stale paste");
}

void cancel_completion_races_and_repeated_requests() {
    for (uint64_t iteration = 0; iteration < 100; ++iteration) {
        std::barrier rendezvous(2);
        ClipboardPasteJob job([&](const std::string &, std::stop_token) {
            rendezvous.arrive_and_wait();
            return Outcome{true, false, {}};
        });
        check(job.start(iteration, iteration + 1000, "race") == Status::Started,
              "launch the operation for a cancel/completion race");
        std::jthread cancellation([&] {
            rendezvous.arrive_and_wait();
            job.cancel();
        });
        cancellation.join();
        const auto result = wait_result(job);
        check(result && !result->success && result->cancelled && result->id == iteration &&
                  result->generation == iteration + 1000,
              "cancel wins before poll regardless of completion publication order");
    }

    std::atomic<int> calls{0};
    ClipboardPasteJob repeated([&](const std::string &, std::stop_token) {
        ++calls;
        return Outcome{true, false, {}};
    });
    for (uint64_t iteration = 0; iteration < 100; ++iteration) {
        check(repeated.start(iteration, std::numeric_limits<uint64_t>::max() - iteration,
                             "repeat") == Status::Started,
              "reuse the same worker owner after consuming the previous result");
        const auto result = wait_result(repeated);
        check(result && result->success && !result->cancelled && result->id == iteration &&
                  result->generation == std::numeric_limits<uint64_t>::max() - iteration,
              "reused jobs do not leak prior cancellation or identity");
    }
    check(calls.load() == 100 && !repeated.busy(), "each reusable request executes exactly once");
}

void shutdown_waits_and_releases_captured_lifetime() {
    Gate gate;
    std::atomic<int> destroyed{0};
    struct Borrowed {
        explicit Borrowed(std::atomic<int> &count) : destroyed(count) {}
        std::atomic<int> &destroyed;
        ~Borrowed() { ++destroyed; }
    };
    auto borrowed = std::make_shared<Borrowed>(destroyed);
    std::weak_ptr<Borrowed> lifetime = borrowed;
    auto exited = std::make_shared<std::promise<void>>();
    auto exit_future = exited->get_future();
    ClipboardPasteJob job([&, borrowed](const std::string &, std::stop_token token) {
        worker_exit.signal = exited;
        std::stop_callback callback(token, [&] {
            std::lock_guard lock(gate.mutex);
            gate.stop_observed = true; gate.cv.notify_all();
        });
        std::unique_lock lock(gate.mutex);
        gate.entered = true; gate.cv.notify_all();
        // 故意不因取消马上返回，验证 shutdown 确实等待借用结束。
        gate.cv.wait(lock, [&] { return gate.released; });
        return Outcome{borrowed != nullptr, false, {}};
    });
    borrowed.reset();
    check(job.start(21, 34, "shutdown") == Status::Started && gate.wait_entered(),
          "the worker starts while the operation owns a captured lifetime");
    auto closing = std::async(std::launch::async, [&] { job.shutdown(); });
    check(gate.wait_stopped(), "shutdown requests cancellation outside the job mutex");
    check(closing.wait_for(20ms) == std::future_status::timeout && !lifetime.expired(),
          "shutdown waits while the operation still uses its captured object");
    gate.release(); closing.get();
    check(exit_future.wait_for(0ms) == std::future_status::ready,
          "shutdown joins actual thread exit before returning");
    check(lifetime.expired() && destroyed.load() == 1,
          "shutdown releases operation captures after the worker has stopped");
    check(!job.busy() && !job.poll() && job.start(22, 35, "closed") == Status::Unavailable,
          "shutdown discards the result and rejects further work");
}

void destruction_cancels_and_joins() {
    Gate gate;
    auto exited = std::make_shared<std::promise<void>>();
    auto exit_future = exited->get_future();
    {
        ClipboardPasteJob job([&](const std::string &, std::stop_token token) {
            worker_exit.signal = exited;
            std::stop_callback callback(token, [&] {
                std::lock_guard lock(gate.mutex);
                gate.stop_observed = true; gate.cv.notify_all();
            });
            std::unique_lock lock(gate.mutex);
            gate.entered = true; gate.cv.notify_all();
            gate.cv.wait(lock, [&] { return gate.stop_observed; });
            return Outcome{false, true, {}};
        });
        check(job.start(1, 1, "destructor") == Status::Started && gate.wait_entered(),
              "start an operation before owner destruction");
    }
    check(exit_future.wait_for(0ms) == std::future_status::ready && gate.wait_stopped(),
          "destruction cancels and joins without a detached borrower");
}

} // namespace

int main() {
    inactive_and_rejected_jobs();
    byte_ownership_and_busy_completion();
    exact_limit_and_operation_outcomes();
    cancellation_wakes_operation_and_allows_callback_reentry();
    cancel_after_worker_exit();
    cancel_completion_races_and_repeated_requests();
    shutdown_waits_and_releases_captured_lifetime();
    destruction_cancels_and_joins();
    std::printf("clipboard_job_test: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
