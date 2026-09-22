# 范围与分期

功能面以 **scrcpy** 为基准（不是 pymobiledevice3 —— 它太宽，VNC/Web/DVT/文件系统/定位/备份等一概不做）。协议实现参考 pymobiledevice3，但只做减法。

## 分期

| | 内容 | 依赖 |
| --- | --- | --- |
| **M1** | 播放已录制的 Annex-B HEVC：SDL 窗口 + VideoToolbox 硬解 + 尺寸裁剪 + 帧率统计 | SDL2 |
| **M2** | USB 实时流：usbmux → lockdown → `CoreDeviceProxy` TCP 隧道 → lwIP 用户态栈 → RSD → `displayservice` | + lwIP |
| **M3** | HID 注入：`hid.universalhidservice` 触摸屏 surface + 鼠标/键盘映射 | |
| **M4** | 音频（`startaudiooutput`，AAC-ELD）、剪贴板（`pasteboardservice`）、录制、CLI 补全、打包 |
| **M5** | Wi-Fi：RemotePairing over bonjour + QUIC/PSK | + quiche |

**USB 优先**带来一个实质简化：iOS 17.4+ 的 USB 路径是 lockdown 上的 **TCP 隧道**，不需要 QUIC。因此 quiche 推迟到 M5，v1 完全不碰。

## scrcpy 参数映射

已核实可行的（服务/协商参数存在）：

| scrcpy | scrctl | 依据 |
| --- | --- | --- |
| `-s/--serial` | `--udid` | usbmux |
| `--list-devices` | `--list-devices` | usbmux/RSD |
| `--bit-rate` | `--bit-rate` | offer 有 `BITRATE`/`bitrate` tier |
| `--max-size` | `--max-size` | `VRAE` = video-resolution-adaptation，offer 内有 `resolution`/`Width`/`Height` |
| `--max-fps` | `--max-fps` | offer 内有 `fps` |
| `--record` | `--record` | 已有 Annex-B 抓取，加 libav mux |
| `--display-id` | `--display-id` | `supportedFeatures=972` 含 `Video output stream by display ID` |
| `--crop` | `--crop` | 主机侧；且**必须**做（见下） |
| `--rotation` | `--rotation` | `devicecontrol.orientation` + `currentOrientation` |
| `--no-video` / `--no-control` | 同名 | |
| `--no-audio` | `--no-audio` | 音频是独立 PT=101 流 |
| `--window-title` / `--window-*` | 同名 | SDL2 |
| `--keyboard` / `--mouse` | 同名 | 虚拟 HID 键盘 + 触摸屏 surface |
| 剪贴板 | `--no-clipboard` 反向 | `pasteboardservice` |
| `--start-app` | `--start-app` | `appservice.launchapplication` |
| `--power-off-on-close` | 同名 | `devicecontrol` |

明确**不做**：`--video-codec`（设备侧只给 HEVC）、`--show-touches`、`--new-display`（`Virtual external video output stream` 虽在 feature 里，但语义是镜像输出而非扩屏，待验证再定）、`--tcpip`（并入 M5 Wi-Fi）。

## 三条必须遵守的实现约束

1. **裁剪**：编码分辨率 `1136x2464` ≠ 逻辑显示 `1125x2436`（CTU 对齐填充）。渲染裁掉右 11px / 底 28px；**触摸归一化一律用显示尺寸**。
2. **offer 串禁止带 `VRAE:0`**。那是苹果 Xcode 抓包里的值，会禁止分辨率自适应并迫使编码器丢帧（实测 drop 207–219 帧、~42fps 且拖影）。用 `FLS;SW:1;`，drop 为 0、53–55fps。**苹果自己的 Xcode 恰恰是那个慢版本，照抄抓包会踩坑。**
3. **注入前必须有一条运行中的视频流**，且起流后等 ~0.3s 让 backboardd 匹配 surface；停止流必须用**全新的 RemoteXPC 连接**发 stop，复用发起 start 的连接会让设备守护进程崩溃。

## M2 硬性需求：缺口检测 + RTCP PLI

设备是**长 GOP** 流——实测 1104 帧里只有 2 个关键帧。一旦某个 AU 丢包或解不出来，参考链断裂，而编码器不会主动补 IDR，错误会一路累积到永远。

M1 阶段用采集工具拿到了实证：噪点（相邻像素差）从第 1 帧的 0.86 单调涨到第 200 帧的 14.04，亮度从 6.5 涨到 42.8；而**同一份文件 ffmpeg 解出来始终干净**（1.12 / 6.69 不变），证明问题在采集侧不在码流。失败 AU 恰好每 ~58 帧规律出现一次，且都含 `BLA_W_LP(16)` 这类 IRAP 恢复点——即每秒一个恢复点全被我方丢掉。

实时链路必须做两件采集工具没做的事：

- **RTP 序列号缺口检测**：有缺口就丢弃整个 AU（pymobiledevice3 里对应 `au_corrupt` + `fu_buffer.clear()`）。
- **发送 RTCP PLI（RFC 4585，PT=206 FMT=1）**请求编码器发新 IDR。注意 `start_video_stream` 有个 **`allow_rtcp_fb` 默认 `False`**，不开就没有反馈通道。

另一个已钉死的实现细节：`NALUnitHeaderLength` **只能用 2**（论证见 `src/decode/VideoToolboxDecoder.cpp` 注释）——传 4 时 hvcC 会正确记成 `lengthSizeMinusOne=3`，但 VideoToolbox 对该会话的每个样本都回 `-12909 kVTVideoDecoderBadDataErr`。副作用是 NAL 上限 65535 字节；真机流实测最大 17431，安全，已加越界守卫并提示需改走软解。

## 依赖策略

- **SDL2**：`find_package` 优先（系统/brew），失败则 FetchContent 固定 tag
- **M1 不引入 FFmpeg**：macOS 用 VideoToolbox 原生硬解，零外部依赖；libav 作为后续平台的兜底后端
- **lwIP / quiche**：M2 / M5 才引入
- **plist：自己实现限定子集，不引 libplist。** 两个独立理由：
  1. **许可证**：libplist 是 LGPL-2.1，而本项目是 Apache-2.0。FetchContent 会静态链接，从而对组合作品施加 LGPL 义务。自己按规范实现一份就完全干净。
  2. usbmux / lockdown 实际只需要 `dict / string / integer / true / false / data / array` 这几种类型，文法很小，没必要为此背一个跨三平台都要构建的依赖。
- **隧道**：实测 `CoreDeviceProxy` 的"TCP 隧道"是**数据包隧道**（40 字节 IPv6 头 + 头内 u16 长度 + body，写进 `tun`），所以可移植路径必须自带 IPv6+TCP 栈 → 用 lwIP 挂自定义 netif。macOS 上"复用 remoted 已建隧道"虽然能免掉 lwIP，但 RSD 端口要靠 shell 出 `/usr/bin/nettop` 猜（macOS 27 起内核对非 root 只返回本进程 socket，NetworkStatistics 又要求 Apple 签名带 `com.apple.private.network.statistics`）——脆弱且绑 macOS 版本，**决定不做**，直接走可移植路径。
