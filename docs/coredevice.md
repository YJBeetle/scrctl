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
6. **XPC 消息必须套在 HTTP/2 DATA 帧里写。** 少一个 9 字节帧头，设备就把 wrapper magic `0x29B00B92` 当帧头解析（长度读成 9572528、类型读成未定义的 `0x29`），回一个 GOAWAY `"too large frame size"` —— 错误信息指不到「自己帧头没写」这种错因上。
7. **主通道终止帧的标志位是 `0x200`，不是 `IS_REPLY(0x20000)`。** 线上 flags = `0x0201`。写成 `0x20000` 的症状是设备把回信通道 `RST_STREAM / FRAME_SIZE_ERROR` 拆掉，`peer_info` 永远不来。
8. **「有字典载荷」不等于回信。** 设备对我们每个握手帧各回一个空字典 `{}` 或空载荷帧当 ACK，只筛「是字典」就会把第一个 `{}` 当 `peer_info` 交上去，调用方看到一个没有 `Services` 的对象却以为握手成功了。必须要求**非空**字典。

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

## 10. 隧道内 RemoteXPC 的线上细节（实测，iPhone 14,4 / iOS 27.0 / USB）

RSD 控制通道 = 隧道内 TCP 上的 HTTP/2，而 XPC 消息装在 DATA 帧里。

**帧序列**（设备侧 RemoteServiceDiscovery 会校验顺序，主通道 HEADERS 必须先于
终止帧、回信通道 HEADERS 必须先于它的 INIT_HANDSHAKE 帧）：

```text
PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n          前置签名，24 字节裸串，不是帧
SETTINGS      {MAX_CONCURRENT_STREAMS=100, INITIAL_WINDOW_SIZE=16MiB}
WINDOW_UPDATE stream=0  +16MiB-65535
HEADERS       stream=1  flags=END_HEADERS  len=0     ← 空 header block
DATA          stream=1  flags=0x001        空字典 {}  44 字节
HEADERS       stream=3  flags=END_HEADERS  len=0
DATA          stream=1  flags=0x0201       空载荷      24 字节   ← 终止帧
DATA          stream=3  flags=0x400001     空载荷      24 字节   ← INIT_HANDSHAKE
（读对端 SETTINGS 后）SETTINGS ACK
DATA          stream=1  flags=0x0101       Handshake 请求，见下
```

**HPACK 完全不需要**：HEADERS 是空的，路由信息全在流号里（1 主通道 / 3 回信通道），
载荷走 DATA。这是这条链路上最值得的一次减法。

**Handshake 请求载荷**（不带这一手，设备回 `"Invalid or missing remote device
connection version flags"` 并取消连接）：

```text
MessageType                = "Handshake"
MessagingProtocolVersion   = UINT64 7
UUID                       = UUID(16 字节，必须稳定，见下)
Properties.RemoteXPCVersionFlags = UINT64 0x0100000000000006
Properties.SensitivePropertiesVisible = true   ← 不给则带 entitlement 的服务被摘掉
Services                   = {} (空字典)
```

**peer_info 的形态**：一次性回来，实测 25 KB、拆成 16374 + 8470 两个 DATA 帧，
所以 XPC 消息层必须能跨帧重拼。顶层键 `MessageType / MessagingProtocolVersion /
Services / Properties / UUID`；`Properties` 是设备指纹（型号、OS 版本、序列号、
MAC…），`Services` 是 85 项目录，每项形如

```text
com.apple.coredevice.displayservice: {
    Properties: {EncryptSocketData: false, Features: [...], UsesRemoteXPC: true},
    Port: "63xxx",            ← 注意是**字符串**不是整数
    Entitlement: "...", ServiceVersion: 1 }
```

`Port` 是隧道内端口，直接对我们自己那个 IPv6/TCP 栈 `connect` 即可，不需要再经
lockdown `StartService`。`UsesRemoteXPC` 决定连上之后是再走一遍 HTTP/2+XPC
握手还是直接当裸服务（lockdown shim 那批是 false）。

**回信判定规则**：设备对上面每个握手帧各回一个空字典或空载荷帧当 ACK，所以
「解出了一个字典」不等于拿到回信，必须要求**非空**字典，否则第一个 `{}` 会被
当成 peer_info 交上去。

**peer UUID 必须稳定**：设备每条隧道只保留一个 RSD 连接，新连接一来就换掉旧的；
iOS 27.2 起它还会记住被换掉那个 peer 的 UUID，来客 UUID 与记忆不符就把整台设备
重新 attach —— 已公布的服务监听全部关闭、端口拒绝连接。scrctl 取配对记录里的
`HostID` 当这个 UUID（现成的、跨进程跨重启稳定的主机标识，不新增状态文件）。

**权限模型实测复核**：RSD 目录里每个服务都带 `Entitlement` 字段，但设备**不对
第三方对端强制校验**它——非 root、无苹果开发者授权签名的进程照样能连上
`displayservice` / `hid.*` 并起流。真正的门是 §4 那四道，不是这个字段。

## 11. 屏幕视频流的线上细节（实测，iPhone 14,4 / iOS 27.0 / USB）

`displayservice` 的 `startmediastream` 之后，设备把 RTP **反向推**到我们隧道地址上的
UDP 端口。整条链路上有六个地方不看真机字节就想不对，每一个都值一次记录。

**先绑端口，再发 RPC。** 设备一返回 answer 就开播，不存在"客户端准备好"的第二
回合。绑晚了丢的是开头几个包，而开头恰好是 VPS/SPS/PPS 和第一个关键帧——丢了
后面整段都解不出来。

**RTCP 与视频共用同一个 UDP 端口。** 实测 PT=72 的包就混在视频包里到达。不按
`connection.streamConfig.RxPayloadType`（实测 100）过滤，RTCP 就会被当成 HEVC
载荷解出类型 0 一类的假 NAL 混进码流——后果不是"这一帧花"，而是**永久**花，原因
见下条。

**这条流不周期发 IDR。** 一个假 NAL 或一个残缺分片就把参考链打断，之后所有帧都
在解同一个已经坏掉的参考，画面自己再也不会恢复。所以丢包处理只有一个正确姿势：
发现缺口就丢掉整个 AU，并想办法要一个关键帧（RTCP PLI，见 SCOPE 的 M2.7）。

**RTP 载荷头之后没有苹果私有子头。** 每个包是 X=1 的标准扩展头，profile
`0x9011`、长度 1 个 32 位字，一共 8 字节——就是这 8 字节被记成了"固定 8 字节的
苹果子头"，于是按扩展头长度跳一遍、又固定多跳 8 字节，每个 NAL 从中间开始，解出
的类型全是 63、92 这种不存在的值。载荷本身是 RFC 7798：type 48 聚合包（2 字节
长度）、type 49 分片（FU 头 S=0x80 E=0x40 TU=0x3F）、其余单一 NAL。

**分片头后面那 2 字节不是 DONL。** 它看起来极像 RFC 7798 里可选的 DONL（同一帧
里几个分片的值还相同），按规范跳过之后 NAL 结构完整、类型全对，但解码器一帧都不
出——每个分片都少了 2 字节真实码流。规范说"允许"不等于这条流里有。

**长度前缀样本里的 NAL 保留 emulation prevention 字节。** hvcC 与长度前缀样本
存的都是起始码之后那段**原样**字节（与 avcC 同一套规则）。去掉 00 00 03 之后
RBSP 里可能凭空出现 00 00 01，是否踩到取决于码流内容，所以出错是概率性的：同一
台机器、同一份 SPS，两份录屏一份"看着基本正常"、一份整片噪声。

**长度前缀是 2 字节，不是 4。** `CMVideoFormatDescriptionCreateFromHEVCParameterSets`
会正确把该参数写进 hvcC 的 `lengthSizeMinusOne`（2→1，4→3），但 `lengthSizeMinusOne=3`
的会话对每一个样本都回 `kVTVideoDecoderBadDataErr(-12909)`，出帧率 0%。已用
"自己 create + 自己 write"的 2×2 矩阵排除是组装写错。

**于是"单帧 > 65535 字节"是这条链路的硬风险，不是边角情况。** 主屏壁纸的一个 IDR
实测 49652~70101 字节（`tools/nalsizes.py` 量录制文件；无边记那种深色画面只有
17KB，所以早期结论"最大约 17KB"是被取样骗了）。VideoToolbox 喂不进去就只能整帧
丢，而流不周期发 IDR——开头那个丢了之后参考链永远起不来，症状是**连上几秒后一片
灰且永不恢复**。重起会话也救不了：内容不变，IDR 还是那么大。

**别指望 offer 能把单帧压小，这条路实测是死的：**
- 码率阶梯里像 bps 的 f2、像缓冲的 f3，各自缩到 0.25 再下发（以及两者同时缩），
  IDR 是 49652 / 49789 / 49852 字节，与不缩时的 49652 没有区别；
- 分辨率条目的 `pair_index` 从 0 扫到 6，编码尺寸一直是 1136x2464（`pair=1` 会被
  拒：code 32035 / GKVoiceChatServiceErrorDomain）。
设备不照 offer 里这几串数编，单帧多大由它自己定。所以唯一的解法是换一个没有 2 字节
限制的后端：`create_software_decoder()`（libavcodec），由泵在**关键帧**上切过去——
换后端等于换一条参考链，只有 IDR 能自洽起新链。同一份录屏两个后端都 412/412 出帧，
同帧逐像素平均绝对差 0.92；真机 --no-window 下软解 44.7fps、VideoToolbox 42.2fps。

**VideoToolbox 的回调可能在 `DecodeFrame` 返回之后才跑**，即使 decodeFlags 没开
异步位。所以输出槽必须活得比回调久（等 `WaitForAsynchronousFrames` 再让它离开作用
域），否则下一次提交以同样的调用深度进来、复用同一个栈地址，上一帧的图像就写进了
这一帧的槽。症状与上面那个 EPB bug 一模一样，但跟调用栈深度相关，更难抓。

**收包必须在独立线程上。** 渲染一帧要几十毫秒，这期间不把隧道读干净，设备侧中继
缓冲溢出后是**静默丢包**——丢在编号之前，所以序号看着仍然连续，我们收到的是一个
"完整"但内容被截断的关键帧：画面顶部对、下面全糊。单线程版录出来的流第 2 帧就是
这个形状。

**编码分辨率比逻辑显示大。** iPhone 13 mini 实测编码 1136x2464、逻辑显示
1125x2436，右侧 11px 与底部 28px 是 CTU 对齐的填充垃圾像素。触摸归一化要用逻辑
尺寸，所以画面也按逻辑尺寸裁；正解是读 SPS 的 conformance window（M2.6 待办），
而不是把这对数字写死。

一次 180 帧的实测：180/180 出帧，57.8 fps（软件渲染器，因为要回读窗口内容），
序号断流 0、溢出丢弃 0、非视频包 3 个被跳过；窗口内容回读与设备截图逐像素比，
平均绝对差 0.828。

## 12. 触摸注入（实测，iPhone 14,4 / iOS 27.0 / USB）

服务：`com.apple.coredevice.hid.universalhidservice`，feature
`com.apple.coredevice.feature.remote.universalhidservice`。

**外壳与 CoreDevice feature 那套不一样。** 这批 dtuhidd 服务要的是
`{messageType: "Request", featureIdentifier: ..., payload: {<动作>: ...}}`，没有
`CoreDevice.input` / `actionIdentifier` / `deviceIdentifier` 那一圈。拿
`core_device_request()` 去调它，设备不安回。

**投递的是原始 HID 报告，地址是 `_ServiceID`。** 真机列出 5 个面：

```text
  257  CoreDevice touchscreen(nil)     真数，58 字节报告（id 0x09）
  512  CoreDevice keyboard             键盘面
 1026  CoreDevice mainScreenButtons    侧键组
 1280  CoreDevice avpCustom
 1281  CoreDevice touchscreenGesture   触控板式指针，19 字节报告（id 0x13）
```

mainTouchscreen 的 58 字节报告：

```text
  0     0x09            报告号
  1-2   0x01 0x05
  3     状态：0xC2 接触 / 0x02 抬起
  4-7   X, Y            各 UInt16 LE，归一化 0..65535
  8-39  32 字节 0
  40-43 0x02 0x00 0x00 0x00
  44-49 时间戳          6 字节 LE，单调即可
  50-57 8 字节 0
```

一次点击 = 同一坐标上一个 CONTACT 加一个 RELEASE；一次拖动 = 一串推进坐标的
CONTACT 加末尾一个 RELEASE。**没有** begin/end 操作码，每个 CONTACT 都是"此刻
在此处接触着"。点与点之间要留间隔（实测 12-20ms 可用），瞬移式的拖动会被当成抖动。

**坐标是归一化的，不是像素。** 所以注入代码与分辨率无关；但也别指望它替你处理
方向：设备横过来时归一化轴跟着屏幕走，这是后面做旋转适配时要操心的事。

**认证门：必须有一条在跑的媒体流。** 没有会话时 dtuhidd 把面标成
`authenticated: NO / eventSource: externalAccessory`，backboardd 丢掉每个
digitizer 事件（"ignoring digitizer event for display <main> from unsupported
service"）。`startmediastream` 起来就把这两个标志翻成 YES，报告一路走到 UIKit
变成真的 `UIEventTypeTouches`；流的载荷可以完全不看，只要它在跑。流起来之后
稍等一下再发（本项目睡 300ms）。

**`send` 不安回信。** 一发一收地等会等到超时——所以投递必须走"只发不收"。
副作用是设备拒收时本侧毫无痕迹，"编码错了"和"发得好好的但应用没反应"长得一样。
两条应对：留一条可选的一发一收路径专门用于排查；以及把整条消息钉成字节基准
（`tests/hid_test.cpp` 里那份 284 字节的黄金向量来自一个确认能画出东西的客户端）。

**查过面之后同一条连接就不能再注入。** 实测：`connectedServices` 的回信到手后
设备关掉连接，之后的 `send` 全打在死连接上而"发送成功"。注入用的连接不要顺手
拿去查面。

**indigo 的 digitizer/keyboard/scroll 走不通。** 它们要 Apple 的 Mercury 对端
事件外壳，设备收到 dispatch 后立刻 "Resetting gesture state then canceling"，
不进任何 handler。`hid.indigo` 上确定能用的是 `remote.hid.button`
（`{messageType: "IndigoButtonEvent", payload: {state, usagePage, usageCode}}`，
usage page 0x0C 是 Consumer：home 0x40 / lock 0x30 / volup 0xE9 / voldn 0xEA /
mute 0xE2，state 1 按下 2 抬起 3 取消）。

**读设备日志这条路在 iOS 27 上不通。** `com.apple.syslog_relay.shim.remote`
能连上，但一行都不发（ASL 早就不承载 os_log 了）。想看 dtuhidd 的态度得走
os_trace/LogArchive，代价另说。

## 13. 停流、关键帧请求与恢复（实测）

**`stopmediastream` 的入参是 `{stopAll: Bool}`**，且必须**另开一条连接**去发
（复用发起 start 的那条有崩溃前科）。成功回：

```text
{serverInfo: {sessions: [], running: false, runDurationSeconds: 0},
 stoppedStreams: [4027965869]}
```

`stoppedStreams` 里那个数是 **offer 里的 u32 session_id**，不是起流时那个
`avcMediaStreamOptionClientSessionID` UUID——两条标识各管各的。形状是问出来的：
不带 `stopAll` 的四种候选形状设备都回 `Expected to find key stopAll.`；
`stopAll` 给整数回 `Expected to decode Bool but found a OS_xpc_uint64`（Swift
Codable，类型必须严格）；`stopAll=false` 配 ClientSessionID 回
`Unable to stop media stream. Invalid request sent.`（code 9009）。定向停要的是
别的标识符，本项目只跑一条流，没继续挖。

**RTCP PLI 设备不理。** 这是 `tools/pli_probe` 专门测出来的负结果：起流后发一个
RFC 4585 的 PLI（`0x81 206 len=2` + sender SSRC + media SSRC，12 字节），目的
端口取 RTCP 实测的源端口（观测到 RTCP 与 RTP 同端口，不是 RFC 3550 的"奇数端口"
惯例），两种 sender SSRC 取值（等于媒体 SSRC / 另给一个）各测一遍：

```text
基线 3 秒：包 247，IRAP 类型: 20        ← 起流那一个关键帧
PLI 后 6 秒：包 2529，IRAP 类型:（空）
```

**这个实验第一版是错的**，值得记下来：一开始对着静止的无边记画布测，4 秒后一个
包都收不到，"PLI 之后没有 IDR"看着成立，其实是因为**画面没在动、编码器根本不出
帧**。现在探针在观察窗里自己拖动画布制造持续变化（包数从 247/3s 涨到 2529/6s，
证明内容确实在动），此时仍然等不到 IRAP，负结果才算立得住。测一个"设备不理我们"
的结论之前，先证明"设备在理别人"。

**所以坏画面的唯一恢复手段是重起媒体会话**：停旧流、起新流，新会话必然带一个
关键帧。判据要两个条件同时成立——序号断流增加 **且** 距上一次关键帧超过 2 秒。
只看断流会误伤，丢一个分片也许下一帧就是关键帧。

## 14. 让设备自己交代入参形状（`tools/feature_schema_probe`）

CoreDevice 的 feature 入参在设备侧是 Swift Codable，而它对**每个必填键都点名**：

| 设备的回话 | 含义 |
|---|---|
| `Expected to find key X.` | 缺顶层或当前字典里的键 X |
| `dictionary required here` + `NSCodingPath` | 路径末端那个键要是字典 |
| `array required here` | 同上，要数组 |
| `Expected to decode String but found a OS_xpc_bool instead.` | 类型不对 |
| `Action '...' is not implemented.` | actionIdentifier 猜错了 |

所以探一个没文档的 feature，正确做法不是找参考实现照抄，而是**发一次、读它点名的
键、补上、再发**，几轮就收敛。`feature_schema_probe` 把这个循环写成了程序：一轮只
多一次 RPC，会话不重建；它按 `NSCodingPath` 把键插到正确的嵌套层，并区分字典/数组/
标量类型。

两个不显然的地方值得记：

- **类型错误报的是"正在解码的容器"，不是出错的字段**。说 `options.user` 要 String 时，
  真正不对的是我们刚往 `user` 里补的 `shortName`。第一版按字面理解，在"要字典"和
  "要 String"之间来回打转了 8 轮。
- **`NSCodingPath` 必须原样交出来**。`Rsd` 原先只把整个 error 字典 `describe()` 后
  截到 600 字节，而深层路径恰好是最长的那段，正好被掐掉——现在单独把
  `NSDebugDescription` 与 `NSCodingPath` 不截断地附上。

### 已经问出来的形状

`stopmediastream` / `action.mediastreamstop`：`{stopAll: Bool}`（见 §13）。

`listapps` / `action.listapps` 的必填键一共 8 个，全给 false 会回一个空数组：

```text
includeAppClips  includeRemovableApps  includeInternalApps  includeDefaultApps
includeHiddenApps  includeContainerPaths  includeAppGroupIdentifiers
requireContainerAccess
```

⚠️ 但只要把其中任何一个改成 true，这台 iOS 27.0 就在 60 秒内不回话（`includeDefaultApps`
单独为 true 也一样）。所以列 App 这件事得另找路子（`streamapplist`？），别指望 listapps。

`launchapplication` / **`action.launch`**（不是 `action.launchapplication`，那个直接
"not implemented"）的结构已经剥出来，但**还没打通**：

```text
options: {
  arguments: []                        // 数组
  environmentVariables: {}             // 字典
  standardIOIdentifiers: {}            // 字典
  standardIOUsesPseudoterminals: false
  startStopped: false
  user: { shortName: String }
  platformSpecificOptions: Data        // 空 Data 会被拒（"Cannot parse a NULL or
                                       // zero-length data"），得是一段 plist
}
```

填到这里设备回 `A URL to open must be specified in the launch options.`（code 10008），
而 `options.url` 给了值也还是这句——说明 bundle id / URL 的真正入口不在这套键里。
没继续挖的理由：MaaFramework 自家的 MacOS 控制单元同样不实现 start_app，它不是阻塞项。
下次要接这个，从"`url` 键名不对"这个事实出发，别从第一轮重探。
