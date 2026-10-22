#include "app/DeviceRotationJob.h"

#include <exception>
#include <utility>

#include "i18n/Translation.h"

namespace scrctl::app {
DeviceRotationJob::DeviceRotationJob(Operation operation) : operation_(std::move(operation)) {}

DeviceRotationJob::~DeviceRotationJob() { shutdown(); }

DeviceRotationJob::StartStatus DeviceRotationJob::start() {
    std::lock_guard lock(mutex_);
    if (!accepting_ || !operation_)
        return StartStatus::Unavailable;
    if (occupied_)
        return StartStatus::Busy;
    occupied_ = true;
    try {
        worker_ = std::jthread([this](std::stop_token token) { run(token); });
        stop_source_ = worker_.get_stop_source();
    } catch (...) {
        occupied_ = false;
        stop_source_ = std::stop_source{std::nostopstate};
        return StartStatus::LaunchFailed;
    }
    return StartStatus::Started;
}

void DeviceRotationJob::run(std::stop_token token) {
    Completion result;
    try {
        if (token.stop_requested())
            result.cancelled = true;
        else
            result = operation_(token);
    } catch (const std::exception &error) {
        result.success = false;
        result.error = error.what();
    } catch (...) {
        result.success = false;
        result.error = SCRCTL_TR("Device orientation operation threw an exception");
    }
    result.cancelled = result.cancelled || token.stop_requested();
    if (result.cancelled)
        result.success = false;
    std::lock_guard lock(mutex_);
    completion_ = std::move(result);
}

std::optional<DeviceRotationJob::Completion> DeviceRotationJob::poll() {
    std::optional<Completion> result;
    {
        std::lock_guard lock(mutex_);
        if (!completion_)
            return std::nullopt;
        result = std::move(completion_);
        completion_.reset();
    }
    if (worker_.joinable())
        worker_.join();
    {
        std::lock_guard lock(mutex_);
        occupied_ = false;
        stop_source_ = std::stop_source{std::nostopstate};
    }
    return result;
}

bool DeviceRotationJob::busy() const {
    std::lock_guard lock(mutex_);
    return occupied_;
}

void DeviceRotationJob::shutdown() {
    std::stop_source source{std::nostopstate};
    {
        std::lock_guard lock(mutex_);
        accepting_ = false;
        source = stop_source_;
    }
    // A synchronous stop callback may inspect busy(); do not hold our mutex here.
    source.request_stop();
    if (worker_.joinable())
        worker_.join();
    {
        std::lock_guard lock(mutex_);
        completion_.reset();
        occupied_ = false;
        stop_source_ = std::stop_source{std::nostopstate};
    }
    operation_ = {};
}

} // namespace scrctl::app
