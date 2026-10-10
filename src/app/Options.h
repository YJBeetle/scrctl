#pragma once

#include <SDL_keycode.h>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace scrctl::app {

struct Options {
    std::string path;   ///< 空表示使用设备实时画面
    std::string serial; ///< scrcpy 的 --serial：指定哪台设备
    /// --wifi 指定局域网地址，使用远程配对验证和 TLS-PSK 隧道。
    /// 本机需要该设备的远程配对记录，无需在无线使用时保持 USB 连接。
    std::string wifi;
    uint16_t wifi_port = 49152; ///< 手动 --wifi 地址的 RemotePairing 端口；发现模式采用 SRV 端口。
    bool pair = false; ///< USB pairing; with --wifi auto accept phone-initiated PIN pairing.
    int pairing_timeout_ms = 120000;
    bool repair_pairing = false; ///< --repair-pairing：允许更新被拒绝或缺少设备身份的旧记录。
    std::string record; ///< .mp4 / .mkv 为容器录制，其余路径兼容裸 HEVC
    bool list_devices = false;
    int discovery_timeout_ms = 3000; ///< --list-devices 的无线扫描时限；0 只列 usbmux
    bool no_control = false; ///< --no-control：关闭输入控制
    /// 任意一个选定的修饰键按下时启用窗口快捷键；默认与 scrcpy 一致。
    uint16_t shortcut_mods = KMOD_LALT | KMOD_LGUI;
    std::string title = "scrctl";
    bool stats = false;
    /// 画面来源：stream 默认实时流，系统版本拒绝时可自动切到截图；
    /// screenshot 强制使用截图轮询。
    std::string video_source = "stream";
    bool crop_set = false;
    int crop_w = 0, crop_h = 0, crop_x = 0, crop_y = 0;
    double scale = 1.0;       ///< 窗口相对裁剪尺寸的缩放
    bool scale_given = false; ///< 显式缩放时禁用自动适应屏幕
    bool debug_input = false; ///< 输出鼠标原始坐标及设备归一化坐标
    /// --stats 中额外显示隧道校验和异常和 ICMPv6 诊断计数。
    bool debug_net = false;
    int exit_after = 0; ///< 渲染多少帧后退出（0=不限）
    int verify_at = 0;  ///< 渲染到第 N 帧时回读窗口内容
    std::string verify_path;
    /// --test-touch x0,y0,x1,y1 注入直线后退出，可用于无窗口验证。
    /// API 返回成功不证明触摸已作用于设备，仍需检查设备画面。
    std::vector<double> test_touch; ///< 空或四个 [0, 1] 内的有限坐标，由 CLI11 校验
    /// 启动后注入硬件键，再继续镜像；可用 --verify 检查瞬时效果。
    std::string test_button;
    uint16_t test_button_code = 0; ///< 参数层将按键名称映射成 HID usage
    /// 启动后注入 ASCII 文本（要有文本框正获得焦点）。
    std::string test_type;
    /// --test-degrade 指定启动完成后的切换秒数，交替强制判定视频不可用和
    /// 解除强制。复用生产状态机，供真机验证画面切换、序号和回收。
    std::string test_degrade;
    /// 启动后将应用打开到前台。? 按名称前缀匹配（忽略大小写），
    /// + 先终止原实例；否则按 bundle ID 精确指定。
    std::string start_app;
    bool list_apps = false; ///< --list-apps：列出设备上装的 App 后退出
    /// --copy TEXT：写入设备剪贴板后退出；空字符串也是显式写入，未指定时不执行。
    std::optional<std::string> copy_text;
    bool paste = false;     ///< --paste：读设备剪贴板打印后退出
    bool no_window = false; ///< 不创建窗口；采集由音频、录制或旧帧诊断消费者决定。
    /// 关闭视频采集；播放关闭且无视频消费者时也归一化到此状态。
    bool no_video = false;
    /// 不显示视频；仍可录制编码视频并在背景窗口转发键盘。
    bool no_video_playback = false;
    /// --display-orientation：画面顺时针转这么多度。-1 = auto，跟着设备报的
    /// `currentOrientation` 走。
    int orientation = -1;
    /// 在裁剪后的源图像上先水平翻转，再施加窗口旋转。
    bool display_flip = false;
    /// 容器的视频方向元数据，顺时针 0/90/180/270；不改设备编码或本机显示。
    int record_orientation = 0;
    int win_w = 0, win_h = 0; ///< --window-width/height：显式窗口尺寸，0=自动
    /// 使用平台硬件解码后端。当前 VideoToolbox 适配的 2 字节 NAL 长度
    /// 限制为 65535 字节，默认软件解码可处理更大的关键帧。
    bool hw_decode = false;
    /// --no-audio 禁止建立音频会话和解码音频。
    bool no_audio = false;
    /// --audio-dup 保留手机播放。默认将音频转到电脑；切换路由可能暂停播放器。
    bool audio_dup = false;
    /// 窗口与运行控制选项。
    bool always_on_top = false; ///< --always-on-top
    bool borderless = false;    ///< --window-borderless
    bool fullscreen = false;    ///< -f / --fullscreen（桌面全屏）
    std::optional<int> win_x;   ///< --window-x
    std::optional<int> win_y;   ///< --window-y
    /// --background-color=#RRGGBB：等比留边那两条边的颜色。默认黑。
    uint8_t bg[3] = {0, 0, 0};
    std::string render_driver;        ///< --render-driver（metal / software / ...）
    bool disable_screensaver = false; ///< --disable-screensaver
    int time_limit = 0;               ///< --time-limit=秒，到点正常退出（会停流）
    bool show_version = false;        ///< --version
    /// --no-audio-playback 禁用本机播放；仅容器录制需要音轨时保留采集。
    bool no_audio_playback = false;
    /// --audio-buffer 指定首次预缓冲和目标缓冲水位，单位毫秒，默认 50。
    int audio_buffer_ms = 50;
};

} // namespace scrctl::app
