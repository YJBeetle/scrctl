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

不越狱、不装 WebDriverAgent、不要 root、不依赖 Xcode GUI，实测：

- 屏幕流 **58.9 – 61.1 fps**（面板 60Hz）
- 触摸注入成功，拖动轨迹平滑连续，坐标映射准确
- 音频、硬件按键、键盘输入、剪贴板在协议层均已具备

完整调研记录、设备侧服务清单、以及 5 个必须避开的坑见 **[docs/coredevice.md](docs/coredevice.md)**。

## 技术栈

```
core/            C++20 —— RSD 握手、RemoteXPC、XPC 序列化、
                 RTP/HEVC 解包与关键帧管理、HID 报告构造
transport/       usbmux（Linux usbmuxd / Windows AMDS / macOS）
                 用户态 IP/TCP 栈: lwIP      QUIC(PSK): quiche
platform/macos/  .mm —— VideoToolbox 硬解（可选加速；跨平台兜底走 libav）
app/             C++ + SDL2 —— 原生窗口、鼠标→触摸、键盘映射、CLI
include/scrctl/  C ABI —— 供 MaaFramework 的 iOS ControlUnit 复用
```

跨平台是硬要求，因此**不能**只依赖 macOS 的 `remotepairingd` 捷径（虽然那条路最省事：无 TUN、无 QUIC、无用户态栈，且能与 Xcode 共存）。它作为 macOS 上的可选快速路径保留。

## 状态

协议机制已端到端验证完毕，实现尚未开始。

## 探针工具（研究用）

`tools/probe/` 是立项前的验证脚手架，保留作为机制的可执行文档。

依赖 **pymobiledevice3**（GPL-3.0-or-later，第三方项目）—— 仅用于研究验证，**不随 scrctl 分发、不是 scrctl 的运行时依赖**，需自行安装：

```bash
python3 -m venv .probe-venv
.probe-venv/bin/pip install pymobiledevice3 pillow numpy
cd scrctl/tools/probe && ./probe.sh
```

REPL 支持 `shot` / `tap` / `swipe` / `draw` / `center` / `home` / `volup` 等命令。设备标识不硬编码，按 `--udid` > `SCRCTL_UDID` > 唯一 USB 设备自动发现。

## License

Apache-2.0
