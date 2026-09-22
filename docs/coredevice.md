# CoreDevice / DDI 机制调研

scrctl 立项前的验证记录。目标：确认能否在**不越狱、不装 WDA、不要 root、不依赖 Xcode GUI** 的前提下，从 Mac 对 iPhone 做屏幕采集 + 触摸注入。

结论：**可以，且已端到端跑通。**

- 实测帧率 **58.9 – 61.1 fps**（面板 60Hz）
- 触摸注入成功，鼠标拖动在画图 App 中留下平滑连续笔画，坐标映射准确
- 音频、硬件按键、键盘输入、剪贴板在参考实现中均已具备

---

## 1. 服务从哪来：Developer Disk Image

Xcode 附带四份 DDI，说明该机制覆盖全平台：

```
/Library/Developer/CoreDevice/CandidateDDIs/
  iOS_DDI.dmg  tvOS_DDI.dmg  watchOS_DDI.dmg  xrOS_DDI.dmg
```

`iOS_DDI.dmg` 内层 `Restore/022-*.dmg` 是设备侧 rootfs，`Library/LaunchDaemons/` 列出它向 iPhone 安装的守护进程。每个都用 launchd `RemoteServices` + RemoteXPC 暴露：

| 守护进程 | RemoteService | Features | 设备侧要求 entitlement |
| --- | --- | --- | --- |
| `dtuhidd` | `com.apple.coredevice.hid.indigo` | `remote.hid.button` / `.scroll` / **`.digitizer`** / `.vendordefined` | `com.apple.private.CoreDevice.hid` |
| | `...hid.universalhid` | `remote.hid.keyboard` | 同上 |
| | `...hid.universalhidservice` | `remote.universalhidservice` | 同上 |
| `dtremotedisplayd` | `com.apple.coredevice.displayservice` | `getmediasupportinfo` / `getmediastreamserverstatus` / `startaudiooutput` / `startvideooutput` / `startmediastream` / `stopmediastream` | `...canViewDeviceDisplay` |
| `dtscreencaptured` | `com.apple.coredevice.screencaptureservice` | `capturescreenshot` | `...canViewDeviceDisplay` |
| `dthidd` | `com.apple.coredevice.devicecontrol` | 仅 `devicecontrol.orientation` | `...devicecontrol` |
| `dtappserviced` | `com.apple.coredevice.appservice` | 启停 App | |
| 其余 | `dtpasteboardd` `dtlocationd` `dticond` `dtdeviceinfod` `dtdiagnosticsd` `dtfileserviced` `dtfilesandboxd` `dtdebugproxyd` `dtconfigurationd` `testmanagerd` `gputoolstransportd` | | |

> ⚠️ **命名陷阱**：`dthidd` 名字最像触摸注入，实际只管屏幕方向。真正的注入者是 **`dtuhidd`**（"indigo" 是其代号）。

DDI `version.plist` 显示构建来源含 `XCTest`、`CoreDevice`、`Mercury`、`GPUToolsDevice_DDI`；设备侧还带 `Mercury.framework`、`CoreDeviceMediaStreamSupport.framework`、`DTRemoteServices.framework`。

## 2. 权限链：第三方为何可达

- Mac 侧 `CoreDeviceService.xpc` 持有全部特权（`com.apple.private.CoreDevice.hid` / `.canViewDeviceDisplay` / `.devicecontrol` 等）
- CoreDevice 客户端侧**只对 sysdiagnose 做 entitlement 校验**
- 设备侧 `RequireEntitlement` 是对**隧道对端**校验，而隧道由特权方建立
- **实测**：无 root、无 entitlement 的进程能完整读到设备广播的 RSD 服务表（85 个服务），并成功握手 `displayservice` / `screencaptureservice` / `hid.universalhidservice`

即：设备 RSD 表里的 `Entitlement` 字段只是声明，握手时不校验第三方对端。

## 3. 隧道：macOS 上可以完全不做

`CoreDeviceTunnelProxy` 走 `com.apple.internal.devicecompute.CoreDeviceProxy` lockdown 服务，但隧道承载的是 **IP 数据包**，需要 TUN 设备（免 root 的唯一办法是自己实现用户态 IP/TCP 栈）。QUIC 只在 RemotePairing / Wi-Fi / iOS<17.4 路径才需要。

**macOS 原生路径可以彻底绕开这些**：向系统守护进程 `com.apple.CoreDevice.remotepairingd`（`RemotePairing.framework`，stock macOS 自带、无需 Xcode）借用它已建立的隧道：

1. browse `remotepairingd`，拿到该设备的 per-device XPC endpoint
2. `RemotePairing.CreateAssertionCommand` → 得到设备在隧道内的 `tunnelIPAddress` + 维持隧道的 assertion id
3. 一次 `nettop` 采样找出 `remoted` 到该地址的 TCP 连接 → RSD 端口
4. **普通 TCP socket** 连 `[tunnelIPAddress]:rsdPort`（隧道地址内核可路由，不需 root）
5. 跑标准 RSD 握手

无 root、无 entitlement、无 Xcode，且**不 suspend `remoted`，因此与 Xcode / `devicectl` 共存**。整个 XPC 对话可直接用 `libxpc` C API 完成。

代价：该路径 macOS-only。Linux/Windows 需另做 QUIC + 用户态栈（QUIC 有 quiche / msquic / quinn 可用；真正的成本在 IP/TCP 栈）。

## 4. 信任前置条件（四道门）

1. **lockdown 配对** —— 「信任此电脑」，一次性。配对记录：macOS `/var/db/lockdown/`、Linux `/var/lib/lockdown/`、Windows `%ALLUSERSPROFILE%\Apple\Lockdown\`
2. **开发者模式** —— 设置 → 隐私与安全性 → 开发者模式
3. **DDI 已挂载** —— DeviceKit 的 provider 协商里 `ddi` 是布尔门控
4. iOS 27 新增可选：**设备发起配对**

## 5. 触摸注入要点

- **必须先有一条运行中的视频流。** 否则 backboardd 把 HID surface 发布为 `externalAccessory` 并**静默丢弃所有 report**。起流后需等约 `0.3s` 让 backboardd 把 surface 匹配到这条新授权的流。
- **坐标是归一化 UInt16 `0..65535`，不是像素。** `x_norm = x_px * 65535 / screen_w`
- HID surface ID：`257` = mainTouchscreen（58 字节，report id `0x09`，真触摸）；`1281` = touchscreenGesture（19 字节 `0x13`，仅光标）
- `CONTACT = 0xC2` / `RELEASE = 0x02`；点击需真实按住时长，微秒级 tap 会被 iOS 判为侧键长按
- 视频流与注入可共用同一个 session，一个连接同时服务两者

## 6. 实测踩到的坑

1. **编码分辨率 ≠ 逻辑显示尺寸**。视频是 `1136x2464`，`get-display-info` 报 `1125x2436`（HEVC CTU 对齐填充）。需裁掉右 11px / 底 28px；**触摸归一化必须用显示尺寸**，用视频尺寸会在屏幕底部累积约 28px 偏差。
2. **裸 Annex-B 直接喂播放器不可行**。设备默认不周期发 IDR，一旦丢包导致参考链断裂，`Could not find ref with POC N` → `Skipping invalid undecodable NALU: 1` 会永久不可解，必须重新取得关键帧。
3. **`ffplay -f hevc` 的 `-framerate` 默认 25**，而真实流是 60fps。不设会导致播放器按 40ms/帧消费、按 60 帧/秒接收，队列无界堆积（表现为"延迟 30 秒"且 CPU 仅 0.7%）。
4. **offer 字符串影响巨大**。参考实现的调研记录显示：协商带 `VRAE:0`（禁止编码器自适应分辨率）时，在固定 6 Mbps 上限下编码器靠**丢输入帧**控码率，实测丢 207–219 帧、仅 ~42fps 且有多帧马赛克拖影；去掉该 token 后丢帧为 0、53–55fps。**注意苹果自己 Xcode 抓包里的 offer 恰恰是带 `VRAE:0` 的那个慢版本。**
5. **teardown 顺序敏感**：停止流必须用**全新的 RemoteXPC 连接**发 stop，复用发起 start 的那条连接会让设备守护进程在释放 session 前崩溃。

## 7. 与 iPhone Mirroring / DeviceHub 的关系

- **iPhone Mirroring**（`com.apple.ScreenContinuity`）是 Continuity 用户态会话，**独占**——手机必须保持锁定，无法边镜像边用手机。
- **Xcode 27 DeviceHub**（`com.apple.dt.Devices`）走上述 DDI 开发者服务，是**并行**通道，手机可同时操作。这是 scrctl 选择后者的根本理由。
- DeviceHub 自身用可插拔 provider 优先级协商：帧缓冲三通道（`AVConference` + `AVCVirtualExternalDisplay` / legacy VNC / 内置兜底），HID 走**独立** provider。门控条件含 `bootState`、`ddi`、`reportsMainDisplay`、`videoOutputByDisplayID`、`hasReportedDisplays`、`connectionState`、`interactionRequests`。
- 设备会主动广播 DeviceKit 的 chrome 遮罩资源（`com.apple.dt.devicekit.chrome.phone3`），用于圆角/刘海合成。

## 8. 参考实现

`pymobiledevice3`（**GPL-3.0-or-later，第三方项目，不随 scrctl 分发**）已完整实现上述协议栈，包括 `remote/core_device/` 下的 RTP 解包、手写 HEVC RPS 跟踪、VNC 服务器、WebCodecs 推流、HID 报告构造，以及 `misc/RemoteXPC.md`、`misc/understanding_idevice_protocol_layers.md`、`misc/remotexpc_sniffer.py` 等协议文档与抓包工具。

scrctl 的存在理由是它的**形态**而非能力：原生窗口 + scrcpy 式 CLI + 单二进制 + 供 MaaFramework 链接的 C ABI。协议从可观测行为重新实现，不受版权传染。

## 9. 探针工具

见 `tools/probe/`。需要自行 `pip install pymobiledevice3 pillow`（探针为研究用途，非 scrctl 运行时依赖）。

```bash
./probe.sh                    # 交互 REPL：截图 / 点击 / 拖动 / 按键
../../../.probe-venv/bin/python latency.py   # 帧率与输入->画面延迟
../../../.probe-venv/bin/python stream.py | ffplay -f hevc -framerate 60 \
    -probesize 32 -analyzeduration 0 pipe:0
```

设备标识不硬编码，按 `--udid` > `SCRCTL_UDID` > 唯一 USB 设备自动发现 的顺序解析。
