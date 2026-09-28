#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "decode/Decoder.h"

namespace scrctl::remote {
class Device;
class ServiceConnection;
}  // namespace scrctl::remote

namespace scrctl::media {

/// 把 screencaptureservice 回的一张 PNG 解成 BGRA。libav 没编进来时返回 false 并在
/// err 里说清——兜底路没有别的 PNG 解码后端可用。纯函数，离线可判（tests/media_test）。
bool decode_png_bgra(const std::vector<uint8_t> &png, scrctl::Frame &out, std::string &err);

/// 兜底镜像源：媒体流被设备按版本拒（iOS 27 以下，code 9021）时，轮询截图当画面。
///
/// 实测 iPadOS 18.7.8 上一次截图 RPC 约 0.45~0.5 秒（10 张 5.6 秒含建会话），所以这条
/// 路的上限就是 2 fps 上下：它是"能看、能操作"，不是"能看视频"。两个几何上的好处让它
/// 比实时流简单：截图的像素尺寸**就是**可见区尺寸（没有 HEVC 的 CU 对齐填充），而且
/// 截图本身已按界面方向摆正——所以这条源报的朝向恒为 0，不需要 displayinfoupdates。
class ScreenshotSource {
public:
    /// 目录里没有 screencaptureservice、连不上、或第一张截图就拿不到时返回 nullptr。
    ///
    /// `capture_first`：起流就降级那条路传 true（默认）——兜底路连一张都拿不到时它就是
    /// 空的，原因要直接交出去，而不是起个线程在里面默默失败。运行中降级那条路传 false：
    /// 那次切换发生在渲染线程上，同步拿第一张会把窗口冻到一次截图 RPC 的上限。
    static std::unique_ptr<ScreenshotSource> start(remote::Device &device, std::string &err,
                                                   bool capture_first = true);
    ~ScreenshotSource();

    ScreenshotSource(const ScreenshotSource &) = delete;
    ScreenshotSource &operator=(const ScreenshotSource &) = delete;

    /// 等一张比 `serial` 新的画面（serial 是进出参）。超时或被 stop 唤醒返回 false。
    bool latest(scrctl::Frame &out, uint64_t &serial, int timeout_ms);
    void stop();

    struct Stats {
        uint64_t frames = 0;
        uint64_t bytes = 0;
        uint64_t failures = 0;
    };
    [[nodiscard]] Stats stats() const;
    /// 第一帧到来后才有意义（0/0 之前）。
    void image_size(int &w, int &h) const;

private:
    explicit ScreenshotSource(remote::Device &device);
    bool capture_once(std::vector<uint8_t> &png, std::string &err);
    void loop();

    /// 截图服务**一条连接只服务一次请求**（连拍十张的探针每次都是新连接才成的），
    /// 所以这里存设备引用、每轮自己开一条新连接，而不是握一条长连接复用。
    remote::Device &device_;
    std::thread worker_;
    std::atomic<bool> stopping_{false};

    mutable std::mutex mu_;
    std::condition_variable cv_;
    scrctl::Frame frame_;
    uint64_t serial_ = 0;
    Stats stats_;
};

}  // namespace scrctl::media
