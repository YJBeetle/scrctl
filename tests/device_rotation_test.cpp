#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "app/DeviceRotationJob.h"

namespace {
using Job = scrctl::app::DeviceRotationJob;
using namespace std::chrono_literals;
int failures = 0;
int checks = 0;
void check(bool ok, const char *description) {
    ++checks;
    if (!ok) ++failures;
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", description);
}
std::optional<Job::Completion> wait_completion(Job &job) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto result = job.poll()) return result;
        std::this_thread::sleep_for(1ms);
    }
    return std::nullopt;
}
} // namespace

int main() {
    const auto main_thread = std::this_thread::get_id();
    std::mutex mutex;
    std::condition_variable_any wake;
    bool released = false;
    std::promise<void> entered;
    auto entry = entered.get_future();
    std::atomic<int> calls{0};
    std::atomic<bool> worker_thread{false};
    Job job([&](std::stop_token token) {
        worker_thread = std::this_thread::get_id() != main_thread;
        if (calls.fetch_add(1) == 0) entered.set_value();
        std::unique_lock lock(mutex);
        wake.wait(lock, token, [&] { return released; });
        return Job::Completion{true, token.stop_requested(),
                               scrctl::remote::DeviceOrientation::LandscapeLeft, {}};
    });
    check(job.start() == Job::StartStatus::Started, "start returns while operation waits on worker");
    check(entry.wait_for(2s) == std::future_status::ready && worker_thread.load(), "operation runs off caller thread");
    check(!job.poll(), "poll does not wait on unfinished network operation");
    check(job.start() == Job::StartStatus::Busy && calls == 1, "inflight shortcut is not queued or retried");
    {
        std::lock_guard lock(mutex);
        released = true;
    }
    wake.notify_all();
    check(job.start() == Job::StartStatus::Busy, "release does not admit another request before poll");
    const auto first = wait_completion(job);
    check(first && first->success && !first->cancelled &&
              first->target == scrctl::remote::DeviceOrientation::LandscapeLeft,
          "completion preserves successful target");
    check(!job.busy() && !job.poll(), "poll consumes exactly once and releases slot");
    check(job.start() == Job::StartStatus::Started, "explicit next shortcut can start after poll");
    check(wait_completion(job).has_value() && calls == 2, "two explicit requests invoke operation exactly twice");
    job.shutdown();
    job.shutdown();
    check(job.start() == Job::StartStatus::Unavailable && !job.poll(), "shutdown is permanent and idempotent");

    std::promise<void> cancel_entered;
    auto cancel_entry = cancel_entered.get_future();
    std::atomic<bool> stop_seen{false};
    std::atomic<bool> callback_reentered{false};
    std::atomic<bool> returned{false};
    Job *cancel_job_pointer = nullptr;
    auto borrowed_lifetime = std::make_shared<int>(1);
    std::weak_ptr<int> lifetime = borrowed_lifetime;
    Job cancel_job([&, owned = borrowed_lifetime](std::stop_token token) {
        std::stop_callback callback(token, [&] {
            callback_reentered = cancel_job_pointer->busy();
            wake.notify_all();
        });
        cancel_entered.set_value();
        std::unique_lock lock(mutex);
        wake.wait(lock, token, [] { return false; });
        stop_seen = token.stop_requested();
        returned = true;
        return Job::Completion{true, false, scrctl::remote::DeviceOrientation::Portrait, {}};
    });
    cancel_job_pointer = &cancel_job;
    borrowed_lifetime.reset();
    check(cancel_job.start() == Job::StartStatus::Started &&
              cancel_entry.wait_for(2s) == std::future_status::ready,
          "cancellable operation is active");
    cancel_job.shutdown();
    check(stop_seen && returned && callback_reentered, "shutdown requests stop outside mutex and joins operation");
    check(lifetime.expired() && !cancel_job.poll(), "shutdown releases borrowed operation lifetime and discards completion");

    Job thrown([](std::stop_token) -> Job::Completion { throw std::runtime_error("orientation test failure"); });
    check(thrown.start() == Job::StartStatus::Started, "throwing operation starts normally");
    const auto failure = wait_completion(thrown);
    check(failure && !failure->success && !failure->cancelled && failure->error == "orientation test failure",
          "operation exception becomes owned failure result");
    Job cancelled([](std::stop_token) {
        return Job::Completion{true, true, scrctl::remote::DeviceOrientation::Portrait, {}};
    });
    check(cancelled.start() == Job::StartStatus::Started, "explicit cancelled result starts normally");
    const auto cancellation = wait_completion(cancelled);
    check(cancellation && cancellation->cancelled && !cancellation->success,
          "cancelled completion cannot claim success");
    Job empty(Job::Operation{});
    check(empty.start() == Job::StartStatus::Unavailable && !empty.busy(), "empty operation does not launch thread");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
