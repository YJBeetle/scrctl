#pragma once

#include "app/AudioOut.h"
#include "app/FrameSource.h"
#include "app/StatsWindow.h"
#include "hid/Hid.h"
#include "media/FramePump.h"
#include "media/ScreenshotSource.h"
#include "media/StreamSession.h"
#include "remote/Device.h"
#include "remote/DisplayInfo.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace scrctl::app {

/// 真机实时流。收包、拆 AU、解码、以及"画面坏了就重起会话"全在 FramePump 里，
/// 这里只管起流、取帧、以及把窗口的输入投回设备。
class LiveSource final : public FrameSource {
  public:
    ~LiveSource() override;

    /// `watch_display`：挂一条常驻订阅跟着转屏改朝向。不起窗口就别挂——那条
    /// 订阅要独占一个连接、还有一个每 250ms 醒一次的线程，而没有窗口就没人
    /// 消费朝向，纯开销。
    ///
    /// `want_audio`：起不起音频腿。它是**另一条设备侧会话**，起不来或者这个构建
    /// 根本没有音频后端都不致命——没有声音的镜像仍然是可用的镜像，所以这里只打一行。
    /// `audio_buffer_ms` = `--audio-buffer`：缓冲想维持的水位。
    bool start(const std::string &serial, const std::string &wifi, const std::string &record_path,
               bool hw_decode, bool watch_display, bool want_audio, int audio_buffer_ms,
               const std::string &video_source, const std::string &test_degrade, std::string &err);

    /// 打开声卡。要和 `start()` 分开的唯一原因：`start()` 跑在 `SDL_Init` 之前
    /// （窗口还没建就得先有源），而 SDL 的音频子系统在那之后才有。
    bool start_playback(std::string &err);

    void stop_playback() { audio_out_.close(); }

    /// 起流前向设备要来的可见区尺寸（问不到是 0/0，见 `resolve_crop` 的顺序）。
    void display_size(int &width, int &height) const override;

    /// 当前该顺时针转多少度。
    ///
    /// 优先问常驻订阅（`watcher_`），拿不到才退回起流前问到的那一档。顺序不能反：
    /// 常驻订阅是唯一会跟着转屏动的来源，而那一档是窗口打开那一刻的快照。
    ///
    /// 只有朝向是"活的"。可见区尺寸仍然只在起流前问一次——实测转屏时设备报的
    /// `currentMode.size` 根本不变（docs §16.1），而中途改尺寸要重建裁剪框、
    /// 触摸分母与整条几何日志，那些路径现在一条都没验过。
    [[nodiscard]] int orientation_degrees() const override;

    bool next(scrctl::Frame &out, int timeout_ms) override;

    /// 设备走了就**别再重试**，走正常退出路径（停流、关会话），与 --time-limit 同一条。
    ///
    /// 这是把两件事分开：截图/取帧失败一次是常事（RPC 偶发不通、屏幕睡了），下一轮退避
    /// 后会自愈；而隧道死了永远不会自愈。改之前两者在代码里是同一个失败，于是拔线之后
    /// 实测一路退避重试到 --time-limit（60 秒里 193 次失败、0 张新图），窗口模式下用户
    /// 看到的就是"画面停住、没有一句解释"（docs §28）。
    ///
    /// 判据用栈自己的 `pump_error()`：它只在泵线程因**读失败**退出时置位（超时不算），
    /// 是个结构性事实。**故意不匹配错误文本**——文本会随实现变，而"泵已经停了"不会。
    [[nodiscard]] bool finished() const override;
    [[nodiscard]] std::string end_reason() const override;

    [[nodiscard]] bool has_audio() const { return audio_ != nullptr; }

    /// 给"起流之后还要对设备做点别的"那些项用（--start-app）。它故意返回引用而不是
    /// 让每个功能自己存一份：一个会话只有一个 Device，多副本只会多一处要同步的寿命。
    [[nodiscard]] scrctl::remote::Device &device() { return *device_; }

    /// 把窗口里的一次触摸投到设备上。
    ///
    /// HID 服务**第一次用到时才连**：连接要一个来回，没必要把它算进起流路径；
    /// 而且设备不提供该服务（DDI 版本差异）时镜像应当照常工作，而不是整个退出。
    /// 失败过一次就不再重试，免得每帧都去撞一遍。
    bool control(double x, double y, bool down, std::string &err);

    /// 往设备敲一段 ASCII（复用触摸那条连接，键盘是同一个服务下的另一个面）。
    bool type_text(const std::string &text, int hold_ms, std::string &err);

    /// 按一个硬件按键（indigo 服务，惰性连）。按键的效果多半是瞬时的，所以
    /// 它得能和 `--verify` 组合使用：先按键，再等第 N 帧回读窗口内容。
    bool button(uint16_t usage_page, uint16_t usage_code, std::string &err);

    /// **必须打速率，不能打累计数。** 第一版这里打的是累计包数，结果"包 1778"
    /// 被当成每秒读数读了 16 秒，直接把结论带偏到"设备只编 12 帧"上——而它真正的
    /// 意思是这一段里我们一共只收到 110 包/秒。一个没有分母的数不是读数。
    void print_stats() override;

  private:
    std::unique_ptr<scrctl::remote::Device> device_;
    std::unique_ptr<scrctl::media::FramePump> pump_;
    /// 音频腿与声卡出口。它们都引用 `Device&` / `AudioPump`，所以**必须声明在
    /// device_ 之后**（成员反序析构：泵要先停、线程要先 join，才能拆 Device）。
    std::unique_ptr<scrctl::media::AudioPump> audio_;
    AudioOut audio_out_;
    uint64_t last_packets_ = 0;
    uint64_t last_dev_packets_ = 0;
    uint64_t last_dev_change_ms_ = 0;
    double last_dev_rate_ = 0;
    /// 上一个 SR 增量是除以多长的间隔算出来的。打印时要用它，不然读者会把这个
    /// "每秒"当成和"我收到"同一个分母，然后去减两个不同分母的数。
    uint64_t last_dev_span_ms_ = 0;
    uint64_t last_aus_ = 0;
    uint64_t last_decoded_ = 0;
    uint64_t last_audio_packets_ = 0;
    uint64_t last_audio_decoded_ = 0;
    uint64_t last_audio_delivered_ = 0;
    /// **一本账一把尺**（审查 P2）。以前这三本共用一个 `last_stats_ms_`，而计数基线
    /// 各自更新：截图分支每秒推尺，媒体泵与音频的基线却只在媒体分支里动。兜底那 30 秒
    /// 里两条腿都还在计数，切回实时流后第一段 --stats 就把整段增量除以约 1 秒，速率
    /// 虚高几十倍——而打印出来的分母写着 1.0s，看起来完全自洽。
    /// 三把尺都在**各自的源建好时**起表（见 start() 与 next() 里的赋值点），所以第一次
    /// 结算的分母是真实经过的时间，不是 `settle_window` 里那个 1.0 的兜底。
    uint64_t last_stream_ms_ = 0;
    uint64_t last_shot_ms_ = 0;
    uint64_t last_audio_ms_ = 0;
    uint64_t last_shot_frames_ = 0;
    /// 隧道内 TCP 那一行自己的尺（与画面从哪来无关，且只在真丢过东西时才打）。
    uint64_t last_tcp_ms_ = 0;
    uint64_t last_tcp_recv_ = 0;
    uint64_t last_tcp_drop_ = 0;
    /// 那一行打过没有：第一段一定打一次，好让"丢弃 0"与"账没接上"分得清。
    bool tcp_line_printed_ = false;
    /// 起流之前向设备要来的**可见区**尺寸（0/0 = 没问到）。见 `display_size()`。
    int display_w_ = 0;
    int display_h_ = 0;
    /// 同一问带回来的界面旋转（顺时针度数）。0 也是有效值（竖屏），所以它不像尺寸
    /// 那样用"零"表示没问到——没问到就是 0，正立竖屏也是 0，两者行为本来就该一样。
    int degrees_ = 0;
    /// 尺寸是从哪块屏拿的，只为把日志那行说全（多屏设备上这不是废话：主屏与
    /// 无线屏的尺寸实测就不一样）。
    uint64_t display_id_ = 0;
    std::string display_name_;

    /// 常驻的显示几何订阅。它持有 `Device&`，所以**必须声明在 device_ 之后**
    /// （成员按声明反序析构，它得比 Device 先走）。起不来不致命：朝向就退回
    /// 起流前那一档，代价是"转屏不跟着转"。
    std::unique_ptr<scrctl::remote::DisplayWatcher> watcher_;

    std::unique_ptr<scrctl::hid::Service> hid_;
    std::unique_ptr<scrctl::hid::Buttons> buttons_;
    bool hid_unavailable_ = false;
    uint64_t serial_ = 0;

    /// 截图轮询兜底源。两种顶上方式：起流就被设备按版本拒（iOS 27 以下），或者跑着跑着
    /// 解码后端解不出关键帧（FramePump::video_unusable，见 next()）。画面约 2 fps，但
    /// 触摸/按键注入走的是同一条 HID 路，不受影响。任一时刻与 pump_ 只有一条在出画面。
    std::unique_ptr<scrctl::media::ScreenshotSource> shot_;
    /// 「已经取到 shot_ 的第几张」。**不变量：每次给 shot_ 换一个新源，凡是"按源记的账"
    /// 都要归零/起表**——这个数（新源从 0 起算，而 latest() 只接受大于它的序号）、
    /// `last_shot_frames_`（--stats 拿它做差算速率，不归零就下溢）与 `last_shot_ms_`
    /// （那本账的尺，不起表则第一次结算的分母是个假的 1.0 秒）。两个赋值点各自处理：
    /// 起流就降级那条在 start() 里，运行中降级那条在 next() 里。
    uint64_t shot_serial_ = 0;
    /// 运行中降级时截图源起失败的时间点（SDL 时钟）：冷却期（kShotRetryMs）内不再撞，
    /// 冷却一过再试——一次失败不判永久，否则本次会话就一路停在旧画面上（审查 P2）。
    bool shot_failed_ = false;
    uint64_t shot_fail_ms_ = 0;
    /// 切回实时流时退下来的截图源：`request_stop()` 已叫过，销毁（= join）由 next() 每帧
    /// 跑一趟 `app::reap_finished` 逐步做，worker 还没退的就留到下一帧。渲染线程上直接
    /// join 一个可能卡在截图 RPC 里的线程会把窗口冻到 RPC 上限（审查 P3），而一路留到
    /// teardown 又会按切换次数堆内存——每个都揣着一整张 BGRA 画面（审查 P2，第四轮）。
    /// 声明在 device_ 之后，所以先于 device_ 析构（截图源持有 Device&）。
    std::vector<std::unique_ptr<scrctl::media::ScreenshotSource>> retired_;

    /// `--test-degrade` 的时刻表（毫秒，相对 `degrade_t0_`）与起点。空表 = 这条旗标没给，
    /// `next()` 里那次判断恒为假，产品路径一点不受影响。
    std::vector<uint64_t> degrade_marks_;
    uint64_t degrade_t0_ = 0;
};

} // namespace scrctl::app
