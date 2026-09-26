# scrctl

> scrcpy for iOS —— 原生窗口的 iPhone / iPad / Apple TV / Vision 镜像与操控工具。

命名沿用 `scr*` 前缀以复用 scrcpy 已建立的认知，CLI 人体工学也照 scrcpy 设计。

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

完整调研记录、设备侧服务清单、以及踩过的坑见 **[docs/coredevice.md](docs/coredevice.md)**。

## 技术栈

```
src/transport/   usbmux（macOS/Linux 走 usbmuxd，Windows 走 AMDS）+ lockdown TLS + CDTunnel
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

## 状态

镜像 + 操控（触摸 / 按键 / 键盘 / 剪贴板）已在真机跑通，MaaFramework 的 iOS 控制单元
已在真机上 dlopen 验证。剩下的主要是工程化：CI、非 Apple 平台的实机验证、以及
`start_app` / `stop_app` 这类设备侧能力。

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
