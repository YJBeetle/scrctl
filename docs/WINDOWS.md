# Windows 构建与验证

目前已验证 Windows 11 ARM64、MSYS2 CLANGARM64 工具链。x64 和 MSVC 尚未验证。
应用使用普通 `main`，视频和 AAC-ELD 音频使用 FFmpeg，窗口与音频输出使用 SDL2。
真实音乐离线解码及用户确认的短时 USB 播放已通过；无线播放持续 120 秒正常结束，
用户反馈偶发刺啦声，仍需隔离 VM 输出与缓冲调节的影响。USB 持续连接尚未通过；配置与对照见
[音频记录](coredevice.md#171-音频解码后端只能选-audiotoolbox实测同一份-dump-四路对照)。

## 构建

使用 winget 安装 [MSYS2](https://www.msys2.org/)；本次 VM 验证的安装位置是
`C:\opt\msys64`，项目及构建目录放在 `C:\Workspace\scrctl`：

```powershell
winget install --id MSYS2.MSYS2 --exact --source winget --silent `
  --accept-source-agreements --accept-package-agreements --location C:\opt\msys64
```

打开 **CLANGARM64** 终端。不要在 MINGW64、UCRT64
或普通 MSYS 终端混用下面的依赖；CLANGARM64 生成原生 ARM64 程序。

```bash
pacman -S --needed \
  mingw-w64-clang-aarch64-clang mingw-w64-clang-aarch64-cmake \
  mingw-w64-clang-aarch64-ninja mingw-w64-clang-aarch64-openssl \
  mingw-w64-clang-aarch64-gettext-tools mingw-w64-clang-aarch64-SDL2 \
  mingw-w64-clang-aarch64-python mingw-w64-clang-aarch64-pkgconf \
  mingw-w64-clang-aarch64-ffmpeg mingw-w64-clang-aarch64-nlohmann-json \
  mingw-w64-clang-aarch64-pugixml

cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build-win --parallel 4
ctest --test-dir build-win --output-on-failure
cmake --install build-win --prefix dist-win
```

CMake 缺少的固定源码依赖仍会从网络获取，离线配置方法见 README。
Windows 安装需要 CMake 3.21 以上；依赖扫描器会递归收集实际导入的 DLL，不复制
Windows 系统 DLL。其他工具链可通过 `SCRCTL_RUNTIME_DLL_DIRS` 补充 DLL 搜索目录，
这不意味着这些工具链已经通过验证。

保留安装目录中的 `bin` 与 `share`，可以整体移动目录。用 PowerShell 检查安装：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/test_windows_install.ps1 `
  -InstallDirectory dist-win -BuildDirectory build-win
```

该检查临时复制安装目录、隐藏开发构建的翻译目录，并将 PATH 限定为 Windows 系统目录。
它验证 exe 与 DLL 能独立启动，默认 auto 能按 `LC_ALL` 选择英文或中文，结束后恢复临时修改。
不会建立设备连接；上面的执行策略只适用于本次 PowerShell 进程。

## 设备连接

Wi-Fi 路径使用已有 RemotePairing 记录，设备和电脑之间需要能访问其监听端口：

```powershell
$env:LANG = 'zh_CN.UTF-8'
./dist-win/bin/scrctl.exe --wifi 192.168.1.50 --no-audio --stats
```

默认记录目录为 `%LOCALAPPDATA%\scrctl`；`XDG_DATA_HOME` 设置后使用其下的 `scrctl`
目录。新建记录文件从创建时就限制为当前用户访问，写完后替换目标文件；新建的叶子目录
也设置当前用户权限，不修改共享父目录或已有目录的权限。配对记录包含密钥，不要提交到仓库。
使用 `scrctl --pair -s <UDID>` 经 USB 建立或验证远程配对记录。
旧记录缺少设备身份或被设备拒绝时，使用 `--pair --repair-pairing` 显式重配；
手机可能要求确认。新记录通过独立连接验证后才保存。`--wifi auto -s <UDID>`
自动发现该设备，手动地址可用 `--wifi-port` 指定监听端口。

### USB：安装 Apple 设备组件

scrctl 使用 Apple 提供的 `127.0.0.1:27015` usbmux 通道。Windows 11 ARM64 上，本轮
通过 Microsoft Store 的 **Apple Devices（Apple 设备）** 安装后台组件，再补装 Apple
的 ARM64 USB 驱动，完成了真机 USB 镜像和截图恢复。Apple 也将该应用列为 Windows
设备管理入口，见 [官方说明](https://support.apple.com/en-us/118290)。

1. 使用 winget 安装 ARM64 版：

   ```powershell
   winget install --id 9NP83LWLPZ9K --exact --source msstore --architecture arm64
   ```

   本轮安装版本为 `AppleInc.AppleDevices 1.1540.24088.0`，包架构为 Arm64。
   首次打开时完成应用的许可与欢迎流程。商店网络失败时检查代理：当前用户代理和
   WinHTTP 代理可能不同；VM 中的 `localhost` 指向 VM 自身。临时修改后恢复原设置。

2. 将手机接入 Windows。Parallels 中选择“设备 → USB → Apple iPhone”。
   手机解锁，出现“信任此电脑”时确认。USB 直通会中断宿主机对同一设备的访问。

3. 检查 Apple USB 驱动。只看到“便携设备 → Apple iPhone”时，可能仍使用微软的
   通用 MTP 驱动。本轮 Apple Devices 安装后没有自动装好此驱动，手工补装成功。

   优先通过 Windows Update / 设备管理器更新。需要手工安装时，从
   [微软官方更新目录](https://www.catalog.update.microsoft.com/ScopedViewInline.aspx?updateid=33ca8d73-5ef6-45d2-94be-1198e7a74ddc)
   下载 **Apple USBDevice 552.0.0.0，ARM64** 的 CAB，保留原始 INF、CAT、SYS 和 DLL。
   在管理员 PowerShell 中解包并安装，例如：

   ```powershell
   New-Item -ItemType Directory -Path C:\Workspace\scrctl\setup\AppleUsbDriver -Force
   expand.exe "$env:USERPROFILE\Downloads\apple-usb-arm64.cab" -F:* C:\Workspace\scrctl\setup\AppleUsbDriver
   pnputil.exe /add-driver C:\Workspace\scrctl\setup\AppleUsbDriver\AppleUsb.inf /install
   ```

   `apple-usb-arm64.cab` 是这里使用的下载文件名，可按实际文件名修改。本轮所用包的
   INF 声明 `NTARM64` 并匹配 iPhone 的硬件 ID；CAT 通过 Microsoft Windows Hardware
   Compatibility Publisher 签名校验。安装后可见 `Apple Mobile Device USB Composite
   Device` 与 `Apple Mobile Device USB Device`。选择适合系统架构的官方包，保留签名校验。

4. 重启 Windows 并登录。本轮仅重新连接 USB 后，后台仍未完整启动；重启后
   `AppleMobileDeviceLauncher.exe` 和 `AppleMobileDeviceProcess.exe` 正常运行，后者
   监听 `27015`。此 Store 安装路径没有名为 Apple Mobile Device Service 的传统服务项。
   检查实际监听与设备列表：

   ```powershell
   Get-NetTCPConnection -State Listen -LocalPort 27015
   ./dist-win/bin/scrctl.exe --list-devices
   ./dist-win/bin/scrctl.exe --no-audio --stats
   ```

镜像还需要手机开启开发者模式，并挂载与系统版本匹配的个性化 DDI，见
[README 的设备准备](../README.md#首次连接设备)。本轮沿用已有设备信任及已挂载的 DDI，
没有验证从未配对、未挂载 DDI 的 Windows 环境开始完成全部准备步骤。

### USB 排查与当前限制

- `27015` 连接被拒绝：先检查后台进程及监听。设备管理器里的 iPhone 条目不能单独
  证明 usbmux 已可用。
- 能列设备但 `ReadPairRecord` 失败：检查手机解锁、信任状态及当前 Windows 用户。
  不要在问题报告中附上配对记录，里面含有私钥。
- `StartService` 返回锁屏或服务不可用：按错误检查手机解锁、开发者模式和 DDI。
- Parallels 已勾选手机，但 Apple Mobile Device USB Composite Device 报 Code 10：
  本轮重启该 PnP 设备未恢复；在 Parallels 中断开并重新连接手机后，复合设备和
  Apple Mobile Device USB Device 恢复正常，scrctl 随后可连接。可在“设备 → USB”
  中操作，或使用下列临时连接命令。设备 ID 从 `prlsrvctl usb list` 获取，不能照抄
  其他手机的 ID；切换会中断当前 USB 会话。这是本轮恢复方法，不保证所有 Code 10
  都由相同原因造成。

  ```bash
  # 在 macOS 宿主机终端运行
  prlctl set Windows --device-disconnect '<完整 USB 设备 ID>'
  prlctl set Windows --device-connect '<完整 USB 设备 ID>'
  ```

- 本轮 Windows 配对记录的 HostID 为 27 字符的不透明标识。修复后的 scrctl 会从它
  生成稳定的 UUIDv5；原本为 UUID 的 HostID 保持原值，lockdown 使用的原始记录不变。
- 两组 NCM 网络接口仍有黄色叹号；本轮 USB 镜像通过 CoreDeviceProxy 与 lwIP 隧道，
  在这些接口未正常工作的情况下也通过了测试。该结果不覆盖手机热点或直接 NCM 联网。
- Apple Devices 的界面在本轮仍未显示手机，其设备发现问题尚未解决。后台 usbmux、
  lockdown 和开发者隧道已由 scrctl 真机测试确认可用，不能将此描述为该应用所有功能正常。

## 本轮结果

环境为 Windows 11 IoT Enterprise LTSC ARM64（26100）、Parallels、Clang 22.1.8，
设备为 iPhone14,4 / iOS 27.0。测试使用已有配对记录，未重新执行 SRP pair-setup。

- 原生 ARM64 完整构建及离线测试 24/24 通过；同一批改动的 macOS 回归 24/24 通过。
- Windows 本地 socket / PSK TLS 测试连续 10 次通过，包括中断阻塞接收。
  修复前 shutdown 单独调用有接收未退出的样本，补充 CancelIoEx 后通过。
- Wi-Fi 无窗口运行 20 秒，输出 315 帧，完成强制截图降级后恢复视频，正常退出。
- 移动后的安装产物在系统 PATH 下运行 SDL 窗口 25 秒，输出 229 帧；
  强制降级期间取得 18 张截图，随后恢复视频，截图失败为 0，正常退出。
- 安装启动检查覆盖 DLL、随包中文目录和默认 auto 选择。
- 补装 Apple ARM64 USB 驱动并重启后，scrctl 通过 `127.0.0.1:27015` 列出 USB 设备。
  无窗口运行 20 秒输出 881 帧，降级阶段取得 13 张截图，恢复视频并正常退出。
- USB SDL / Direct3D 窗口运行 30 秒输出 498 帧，降级期间 26 张截图；另一次 60 秒
  正常窗口运行输出 426 帧、34 张截图，实际回读确认显示手机内容。两轮均恢复视频、
  截图失败为 0；帧数随画面活动变化，不用这些不同场景比较吞吐。
- HostID 修复后，macOS 与 Windows ARM64 完整离线回归均为 24/24，包括独立 Python
  UUIDv5 向量、已有 UUID 保持不变及同一不透明 HostID 重读后身份稳定的判据。

这些是有限时长回归，不代表真实断网、物理拔插、多设备或长期运行已验证。
后续 USB 触摸验证见下一节。Windows 当前没有音频后端；新建配对及 DDI 的完整初始
安装流程仍需分别验证。

## 2026-10-08：USB 竖屏触摸回归

使用 `4976b3b` 对应的 HID 及主程序源码，在独立目录
`C:\Workspace\scrctl\hid-validation-83d5822-20261008` 构建；目录名保留最初快照的
基线提交，实际 HID / CLI / 翻译文件已与 `4976b3b` 逐项核对。工具链位于
`C:\opt\msys64`，主程序和诊断工具从搬移后的 `dist\bin` 启动，PATH 只包含该目录
和 Windows 系统目录。仍沿用已有信任、配对记录和 DDI。

- 重新连接 USB、恢复 Code 10 后，`hid_probe` 依次截图、枚举五个输入面、重开 HID
  服务并发送三段三角形笔画，6.02 秒返回 0。手机前后截图确认三角形出现在请求位置。
- 主程序开启视频窗口，关闭音频，窗口尺寸为 560×1213。用户在电脑窗口画两个 L，
  并确认手机落点一致；回读手机截图确认两个 L，日志记录了 SDL 鼠标按下、移动及
  对应的设备归一化坐标。这覆盖鼠标 → SDL → HID → 手机的竖屏路径。
- Computer Use 对 Parallels 的自动鼠标尝试没有产生 SDL 输入记录；窗口测试由用户
  实际操作完成，不能把自动化尝试记为通过，也不能据此判定产品输入失败。
- NCM 接口仍有黄色叹号，未阻止本轮 usbmux、CoreDeviceProxy、截图和触摸。
  未测试热点或直接 NCM 联网，也未重新验证 Apple Devices 界面的设备发现。

本轮未覆盖横屏、键盘、硬件按键、Wi-Fi 输入、首次配对、DDI 初始安装或音频。
原始截图和日志保存在本机 `/private/tmp/scrctl-hid-device-20261008/`。

## 2026-10-08：横屏 USB 与 Wi-Fi 鼠标触摸

使用上一节同一批安装二进制。窗口为 1040×480，手机无边记为横屏，
设备可见区为 2436×1125，视频源像素由窗口顺时针旋转 90 度显示。

- USB 视频窗口接收鼠标方框笔画；用户确认手机同位置出现方框，手机截图及 SDL
  坐标日志相互对应。关闭窗口后正常结束，共处理 4368 帧。
- Windows VM 使用桥接网络。确认设备 RemotePairing 端口可达后，以 `--wifi` 和
  明确的设备标识连接；沿用此前在 Mac 上经用户确认、保存设备身份的 USB 配对记录，
  通过该测试进程的 `XDG_DATA_HOME` 读取，没有执行 Windows 首次配对。
- Wi-Fi 输入测试前已将手机 USB 从 VM 断开。用户在电脑窗口画三角形并确认手机
  同位置出现；SDL 日志记录三次按下和 21 次移动，没有输入注入错误。
- 使用另一个仅截图模式的 Wi-Fi 会话回读画面，源图为 2436×1125，窗口回读为
  1040×480，三角形可见。该会话处理三帧后正常退出；此项只验证截图显示，
  没有在截图模式下再次绘图。
- Wi-Fi 视频窗口关闭后正常结束，共处理 3447 帧；两轮视频统计中的序号缺口、
  分片丢弃及自动重启计数均为零。静止画面可能停止输出，不能据此判定断流。

第一次启动 Wi-Fi 时，Windows 默认记录目录没有对应配对文件，程序在创建窗口前
报错退出；TCP 端口可达不能替代远程配对条件。旧版裸 Q 退出在本轮用户操作中
未生效，关闭按钮则正常；这项现象没有归因于键盘布局或 VM。随后退出规则按
scrcpy 的 MOD+Q 改动，改动后的验证另行记录。

本轮不覆盖 Windows 首次配对、DDI 初始准备、长期弱网或音频。手机已交回 Mac，
其 USB 设备列表确认可见。证据位于 `/private/tmp/scrctl-hid-device-20261008/`，
汇总为 `win-landscape-summary.json`。

## CI 产物

GitHub Actions 的 `Windows ARM64` 作业使用 `windows-11-arm` 和 MSYS2 CLANGARM64，
执行构建、离线测试、安装及独立启动检查。产物名为 `scrctl-windows-arm64`，
测试日志另外保存。[提交 8efbfdd 的 Windows 作业](https://github.com/YJBeetle/scrctl/actions/runs/37596738733/job/112711112940)
已完成构建、离线测试、安装检查与上传；下载的实际 artifact ID 为 `11472035027`。
包内 `source-commit.txt` 与 `scrctl-source.tar` 的归档提交均为
`8efbfdddcbb39f653586377f022d313bd1bf63f4`。

2026-10-08，将该包复制到 Windows ARM64 VM 的
`C:\Workspace\scrctl\ci-artifacts\8efbfdd`，完成了五次独立进程启动。每个子进程的
PATH 仅含 `C:\Windows\System32;C:\Windows`，工作目录为 `C:\Windows`，并清除
`LC_ALL`、`LC_MESSAGES`、`LANGUAGE` 和 `SCRCTL_LOCALEDIR`：

| 参数 | LANG | 预期输出 | 退出码 |
| --- | --- | --- | --- |
| `--version` | `C` | `scrctl 0.1.0` | 0 |
| `--lang=en --help` | `zh_CN.UTF-8` | 英文帮助 | 0 |
| `--lang=zh-CN --help` | `en_US.UTF-8` | 中文帮助 | 0 |
| `--help`（默认 auto） | `en_US.UTF-8` | 英文帮助 | 0 |
| `--help`（默认 auto） | `zh_CN.UTF-8` | 中文帮助 | 0 |

下载包、VM 复制后及启动结束后的 exe SHA256 均为
`c80acdd1385ff19cfd5109de38003e0886d07f75fb753d3c71d859927526b5f8`。
83 个 DLL、五份来源记录（包含源码归档）及中文 `scrctl.mo` 的复制前后哈希全部一致。
随包记录包含 272 条已安装包版本和 160 份依赖许可文件；没有逐 DLL 到包的来源映射，
不以 VM 中另一套工具链的包信息推断 CI 来源。

下载 artifact 后整体解压，保留 `bin` 和 `share` 的相对位置，运行 `bin\scrctl.exe`。
`bin` 内应包含 exe 及其 DLL；`share` 内含翻译和构建记录。只复制 exe 会导致
`libssl-3-arm64.dll` 等依赖找不到。构建目录中的 exe 依赖开发环境，不能单独分发。
这次检查覆盖实际安装包的启动、DLL 加载、版本和语言选择，没有连接设备；本提交的
USB、Wi-Fi、音频及真实媒体采集或显示未由这五次启动验证。前文 Apple 组件、USB 镜像
及截图恢复的数据来自此前真机实验，保留其原有环境和有限时长边界。

CI 包包含 DLL、翻译目录、已安装包版本、FFmpeg 构建配置、依赖许可文件和本项目源码快照。
MSYS2 当前 FFmpeg 包启用了 GPL 和 version3，属于 GPL-3.0-or-later 构建，不能将其标为
纯 LGPL 包。本项目的 Apache-2.0 声明不会覆盖随包依赖的许可；对外发布前还需准备与实际
二进制对应的第三方源码及构建材料，审核整个依赖组合的分发条件。当前 CI 产物用于开发验证。

## FFmpeg 音频后端验证（2026-10-08）

Windows / Linux 现在使用 FFmpeg 原生 AAC-ELD 解码和 libswresample，
macOS 继续默认使用 AudioToolbox。当前支持 48 kHz、双声道、每帧每声道
480 或 512 个采样；480 配置使用 ASC `F8 E6 50 00`。

- Windows ARM64 对同一份真实音乐 RTP 解码 290/290 帧，无失败，输出
  278400 个交织采样（2.9 秒），峰值 32497，四个时间段均非静音。
- 同一份载荷在 macOS 上用 FFmpeg 和 AudioToolbox 解码，采样逐一对齐，
  最大差异为 1 个 s16 量化单位，见 [后端对照](coredevice.md#171-音频解码后端只能选-audiotoolbox实测同一份-dump-四路对照)。
- 用户已确认 Windows USB 实时播放时电脑发出音乐；日志显示 WASAPI 输出，
  音频解码失败为 0。这一趟约 7 秒后以 `Tunnel TLS read failed` 断开，
  用户确认未再次拔插。持续连接尚未通过，不能用短时发声替代稳定性验收。
- 新音频测试及配对/发现的五组 Windows 离线回归通过；正式配对命令、
  无线自动选择及新安装包的真机验证另行记录。

FFmpeg 5.0/5.1 只完成官方头文件的 API 编译检查，未运行旧版解码库；
512 配置目前只有静音样本。其他采样率、声道布局、ELD SBR 配置仍需独立验证。
