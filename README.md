# scrctl

基于 CoreDevice 开发者服务的 iOS 镜像与控制工具，提供原生窗口和命令行接口。
支持 USB 和已配对的 Wi-Fi 连接；镜像时设备仍可直接操作。命令行设计参考 scrcpy。

## 功能与验证范围

历史真机记录覆盖了屏幕镜像、触摸、硬件按键、ASCII 键盘输入、剪贴板、音频、
自动转屏和截图轮询。默认视频解码使用 FFmpeg，可选 VideoToolbox；录制保存裸
Annex-B HEVC，不包含音轨。

主要真机验证环境为 macOS、iPhone14,4 / iOS 27.0。已有活动画面记录约为 60 fps，
静止画面允许暂停输出。本轮重构已验证 USB / Wi-Fi 传输及强制视频与截图切换，
并对丢包和关键帧解码失败做了离线或本地故障注入；这些结果不代表所有机型、
真实弱网或长时间运行已经验证。

| 平台 | 当前范围 |
| --- | --- |
| macOS | 有真机连接、镜像、输入和音频记录；本轮验证结果见重构记录 |
| Linux | CI 包含构建与离线测试，音频使用 FFmpeg；尚缺 Linux 真机端到端验证 |
| Windows | ARM64 构建与离线测试、USB / Wi-Fi 镜像、截图切换及竖横屏鼠标触摸已验证；FFmpeg 真实音乐解码与短时 USB 播放已通过，持续连接与初始配对准备待验证，见 [安装说明](docs/WINDOWS.md) |

音频在 macOS 使用 AudioToolbox，Windows / Linux 使用 FFmpeg 的原生 AAC-ELD
解码器与 libswresample。当前 FFmpeg 适配支持 48 kHz、双声道、每帧每声道 480 或
512 个采样；真实音乐的 480 配置已与 AudioToolbox 逐样本对照，最大差异为 1 个
16 位量化单位。旧实验使用了错误的编码配置，其“不支持 ELD”的结论已修正，见
[音频记录](docs/coredevice.md#171-音频解码后端只能选-audiotoolbox实测同一份-dump-四路对照)。
本机播放音量由电脑控制，也可使用 `--no-audio-playback` 关闭；手机媒体音量可能
同时影响捕获声音，不能靠将手机静音来实现只在电脑播放。

音频默认转到电脑，手机停止发声；`--audio-dup` 保留手机播放，同时把音频转发到
电脑。切换路由时播放器可能暂停；本轮 QQ 音乐测试观察到路由激活时暂停一次，
需要在手机上手动继续。退出后若播放器仍暂停，也需要手动继续。程序不调整手机
音量、不自动按播放键；默认路由不可用时会报错并继续视频，不自动改为双端播放。

`--no-audio` 与 scrcpy 的含义一致：跳过音频采集和路由请求，保持手机原有的
播放与输出状态，也不恢复已经暂停的播放器。`--no-audio-playback` 只关闭电脑
播放，仍采集音频并遵循所选路由：单独使用时仍将音频转到电脑，手机无声；
需要手机继续发声时使用 `--audio-dup --no-audio-playback`。

播放后端初始化失败时不启动音频；起流后若声卡打开失败或设备答复无效（含路由不符），
会停止音频接收和续期，并保留视频。已建立的设备音频路由可能需等待会话到期
（当前音频租期 20 秒），之后播放器仍可能需要手动继续；恢复时协商答复无效也会
停止音频重试。具体回收时刻尚未在所有设备上验证。

默认路由已在 macOS、iPhone14,4 / iOS 27 的 USB 与 Wi-Fi 音乐播放中验证：电脑有声、
手机无声，结束后手动继续可恢复手机播放。该路由的 Windows、Linux 和旧系统
行为尚未验收；音频转发已开始不代表播放器在切换期间从未暂停。

启用 FFmpeg 的构建使用 libswresample 平滑补偿音频时钟差异，macOS 的 AudioToolbox
解码也共用这层补偿。`--audio-buffer` 默认 50 ms，目标保持固定；网络或调度抖动
较大时可增加到 `--audio-buffer=200`，以较高延迟换取更连续的播放。

[协议调研记录](docs/coredevice.md) 保留原始观察与实验过程；
[重构记录](docs/REFACTOR.md) 说明当前实现及验证边界；
[路线图](docs/ROADMAP.md) 列出剩余工作。
GitHub Actions 配置了 macOS、Ubuntu、Windows ARM64 和 ASan / UBSan 离线作业，真机测试单独进行。
Windows ARM64 和 macOS 作业另保存可搬移安装包；已下载的真实产物验证结果见各平台说明。

## 实现结构

| 模块 | 职责 |
| --- | --- |
| `src/transport` | usbmuxd、lockdown TLS 与 CDTunnel 包传输 |
| `src/net` | lwIP IPv6 / TCP / UDP 适配及隧道 I/O |
| `src/http2` | RemoteXPC 使用的 HTTP/2 帧子集，不消费 HPACK header block |
| `src/xpc`、`src/remote` | XPC 编解码、服务目录、CoreDevice RPC 和设备会话 |
| `src/wifi` | 远程配对、记录及密码运算 |
| `src/media`、`src/rt` | 媒体协商、RTP 组帧、恢复、音频和截图源 |
| `src/bitstream`、`src/decode` | Annex-B / NAL 与视频、音频解码后端 |
| `src/hid` | 触摸、键盘和硬件按键报告 |
| `src/app` | CLI、SDL 显示、输入映射和应用生命周期 |
| `src/i18n`、`po` | gettext 中英文消息和语言选择 |

scrctl 是独立产品，也是设备协议的验证项目。MaaFramework 后续倾向于参考这里
验证过的实现独立接入；当前先完成 scrctl，不以公共 SDK 或稳定 C ABI 为目标。

## 窗口操作与 scrcpy 用法

常用参数和窗口快捷键以 scrcpy 为参照，当前支持：

| 操作 | 快捷键 |
| --- | --- |
| 退出 | MOD+Q，或关闭窗口 |
| 切换全屏 | MOD+F，或不带修饰键的 F11 |
| 设备触摸 | 鼠标左键点击、拖动 |

MOD 默认是左 Alt 或左 Super（Windows 键 / Mac Command），可以通过
`--shortcut-mod=rctrl` 等配置。普通 Q 和 Esc 不退出；窗口目前尚不转发日常键盘
输入，`--test-type` 是单独的诊断注入入口。与 scrcpy 的具体差异和后续对齐项见
[用法对照](docs/SCRCPY_USAGE.md)。

## 命令行语言

支持英文和简体中文，注释保持中文。默认 `--lang auto`，按首个非空的 `LC_ALL`、
`LC_MESSAGES`、`LANG` 选择语言；`zh` 系列 locale 使用简体中文，其他语言、
`C`、`POSIX` 或未设置时回退英文。`--lang en` / `--lang zh-CN` 显式覆盖环境。
`LANGUAGE` 不参与 scrctl 的自动选择，避免与上述规则冲突。

```bash
LANG=zh_CN.UTF-8 scrctl --help
scrctl --lang en --help
scrctl --lang zh-CN --stats
```

中文目录缺失时显示英文。安装会同时安装 `share/locale/zh_CN/LC_MESSAGES/scrctl.mo`；
保留 `bin` 与 `share` 的相对目录后可以移动安装目录。开发构建直接读取构建目录的翻译，
也可用 `SCRCTL_LOCALEDIR` 指定目录根。
项目自己的帮助、状态和错误提示已接入翻译；设备响应、系统错误与 CLI11 参数校验正文保留其原文。
Linux/glibc 需要至少安装一套中文或英文 UTF-8 locale 才能加载 gettext 中文翻译；
只有 C / C.UTF-8 时回退英文。macOS 可搬移安装包及检查方法见 [macOS 说明](docs/MACOS.md)。
十个常用诊断工具也已接入语言选择与消息目录：`screenshot_probe`、`rr_keepalive_probe`、
`wifi_probe`、`stall_probe`、`audio_pump_probe`、`audio_decode_probe`、`feature_probe`、
`applist_probe`、`app_launch_probe` 和 `hid_probe`。归档实验的独立输出仍在整理。

修改用户提示时，英文消息使用 `SCRCTL_TR`（延后翻译的常量使用 `SCRCTL_N_`）。
运行 `python3 tools/update_translations.py` 更新模板与中文目录，补齐 `po/zh_CN.po`
后再运行 `python3 tools/update_translations.py --check`。该检查使用 gettext 的
`msgcmp` / `msgfmt`，会发现源消息未同步、缺失译文或 printf 占位符错误；离线测试也会执行。

## 构建

```bash
brew install sdl2 openssl ffmpeg gettext  # ffmpeg 提供软件解码，gettext 提供翻译支持
cmake -B build-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake -j
ctest --test-dir build-cmake              # 离线自检，不需要真机（项数用 `ctest -N` 数）
./build-cmake/scrctl --help
```

构建还需要 GNU gettext 工具（`msgfmt`）及消息运行库（Linux glibc 通常已内置；
macOS 使用 gettext 的 libintl）。开启离线测试需要 Python 3。Debian/Ubuntu 可安装
`gettext python3 libsdl2-dev libssl-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev`。

JSON、命令行和 XML plist 解析分别使用 nlohmann/json（>= 3.12.0）、CLI11（>= 2.5.0）和 pugixml（>= 1.16），许可依次为 MIT、BSD-3-Clause、MIT。
CMake 优先找系统包，缺失时下载固定版本并校验 SHA256；首次配置需要网络。
离线构建可以安装这三个库，或通过
`FETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON` / `FETCHCONTENT_SOURCE_DIR_CLI11` /
`FETCHCONTENT_SOURCE_DIR_PUGIXML` 指定已有源码目录。
隧道内的 IPv6 / TCP / UDP 使用 lwIP 2.2.1（BSD-3-Clause），固定源码构建以保证配置一致。
离线时还需用 `FETCHCONTENT_SOURCE_DIR_LWIP` 指定该版本源码。
mDNS 使用 mjansson/mdns 1.4.3（Unlicense）；离线时设置 `FETCHCONTENT_SOURCE_DIR_MDNS`。
`-DSCRCTL_FETCH_DEPENDENCIES=OFF` 禁止下载缺失依赖。
该选项仍允许使用上面指定的本地源码目录；目录无效时配置失败，不回退下载。
第三方许可证位于对应依赖源码中，分发时应保留其许可声明。

应用模块的职责与后续替换边界见 [重构说明](docs/REFACTOR.md)。
lwIP 的生产接入与默认关闭的独立探针，构建方式与判据见 [lwIP 验证记录](docs/LWIP_COMPATIBILITY.md)。

FFmpeg 软件解码是默认的视频后端，`--hw-decode` 选择平台硬件后端。
`-DSCRCTL_LIBAV=OFF` 可关闭软件解码。当前 VideoToolbox 适配使用 2 字节 NAL 长度
前缀，不能处理超过 65535 字节的 NAL；已有真机记录包含超过这一上限的帧，因此
通常应保留软件后端。非 Apple 平台目前只有软件视频后端。

默认不构建开发探针。离线测试可执行文件位于 `build-cmake/tests/`，主程序仍位于
构建目录根部。只需要主程序时加 `-DSCRCTL_BUILD_TESTS=OFF`；开发诊断用
`-DSCRCTL_BUILD_PROBES=ON`，历史协议实验用 `-DSCRCTL_BUILD_EXPERIMENTS=ON`。
分组、输出目录及运行方法见 [工具说明](docs/TOOLS.md)。

Windows ARM64 的构建、DLL 安装、AMDS 要求与验证范围见 [Windows 说明](docs/WINDOWS.md)。

## 首次连接设备

USB 连接通常需要以下设备准备：

1. 开启开发者模式，按设备提示重启。
2. 插线后在设备上信任这台电脑。
3. 挂载与系统版本匹配的个性化开发者磁盘镜像（DDI）。可用 Xcode 的设备管理界面，
   或研究环境中的 `pymobiledevice3 mounter auto-mount`；scrctl 当前不负责挂载。

DDI 会影响 `com.apple.coredevice.*` 服务是否出现在目录中。服务已出现也不保证某个
feature 可用：已有 iPadOS 18.7.8 记录中，设备拒绝实时媒体流并要求 iOS 27.0，
但截图和 HID 可用。应用遇到对应版本拒绝时可切换截图轮询，也可使用
`--video-source screenshot` 强制选择；截图模式不录制 Annex-B，也不启动音频。
该设备的验证过程见协议记录第 23、24 节。

Wi-Fi 需要设备可达和已有 RemotePairing 记录。使用 `scrctl --pair -s <UDID>`
经 USB 建立或验证记录；手机可能要求确认。旧记录缺少设备身份或被拒绝时，
使用 `scrctl --pair --repair-pairing -s <UDID>` 显式重配。
新记录通过独立连接验证后才保存，已有有效记录直接复用。验证边界见
[路线图](docs/ROADMAP.md)。
记录现在需要包含 USB 配对时校验并保存的设备标识和长期公钥；旧记录缺少这些字段时，
请重新通过 USB 配对。探针的 `--pmd3-record` 当前只导入主机密钥，因此也不能直接用于
设备身份验证。通过 Wi-Fi 地址进行首次配对仅供实验，需显式指定 `--no-save`。

```bash
scrctl --list-devices
scrctl --list-devices --discovery-timeout=0  # 只列 usbmux 设备
scrctl --stats
scrctl --wifi 192.168.1.50 --stats
scrctl --video-source screenshot
scrctl --help
```

`--list-devices` 合并 usbmux 与 RemotePairing 的发现结果，默认扫描无线服务约 3 秒；
`--discovery-timeout` 可以设置 0..60000 毫秒。每台设备保留所有地址、实际服务端口和
网络接口；IPv6 link-local 地址包含 `%接口索引`。USB 网络地址也可能出现在发现结果中，
列出地址不代表已经验证其可达性。

无线广播的标识与 UDID 不同。只有 `authTag` 与一份本地配对记录唯一匹配，才将无线
候选归到该记录的 UDID；未知设备、旧记录线索和匹配冲突分别显示状态。
“配对记录可用”表示本地材料完整，连接时仍需通过 PairVerify 验证设备身份。
扫描不会配对或启动媒体会话；多播被网络阻止时可以继续使用手动 `--wifi` 地址。
默认先选择 usbmux 中匹配的设备；没有候选时再发现已配对的无线设备。
`--wifi auto -s <UDID>` 只走无线发现；`--wifi <地址> -s <UDID>` 跳过发现，
可用 `--wifi-port` 指定端口。未指定 `-s` 时只接受唯一可用设备，不任取第一台。
设备已出现在 usbmux 中但连接失败时，保留原错误，不悄悄改连无线会话。

核心接口为 `remote::discover_devices()`（`remote/Discovery.h`），返回设备、连接候选、
来源可用状态和诊断；调用方通过 `should_cancel` 回调取消，保留已经发现的快照。
该回调可读取原子标志，或捕获工具链支持的 `std::stop_token`，不要求系统 C++ 库提供后者。

## 剪贴板

`--copy TEXT` 请求写入设备剪贴板，`--paste` 读取设备剪贴板并输出文本。
这些命令完成后退出，可用于传递中文和 emoji；它们不会模拟设备上的粘贴动作。
同时指定两者时，先写入再读回，便于检查设备实际保存的内容。

```bash
scrctl --copy "中文🙂"
scrctl --paste
scrctl --copy "" --paste
```

空文本应作为独立参数传入，例如 `--copy ""`；`--copy=` 按缺少参数处理。
存在但长度为零的文本表示会成功读回；只有图片或没有文本表示时给出错误原因。
输出按文本字节长度写入，包含内嵌零字节，终端未必能直接显示这类字符。

## 研究工具

10 个常用诊断工具保留在 `tools/`，19 个协议实验归档到 `tools/experiments/`。
它们默认不构建，不随产品安装；常用工具输出到构建目录的 `tools/`，归档实验输出到
`experiments/`。其中 lwIP / nghttp2 的库适配实验通过各自独立开关启用。
具体用途和使用边界见 [工具说明](docs/TOOLS.md)。
`tools/nalsizes.py` 离线分析录制文件的 NAL 尺寸，`tools/probe/` 保留早期 Python 验证工具。

Python 探针使用第三方 pymobiledevice3，仅用于研究，不随 scrctl 分发或作为运行依赖。
可在仓库根目录建立单独环境：

```bash
python3 -m venv .probe-venv
.probe-venv/bin/pip install pymobiledevice3 pillow numpy av
tools/probe/probe.sh
```

REPL 支持 `shot` / `tap` / `swipe` / `draw` / `center` / `home` / `volup` 等命令。
设备选择顺序为 `--udid`、`SCRCTL_UDID`、唯一 USB 设备。

## License

Apache-2.0
