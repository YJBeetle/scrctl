#pragma once

#include "app/AudioOut.h"
#include "app/FrameSource.h"
#include "app/LiveStats.h"
#include "hid/Hid.h"
#include "media/FramePump.h"
#include "media/Recorder.h"
#include "media/ScreenshotSource.h"
#include "media/StreamSession.h"
#include "remote/Device.h"
#include "remote/DisplayInfo.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace scrctl::app {

/// 设备画面源，负责会话建立、画面来源切换和输入转发。
/// 实时流的收包、组帧、解码及恢复由 FramePump 管理。
class LiveSource final : public FrameSource {
  public:
    ~LiveSource() override;

    /// watch_display 订阅原始显示方向，用于自动旋转及已转正截图的触摸映射；
    /// 显式渲染角也需要这份原始方向。无窗口时可关闭，避免独占连接
    /// 及订阅线程。want_audio 决定是否建立独立音频会话；失败时视频继续。
    /// audio_buffer_ms 是音频预缓冲与目标水位对应的时长。audio_dup 保留手机
    /// 播放；默认请求转到电脑，不支持该路由时禁用音频，不自动切回双端播放。
    /// record_orientation 写容器方向元数据，默认 0；不改变编码数据或触摸映射。
    /// decode_audio=false 仅录原 AAC 音轨，跳过 PCM 解码；须同时请求容器录制。
    /// should_cancel 在连接和启动步骤之间检查退出请求，阻止后续起流与路由切换；
    /// 已在进行的底层连接或 RPC 仍可能等待自身超时后才返回。
    bool start(const std::string &serial, const std::string &wifi, const std::string &record_path,
               bool hw_decode, bool watch_display, bool want_audio, int audio_buffer_ms,
               const std::string &video_source, const std::string &test_degrade, std::string &err,
               uint16_t wifi_port = 49152, bool audio_dup = false,
               const std::function<bool()> &should_cancel = {}, int record_orientation = 0,
               bool decode_audio = true);

    /// 打开音频输出。应用先准备 SDL 音频子系统，再调用 start() 建立媒体会话；
    /// 取得 AudioPump 后才可打开声卡。输出失败时，容器录制继续接收音频；
    /// 未录制时停止本地音频接收和续期。
    bool start_playback(std::string &err);
    /// 停止声卡、音频收包和 RR 续期，不调用会中断视频的 stopAll。
    /// 设备侧需等待音频会话到期（当前租期 20 秒）；播放器可能需要手动继续。
    void abandon_audio();

    /// 退出专用：先关闭声卡，停止并等待两个收包线程，再检查录制文件收尾。
    /// 重复调用保留同一结果；禁止在收包线程仍可投递数据时封闭 Recorder。
    /// 录制写入失败期间镜像仍继续，退出状态由调用者根据此结果决定。
    bool finish_recording(std::string &err);

    /// 返回最近一次交付帧的面板尺寸、原始方向及截图标志。
    FrameGeometry frame_geometry() const override { return delivered_geometry_; }

    bool next(scrctl::Frame &out, int timeout_ms) override;

    /// 隧道终止后结束画面源，触发会话清理；暂时截图失败仍按退避重试。
    /// 依据 Stack::pump_error() 判断传输失败，不匹配具体错误文本。普通读超时
    /// 不会停止隧道。该会话不负责重新建立已终止的设备连接。
    [[nodiscard]] bool finished() const override;
    [[nodiscard]] bool failed() const override { return finished(); }
    [[nodiscard]] std::string end_reason() const override;

    [[nodiscard]] bool has_audio() const { return audio_ != nullptr; }

    /// 返回会话使用的 Device 引用，供 --start-app 等设备操作复用，避免重复所有权。
    [[nodiscard]] scrctl::remote::Device &device() { return *device_; }

    /// 转发触摸。HID 在首次使用时连接，避免增加首帧延迟；服务不可用不影响
    /// 镜像。连接或发送失败后停止本次会话的输入重试，避免反复建立连接。
    bool control(double x, double y, bool down, std::string &err);

    /// 发送当前仍按住的完整 keyboard usage 集合，空集合松开全部键。
    /// 与触摸复用同一 HID 连接，调用者串行安排；成功只表示本地发送完成。
    bool keyboard_state(const std::vector<uint16_t> &usages, std::string &err);

    /// 注入 ASCII 文本，复用触摸服务的 HID 连接。
    bool type_text(const std::string &text, int hold_ms, std::string &err);

    /// 按硬件键，首次使用时连接 indigo。可与 --verify 组合，在注入后回读画面
    /// 验证短时效果，例如音量 HUD。
    bool button(uint16_t usage_page, uint16_t usage_code, std::string &err);

    /// 统计同时标明速率的采样窗口和累计计数范围，避免把累计值误读为每秒速率。
    void print_stats() override;

  private:
    bool start_screenshot(bool capture_first, std::string &err);
    void update_picture_source();
    FrameGeometry sample_geometry(bool screenshot) const;
    bool ensure_hid(std::string &err);
    void fail_hid(const std::string &reason);
    /// 只使用已经存在的连接尽力松开输入，不为清理建立新连接。
    void release_hid();

    std::unique_ptr<scrctl::remote::Device> device_;
    /// 两个媒体泵借用此对象；逆序析构必须先停止媒体泵，再销毁录制器。
    std::unique_ptr<scrctl::media::Recorder> recorder_;
    std::unique_ptr<scrctl::media::FramePump> pump_;
    /// 音频泵引用 Device，声卡回调引用 AudioPump。按声明逆序析构，必须在
    /// Device 之后声明，并先关闭声卡、停止音频线程，再销毁设备。
    std::unique_ptr<scrctl::media::AudioPump> audio_;
    AudioOut audio_out_;
    LiveStats stats_;
    /// 设备可见区尺寸，未知为 0/0。
    int display_w_ = 0;
    int display_h_ = 0;
    /// 启动查询得到的原始面板角，未知不等于 rot0，也不被截图渲染角覆盖。
    std::optional<int> panel_degrees_;
    /// 仅在 next 成功时发布；后续 watcher 推送或来源切换不改变已交付帧的快照。
    FrameGeometry delivered_geometry_;
    /// 几何信息所属显示屏，用于区分主屏和外部显示屏的尺寸来源。
    uint64_t display_id_ = 0;
    std::string display_name_;

    /// 显示订阅持有 Device 引用，需先于 Device 析构。订阅不可用时视频沿用
    /// 最后已知朝向；截图方向标为未知，避免用过期角度发送触摸。
    std::unique_ptr<scrctl::remote::DisplayWatcher> watcher_;

    std::unique_ptr<scrctl::hid::Service> hid_;
    std::unique_ptr<scrctl::hid::Buttons> buttons_;
    bool hid_unavailable_ = false;
    std::string hid_error_;
    bool touch_down_ = false;
    bool keyboard_down_ = false;
    double touch_x_ = 0, touch_y_ = 0;
    uint64_t serial_ = 0;
    bool recording_error_reported_ = false;

    // 截图源的序号和失败状态随每次新源一起重置，只由 start_screenshot 安装。
    struct ScreenshotState {
        std::unique_ptr<scrctl::media::ScreenshotSource> source;
        uint64_t serial = 0;
        std::optional<uint64_t> failed_at;
    } screenshot_;
    /// 切回实时流后，request_stop 的截图源暂存在此。next() 仅回收 worker 已
    /// 退出的源，以免渲染线程阻塞在截图 RPC 的 join 上，也避免每次切换留下
    /// 一份 BGRA 帧直到会话退出。此容器必须先于 Device 析构。
    std::vector<std::unique_ptr<scrctl::media::ScreenshotSource>> retired_;

    /// 降级时刻表，单位为相对 degrade_t0_ 的毫秒；为空时不强制切换。
    std::vector<uint64_t> degrade_marks_;
    uint64_t degrade_t0_ = 0;
};

} // namespace scrctl::app
