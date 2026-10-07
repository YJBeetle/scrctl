# CoreDevice / DDI 机制调研

scrctl 立项前的验证记录。目标：确认能否在**不越狱、不装 WDA、不要 root、不依赖 Xcode GUI** 的前提下，从 Mac 对 iPhone 做屏幕采集 + 触摸注入。

结论：**可以，且已端到端跑通。**

- 实测帧率 **58.9 – 61.1 fps**（面板 60Hz）
- 触摸注入成功，鼠标拖动在画图 App 中留下平滑连续笔画，坐标映射准确
- 音频、硬件按键、键盘输入、剪贴板在参考实现中均已具备

> **阅读提示（当前实现，2026-10-07）**：本文保留了按时间推进的实验和排错假设，原始读数不等于当前产品策略。
> 媒体会话的当前依据是 [UDP 修复后的反馈对照](#udp-修复后的反馈对照)：已测设备可用 RR 续期，PLI 可请求关键帧；产品不发送 FIR。
> 起流 offer 已改用 XML plist，格式对照与设备范围见 [BPLIST_COMPATIBILITY](BPLIST_COMPATIBILITY.md#当前结论)。
> 后文标为历史记录的固定倒计时、无 RTCP 保活和定时接续方案，保留用于说明当时的推理过程。

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
发现缺口就丢掉整个 AU，然后去拿一个新关键帧——**关键帧请求是发 PLI**（实测 20~35ms
回 IDR，见 §13 末尾；此前这里写着"PLI/FIR/NACK/RR 四种都试过、要不来"，那是建立在
我们发出的 UDP 从来没到设备之上的错账），PLI 不管用时才重起媒体会话。

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

> **本节当前结论**：timeout 是已测设备的 RTCP 空闲超时。产品视频、音频分别报 20 秒并每秒发送 RR；
> 丢包后的关键帧恢复先用 PLI。allowRTCPFB 默认关闭时该路径已验证，字段的完整作用尚未确认。
> 下面包含 UDP 修复前的失效推断，当前反馈行为请直接看 [UDP 修复后的反馈对照](#udp-修复后的反馈对照)；音频续期见 [音视频并行与独立续期验证](#172-音频腿接进产品四个问题的真机读数toolsaudio_pump_probeasan-下跑)。

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

> **作用范围更新**：上段“本项目只跑一条流”描述的是当时的研究阶段。当前产品可同时运行视频和音频，
> stopAll 仍会停止设备上的所有媒体会话。两种媒体的共存验证见 [音视频并行与独立续期验证](#172-音频腿接进产品四个问题的真机读数toolsaudio_pump_probeasan-下跑)。

**副作用要知道**：`stopAll: true` 停的是设备上**所有**会话，不只是我们这条。所以
同时跑两个 scrctl（或一边跑一边被人用 Xcode 投屏）会互相把对方的流掐掉，症状是两边
都在不停地"设备已结束这条流，重起媒体会话"。定向停这条路没挖通之前，一次只跑一个。

**RTCP PLI 设备不理。**（**已作废，正确结论在本节末尾的"修好之后第一次真测"**：PLI
有效。留着这一段是因为它记录了两个各自独立就能造出假负结果的坑——发的是坏包、以及
在静止画面上测。）这是 `tools/pli_probe` 专门测出来的负结果：起流后发一个
RFC 4585 的 PLI（sender SSRC + media SSRC），目的端口取 RTCP 实测的源端口（观测到
RTCP 与 RTP 同端口，不是 RFC 3550 的"奇数端口"惯例），两种 sender SSRC 取值（等于
媒体 SSRC / 另给一个）各测一遍：

> 这一版发的其实是**坏包**——组装器把 RTCP 的 16 位长度字段写成了 32 位，整包 14
> 字节而不是 12 字节。而"用修对的包重测过仍然成立"这句话**也是错的**：那一次重测时
> 我们发出的 UDP 数据报根本没到设备（`UdpSocket::send()` 的缓冲区拼装 bug，见本节末尾
> "根因的代码位置"）。**真测的结论在节末：PLI 有效，20~35ms 就回一个 IDR。**

```text
基线 3 秒：包 247，IRAP 类型: 20        ← 起流那一个关键帧
PLI 后 6 秒：包 2529，IRAP 类型:（空）
```

**这个实验第一版是错的**，值得记下来：一开始对着静止的无边记画布测，4 秒后一个
包都收不到，"PLI 之后没有 IDR"看着成立，其实是因为**画面没在动、编码器根本不出
帧**。现在探针在观察窗里自己拖动画布制造持续变化（包数从 247/3s 涨到 2529/6s，
证明内容确实在动），此时仍然等不到 IRAP，负结果才算立得住。测一个"设备不理我们"
的结论之前，先证明"设备在理别人"。

> **历史恢复策略，已更新**：下面的“唯一恢复手段”基于尚未送达设备的反馈包。
> 当前先请求 PLI 并等待关键帧，未恢复时再由上层重起会话，见 [UDP 修复后的反馈对照](#udp-修复后的反馈对照)。

**所以坏画面的唯一恢复手段是重起媒体会话**：停旧流、起新流，新会话必然带一个
关键帧。判据要两个条件同时成立——序号断流增加 **且** 距上一次关键帧超过 2 秒。
只看断流会误伤，丢一个分片也许下一帧就是关键帧。

> **历史超时推断，已更新**：下面的静止间隔与固定硬租期来自当次没有有效 RTCP 到达的样本。
> 这些时刻不能说明静止画面必然结束会话，也不能说明 timeout 无法复位；有效 RR 续期见 [UDP 修复后的反馈对照](#udp-修复后的反馈对照)。

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

> **表格解释更新**：原表中的断流时刻保留，但 UDP 未到达设备的 RR 行不能用于否定 RTCP 续期。
> 画面变化和状态查询也不等同于 RTCP 接收，见 [设备日志与接收计数](#根因找到了而且答案是设备自己说的我们的-rtcp-一个都没到) 与 [UDP 修复后的反馈对照](#udp-修复后的反馈对照)。

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

> **SSRC 语义补充**：LocalSSRC 是设备的媒体源，RemoteSSRC 是客户端发送反馈的身份。
> 已测设备回显 offer 声明的客户端 SSRC；下文“替我们编好”“分配”是当时的描述，不表示设备总会另行生成该值。

**`LocalSSRC` / `RemoteSSRC` 这两个名字是从设备的视角起的**，这一点是探针一比就露出来的：
RTP 头里设备自己那个 SSRC 等于 answer 的 `LocalSSRC`，所以 `RemoteSSRC` 才是"设备替我们
编好的发送者 SSRC"。以前所有 RTCP 实验都在自己编 SSRC（`0x35c0ffee`）或者拿设备那个当
发送者，也就是说**从来没有填对过发送者**。

### 把发送者填对之后仍然续不上命

> **这张表里每一臂"20.0s 死"都不作 SSRC 的证据**：那一轮发出去的 UDP 全部没到设备
> （`pkts in: 0`）。SSRC 到底要不要填设备分配给我们那一个，是在修好拼装之后重测的，
> 答案记在本节末尾——**要填**，而 `rrsrc` 那一臂（发送者 = `RemoteSSRC`、报告 =
> `LocalSSRC`、目的 = `sender.port`）实测每秒一个 RR 能把会话续过 40 秒。

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

> **历史模型与产品方案，已更新**：timeout 取值对无有效 RTCP 的断流时刻有影响，这组测量仍成立。
> 由此推导的“固定倒计时”“RTCP 不复位”及 FramePump 一小时定时接续已不适用。
> Request 默认 3600 留给一次性调用方；当前产品使用 20 秒加周期 RR，见 [UDP 修复后的反馈对照](#udp-修复后的反馈对照)。

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

### 那有没有"保活"？在我们这条路（CoreDevice feature）上没有——三条独立的证据

> **历史否定结论，已作废**：本标题和下文“没有保活”不能作为现状引用。
> 不存在续租 RPC 不代表不存在 RTCP 续期；送达验证与当前行为分别见 [设备日志与接收计数](#根因找到了而且答案是设备自己说的我们的-rtcp-一个都没到)、[UDP 修复后的反馈对照](#udp-修复后的反馈对照)。

> **这一整节的结论已经作废**，留着是因为它就是那条走了两天的弯路：它"实测"的是
> **我们发出去的 UDP 数据报**，而那时那些包一个都没到设备（`UdpSocket::send()` 的缓冲区
> 拼装 bug，见本节末尾"根因的代码位置"）。正确答案在下一节末尾：**保活就是回 RTCP**，
> 1Hz 的裸 RR 就能把 20 秒的空闲计时器一直归零。
>
> 下面那段范围收窄的话当时是对的、但只是半步：它承认了"苹果不断流是一桩还没解释完的差异"，
> 而那个差异后来也解释完了——苹果每秒真的有一个 RTCP 落到设备的 socket 上，我们没有。

> 这一节的标题原先是无条件的"没有保活"。**范围要收窄**：三条证据覆盖的是
> `com.apple.coredevice.feature.startmediastream` 这条路，而我们后来抓到苹果客户端的
> **请求原文**——它走的正是这个 feature、`timeout` 也照样报 20（见下面"把苹果的请求逐键
> 对齐之后"那一节）。所以"苹果不断流"既不是反证也不是保活的证据，它是一桩**还没解释完的
> 差异**；对我们这条路的结论不变——我们只有这条路可走（用户态、免 root、可跨平台）。

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

> **历史复刻实验的范围**：下列本项目探针的断流读数受 UDP 发送缺陷影响，不能据此否定该协议的 RR 保活。
> 参考代码描述也不能替代接收端计数；本项目修复后已验证的结果见 [UDP 修复后的反馈对照](#udp-修复后的反馈对照)。

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

> **当次参考客户端故障记录**：下面保留指定版本、环境与订阅条件下的真实读数。
> 这次运行的根因没有由 scrctl 的 UDP 修复直接证明，不能将其归因成同一个缺陷，也不能推广为所有参考客户端都无法用 RR 续期。
> 当前本项目的反馈验证见 [UDP 修复后的反馈对照](#udp-修复后的反馈对照)。

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

> **音频结论的范围更新**：下列数据描述当次 p3 音频路径的故障，不能证明合法 RR 无法保活音频。
> 当前 AudioPump 使用独立 RR，静音与音视频并行的续期已验证，见 [音视频并行与独立续期验证](#172-音频腿接进产品四个问题的真机读数toolsaudio_pump_probeasan-下跑)。

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

#### 那 DeviceHub 为什么不断流？——本节当时的结论"它压根不走这个 feature"已被抓包否证

观测方法便宜得出奇：让 Xcode 的 DeviceHub 把镜像起起来，同时用只读 RPC
`getmediastreamserverstatus` 每 7 秒问一次设备"你手上现在有哪些会话"。不用逆向、不用抓包、
不用 root。（这一步是用户点的：他打开 DeviceHub 的同时我轮询。）

设备回的内容，12 次抽样：

- 两条会话，`TxPayloadType=101/AudioStreamMode=8` 和 `TxPayloadType=100/Framerate=60`，
  共用同一个 `avcMediaStreamOptionClientSessionID`（音频+视频配成一条 Mirror 会话，
  这一点 p3 也照做了）。
- 每条的 `status.runDurationSeconds` 依次是 **84, 91, 98, 104, 111, 118, 124, 131, 138,
  144, 151, 158**——每次 +7，和墙钟严格同步；而 `LocalSSRC`/`RemoteSSRC`/`SourcePort`/
  `DestPort` **全程一个字节没变**。也就是它 **158 秒里一次都没换过会话**。
- 每条的 `RTCPTimeoutInterval` 都是 **20.0**、`RTCPTimeoutEnabled=True`、
  `RTCPSendInterval=1.0`。
- **会话条目里没有 `timeout` 键，也没有 `type` 键。**
- 视频那条 `IsltrpEnabled=True`、特性串 `VRAE:0;SW:1;FLS`（p3 是 LTRP 关、`FLS;SW:1`，
  并且它注释里专门写"VRAE:0 不能进 avc 特性串"——又一处我们和苹果不一样的地方）。

"它不带 `timeout` 键却活了 158 秒"和"我们带 `timeout` 就多少秒死"如果都成立，只剩一种读法：
**报数=硬租期，不报=用设备那个能被 RTCP 复位的空闲计时器**。这个假设可以直接判——探针加
`--no-timeout-key`，照苹果那样整个键不发。结果两臂（`none` 和 `rrsrcsd`）都被拒：

```text
com.apple.coredevice.feature.startmediastream 失败（code 4865）
  NSDebugDescription: Expected to find key timeout.
```

**但上面那段"DeviceHub 不走这个 feature"的结论是错的**，被下一节的抓包直接否掉了：它走的
就是 `com.apple.coredevice.feature.startmediastream`，连 `timeout` 也照样报 20。会话条目里
没有 `timeout`/`type` 只是因为设备把它消费掉了、没往记录里放——不是"这个客户端没发"。
一条从"设备回显了什么"倒推"客户端发了什么"的推理，中间缺了一环：**回显是子集，不是同射**。

#### 把苹果的请求逐键对齐之后：八个假设连同数据一起作废，请求与回信层面已经没有没控住的差异

> **历史假设表的证据边界**：请求原文、参数回显与连接关闭时刻保留为观测；
> 依赖“RTCP 已送达”的否证在当时不成立，不能据这些断流行排除反馈续期或认定全部请求差异都已控制。
> 后续送达证据与重测见 [设备日志与接收计数](#根因找到了而且答案是设备自己说的我们的-rtcp-一个都没到)、[UDP 修复后的反馈对照](#udp-修复后的反馈对照)。

抓包方法（这次真的用到，而且不需要 root）：DeviceHub 的媒体会话跑在本机 **utun7**
（`fda4:4c2:5901::2` 就是这台 Mac 在那个隧道里的地址），隧道已经被 `remoted` 解封装，所以
**内层的 RTP/RTCP 和 XPC 消息都是明文**。`sudo tcpdump -i utun7 -w /tmp/dh.pcap` 抓 95 秒，
再用 p3 的 `XpcWrapper`（外层 magic `0x29B00B92`、内层 `0x42133742`/v5）按消息边界切开解码。
注意外层还套了 **HTTP/2**（流开头是 `PRI * HTTP/2.0`，XPC 装在 DATA 帧里）。

解出来的苹果请求，和我们的逐键比对：

```text
CoreDevice.input = { options{avcMediaStreamOptionClientSessionID:uuid,
                             AccessNetworkType:1, TransportProtocolType:2,
                             CoreDeviceVideoDisplayMode:"DisplayByID", VideoStreamForDisplayID:1},
                     receiverIP, receiverPort, senderIP, direction:"output",
                     clientSupportedFeatures:140, type:"audio"|"video",
                     timeout:20,                       ← 和我们一样是 20
                     sessionEventChannel:<UUID>,       ← 只有这一项我们没有
                     negotiatorOffer:<bplist> }
actionIdentifier = com.apple.coredevice.action.mediastreamstart
```

> **offer 格式更新**：上框的 bplist 是该次 Apple 抓包的实际格式，不是设备只接受 binary plist 的协议声明。
> 当前生产请求仍将 offer 放入 XPC Data，但内容使用 XML；视频、音频与生产恢复对照见
> [BPLIST_COMPATIBILITY 的真机对照](BPLIST_COMPATIBILITY.md#真机对照)，结论仅覆盖所列设备、系统与请求。

另外几条从抓包里读到的事实：先起音频再起视频、两条共用一个 `ClientSessionID`、
**但每条各有自己的 `sessionEventChannel`**（所以它是按流给的）；`startmediastream` 整场只发了
**2 次**（相隔 0.27 秒，一次 `type:"audio"` 一次 `type:"video"`，**不是重试**），之后到抓包结束
再没有第三次。

**"它的保活包形状我们早就复刻对了"这句是错的，这里更正**（错在把两条腿的包混成一条腿看的，
下一节按端口重新量了一遍）：那条精确 1.000Hz 的 44B `RR+SDES` 是发在**音频腿**上的
（客户端 52800 → 设备 54228），而**视频腿整场几乎没有 RR**——只有 46 个，间隔在 1 到 4 秒之间
跳，是事件驱动不是定时。视频腿上一直灌的是两种 PT=204 的厂商 APP 包。

在这个基础上判掉的假设，每个都带数：

| 假设 | 判据 | 结果 |
| --- | --- | --- |
| 不发 `timeout` 就能绕开租期 | `--no-timeout-key` 两臂 | **feature 层必填**，直接拒：`code 4865 / Expected to find key timeout.` |
| 悬空的 `sessionEventChannel` 能破租期 | `--timeout 20 --event-channel --what none` | +19.961s 死，死时表里 0 条会话 |
| 苹果那套全量 RTCP 能复位计时器 | `--timeout 20 --what rctlrr --hz 20`，60 秒发了 **1650** 个包 | +20.005s 死 |
| 差在 offer 的 `VRAE:0` 上 | `--avc-features 'FLS;VRAE:0;SW:1;'` + 事件通道 + 全量 RTCP | 设备**照收并回显** `TxCodecFeatureListString=VRAE:0;SW:1;FLS`（和 DeviceHub 一模一样），然后 +20.005s 死 |
| 差在 offer 的 `ltrpEnabled` 上（苹果的答案回 `IsltrpEnabled=True`，我们一直是 False） | `--timeout 20 --what rctl,rctl+ltrp` 同一轮交替 | 设备**受理了**（`--dump-status` 里回显 `IsltrpEnabled=真`），流照旧死：`rctl` +19988ms、`rctl+ltrp` **+20004ms** |
| 信封里的 `coreDeviceVersion` 是策略开关（苹果 642.16，我们 629.3） | 查 p3 | p3 也发 629.3 且也 20 秒死 → 不是它（642.16 没实测） |
| 会话要有一条**活着且被服务的**宿主连接才不被回收 | `--hold --hold-no-poll`：自己开连接起流、握着并持续 `service()` | **能握**——30 秒整场连接没断，而流仍然死在 **+19968ms**。所以"有没有宿主连接"不是那个变量 |

**"唯一还站得住的差异是 HTTP/2 传输框架"这句也作废了**，而且作废得干脆：我们自己的服务连接
本来就走 HTTP/2——`remote/RemoteXpc.cpp` 发的是 `PRI * HTTP/2.0` 前置签名 + SETTINGS +
WINDOW_UPDATE + PING，XPC 消息装在 DATA 帧里，和苹果同构。把两份抓包按消息逐条对齐之后：
苹果每条新服务连接的前置三帧 flags 是 `0x1 / 0x201 / 0x400001`，和我们的一字不差；整场 95.8 秒
里苹果用到的 flags 位只有 `{0x1, 0x101, 0x201, 0x10101, 0x20101, 0x400001}`，**没有出现任何
我们没名字的位**（`xpc_flags=0x10101`、`messaging_protocol_version=7`、UUID 的类型标记
`0xa000` 全都一样）。所以差异不在传输框架，也不必为它去写 HPACK。

**握连接那三条臂顺带挖出一条产品规则**，代价很高且原先不知道：

```text
A. 只握连接、不起流                 连接在 <=5.2s 被设备关掉
B. 起流 + 握着 + 不再发第二次请求     连接 30 秒整场没断；流仍死在 +19968ms
C. 起流 + 握着 + 同一条连接查状态      连接与会话【同时】死在 +241ms / +242ms（两次独立复现）
```

C 那一臂做的就是"在发起 `startmediastream` 的那条连接上再发一次请求"，后果是**当场把媒体会话
带走**（视频包冻在 61–65 个、一个 SR 都没收到）。这不是会话到期，是我们自己把它戳死的。
已知的同族规则是 `stop()` 必须另开一条连接（设备侧有崩溃前科，见 `StreamSession.h`），现在
把它推广成：**任何后续请求都得另开连接；起流那条可以握着不放手，但只能握着。**
B 同时否掉了"会话要有一条活着且被持续服务的宿主连接才不被回收"——能握，握住了也不延长租期。

**"目前唯一还没控住的结构性差异：苹果有音频腿"——这条也否证了，而且这次拿得出凭证。**
判据不是"起了音频腿看它断不断"，而是设备在 answer 里自己写的配对标记
`SyncStreamToken` / `VideoSynchronizationSourceStreamToken`：苹果视频腿=1183494201、
音频腿=0。我们带上音频腿再跑（`--audio-leg --audio-rr --what rctl --timeout 20`），
视频腿拿到 **1183494365**、音频腿 0，**与苹果逐字同形**——也就是说设备确实把两条腿
配成了同一组，而视频腿仍然死在 **+20003ms**。之前那几轮"带了音频腿照样死"不作数，
是因为当时没有任何一位能证明配对成功过；现在能证明了，这条假设才是真的判掉。

### answer 逐字段对完之后：差异只剩三个，全部各自判死；顺带纠出两处我们自己的错

> **历史对照的范围**：此表保留协商字段相同与回显成功的事实。
> 当时的 UDP 未到达设备，因此“字段对齐后仍断流”不能证明这些参数决定了合法 RTCP 的处理方式。
> RR、PLI 和 allowRTCPFB 的后续对照见 [UDP 修复后的反馈对照](#udp-修复后的反馈对照)。
> 其中旧 FIR 使用非标准布局，结论范围已在该节修正。

把苹果视频腿 answer 的 40 个 `streamConfig` 键抄下来，和我们 `bitrate_probe --dump-answer`
的逐键对，**不同的只有三行**：

| 键 | 苹果 | 我们 | 判据 |
| --- | --- | --- | --- |
| `IsltrpEnabled` | True | False | 我们申报 `ltrpEnabled=1` 后设备回显 `IsltrpEnabled=真`，流仍 +20004ms 死 |
| `TxCodecFeatureListString` | `VRAE:0;SW:1;FLS` | `FLS;SW:1` | 拿苹果 offer 原文 482 字节直接发（不走我们的构造器），设备回显一字不差，流仍 +19977ms 死 |
| `SyncStreamToken` 对 | 1183494201 / 0 | 1183494365 / 0 | **已经对齐**（上一节），仍然死 |

其余 37 个键、`connection.*` 全部、以及端口三项都一模一样。这里必须记下一条**纠正**：
`RTCPRemotePort` 不是"设备的另一个端口"，它是**我们自己的收流端口**——苹果那边
`RTCPRemotePort=49637=DestPort=receiver.port`，而它的客户端把 RTCP 发到
`56179=SourcePort=sender.port`；我们这边同样 `DestPort==RTCPRemotePort`、RTCP 发到
`sender.port`。**两边逐字同形**，所以"设备的 RTCP 监听在另一个端口上、我们前面所有
RTCP 实验都发到了没人收的端口上"这个怀疑不成立（它当时写在探针注释里，还配了一组
"实测 54351 与 61422"的数字——把 /tmp/p3test 里 21 次跑的 answer 拉出来对，
`sender.port` 与 `SourcePort` **21/21 全相等**，那两个数在任何一份日志里都不存在，
是编的。`rrsrc*` 那几臂从头到尾没换过端口）。

> **尚未确认的语义**：下文 UUID 出现次数是抓包事实，但只出现一次不能证明该事件通道没有注册或对端。
> 当前仍未确认 sessionEventChannel 的完整语义，不能把悬空 UUID 视为已证明等价的通道实现。

**`sessionEventChannel` 这个"苹果有、我们以前没有"的键，也不再是差异了。** 拿那两个
UUID 的 16 字节原文在整场抓包里搜：视频腿的 `2ebb6824-…` **只出现 1 次**、音频腿的
`5085804c-…` **只出现 1 次**，都只在各自的起流请求里，此后设备与客户端谁都没再提过
（对比：`ClientSessionID` 出现 6 次）。也就是说苹果自己也没给这个通道接上对端。
我们 `--event-channel` 那一臂填的悬空 UUID，恰好就是苹果的形状。

> **可观测性更新**：下文仅描述当时查询到的 status 字段。未在该回复找到计数器，不等于设备侧无法确认接收。
> 后续已通过 syslog 与 socket 的 pkts in 证明数据报未到达，见 [设备日志与接收计数](#根因找到了而且答案是设备自己说的我们的-rtcp-一个都没到)。

**"设备到底收没收到我们的 RTCP"这一问问不出来**：`mediastreamstatus` 的整棵原文里
只有 `runDurationSeconds` 与 `connectionErrors` 两个活字段，没有任何方向的包计数
（`--dump-status` 每 10 秒全树打一次，看到的就这些）。所以"我们的包没送到"这个解释
**既不能证实也不能证否**——但它已经不是唯一剩下的解释，见下面第三条。

**"+40 秒之后苹果还在发什么"这件事有明确答案，而且它把保活的可能压到了很小**：按连接
枚举 +40s 之后的客户端→设备写入，**所有 CoreDevice 服务连接全部静默**（displayservice
上的 21 次 `mediastreamgetsupportinfo` 集中在 +19.7~+21.2，之后一次没有；devicecontrol
即 `universalhidservice` 的 10 条消息集中在 +20.9~+22.8；deviceinfo、configuration、
pasteboard 同），唯一还在动的是一条**隧道维护**连接：

```text
客户端 -> 设备端口 52116   MessageType=Heartbeat, SequenceNumber=n   每 30.00 秒一个
                           设备回一个同 SequenceNumber 的空消息
建立时：MessageType=Handshake, MessagingProtocolVersion=7,
        Properties{RemoteXPCVersionFlags, BoardId, ProductType…}, 之后有 StartTls
```

这个端口不在 RSD 的服务表里，消息也不是 `CoreDevice.*` 的形状——它是 RemotePairing 那套
隧道消息，30 秒心跳维持的是**隧道本身**，不是那条媒体会话（而且 30 秒复位不了 20 秒的
计时器）。我们自己的隧道今天能在没有这种心跳的情况下连续跑 150 秒并持续收流，所以它
不是媒体租期的钥匙。记下来是因为它是"苹果做了、我们没做"清单上最后一项有形的东西。

**"客户端侧还附着哪些连接"这个方向已经走到头了**，三个候选各测一臂、全部维持 20 秒死：

| 附着的连接 | 臂 | 结果 |
| --- | --- | --- |
| 起流那条 displayservice 连接，握着且持续 `service()` | `--hold --hold-no-poll` | 握得住（30 秒不断），流 +19968ms 死 |
| `deviceinfo` 上的 `displayinfoupdates` 流式订阅，起流前挂上、跨过期点不断重连 | `--display-subscribe --what none` | 收到 3 次显示推送（含 +20092ms 那次），视频腿 +19999ms 死 |
| `universalhidservice`（抓包里设备端口 54572，苹果起流后 0.2 秒连上且整场不关） | `--hid-attach --what none` | `connectedServices` 查到 5 个面，视频腿 +19993ms 死 |

**还有一处是我们自己填错的，与租期无关但要记下来**：RCTL 最后一个字。我们按
`(累计包数<<16)|60001` 填，注释里写着它是"累计包数"。拿苹果 1469 个 RCTL 量：高 16 位
**首 72、末 2246，净增速 29.3/s，而且会倒退**（每秒窗口里的增量中位数 76），而同期视频
包速率 83.9/s、marker 帧速率 42.1/s——**三个数谁也不是谁**，所以它既不是累计包数也不是
累计帧数，是一个我们没定性的量（低 16 位 `{60001:1075, 60000:355, 0:39}` 我们填对了）。
前面几轮"包形状全部复刻对了"这句话在这一位上说过头了。这一位填错的后果**不知道**——
它的语义我们没定性，所以既不能说它无关，也不能说它有关；只能说"苹果发的那个量既不是
我们的包数也不是帧数"。

顺带记一条不对称，它是下一轮的方向：**我们的音频腿活得过 20 秒，视频腿活不过。**
`--audio-leg` 那几轮里，视频包在 +20s 之后冻住不再增长，而音频包一直还在来
（+30s、+40s 两档的计数仍在涨）。所以被按时摘掉的是**显示采集那一条会话**，
不是 `ClientSessionID` 那一组。这一列的绝对数不可信（音频包速率我们量到 12~29/s，
而苹果是 100/s，中间还夹着探针单循环轮流收两条腿的取样偏差），但"死后还在来包"
这个定性是稳的。

### 根因找到了，而且答案是设备自己说的：我们的 RTCP 一个都没到

上面那一串"逐个变量控住再重测"的臂，全部建立在同一个从没验过的前提上——
**"我们发出去的 RTCP 到了设备"**。它错了。判它的仪器不是抓包（用户态隧道抓不到，
见本节开头），而是**设备自己的 os_log**：`pymobiledevice3 syslog live -o 文件`
（非 root 可用）能流式收到设备上 `dtremotedisplayd`、`avconferenced`、`kernel`
三个进程的明文日志，而媒体会话就是它们在管的。

起一条 20 秒租期的流、按 `--what rctlrr --hz 20` 发了 **1041 个 RTCP** 之后，
设备在死亡那一刻依次打出这四行：

```text
kernel  : udp connect: [::1:62073<->::2:56006] interface: utun7 (skipped: 1025)
avconferenced[VCMediaStream [ERROR] checkRTCPPacketTimeoutAgainstTime:lastReceivedPacketTime:
                    Last RTCP packet receive time:nan
dtremotedisplayd   : streamDidRTCPTimeOut(_:): AVC[1183494379] RTCP Timeout
kernel  : udp_connection_summary [...] process: avconferenced:21675
          Duration: 20.081 sec  bytes in/out: 0/1772803  pkts in/out: 0/1864
          rxnospace pkts/bytes: 0/0  so_error: 0
```

**`pkts in: 0`。** 那条 UDP socket 收进 1864 个包（我们收到的 RTP），收进来 0 个，
而且**没有任何丢弃计数**（`rxnospace 0`、`so_error 0`）——不是到了被丢，是根本没到 PCB。
`lastReceivedPacketTime` 是 `nan`（从未被赋值），于是触发 `streamDidRTCPTimeOut`，
于是会话被摘。

这一条同时把两件事翻正、把八件事解释干净：

- **"20 秒是 RTCP 空闲超时"这个最初读法是对的**。后来我说它被推翻了，那句话只对了
  一半：`RTCPTimeoutInterval` 的确就是我们在请求里报的 `timeout`（所以报多少秒死多少秒
  这条测量没错），但**计时的机制确实是"多久没收到 RTCP"**，不是无条件倒计时。
  错的是我由此得出的"设备不看我们发什么"。
- **上面那八条否证一次性全解释完了**：LTRP、VRAE、音频腿、配对令牌、宿主连接、
  displayinfoupdates 订阅、HID 附着、offer 原文直发——报什么形状都没用，
  因为一个字节都没落到那个 socket 上。这批臂不是白跑（它们把"请求侧差异"这个变量
  真的钉死了），但它们的**结论理由**要换成这一条。
- **凡是"设备不理我们发的 RTCP"的旧结论都要重读**，尤其是关键帧请求那一串
  （PLI 无反应、FIR 在 `allowRTCPFB=1` 下仍无反应）。**当时的产品后果**：我们根本没有
  可用的关键帧请求路径，序号缺口之后的恢复全靠下一帧 IDR 自己来，而"设备不响应 PLI/FIR"
  不是设备的态度，是我们发包没到。**这一条已经在下一节重测并翻正**：PLI 有效（20~35ms 回
  IDR）。当时 FIR 的负面结果来自非标准包，不能推广到标准 FIR；格式修正后的观察见下文。

### 这一轮已经排除掉的嫌疑（不需要设备也能排除的部分）

按"包为什么没到"能有的四个位置逐个查，其中三个已经在手的数据里排掉了：

| 嫌疑 | 怎么排的 | 结果 |
| --- | --- | --- |
| 目的端口错（发到没人收的端口） | 设备 `udp connect` 那行的四元组 vs 探针打印的 answer：设备绑 `::1:62073` 并 connect 到 `::2:56006`；我们这侧 `connection.sender.port=62073`、自己的收流端口 56006 | **两边逐字一致**，我们发的就是它 connect 的那个元组 |
| IPv6/UDP 头或校验和算错，被内核静默丢 | 拿苹果抓包里那个**设备确实收进了**的 32 字节 RCTL 当金标准，喂我们的 `l4_checksum`（`tests/net_test.cpp`） | 算出 **0xb212**，与抓包字段一致；回验也得 0 —— 封装没错 |
| 隧道不往设备方向投递 | 同一条隧道、同一个 `utun7` 上，我们的 **TCP** 是从我们这边到达设备 socket 的（`dtremotedisplayd{Network}: nw_listener handleInbound ... interface: utun7`） | 隧道至少对 TCP 是双向通的 |
| 源地址/源端口不符 | `UdpSocket::send()` 用 `local_port_` 填源端口，而 `receiver_port()` 返回的就是同一个 `local_port_`，也就是我们写进请求 `receiverPort` 的那个数 | 同源同端口，与 connect 的对端一致 |

还没排掉的：**同一个隧道上客户端→设备的 UDP 到底通不通**。这一条只能问设备，
仪器已经备好（`--udp-canary`：往设备一个确定没人监听的端口打三个包，看它回不回
ICMPv6 端口不可达）。三种结果各有含义，写在探针的注释里。

### 根因的代码位置：`UdpSocket::send()` 把载荷落在了缓冲区尾巴上（已修）

上面那句"还没排掉的"就是最后一步，而答案是**我们自己发的包是坏的**。代码只有一处：

```cpp
std::vector<uint8_t> dgram(kUdpHeaderLen + payload.size());  // 申请 8+N，内容全零
put16(dgram.data() + 4, static_cast<uint16_t>(dgram.size()));  // 长度字段写 8+N
dgram.insert(dgram.end(), payload.begin(), payload.end());     // 却又 append：实际 8+2N
l4_checksum(..., dgram.data(), dgram.size(), ...);             // 校验和按 8+2N 算
```

于是同一时间里发生三件事：长度字段里声明的那 N 字节**全是 0**（真载荷在缓冲区尾巴上，
被长度字段截在外面）、UDP 长度与 IPv6 payload length 不一致、校验和覆盖的字节比内核核对
的多一倍。内核在校验和那一步就丢，而**丢校验和错的 UDP 包是静默的**：不进 PCB、不计
`pkts in`、不回 ICMP。所以设备侧唯一的表现就是"一个都没到"，`so_error 0`、`rxnospace 0`
——那四行日志一字不差地对上了。

修好之后的四组前后对照，每组都带数：

| 观测量 | 修之前 | 修之后 |
| --- | --- | --- |
| 设备 socket `pkts in`（20 秒租期，RR 1Hz） | **0** | **41**（正好 1Hz × 40 秒） |
| 会话寿命（租期 20） | +19.97s / +19.99s 死 | **40.2s 还在**，两轮都是 |
| `streamDidRTCPTimeOut` 触发次数 | 每轮 1 次 | **0 次** |
| UDP 金丝雀（打给设备一个确定没人听的端口） | 三条全哑 | **三条全收到 `type=1 code=4` 端口不可达**，回带的内层四元组就是我们的发包 |

还有两条当时用来"缩小范围"的读法，现在要更正：

- **ICMPv6 回音 5/5 有应答**这件事当时被我读成"隧道投递非 TCP 流量，所以问题只能在我们
  包的内容之外"。它本身没错（也确实排除了"隧道不投递"），但它**没有**排掉"我们的 UDP 包
  自己坏"这一项——而 ping 走的是另一条拼装代码（`Stack::send_echo_request()`，那一条是对的）。
  **教训：拿两条不同代码路径的观测去推断网络行为，等于什么都没推断。**
- **"p3（另一个独立实现）也是 `pkts in: 0`，所以不是我们的包"**——这条推定也作废了。
  p3 有它自己的包拼装，它失败只能说明"这条路很难走通"，不能给我们的字节清白作证。
  我当时用它把"封装没错"从推测升级成了证据，那是过头的。
- `--flow-label`（非零 IPv6 流标签，苹果实测 0xd0d00）判掉：打了非零标签后设备侧照旧
  `pkts in: 0`、照旧 20 秒死。

金标准那条测（`tests/net_test.cpp` 里拿苹果真人包验 `l4_checksum`）**当时是必要但不充分的**：
它验的是那个函数，而产品代码里真正上线的是 `UdpSocket::send()` 的缓冲区。现在同一条向量
改走产品代码的拼装，并要求**逐字节相等**（含 0xb212），另加一组回归测直接钉那三点
（长度 = 8 + 载荷、载荷紧跟在头后、按内核会核对的那段字节自校验得 0）。

### UDP 修复后的反馈对照

> **2026-10-07 更正**：此节保留 UDP 修复后实验的原始数值，但旧 FIR 构造器生成的是
> 24 字节、length=5 的非标准包，并将请求序号写入公共媒体 SSRC，FCI 序号实际为零。
> 因此撤回“FIR 有害”和“永远不要发 FIR”的结论；负面结果仅描述这个旧包。
> RR 与 PLI 的既有验证仍保留。标准 FIR 的布局、快速复测与未完成的对照见下一节。

包能到了，才第一次问得出真问题。四臂对照（`--timeout 20 --hz 1 --seconds 30`，每臂两轮，
`--request-always`）：

| 臂 | 发什么 | 换到的 IDR | 结局 |
| --- | --- | --- | --- |
| `rrsrc` | RR 1/s | **1**（只有起流那一个） | 活 2/2 |
| `pli` | RR + PLI 1/s | **28~29** | 活 2/2 |
| `fir` | RR + 旧非标准 FIR 1/s | 1 | **死 0/2，+19.99s** |
| `fir+fb` | 同上 + offer `allowRTCPFB=1` | 1 | 死 0/2 |
| `rrsrc+fb` | RR + fb 位 | 1 | 活 2/2 |

1. **租期是可续的**：`RTCPTimeoutInterval` 是"距离上次收到我们 RTCP 多久"的空闲计时器，
   1Hz 的 RR 就能续住。所以之前那句"设备不看我们发什么"整个作废。
2. **PLI 管用**：29 次请求换 28~29 个 IDR，一对一；单独量延迟是 **+19ms / +35ms**
   （起流后 3 秒内的请求不计延迟，否则会话开头那个 IDR 会被算成功劳——真出过 "+1ms"
   这种不可能的数）。参考实现笔记里"the device ignores RTCP PLI for refresh"**作废**。
3. **旧非标准 FIR 未产生额外 IDR，且该臂没有续期**：设备侧记录为 `pkts in: 40`、
   `Last RTCP packet receive time:nan`，20.13s 结束。包已经投递不等于格式有效；
   不能从这些观测推出标准 FIR 会破坏 RTCP 解析或应永久禁用。
4. 切换 `allowRTCPFB` 未改变这组 RR 和旧非标准 FIR 的结果；这不证明它是标准 FIR
   的受理开关，也不能据此确定该字段在其他设备或协议版本上的含义。

判 IDR 的仪器本身也错过一次，值得记：第一版在 RTP 载荷头两字节读 NAL type（按 RFC 7798
认聚合 48 / 分片 62），跑出"30 秒 18000 个包、IDR 0 个"——连起流必然存在的那个关键帧都没认
出来。真相是**这条流的分片包类型是 49 不是 62**（`HevcRtpDepacketizer` 里本来就带着这份实测），
改成复用拆包器扫它吐出的 Annex-B 之后，基线一臂读出 1 个、PLI 臂读出 28 个。一个"设备不发
关键帧"的结论差点由读错载荷类型读出来。

产品后果（都已接进 `media/FramePump`）：租期从"绕不开的一小时"回落到 **20 秒 + 每秒一个
RR**（实测产品二进制跑满 74 秒、60fps、零重起，设备侧 `pkts in: 75`、`streamDidRTCPTimeOut`
0 次）；丢帧之后先发 **PLI** 而不是重起会话（实测注入一个真实丢包：`序号缺口 1 PLI 1
等关键帧丢 3 重起 0`，代价从约 300ms 无帧降到冻 3 帧约 50ms）。"卡住就重起"那条判据
必须加"已经发过 PLI 且等满了 `stall_restart_ms`"这个条件，否则它永远抢在 PLI 前面触发——
因为这条流 30 秒才自发一个 IDR，"距上一个关键帧超过 2 秒"几乎恒真。

### 标准 FIR 的修正与复测（2026-10-07）

按照 [RFC 5104 §4.3.1.1](https://datatracker.ietf.org/doc/html/rfc5104#section-4.3.1.1)
修正公共构造器：单项 FCI 包为 20 字节，length=4，公共媒体 SSRC 为零，目标 SSRC
在偏移 12，请求序号在偏移 16，随后为 24 位保留零。请求序号由调用方按模 256 递增，
重发同一请求保持序号。旧代码对这些标准字节判据有 13 项失败；修正后 RTP 全部 111 条通过。
`fir_probe` 也复用公共构造器，`fir205` 仅作为修改 PT 的非标准实验保留，不再称为标准 FIR。

同一台 iPhone 14,4、iOS 27、macOS USB 上先做一次快速观察：

```sh
rr_keepalive_probe --what fir --seconds 30 --attempts 1 --timeout 20 --hz 1 --request-always
```

`allowRTCPFB=0`，发送 29 次标准 FIR，窗口内观察到 26 个 IDR，首次参与延迟计时的请求后
32ms 收到 IDR；最后视频包在 +29999ms，设备会话在 30 秒结束时仍存在，跨过协商的
20 秒租期。同时发送 RR，因此不能归因于 FIR 单独保活；这轮仍使用探针旧 RR 发送者配置，
也没有同轮 none/PLI 对照，不据此比较两种请求的优劣。

随后补做基础对照，每轮观察 30 秒，共两轮：

```sh
rr_keepalive_probe --what none,rrsrc --seconds 30 --attempts 2 --timeout 20 --hz 1 --request-always
```

| 臂 | IDR 数（两轮） | 最后视频包 | 30 秒结束时的设备会话 |
| --- | --- | --- | --- |
| `none` | 1 / 1 | +20004ms / +19997ms | 均已消失，0/2 存活 |
| `rrsrc` | 1 / 1 | +29960ms / +29990ms | 均存在，2/2 存活 |

后续将 PLI / FIR 臂的 RR 发送者、报告块及目的端口统一为与 rrsrc 相同的协商
RemoteSSRC / LocalSSRC / SourcePort，再尝试 PLI、FIR、FIR+fb 两轮比较。
第一轮 PLI 在 +7011ms 停止收包，随后出现隧道发送和绑定失败；宿主机 USB 设备枚举为空。
后续五次起流均失败；用户随后确认已拔掉 USB，因此这组比较未完成，不将该中断解释为
PLI 或 FIR 的协议行为。

当前证据已经否定“标准 FIR 必然有害”的旧判断，并支持已测设备在默认反馈位下响应标准
FIR 的观察。产品继续使用经过恢复验证的 RR / PLI；同配置 PLI / FIR 的重复比较、
FIR+fb、真实丢包恢复及其他设备版本仍待验证。



**剩下的字节级差异只有一处**，在 RCTL 那个字段上。下表是拿苹果那 1469 个 RCTL 和它当时刚收到
的视频包逐个对齐出来的，不是推的：

| RCTL 字段 | 苹果实测 | 我们 |
| --- | --- | --- |
| w4 = `(最后一个视频包 RTP 时间戳 >> 8) << 16` | 2002>>8=7 → `0x00070000`，逐个对上 | **一致** |
| w5 = 上一帧的包数 | 6/6/0/14/16…（全场范围 0..60） | **一致** |
| w6 高 16 = 墙钟 | **1024Hz 单调计数**，全场斜率实测 1024.0/s，16 位会回绕 | `ts/24` = 1000Hz 毫秒，20 秒差约 480ms |
| w6 低 16 | 0..255 的小数，非单调（抖动/时延一类，语义未定） | 恒 0 |
| w7 高 16 = 累计收包数 | 72 / 689 / 1515 / 2756，与当时实收包数对得上 | **一致** |
| w7 低 16 | **主值就是 60001**：分布 `{60001: 1075, 60000: 355, 0: 39}` | `0xEA61`=60001，**一致** |
| 伴随包（PT=204、name=`00 00 00 05`）那个字 | 就是刚收到的 RTP 时间戳，差值 0 | **一致** |
| 伴随包速率 | 每帧一个：`marker=1` 的包 3061 个 vs 伴随包 3048 个 | **一致** |

（这里要记一次方法上的错：我最初只看前 12 个 RCTL 的 w7 低 16 全是 0，就断言"苹果发 0、
`0xEA61` 是 p3 的私货"。把 1469 个全查一遍，主值恰恰是 60001。**判据要按全场分布，不能按前
N 个样本**——这条已经写在 `feedback-first-hand-evidence` 里，当场又犯了一次，所以留在这儿。）

> **下面继续保留早期排错记录**：后续长租期、定时接续和反馈矩阵段落不是当前生产方案，请对照 [UDP 修复后的反馈对照](#udp-修复后的反馈对照)。
> 其中“displayinfoupdates 尚未实现”也是当时状态，后续验证见
> [显示几何订阅](#16-displayinfoupdates设备自己报的显示几何实测iphone144--ios-270--usb)。

另外更正一条被误读的抓包现象：整场唯一那条带 `XPCSideChannel.uniqueIdentifier` +
`sideChannelStatus` 的推送（`{pushing:{elements:[方向 / primary LCD 1080x2340 / 6 个 wireless
虚拟显示器 / backlightState:activeOn]}}`）落在**设备端口 54583 = `com.apple.coredevice.deviceinfo`**
上，而它的 feature 列表里有 `com.apple.coredevice.feature.displayinfoupdates`。那是
**显示器与方向状态订阅**，不是媒体会话的事件通道，和租期无关——"设备反过来拨客户端来续命"
这个想法到此为止。顺带记下：`displayinfoupdates` 是我们还没实现的 feature，转屏时它会主动推，
以后窗口自适应/`--stats` 可以用它，不必轮询 `getdisplayinfo`。

**两个方法论账，都要记**：

1. **后台开着的 DeviceHub 会抢那唯一的槽**，而它抢走之后我们的会话"提前死"看起来和租期
   一模一样。这一轮里 `+16.99s 死` 的那一臂事后确认是被抢的，全部重测才拿到干净的 20.0s。
   判据补进了探针：收不到包的那一刻把表里每条会话的身份打出来（是不是我们的 uuid、
   `type` 在不在、活了多久），"到点死"和"被顶掉"从此当场可分（`dump_sessions()`）。
2. **我自己引入过一次干扰**：拿一条已被设备关掉的连接去发探测，那一臂死在 **+10.4 秒**
   而不是 20 秒——探测本身改变了被测对象。所以那个"每 10 秒探一次连接"的代码没有留下来。
   （同一个坑的第三种形态：`lsof` 在我们的架构上**看不见**连接，因为 TCP/IP 栈是用户态的；
   "探针看不见"不等于"没发生"。）

**这批 8 秒实验里有一处没解释的现象，记下来别丢**：修好"发送节奏"之后重跑同样三臂
（`--hz 1/10/25`），每一臂都在**起流后约 1.1 秒**就视频档和 SR 档一起停掉（344 个视频包、
1 个 SR），而不是请求里那个 8 秒；同一晚紧接着的另外两次长窗口实验（40 秒、45 秒，包括
一次"静置 150 秒不碰手机再起流"）都是正常的 1/s SR 一路不断。也就是说这个 1.1 秒静默
**既不是租期到点**（8 秒/3600 秒都不符），也不是产品路径的常态——产品那一侧同一晚两次
长跑（75 秒、40 秒）都是 0 重起、SR 每秒一个。原因没查出来，怀疑与"设备侧编码器/显示
管线在某种闲置状态下暂停推流"有关，但没有证据。**它不影响上面那张表的结论**：那一轮四臂
（对照 + 三种 RTCP）彼此一致，都是死在自己请求的那个数上。下次再遇到 1.1 秒静默，先看
`发出 RTCP` 那一列和 `status.connectionErrors`，再决定要不要怀疑设备。

**后记（同一晚晚些时候）**：这个"视频档和 SR 档同时停、而会话表里已经没有了"的形状，在
上面那条 C 臂里被**稳定复现**了两次（+241ms、+242ms），成因明确——是我们自己在起流那条连接上
又发了一次请求。但**不要把两者当成同一件事**：那批 `--hz` 臂没有握连接、也没有在同一条连接上
发第二次请求，走的是"每次调用新开一条"的老路，所以 1.1 秒静默的成因**仍然没解释**，只是从
"完全没见过"变成了"见过一个能产生同样形状的操作"。要判是不是同一个机制，办法是在探针里把
"起流之后到静默这段时间内，我们在这条会话上发过的每一个请求"都打时间戳——现在还没有这个记录。

顺带记一个还没用上的字段：`status.connectionErrors`。它的语义没查（跑动图 + 长会话时它
一直是 0），但它是目前唯一"设备自己承认状态有问题"的出口，将来要区分"我们没收齐"和"设备
发送侧出错"时可以从这里找。

> **历史恢复判据，已更新**：下面仅靠序号缺口与关键帧年龄直接重起的方案不是当前流程。
> 当前恢复先发 PLI 并等待有效关键帧，后续恢复边界修正见
> [§20.3](#203-第二条-p1关键帧没解出来就清了恢复状态判据本身先要能判别)；反馈依据见 [UDP 修复后的反馈对照](#udp-修复后的反馈对照)。

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

> **符号推断已更新**：下面的框架符号本身保留，但“DeviceHub 可能长租期或静默重起”不是当前已知解释。
> 后续抓包确认它报短 timeout，UDP 修复后的对照确认有效 RR 可续期，见 [UDP 修复后的反馈对照](#udp-修复后的反馈对照)。

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

> **历史定时接续方案，已退出产品路径**：下列阈值及随后改为一小时的方案均保留为开发过程记录。
> 当前会话用 RR 维持 RTCP 空闲计时器，不按请求 timeout 周期更换会话，见 [UDP 修复后的反馈对照](#udp-修复后的反馈对照)。

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

> **两会话实验的范围**：下面是两条视频流的替换对照，不能推广为设备只允许一条任意媒体流。
> 视频与音频可共存；视频重起对在场音频的影响另有独立验证，见 [音视频并行与独立续期验证](#172-音频腿接进产品四个问题的真机读数toolsaudio_pump_probeasan-下跑)。

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

> **历史 RTCP 否定结论，已作废**：下面的反馈实验发生在 UDP 发送修复之前。
> 包格式修正不等于包已到达；当前 RR 可续期、PLI 可恢复的依据见 [UDP 修复后的反馈对照](#udp-修复后的反馈对照)。

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

> **反馈假设与标准判断更新**：下文把 allowRTCPFB 当作 FIR 受理闸，是尚未成立的参考推测；
> 已测默认 false 下 PLI 有效，切换此位没有改变 RR 续期和旧非标准 FIR 失败的结果，见 [UDP 修复后的反馈对照](#udp-修复后的反馈对照)。
> 原文“标准 FIR 是 RTPFB=205”的判断也有误：标准 FIR 属于 PSFB=206、FMT=4，见
> [RFC 5104 §4.3、§4.3.1](https://datatracker.ietf.org/doc/html/rfc5104#section-4.3)。原矩阵数值保留，不能作为当前反馈行为结论。

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

> **下句结论已作废**：关键帧反馈与会话续期仍是不同用途，但合法 RR 会影响 RTCP 空闲超时。
> timeout 取值本身不能推出“所有反馈都不影响断流”，见 [UDP 修复后的反馈对照](#udp-修复后的反馈对照)。

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

## 16. `displayinfoupdates`：设备自己报的显示几何（实测，iPhone14,4 / iOS 27.0 / USB）

镜像的可见区尺寸此前是**反推**的：编码帧 1136x2464 比真正的显示区大一圈（HEVC 按 CU
对齐的填充），而这一圈多大协议里没有。我们只按机型量过一档并硬编码进
`media::display_crop`，于是表外的机型就把整幅编码帧当可见区——后果不只是边缘一条噪声边，
**触摸分母跟着错**（HID 报告的 0..1 是相对可见区的）。

`com.apple.coredevice.feature.displayinfoupdates`（服务
`com.apple.coredevice.deviceinfo`，流式）给的是权威值。整条推送（`display_info_probe`
打下来的，字段名照原样）：

```text
{current: true,
 backlightState: "activeOn",
 orientation: {currentDeviceOrientationLocked: false,
               currentDeviceNonFlatOrientation: "portrait",
               currentDeviceOrientation: "portrait"},
 displays: [
   {displayId: 1, primary: true, external: false, name: "LCD", deviceName: "primary",
    currentMode: {size: [1125, 2436], preferredUIScale: 3, refreshRate: 60,
                  hdrMode: "standard", bitDepth: 8, colorGamut: "displayP3"},
    nativeSize: [1080, 2340], bounds: [[0,0],[1125,2436]], frame: [[0,0],[1080,2340]],
    physicalSize: [2.26891, 4.91597],            // 英寸
    logicalScale: [1, 1], pointScale: 3,
    nativeOrientation: "rot0", currentOrientation: "rot0",
    type: {integrated: {}}, chromeIdentifier: "com.apple.dt.devicekit.chrome.phone3",
    framebufferMaskIdentifier: "<UUID>", availableModes: [ 同上那一条 ]},
   {displayId: 2, primary: false, external: true, name: "Wireless", deviceName: "wireless0",
    nativeSize: [1136, 2448], currentMode: {size: [1136, 2448], pointScale 侧是 1, ...}},
   {displayId: 3..6, name: "Wireless-1..4", 尺寸全零},   // 在册但没插
 ]}
```

三个必须记住的点：

1. **可见区取 `currentMode.size`，不是 `nativeSize`。** 这台设备上它们是两组数
   （1125x2436 与 1080x2340），拿错的那一版会把画面裁掉一条真内容。
2. **几何字段的 XPC 类型是 Double**，而 `displayId` 是 UInt64、`preferredUIScale` 与
   `bitDepth` 是 Int64、`refreshRate` 是 Double。第一版解析器只认两种整数，于是
   `currentMode.size` 一个都取不到，表现为"问到设备了却拿到 0x0、退回兜底表"，
   而日志上完全看不出来——因为 `describe()` 打出来 `1125` 与 `1125.0` 长得一模一样。
   类型是用探针把每个叶子的 XPC 类型打出来才看到的（`display_info_probe` 第一段输出）。
   **教训：类型要问机器，不能看打印出来的样子猜。**
3. **它是"订阅即给现状"，不是周期报。** 实测订阅后 +3ms 就推第一条，之后 21 秒内
   一条都没有，直到我们超时；期间不转屏、不锁屏。所以产品路径分了两层：起流之前
   用一次性订阅问现状（`fetch_display_info`），跑起来之后由 `DisplayWatcher` 挂着
   一条常驻订阅跟着转屏改朝向。常驻这条必须独占一个连接
   （`Channel::take_message` 不看消息 id 与 flags，一条连接上挂两个要回信的请求
   会互相拿错），这也是 `ServiceConnection` 要把 `stream()` 拆成
   `subscribe()` + `next_batch()` 的原因：整块阻塞的循环没法定时回头看一眼该不该停。

时序与推送次数的判据来自同一个探针：`共收到 1 次推送，重订 1 次`。外接那几块
`Wireless-1..4` 尺寸全零，所以 `find(displayId)` 命中之后还得看宽高是否大于 0，
否则"找到了"不等于"能用"——产品路径在宽高有一个为零时照样退回兜底表并打一行说明。

## 16.1 界面旋转时到底什么在变（实测，同一台设备，横屏全屏播放器）

转屏适配要用的三条事实，全部是在设备**正处于横屏全屏**时量出来的：

1. **编码帧不跟着换向。** 界面报 `rot270` 时码流仍是 1136x2464 竖幅，画面内容躺在
   里面。所以"转正"是客户端的事，不是重新起流的事。
2. **可见区尺寸也不跟着换向。** 同一条推送里 `currentMode.size` 还是 `[1125, 2436]`、
   `bounds` 还是 `[[0,0],[1125,2436]]`——只有 `currentOrientation` 从 `rot0` 变成
   `rot270`。所以裁剪框不需要按旋转对调，需要换向的只有**窗口/视口**那一侧。
   （截图服务给的是已经转正的 2436x1125，别拿它当"设备报的尺寸会换向"的证据：
   那是那个服务自己转的。）
3. **`orientation.currentDeviceOrientation` 与 `currentOrientation` 是两回事。** 这一次
   前者是 `portrait`（手机物理上竖着拿），后者是 `rot270`（App 界面横着画）。
   只盯其中一个就会在"手机竖着但 App 横屏"这一档完全对不上——而这一档恰恰最常见。

**转正方向是量出来的**：`rotN` 的意思是把面板那一帧**顺时针转 N 度**得到正立画面。
对照方法是同一屏取两份图（我们收到的面板帧、与截图服务给的已转正 PNG），
再用 `ffmpeg -vf transpose` 试出哪一档对得上。

**触摸的逆映射也在真机上验过**，判据不是"看着像"而是量出来的：在横屏全屏的播放器里
注入一次拖拽，起点/终点取视口（转正后的横屏图）上的 (0.30, 0.7964) → (0.70, 0.7964)，
也就是进度条那一行；按 `rot270` 的逆变换算成面板坐标是 (0.2036, 0.30) → (0.2036, 0.70)，
即面板上的一条**竖直**拖拽。结果设备真的把进度拖走了，而且：

- 进度条响应本身就否证了"不用换轴"：不换算的话这条拖拽落在视口左侧 20% 处、
  从 30% 高度划到 70%，够不到 y≈79.6% 的那条线。
- 播放头从 18.3% 走到 63.6%，方向与前向映射 `视口x = 面板y` 一致；若是镜像
  （`视口x = 1-面板y`）会退到 26% 一侧。
- 播放头的像素位置与时间标签**互相印证**：量得 63.65%，标签 13:34/21:19 = 63.64%。
  轨道端点 186..2249 也是这么定出来的（静止时 3:54 对应播放头 18.27%，与 234/1279
  = 18.29% 吻合）。

**常驻订阅真的会跟着转屏推第二条约 1 秒内到。** 判据是在同一个进程里既挂着
`DisplayWatcher` 又注入一次触摸（同屏两点：`--tap 5:0.5,0.5 --tap 7:0.2889,0.8999`，
第一次把自动收起的工具条叫出来，第二次点"退出全屏"）：

```text
  +0s 推送 #1：1125x2436 rot270
  +5s 注入触摸 (0.500,0.500): 已发
  +7s 注入触摸 (0.289,0.900): 已发
  +8s 推送 #2：1125x2436 rot0
```

这一次同时钉住了三件事：订阅是活的（不是一次性的）、推的延迟是秒级以内、
以及**退出全屏这个动作确实由那次换算出来的触摸完成**——它是第一个不依赖
"看着像"的位置证据。顺带一条：截图服务给的尺寸也跟着界面旋转
（横屏时 2436x1125，退出全屏后 1125x2436），所以拿它当"设备报的尺寸"是错的，
它给的是转正之后的图。

一个没验成的部分，记在这里免得下次重复踩：**这个播放器对进度条的"点按"不响应**，
只认拖拽。所以第一次拿单点去试点赞按钮与进度条都是空结果——那不是映射错了，
是判据选错了。同理，"点赞图标没变红"那次也不能当反证：注入发生在起流之后约 25 秒，
播放器的工具条那时多半已经自动收起，点在任何位置都只是把工具条叫出来（回读到的
截图正是"工具条在、心形仍是轮廓"）。**要判触摸位置，得选一个不依赖临时 UI 的判据。** 上面那次成功的退出全屏正是照这条
做的：先补一次触摸把工具条叫出来，再点目标。**

顺带纠正两处旧措辞。§6 与 §11 把 1125x2436 叫"逻辑显示"，那是从代码字段名带出来的；
设备自己的说法是**当前模式**（`currentMode.size`）的尺寸，而同一块屏另有一个
`nativeSize`（这台设备 1080x2340），两者不是一回事。另外 §6 那条写的是"`get-display-info`
报 1125x2436"——那是立项早期从 lockdown 那侧问的，与我们现在这条 CoreDevice feature
**是两个不同的服务给了同一个数**，所以这一档的 1125x2436 有两处独立来源。
## 17. 音频腿的编码鉴定（实测，iPhone14,4 / iOS 27.0 / USB）

**结论：`PT=101` 上跑的是裸 AAC-ELD（Apple 的 `'aace'`），48kHz 立体声，每帧 480 采样
（10ms），一包一帧，没有 RFC 3640 的 AU 头。** 两条独立证据：

1. 设备自己的音频日志（`pymobiledevice3 syslog live` 免 root 抓到，边收音频腿边录）：

```text
avconferenced{AudioCodecs}: ACMP4AACBaseEncoder.cpp:314
    Output format: 2 ch, 48000 Hz, aace (0x00000000) 0 bits/channel, 0 bytes/packet,
                   480 frames/packet, 0 bytes/frame
avconferenced{AudioCodecs}: ACMP4AACBaseEncoder.cpp:733
    @@@@ 'aace' encoder configuration: srIn = 48000, srOut = 48000, chans = 2,
                                       bitRateFormat = 1, bitrate = 128000
avconferenced{AVConference}: VCAudioStream setupPayloads:786
    currentAudioPayload={ <VCAudioPayload> config=VCAudioPayloadConfig payload=101
      blockSize=480 codecSampleRate=48000 codecSamplesPerFrame=480
      inputSampleRate=48000 inputSamplesPerFrame=480 isDTXEnabled=0 octedAligned=1
      useSBR=0 internalBundleFactor=1 initialBitrate=32000 maxBundleFactor=1 ... }
```

   `payload=101 / blockSize=480 / codecSampleRate=48000` 与我们收到的完全一致，
   `maxBundleFactor=1` 就是"一包一帧"，`useSBR=0` 说明是 ELD 而不是 ELD-SBR。
   四字符码 `aace` 在 Apple 的 AudioFormat 定义里就是 `kAudioFormatMPEG4AAC_ELD`
   （带 SBR 的是 `aacf`）。

2. 字节结构自己也能对上：ELD 帧的第一个比特是 `raw_data_block_flag`，为 0 表示
   "这帧没有数据"。实测静音时**每个包都是 4 字节且首字节 0x00**（flag=0），出声时
   首字节变成 0x88/0x89/0x8A（flag=1）——静音/有声的分界恰好落在这一个比特上。

配套的线上读数（`rr_keepalive_probe --audio-leg --audio-out`）：

| 状态 | 包数/秒 | 载荷 | 时间戳步长 |
| --- | --- | --- | --- |
| 设备静音 | ~101 | 恒定 4 字节 `00 68 34 00` | 480 |
| 放视频（有声） | ~68（受探针排水上限影响，非设备速率） | 243–400 字节，中位 369 | 480 |

**这一节推翻上一版探针 commit（49d28f8）里那句"还认不出编码"**：那时手上只有静音包，
而静音包对 AAC-ELD 和 Opus 两种假设都自洽（我甚至按 Opus 的 TOC 位解释过 `0x89`，
纯好看不像话）。真正给出答案的是**设备日志 + 有声时的字节**，不是对着静音包猜。
教训照旧：认格式要有内容的那一段数据，加上一个会自己交代名字的来源。

> **解码方案已更新**：下面“libav 原生 aac 加 extradata 就能解”的写法是实际后端验证前的推断。
> 同一份设备载荷的后续对照没有支持它；当前 AudioToolbox 以协商的 ASBD 配置解码，未设置 magic cookie，见 [同一份 dump 的后端对照](#171-音频解码后端只能选-audiotoolbox实测同一份-dump-四路对照)。

对实现的直接含义：解码要喂 `AVAudioCodecDescription` 里的 **AudioSpecificConfig**
（AOT 39 = AAC-ELD v2，48kHz，2ch，frameLengthFlag=1 即 480），因为包里没有 AU 头也没有
ADTS；libavcodec 的 `aac` 解码器带 extradata 就能解 ELD。静音时设备**照样 100 包/秒地发**
（`isDTXEnabled=0`），所以"没声音"不会表现为断流。

## 17.1 音频解码后端只能选 AudioToolbox（实测，同一份 dump 四路对照）

> **后端结论的范围**：本标题及下文“非 Apple 平台没有音频”描述本项目已验证、已接入的后端。
> 原生 FFmpeg 在这份设备配置上失败，不能据此推断所有版本或所有平台都没有 AAC-ELD 实现。
> 当前仅接入 macOS AudioToolbox；FDK AAC 等候选仍需用同一份载荷验证，计划见 [ROADMAP](ROADMAP.md)。

编码定下来是 AAC-ELD 之后，下一个问题不是"怎么接"而是"**有没有东西能解它**"。
判据是同一份真机 dump（1406 个 PT=101 的包，246~400 字节一个，14.06 秒）分别喂给
四个候选：

| 后端 | 结果 |
| --- | --- |
| libav 原生 `aac` + 按规范拼的 ASC `F8 E6 28` | **连 `avcodec_open2` 都过不去**："AAC data resilience (flags 4) is not implemented" |
| libav 原生 `aac` + 苹果自己那份 cookie `F8 E6 40 00` | 打得开，1406 帧只出得来 **109** 帧，每帧 **512** 采样（不是 480），峰值顶满 32768 —— 是解歪了的样子不是解错了几个字节 |
| libav 的 AudioToolbox 壳 `aac_at` | 同一条 dump 同样只有 **109/1406** |
| 直接对 AudioToolbox 的 `AudioConverter` | **1406/1406**，每帧正好 480 采样/声道，峰值 20434 |

第四行与参考实现（pymobiledevice3 用 ctypes 直调 AudioToolbox）逐项相同：样本总数
1349760、时长 14.06 秒、峰值 20434、四段峰值 17715/20434/17499/18250。两条独立实现
给出同一串数字，才敢说这不是"我们这边凑巧对上"。

顺带把 ffmpeg 自己的话也记下来：`-c:a aac -profile:a aac_eld` 直接回
**"Profile not supported"**——它的原生编码器都不产 ELD，解码器更不是。

（2026-09-27 在 ffmpeg 9.0.2 上复验过一次，结论没变但报错内容更具体：`-decoders` 里
只有 `aac / aac_fixed / aac_at / aac_latm`，**没有任何 ELD 解码器**；把 1014 个 PT=101
的裸载荷按顺序拼成一个文件喂 `-c:a aac`，回的是 "Number of bands (43) exceeds limit
(27)"、"channel element 1.1 is not allocated"，解码错误率 0.990099 后直接判死。前一条
正是 ELD 的带宽扩展结构与 LC 的表不一致的形状，不是字节错位。）

**AudioConverter 不需要 magic cookie。** 这一点值得单独记，因为它和"规范怎么说"相反：
`AudioConverterSetProperty(conv, 'dmgc', ...)` 在这台 macOS 上无论塞规范拼的 ASC 还是
塞苹果那份 cookie 都回 `!dat`（0x21646174），**而不塞照样全解出来**。ELD 的档位信息
其实在 ASBD 里就齐了——`mFormatID='aace'` + `mFramesPerPacket=480`（1024 才是 LC）+
`mBytesPerPacket=0`（变长）。所以代码里根本不去设那一位：设了只会把一个无关紧要的
失败变成"看起来像初始化没成功"。

（参考实现那边也没设成功——它同样忽略 `SetProperty` 的返回值。这条只有把返回值打出来
才看得见，而我们是因为先当成致命错误才去打的。）

**后果要说白：非 Apple 平台没有音频。** 这不是"还没做完"，是这条码流在那些平台上
没有能解它的自由实现（fdk-aac 能解但许可证不是自由的，不列进来）。所以
`decode/AudioDecoder.h` 上有一个编译期常量 `kHaveAudioDecoder`，产品路径必须在
**起流之前**问一句并说人话，而不是等第一帧音频到达时给一个空指针。

## 17.2 音频腿接进产品：四个问题的真机读数（`tools/audio_pump_probe`，ASan 下跑）

编码器定了之后，剩下的四件事都只能问设备，而每一件都决定 `media/AudioPump` 里一段
逻辑的写法。一次跑 24 秒两条腿，读数如下。

**① 音频腿不跟视频腿共用 `ClientSessionID` 也起得来**（收流端口各自一个，PT=101）。
产品**故意分开**，虽然苹果是两条腿同一个 UUID：判活用的 `getmediastreamserverstatus`
是按 ClientSessionID 查的，共用时"查得到"只等于**至少有一条腿还活着**，于是视频腿
"设备已结束这条流"的判据会被音频腿掩护掉——画面冻住而泵以为流好着。租期靠每条腿
各自的 RR 已经能续住（③），不需要用共换来换。

**② 内容静音的时候设备照发包**：有一整段 28 秒里每个解出来的帧都是**全零**，而包
仍然按每秒 100.5 个（一帧 10ms）一个不停地到。所以"多久没收到包"在音频这条腿上
**就是**死因，不需要像视频那样先分辨"画面静止"与"流死了"。

这一臂差点被我自己写成一个没测过的结论：第一版读数（24 秒、峰值 2 万左右）是**有声
内容**在发 100 包/秒，而"没声音时还发不发"根本没测。补了 `--mute` 臂才拿到上面那句。

**而 `--mute` 那一臂按的音量键其实管不着这条流**（同一臂顺手量出来的，是个产品级
事实）：把设备媒体音量按到 0 之后再跑，解出来的帧峰值仍然是 12780 / 22097 这个量级。
也就是说 `source: {audioSystemOutput: {}}` 是**音量之前**的抽头，和 AirPlay 一样。
所以那 28 秒的全零不是按键按出来的，是内容自己静了下去（应用正好在两个视频之间）——
结论不受影响，因为要问的就是"承载的声音是零时包还来不来"。

**对使用的影响要说白**：在手机上按音量键**不能**把镜像的声音关掉，反过来手机静音了
镜像里也照样有声。要在电脑上不出声只有两条路：本机输出音量，或者
`--no-audio-playback`（收与解照跑，只是不放）。

**③ 每秒一个 RR 也续得住音频这条会话**：报 20 秒租期、跑 28 秒、`重起=0`。它是
**独立会话独立计时器**，所以泵必须自己发 RR——搭视频腿那份的便车这个假设不成立，
也没打算赌。

**④ 两条腿共存互不顶替**：同一场里视频腿 10593 包 / 928 AU、两边都 `重起=0`。
"一台设备只容一条流"那条实测（§13）说的是**同一种**流，音频腿不占视频那一格。

顺手两条踩坑，都是"账面数字很响而现实没事"的形状：

- **序号记账必须排在 payload type 过滤之后。** 先写在前面时，同端口来的 RTCP SR 被
  当成 RTP 记进了序号序列（RTCP 头第 3-4 字节是长度不是序号），每个 SR 造两次
  "缺口"：实测每 2 秒报 4 次、按 mod 2^16 解释成"往前跳"之后累计**真丢 44816 个包**，
  而这条流实际一个都没少。
- **`AudioPump::read()` 的单位是帧、缓冲区容量是采样**，探针把 960 个采样当 960 帧
  传进去，就往 1920 字节的缓冲后面写了 3840 字节。现场不是探针报错，是几百毫秒后
  **另一个线程崩在 `malloc` 里**（exit 133/SIGTRAP，栈指向 AudioToolbox 解码器）。
  ASan 一跑就落在 `read()` 第 145 行。

**⑤ 视频腿每一次重起都会把音频会话一起带走**，而这不是 `stopAll` 的锅。

推理链值得完整留着，因为它的前半段是**对的**、结论是**错的**：`stopmediastream` 只有
`stopAll: true` 一种形状（本节上面），它停的是设备上所有会话，所以 `FramePump::restart()`
每重起一次视频就把音频杀一次。看着像修法是"那一下 stop 本来就多余——第二条
`startmediastream` 自己会把第一条顶掉（§13 的 `two_session_probe`），去掉它就行"。

`--revideo-at N` 就是去验这个的：第 N 秒**一个 stop 都不发**，直接起第二条视频会话。
两臂对照：

| 在场会话 | 动作 | 音频腿 |
| --- | --- | --- |
| 只有音频 | 起一条视频 | 两条都活 18 秒，最长包静默 268ms，零重起 |
| 音频 + 视频 | 再起第二条视频 | 音频断（400ms 静默后判死重起），**旧那条视频也断** |

所以设备侧的会话表不是"按类型各留一格"，一条新流进来会连带影响另一条腿，与我们发不
发 stop 无关。**对策只能是发现得快**：`kQuietProbeMs` 从 2000 降到 400（一包 10ms、
一秒 100 包，400ms 已经不是抖动），实测掉声从约 2.4 秒缩到 **672~689ms**（两臂各一次）。
视频腿自己那一侧的回流是 1.2 秒量级，所以改完之后音频不再是那条更长的尾巴。

**⑥ 环里囤着的东西不会自己排掉，而那就是一个永久性的音画不同步。**

生产与消费的标称速率相等（都是 48kHz），所以缓冲水位是个**只进不退**的量：开局那一段
"音频腿已经起了、声卡还没开"（产品里隔着 `SDL_Init` 与等第一帧）攒下的 100~300ms 会
一路留着，镜像里的声音就比画面晚这么多，而且只会更糟（两台机器的时钟差着几十 ppm，
每小时再涨约 0.5 秒，到 0.5 秒的环顶之后开始丢旧帧）。

修法是在取的时候做**水位导向**，分两档（理由不同，别合成一档）：

| 水位 | 动作 | 为什么 |
| --- | --- | --- |
| 超过两倍目标 | 一次砍回目标 | 囤着的是"没人听过"的旧内容，整段丢掉是对的；慢慢调速要二十几秒，那期间速率偏快、听感是变调 |
| 两倍以内 | 每次悄悄多跳 ≤8 帧 | 漂移那种量级不值得做一次剪切；8 帧是 21ms 回调的 0.8%，听不出 |

判据臂是 `--realtime --late-open 4`：先把消费方按住 4 秒让环顶到 24000 帧，再放开按
**实时速率**取（每 10ms 取 480 帧，生产与消费的标称速率相等，于是水位的任何变化都只
可能来自导向本身）。实测一次调用内 24000 → 1840，之后稳定在 1000~2000 帧。

**长跑对账**（3 分钟、7000 帧，出口用 disk 驱动——它比标称慢约 7%，正好是一个持续
超速的生产者）：导向一共悄悄排掉 628293 帧，而**"丢最旧"全程是 0**，水位稳在 3100 帧
（65ms），补静音只有起始那 2080 帧。改之前同一条臂是环顶满、`丢最旧` 一路涨——
也就是说这一档导向把以前会溢出成丢帧的东西接住了。同一场里视频腿 0 重起、0 序号缺口、
0 解码失败，音频腿 RR 发出 193 个、失败 0、重起 0。

**这一臂上"调速"这个数要会读，否则会误伤结论。** disk 驱动的出口每秒只消化得掉约
45000 帧（标称 48000），差的那 3000 多帧/秒**必须由导向排掉**，不然水位就一直涨——
所以"每秒悄悄排掉约 7% 的音频"在这条臂上是**正常读数**，不是产品在生产 7% 的垃圾。
账是闭合的（产品臂，12 秒）：产 100.5 包/秒 × 480 = 48240 帧/秒，实测交付 45056 +
调速 3268 ≈ 48324，而水位恒定 2163 帧、`丢旧=0`、`补静音=0`。
反过来，接真 CoreAudio 设备的臂上 `调速` 应当只吸收漂移（量级是"每小时几十毫秒"，
也就是每秒零点几帧），若它也读到几千/秒，那说明**出口**消化不掉——那时该查的是声卡
那一侧而不是导向。这一条推论**没有实测**：要量它就得让机器出声，夜里不做。

这一臂本身修过两次才可信：第一版 `sleep_for(10ms)` 每次实际睡到 13~15ms，消费者只有
标称速率的七成，环全程顶满、"丢最旧"一路猛涨，看起来像导向没生效；第二版把节拍改成
绝对时刻（`sleep_until`）之后才发现真正的问题是**按住消费方这件事根本没发生**——
那个"什么时候开始放"的时刻是在建立设备会话**之前**取的，等到循环开始时早就过了。
两个版本共同的教训和 §13 那条一样：拿"水位不动"当结论之前，先确认那把尺自己是不是直的。

**⑦ 产品这一侧的出口用 disk 驱动量，不需要让机器出声。**

上面六臂量的都是 `AudioPump`，而产品还多一段：`AudioOut` 的 SDL 回调、预滚闸门、以及
它到底有没有把解出来的东西送进设备。`SDL_AUDIO_DRIVER=disk` 会把回调产出的每个样本
原样写进当前目录的 `sdlaudio.raw`（48kHz/2ch/s16），于是这一段也能**无显示器、不出声**
地判对错——在夜里这是唯一可接受的自检形式。

```
SDL_VIDEODRIVER=dummy SDL_AUDIO_DRIVER=disk ./build-cmake/scrctl --no-window --exit-after 3000
```

读数（3000 帧视频、72.1 秒音频 = 3460096 帧）：峰值 25109、RMS 4509、按 10ms 窗口切
7208 段里只有 4 段全零。送到设备口子上的是**连续、在量程之内**的真实波形（不是零，也
不是顶满刻度那种失控值）。至于"波形形状对不对"这一层，disk 驱动给不出判据——它只能
证明有东西连续地流到了回调出口，音质仍要人耳（这一格后来由用户听过，见本节末）。

两个坑记在这里：disk 驱动**只写文件、不等时**，所以它消费得比标称慢约 7%（实测交付
44~45k 帧/秒），恰好构成⑥里那种"生产者持续超速"的场景——这既是它的用处也是它的局限，
别拿它的耗时当实时性判据；另一个是它把 `sdlaudio.raw` 落在**当前工作目录**，所以要么
在仓库外面跑这一条，要么跑完删掉，否则 `git status` 会被一个 13MB 的无主文件污染。

**disk 驱动照抄请求，所以它当不了"协商对死"那一判的仪器。** `AudioOut::open()` 现在
要求 `have` 与 `want` 逐项相同（48kHz/2ch/S16SYS），不同就关设备并说人话——因为这一层
没有重采样器，把 48k 的样本按 44.1k 放出去听感是**音调低半档**，而没人会往"声卡还价"
上想。这一判的**正向**是实测过的（disk 下 `have` 就是 48000/2/S16LE，出口照常开）；
**负向没有**——disk 永远给回你要的，构造不出一次还价，而能构造它的真 CoreAudio 设备
一开口就是出声的。所以那一支今天是"照着定义写的、没被反例打过"的形状；要补的话，
得有一台能把默认输出改成 44.1kHz 的机器。

**最后那一格由人耳关闭了**（2026-09-27 上午，用户在本机听过镜像的声音，报的是
"正常"这一句）。这是本节唯一一条不是仪器给的判据，所以它的边界要写清楚：它确认的是
**默认参数下（48kHz 立体声、`--audio-buffer 50`）听着是对的**——也就是不是"没声/杂音/
低半档"这三种失败里的任何一种。它**不**给出延迟的毫秒数，也没对过口型：那要一次双通道
录制（同一个事件同时进画面与录音）才判得了。留这句在这里，是为了以后有人把"音频做完了"
读成"延迟也量过了"时能看出差在哪。

## 18. 非 Apple 平台：静态查得出来的都查了，剩下的只能等 CI

跨平台是硬要求，但写这些代码的机器上没有 Linux 工具链也没有容器，所以"Linux 能不能
构建"这件事**没有一手证据**。能做的是把"必然会红的几条"提前拿掉：

- **libstdc++ 不像 libc++ 那样顺手替我们带传递 include。** 判据不是"看着像"，是脚本
  扫"这个 .cpp 用到了某个 std 符号，而它自己与它（递归）包含的本地头里没有任何一处
  显式 include 对应系统头"。八处命中（`std::malloc/free`、`std::min/max`、
  `std::equal`、`std::atoi`），补完再扫剩 0 处。同一把尺扫"裸 `uint32_t` 没有
  `<cstdint>`"——那一类本来就是 0 处。
- **`cmake_minimum_required` 从 3.28 降到 3.20。** 仓库里用得最新的特性是
  `pkg_check_modules(... IMPORTED_TARGET)`（3.16）与 C++20（3.12），没有理由为一个
  用不上的版本号把 Ubuntu 22.04（cmake 3.22）与 Debian 11（3.18）挡在外面。
- **Apple 专有 API 的边界是干净的**：全仓只有 `decode/VideoToolboxDecoder.cpp` 与
  `decode/AudioToolboxEldDecoder.cpp` 两个文件 include 苹果框架，两者都在
  `if(APPLE)` 分支里，别处换 `PlatformDecoderFallback` / `AudioDecoderNone`。
  `-framework` 那几个 flag 也只在 `APPLE` 分支上加。
- **POSIX 面只有 `transport/Usbmux.cpp` 一处**（`<sys/socket.h>`/`<sys/un.h>`/`poll`，
  路径 `/var/run/usbmuxd` 在 macOS 与 Linux 上都是对的）。这也意味着 Windows 不是
  "差一个后端"而是"这一层要重写"——AMDS 那条路一行没有。
- 全仓没有 `__attribute__`/`__builtin__`/#pragma once 之外的编译器扩展。

这些加起来仍然只是"少了几条已知的红"。真正的判据只能来自 CI，而它现在有了：
2026-09-27 那两次 push 之后 Ubuntu 格的**步级**结论是
`装依赖（Linux）/ 配置 / 构建 / 可执行文件冒烟 / 离线自检` 全 success，日志里是
`100% tests passed, 0 tests failed out of 15`。于是 `continue-on-error` 已撤，
Ubuntu 与 macOS 一样是门槛。

**这里有一个通用教训，比这条 CI 本身值钱**：带 `continue-on-error` 的 job，它的
`conclusion` 会把"这一步失败了"和"这一步根本没跑"折叠成同一个绿色。所以"它绿过几轮"
**不构成**撤掉那一行的理由——要看的是步级结论加日志里的实际计数（`gh run view --json jobs`
取步级；取整份日志要挂代理，直连 GitHub 会把下载静默截断）。仍然没被 CI 覆盖的是**真机**
那一半：Linux 上 libusb、DDI 挂载、隧道建立没有一次对着手机跑过。

## 19. `--verify` 的回读一直读错了地方（以及它为什么一直没被发现）

加 `--background-color` 时发现：涂上去的颜色在回读图里"不存在"。查下来错的是**回读
自己**，不是渲染。

`SDL_RenderReadPixels` 的 `rect` 在渲染器挂着 `SDL_RenderSetLogicalSize` 的时候是按
**逻辑**坐标解释的。产品传的是 `(0,0,out_w,out_h)`——那两个数是问
`SDL_GetRendererOutputSize` 拿的**设备**像素尺寸。窗口比例与画面比例一致时两者几乎
重合，读回来看着没问题；一旦窗口比例不同（有等比留边），读到的就是**从内容区左上角
起的另一块设备矩形**，边上那些像素根本不在范围内（微测：内容区之外的 1x1 读直接
返回失败）。

修法：读之前把 logical size 摘掉，读完挂回去。

顺带一条被否证的猜测：本以为 `SDL_RenderClear` 也只清视口、涂不到两条边，所以在它
前后也加了一对摘/挂。把这对去掉再回读，边上仍然是背景色——**clear 清的是整块目标**，
不需要那个仪式。留着它就是一段没有依据的仪式码，所以删了。

判据进 `tests/render_test.cpp`：造一个 800x400 的窗口装 1125x2436 的画面（故意让
比例不等），断言"边上 = 背景色、内容中心 = 纹理"。这条断言是**变异验证**过的：把
回读前的 detach 拿掉，它报的是 `边=(255,255,255) 内容=(0,0,0)`——正好是"读到了
错位的那块"该有的样子；而把 clear 前后的 detach 拿掉，它照样通过，这才确认后者确实
多余。

**教训**：`--verify` 是这套东西里唯一"看得见窗口里到底画了什么"的仪器，而它自己
错了 4 个月。原因是它的判据一直是"图看起来对"，而默认窗口尺寸是按画面比例算出来的
——**留边为零**，读错区域这件事在默认配置下没有可观测后果。仪器要有一个专门造出
不一致的档位，不能只跑默认形状。

## 20. 一轮外部 review 抓出的四条，以及"时间型判据摆错位置"这个复发

2026-09-27 上午，另一份代码复核报了四条。逐条回代码验，**四条全部成立**；其中第 4 条
在视频拆包器里还有一份一模一样的写法，报告没提，一并修了。四条各一个 commit：

| 级别 | 症状（用户会看到什么） | 根因 | 修法 |
| --- | --- | --- | --- |
| P1 | 丢包之后画面**永久停在旧帧**，直到有人动鼠标 | PLI 的后备重起判据挂在"这轮解析出了视频字节"之后 | 搬到每轮必做的位置 |
| P1 | `--audio-buffer 600` 之后**一整条腿静音** | 环容量是常量 500ms，而预滚目标随便填，闸门等一个装不满的水位 | 容量跟着水位算，目标封顶 1000ms |
| P2 | `--no-window` 在无显示器/坏 `SDL_VIDEODRIVER` 下起不来 | `SDL_Init` 无条件带 `SDL_INIT_VIDEO` | 按需初始化；顺手拦下 `--no-window --verify` 这个静默无效组合 |
| P2 | 乱序到达被记成丢包（`seq_lost` 虚增） | 序号水位更新成"最后**到达**的"而不是"见过的**最高**" | 抽成 `rt::RtpSeq`，两条腿共用一份 |

**值得单独记的是第一条的复发。** 同一份判据在两个月内被同一个位置错法伤过两次：
上一版的条件是 `gaps > gaps_at_last_check_`（每轮跟着更新，所以只在丢包那一瞬成立，
而那一刻时间项必然还没到点），当时的注释已经把这件事写下来了。判据**内容**修对了，
摆放**位置**没变——所以症状换了一种触发条件（SR 心跳让 `next_packet` 成功）又活了
一次。归纳成一条可以复用的规矩：

> 凡是"靠墙上时钟到点就要做某事"的判据，不能挂在任何"收到了某种包"的支路后面。
> 这条流上每秒一个 SR 会让收包永远成功、让"多久没包"的静默判据永远不响，于是
> 挂在收包之后的时间判据**既等不到超时、也等不到新内容**。
> 泵里现在的四样东西（保活 RR、静默判据、超大 NAL 重试、PLI 后备重起）全部并列在
> `next_packet` **之前**，以后加的第五样也必须在那一区。

**四条的判据状况不一样，别混着信**：

- 第 4 条有离线判据（`tests/rtp_test.cpp` 13 条断言：mod 2^16 回绕、重复包、换会话
  重置、端到端喂真 RTP 包）。**做过变异验证**：把水位改回"迟到也推进"，两条立刻 FAIL。
- 第 2 条有离线判据（`tests/media_test.cpp`：默认档回归 + 扫 16 个请求值断言
  `target < capacity`）。**也做过变异验证**：容量改回常量，扫描那条列出 500ms 是边界、
  501ms 起全部装不满。
- 第 3 条的判据是命令行走出来的（`SDL_VIDEODRIVER=nosuchdriver` 下无窗口能过、
  要窗口如实失败），**并且做了变异验证**。
- 第 1 条当时没有判据（要"丢包 + IDR 不来 + 只剩心跳"三件事同时成立）。**下午设备
  空出来后补上了，见下面 §20.1** —— 补判据的过程本身又翻出两处真问题。

### 20.1 给"后备重起到不到点"补一个能确定复现的档位（真机，iPhone14,4 / iOS 27.0）

判据要成立得同时满足三件事：真丢了一个包、IDR 没来、而之后**只剩 SR 心跳**。
这三件都不能靠运气，所以给 `FramePump::Options` 加了三档（产品路径恒为默认值）：

| 档位 | 做什么 | 为什么需要它 |
| --- | --- | --- |
| `debug_drop_nth_packet N` | 第 N 个视频包我们**不吃**（设备照发） | 造一次真实的序号缺口，而不是等网络哪天抖一下 |
| `debug_suppress_pli` | 一个 PLI 都不发 | 让"IDR 不来"成为确定事件，而不是赌设备的响应 |
| `debug_ignore_video_after M` | 第 M 个之后只吃 SR | 本地模拟"画面静止"，于是**屏幕上在演什么与实验无关** |

第三档是走过弯路之后加的：第一版以为按 home 退回主屏就"静止"了，实际设备上
YouTube 的画中画还在放，159 包/秒一刻没停，整个"只剩心跳"的前提根本没成立。
被测的判据关心的只是"这一轮有没有拿到可解析的视频字节"，那就从这一位直接切。

配套把 `silence_restart_ms` 设成 0（关掉静默那条），于是"重起发生了"只剩一个来源；
`Stats::stall_restarts` 单列一位，就是为了能把"谁救的场"说清。

**读数（`tools/stall_probe --stall-ms 4000 --drop-at-packet 700 --ignore-video-after 800`）**：

| | 修好的那版 | 把判据搬回旧位置（变异臂） |
| --- | --- | --- |
| 丢包 -> 重起 | **4089 ms** | 30 秒内 **0 次**重起 |
| 武装 -> 重起 | 4045 ms（阈值 4000，过冲由 50ms 轮询决定） | — |
| 重起之后新解出的帧 | 163 | 0 |
| PLI 被压住次数 | 45（=一直武装在等） | 38 |
| SR 心跳 | 29 条（所以静默判据全程不会响） | 30 条 |

同一台设备、同一档位、只差那段代码摆在哪 —— 旧位置那一臂就是"画面停在丢包那一刻的
旧帧上，30 秒没有任何恢复动作"。复核报的 P1 到此坐实。

**补判据时顺带翻出来的两处**（都是真的，不修的话上表第一列也拿不到）：

1. **武装本身挂在 AU 回调里**。`awaiting_idr_from_loss_` 是在解析器交出 AU 的时候
   才置起的，而"解析出 AU"这件事在静帧时根本不发生 —— 所以后备判据不只是"到点没跑"，
   它**从来没被武装**。修成 `note_loss()`：拆包器一看见缺口就地武装并立刻发 PLI。
   这一改还顺手把"发现丢包"提前了整整一个 AU 的时间（以前要等下一个 AU 送上门才知道）。
   消费端留了一个 `loss_since_au_` 标记，因为解析器回调要的是另一件事："这个关键帧
   自己的组装期间沾没沾丢包"，两个用途不能共用一个计数器。
2. **`debug_suppress_pli` 顺手把后备判据的时钟也压掉了**。`first_pli_ms_` 是在
   `request_keyframe()` 里、发完包才记的，而我把压制检查写在了它前面 —— 于是第一次
   真机跑出来的结论是"没到点"，而没到点的原因是**档位**不是被测代码。现在那份时钟
   挪到函数最顶端：等 IDR 从决定要等的那一刻就开始计时，发不发得出包是另一回事。
   记下来是因为这类错会复发：**任何"提前返回"的开关，都要问一句它顺手跳过了多少
   状态更新。**

**音频那两条也一并量了**（`--no-window` + `SDL_AUDIO_DRIVER=disk`，本机不出声）：
`--audio-buffer 800` 交付 45056 帧/秒、补静音恰好 35840 帧（=746ms 的预滚）、丢旧 0、
重起 0，**闸门真的开了**（改之前这里是永远 0 交付）；`--audio-buffer 2000` 打出了
"被收到 1000 毫秒"那行且补静音变成 44032 帧（=917ms，对得上收档后的目标）；
默认 50ms 档与 §17.2 ⑥ 那批旧读数一致（缓冲 1720~3600 帧、丢旧 0），没有回归。

### 20.2 第二轮 review 的第 3 条：`seq_lost` 报的到底是哪个数

review 说得很准：`RtpSeq.h` 与 `AudioPump.h` 把它写成"**真正没到**的包数"，而代码算的是
"累计检测到的缺口"。这两者在乱序到达时分岔，而分岔的方向恰好是虚增。上一轮我只修到
"迟到不再被重复记成一次新缺口"（水位改成见过的最高号），漏了另一半：**补到的包要把
欠账冲回去**。所以这一条改的是口径，不是那个 mod 2^16 的坑。

现在 `RtpSeq` 给三个数，各回答一个问题：

| 取数 | 含义 | 只增？ |
| --- | --- | --- |
| `observe()` 返回 `kGap` 的处数（`seq_gaps`） | 序号往前跳过几个**事件** | 是 |
| `gaps_detected()` | 累计"跳过几个号" | 是 |
| `lost()` = `gaps_detected - filled` | **现在看，几个包确实没了** | 否，会被冲销 |

第三行就是 RTCP 接收报告里 cumulative lost 的口径，也是 `app/main.cpp` 那行"真丢 %llu"
一直声称却在说谎的那个位置 —— 标签没改，是数改对了才配得上它。

冲销要有期限，否则一个永远没补到的洞会一直挂着等：`kReorderWindow = 1024` 之外的迟到
不再冲销。这条链路实测的乱序跨度是个位数（音频一秒 100 个包，1024 号等于 10 秒），
所以这个界既不咬真丢包、又把欠账表卡在一千多条以内（表里的号天然不重复，水位每往前
走一次就剔掉落后超窗的）。一次跳掉整个窗口（换个流就会这样）不登记洞、账照记 ——
判成丢失是如实的，留一张千把条的表去等它们全到才是不理性的。

**判据（离线，`tests/rtp_test.cpp`）**：100、102、101、103 这个一个没丢的序列现在读出
`gaps_detected()==1 && lost()==0`；100、101、103、104 读出 `lost()==1`；迟到补齐之后同一
个号再重复到达不能再冲一次；101 在 `101+1099` 之后才到则不冲销；回绕那一圈（65534、
65535、0、2、然后 1）同样从 `lost()==1` 回到 0。端到端那份喂真包给拆包器的断言顺手改了
口径 —— 它上一版写的是 `seq_lost == 1`，**把虚增当成了预期**，所以这条测试当时是绿的却
在替错误背书；现在是 `== 0`，并且再喂一个跳号包要求它变成 1（证明它是当前欠账不是历史
累计）。

四个变异，每次只改一处再跑（`build-cmake`，`rtp_test`）：

| 变异 | 红掉的断言 |
| --- | --- |
| `lost()` 直接返回 `detected_`（不退账） | 5 条：`补齐之后真丢 0 个`、`同一个迟到包重复到达只冲销一次`、`回绕之后的迟到补齐同样要冲销掉`、端到端两条 |
| `prune()` 把在水位之后的洞**全**剔（我中途真写过这一版） | 上面那 5 条，一条不差 |
| 登记洞时 `from = high_` 而不是 `high_ + 1` | 上面那 5 条 |
| 迟到无脑 `++filled_`（不查欠账表） | 另 2 条：`同一个迟到包重复到达只冲销一次`、`洞判死之后不再冲销` |

前三行红得一模一样不是抄漏了：它们破坏的是同一件事 —— "欠账表此刻等不到任何东西"。
这一条等价类能被抓到，靠的就是端到端那两条把 `lost()` 当**当前欠账**而不是历史累计来判。
第四行是另一回事：表还在、只是查得不严，所以只有"不该冲销的时候别冲"那两条咬得住它。

第二行值得单独记：那个 prune 的错我写完当场就发现了（"仍然没到的洞被当成可剔"），但
它当时**编译得过、也测不出红** —— 因为那套断言还不存在。判死条件用的是"落后水位 ≥
窗口"这个 mod 差，不是 `before(s, high_)`：后者对"在水位之前"的洞恒真，等于每轮清空欠账表。

这个口径改动顺带逼出一件以前没注意的事：**`reset()` 会把 `lost()` 一起归零**。
音频泵每换一条会话就 `seq.reset()` 一次（不重置会把新流的第一包判成丢几千个），而它的
`stats_.seq_lost` 要的是整条腿开下来真丢了多少 —— 旧的 `+= lost` 写法天然跨会话，换成
取累计值之后就没了这个性质。所以那里加了一个 `lost_carry`，在 reset 之前把旧读数搬过去。
这类"改了取数方式才发现原来依赖的是一个副作用"的地方，值得在提交里单独说一句。

第 1、2 条（关键帧未解出就清除恢复状态；探针默认参数造不出可见缺口）写在 §20.3：那条
判据要在真机上量，而设备当时被带走了，所以那份改动在之后的提交里才进来 —— 中间隔了一次
"重读自己写的臂、发现它不判别"的返工，值得单独一节。

### 20.3 第二条 P1（关键帧没解出来就清了恢复状态）：判据本身先要能判别

review 那条成立：`on_access_unit` 在解析器交出"看起来干净的关键帧 AU"那一刻就把
`need_keyframe_ / awaiting_idr_from_loss_ / first_pli_ms_` 清了，可解码器完全可能收下同一个
AU 却给不出图（`!ok || !f` 那一支）。清早了的后果不只是一个计数不准：**参考链断掉的 P 帧
会被直接发布**（画面是花的），而且不再要 IDR、后备也永不武装。修法是把解除警戒挪到"这一帧
真的解出来并发布了"之后。

设备被带走之前我抢跑了一次，只抓到"后备重起发生了"，没抓到"假装失败发生在 +X ms"，
判据不成立也不推翻。**回去重读自己写的那一臂，发现它根本判别不了** —— 三处各自都不够：

1. **受害者挑错了。** `debug_fail_decode_of_keyframe` 见着关键帧就下手，而第一个下手对象是
   **会话开头那个 IDR** —— 那一刻 `awaiting_idr_from_loss_` 还是 false（它是丢包才置起的），
   于是"后备武装"这件事跟被打的那一刀毫无关系。没修的版本清完状态照样在后面的真丢包上武装、
   照样重起 → 两臂都读出"重起发生在失败之后" → **假绿**。现在门槛是 `keyframe &&
   awaiting_at_entry`（进轮快照，理由见下面读数里那条"门槛要读快照"）：只打"来修丢包的那个
   IDR"。
2. **PLI 全压住 = 没有 IDR 可打。** 臂 A 用"一个 PLI 都不发"来保证 IDR 不来；臂 B 要的却是
   IDR 真的来、然后把它判死。所以那一臂改成"PLI 照发，直到假装失败发生，此后全按住"
   （新档位 `debug_suppress_pli_after_fail`）。顺序是 丢包 → 武装 → 一个 PLI → 设备 20~35ms
   回一个 IDR → 这个 IDR 判死 → 之后只剩后备这一条路。`pli_off_` 故意**不**随会话重起清零，
   否则新会话又放出一次 PLI 就多出第二条恢复路径。
3. **判据不该只盯"重起发生了没"。** 重起是这条链上最慢、最容易被别的事件污染的一个读数。
   分岔其实在失败之后**几毫秒**就摆明了：修过的版本仍然算"在等干净关键帧"，于是
   `dropped_awaiting_keyframe` 一秒涨几十而 `decoded` 一动不动；没修的版本立刻开始发布断链的
   P 帧，`decoded` 涨而 `dropped_awaiting_keyframe` 平。所以探针现在是**两问**：先看失败后
   1 秒那两个增量谁在涨（这一步就能定"清早了没"），再看后备重起有没有跟上来（那一步定
   "画面回没回来"）。`decoded > 0` 直接判"没修"并返回 1。

**读数（真机，iPhone14,4 / iOS 27.0 / USB，`--fail-decode 1 --drop-at-packet 700 --stall-ms 4000 --watch 22000`）**：

| | 修过的（解除警戒在发布之后） | 变异臂（挪回解码之前） |
| --- | --- | --- |
| 假装失败发生在 | +1461 ms | +1456 ms |
| 此后 1 秒里挡下的 AU | **+62** | +0 |
| 此后 1 秒里发布的帧 | **0** | **+63（参考链是断的）** |
| 此后 1 秒里按住的 PLI | +234 | +0（没东西再武装） |
| 后备重起 | +5421 ms（失败之后 3960，阈值 4000） | **一次都没有**（`stall_restarts=0`） |
| 重起之后 | 又解出 986 帧 | — |
| 探针退出码 | 0 = 判据成立 | 1 = **没修** |

两臂在"失败之后第一秒"这一对读数上是 62/0 与 0/63 的镜像，判据至此是判别的。修过的这一臂
连跑三次（含改动门槛前后各一次）读数一致：挡下 62~63 个 AU、发布 0 帧、重起落在失败之后
3939~3960 ms（差值由 100ms 采样与 `first_pli_ms_` 早于失败那几十毫秒决定）。臂 A 也按新默认
值复跑过一遍确认仪器没退化：丢包 → 重起 4084 ms、武装 → 重起 4052 ms（§20.1 那一次是
4089/4045，同一分布）。

**门槛要读快照，不然变异臂会被自己判空。** 第一次跑变异臂的读数是"判死=0、假装失败根本没
发生"，退出码 2 —— 看着像"这一臂白跑"，其实是变异把受害者门槛的输入抹掉了：那道门槛写的是
活的 `awaiting_idr_from_loss_`，而被测的那个"清得太早"恰好清的就是它。**仪器依赖了被测物**，
这与 §20 记的"`debug_suppress_pli` 顺手压掉后备时钟"是同一类错，只不过这次被压的是自己的
判据输入。改成进轮时先快照 `awaiting_at_entry` 之后，修过的那一臂读数一字不差（62/0/重起），
变异臂才真的红成"没修"。固定动作记一次：**做变异臂之前，先问它破坏的变量是不是自己读的。**

读数里有个坑要说清，不然下一次会被误导：那行"读账"里的**缺口**是 `FramePump::Stats` 每轮从
**当前**拆包器抄来的快照，不是全程累计 —— 所以上表第一列在重起之后读到的是**新会话**的缺口
（0），看着像"从头到尾没丢过包"。真正全程累计的是 `总重起`/`判死` 那几个。要跨会话累计缺口，
就得照 §20.2 那个 `lost_carry` 的法子先收走旧值。

顺手补的两条参数校验（不用真机就能验，已验）：丢包点 <200 包会被拒 —— 缺口打在会话开头
那个 IDR 自己身上时，每条会话都注定解不出东西，量到的全是"重起—失败—重起"循环（§20.1
就栽过一次，那回是 12）；`--ignore-video-after` 不比 `--drop-at-packet` 大 40 以上也拒
（上一轮 review 的第 2 条）。默认值同时改成 700/900，退出码定成 0=判据成立、1=不成立、
2=现场没造出来。

**一个改完之后仍然存在的行为变化，要他拍板**：解除警戒挪到出图之后，意味着"设备送来的 IDR
我们的后端始终解不出"这一种坏法，从"画面永久冻在旧帧"变成"每 `stall_ms` 重起一次会话、
一直试"。后者能自愈（画面一旦变得可解就回来），但会没完：`nokey_restarts_` 那种上限
这里没有对应物（超大 NAL 那一路有 `oversized_restarts_` 退避加 `video_unusable_` 降级到
截图服务）。要不要给后备重起也加一个"连续 N 次重起都没解出任何关键帧就降级"的上限，
是一个取舍而不是 bug，等回来定。

### 20.4 第三条 P2：持续解不出关键帧时"到顶"不等于可以停手（而且那把阶梯原先根本跑不到）

review 报的是：丢包后的 IDR 解不出图 → 后备重起 → 新会话还解不出 → `nokey_restarts_` 满 3 次
就停；而 SR 每秒还在到，`last_packet_ms_` 一直被喂，静默那条永不响。**结论成立**，但真机第一跑
把它报得还轻了——臂 C（`--fail-every-keyframe 4`，不丢包、每个 IDR 都判死）115 秒的读数是：

```
读账：包=69316（视频 69201 / SR 115） AU=6894 挡下=0 解出=0 判死=1 后备重起=0 总重起=0
```

**总重起 0 次。** 那把阶梯一次都没走。原因不是"到顶之后停手"，是**它压根到不了顶**：整段判据
挂在 `if (!session_->next_packet(...))` 这条**超时支路**里，而画面在动的时候包是连续到达的，
50 毫秒的轮询几乎永远成功。于是这条流每秒送来一千个包、一个 AU 都解不出图、泵既不重起也不
降级，窗口永久停在最后一帧好画上。

这是 docs §20 那条"时间型判据必须挂在每轮必到的位置上"**第三次**复发，三次成因各不相同：

| 次 | 判据 | 当时挂在 | 为什么跑不到 |
| --- | --- | --- | --- |
| §20 | PLI 后备重起 | "解析出了视频字节"之后 | 静帧时一个视频字节都不来 |
| §20.1 | 同一条（搬对位置后） | `next_packet` 之后 | SR 让那条支路的 `continue` 跳过它 |
| §20.4 | 起流无关键帧的阶梯 | **读包超时**那条支路 | 包连续到达时超时永不发生 |

第三次值得记的点：前两次的教训是"别挂在包之后的支路上"，我把判据搬到"每轮必做"那一区时，
**默认剩下的那条超时支路是每轮都会进的**——它不是。一台忙碌的设备会把超时彻底饿掉。

**改法**照这个文件里已有的那一路（超大 NAL，§15 之后加的）：到顶之后转成
`video_unusable_` 降级（取帧方改走截图服务），重试交给**泵自己驱动**的 60 秒退避计时
（`kOversizedRetryMs`，它挂在每轮必做那一区，不依赖包到达），解出一帧关键帧就把标记清掉。
判据的形状抽成纯函数 `plan_nokey(restarts, max, already_unusable)`：`kRetry` / `kDegrade` /
`kWait` 三档，**没有"停手"这一档**。离线测试扫 0..12 次断言"没到上限之前永远不会提前躺平"，
变异（把到顶那格改成 `kWait`，也就是修之前的行为）红 4 项。

**真机两跑（iPhone14,4 / iOS 27.0 / USB）**：

| | `--fail-every-keyframe 4` | `--fail-every-keyframe 5` |
| --- | --- | --- |
| 阶梯 | 5 秒一次、重起 3 次 | 同 |
| 降级立于 | +20473 ms（其时总重起 3、解出 0 帧） | +20459 ms |
| 帧恢复于 | +20784 ms（降级之后 **311 ms**） | +80858 ms（降级之后 **60399 ms**） |
| 结束 | 解出 5663 帧、总重起 4 | 解出 5351 帧、总重起 5 |

两跑的差别是故意留的：N=4 时阶梯自己那几次重起里就有一个 IDR 解得开（311 ms 就活了），
**证明降级不是终局**；N=5 时第 4 次也判死，只能等那把 60 秒退避——它真的把流救回来了
（60399 ms，误差就是那 50 毫秒的轮询与打点），这才把"降级成了一个出不来的坑"那种形状否掉。
`--fail-every-keyframe` 小于 4 会被拒：走不到顶就量不到被测的那一格。

顺带把臂 C 需要的档位补上：`debug_fail_any_keyframe`（true 时每个关键帧都算解不出）。
原来那道受害者门槛要求 `awaiting_at_entry`，而**臂 C 现场没有丢包**，一条都不命中，
于是"后端一直解不出"这个终态在旧档位下根本造不出来。

## 21. 第一份外部反馈：一台 iPad mini(iOS 18) 连不上，以及我们三处把方向指错的报错

别人的 iMac（AppleClang 17 / OpenSSL 3.6.3）、自己的 iPad mini、从 GitHub clone 后 `make`
一次通过——**构建这条没问题**（非 Apple 之外第二台机器上第一次跑通，值得记）。问题全在连接：
`./scrctl` 跑了 22 次、**一次都没成功**，失败分三层，一层比一层深：

| 报的那句 | 出现次数 | 真正的原因 |
| --- | --- | --- |
| `没有在连设备` | 9 | 设备当时没在总线上（推断：他后来插好了才走到下一层） |
| `ReadPairRecord 没有返回 PairRecordData` | 5 | 设备上还没点「信任这台电脑」 |
| `StartService(CoreDeviceProxy) 被拒: PasswordProtected` | 2 | **设备锁着**——iOS 只在解锁状态放行开发者服务 |
| `设备目录里没有 displayservice` / `…没有服务 com.apple.coredevice.deviceinfo` | 6 / 6 | 见下 |
| `displayinfoupdates … is not implemented` | 3 | 设备自己说这条 action 它没实现 |

最后一层是唯一可能推翻"支持范围"的：**隧道建起来了、feature 层能对话**（所以配对与裸
CoreDevice 那部分是通的），但目录里既没有 `deviceinfo` 也没有 `displayservice`。这有两种
解释——(a) 那台 iPad 从没让 Xcode 挂过 DDI（他的日志里没有任何 `devicectl`/Xcode 的痕迹，
而这台设备"刚开的开发者模式"）；(b) iOS 18 的 DeviceKit 就是没实现这套媒体流服务。
**现在分不开，而这两条的处置完全不同**：(a) 让他用 Xcode 连一次就好，(b) 是支持范围，得写进
README 并考虑退回截图服务。所以这一节留在这儿等下一次反馈。

**我们有三句报错在帮倒忙，都改了**：

1. `PasswordProtected` 后面我们统一接"（DDI 是否已挂载？开发者模式是否开着？）"。它的意思
   是"请把屏幕解开"，照着 DDI 去查的人会白跑半天。现在按错误码分派（`proxy_failure_hint`）：
   锁屏 / 没信任 / 服务不存在 / 兜底四种，**锁屏那一句里不再同时反问 DDI**。
2. UDID 没匹配上时我们写"（注意信任与锁屏状态）"——**这是假的因果**：没点信任、锁着屏的设备
   照样出现在 usbmux 的列表里，一手证据就是他自己那份日志：`ReadPairRecord` 失败发生在
   设备**已经被列出来之后**（列表来自 USB attach，信任是后面 lockdown 那一步才要的）。
   现在这句改成"当前在连的是 ****801E（一共 1 台）……要么是插的不是这台，要么那台已经掉线"。
3. "目录里没有 X 服务"以前只报缺什么。现在把**目录里 `com.apple.coredevice.*` 这一族整段打
   出来**（实测我这台 iPhone 是 85 条服务、其中 24 条是这个族——全打没人读，所以只打这一族，
   族外报数目），族里一条都没有时直接说"这就是没挂 DDI，先用 Xcode 连一次"。这一族有没有、
   有几条，正是上面 (a)/(b) 的分界。

判据：前两处是纯函数（`proxy_failure_hint`、`Rsd::missing_service_message`），`tests/net_test.cpp`
里 10 条断言，含"锁屏那句不许反问 DDI 挂载"、"族外服务不列"、"空目录要说清是空的"。第 2 处
另外在真机上跑了一遍：`scrctl -s 11111111-…` 打出
`没有在连设备匹配 ****2222；当前在连的是： ****801E（一共 1 台）…`。第 1、3 处的**设备端**
形状只能等他下一次跑（我这台 iPhone 一切正常，造不出那种现场），所以这里如实标注：改动是
离线判过的，真机那一半还没落地。

顺带记一件对本机有意义的事：`--list-devices` 与 `ioreg -p IOUSB` 这两个口子已经足够判断
"设备到底在不在总线上"——今晚我自己那根只供电的线就是靠它们当场定性的（详见提交历史）。

## 22. M5 侦察：Wi-Fi 这条路今晚走通了哪几段（实测，iPhone13 mini / iOS 27.0 / 同网段）

**结论先行，三句话**：

1. 无线隧道**建得起来**，而且起来之后 RSD 目录与 USB **逐项一样**（各 85 条服务），
   `displayservice` 在——所以 M5 不需要为"无线上某些服务没有"设计降级路径。
2. 今晚最大的收获是**配对可以不碰手机**：USB 上那条 `remotepairingdeviced` 控制面能把
   RemotePairing 记录引导出来（免弹窗），之后网络侧只做 pair-verify。这把 M5 原先最大的
   未知数——"用户是不是必须去 设置 > 开发者 > 配对的 Mac 里点一下"——从关键路径上摘掉了。
3. 只剩一个真未知数：**媒体流在无线上的表现**（设备反向打进隧道的 RTP 走空中），以及我们
   自己的实现。

下面每一条都标了是量出来的还是推出来的。仪器是 pymobiledevice3（只当仪表用，一行代码不进
产品，GPL 那条红线照守）。

### 22.1 发现（量出来的）

- 设备在局域网广播 `_remotepairing._tcp`，看到的端口是 **49152**；TXT 四键
  `identifier` / `authTag` / `flags=0` / `minVer=8` / `ver=26`。
- **同一台设备有两个 identifier**：USB 控制面上 `peerDeviceInfo.identifier` 就是 **UDID**，
  而 mDNS 里那个是不透明 UUID（`32567CFA-…`）。所以我们存记录不能只按 identifier 索引，
  得把"这条记录对应哪台设备"和"它在广播里叫什么"分开记。
- **不用 mDNS 也能定位设备**（这条对产品形状影响很大）：lockdown 顶层 `WiFiAddress` 与那条
  A 记录 `10.24.24.7` 的 ARP MAC 一致——MAC 是我拿 `arp -n` 与 lockdown 的值比出来的，不是
  猜的。也就是说"已配对的设备要做 pair-verify"时地址可以问 lockdown 要；mDNS 只有在**第一
  次配对**才真正需要。
- 同一个 49152 在四条链路上都握手成功：USB 的 NCM 口（`fe80::…%en6`、`fe80::…%en7`、
  `169.254.73.199`）和 Wi-Fi 口（`10.24.24.7`）。顺带：广播里到底出现几个地址**不稳定**，
  同一台设备连跑三次分别给 3/3/5 条——Wi-Fi 那条 A 记录有时不在。所以"靠一次 browse 拿地址"
  这种写法天生会偶发失败，这也是一手证据支持"地址走 lockdown、mDNS 只做配对"。

### 22.2 配对：USB 引导、免弹窗（量出来的）

`com.apple.dt.remotepairingdeviced.lockdown` 这条 lockdown 服务说的是**同一套 RPPairing 帧**
（`RPPairing` magic + u16BE 长度 + JSON 信封）。设备在这条面上答的：

| 字段 | 值 |
| --- | --- |
| `wireProtocolVersion` | 26 |
| `minimumSupportedWireProtocolVersion` | 8 |
| `deviceOptions.allowsPairSetup` | **true** |
| `deviceOptions.allowsPinlessPairing` | true |
| `deviceOptions.allowsFreePairing` | false |
| `deviceOptions.allowsIncomingTunnelConnections` | **false**（这条面只配对，不建隧道） |

走完 pair-setup 落的记录有四个键：`public_key` / `private_key` / `remote_unlock_host_key` /
**`peer_alt_irk`（16 字节）**。最后这个是关键：广播里的 `authTag` 就是它算出来的，主机靠它在
不连接的前提下认出"这条广播是我已经配对过的哪台设备"。今晚实测匹配成功（`authTag=cuHWkXdk`
对上了我们的记录），所以 authTag 那条推导链不是纸面说法。

两个要记下的对照事实：

- **`_remotepairing-manual-pairing._tcp` 我们这台根本没广播**（browse 回来是空列表）。所以
  "等设备进配对模式、主机主动 SRP"那条路在 iOS 27 上是走不通的；能走的是 USB 引导（今晚这条）
  或者 iOS 27 的设备端主动配对（设置 > 开发者 > 配对的 Mac，要人点）。
- USB 引导之所以免弹窗，是**推出来的机制**：它跑在已经互信的 lockdownd 通道上，设备不再问用户
  一次。我量到的是结果——整条 pair-setup 期间没人碰手机，记录落了地。

### 22.3 隧道：pair-verify → createListener(tcp) → TLS1.2-PSK → CDTunnel（量出来的）

对 `10.24.24.7:49152` 逐段跑通：pair-verify 过（设备认我们的 host 密钥）、`createListener`
回了端口、TLS-PSK 握手过、CDTunnel 握手给出隧道内地址 `fd45:230b:54f1::1` 与 RSD 端口 61990。
网络那条面上设备答的 `allowsIncomingTunnelConnections` 是 **true**，和 USB 控制面正好相反——
"哪条面只配对、哪条面才建隧道"就是这么分的。

还有一条写实现时要用得上：设备 TXT 与握手都报 **`wireProtocolVersion=26`**，而今晚真正握手成功
时主机发出去的是 **19**（仪器里那个常量），设备照收。**别拿设备的版本号当自己该发的值**——
发 26 会怎样没测，但"发 19 能用"这条是量出来的。

一个坑，**是仪器侧的不是设备的**：TLS-PSK 要 OpenSSL 后端，而 macOS 系统 python 链的是
LibreSSL 2.8.3，`set_ciphers("PSK")` 直接 `No cipher can be selected`。本机为此另开了
`.probe-venv314`（brew python3.14 + OpenSSL 3.6）。**对我们的产品这不是问题**：
`src/transport/TlsChannel` 本来就链 OpenSSL，PSK 只是多一个 `SSL_CTX_set_psk_client_callback`。

### 22.4 Wi-Fi 与 USB 的 RSD 目录：逐项一样（量出来的）

两边各 **85** 条服务。逐名 diff 只剩两条差异，而且两条都是我正则截断造成的假差异
（`com.apple.carkit.remote` ⊂ `…remote-iap.service`、`com.apple.dt.remote` ⊂
`…remoteFetchSymbols`）。真正关心的都在：`displayservice`、`screencaptureservice`、
`hid.indigo`、`hid.universalhidservice`、`appservice`、`pasteboardservice`、`devicecontrol`。

（这里要给自己记一笔：第一次比的时候我把 `feature_probe --all` 输出里的**feature 标识符**当成
服务名去 diff，得出"USB 多 30 多条"的假结论。检查的方式很简单——两边目录总数都是 85，差 30
条不可能对得上。**计数不一致时先怀疑自己的抽取，不要先怀疑设备。**）

### 22.5 还没测的（别提前当结论用）

1. 媒体流在无线隧道上的真实表现。设备反向打进隧道的 RTP 走的是空中：MTU、丢包率、时延都和
   USB 不同，我们的 64KB 长度前缀、PLI 阶梯、租期那套判据要拿无线再跑一遍才敢说话。
2. "一台设备只留一条 RSD 连接、换了 peer 就把已公布的服务端口全关掉"这条规矩（见
   `remote/Device.cpp` 里那段关于 peer UUID 必须稳定的注释）在"USB 一条 + Wi-Fi 一条"下还成
   不成立。这条如果反过来咬，症状会很像"起了 Wi-Fi 之后 USB 的流死了"。
3. 我们自己的实现。剩下的量：RPPairing 帧、pair-verify 密码学（X25519 + HKDF-SHA512 +
   ChaCha20-Poly1305 + Ed25519，OpenSSL 全给）、TLS-PSK、记录存取；mDNS 只有第一次配对才需要。
   `Tunnel.cpp` / 用户态 IPv6+TCP 栈 / `Rsd` 三块是现成的，直接复用。

### 22.6 同一晚：我们自己的实现走到哪一步（真机读数，不是仪器）

上面那些是"拿仪器量出来的路"。这一节是**我们自己的代码**在同一台设备上跑出来的：

| 段 | 读数 |
| --- | --- |
| pair-verify | 过（设备认我们的 host 密钥），`tools/wifi_probe --address <ip>` |
| createListener(tcp) | 给了端口，端口连得上 |
| TLS1.2-PSK + CDTunnel | 过：本机 `fde1:71fc:88cc::2` / 设备 `::1` / 隧道内 RSD 61998 / **MTU 16000** |
| 隧道内 RSD 目录 | 85 个服务，`displayservice`、`screencaptureservice`、`hid.indigo`、`appservice` 都在 |

隧道参数与 USB 那条**一模一样**（USB 侧 MTU 也是 16000，是我们请求的那个值），
所以从 RSD 往上的代码不需要为无线分叉——这一条是 M5 最重要的架构结论：Wi-Fi 只是
"把隧道接上来"的那一段不同，隧道里面完全相同。

踩到的坑值得单独记，因为它的形状很骗人：握手答得好好的，**第一条 pairingData 发出去
连接就被关**。第一反应是"我们的 JSON 是紧凑的、参考实现带空格"——把两边的字节都灌进
一个本地 dump 服务器逐字节一比，差别确实只有空格。但那是**假线索**：真原因是
`{"event":{"_0":{"pairingData":…}}}` 里 `event` 的联合体那层 `_0` 我漏了。教训是
**"两边字节不一样"不等于"字节不一样导致失败"**，判据要落在设备上，不能落在 diff 上。
这种 bug 在现场看起来永远像网络问题，因为设备不给任何错误码、直接关连接。

所以现在有两道离线判据：
- `tests/wifi_test.cpp` 里拿真机回信搭的"假设备"回放，钉死 `event._0.pairingData._0`
  这两层包装、两条 verify 的 `startNewSession` 一 true 一 false、以及设备回 ERROR 时
  要判"没配对"并补一句同样两层包装的 `pairVerifyFailed`。变异判据做过：把 `_0` 去掉，
  红的正好是那一条。
- PSK 这条本身就是 pair-verify 的判据：隧道监听器只认那把密钥，密钥派生差一个字节
  都握不上（`handshake_psk` 的失败文案专门认这句）。

没测的（下一步的头两个问题）：
1. **流在无线上跑不起来/跑得好不好**——目前只到"读得到目录"。
2. **USB 一条 + Wi-Fi 一条并存**时，"设备每条隧道只留一个 RSD 连接、peer UUID 一变就
   把这台机器重新 attach"这条规矩还成不成立。

### 22.7 无线镜像端到端跑通了（真机读数）

```
scrctl -s ****801E --wifi 10.24.24.7 --no-window --no-audio --stats --time-limit 12
  渲染 719 帧  本段 59.9 fps  全程均 60.2 fps
  解码每帧 3.8 ms   序号缺口 0   重起 0   等关键帧丢 0
```

也就是说无线这条不是"能连上"，是**能镜像**，而且这条路上用的就是与 USB 同一份
起流/收包/解码代码（`establish_wifi()` 与 `establish()` 只在"隧道怎么接上"这一段
不同，接上之后收在同一个 `finish_session()` 里）。

**并存**那条问题（§22.5 第 2 条）有了一半答案：跑完无线那 12 秒之后紧接着跑 USB，
8 秒 482 帧、缺口 0，一样好——所以 Wi-Fi 不会把 USB 那条"顶掉"。**但测的是先后各跑
一次，不是两条同时开镜像**，同时开没测过，别当结论用。

还差的三件事，按"什么时候能被判住"排：

1. **pair-setup**。现在这份配对记录是靠参考实现落的（`lockdown remotepairing --pair`
   那条免弹窗的路），产品自己只会 verify。要补的是 SRP-3072/SHA-512 那几步 +
   M5 里那份 OPACK 设备信息。好消息是这条**不需要人碰手机**就能判：USB 那条控制面
   免弹窗，所以它属于"我手上就能测完"的一类。
2. **发现**。地址现在手动给（`--wifi 10.24.24.7`）。要么自己写 mDNS 广播查询，
   要么问 lockdown 的 `WiFiAddress`（§22.1：那条已经够定位了，只是端口仍来自广播）。
3. **HID 在无线上落不落地**。注入服务连的是"设备反推回来的 TCP"，隧道换了底层之后
   这条路没重测；而且按 §15 的教训，判"落地"要用区域差分，不能信返回成功。

### 22.6 三件欠账结了两件：记录换成自己的，HID 在无线上验过（实测，2026-09-29 05:41–05:43）

§25.8 把 pair-setup 跑通之后，无线这条路**全程不再碰参考实现**。同一台设备、同一条
Wi-Fi（10.24.24.7），用 scrctl 自己落的记录：

```
scrctl --wifi 10.24.24.7 --no-window --no-audio --stats --time-limit 20   # 不给 -s
  用目录里唯一一条配对记录（设备尾号 ****801E）
  完成：渲染 987 帧   全程均 48.9 fps   序号缺口 0   重起 0   PLI 0
```

`--wifi` 现在不给 `-s` 也能跑：记录按 UDID 存而手上只有 IP，目录里正好一条就用它，
多于一条把候选尾号列出来（mDNS 发现 #49 落地前的过渡办法，见 898b019）。

**HID 在无线上落地**，两条判据都是"回读整屏看变化"，不是"注入返回成功"：
- `--test-button home`：手机从设置页回到主屏（回读帧里是 App 资源库）；
- `--test-touch 0.5,0.35,0.5,0.75`（下拉）：App 资源库变成聚焦搜索，**连键盘都弹出来**。

反例也记一条：在 App 资源库第一页横滑（0.85→0.15），注入报"已注入"、整屏一个像素
都没变。这一条**不当结论用**——没进一步 isolating 是"手势在该页本来无效"还是"被吞"；
它的作用只是提醒：§15 那条"返回成功≠落地"在无线上同样成立，判据必须选必然改屏的动作。

设置页上同时挂着两条活记录、都叫 YJBeetle-M2（一条 macOS/Xcode 的、一条我们的），
下面"最近取消配对"里躺着被撤销的那条——identifier 加后缀不顶掉系统记录这个设计，
在设备 UI 上看得见。

**仍未结**：发现（#49，地址手动给）、两条隧道同时开与弱网（#50 余下）。§22.5 那句
"先后各跑一次不等于同时开"依然有效，别拿这一节当并发结论。

## 23. 那台 iPad mini 到手了：§21 留的 (a)/(b) 两问都有答案了（实测，iPad11,2 / iPadOS 18.7.8 / USB）

§21 里那台别人反馈连不上的 iPad mini，现在有一台同形状的在手边（iPad11,2 = iPad mini 5，
iPadOS 18.7.8 build 22H352，开发者模式已开、已信任）。留的两个解释如今都能判：

**(a) 成立，而且是第一层。** 到手时 `pymobiledevice3 mounter list` 是 `[]`（什么都没挂），
RSD 目录 58 条服务、`com.apple.coredevice.*` 一条都没有——与 §21 那份日志的形状逐项对上。
挂上 DDI 之后目录变成 **79 条**，`displayservice` / `screencaptureservice` / `hid.indigo` /
`hid.universalhidservice` / `deviceinfo` 全在。所以"目录里没有 coredevice 族"首先就是没挂 DDI。

**(b) 也成立，在更深一层。** DDI 挂上、服务都在，但媒体流这条路在 iPadOS 18 上是空的：

```
getmediasupportinfo -> {supportedFeatures: 0,
                        supportedFeaturesDescription: "No supported features are available:  (Raw Value: 0)",
                        avcFrameworkVersion: "2125.2.1"}
startmediastream    -> 失败：Remote control requires iOS 27.0 or later on this device.（code 9021）
```

后一句是**设备自己说的版本门槛**，不是我们猜的；前一句是旁证（对照 iOS 27 那台 iPhone 回的是
`supportedFeatures: 972`）。结论：镜像/音频这条媒体流是 **iOS 27+ 的能力**，iOS 18 上服务名在、
feature 列表在、但一调就被设备按版本拒。这就是支持范围，得写进 README。

**第二路独立旁证（苹果第一方客户端，同一天）**：在同一台 Mac 上走苹果自己的屏共享入口连这台
iPad，弹的是「Screen Sharing Unavailable — <设备名> must be running iOS 27.0 or above to screen
share with this Mac.」——文案里写死了 27.0。至此这个边界有两路互不相通的来源：设备 RPC 的原话
（code 9021）与苹果自家 UI 的文案，两边对得上。

**iOS 18 上还能用的两样**（都是量出来的）：

- 截图服务：`capturescreenshot` 回 41384 字节 PNG、1536x2048，头校验通过。兜底路在这台设备上通。
- HID 按键面（`hid.indigo`）：息屏状态下按 home，屏幕醒了（截图从全黑变成有内容）。落地。

**触摸面也落地——先前"不落地"的结论作废，作废原因逐条记在这里**。23:30 的单进程判据：无边记
白底画布上连画两笔，区域差分 **3686 / 9542 像素**，PNG 体积 461404 → 478292 → 947570 字节，
回读的截图里就是一个 X。之前那两次"逐像素差 0"每一轮都有各自的假基线：

1. 横拖翻页那一笔，我们站在**最后一页**上往左拖——本来就不会翻页，零差分是应该的。
2. 其余几轮的基线拍在**睡眠黑屏**上：这台上屏幕几秒就睡（而且会直接自动锁），睡眠时截图服务
   回纯黑 PNG，"前后相同"什么都说明不了。有一轮我以为是"唤醒后拍的"，其实唤醒到拍基线之间
   隔了跨进程的一两秒，够它再睡过去。
3. 下拖叫 Spotlight 那一笔零差分，还有一个更朴素的解释：手势太快没被识别（24 点 × 12ms ≈ 300ms）。
   它当时是唯一的"负证据"，单点不成结论。

改法是给 `hid_probe` 加了**按命令行顺序执行的动作队列**和 `--shot`：唤醒、截图、手势、截图全部
活在同一个进程里，间隔毫秒级，锁屏抢不跑。§15 的"判落地用区域差分"因此要加两句：**基线必须与
动作同进程拍摄**；**先确认基线不是睡眠黑屏**。另有一件没对上、也不再当结论依据的事：用户目击
到"拖拽时屏幕在动"（当时前台是个全屏游戏），而同一时段的截图差分是 0——全屏游戏在前时截图服务
是否冻结，没有单独判过，留作未解释项。键盘面仍未测（要先有聚焦的文本框）。

**所以 iOS 18 的能力边界更正为**：媒体流（镜像 + 音频）被设备按版本拒（9021，§23 开头）；**截图、
按键、触摸注入三样都可用**。对 MaaFramework 控制单元的含义：在 iOS 18 设备上"看（截图轮询）+
操控（触摸 / 按键）"是齐的，缺的只是实时视频流与音频——兜底形态就是截图轮询（任务清单里的
"截图兜底镜像"）。

### 23.1 DDI 挂载怎么操作（以及 scrctl 不自己做这件事）

- 挂：用 Xcode 连一次这台设备（Window → Devices and Simulators），或命令行
  `pymobiledevice3 mounter auto-mount`——它会按设备的 build 从 Apple 拉对应那份**个性化**
  DDI（TSS 签名那一步约 2 秒），实测 exit 0 后 `mounter list` 非空、目录 58 → 79 条。
- 查：`pymobiledevice3 mounter list`（空 = 没挂）。
- 卸：`pymobiledevice3 mounter umount-personalized`；**重启也会自己卸**，所以这件事天然可逆，
  不是对设备的持久改动。卸需要设备处于解锁态——锁着时回 `{'Error': 'DeviceLocked'}`（实测两次）。
- scrctl **不自己挂**：它只在报错里把"没挂 DDI"这件事说清楚（§21 第 3 条那段目录诊断）。
  自动挂意味着要替用户下载并 personalize 一份镜像，这个动作留给 Xcode / 仪器更显式。

### 23.2 同一段目录诊断打三遍，等于没打（§21 第 3 条的后续）

§21 把"目录里没有 X 服务"改成了整段目录诊断，但没改**它会被打几次**：问显示几何、挂转屏订阅、
起流这三步各自失败各自打一遍，没挂 DDI 的设备上同一段话连着出现三次（用户贴回来的报错里就是
三次）。现在 `LiveSource::start` 先看目录里 `com.apple.coredevice.*` 是不是零条：是零条时前两步
闭嘴——它们在这种情形下**必然**失败，而致命的起流那步一定会把整段诊断打出来。真机验证靠卸 DDI
造现场：改前 `grep -c "请把这一段整段贴回来"` = 3，改后 = 1。目录正常但单独缺 `deviceinfo`
的设备（族非空）行为不变，两处警告照打。

## 24. 截图兜底镜像：iOS 18 上"能看能操作"的那条路（实测，iPad11,2 / iPadOS 18.7.8）

§23 定下的边界是"媒体流被设备按版本拒，但截图 / 按键 / 触摸都可用"。兜底镜像就是把这三样
拼成一个能用的产品形态：**轮询 screencaptureservice 当画面源**，接进与实时流同一条
`FrameSource` 接口——窗口渲染、裁剪、触摸映射、`--stats` 全部复用，触摸 / 按键注入走的还是
同一条 HID 路，所以"看是 2 fps、操作是即时的"。

真机读数（`--no-window --stats --time-limit 8`，前台是无边记画布）：

```
兜底镜像已建立：iPad11,2 / iOS 18.7.8，截图 1536x2048（约 2 fps）
  渲染 9 帧（本段 1 帧 = 1.0 fps，全程均 2.2 fps）
  兜底截图: 画面  0.98/s 累计 9 张 / 6816 KB 失败 0
```

画面速率 2~3 fps（画布满时 PNG 约 750KB/张；主屏那种简单画面更快），失败 0。帧率上限就是
单次截图 RPC 的耗时：10 张连拍 5.6 秒（含建会话），即 **0.45~0.5 秒/张**。

**两个真机撞出来的实现约束**（都是第一版红了之后才量清的）：

1. `screencaptureservice` **一条连接只服务一次请求**。第一版握一条长连接复用，第二张开始
   全失败（失败计数 +4/s 稳定增长）；探针连拍十张之所以成，是因为它每次调用都新开连接。
   改成每轮自己开一条新连接后失败归零。建连接的开销本来就算在那 0.5 秒里，不是额外代价。
2. 设备回的 PNG **像素格式随画面内容变**：纯色/简单画面与画布画面解出来的 AVPixelFormat 不是
   同一个（第一版白名单 RGB24/RGBA，换到画布屏立刻红）。改成一律 libswscale 转 BGRA——
   libswscale 与 libavcodec 同在链接面里（CMake 的 pkg_check_modules 一起要的），不新增依赖。

**降级的门**：只在设备原话带 "requires iOS"（code 9021）时自动降到兜底，并打一行说明；
`--video-source=screenshot` 可以强制走兜底（测试与将来别的用途）。**别的失败不自动降**——
比如另一客户端占着流时也降到 2 fps，会把真问题盖住，用户只会看到"卡"。

几何上兜底路比实时流简单两档：截图的像素尺寸**就是**可见区尺寸（没有 HEVC 的 CU 对齐填充），
且截图已按界面方向摆正（朝向恒报 0）。iOS 18 上 `deviceinfo` 不在目录里、问几何必然空手，
所以兜底源拿到第一帧时把尺寸补进几何读数里。`--record` 在兜底路忽略（没有码流可录），打一行说明。

**还没验的**：窗口里的真机手感（无头跑的读数只证明帧在流、几何对；窗口缩放 / 触摸分母在
兜底路上没真机点过），以及横屏时截图摆正与触摸逆映射的组合。这两件等一次开窗口的真机会话。

### 24.1 设备归还前的最后一轮：这台 iPad 还能回答的都问完了（2026-09-29 凌晨）

设备要归还，所以这一轮只跑"只有它能回答"的问题，逐项读数：

- **窗口真机会话（用户亲手跑）**：`./build-cmake/scrctl` 直接起兜底路，24 帧、"控制已接通"、
  窗口 742x990 点 / 视口 1536x2048 / 裁剪全幅 / 转正 0°，全部与无头读数一致；用户评语"真机手感
  勉强能用"。窗口里鼠标拖拽在无边记画布上留下一团连续笔迹——**窗口→触摸映射的活证据**
  （§24 末尾挂账的"窗口手感"至此有了主观+映射两半；剩横屏组合，等设备再入手）。
- **launchapplication**：`--start-app com.apple.Preferences` 落地，回读截图是「设置」。
- **剪贴板往返**：写 18 字节、读回 18 字节、内容一致。`--list-apps` 正常出表。
- **发现（#49 的设备侧一半）**：这台 iPad 在局域网广播 `_remotepairing._tcp`，
  `sssssssuriel.local.:49152`，TXT 为 `identifier=<UUID> authTag=7PPkfUpT ver=24 minVer=8 flags=0`
  ——与 iOS 27 那台同构，只是 `ver` 是 24（iPhone 是 26）。端口仍是 49152。
- **键盘面：落地。** 前两次尝试被现场状态污染（无边记把下拖当画画；Spotlight 在敲键前消失），
  第三次由用户把 Spotlight 搜索框保持聚焦，`hid_probe --keys 512` 敲 a/b/c，回读截图里搜索框
  就是 "abc" 且联想结果跟着出来——与触摸面、按键面同属 universalhidservice 的三个面在 iOS 18
  上**全部落地**。教训留着：`--keys` 目前还是"队列之后的旧动作"，会排在 `--shot`/`--button`
  后面，做这种时序敏感的判据时要记得它不在队列里（下一台设备上把它进队列）。
- **Wi-Fi / pair-setup 不在这一轮**：按用户指示留到其 iPhone 上做（协议栈与版本门槛无关，
  媒体流的 9021 不影响隧道那一层；且给借来的设备写配对记录不合适）。

## 25. pair-setup：iOS 27 上被设备拒收的追查（已解决，答案在 25.8：门在传输层）

这一节记的是"做完了哪几段、被什么挡住、挡住的证据是什么、最后怎么通的"。结论先行：
**pair-setup 的实现（SRP-3072 + OPACK + M1..M6 + createRemoteUnlockKey）与离线判据一直是齐的，
挡住的从来不是我们的字节，而是载体**——iOS 27 只在 RemoteXPC 入口上受理 pair-setup，字节流
入口一律在 M1 就把连接 invalidate 掉。把同一套信封改由 XPC 字典承载之后当场走通（25.8）。
25.1–25.7 保留为追查记录：那十一条被逐条否证的假设正是"门在传输层"这个答案的证据链。

### 25.1 已经落地的部分

- `src/wifi/Srp.{h,cpp}`：SRP-6a（RFC 5054 3072 位模数、SHA-512），公式与参考实现逐条对齐，
  离线拿 python srptools 当 oracle 对过 A/K/M1/M2。
- `src/wifi/Opack.{h,cpp}`：Apple 的 OPACK 二进制对象格式（M5/M6 的 INFO 用），离线拿
  python opack2 的编码结果逐字节对过。
- `src/wifi/PairSetup.{h,cpp}`：`pair_setup()` 走完整条链：先按 pair-verify 的路数问一次
  "你认不认识我"（见 25.2 第二条），再 M1→M2（SRP 参数）→M3→M4（双向证明）→M5（host 的
  Ed25519 密钥 + OPACK 设备信息，ChaCha20-Poly1305 封在 PS-Msg05 的 nonce 下）→M6（解出
  设备的 altIRK）→ 装主密钥 → createRemoteUnlockKey（失败不致命）。
- `tools/wifi_probe --pair-setup`：两条控制面都能跑——USB 的
  `com.apple.dt.remotepairingdeviced.lockdown`（要 TLS）与 Wi-Fi 的 mDNS 手动配对端口；
  成功后落盘并**重开一条连接用新记录 pair-verify 验收**（设备配对完会关掉旧连接）。
- 离线判据 100 条全绿（`wifi_test`）：uuid3 的三个 KAT、密钥生成、以及 pairingData 管道对
  `awaitingUserConsent` / `pairingRejectedWithError` / 认不出的形状这三种回信的处理。

identifier 默认取 `uuid3(DNS, 主机名 + ".scrctl")`，**不**用苹果那个 `uuid3(DNS, 主机名)`。
这一条本来只是"怕顶掉别人的记录"的保守选择，25.3 的 oslog 把它变成了有证据的决定：设备钥匙串里
已经躺着一条 identifier 正好是 `uuid3(DNS, 主机名)` 的记录（macOS/Xcode 自己那条），identifier
相同就是同一条记录，pair-setup 会把它的 Ed25519 公钥换掉。

### 25.2 两个量出来的协议细节

1. **handshake 之后设备停在 `deviceAwaitingPairVerify`**，要先走完一轮 verify（设备回 ERROR）
   才肯谈 setup。直接发 setup 的 M1，症状是连接被立刻掐掉、一句解释都没有。设备 oslog：
   `socket-8: ControlChannel connection state changing from deviceAwaitingPairVerify to
   deviceValidatingPairingPolicyInProgress(initialPairingData: ... setupManualPairing ...)`
   紧跟 `Invalidating control channel connection due to reason: <private>`（0.3 毫秒后）。
   参考实现也是这么绕的：它拿一把全零密钥去签 verify，吃一个 ERROR，再继续 setup。
2. **`pairVerifyFailed` 这句收尾事件在 iOS 27 上会把连接掐掉**（参考实现照发不误，疑似它在
   新系统上这条路本来就坏了）。oslog：收到该事件后
   `state changing from verifyManualPairingInProgress to invalidated`。所以 `pair_setup()` 里
   那一轮 verify 探针**不发**这句——`pair_verify()` 有个 `announce_failure` 开关管这件事。

### 25.3 挡住我们的那道墙（以及为什么判定是设备侧）

两条控制面、解锁状态、屏幕亮着（截图确认过两次主屏）、连试三次、换参考实现，症状一致：M1 发出，
设备 `Received pairing data from peer` 之后立刻 `Invalidating control channel connection`，
Wi-Fi 面直接关连接，USB 面连 FIN 都不发（我们这边 read 永久挂住——已给控制面加了 120s 读超时，
把"挂死"变成"报超时"）。

同一窗口里设备 oslog 还有两条值得记的：

- `Fetching paired peer with identifier AC106655-9E9F-3445-96B3-075257AF1912`（以及另外三条）
  ——苹果自己的工具用的 identifier 就是 `uuid3(DNS, 主机名)`，见 25.1 末段。
- `ManagedConfiguration: Pairing is allowed pending user acceptance.` /
  `remotepairingdeviced: ManagedConfiguration approved pairing.` ——MDM 那条策略链**批准**了，
  但这两行出现在连接已经被掐掉**之后**。要么是这个 daemon 的策略应答天生慢一拍（掐连接是另一个
  本地检查干的），要么掐连接本身就是"策略还没答完就先拒绝"的实现方式。原因字段是 `<private>`，
  不开 private-data 日志拿不到，而没有越狱开不了。

参考实现当 oracle 的那一趟特意用了另一个 identifier（`uuid3(DNS, 主机名 + ".pmd3probe")`），
不碰这台 Mac 已有的记录；它同样以超时告终。

### 25.4 剩下要问的

- 那个"本地检查"到底是什么：iOS 27 是否要求手动配对由**设备侧**发起（设置里某个入口），或要求
  连接带 mDNS TXT 里的 authTag（新主机拿不出来），或要求某个我们没开的开关。等下一台设备或用户
  在设置里翻一遍再试。
- 一旦 M1 能被收下，后面的 M2..M6 只有真机才能验；`wifi_probe --pair-setup` 已经把验收
  （重连 + pair-verify）接在落盘后面，跑通一次就是端到端的判据。

### 25.5 设置页给的第二批事实（实测，同一台设备，2026-09-29 凌晨）

设置 → 开发者 → 配对的电脑 这一页（含每条记录的详情页与「近期动态」）把 25.3 的墙又削掉了几块：

- 「最近取消配对」里那条 `YJBeetle-M2.local`，详情页写着**序列号 `AAAAAAAAAAAA`**——正是
  参考实现 pair-setup 时往 OPACK 里塞的 `remotepairing_serial_number`（我们也照抄了这一个）。
  所以那条记录就是 9/28 04:14 参考实现 pair-setup **成功**建出来的那条，identifier 正是
  `AC106655-…`，也就是我们 Wi-Fi 路记录文件一直在用的那把。**它已在 9/29 00:52（最后一次
  会话）与 02:57 之间被设备撤销**；这个时间窗覆盖本轮 pair-setup 尝试，最自洽的解释是设备
  收到同主机名的新配对请求时先撤销旧记录、再回滚没走完的新记录——即**我们的尝试大概率是
  弄没这条记录的直接原因**（承认，并负责恢复）。
- 撤销之后我们的记录 pair-verify 报"设备不认识这条记录"：Wi-Fi 路当前是断的（USB 路不受影响）。
- 又否证两条假设：换全新的 `sendingHost`（`--host-name`）发 M1 仍被掐（"按主机名冷却"不成立）；
  用**已存在**的 identifier（`--host-id AC106655-…`）发 M1 仍被掐（"只许给已有记录换钥匙"不成立）。
  累计否证九条，见 25.3 与本节。
- 剩下唯一没验的结构差异：9/28 成功时配对列表里可能**没有任何活记录**，而现在还剩一条
  macOS 建的活记录。判据实验 = 在列表页对最后那条点「取消配对」再跑 pair-setup。这一步是
  删用户活记录的破坏性点击，且本轮实测**我们在这台 iPhone 上的单点触摸坐标不可靠**（此前
  "注入落地"的判据都在 iPad 上、且是 stroke/差分这类大区域判据），所以这两下留给人的手做，
  不用盲点做。

### 25.6 成功样本抓到了：差异不在字段，在传输层（实测，2026-09-29 凌晨，Xcode Pair 当 oracle）

用户在 Xcode 设备窗点 Pair（USB 面）能成功。抓设备 oslog 把成功序列与我们的失败序列逐行对：

成功序列（通道名 `remotexpc-8`）：
1. handshake → `deviceAwaitingPairVerify`；
2. verify 的 M1 → 设备日志原话 "**Not paired with anyone, failing pairVerify**"，回一条带 ERROR
   的 pairingData（活记录为零时它连 SRP 都不进）；
3. 客户端**不发 PV-Msg03**，回一句 `pairVerifyFailed` → 状态落 `unauthenticated`（连接活着）；
4. pairingData **kind = `upgradeNonAutomationLockdownPairing`**（不是 `setupManualPairing`），
   startNewSession=true、data 6 字节、sendingHost="YJBeetle-M2"（不带 .local）、pairingOptions=nil
   → `upgradeLockdownPairingInProgress` → 三轮 SRP → `authenticated`。

我们此前失败的三个真实原因，按发现顺序：
- kind 错：`setupManualPairing` 在这台设备上两条字节流面都被直接掐掉；
- 序错：M2 带 ERROR 时我们还发 PV-Msg03（序外消息），之后无论发什么都被掐——"pairVerifyFailed
  掐连接"这个早先结论是错的，掐连接的是它前面那条 Msg03（`pair_verify()` 与 `pair_setup()` 都已
  按成功样本改正：M2 见 ERROR 就只回 pairVerifyFailed）；
- **传输层错（最终答案）**：把上面全改对、活记录清零、M1 字段与成功样本逐字一致之后，
  `socket-N`（USB 字节流面）上依旧 `unauthenticated → invalidated`。成功样本走的是 `remotexpc-N`
  ——lockdown 起出来的 RemoteXPC 入口。设备把 pair-setup 的受理 gate 挂在传输层：只认带会话
  凭据的 RemoteXPC 入口，字节流入口一律拒。参考实现只说字节流面，所以它在这台设备上也永远
  失败（与 25.3 的 oracle 实验一致）。

顺带修正 25.3/25.4 的两条旧结论：活记录条数、主机名冷却、identifier 已知/未知都**不是** gate
（各自单独否证，见提交历史）；"M2 带 ERROR 时设备不回话"也不对——回话与否取决于活记录是否为零
（为零时 M1 就被拒并回 ERROR，有活记录时进 SRP 再在 Msg04 回 ERROR）。

### 25.7 剩下的路

pair-setup 要通，得把配对通道搬上 RemoteXPC 入口：lockdown StartService 起 remotexpc 那个服务
（名字待枚举），复用 `src/http2` + `src/xpc` + `src/remote/RemoteXpc.cpp` 那套栈（#48 读 RSD 用的
就是它），RPPairing 的 JSON 信封改由 xpc 消息承载。代码侧本轮已把协议层改对（kind 可配、
verify 探针用真钥匙、M2-ERROR 的正确收尾），离线判据 100 条仍全绿；差的只是这层传输。

### 25.8 通了：载体搬上 RemoteXPC，字段一个没改（实测，2026-09-29 05:31，同一台设备）

**结果**：`wifi_probe --pair-setup-xpc --pairing-kind setupManualPairing` 全程走通，记录落盘
`~/.local/share/scrctl/remote-00008110_000429000209801E.pair`，随后**另开一条控制面**用这条新
记录 pair-verify 通过。设备侧 `remotepairingdeviced` 的钥匙串条目从 1 条变 2 条，并且能按我们的
identifier 查到（日志原话 `Found paired peer matching query`）——macOS/Xcode 自己那条（identifier
`15C2DCDC-…`）没被动过，这正是 identifier 加 `.scrctl` 后缀想要的效果。

**入口在哪**：不在 lockdown 的服务名里，而在**隧道内的 RSD 服务表**里。`--usb-services` 打出 85 个
服务，唯一与配对相关的字节流入口是 `com.apple.dt.remotepairingdeviced.lockdown.shim.remote`
（UsesRemoteXPC=false，就是 25.3 里被掐的那条）；真正的 RemoteXPC 入口是
`com.apple.internal.dt.coredevice.untrusted.tunnelservice`（UsesRemoteXPC=true，entitlement
`com.apple.dt.coredevice.tunnelservice.client`，端口每次变：实测 61615/61703/61797/61885）。路径是
USB lockdown → `com.apple.internal.devicecompute.CoreDeviceProxy` → 包隧道 → 用户态栈 → RSD →
该服务的 RemoteXPC 连接。

**载体长什么样**（量出来的，来自参考实现 + 真机往返）：
- 连上后设备**先自报一句** `{ServiceVersion: 2}`，要在发 handshake 之前消费掉；
- 每条信封裹进 XPC 字典 `{mangledTypeName: "RemotePairing.ControlChannelMessageEnvelope",
  value: <信封>}`，发送**不带** WANTING_REPLY，回信照样来；
- 类型规则三处：`pairingData._0.data` 与 `message.streamEncrypted._0` 必须是 **XPC data**
  （字节流载体上它们是 base64 文本），`sequenceNumber` 必须是 XPC uint64，
  `wireProtocolVersion` 是 int64；回信方向 data→base64、uint64→整数、UUID→8-4-4-4-12 文本，
  于是两种载体交给上层的形状完全一致，`pair_setup()`/`pair_verify()` 一行没改。

**kind 决定要不要"授权"**（两条都是设备原话）：
- `upgradeNonAutomationLockdownPairing`（Xcode 成功样本用的那个）→ `pairingRejectedWithError:
  This host is not authorized to complete promptless pairing`。同一趟 handshake 里设备自报
  `allowsPromptlessAutomationPairingUpgrade=是`，所以这不是设备能力开关，而是**按主机授权**：
  Xcode/macOS 有资格走免提示升级，我们没有。
- `setupManualPairing` → 设备回 `awaitingUserConsent`、屏幕上弹提示、点了之后三轮 SRP →
  `SRP 双向证明通过` → M5/M6 → `authenticated`。

**同意窗口约 29 秒**（量出来的）：`awaitingUserConsent` 之后设备在 ~29s 自己写下
`The notification was cancelled`，14 毫秒后 `Lockdown tunnel connection receive error`，整条面拆掉。
第一趟就是这么死的——当时误以为"是不是谁点了不要"，日志证明是超时。所以探针里的音效要绑在
"设备说在等同意"那一刻响，而不是绑在进程启动那一刻。

**那张提示不是 lockdownd 的「信任此电脑？」**（用户观察 + 推断）：苹果自己那条是中文的，我们这条
渲染成英文。结合日志里的进程名，推断它来自 `remotepairingdeviced`/CoreDevice 那层的开发者配对
提示，与 lockdown 的信任 sheet 是两套 UI。这一条只是解释现象，不影响实现。

**这台设备在该面上自报的 handshake**：`wireProtocolVersion=26`（我们仍发 19，设备接受）；
`deviceOptions`: allowsFreePairing=否、allowsIncomingTunnelConnections=是、allowsPairSetup=是、
allowsPinlessPairing=是、allowsPromptlessAutomationPairingUpgrade=是、allowsSharingSensitiveInfo=是；
设备自报 identifier 尾 4 位 = `801E`（就是它 UDID 的尾巴）。

**代码侧的形状**：`Rppairing` 底下抽出 `EnvelopeCarrier`（`write_envelope`/`read_envelope`/
`wait_readable`），字节流帧成为 `FramedCarrier`，RemoteXPC 成为 `remote::XpcPairingCarrier`
（`src/remote/PairingChannel.*`，含 JSON↔XPC 转换与那三条类型规则）。离线判据从 100 条增到 113 条
（新增的全部针对转换器：三条类型规则、往返等价、UUID 文本化、"别处的 data 不当二进制"）。
25.6 那十一条已否证的字段假设一条都不用翻案：**门确实只在传输层**。

### 25.9 事后才看到的门牌：`deviceOptions.allowsPairSetup`（实测，2026-09-29）

用新记录在 Wi-Fi 面（`10.24.24.7:49152`，字节流载体）跑 pair-verify + createListener，全通：
`pair-verify 通过` → `createListener 给了端口 62165` → 端口连得上。同一趟的 handshake 里，
设备把 25.6 那道门**自己写在脸上**：

| deviceOptions | RemoteXPC 面（USB 隧道内） | 字节流面（Wi-Fi 49152） |
| --- | --- | --- |
| allowsPairSetup | 是 | **否** |
| allowsPinlessPairing | 是 | **否** |
| allowsPromptlessAutomationPairingUpgrade | 是 | **否** |
| allowsIncomingTunnelConnections | 是 | 是 |
| allowsFreePairing | 否 | 否 |
| allowsSharingSensitiveInfo | 是 | 是 |

也就是说：字节流面并不是"讨厌我们的 M1"，它**根本不接受 pair-setup**，而且提前就说清楚了。
`pair_setup()` 现在在 handshake 之后、发任何 pairingData 之前读这一句，不收就立刻失败并把设备
原话交出去（真机实测 0.44 秒返回；离线判据 116 条，含"判失败前不得发出 M1"这一条）。

这条教训比协议事实更值钱：**先读对端自报的能力位，再去逐字段对差异**。25.1–25.7 那十一条假设
全部是在没读 `deviceOptions` 的情况下烧掉的。

## 26. 运行中降级：终于有真机读数了（实测，2026-09-29，iPhone 13 mini / iOS 27.0 / USB）

"媒体流跑着跑着解不出画面 → 切截图兜底 → 泵回升再切回来"这条路连着三轮审查都在改，而
**一直没有设备端读数**：触发器打不响（要画面复杂到编码器交出超过解码后端上限的帧，或连续
三次重起都拿不到关键帧；静止画面上 45 秒 0 次）。现在有了 `--test-degrade T1,T2,...`——
从起流那一刻算起，到点交替"强制判媒体流解不出画面 / 放开"，它只顶 `video_dead` 那**一个
入参**，`pick_picture_source` 与 `next()` 里的切换代码一行没改，所以下面三组读数对真降级
同样成立。

### 26.1 切换本身（`--test-degrade 4,8,12,16 --time-limit 20 --no-window --stats`）

| 时刻 | 事件 | 判据 |
| --- | --- | --- |
| 4.35 s | 降级 | 5.48 s 起每秒 1 帧；第一张约 1.1 秒到（截图 RPC 实测 0.2~0.6 秒 + 起 worker） |
| 8.74 s | 切回实时流 | **0.17 秒**后那一段就有 9 帧，随后回到 41 fps |
| 12.71 s | 第二次降级 | 1 秒内出第一张，与第一次同节奏 |
| 16.36 s | 第二次切回 | 0.34 秒后 16 帧，随后 41 fps |

全程 `--stats` 每秒都在打、**没有空洞**。这就是"切换不冻取帧循环"的判据：`stop()` 若还在
join（上一轮 P3），这里会出现最长 5 秒（`kCaptureTimeoutMs`）的静默段。

### 26.2 序号不归零会怎样（A/B，同一台设备、同一时间表 `4,16,20`，`--time-limit 26`）

| | 修好的 | 故意不归零 `shot_serial_` |
| --- | --- | --- |
| 第二次降级（20.4 s）之后 | 21.76 s 起每秒 1 帧，收尾 **350 帧** | **渲染数死在 349 帧**，此后 6 秒每段 0 帧 |

第一轮跑了 12 秒、累计 14 张，所以未修那一版要等新源的序号爬过 14（≈14 秒）才出画面——
审查说的"上次截图用了多久，第二次就可能要等相近的时间"，这下有了设备端读数。

**判据教训（我自己踩的）**：第一版 A/B 我盯的是 `兜底截图: 累计 N 张`，两版**一模一样**，
差点判成"这条修复没有可观测差别"。那一行是 worker 的账（它一直在截图），不是 `latest()`
交出去的账。换源这类问题的判据必须看 **`渲染 N 帧`**。

### 26.3 退役源不回收会怎样（A/B，10 次切换、每 2 秒一次，`--time-limit 40`）

| | 每帧回收（现在） | 故意不回收 |
| --- | --- | --- |
| 预热后的 RSS | 216 MiB，此后 **32 秒一动不动** | 213 MiB 起，**每 2 秒一级台阶 +10.5 MiB**：224 / 234 / 245 / 255 / 266 / 277 / 287 / 298 |

台阶高度与一张解码好的 BGRA 画面对得上：设备报的截图尺寸 1125×2436，×4 字节 = 10.45 MiB。
（RSS 用 `ps -o rss=` 每秒采一次；两版都正常退出，854 / 856 帧。峰值差 81 MiB ≈ 8 MiB/次，
比 10.45 略小，是分配器复用的量。）

### 26.4 开关打开的第一趟就撞出来一个 bug

第二次降级那一刻打出 `兜底截图: 画面 18156244167036960768.00/s 累计 0 张`。截图速率是
uint64 做差，换源时新源从 0 重数、而上一次的累计还留着 → 下溢。泵那侧**早就**防过同一件事
（`print_stats` 里的 `dev_reset`，注释写着"重起会话会让设备侧的累计数归零，做差会下溢成一个
天文数字"），截图侧没有，因为在有 `--test-degrade` 之前，一次会话里的截图源从来不会被换掉。

值得单独记一句：**没有触发器的时候，连日志里的垃圾数都不会出现**。这个数长得就像读数，而它
出现的位置恰好是唯一能证明"降级切换在真机上跑通"的那几行里——照着它去解读，结论会变成
"截图路炸了"。

## 27. 隧道内 TCP 的乱序取舍：量到占比之后决定不做重组（实测，2026-09-29，Wi-Fi）

`TcpStream` 只接受 `seq == rcv_nxt_` 的段，落在期望之外的一律丢掉、回一个重复 ACK 催对端
重传——不缓存、不重排。这条取舍值不值得改，之前**没有数**：能看到的只有逐段的 stderr，
而它是阵发的，看见 5 行也不知道占多少。

现在收/丢的字节数聚合在 `Stack::tcp_counters()`，`--stats` 打一行速率 + 全程占比，逐段日志
挪到 `--debug-net` 后面。**账记在栈上而不是每条连接上**：兜底截图那条路每截一张就新建一条
连接（这条服务一条连接只服务一次请求），连接级的计数活不到 --stats 那一刻。

量到的（兜底截图路，每次回复约 3.5 MB）：

| 跑法 | 收到 | 丢弃 | 占比 |
| --- | --- | --- | --- |
| Wi-Fi 60 秒（链路干净） | 124.55 MB | 0 段 | 0.00% |
| Wi-Fi 180 秒（同台同晚） | **536.32 MB** | **1 段** | 0.00% |
| Wi-Fi 8 秒（当晚早些时候，`.lan` 解析的 ping 丢一半） | — | 5 段 | 当时还没有这个读数 |
| USB 8 秒 | 0.03 MB（只有控制面） | 0 段 | 0.00% |

**结论：不加乱序重组。** 536 MB 里一次重排，代价是一段重传；而重组要引入一个"最多缓存多少
字节"的闸（否则对端一个远序号就能把我们撑爆，那是 H3/H4 那族"无界增长"的老坑），几十行
代码换一个 1e-6 量级的问题不划算。读数留在 --stats 里，等 Wi-Fi 变成主路径、或占比涨到
百分位，再回来做。

仪器本身也有一条教训：**这一行第一段必须打一次，哪怕丢弃是 0**。第一版写成"只在丢过东西
时打"，结果 60 秒零输出——"真没丢"与"账根本没接到这条栈上"在日志里长得一模一样，而那一次
我差点把 0 直接当结论写进去。自证的办法是把分母（收到的字节）一起打出来。

## 28. 拔线复现 SIGPIPE：设备端 A/B（实测，2026-10-01，USB）

`Socket` 接管 fd 时统一设 `SO_NOSIGPIPE`、TLS 握手入口无条件装进程级 `SIG_IGN`——这两层
此前只有离线负对照（socketpair 里 fork 一个子进程做裸 write，防护关掉时它被信号 13 打死）。
设备端那一趟补上了，两臂同一台设备、同一条兜底截图路：

| 二进制 | 拔线那一刻 | 之后 |
| --- | --- | --- |
| 防护关掉（不设 `SO_NOSIGPIPE` + `SIG_IGN` 那支 `#if 0`） | **退出码 141**（= 128+13，SIGPIPE），与拔线同一秒 | 日志停在 `失败 0` 的正常读数上，**一句错误都没有** |
| 现在的产品版 | 活着 | 跑满 `--time-limit 60`、退出码 0；截图停在 12 张，**失败 193 次**（250 ms 退避 × 51 秒，对得上） |

为什么用兜底截图路：它每约 0.5 秒就往隧道里发一次 RPC，拔线之后**必定**还有写动作
（`Stack::send` 不查泵的死活，直接 `SSL_write`）。流路径的写只有每秒一个 RTCP RR，
撞不撞得上全看时机。

**两次白跑的教训**（比结果本身值钱）：

1. 第一趟用声音提示拔线、人不在场，两臂都跑满 40 秒、截图 **0 失败**。日志里"一切正常"与
   "实验根本没打响"长得一模一样。判据因此必须包含**"设备真的断了"这一半**：失败计数要涨。
   只看"进程活着"不够——没拔线它也活着。
2. 第二趟设备插回后 **1 秒**就起流，隧道 TLS 还没稳，进程自己退了（退出码 1，日志
   `隧道 TLS 读失败`），提示音对着一个已经死掉的进程响。修法两条：插回后给 10 秒安定；
   起流后**先等它真的出图**（3 段 stats）再提示拔线。

顺带量到一条产品行为：拔线之后 scrctl 不退出，而是一路退避重试到 `--time-limit`
（这一趟 193 次失败、0 张新图）。窗口模式下用户看到的就是"画面停住、没有一句解释"，而
scrcpy 是设备断开就退出。**已按"分开处理"改掉并复测**（2026-10-01 21:56）：

| | 改之前 | 改之后 |
| --- | --- | --- |
| 拔线之后 | 一路 250 ms 退避重试到 `--time-limit`（60 秒 / 193 次失败 / 0 张新图） | **1 秒内自己退出**，退出码 0 |
| 用户看到的 | 画面停住，没有一句解释 | `设备断开了（隧道已死：隧道 TLS 读失败），走正常退出路径` |

复测那一趟 `--time-limit` 故意给到 120 秒，而日志里**没有**"达到 --time-limit"——所以退出
确实是"断开"触发的，不是被时限收走。判据用栈自己的 `pump_error()`（只在泵线程因**读失败**
退出时置位，超时不算），**故意不匹配错误文本**：文本会随实现变，而"泵已经停了"是结构性事实。
检查挂在 `next()` 返回 false 之后，所以码流与截图两条路都覆盖。

（这一趟之前还白跑了一次：想用 `lldb` 对进程调 `shutdown(fd, SHUT_RDWR)` 来模拟对端消失、
省掉拔线，结果 attach 要指纹授权，晚按一步就 "could not pause execution"，而且它把目标进程
带崩了——日志戛然而止、没有任何退出消息。**结论：这条路不如让人拔线可靠。**）







## 29. 第五轮审查：两条 P2 与"一本账一把尺"（离线 + 真机，2026-10-01）

两条都成立，各一个提交。这一节记的是**判据的形状**，因为两条都不是"代码写错了"，而是
"仪器自己会撒谎"这一族。

### 29.1 `--test-degrade` 的时刻表：非有限数与装不下的数（离线，UBSan）

`parse_degrade_marks` 用 `strtod` 取值后直接 `static_cast<uint64_t>(secs * 1000.0)`。
`strtod` 认 `nan` / `inf`，也认 `1e400`（溢出成 inf），而负数那一关拦不住 `nan`——它与
任何数比较都是 false。UBSan 在本仓库的 `build-san` 上复现：

```
SourcePick.h:85:47: runtime error: nan is outside the range of representable values
                    of type 'unsigned long long'
```

同一处只报一次，所以 `inf` / `1e400` / `1e18` 那几种在日志里是**被抑制的，不是没发生**。
修之前先加测试行，读到一个值得记的假象：`"4,nan"` 看起来被拒了，报错是"时刻要按升序给"
——那是**碰巧**：垃圾值撞上了升序检查。单独一个 `"nan"` 就静默通过。**判"某个非法输入被
挡住了"，要看它是被哪一关挡的，不能只看返回 false。**

修法两道关：先 `isfinite`，再判"乘 1000 之后装不装得下 uint64"。**不设人为上限**——`1e9`
秒这种装得下的大数照收（新增反面行），它只是永远到不了那一刻；多设一个上限就多一条要
解释的规矩。

### 29.2 切回实时流后第一段 `--stats` 速率虚高：分母与计数基线不是一套账

`print_stats` 里 `last_stats_ms_` 是**两个画面源分支共用的一把尺**（截图分支与媒体分支
各自推它），而媒体泵的计数基线只在媒体分支里更新。截图兜底那段时间里泵仍在后台收包、
音频腿也照收照解（它们是独立会话、独立线程），于是切回实时流后的第一段把**整段兜底期间
的增量除以约 1 秒**：收包 / AU / 解码 / 音频四个速率一起虚高几十倍。

最坏的地方是它**看起来完全自洽**：同一行下面还打着 `分母：我 1.0s`，读者会以为窗口就是
1 秒。这与 §20 那条"两个每秒分母不同却硬减"是同族错误，也正是隧道内 TCP 那一行为什么
早就自带 `last_tcp_ms_`（当时的注释已经承认共用尺有问题，却没往上追一层）。

修法是**一本账一把尺**：`last_stream_ms_` / `last_shot_ms_` / `last_audio_ms_` 各自独立，
并且**在各自的源建好那一刻起表**（泵在 `start()`、截图源在 `start()` 与 `next()` 两个
赋值点、音频腿在 `start()`）——顺手去掉了 `last == 0 → 1.0` 那个假分母：第一段现在打的是
真实经过的时间。分母与做差抽到 `src/app/StatsWindow.h`（`settle_window` / `counter_delta`），
于是规则本身能离线判。

离线判据里**把"共用一把尺"当负对照钉住了**：同一段时间轴跑两种接法，分账尺读 100/s，
共用尺读 3100/s（31 秒里 3100 个包除以约 1 秒）。哪天有人把三把尺合回一把，这段会先红
给他看，不必再等真机重现一次。

**判据边界要说清**：离线那段判的是**规则**，不是 `main.cpp` 用了哪把尺——接线本身没有
离线判据（`print_stats` 要活的泵与设备）。所以补了一趟真机 A/B，用 `--test-degrade 5,20`
在真机上打响"降级 → 切回"，对照臂是从 `ad28cfc`（共用尺）建出来的同一个二进制：

| 切回后第一段 | 对照 `ad28cfc`（共用尺） | 产品 `8215bf2`（分账尺） |
| --- | --- | --- |
| 分母（我那档） | **我 1.0s** | **我 16.4s** |
| 我收到 | **1517/s** | 88/s |
| AU / 解码 | **682.4/s** | 42.0/s |
| 音频 包 / 解出 | **1618/s** | 100/s |
| 音频 交付 | 775509 帧/s | 47940 帧/s |

两臂**同形**：各 230 行、降级都在第 47 行、切回都在第 78 行、兜底截图各 15 行，所以差别
只能来自那把尺。三个交叉核对让这组数站得住：

1. **设备那档的分母两臂都是 16.4s**（`last_dev_change_ms_` 本来就是独立的一把尺），于是它
   成了内置的对照：设备自报 85/s（产品臂）与 91/s（对照臂），与产品臂"我收到 88/s"同量级；
   对照臂那个 1517/s 对着设备自报的 91/s 一眼就是假的。
2. 音频腿读到 **100/s**，与 §17.2 实测的"设备没有声音也照发 100 包/秒"一致；对照臂 1618/s。
3. 产品臂那行"交付 47940 帧/s"是 coreaudio 出口的**正常**读数（§17.2：出口每秒只消化约
   45000~48000 帧，差额由水位导向排掉），不是异常；对照臂的 775509 帧/s 才是。

顺手补的一处：音频行现在把自己的分母打出来（`｜分母 16.4s`）。两本账的窗口从此**可以**
不同，而不同就要看得见——这条 bug 之所以能自洽地骗人，就是因为分母只打了一个。
