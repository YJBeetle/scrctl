# scrctl

> scrcpy for iOS —— 原生窗口的 iPhone / iPad / Apple TV / Vision 镜像与操控工具。

命名沿用 `scr*` 前缀以复用 scrcpy 已建立的认知，CLI 人体工学也照 scrcpy 设计。

离线自检由 GitHub Actions 跑（macOS / Ubuntu 各一格，外加 ASan+UBSan 一格），见
`.github/workflows/offline-selftest.yml`。**真机那部分不在 CI 里**：一台设备同时只容得
下一条流，起流、注入、保活这些只能插着手机手工跑，判据都写在 `docs/` 里。

## 它解决什么

苹果生态里已有的两条路都不够：

| | 问题 |
| --- | --- |
| **iPhone Mirroring** | 独占会话——手机必须保持锁定，无法边镜像边用 |
| **QuickTime 投屏** | 只能看，不能操作 |
| **Xcode 27 DeviceHub** | 非独占且可操控，但它是 Xcode 的 GUI 组件，没有 CLI、不能脚本化、无法作为库被链接 |

scrctl 走 DeviceHub 底下的那条路（CoreDevice DDI 开发者服务），但做成**单个原生二进制 + 原生窗口 + scrcpy 式 CLI**，并且把核心暴露成 C ABI 供 MaaFramework 直接链接。

关键差异：**镜像的同时手机可以继续拿在手里用。**

## 已验证

不越狱、不装 WebDriverAgent、不要 root、不依赖 Xcode GUI，iPhone 14,4 / iOS 27.0 / USB 实测：

- 屏幕流 **58.9 – 61.1 fps**（面板 60Hz）。解码**默认软件后端（libavcodec）**，
  `--hw-decode` 才用 VideoToolbox：硬解只吃 2 字节的 NAL 长度前缀，而这条流单帧
  实测能到 256278 字节（转场动画里的一个 P 帧），硬解遇到就得丢帧重起。
  同一份录屏两后端都 412/412 出帧、同帧逐像素平均绝对差 0.92；同一 600 帧
  CPU 6.7 秒 vs 1.0 秒，但墙钟几乎一样（24.7s / 26.1s）——瓶颈是包到达不是解码
- 触摸注入成功，拖动轨迹平滑连续，坐标映射准确（窗口任意缩放、任意裁剪下都对）
- 硬件按键（home / 锁屏 / 音量）、ASCII 键盘输入、组合键、剪贴板读写
- 断流自愈：序号缺口、完全静默、关键帧过大都会走对应的恢复路径
- **音频**：设备系统输出（PT=101 上的裸 AAC-ELD，一包一帧）解成交织 s16 交给 SDL
  出声，**本机喇叭听过：正常**。真机 28 秒读数：100 包/秒、解码失败 0、序号真丢 0、
  重起 0，内容静音的那段也照发包（所以"没包"就是死讯）。注意**手机上的音量键管不着
  这条流**：设备给的 `audioSystemOutput` 是音量之前的抽头（按到 0 之后镜像里的样本峰值
  照旧），要在电脑上静音请用 `--no-audio-playback` 或本机输出音量。**只在 macOS 上有**
  ——设备给的是苹果专有的 AAC-ELD，别处没有能解它的自由实现，Linux/Windows 上会明说
  "这一路是缺的"而不是默默没声。判据与四路后端对照见 docs §17
- **转屏**：画面按设备报的界面旋转自动转正，窗口跟着换向，触摸按同一个角度逆映射
  回去。这条必须客户端做——设备编码出来的帧**永远不跟着转**（界面 rot270 时码流
  仍是 1136x2464 竖幅），协议里没有任何地方替我们转正。真机判据见 docs §16.1

完整调研记录、设备侧服务清单、以及踩过的坑见 **[docs/coredevice.md](docs/coredevice.md)**。

## 技术栈

```
src/transport/   usbmux（macOS/Linux 走 usbmuxd 的 Unix 套接字）+ lockdown TLS + CDTunnel
src/net/         用户态 IPv6/TCP/UDP 栈（隧道是用户态数据报，没有内核网卡可用）
src/http2/       隧道上的 HTTP/2（不需要 HPACK：对端只发定长头表）
src/xpc/ src/remote/  RemoteXPC 编解码、RSD 服务目录、CoreDevice feature RPC
src/media/       起流协商（offer 是 bplist+zlib+protobuf 三层）、收包泵与自愈
src/rt/          RTP 拆包（RFC 7798 HEVC，聚合包与分片包）
src/bitstream/   Annex-B 与 NAL 组 AU
src/decode/      VideoToolbox 硬解（Apple）+ libavcodec 软解，接口统一
src/hid/         HID 报告构造：触摸屏面 257、键盘面 512、硬件按键
src/app/         C++ + SDL2 —— 原生窗口、鼠标→触摸、按键映射、CLI
```

跨平台是硬要求，因此**不能**只依赖 macOS 的 `remotepairingd` 捷径（虽然那条路最省事：无 TUN、无 QUIC、无用户态栈，且能与 Xcode 共存）。它作为 macOS 上的可选快速路径保留。

给 MaaFramework 的复用目前是**直接链接 `scrctl_core` 静态库、用 C++ 类**（`MaaIOSControlUnit/Session/ScrctlSession`），还没有包一层稳定的 C ABI。

## 构建

```bash
brew install sdl2 openssl ffmpeg          # ffmpeg 提供软解后端，见下
cmake -B build-cmake -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake -j
ctest --test-dir build-cmake              # 离线自检，不需要真机（项数用 `ctest -N` 数）
./build-cmake/scrctl --help
```

ffmpeg 是**默认解码后端**，`-DSCRCTL_LIBAV=OFF` 可以关掉，但关掉之后会退回
VideoToolbox，而它只吃 2 字节的 NAL 长度前缀（上限 65535）——真机主屏的关键帧实测
49652~70101 字节、动画里的 P 帧 256278 字节，那种帧只能整帧丢弃并重起会话。非 Apple
平台没有 VideoToolbox，软解就是唯一后端。

## 首次连接一台设备

三件事，缺一不可，全是设备侧的开关：

1. **开发者模式**：设置 → 隐私与安全性 → 开发者模式，打开后重启一次。
2. **信任本机**：插上之后在设备上点「信任」。
3. **挂 DDI（个性化开发者磁盘镜像）**：iOS 17+ 上镜像 / 截图 / 注入用的那一批
   `com.apple.coredevice.*` 服务，只在 DDI 挂上之后才出现在设备目录里。挂法二选一：
   用 Xcode 连一次这台设备（Window → Devices and Simulators），或命令行
   `pymobiledevice3 mounter auto-mount`（按设备的 build 从 Apple 下载并 personalize 一份）。
   `pymobiledevice3 mounter list` 输出为空就是没挂。**这件事可逆**：重启即卸，也能
   `mounter umount-personalized` 主动卸。scrctl 不替你做这一步——没挂时它会在报错里直说，
   并打出设备自己给的目录里 `com.apple.coredevice.*` 这一族有几条（0 条 = 没挂）。

**支持范围是真机量出来的，别按服务名在不在猜**：镜像与音频这条媒体流要求 **iOS 27+**。
iPadOS 18.7.8 上 `displayservice` 和它的 feature 列表都在目录里，但 `startmediastream` 被设备
按版本拒（code 9021，设备原话 "Remote control requires iOS 27.0 or later on this device"），
`getmediasupportinfo` 回 `supportedFeatures: 0`（iOS 27 的 iPhone 回 972）。同一台 iPadOS 18 上：
**截图服务可用**（1536x2048 PNG 实测）、**HID 按键可用**（息屏时按 home 能把屏幕唤醒）、
**触摸注入落地**（无边记画布上连画两笔，区域差分 3686 / 9542 像素）。也就是说 iOS 18 上缺的只是
实时视频流与音频，"看 + 操控"都还在。判据与过程（含一轮作废的错判及其原因）见 docs §23。
iOS 18 上 scrctl 会**自动降到截图轮询兜底**（`--video-source=screenshot` 可强制）：画面 2~3 fps、
注入即时，`--record` 在这条路忽略；窗口里的真机手感还欠一次开窗口的会话。判据与两个实现约束
（截图服务一条连接只服务一次、PNG 像素格式随画面内容变）见 docs §24。

## 状态

镜像 + 操控（触摸 / 按键 / 键盘 / 剪贴板）已在真机跑通，MaaFramework 的 iOS 控制单元
已在真机上 dlopen 验证。CI 已有（`.github/workflows/offline-selftest.yml`：macOS 与
Ubuntu 各一格跑 `ctest`，另有一格 ASan+UBSan），但它只覆盖不需要真机的那部分——真机
回归目前仍然只能人工跑。

**平台现状要说清楚，别按"跨平台"这个词理解**：

| | 现状 |
| --- | --- |
| macOS | 真机验证过的那条路（本仓库所有数字都是它给的） |
| Linux | **构建与离线自检在 CI 的 Ubuntu 格上真跑过**（配置、链接、`scrctl --help` 起得来、15 项 ctest 全绿，2026-09-27 起这一格是门槛不是探针）。但这只覆盖"不需要手机的那一半"：libusb、DDI 挂载、隧道建立**没有一次对着真机跑过**，写这些代码的机器上没有 Linux。音频那一路在 Linux 上**没有可解的后端**（见上） |
| Windows | 只有计划。`transport/Usbmux.cpp` 用的是 POSIX 套接字，要走 AMDS 得先换掉那一层，现在连编译都过不去 |

剩下的主要是工程化收尾：Linux 那一半要有真机才能判（CI 只覆盖不需要手机的部分）、
`--record` 带上音轨（视频现在是裸 Annex-B，加音轨要先有容器）、以及 Windows 那一层。

## 探针工具（研究用）

`tools/*.cpp` 是需要真机在线的手工探针（起流、HID、feature schema、剪贴板各一个），
不进 ctest；`tools/probe/` 是立项前的 Python 验证脚手架，保留作为机制的可执行文档。

`tools/nalsizes.py` 离线分析录制文件里的 NAL 尺寸分布——"关键帧有多大"这件事只能量
出来，不能猜。

Python 侧依赖 **pymobiledevice3**（GPL-3.0-or-later，第三方项目）—— 仅用于研究验证，**不随 scrctl 分发、不是 scrctl 的运行时依赖**，需自行安装：

```bash
python3 -m venv .probe-venv
.probe-venv/bin/pip install pymobiledevice3 pillow numpy
cd scrctl/tools/probe && ./probe.sh
```

REPL 支持 `shot` / `tap` / `swipe` / `draw` / `center` / `home` / `volup` 等命令。设备标识不硬编码，按 `--udid` > `SCRCTL_UDID` > 唯一 USB 设备自动发现。

## License

Apache-2.0
