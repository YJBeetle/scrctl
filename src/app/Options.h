#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace scrctl::app {

struct Options {
    std::string path;   ///< 空 = 走真机实时流
    std::string serial; ///< scrcpy 的 --serial：指定哪台设备
    /// `--wifi=<局域网地址>`：走无线那条路（pair-verify + TLS-PSK 隧道），而不是 USB。
    /// 设备还是要**插着或者曾经插过**——配对记录得先在这台机器上存在。
    std::string wifi;
    std::string record; ///< 实时流顺手把 Annex-B 录到文件
    bool list_devices = false;
    bool no_control = false; ///< scrcpy 的 --no-control：只看不动
    std::string title = "scrctl";
    bool stats = false;
    /// --video-source=stream|screenshot。默认 stream：媒体流被设备按版本拒时自动降到
    /// screenshot（见 LiveSource::start 的兜底门）；显式给 screenshot 是强制走兜底。
    std::string video_source = "stream";
    bool crop_set = false;
    int crop_w = 0, crop_h = 0, crop_x = 0, crop_y = 0;
    double scale = 1.0;       ///< 窗口相对裁剪尺寸的缩放
    bool scale_given = false; ///< 显式给过 --scale 就别再自动缩进屏幕
    bool debug_input = false; ///< 把每次鼠标事件的原始坐标与算出的归一化值都打出来
    /// 把隧道内 TCP 的逐段"序号不连续"日志打开。默认关：链路差的那几分钟里它刷屏
    /// （实测 8 秒 5 行），而聚合读数（收到的字节、丢弃占比）已经在 --stats 里。
    bool debug_net = false;
    int exit_after = 0; ///< 渲染多少帧后退出（0=不限）
    int verify_at = 0;  ///< 渲染到第 N 帧时回读窗口内容
    std::string verify_path;
    /// 注入一条直线后退出：`--test-touch x0,y0,x1,y1`。
    /// 窗口与鼠标不在场时也要能验证输入通路，理由同 --verify：日志说"注入
    /// 调用返回成功"证明不了设备上真的收到了触摸。
    std::string test_touch;
    /// 起流后按一次硬件按键（home/lock/volup/voldn/mute），然后照常镜像。
    /// 按键效果是瞬时的，所以它要能和 --verify 组合：按完等第 N 帧回读窗口。
    std::string test_button;
    /// 起流后往设备敲一段 ASCII（要有文本框正获得焦点）。
    std::string test_type;
    /// `--test-degrade 4,8,12`：从起流那一刻算起，到点交替"强制判媒体流解不出画面 /
    /// 放开"。运行中降级这一格在真机上打不响（要画面复杂到超出解码后端上限），而
    /// 切换/序号/回收那几条修复全在这一格上——没有开关就只能一直交离线判据。
    std::string test_degrade;
    /// scrcpy 的 --start-app=name：起流之后把某个 App 拉到前台。名字里可以带两个
    /// 前缀，语义照 scrcpy：`+` = 先杀掉在跑的实例再冷启动，`?` = 按 App 名字前缀
    /// 匹配（大小写不敏感）而不是按 bundle id 精确匹配。
    std::string start_app;
    bool list_apps = false; ///< --list-apps：列出设备上装的 App 后退出
    std::string copy_text;  ///< --copy TEXT：写进设备剪贴板后退出
    bool paste = false;     ///< --paste：读设备剪贴板打印后退出
    bool no_window = false; ///< --no-window：不起窗口，只收流（脚本/自动化用）
    /// --display-orientation：画面顺时针转这么多度。-1 = auto，跟着设备报的
    /// `currentOrientation` 走。
    int orientation = -1;
    int win_w = 0, win_h = 0; ///< --window-width/height：显式窗口尺寸，0=自动
    /// 用平台硬件解码后端（VideoToolbox），而不是默认的软件解码。
    /// 见 FramePump::Options::use_hardware——默认软解的原因是硬解吃不下超过 65535 字节的帧。
    bool hw_decode = false;
    /// scrcpy 的 --no-audio：连音频腿都不起（不占设备上那条会话、不解码）。
    bool no_audio = false;
    /// --- 下面这批是窗口与运行控制的 scrcpy 同名项，逐个都是"照抄名字"级别的活 ---
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
    /// scrcpy 的 --no-audio-playback：收流与解码照跑，只是不在电脑上出声。
    /// 录制或排障要"有音频数据但安静"时用它——本机夜里跑真机回归也靠它。
    bool no_audio_playback = false;
    /// scrcpy 的 --audio-buffer=ms（默认同为 50）。它同时是两件事的那一个数：
    /// 开口放之前先攒多久，以及缓冲想维持的水位（高出它就开始悄悄排）。
    int audio_buffer_ms = 50;
};

} // namespace scrctl::app
