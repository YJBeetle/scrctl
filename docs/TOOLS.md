# 开发诊断与归档实验

普通使用从 `scrctl --help` 开始。设备列表、应用列表与启动、剪贴板、统计和录制
已在主程序中提供，分别使用 `--list-devices`、`--list-apps`、`--start-app`、
`--copy` / `--paste`、`--stats` 和 `--record`。

诊断工具保留独立的采样与故障注入能力，供开发时定位问题。协议实验用于复现
特定变量的对照，源码保留在 `tools/experiments/`；实验结果及其适用范围见
[协议记录](coredevice.md) 和各库的兼容性记录。

## 构建与目录

默认构建主程序和离线测试，诊断工具及实验均关闭。它们不随安装包分发。
单配置生成器的可执行文件目录如下；多配置生成器会再增加 `Debug` / `Release`
等配置子目录，CTest 会使用对应目标的实际路径。

| 内容 | CMake 开关 | 默认值 | 可执行文件目录 |
| --- | --- | --- | --- |
| 主程序 | 始终构建 | — | 构建目录根部 |
| 离线测试 | `SCRCTL_BUILD_TESTS` | ON | `tests/` |
| 10 个常用诊断工具 | `SCRCTL_BUILD_PROBES` | OFF | `tools/` |
| 17 个历史协议实验 | `SCRCTL_BUILD_EXPERIMENTS` | OFF | `experiments/` |
| lwIP 适配实验 | `SCRCTL_LWIP_PROBE` | OFF | `experiments/` |
| nghttp2 适配实验 | `SCRCTL_NGHTTP2_PROBE` | OFF | `experiments/` |

```sh
# 只构建主程序
cmake -S . -B build-product -DSCRCTL_BUILD_TESTS=OFF
cmake --build build-product -j

# 开发诊断及其离线参数测试
cmake -S . -B build-tools -DSCRCTL_BUILD_PROBES=ON
cmake --build build-tools -j
ctest --test-dir build-tools --output-on-failure
./build-tools/tools/wifi_probe --help

# 复现历史实验，同时保留常用诊断工具
cmake -S . -B build-research \
  -DSCRCTL_BUILD_PROBES=ON -DSCRCTL_BUILD_EXPERIMENTS=ON
cmake --build build-research -j
./build-research/experiments/pli_probe --dry-run
```

启用历史实验不会自动启用两项库适配实验。nghttp2 需要系统 `libnghttp2 >= 1.50`
及 pkg-config；构建方法与判据见 [nghttp2 记录](NGHTTP2_COMPATIBILITY.md)。
lwIP 实验复用项目固定版本的库，见 [lwIP 记录](LWIP_COMPATIBILITY.md)。

未指定录制码流、且关闭两项库适配实验时，默认注册 26 项离线测试；仅启用常用
诊断为 36 项，仅启用历史实验为 28 项，两者均启用为 38 项。两项库适配再增加
2 项。实际注册以 `ctest --test-dir <构建目录> -N` 为准。
CI 显式开启常用诊断和历史实验，检查它们的编译与离线参数；CI 不连接设备。

重新配置旧构建目录不会自动删除已不再构建的可执行文件。检查新目录或使用全新
构建目录，避免误运行根部遗留的旧版本。

## 常用诊断工具

| 工具 | 用途 |
| --- | --- |
| `feature_probe` | 调用设备 feature 并检查回复 |
| `screenshot_probe` | 截图服务与图片输出 |
| `rr_keepalive_probe` | RR 保活、PLI / FIR 对照及媒体收包统计 |
| `wifi_probe` | USB 远程配对、已有记录的 Wi-Fi 验证和隧道连接 |
| `stall_probe` | 注入视频静默、丢包或解码失败，检查恢复和截图切换 |
| `audio_pump_probe` | 音频收包、解码与播放交付 |
| `audio_decode_probe` | 离线解码录制的音频数据并保存 WAV |
| `applist_probe` | 应用列表服务 |
| `app_launch_probe` | 应用启动服务 |
| `hid_probe` | 按命令行顺序执行触摸、按键、文本与服务查询 |

十个工具均支持 `--lang auto/en/zh-CN`；不指定时按 locale 自动选择。
帮助、状态和错误提示支持中英文；CLI11 校验正文、系统错误及设备响应保留原文。
先看各工具的 `--help`。`hid_probe --dry-run` 可以检查动作计划而不连接设备。
该工具已移除无参数时自动输入的演示：无参数显示帮助；`--no-stream` 单独使用
只连接 HID 服务，不自动发送动作。

```sh
# 先检查执行顺序，不连接设备
./build-tools/tools/hid_probe --lang zh-CN \
  --shot before.png --line 0.2 0.4 0.4 0.6 --shot after.png --dry-run
```

去掉 `--dry-run` 后会执行计划。输入是否生效要看设备画面或前后截图；
没有辅助媒体会话时，已测设备可能不接受输入，`--no-stream` 不保证输入生效。

## 归档实验

| 范围 | 工具 |
| --- | --- |
| 连接与服务结构 | `usbmux_probe`、`lockdown_probe`、`feature_schema_probe`、`display_info_probe` |
| 输入与剪贴板 | `hid_gate_probe`、`pasteboard_probe` |
| RTCP 与关键帧 | `pli_probe`、`fir_probe`、`rtcp_probe` |
| 会话期限、重起与并发 | `lifetime_probe`、`two_session_probe`、`lease_renew_probe`、`restart_gap_probe`、`wake_latency_probe` |
| 原始媒体与码率 | `stream_probe`、`frame_probe`、`bitrate_probe` |
| 库适配 | `lwip_probe`、`nghttp2_probe`（各自单独启用） |

PLI 和六个生命周期实验已补齐 CLI 校验及 `--dry-run`：`hid_gate_probe`、
`lifetime_probe`、`two_session_probe`、`lease_renew_probe`、`restart_gap_probe`、
`wake_latency_probe`。其余历史实验仍按源码文件头的参数约定使用，不能假定
它们均支持 `--help` 或离线预演；部分程序无参数就会连接设备。

会话观察须区分视频静默、SR 停止和设备会话结束。无反馈实验明确关闭 RR；
生产 FramePump 默认仍发送 RR。观察窗口有限，但设备连接、RPC 和清理另有
协议超时，窗口参数不是整个进程的总运行时限。
部分工具的清理调用会停止设备上的所有媒体会话，帮助中已说明；运行媒体实验时
请独占设备，不与主程序或其他媒体探针并行。

归档保留了独立变量及历史证据，没有把它们合并成一个通用命令。继续使用前应
检查具体参数、失败路径和预期结果；退出码为 0 也不能代替画面、声音或输入落地的
设备验证。本次整理只做离线回归，没有重跑真机实验。

## Python 与文件分析工具

`tools/nalsizes.py` 分析录制的 NAL 尺寸，`tools/probe/` 保留早期 Python 探针。
这些脚本不随产品安装，pymobiledevice3 也不是 scrctl 的运行依赖。环境与使用方法
见 [README](../README.md#研究工具)。
