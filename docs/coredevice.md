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

### "大回复（>1MB）传不完"的真凶：`TcpStream::recv` 的两步不在同一把锁里

症状是一整族，之前一直分开被记成三个问题：服务连接上回信大于 ~1MB 就稳定传不完、
HTTP/2 偶发 `帧长过大: 12397150`、截图服务十次里失败一两次。

真因在自家 TCP 栈里。`recv()` 做的是两件事——把 `[rx_pos_, rx_.end())` 拷出去，
再把 `rx_pos_` 推到 `rx_.size()`——而这两步**都不持锁**（`wait_for` 一返回，它自己的
`unique_lock` 就放了）。泵线程的 `on_segment` 正好可以插在中间：它往 `rx_` 尾部追加
一个段，紧接着 `rx_pos_ = rx_.size()` 把这些新字节当成"已经交出去过"，于是**一整段
被静默跳过**。回复越大、段越多，撞上窗口的概率越高；小回复基本碰不到，所以表现得
像"只有大回复有问题"。

为什么现场一点痕迹都没有：字节是被我们取走之后丢的，不是没收到，所以 TCP 序号完全
连续，专门为此加的"序号不连续"日志一声不响。

怎么抓到的：给 `Channel` 加了 `SCRCTL_H2_DUMP=/前缀`，按连接把入流原始字节另存一份，
再用 `tools/h2_replay.py` 离线当 HTTP/2 重放。一次 2MB 的成功回信在文件里 133 帧
**严丝合缝**（每帧 `DATA len=16374`，最后一帧刚好走到文件末尾），而失败那次前 10 帧
也完全对齐，到"应该有第 11 个帧头"的位置上是纯载荷字节、附近找不到任何像帧头的地方
——**是少了字节，不是字节坏了**。少的那个位置正好是帧头：设备会把一个 9 字节的帧头
单独 `write()` 成一段，丢掉这样一段之后，下一个帧头就错位成载荷字节，`len` 于是读成
一千多万。

教训两条：

1. **异步读一个双游标缓冲，"拷出去"和"推进游标"必须是一个原子动作。** 这类 bug 不在
   任何一次调用的返回值里留痕迹，只能靠把线上字节 dump 下来重放。
2. **诊断输出要自带"能对上文件"的坐标。** 报错里那句"流内偏移"（累计进来 − 手上还剩）
   才是把现场和 dump 文件对上的钥匙；没有它，dump 下来也不知道看哪一段。

修法就是让 `recv()` 在 `on_segment` 那把锁里完成拷贝和推进。修复后同一套复现脚本
（长连接连打 30 轮 2MB 截图 + 每轮新建连接 20 次）零失败。

顺带记两个副产品结论：设备的 DATA 帧是 **16374 字节**（不是 16384，别按 16K 对齐去
假设）；`Stack::bad_checksums()` 是"字节被改了"和"字节丢了"的分水岭，报这类错时先看
它，能省掉一半猜测。

## 11. 屏幕视频流的线上细节（实测，iPhone 14,4 / iOS 27.0 / USB）

`displayservice` 的 `startmediastream` 之后，设备把 RTP **反向推**到我们隧道地址上的
UDP 端口。整条链路上有六个地方不看真机字节就想不对，每一个都值一次记录。

**先绑端口，再发 RPC。** 设备一返回 answer 就开播，不存在"客户端准备好"的第二
回合。绑晚了丢的是开头几个包，而开头恰好是 VPS/SPS/PPS 和第一个关键帧——丢了
后面整段都解不出来。

**RTCP 与视频共用同一个 UDP 端口，而且它是裸 RTCP、不是 RTP。** 混在视频包里到达
的那批包一度被记成"PT=72"——那是我们自己看错了：它们的第一个字节是 `0x81`、第二个
`0xc8`，`0xc8` 是 RTCP 的 PT=200（SR），而 RTP 的 PT 字段只有 7 位，`0xc8 & 0x7f`
正好等于 72，marker 位还被顺带读成了 1。所以"PT=72"这个观测值的真实含义是
**一条 SR 被 RTP 解析器按 RTP 的规则解了一遍**（形状见 §13 末尾）。
不按 `connection.streamConfig.RxPayloadType`（实测 100）过滤，它就会被当成 HEVC
载荷解出类型 0 一类的假 NAL 混进码流——后果不是"这一帧花"，而是**永久**花，原因
见下条。

**这条流不周期发 IDR。** 一个假 NAL 或一个残缺分片就把参考链打断，之后所有帧都
在解同一个已经坏掉的参考，画面自己再也不会恢复。所以丢包处理只有一个正确姿势：
发现缺口就丢掉整个 AU，然后去拿一个新关键帧——**而这条流上关键帧请求要不来**
（PLI/FIR/NACK/RR 四种都试过，见 §13），于是唯一手段是重起媒体会话。

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

**别指望 offer 能把单帧压小。** 早先那轮 A/B 的记法是"把 f2/f3 各缩到 0.25，IDR
字节数不变，所以设备不照这张表编"——**判据用错了**（IDR 尺寸是质量决定的，不是码率
预算决定的），结论却碰巧没错。这一轮把表整个改了三遍重测，量的是实测码率和帧率：

| offer 里的码率阶梯 | answer 的 TXMaxBitrate | 实测帧率 | 实测码率 | 每帧包数 |
| --- | --- | --- | --- | --- |
| 原样（含 6000000 那档） | 6000000 | 58.8 / 59.2 / 59.1 | ~5.4 Mbps | 9.9 |
| 把 6000000 改成 60000000 | **6000000** | 59.2 | 5.39 Mbps | 9.9 |
| 删掉 6000000 那档 | 299 | 8.2 | 0.10 Mbps | 6.6 |
| 只留 >=20M 的档 | 299 | 59.5 | 0.53 Mbps | 4.8 |

三条一起读：**那个 6 Mbps 不是从我们表里挑的**（表里已经没有 6000000 了，answer 照旧
回 6000000），所以往上调没用；而表**又不能乱动**——抽掉一档会让设备退到 `f2=299` 那条
上去，流直接废掉。分辨率那头同样没有旋钮（`pair_index` 扫 0..6 恒为 1136x2464，
`pair=1` 被拒 code 32035）。申报的 `clientSupportedFeatures` 从 140 换成设备自己报的
972、或全 1 的 1023，也量不出稳定差别（一次看着像差 5fps，交替重测就抹平了——
见 §13 那条"单次对照不算对照"）。

**6 Mbps 这个预算我们改不动，但"复杂画面就掉帧"这件事当时被我按在了设备头上——那是错的，
真凶在自己进程里。** 记录一下这个弯，因为它错在一个很好笑的细节上。

当时的现象：跑游戏时手机自己屏幕 60fps 很顺，镜像里却是慢动作，实测只有 ~12fps，
而码率始终是 ~5.5Mbps。自然的读法是"设备保码率、砍帧率"。这个读法**错在没检查每帧包数**：
健康时是 10 包/帧，"卡住"时量出来还是 ~9 包/帧——**每帧大小根本没变**，变的只有帧数
和总字节数。真要是编码器在码率封顶下丢帧，帧应该变大而不是整体等比缩小。等比缩小
意味着"我们没把包收完"，而不是"设备没编出来"。

坐实它的是 `--stats` 新加的分段计时（`FramePump::Stats` 里的 `ms_depacketize` / `ms_decode` / `ms_publish`）：

```text
之前：解码 47.2 ms + 交付 33.9 ms = 81 ms/帧  -> 12.3 fps
之后：解码  3.6 ms + 交付  0.0 ms =  4 ms/帧  -> 55-60 fps
```

三处每帧重新申请一块 11MB 缓冲：解码器 `assign(n, 0)` 先 memset 一遍（sws_scale 随后
会重写每个像素）、泵发布时 `frame_ = std::move(f)` 把解码目标的缓冲搬走、主循环每轮
新建一个 `Frame` 再接 `out = frame_` 的整幅拷贝。修完就是上面那个"之后"。

所以：**这条流的码率上限确实是设备定的 6 Mbps 且改不动**（上面那张表说的就是这件事，
它仍然成立），但 12fps 不是它造成的。设备侧那两个入口（码率、分辨率）没有旋钮这一点
也仍然成立——只是当时不该拿它当借口。

**换后端要靠软解，不是靠调参。** 单帧长到 70101 字节是实测发生过的（主屏壁纸的 IDR
49652~70101，`tools/nalsizes.py` 量的；无边记那种深色画面只有 17KB，早期"最大约 17KB"
是被取样骗了），而 VideoToolbox 只吃 2 字节长度前缀（上限 65535）。喂不进去就整帧丢，
而流不周期发 IDR——症状是**连上几秒后一片灰且永不恢复**。唯一的解法是换一个没有
2 字节限制的后端：`create_software_decoder()`（libavcodec），由泵在**关键帧**上切过去——
换后端等于换一条参考链，只有 IDR 能自洽起新链。同一份录屏两个后端都 412/412 出帧，
同帧逐像素平均绝对差 0.92；真机 --no-window 下软解 44.7fps、VideoToolbox 42.2fps。

**没有软解可换时（控制单元那种 dylib 就是），这件事会从"卡一下"变成"永远解不了"。**
无边记画满白线的看板上，IDR 的单 NAL 实测超过 65535 上限（上面那句"深色画面只有
17KB"是被普通画面骗了——细白线是 HEVC 帧内编码的最坏情况），于是每一次重起拿回来的
都是同一个解不了的 IDR。泵原本只限频 1.5 秒，等于设备只要停在这种画面上，我们就每
1.5 秒做一次永不成功的停+起。现在的规则是：连续 3 次如此就置 `FramePump::video_unusable()`，
退避到每 60 秒才试一次，**解出任意一帧就自动清零回到正常节奏**。

退避计时得泵自己走，不能挂在"又收到一个超大 AU"上：降级之后会话早已被设备拆掉，
一个包都收不到，那个条件永远不成立，降级就成了出不来的坑。

调用方要配合两件事（MaaFramework 的 iOS 控制单元就是这么做的）：拿不到第一帧**不算
连接失败**（输入通路不需要这条流，取图还有截图服务这条路），以及 `video_unusable()`
时截图直接走 `capturescreenshot` 快路径，别再花 300ms+3000ms 等一帧。实测这种降级下
每次截图 ~450-900ms，画面内容与权威截图一致。

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

**认证门：曾经以为"必须有一条在跑的媒体流"，2026-09-25 复测推翻。** 早先的观测链条
是这样的：没有会话时 dtuhidd 把面标成 `authenticated: NO / eventSource:
externalAccessory`，backboardd 在 syslog 里对每个 digitizer 事件打 "ignoring
digitizer event for display <main> from unsupported service"，而 `startmediastream`
起来之后报告就一路走到 UIKit 变成真的 `UIEventTypeTouches`。于是写下了"流是输入的
硬前提"，并让每个注入探针都顺手起一条流。

复测用 `tools/hid_gate_probe`：同一次运行里按四种状态各画一条线，线落在互不重叠的
纵向带上，前后各用 `screencaptureservice` 抓一张图（这条通道与媒体会话无关，所以
会话死着也能读屏），再由 `tools/gate_diff.py` 按带数亮像素增量：

| 状态 | 注入时的处境 | 新增亮像素 | 判定 |
| --- | --- | --- | --- |
| control | 流正在跑 | +2671 | 落地 |
| idle-dead | 设备已把流结束掉、且已静默 9 秒（我们这侧的会话对象还活着） | +2488 | 落地 |
| session-gone | 我们主动 `stopmediastream` 之后 2 秒 | +2457 | 落地 |
| revived | 重新起流之后 | +2667 | 落地 |
| 另测 | 全新进程，从头到尾一次流都没起过（`hid_probe --no-stream --line`） | +2622 | 落地 |

五次增量都是同一个量级，也就是每次都真画出了一条完整的线。**结论：注入不要求此刻
有流。** 那条 `authenticated` 到底挂在什么上、什么时候会翻回 NO，仍然没查清——
`connectedServices` 的回信里 `describe()` 把这个键省略了，光靠现有探针看不到原文。
所以正确的态度是：别再为它写等待逻辑，但把这条探针留着，改动 HID 路径后重跑一次
（约 40 秒）就知道有没有退化。

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

**副作用要知道**：`stopAll: true` 停的是设备上**所有**会话，不只是我们这条。所以
同时跑两个 scrctl（或一边跑一边被人用 Xcode 投屏）会互相把对方的流掐掉，症状是两边
都在不停地"设备已结束这条流，重起媒体会话"。定向停这条路没挖通之前，一次只跑一个。

**RTCP PLI 设备不理。** 这是 `tools/pli_probe` 专门测出来的负结果：起流后发一个
RFC 4585 的 PLI（sender SSRC + media SSRC），目的端口取 RTCP 实测的源端口（观测到
RTCP 与 RTP 同端口，不是 RFC 3550 的"奇数端口"惯例），两种 sender SSRC 取值（等于
媒体 SSRC / 另给一个）各测一遍：

> 这一版发的其实是**坏包**——组装器把 RTCP 的 16 位长度字段写成了 32 位，整包 14
> 字节而不是 12 字节。结论后来用修对的包重测过，仍然成立（见本节末尾），但当时
> 那份证据不成立。

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

**画面静止约 7 秒后，设备会自己把这条流整个结束掉**（不是"暂时不发"）。判据是
查它自己的会话表：`getmediastreamserverstatus` 回
`{sessions: [...], running: Bool, runDurationSeconds: N}`，每条 session 里带
`connection.options.avcMediaStreamOptionClientSessionID`——拿我们起流时那个 16 字节
UUID 一比就知道这条还在不在。实测静止主屏上它会从"在"变成"不在"，而刚起流时同一
比对返回"在"（所以不是比对没匹配上）。因此"多久没收到包"这个时间戳判据要分成两种
处理：会话还在表里 = 流活着、只是画面没变化，什么都不该做；不在了 = 必须重起。

**这条流是"起流后约 20 秒的硬租期"，不是空闲超时。**（这一行的结论形态是错的——那 20 秒
其实是我们自己在请求里报的数，见下面"那 20 秒是我们自己在请求里报的"那一节。这一整段
**观测都成立**，包括"锚点是起流时刻而不是最后一个视频包"，只是它的成因不是设备定的。）
之前这一节写的是"画面静止 6.9
秒后被结束"，那是**一个样本读出来的**：那次视频包在 13.1 秒停、会话在 20.0 秒消失，
于是把 6.9 秒当成了间隔。后来同一批数据里另一个样本（视频 7.07 秒停、仍 20.0 秒消失）
给出的是 12.9 秒——两个"间隔"都对不上，但**两个"死亡时刻"都是 20.0 秒整**，锚点是起流
时刻而不是最后一个视频包。

把这件事钉死的是这组对照（`tools/rr_keepalive_probe`、`tools/lifetime_probe`）：

| 这一条会话 | 画面 | 我们回的 RTCP | 死亡时刻 |
| --- | --- | --- | --- |
| 静止主屏，什么都不发 | 静止 | 无 | 20.0s |
| 全程交替按音量上/下喂变化 | 一直在动（1465 个视频包，最后一个在死前 58ms） | 无 | 20.05s |
| 裸 RR 每秒一个 | 静止 | RR，发到媒体端口 | 20.0s |
| RR，发送者 SSRC 用设备那个 | 静止 | 同上 | 20.0s |
| RR + SDES(CNAME) 复合包 | 静止 | 复合 | 20.0s |
| 裸 RR 发到 端口+1 | 静止 | RFC 3550 的 RTCP 端口 | 20.0s |
| 上面三种全叠上 | 静止 | — | 20.0s |
| 每 2 秒 / 每 5 秒查一次会话表 | 静止 | 无（只是查状态） | 20.0s |

**所以：喂画面不续命，回 RTCP 也不续命，查状态也不续命。** 这张表当时读到这里就停住了——
"数字正好等于协商参数里的 `RTCPTimeoutInterval: 20`，那一定是我们没发对 RTCP"，下面那节
"为什么仍然认为存在一种能续命的写法"就是这个停法留下的。它停早了，原因见再下面一节。

这批 `rr*` 行还要交代一句证据强度：最早那几臂是用 `put32` 写 RTCP 公共头里那个**16 位**
长度字段拼出来的包，整包从第 3 字节起错位 2 字节。后来把长度字段修对、又按 answer 分配的
SSRC 重跑了一遍（表在"把发送者填对之后"那节），结论没变，但**上面这几行的原始数据里有一部分
是畸形包给出的**，别照抄成"合法的 RR 也已经被排除"。

### answer 里到底写了什么（`bitrate_probe --dump-answer`）

只打自己预先想到的那几个键，就永远发现不了"设备其实给了一个我们没读的键"。把 answer 的
`connection.streamConfig` 一个不落地展开之后，RTCP 这件事的完整合同是：

```text
RTCPEnabled            = true        RTCPTimeoutEnabled       = true
RTCPTimeoutInterval    = 20          RTCPSendInterval         = 1
RTCPRemotePort         = <我们的收流端口>      DestPort = 同一个数
LocalSSRC              = <设备自己那条流的 SSRC>   RemoteSSRC = <设备给我们这端分配的>
SourcePort             = <设备的发送端口>          SourceIP / DestIp（隧道内 v6）
RTPTimeoutEnabled      = false       RTPTimeoutInterval       = 0
SRTPCipherSuite        = 0（不加密） SRTCPCipherSuite         = 0
KeyFrameInterval       = 0           RateAdaptationEnabled    = true
TXMaxBitrate           = 6000000     TXMinBitrate             = 333000
CustomWidth/Height     = 1136/2448   Framerate                = 60
TxCodecFeatureListString = "FLS;SW:1" ...
connection.timeout     = 20
```

**`LocalSSRC` / `RemoteSSRC` 这两个名字是从设备的视角起的**，这一点是探针一比就露出来的：
RTP 头里设备自己那个 SSRC 等于 answer 的 `LocalSSRC`，所以 `RemoteSSRC` 才是"设备替我们
编好的发送者 SSRC"。以前所有 RTCP 实验都在自己编 SSRC（`0x35c0ffee`）或者拿设备那个当
发送者，也就是说**从来没有填对过发送者**。

### 把发送者填对之后仍然续不上命

`tools/rr_keepalive_probe` 加了按 answer 分配的 SSRC 来发的三臂，每臂 30 秒观察窗：

| 臂 | 发送者 SSRC | 报告块指认 | 包型 | 目的端口 | 结果 |
| --- | --- | --- | --- | --- | --- |
| none | — | — | — | — | 20.0s 死 |
| rrmine | answer 的 `RemoteSSRC`（=设备给我们分配的） | `LocalSSRC` | RR | 设备发送端口 | 20.0s 死 |
| rrminep1 | 同上 | 同上 | RR | 发送端口 +1 | 20.0s 死 |
| rrminesr | 同上 | 同上 | SR(RC=0) | 设备发送端口 | 20.0s 死 |
| rrneg / rrnegp1 / rrnegsr | `LocalSSRC`（填错人的那一版，留着当对照） | `RemoteSSRC` | RR / SR | 两种端口 | 20.0s 死 |

连同前面那五种写法，现在排除掉的是：**自己编的 SSRC、设备的 SSRC、设备分配给我们的那个
SSRC × {RR, RR+SDES(CNAME), SR} × {媒体端口, 端口+1} × {复合, 拆开}，外加查状态、喂画面**。

### 那 20 秒是我们自己在请求里报的：`startmediastream` 的 `timeout`

上面这一整套"到底哪一种 RTCP 能续命"的问题，问法本身就是错的。合同的另一半在我们自己的
代码里，`src/media/StreamSession.cpp` 的 `build_start_request()`：

```cpp
xpc::dict_set(d, "timeout", xpc::make_uint64(timeout_seconds));   // Request 里默认 20
```

于是 `20` 这个数同时出现在三个地方：我们发出去的请求、设备回给你的 answer
（`RTCPTimeoutInterval: 20`、`connection.timeout: 20`），以及那条准点到的死亡时刻。**第一
个是我们自己写的**，从第一天起照抄抓包观测值，从来没动过。所有"发 RTCP 能不能续命"的实验
都是在这个数=20 的前提下跑的，所以它们不可能越过 20 秒——不是 RTCP 没发对，是那道题压根
不在这张卷子上。

改这一个整数就能拿到结果（`tools/rr_keepalive_probe --timeout N --attempts 1 --what none`：
什么都不回，只看设备自己那个每秒一个的 SR 什么时候停）：

| 请求 `timeout` | answer `RTCPTimeoutInterval` | 最后一次收到包的时钟 |
| --- | --- | --- |
| 6 | 6 | +5.99s |
| 20（旧默认） | 20 | +19.99s（前后二十多臂全落在 20.0±0.05s） |
| 30 | 30 | +30.00s |
| 3600 | 3600 | 150 秒观察窗**跑满**：53520 个视频包、150 个 SR 心跳，结束时查会话表**还在** |
| 4294967295 | 4294967295 | 40 秒窗跑满（8084 个视频包、39 个 SR），会话表里还在 |

**死亡时刻逐字跟着我们报的那个数走。** 所以这条流并没有"20 秒租期"这回事，只有一条"客户端
要求设备保留多久"的租期；设备也不需要收到我们的 RTCP 才兑现它。它是一个**从起流时刻开始、
长度由请求参数指定的固定倒计时**——这个模型和此前每一条观测都自洽：喂画面不延长、回 RR 不
延长、把发送者 SSRC 按 answer 填对也不延长，因为它不看这些。

**产品侧的直接影响**：`StreamSession::Request::timeout_seconds` 从 20 改成 3600
（`FramePump` 显式传这个数，接续时刻按它算），那条"每 20 秒必须换一次会话"的节拍就消失了。
真机验证：`scrctl --stats` 连跑 75 秒，`重起 0`、`序号缺口 0`、渲染 4361 帧、**全程 60.0fps**、
SIGTERM 之后干净停流。改之前这 75 秒里会有 4 次接续（每次约 300ms 没有新帧），也就是用户报
的那句"看起来还是有时候会断"。

报 3600 而不是报到顶，是因为租期越长，进程被 SIGKILL 时留在设备侧的那条僵尸会话就占住设备
越久（一台设备一次只容一条流，Xcode 的 DeviceHub 也共用这一格）。正常退出路径我们会主动停。

### 那有没有"保活"？没有——三条独立的证据

键名叫 `RTCPTimeoutInterval`、旁边还写着 `RTCPTimeoutEnabled=true`、`RTCPSendInterval=1`，
读起来就是一个"收到对端 RTCP 就复位"的空闲计时器。它**不是**。三条各自成立的证据：

1. **没有这么一条 RPC。** `displayservice` 在 RSD 目录里只暴露六个 feature：
   `getmediasupportinfo`、`getmediastreamserverstatus`、`startaudiooutput`、
   `startvideooutput`、`startmediastream`、`stopmediastream`。没有 renew / update /
   keepalive 之类；`getmediasupportinfo` 的回复里也没有 timeout 的取值范围
   （只有 `supportedFeatures: 972` 和 `avcFrameworkVersion`）。
2. **把 RTCP 打满也不复位。** 判据要短租期才灵敏：报 20 秒时"1/s 的 RR 到底有没有复位
   那个计时器"和"它就是个固定 TTL"两种模型都表现为 20.0 秒死。改成报 8 秒再跑（
   `rr_keepalive_probe --timeout 8 --hz N`），三种频率 + Apple 自己那条 RCTL 通道：

   | 臂 | 8 秒里发出的 RTCP | 最后一个 SR | 会话表 |
   | --- | --- | --- | --- |
   | none（对照） | 0 | +7.99s | 已没了 |
   | rrsrcsd @1Hz | 21 | +7.995s | 已没了 |
   | rrsrcsd @10Hz | 21 | +7.987s | 已没了 |
   | rctl（20/s + 每帧伴随包） | 641 | +7.996s | 已没了 |

   每一臂都死在**请求里那个数**上，一个字节都不差。注意"发出 RTCP"这一列：@10Hz 那一臂
   也只发了 21 个，和 @1Hz 一样——因为发送判断在收包内层循环之外，而忙画面上那一层永远
   不空。**这是探针自己的节奏缺陷**（已修：内层每 32 个包回一次外层），所以这一轮里真正
   把频率打上去的是 rctl 那一臂，8 秒 641 个包（约 80/s，含每帧一个的伴随包）仍然不复位。
3. **设备那边根本没有"收到多少 RTCP"这个可观测项。** 把
   `getmediastreamserverstatus` 整棵树一页不落地展开（`--dump-status`），会话条目里和
   状态有关的只有三个：`status.running`、`status.runDurationSeconds`、
   `status.connectionErrors`。**没有任何收包/收 RTCP 的计数器**，所以连"我们的 RTCP 到
   没到"都没法从设备侧证实——能观测到的只有那口死亡时钟，而它对一切无动于衷。

结论：这条路上唯一的"续命"手段就是**再发一次 `startmediastream`**，而它会顶掉旧会话
（`two_session_probe`），也就是那约 300ms 没有新帧的代价。所以正确用法不是"想办法续命"，
而是**一开始就把租期报得够长**，把接续从"每 20 秒一次"变成"一小时一次"。

#### 那 pymobiledevice3 那一配方（`timeout=20` + 每秒 RR+SDES）到底断不断？断，每 20 秒

这个问题不该靠"它是参考实现"来推，也不该靠读它的代码来推，答案在字节里。它视频侧的保活
（`screen_stream.py` 的 `_build_rtcp_rr` + `_build_rtcp_sdes` + `_rtcp_send_loop`）是：
1 秒一个复合包，RR 32 字节（`0x81 C9 0007`，发送者 SSRC=answer 的 `RemoteSSRC`（我们这边），
被报告的 SSRC=`LocalSSRC`（设备那条流），丢包三项全 0，扩展最高序号=真实收到的，抖动/LSR/DLSR
全 0）+ SDES 12 字节（空 CNAME），目的端口 `streamConfig.SourcePort`。这一段 `rr_keepalive_probe
--what rrsrcsd` 逐字节复刻过（探针开头自检 `RR 32 / SDES 12 / 复合 44`）。

拿旧默认 `timeout=20` 跑它，真机记录是两轮独立的 `[rrsrcsd]`：`最后视频包 +19998ms
最后 SR +19983ms 结束时会话表=已没了`、`+19993ms / +19990ms / 已没了`。前一轮的观察窗
是 45 秒（租期的两倍多），所以"已没了"不是窗口先合上。**保活包照发，时钟照走。**
反过来把 `--timeout 3600` 配上同一臂，150 秒窗口跑满（90353 个视频包、150 个 SR），结束时
会话表还在——同一套包，唯一的变量是请求里那个整数。中间还有一臂 `--timeout 30 --what rrsrcsd`：
视频包在 +19916ms 就停了（静止画面本来就不发，见 §11），但 SR 心跳一直发到 +24989ms、观察窗
结束时会话表还在——**30 秒的租期就活过 30 秒之前**，这一臂顺手把"视频停了"和"流断了"分开
了，正是 `ServerState` 那个枚举要防的混淆。

p3 自己的代码就是这件事的第二份证词，只是它把结论读反了方向：`_build_rtcp_rr` 的注释写着
"if we never send RTs the encoder stalls within ~25 s"。**25 = 20 秒租期 + 它自己那个
`_STALL_RESTART_SECS = 5.0` 看门狗**，也就是它确实每 20 秒丢一次流，只是把这件事归因成了
"编码器卡住"，再靠 `_stall_watchdog` 重发 `startmediastream` 兜回来（`_STALL_RESTART_COOLDOWN_SECS
= 15.0`、`_MAX_STALL_RESTARTS = 3`，注释里还留着一句"重启太频繁会把 coredeviced 搅到新的
RemoteXPC 握手都超时、只有重启设备能救"）。连 Apple 自己那条 RCTL 反馈通道也不续命：
8 秒租期那一臂发了 641 个 RTCP（约 80/s，含每帧一个的伴随包），仍死在 +7.996s。

#### 拿真 p3 复跑了一遍：同样断，而且比推断更难看

上面那句"它每 20 秒丢一次流"当时只是从复刻臂 + 它的代码推出来的，我一度以为它的看门狗会把
代价摊薄成一次约 300ms 的卡顿。**这个量级是错的**，实测比它差一个数量级。

条件：pymobiledevice3 11.17.0，`developer core-device display serve-web`（macOS 上它默认
piggyback Apple 的 `remoted` 原生 tunnel，**不需要 root**），挂一个 `/stream.bin` 订阅者
（订阅者存在才会武装它的 stall 看门狗），画面是持续动着的地图。三个互不相干的观测点：

| 观测点 | 结果 |
| --- | --- |
| 页面像素（往观看页注入 16×16 指纹、10Hz 采样） | 冻结区间 0.2–3.9s、24.0–29.0s、49.1–54.2s、74.2–79.2s、99.3–104.3s：**节拍 25.0±0.1 秒，每次约 5.0 秒**，期间它自己的徽标显示 `0.0 fps`、覆盖层写着 "Stream offline – waiting for frames…" |
| p3 服务端日志 | `no AU progress in 5.0–5.4s (subscribers=1, attempt 1/3) - restarting stream` @ +43.7s、+68.8s、+93.9s，同样 25.1 秒节拍 |
| 设备会话表（用 p3 自己的 `get-media-stream-server-status` 问） | +15s 两条会话（audio+video），每条都回显 `'timeout': 20`、`RTCPTimeoutInterval: 20.0`；+40s/+68s/+90s 只剩 video，且 `status.runDurationSeconds` 是 6/6/2 —— 每次都是刚重起的那条 |

另一轮没挂订阅者的独立测流（只读 `/stream.bin` 的字节到达时刻）给出同一件事的客户端视角：
空隙起于 +25.1s、+51.2s、+76.1s、+101.3s，各持续 3.3–4.3 秒（另有冷启动那 3.1 秒在等第一个
IDR）。**为什么是 5 秒而不是我们那次接续的约 300ms**：它要先等满 5 秒无 AU 才肯动手，加上
重起会话本身 1–2 秒——判据的等待时间全算进了用户可见的黑屏里。

**顺手证伪了它音频侧那句注释**。`_audio_rtcp_send_loop` 写着"不发 RR 设备 20 秒就收走音频
会话；一个 highest-seq=0 的 RR 是能撑过静音期的合法保活"。它的音频 RR 从起流起就无条件 1 Hz
在发（那段代码特意不 gate 在收包上），可音频会话照样在 20 秒从表里消失，而且再也没回来——
因为它的看门狗只管 video，audio 要有 `/audio.bin` 订阅者才会被重起。所以"RR 能续命"在它的
音频路径上同样不成立，只是那里的表现是"音频悄悄死掉且没人重启它"。

**一个当时没注意、其实该早点用的可观测项**：设备的会话表里每条会话都带着 `'timeout': 20`
和 `type: audio|video`，还有各自的 `status.runDurationSeconds`。也就是说"这条会话报了多长
租期、现在跑了几秒、是哪一路"是**能在设备侧直接读到的**，不必只靠"uuid 还在不在"这一位。
`StreamSession::status()` 已经把整棵树取回来了，将来要把接续时刻钉得更准，从这几个字段取
比用我们自己的墙钟更贴近设备的事实。

**这批 8 秒实验里有一处没解释的现象，记下来别丢**：修好"发送节奏"之后重跑同样三臂
（`--hz 1/10/25`），每一臂都在**起流后约 1.1 秒**就视频档和 SR 档一起停掉（344 个视频包、
1 个 SR），而不是请求里那个 8 秒；同一晚紧接着的另外两次长窗口实验（40 秒、45 秒，包括
一次"静置 150 秒不碰手机再起流"）都是正常的 1/s SR 一路不断。也就是说这个 1.1 秒静默
**既不是租期到点**（8 秒/3600 秒都不符），也不是产品路径的常态——产品那一侧同一晚两次
长跑（75 秒、40 秒）都是 0 重起、SR 每秒一个。原因没查出来，怀疑与"设备侧编码器/显示
管线在某种闲置状态下暂停推流"有关，但没有证据。**它不影响上面那张表的结论**：那一轮四臂
（对照 + 三种 RTCP）彼此一致，都是死在自己请求的那个数上。下次再遇到 1.1 秒静默，先看
`发出 RTCP` 那一列和 `status.connectionErrors`，再决定要不要怀疑设备。

顺带记一个还没用上的字段：`status.connectionErrors`。它的语义没查（跑动图 + 长会话时它
一直是 0），但它是目前唯一"设备自己承认状态有问题"的出口，将来要区分"我们没收齐"和"设备
发送侧出错"时可以从这里找。

**要意识到长租期顺手拿走了一样别的东西**：过去那次"每 20 秒换一次会话"虽然烦人，却顺带
充当了"定期拿到一个干净 IDR"的兜底——这条流不周期发 IDR（§11），参考链一旦因为丢包坏掉，
本来要等到下次换会话才自愈。现在那个兜底没有了，真正兜事的是 `stall_restart_ms` 那条判据
（**新的序号缺口** + **超过 2 秒没解出关键帧** 两条同时成立就重起会话，见 `FramePump.cpp`
里 `stalled` 那段），它和租期长度无关，所以长租期不会把"丢一个包"变成"画面永久坏掉"。
两条判据都要留着：只看缺口会误伤（丢一个分片也许下一帧就是关键帧），只看"多久没关键帧"
又会在静止画面上白白重起。

为什么这么久才看出来：`timeout` 这个键在参考实现里被注释成 "Negotiation timeout in
seconds"（读起来像"客户端等 answer 的超时"），而它的默认值恰好也是 20，于是谁都没把它和
那条 20 秒的死亡时刻联系起来。教训是一句更一般的话：**当一个观测值同时是"我们发出去的
一个参数"和"对端回来的一个字段"时，它就是自变量，不是环境常量。** 我们手里那份
`--dump-answer` 里甚至写着 `connection.timeout = 20`，看到了、记下来了、当设备参数读了。

顺带把 Mac 侧那些符号留在这里，它们本身没错，只是被读反了方向。承载这条流的框架里
Apple 自己的接收端实现是**双向**计时器：
`/Library/Developer/PrivateFrameworks/CoreDevice.framework/.../CoreDeviceMediaStreamSupport`
里能读到这些名字——

```text
isRTCPEnabled  isRTCPTimeOutEnabled  isRTPTimeOutEnabled
rtcpRemotePort  rtcpSendInterval  rtcpTimeOutInterval  rtpTimeOutInterval
stream:didReceiveRTCPPackets:  streamDidRTCPTimeOut  streamDidRecoverFromRTCPTimeOut
"Got event: %%. Stream hit RTCP timeout. Treating as an error."
底层是 AVConference.framework 的 AVCMediaStreamConfig / AVCMediaStreamNegotiator
```

`rtcpTimeOutInterval` 是这些属性的**输入**之一，也就是"谈出来的那个数"；
`streamDidRTCPTimeOut` / `streamDidRecoverFromRTCPTimeOut` 说的是**到期时接收端怎么处置**
（当成错误、并允许恢复），而不是"只要发了 RTCP 就能免于到期"。DeviceHub 那边之所以看着
不断流，与这一点也不矛盾：它要么报了一个很长的数，要么就是在我们看不到的一层把到期当作
一次静默重起——这正好是 `streamDidRecoverFromRTCPTimeOut` 这个名字描述的动作。

**顺带记一个工具级的教训：`lifetime_probe` 之前把 `next_press`/`next_second` 初始化成
了绝对时刻（`t0 + 1000`），而比较用的是相对秒表 `t = now_ms() - t0`，于是 `t >= 那个
绝对值` 永远不成立——它既不按键喂画面、也不打每秒那一列，静默地什么都没做。** 而它交
回来的"活了整整 45 秒"被当成了"证明不是固定 20 秒租期"的关键证据写进过这一节。一个
什么都不做的探针给出的恰恰是对照组数据，却读成了实验组。修好之后同一臂 20.05 秒就死。
（同一个坑的另一种形态见上面"包 1778 被读了 16 秒"：**探针必须能证明自己做了事**——
要么打计数，要么打副作用，否则它"没报错"不等于"它测的那个东西成立"。）

**静止这件事在包层是可观测的，而且不用问设备。** 设备在它自己的 SR 里带"累计已发视频包
数"这件事给了第二条尺：画面静止时**视频包一个都不发，而每秒那个 SR 照旧**。于是泵里有两
把分开的尺（`Stats::video_packets` / `sr_packets`，收包处按数据报开头字节分），"画面静止"
= 视频档静默而 SR 档不静默，"流死了"= 两档一起停。实测一条静止会话的读数：

```text
  + 7s 帧号 416 收包 479（视频 472 SR 7）距上一帧 180ms
  + 8s 帧号 416 收包 480（视频 472 SR 8）距上一帧 1147ms
  ...   视频档整整 12 秒不动，SR 一档一秒一个，一个不多一个不少
  +19s 帧号 416 收包 488（视频 472 SR 16）
```

**"到点就接续"因此改成"到点之后挑一个静止的间隙接续"。** 这笔钱在当时那个租期下非付不可
（20 秒不延长），能选的只有时刻：画面在动时接续是看得见的一次顿挫，画面静止时接续是免费的
（显示的那一帧本来就停在那里，新会话回来的第一帧和它一模一样）。所以 `kSessionLeaseMs =
18000` 之后还有 `kSessionLeaseHardMs = 19400`——18 秒到点先等着，视频档一静默
（`kRenewQuietMs = 1000`）就立刻接，等不到就在 19.4 秒硬接。催流那条路反过来：只有过了那
个租期才直接重起，租期内会话**还活着**，手上这一帧就是用户这一下要的帧，不能因为"快到点了"
扔掉。

**这一整段的前提后来被推翻了**：那个 20 秒是我们在请求里自己报的（见上面"那 20 秒是我们
自己在请求里报的"）。把请求值报成一小时之后，"每 20 秒付一次钱"这件事就没有了——接续从
"每 20 秒一次"变成"连续镜像一小时才一次"，所以那几个阈值也从按比例（90% / 97%）改成按
固定余量（提前 2 分钟开始找静止间隙、提前 30 秒硬接），挑静止时刻这套逻辑本身留着。

**为什么必须付、而且没法靠"先起新的再切"躲掉**：`tools/two_session_probe` 量了设备上能
不能同时跑两条会话。答案是不能，而且第二次的 `startmediastream` 会**把第一条从会话表里
顶掉**：

| 观察 | 读数 |
| --- | --- |
| 起 B 之前先收空 A（基准线） | A 在 2000ms 里 419 个视频包 |
| B 的起流 RPC | 84 ms |
| A 在 B 起来之后 | 6 个包，全部落在 RPC 返回那一刻（socket 里的欠账），此后 6 秒 0 包 0 SR |
| B 的第一个视频包 / 第一个 IDR | +184 ms / +306 ms（对着发出请求那一刻） |
| 两条会话的 RTP SSRC | 不同（`35e14ede` vs `00ddfe5c`）——真是两条独立的流，不是同一条复制到两个端口 |
| 结束时设备会话表 | A 不在，B 在 |

SSRC 那条尤其要看：它排除了"第二条只是把同一条流镜像了一份到另一个端口"这种可能，所以
"只有一条活着"是设备侧的独占，不是我们的收包顺序问题。A 那 6 个包则是这条探针差点读错的
地方——只看窗口内计数会得出"两条都在发"，而 A 的最后一个包在 +84ms。**判活要按时刻判，
不能按计数判**（`tools/two_session_probe` 的第一版就交回过这个假结论）。

于是换一次会话的代价被钉死在约 300ms（RPC 84 + 首帧 100 + IDR 306），`--feed` 全程喂画面
变化实测一次硬接续吃掉 **270 ms**（其间按过 1 次音量键，所以确实吃掉了内容），占 45 秒的
0.6%；而两次挑到静止间隙的接续（`画面已静止 1782ms（数据报静默 51ms）`、`6619ms（0ms）`）
没有产生任何可见损失——接续之前早就没新帧了。这条判据本身由
`tools/lease_renew_probe` 看着泵自己的对外读数打（serial 间隔 + `stats().restarts` 增量，
后者用来把一条顿挫归到"换会话"头上而不是"画面本来就静止"）。

**这条重起是承重的，别当成浪费去优化掉**：设备结束流之后，画面再变也不会自己恢复，
只有重起能拿到新 IDR。

**回任何 RTCP 都拦不住它拆流，静止画面上也喂不出帧。** 协商参数里写着
`RTCPSendInterval: 1` / `RTCPTimeoutInterval: 20`，很自然地会猜"是不是我们不回
接收报告设备才结束流"。测下来这条路是死的，但过程里踩到两个自己的坑，都值得记：

1. **旧实验发的是坏包。** `rtcp_header` 把 RTCP 的**16 位**长度字段写成了 32 位
   （`put32(p, 2u)`），整包从第三个字起错位两字节。所以"设备不理 PLI"这个旧结论
   的证据基础是一个畸形包——修对格式之后重测，结论没变，但**这条得记下来**：
   拿一个自己组装的包去证明"设备不理"之前，先核对它的字节数。
2. **一次假阳性。** 修格式前那轮，`pli` 收到 120 个包和一个 type 20 的 IDR，看着
   像"PLI 有效"。把 `none`/`pli` 交替跑 4 轮就露馅了：

   | 轮次 | none（什么都不发） | pli |
   | --- | --- | --- |
   | 1 | 551 包 + IDR | 0 |
   | 2 | 0 | 0 |
   | 3 | 0 | 229 包 + IDR |
   | 4 | 0 | 347 包 + IDR |

   帧在**两组里都随机出现**——屏幕上每隔几十秒就有东西自己在动（无边记的浮层/光标），
   是它把帧喂出来的，不是我们的请求。控制组只跑一次不算控制。

修对格式后（PLI 12 字节 `81ce0002`+sender+media、FIR 16 字节 `84ce0003`+序号、
NACK 16 字节 `81cd0003`+PID/BLP、RR 32 字节 `81c90007`+20 字节报告块；SSRC 用媒体
SSRC，目的端口用设备的发送端口），真静止画面上 6 秒观察窗里**四种全是 0 个视频包、
0 个 IRAP**。加上早先"RR 发到视频端口会让投递几乎停"的观测，当时的结论是：
**这条流上没有能用的"请设备给一帧"的 RTCP 通道**。

这一句现在要限定范围，它写得太满。那次实验有三个变量同时没控住：① 两个 SSRC 位置都填了
设备自己那条流（发送者应当填 answer 分配给我们这端的 `RemoteSSRC`，见上面那节）；② FIR 的
PT 用的是 206，而标准里 FIR 是 RTPFB=205 FMT=4；③ **offer 里 `allowRTCPFB=0`**——参考实现
的抓包笔记明写着这一位是 FIR 的受理闸（"it honors FIR (PT=206 FMT=4, requires
`allowRTCPFB`)"），闸关着时测出来的"不理"是必然的。

**这三个变量后来各自控住重测了一遍，答案是负面的：设备仍然不给 IDR。** 租期改大之后
（观察窗不再被 20 秒截断），`tools/fir_probe` 按 `{PT=205, PT=206} × {发送者填设备的流号,
发送者填 answer 分配的 `RemoteSSRC`} × {allowRTCPFB=0, 1}` 组合跑，每臂"前摇 3 秒什么都不
发，之后每 2 秒发一次请求、共 12 秒"：

| 臂 | 视频包（12s） | 前摇里的 IRAP | 后 9 秒的 IRAP | 其中跟在某次发送后 1.5s 内 |
| --- | --- | --- | --- | --- |
| none | 7257 | 1 | 0 | 0 |
| fir205m+fb | 7256 | 1 | 0 | 0 |
| fir205m | 7251 | 1 | 0 | 0 |
| firm（PT=206 + 填对角色） | 7232 | 1 | 0 | 0 |
| fir+fb（PT=206 + 填对角色 + 闸门开） | 7239 | 1 | 0 | 0 |
| plim+fb | 7225 | 1 | 0 | 0 |

**没有一个臂在发出请求之后拿到 IRAP。** 所以成立的说法是"填对 SSRC、PT 两种都试、并且
申报 `allowRTCPFB` 之后，设备依旧不响应关键帧请求"——参考实现那句"requires allowRTCPFB"
在这台设备（iPhone14,4 / iOS 27.0）上没有兑现成"给了就能受理"。
顺带这一轮还第一次量清了"这条流自己多久产一个 IDR"：画面一直在动（12 秒 7200+ 个视频包），
后 9 秒里自发 IRAP 是 **0** ——和 §11 那条"不周期发 IDR"以及参考实现记的"1588 帧零 IDR"
对上了。

判据本身也修过一次，值得记：上一版从"拿到起流 IDR"直接接进观察窗并且**立刻**发第一个请求，
于是五个臂（包括从不发包的 `none`）全都报出"IRAP 在 +1~2ms 到达"，看起来像 FIR 被受理了。
那一个是**起流自带的那个 IDR 的尾巴**——它有几十上百个分片，阶段 1 在第一个分片就判定成功
并 break，剩下的分片在几毫秒内组装完成，而发请求恰好也在 +0ms。所以现在的判据前面必须有
3 秒"前摇"（什么都不发），前摇里来的 IRAP 单独计数，非 0 就推翻那一轮的相关性读数。

（注意"请设备给一帧"和"我们发出去的 RTCP 能不能给自己续命"是两件事：后者已经定案，答案是
**都不影响**，因为那个时刻就是我们自己报的 `timeout`。）

**重起本身很便宜，贵的是"发现"。** `tools/restart_gap_probe` 量了三种情形各 3 次：
会话还活着就停、等它自己结束后再 `stop`、等它结束后不 `stop` 直接起——**九次全部
成功，间隔 0ms 也没问题，停+起一共 37–90ms**。对着一条已被设备结束的会话调
`stopmediastream` 不报错，也不需要额外等待。所以"点下去愣一下"几乎全在发现延迟上，
下面三件事都是在省这一笔。

**"静默多久算流死了"要按 SR 心跳来量，不是按拆流时长。** 这条判据错过三次，把三次
都记下来：

- 第一版 150ms：画面只要变得比 150ms 慢一点，每次按下都撞上一整轮"停+起+全量 IDR"，
  延迟叠加到用户看到慢动作。
- 第二版 7.5s：拿"最后一个视频包之后 6.9 秒拆流"当数据报静默的阈值。错在**两把尺**：
  6.9 秒量的是视频包，而 `last_packet_ms_` 量的是数据报，中间隔着设备那每秒一个的
  SR。会话消失的同一秒 SR 才停，所以静默是从**死亡那一刻**开始计时的，7.5 秒等于在
  死亡之后还要再瞎等 5.5 秒。实测后果：静置 20 秒去截图，那时会话已经死了 1.3 秒，
  阈值没到，于是交回一张与动作前逐像素相同的旧图。
- 现在按心跳分三档（`kSrPeriodMs` 附近）：静默 ≤1.2s 心跳还在，什么都不做；
  1.2~2.5s 可疑（一个 UDP 丢包就能造出同样的现象），去问设备那一条状态 RPC；
  >2.5s 连着两个心跳都没了，直接重起不花 RPC。
- **这三档得是催流和静默自催共用的同一把尺。** 曾经只有 `wake()` 走它，而"没人动手
  时"的静默定时器是盲等 `silence_restart_ms`=3000ms 才去问一句——于是每一次设备拆流
  后面都跟着至少 3 秒的冻屏，而这段冻屏里只要有任何变化（推送、时钟走字）就是看不见
  的。用户报"看起来还是有时候会断"，账就记在这 3 秒上。现在两条路调同一个 `judge_quiet`
  （差别只是第三档的门：催流 2.5s，自催用 `silence_restart_ms`），真机 62 秒里两次拆流
  都在**静默 1247ms / 1205ms** 处被发现并重起，整段可见停顿 ~1.5 秒。1.2 秒这把尺的下限
  就是心跳周期：SR 一秒才一个，想再快只能更频繁地问设备，那笔 RPC 是每次 100~300ms。

**中间那一档不能省，因为纯时间阈值必然留一个盲区。** 死亡时刻相对我们的检查点是
任意的，而"要静默满一个 SR 周期才敢断定死了"就意味着总有一段"刚死、阈值未到"的窗口
落在窗口里。把阈值调大不会消灭它，只会把它推到别处（第二版就是这么把 150ms 的误伤
换成了 5.5 秒的漏判）。要么问设备，要么等——问设备实测几乎不要钱，见下表。

**wake() 到第一帧的实测预算：两档都是 191–272ms。** `tools/wake_latency_probe`
把 silence/stall 自动重起都关掉，等包计数静默到指定值再催一次，扫了两档各 2-3 次：

| 催的时机 | 走哪条路 | wake() 到第一帧 |
| --- | --- | --- |
| 静默 1537/1548/1545ms | 问设备一句，答"不在"，重起 | 241/239/272ms |
| 静默 4115/4090/4165ms | 直接重起 | 232/191/232ms |

问那一句的代价在墙钟上几乎看不出来（中位差 9ms）——大头是设备吐第一个 IDR，不是
我们的 RPC。**为什么专门量它**：任何按"截图 -> 动作 -> 再截图"跑的调用方都会给等帧
设一个固定超时，这个数就是超时该设多大的依据；设小了它不等重起回来就退回旧帧，调用
方拿到的是拆流前那一刻的画面，却以为是动作之后的。取帧方（MaaFramework 的 iOS 控制
单元）据此把预算定在 800ms。

**这一句 `wake()` 有没有用，是用交替对照验的。** 驱动 `driver6`（在 /tmp，脚本性质，
未入库）跑"连接 -> 截图 -> 静置 20 秒 -> swipe 画一条横贯画面的线 -> 再截图"，两种
构建**交替**各跑两次：

```text
有 wake()：截图耗时 241/228ms   视频帧 vs 权威画面 差   32/133 个亮像素   PASS
无 wake()：截图耗时 308/307ms   视频帧 vs 权威画面 差 2769/2621 个亮像素  FAIL
                                （视频帧的带内计数与动作前逐字相同：7892->7892、
                                  6377->6377，而权威画面同期 +2769、+2621）
```

三个判读上的坑，都是这一轮踩出来的：

1. **不能拿视频帧和截图服务的图逐像素比。** 视频是有损编码，细白线（无边记的笔迹）
   上编码误差就能造出几千个"差异像素"，把通过判成失败。要比的是**同一条带里的亮像素
   计数**，不是像素本身。
2. **画布会被自己写满。** 前面几十条线画下去之后，新的线大半落在旧线上，带内净增只有
   几个像素，"动作落地"这条判据直接失效。要么换空带，要么画横贯画面的长线。
3. **`newer()` 的 `since` 必须是"调用这一刻的 serial()"**，不是"上次取走的那一帧的
   号"。会话死了之后泵手里那张旧帧也满足"比上次取走的大"，于是 3ms 内就被当成新帧交
   出去——这是本轮最难看见的一个错，因为它**返回真、耗时正常、内容却是陈的**。
   FramePump.h 里那条已经写成显式警告。

另外 `reviving()` 的置位点必须在**问设备那一句之前**：那条 RPC 自己就要 100~300ms，
只置在 `restart()` 里的话，取帧方在这段窗口读到 false，照样交出旧帧（实测三次全这样）。

**`--stats` 的读数自己会造出"在断流"的错觉，三种。** 用户拿一份"看起来还是有时候会断"
的日志来对账，逐行核下去发现真正的断只有一次（设备拆流 + 我们重起），其余全是读数：

1. **打印挂在"这轮取到帧了"的分支上** → 断流的那几秒正是日志最该说话的那几秒，却
   什么都不打；等它恢复，读者只看到一串掉下去又慢慢爬回来的帧率，没有原因。
2. **`渲染 N 帧 X fps` 打的是 N/全程时间**，是滑动平均的亲戚：一次 3 秒停顿能把平均
   压到 47，之后连着几行 47.3/47.6/47.9/48.2，读起来像"恢复之后还在持续掉帧"，而那
   几段的瞬时值实测 55/55/56。平均数只能回答"是不是一直在掉"，不能回答"现在在掉吗"。
3. **两个"每秒"的分母不是同一把尺**：设备那一档只能除以"上一个 SR 到现在"（SR 一秒
   一个），我们那一档除以打印窗口（实测 0.6~1.3 秒飘）。把它们相减就是那行
   `差 +283 / −283` ——一虚一实，看着像在大量丢包，而真正的丢包计数 `序号缺口` 全程为 0。
   同理，**累计数不能跨会话比**：设备 SR 里的包数每条会话从零重数，我们的 `packets`
   全程连着涨，重起一次之后"设备 143 我 5866"凭空多出五千包"丢失"。

教训是同一句：**诊断输出里每一个数都得写清它的分子分母和基线**，否则它会替读者编出
一个不存在的问题。同一个坑早先还以另一种形状踩过一次——把累计包数当每秒读数打
（`src/app/main.cpp` 的 `print_stats` 注释里记着那个"包 1778"）：一个没有分母的数
不是读数。

**设备的 RTCP 形状（第一次解码）。** 从视频同一个 UDP 端口发来，每秒一个，整包 64
字节，是一个裸 RTCP 复合包：

```text
81 c8 000c                       V=2 RC=1 PT=200(SR) 长度=12 字 -> SR 本体 52 字节
b6d5b756                         SSRC = 视频流的 SSRC
ee5fe7ad ce849000                NTP 时间戳（秒.分数秒）
000057fb                         RTP 时间戳
0000016c                         已发包数（对得上那一秒收到的视频包数）
00062109                         已发字节数
db1aac0e 00000001 00000000…      报告块，24 字节——比 RFC 3550 的 20 字节多 4 字节，
                                 多出来的语义没记
81 ca 0002 b6d5b756 01000000     SDES：CNAME，长度为 0
```

我们的 RTP 解析器会把它读成"PT=72"：`0xc8` 的 bit7 是 RTCP PT=200 的一部分，而 RTP
的 PT 只有 7 位，`0xc8 & 0x7f = 72`，marker 位还被顺带读成 1。

**设备在通话中会直接拒绝起流：code 9022** "A phone or VoIP call is currently in
progress on the device."。这不是我们这边的问题，也不是会话表被占住——同时刻查
`getmediastreamserverstatus` 回的是 `{sessions: [], running: false}`，表是空的。
顺手排掉一个更吓人的怀疑：**`kill -9` 掉 scrctl 不会在设备上留下僵尸会话**（留的话
这里就会看到 sessions 非空，而后面每次起流都被拒）。所以 9022 的真实含义就是"设备
正在通话"：这时**截图服务是好的**（实测 `capturescreenshot` 照样出图），只有视频流
起不来——正是控制单元那条兜底路径，所以碰到它不要绕、不要重试，让调用方按它自己的
"还没有第一帧"处理（scrctl 命令行则是提示去挂断，见 `src/app/main.cpp` 起流失败处）。

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

⚠️ 但只要把其中任何一个改成 true，这台 iOS 27.0 就在 60 秒内不回话。**真正的原因不是
那个开关，是回复尺寸**（2026-09-25 定论）：全部为 false 时秒回一个空数组，而一个 App
都不漏的列表在这台设备上是 239 条、每条带完整安装路径——正好落在"服务连接上的大回复
传不完"那一类（§15）。所以列 App 一律走 `streamapplist`，别再碰 listapps。

### 流式 feature（streamapplist / streamprocesslist）

不是"一条大回信"，而是**一次请求 + 一串小回信**，所以天然绕开尺寸问题。形状：

```text
请求：CoreDevice.input = { actualInput:  <真正的参数>,
                           streamProxy:  { sideChannel: <客户端自己生成的 XPC UUID> } }
回信：一串，每条带 CoreDevice.XPCMessageKey.sideChannelStatus，值是单键枚举
        pushing:        { elements: [ ... ] }   若干条
        finishStreaming: {}                     最后一条
        receivedError:  ...                     中途失败时取代上面两条
```

`XPCSideChannel.uniqueIdentifier` 会把那个 UUID 原样带回来；一条连接只跑一条流时
不需要校验。整条流的失败是一个普通的 `CoreDevice.error` 回信，不是 sideChannelStatus。
实测（iPhone14,4 / iOS 27.0）：239 个 App 全部推完，正常收尾。
实现见 `ServiceConnection::stream`，用法见 `App::list`。

### 停一个 App

设备目录里**没有** terminate/kill app 这种 feature，只有 `sendsignaltoprocess`，而它
只认 pid；`listprocesses` 又只回 `{processIdentifier, executableURL}`。所以
bundle id → 进程要绕一圈（`App::stop`）：

```text
streamapplist  →  该 bundle id 的安装目录（…/<UUID>/MobileSafari.app）
listprocesses  →  可执行文件路径落在这个目录下的所有 pid
sendsignaltoprocess {process:{processIdentifier}, signal:9}  逐个发
```

匹配必须带上末尾那个 `/`（`…/Foo.app` 不能把 `…/Foo.app2` 算进来），钉在
`tests/app_test.cpp`。真机判据用进程表而不是截图：`--grep MobileSafari.app` 从
1 个变 0 个，总进程数 312 → 303（它自己的 XPC 服务跟着没了）。

**刚杀掉的 App 立刻再起，设备有概率回 code 10004** "The process identifier of the
launched application could not be determined. It may have already terminated."——这句
字面上像"起来了但报不出 pid"，实测**是真的没起来**（进程表里 0 个）。它是进程还没死
干净时的竞态：隔 2 秒再起必成，立刻起则时好时坏，而成功那次往往要 300~400ms（平时
50ms）。所以 `App::launch` 把它当可重试错误，最多 4 次、间隔 500ms。加了重试之后
"停→起"连跑 5 次全成。

这条也修正了本文早先那句"`terminateExisting: true` 会先杀掉再报 10004"：10004 跟那个
开关无关，任何"刚杀完就起"都会撞。不杀（false）仍然更稳，理由见下面。

`launchapplication` / **`action.launch`**（不是 `action.launchapplication`，那个直接
"not implemented"）**已打通**（2026-09-25，iPhone14,4 / iOS 27.0，Safari 与无边记都
被切到前台）。完整形状在 `remote/App.cpp`，两个关键点都是"照着字段名猜一定猜错"：

```text
{ applicationSpecifier: { bundleIdentifier: { _0: "<bundle id>" } },   // 顶层！
  options: { arguments: [], environmentVariables: {},
             standardIOUsesPseudoterminals: true, startStopped: false,
             terminateExisting: <bool>, user: { shortName: "mobile" },
             platformSpecificOptions: <Data: 一段 plist，空字典的 plist 就行> },
  standardIOIdentifiers: {} }
```

1. **bundle id 在顶层的 `applicationSpecifier`，而且还要再套一层 `_0`**（XPC 里带
   关联值的枚举 case 就是这个形状）。之前所有次尝试都把它塞在 `options` 里，设备的
   回话是 `A URL to open must be specified in the launch options.`（code 10008）——
   一个 specifier 都没认出来时它退到"按 URL 启动"那条分支去要 url。**这句报错是误导
   性的**，照着它找"url 键名"会一路找错。
2. `platformSpecificOptions` 不能是零长 Data（"Cannot parse a NULL or zero-length
   data"），得是一段解得开的 plist。

**`terminateExisting: true` 不是"更安全地重来"，是会把 App 弄丢。** 实测对一个正在
前台的 App 用它：设备先把实例杀掉，然后回 code 10004（"…could not be determined"）
——**返回失败而前台 App 已经没了**，比不调用还糟。事后看，10004 的真凶是"刚杀完就起"
这个竞态而不是这个开关本身（见下一节），但开关确实每次都制造一次竞态，而 false 不制造。
所以默认走 false（只唤起、不动在跑的实例），这也正好与 MaaFramework Android 侧的语义
一致：那边 start_app 是 `monkey -p <pkg> 1`，`am force-stop` 才是 stop_app。

形状钉在 `tests/app_test.cpp`（离线，不碰设备）——这条 RPC 键放错位置时设备不给字段级
报错，所以线上看不出来，只能在这里拦。
