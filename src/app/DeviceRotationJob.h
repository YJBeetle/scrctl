#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

#include "remote/OrientationControl.h"

namespace scrctl::app {

/// 单任务设备转屏 worker。start/poll/shutdown 由 SDL 主线程串行调用；
/// 网络操作只发生在 worker。Device 必须保持地址稳定并活到 shutdown 完成。
class DeviceRotationJob {
public:
    struct Completion {
        bool success = false;
        bool cancelled = false;
        /// 未取得有效基线前保持 Unknown；不是手机当前方向的缓存。
        remote::DeviceOrientation target = remote::DeviceOrientation::Unknown;
        std::string error;
    };
    using Operation = std::function<Completion(std::stop_token)>;
    enum class StartStatus { Started, Busy, Unavailable, LaunchFailed };

    explicit DeviceRotationJob(remote::Device &device, bool verbose = false);
    /// 注入完整一次操作，供离线验证生命周期；不改变生产请求/重试策略。
    explicit DeviceRotationJob(Operation operation);
    ~DeviceRotationJob();
    DeviceRotationJob(const DeviceRotationJob &) = delete;
    DeviceRotationJob &operator=(const DeviceRotationJob &) = delete;

    /// 已完成但尚未 poll 消费的请求仍 Busy，不覆盖结果或排队重复快捷键。
    [[nodiscard]] StartStatus start();
    /// 无结果时立即返回；有结果时取走，并 join 已完成操作的线程尾部。
    [[nodiscard]] std::optional<Completion> poll();
    [[nodiscard]] bool busy() const;
    /// 永久停止接收任务，取消并 join；重复调用安全，丢弃未消费结果。
    /// 取消仅关闭网络连接，不能撤销已执行转屏；没有自动恢复或 mutation 重试。
    /// operation 必须配合 stop_token 或有界返回；必须先于 Device 销毁调用。
    void shutdown();

private:
    void run(std::stop_token token);
    Operation operation_;
    mutable std::mutex mutex_;
    std::optional<Completion> completion_;
    bool accepting_ = true;
    bool occupied_ = false;
    std::stop_source stop_source_{std::nostopstate};
    std::jthread worker_;
};

} // namespace scrctl::app
