#pragma once

#include "app/AudioOut.h"
#include "app/FrameSource.h"
#include "app/LiveStats.h"
#include "app/RecordFormat.h"
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
#include <utility>
#include <vector>

namespace scrctl::app {

/// 设备会话源，负责会话建立、画面来源切换和输入转发。
/// 视频由 FramePump 管理；无视频时仍可提供独立音频或仅输入控制。
class LiveSource final : public FrameSource {
  public:
    ~LiveSource() override;

    struct Options {
        std::string serial, wifi, record_path;
        std::optional<RecordFormat> record_format;
        uint16_t wifi_port = 49152;
        bool hw_decode = false;
        bool watch_display = true;
        bool want_video = true;
        bool decode_video = true;
        bool want_audio = true;
        bool decode_audio = true;
        int audio_buffer_ms = 50;
        bool audio_dup = false;
        std::string video_source = "stream";
        std::string test_degrade;
        int record_orientation = 0;
        std::function<bool()> should_cancel;
    };

    /// 命名选项区分设备连接、视频、音频和各自的解码需求。
    /// 无视频时跳过显示查询/订阅/媒体/截图，仍可保留音频或输入控制连接。
    /// 无视频录制仅支持包含音轨的 MP4/MKV；格式、音频需求和视频方向先于连接检查。
    /// should_cancel 在启动步骤间检查；正在执行的底层 RPC 仍受自身超时约束。
    bool start(const Options &options, std::string &err);

    /// 打开音频输出。应用先准备 SDL 音频子系统，再调用 start() 建立媒体会话；
    /// 取得 AudioPump 后才可打开声卡。输出失败时，容器录制继续接收音频；
    /// 未录制的镜像停止本地音频，纯音频由应用报告原错并执行最终停止。
    bool start_playback(std::string &err);
    /// 停止声卡、音频收包和 RR 续期；与视频并用时不调用会中断视频的 stopAll。
    /// 默认设备音频会话需等待租期；纯音频拥有者使用既有会话尽力 stopAll。
    void abandon_audio();

    /// 退出专用：先关闭声卡并 join 收包线程，seal/join Recorder 后再停止设备音频。
    /// 重复调用保留同一结果；禁止在收包线程仍可投递数据时封闭 Recorder。
    /// 解码模式下写入失败期间镜像仍继续；仅编码视频或纯音频录制首错会结束来源。
    /// 慢速音频 stopAll 不阻塞 Final 尾包结算；设备清理错只影响退出结果，录制首错优先。
    bool finish_recording(std::string &err);

    /// 返回最近一次交付帧的面板尺寸、原始方向及截图标志。
    FrameGeometry frame_geometry() const override { return delivered_geometry_; }

    bool next(scrctl::Frame &out, int timeout_ms) override;

    /// 隧道、纯音频不可重试协商错误或纯音频录制首错结束会话；暂时恢复仍按退避重试。
    /// 依据 Stack::pump_error() 判断传输失败，不匹配具体错误文本。普通读超时
    /// 不会停止隧道。该会话不负责重新建立已终止的设备连接。
    [[nodiscard]] bool finished() const override;
    [[nodiscard]] bool failed() const override { return finished(); }
    [[nodiscard]] std::string end_reason() const override;

    [[nodiscard]] bool has_audio() const { return audio_ != nullptr; }
    [[nodiscard]] bool has_video() const { return want_video_; }
    [[nodiscard]] bool video_decoding_enabled() const { return want_video_ && decode_video_; }

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

    /// 转发 Consumer 按钮 DOWN/UP，indigo 独立连接，与触摸/键盘共享首错门控。
    /// 每次 DOWN 都发送，按 page/code 仅保存一份待释放状态；失败也可能部分送达。
    /// 未按住的 UP 成功且不建立连接，失败清理已完成后的 UP 也不重复发送/报错。
    bool button_state(uint16_t usage_page, uint16_t usage_code, bool down, std::string &err);

    /// 按硬件键，首次使用时连接 indigo。可与 --verify 组合，在注入后回读画面
    /// 验证短时效果，例如音量 HUD。
    bool button(uint16_t usage_page, uint16_t usage_code, std::string &err);

    /// 统计同时标明速率的采样窗口和累计计数范围，避免把累计值误读为每秒速率。
    void print_stats() override;

  private:
    bool start_screenshot(bool capture_first, std::string &err);
    std::string encoded_capture_error() const;
    void update_picture_source();
    FrameGeometry sample_geometry(bool screenshot) const;
    bool ensure_hid(std::string &err);
    bool ensure_buttons(std::string &err);
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
    std::vector<std::pair<uint16_t, uint16_t>> buttons_down_;
    double touch_x_ = 0, touch_y_ = 0;
    uint64_t serial_ = 0;
    bool recording_error_reported_ = false;
    bool want_video_ = true;
    bool decode_video_ = true;

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
