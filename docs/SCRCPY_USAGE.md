# 与 scrcpy 的用法对照

常用操作以 [scrcpy v5.0](https://github.com/Genymobile/scrcpy/releases/tag/v5.0)
为参照，优先保持参数和快捷键的含义一致。新增功能按实际设备能力逐项验证。
本文记录当前实现；未实现的能力不会通过接受参数后忽略来模拟支持。

## 已对齐的操作

| 用途 | scrctl 参数或操作 |
| --- | --- |
| 镜像已连接设备 | 不带参数运行 |
| 选择设备 | `-s / --serial`，标识为 Apple 设备 UDID |
| 列出设备 | `--list-devices` 合并 USB 与 mDNS 发现；无线默认扫描 3 秒 |
| 禁止窗口控制 | `-n / --no-control` |
| 不采集音频，保持手机原有路由与播放状态 | `--no-audio` |
| 音频转到电脑、手机停止发声 | 默认行为 |
| 保留手机播放并转发音频到电脑 | `--audio-dup` |
| 只关闭电脑音频播放 | `--no-audio-playback`，仍采集音频并使用所选路由 |
| 启动时全屏 | `-f / --fullscreen` |
| 运行中切换全屏 | `MOD+F` 或无修饰 `F11` |
| 退出窗口 | `MOD+Q` 或关闭按钮；终端可以按 Ctrl+C |
| 自定义快捷键修饰键 | `--shortcut-mod=lalt,lsuper`，默认左 Alt 或左 Super |
| 鼠标触摸 | 左键点击和拖动 |
| 窗口设置 | `--window-title`、位置、尺寸、无边框、置顶、禁止屏保 |
| 自动窗口位置 | `--window-x=auto --window-y=auto`，也接受负数坐标 |
| 背景颜色语法 | RGB 或 RRGGBB，均可带 `#` 前缀 |
| 屏幕来源 | `--video-source=display`；旧 `stream` 值继续有效 |

`--shortcut-mod` 接受 `lctrl`、`rctrl`、`lalt`、`ralt`、`lsuper`、`rsuper` 的
逗号列表；任一选定修饰键按下时启用快捷键。Super 对应 Windows 键或 Mac Command。
普通 Q 和 Esc 不退出，默认不会截获右 Alt / AltGr+Q。按键重复不反复切换全屏。
快捷键在 `--no-control` 和文件播放模式中也可用；设备转屏重建窗口保留当前全屏状态。

`--no-control` 与 `--test-touch`、`--test-button`、`--test-type` 相冲突时在连接设备前
报错。`--copy` / `--paste` 保留独立命令行为，使用时仍会显式执行剪贴板操作。

依据：[官方快捷键](https://github.com/Genymobile/scrcpy/blob/v5.0/doc/shortcuts.md)、
[官方窗口选项](https://github.com/Genymobile/scrcpy/blob/v5.0/doc/window.md)。

## 现有参数中的差异

| 项目 | 当前行为及使用边界 |
| --- | --- |
| `--record / -r` | 保存裸 HEVC Annex-B，只有视频。请使用 `.hevc`；指定 `.mp4` 不会生成 MP4 容器 |
| `--display-orientation` | 只改变本机显示；现有 `--orientation` 是其兼容别名，也不改变录制 |
| `--crop` | 裁剪本机显示的源像素；视频和截图各使用自身的像素坐标，不改变设备采集或录制 |
| `--video-source` | 支持 display 和扩展值 screenshot（截图轮询）；没有 camera 能力 |
| `--no-window` | 不创建窗口，仍接收和解码视频；可继续播放音频 |
| `--copy / --paste` | 写入 / 读取设备剪贴板后退出，不模拟设备上的粘贴动作 |
| 窗口标题与比例 | 默认标题为 scrctl，默认背景为黑色；可以自由调整窗口比例 |
| Wi-Fi | `--list-devices` 用 mDNS 枚举；`--pair` 经 USB 创建/验证记录，`--wifi auto -s <UDID>` 自动选择无线候选；手动地址可用 `--wifi-port` 指定端口。scrctl 承担配对与连接，Android 的对应入口由 ADB 提供 |
| 编码控制 | 当前 Apple 服务决定尺寸、码率、FPS 和编码器，未提供相应覆盖参数 |
| 视频解码 | 默认软件解码；VideoToolbox 适配存在 NAL 长度限制，见 README |
| 音频路由 | 默认转到电脑，`--audio-dup` 保留手机播放，语义与 scrcpy 对齐；切路由可能暂时暂停手机播放器，需要手动继续。默认路由失败时音频禁用、视频继续，不自动改为双端播放 |

scrcpy 的 `--orientation` 可以同时改变显示和录制；它的 crop 作用于采集，录制可以
包含 MP4 / MKV 容器及音频。scrctl 的原始码流录制尚不能承担这些含义，所以帮助中
明确显示与录制的区别。参见
[视频方向](https://github.com/Genymobile/scrcpy/blob/v5.0/doc/video.md#orientation)、
[录制](https://github.com/Genymobile/scrcpy/blob/v5.0/doc/recording.md)。

音频路由参照 [scrcpy 音频说明](https://github.com/Genymobile/scrcpy/blob/master/doc/audio.md#source)。
scrctl 的默认路由已在 macOS、iPhone14,4 / iOS 27 的 USB 与 Wi-Fi 音乐播放中验证：
电脑发声、手机无声，退出后手动继续可恢复手机播放。Windows、Linux 与旧系统
的该路由尚未验收。程序不调整手机音量，也不自动按播放键。
本轮 QQ 音乐测试观察到默认路由激活时暂停一次，需要在手机上手动继续。

`--no-audio` 与 scrcpy 一致，跳过音频采集和路由请求，保持手机原有的播放与
输出状态；它不会自动恢复已经暂停的播放器。`--no-audio-playback` 仅关闭电脑
音频输出，仍采集并使用默认电脑路由（手机仍无声），或使用 `--audio-dup`
保留手机播放。只需要手机发声时可组合 `--audio-dup --no-audio-playback`。
`--audio-dup` 与 `--no-audio` 冲突，会在连接前报错。

本机播放后端初始化失败时不请求手机音频；声卡打开失败或设备答复无效（含路由不符）时
停止音频收包及续期，保留视频。已切换的设备路由可能需等待音频会话到期
（当前租期 20 秒），播放器仍可能需手动继续；具体回收时刻未在所有设备上验证。
恢复时的无效协商答复不会进入反复切换路由的重试。
启动期间收到 Ctrl+C 时，程序在步骤之间检查退出请求，不再继续起流、切换
音频路由或执行设备输入。已经开始的连接和 RPC 仍可能等待各自的协议超时。

## 后续对齐顺序

1. 无线流程已接入主程序：继续验证正式配对、记录复用、按 UDID 自动选择及
   候选重试在更多设备和网络上的行为。Android 由 ADB 承担的配对与连接由 scrctl 处理。
2. Windows 音频：FFmpeg AAC-ELD 后端已接入，真实音乐解码及短时 USB 发声通过；
   无线播放已正常运行 120 秒。Mac 的重采样时钟补偿改善了刺啦声，新调节层
   仍需 Windows 回归；继续验证持续连接和安装包。
3. 容器录制：使用 libavformat 写 MP4 / MKV，保留时间戳、关键帧和编解码参数；
   验证音视频同步、断流恢复和退出收尾。随后让 `--orientation` 同时设置显示与录制，
   并提供独立的 `--record-orientation`，录制旋转优先使用容器方向信息。
4. 日常键盘输入与粘贴：接入窗口键盘事件，维护按下 / 抬起状态，在焦点丢失、
   转屏或退出时释放；区分键盘报告和文字输入，验证中文等文本的粘贴。
5. 窗口动作：MOD+G / W、双击留边调整大小、显示旋转快捷键，以及转屏后的窗口
   位置和用户尺寸保留。目前只保留全屏状态，不把这些动作记为已实现。
6. HOME、音量、锁定等已具备底层能力的快捷键：接入主窗口回调并验证设备效果。
   Android BACK、APP_SWITCH、通知栏等不能直接套在 iOS 上。
7. 输出控制：明确采集 / 解码 / 播放开关，随后考虑音频单独模式。

这些是能力待办。Q 退出规则修正不代表窗口键盘输入已经完成；离线 SDL 事件测试
也不能替代 VM、真实键盘、系统快捷键拦截或设备输入的验证。
