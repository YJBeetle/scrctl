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
2. **offer 串禁止带 `VRAE:0`**。那是苹果 Xcode 抓包里的值，会禁止分辨率自适应并迫使编码器丢帧（实测 drop 207–219 帧、~42fps 且拖影）。用 `FLS;SW:1;`，drop 为 0、53–55fps。
3. **注入前必须有一条运行中的视频流**，且起流后等 ~0.3s 让 backboardd 匹配 surface；停止流必须用**全新的 RemoteXPC 连接**发 stop，复用发起 start 的连接会让设备守护进程崩溃。

## 依赖策略

- **SDL2**：`find_package` 优先（系统/brew），失败则 FetchContent 固定 tag
- **M1 不引入 FFmpeg**：macOS 用 VideoToolbox 原生硬解，零外部依赖；libav 作为后续平台的兜底后端
- **lwIP / quiche**：M2 / M5 才引入
