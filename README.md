# scrctl

基于 CoreDevice 开发者服务的 iOS 镜像与控制工具，提供原生窗口和命令行接口。
支持 USB、Wi-Fi 连接与手机发起的无线 PIN 配对；镜像时设备仍可直接操作。命令行设计参考 scrcpy。

## 使用 xdb Server

设备连接、配对、DDI、视频/音频采集与解码由 [XDeviceBridge](https://github.com/YJBeetle/XDeviceBridge) Server 提供。scrctl 链接轻量 `xdb_client`，负责 SDL 显示、键盘快捷键、声音输出以及本地 HEVC 文件回放。

```bash
git submodule update --init --recursive
cmake -S . -B build
cmake --build build --parallel

# 默认连接本机端口 17423，缺少 Server 时自动在后台启动
xdb pair --wifi
xdb devices
xdb -s '<UDID>' ddi prepare --wifi auto --cache-dir '/absolute/ddi-cache'
./build/scrctl -s '<UDID>' --wifi auto
```

也可以用 `xdb pair --usb` 建立配对。GUI 可直接调用 xdb 客户端 SDK，无需读取配对文件。手机凭据由 Server 保存；scrctl 不读取原来的配对目录。需要迁移旧凭据时，显式执行 `xdb pairing import <record-file>`。

scrctl 原有 `--pair`、`--install-ddi`、`--download-ddi`、`--list-devices`、剪贴板及应用命令转发给同一个 Server。录制文件由 Server 在本机绝对路径写入，保留原始编码轨道；窗口暂停只冻结显示。Server 仅监听本机；桌面客户端会自动启动并复用后台 Server。可用 `xdb start-server` 显式启动、`xdb kill-server` 停止。

请将独立安装的 `xdb` 放在 `PATH` 上；分开构建时可用 `XDB_EXECUTABLE` 指定其可执行文件路径。SDK 也会查找客户端库旁或相邻 `bin` 目录中的 `xdb`。

前端构建需要 SDL2、CLI11、nlohmann-json、gettext 和 C++20。FFmpeg 只用于本地 HEVC 文件回放；实时设备媒体在 Server 解码。运行两者分别遵循其依赖与许可证要求。下方历史协议研究及真机数据描述直接后端，不能当作本轮 xdb 链路的验收结果。

## 下载与安装

正式版：[v1.0.0](https://github.com/YJBeetle/scrctl/releases/tag/v1.0.0)。
提供 macOS ARM64、Windows ARM64 和 Windows x64 安装包；Linux 请按下方说明从源码构建。
旧版发布包不代表当前 XDB 架构的交付内容。当前源码构建的 scrctl 需另外安装
[XDeviceBridge 运行包](https://github.com/YJBeetle/XDeviceBridge)，将 `xdb` 放入 `PATH`
或设置 `XDB_EXECUTABLE`；scrctl 安装目录仅附带客户端库，不捆绑 Server。
解压后运行 `bin/scrctl --help`；Windows 的 USB 驱动要求见 [安装说明](docs/WINDOWS.md)。
macOS 包使用 ad hoc 签名，未经过 Apple 公证；系统可能要求确认打开来源。
DDI 按需下载，不包含在安装包中。

发布页的 `SHA256SUMS.txt` 用于核对下载文件。每个运行包都有同平台对应源码附件，
包含依赖源码、构建配方与补丁，见 [源码材料说明](docs/SOURCE_MATERIALS.md)。
版本变化见 [更新记录](CHANGELOG.md)，具体系统与机型的验证范围见下文。

## 功能与验证范围

历史真机记录覆盖了屏幕镜像、触摸、硬件按键、ASCII 键盘输入、剪贴板、音频、
自动转屏和截图轮询。默认视频解码使用 FFmpeg，可选 VideoToolbox；录制支持
MP4 / MKV（HEVC 和 AAC-ELD）及裸 Annex-B HEVC。

主要真机验证环境为 macOS、iPhone14,4 / iOS 27.0。已有活动画面记录约为 60 fps，
静止画面允许暂停输出。本轮重构已验证 USB / Wi-Fi 传输及强制视频与截图切换，
并对丢包和关键帧解码失败做了离线或本地故障注入；这些结果不代表所有机型、
真实弱网或长时间运行已经验证。

### iOS / iPadOS 与机型

下面是具体设备的实测结果。DDI 的 manifest 声明某个机型，只是型号检查的一部分：
Personalized 还需匹配同一安装身份与实际 payload，Cryptex 则使用通用资产身份，不用 `SupportedProductTypes` 列表提前拒绝机型。
实际安装还需要设备接受个性化签名，产品功能仍取决于系统提供的服务。
`--install-ddi` 会先检查可复用的挂载：Classic 必须匹配固定目录的签名，
CoreDevice 系统还需提供截图服务能力；符合条件时不下载镜像。
不匹配的 Classic 镜像须使用 `--replace-ddi` 显式替换。
不能把 manifest 的机型列表当作已通过 scrctl 真机测试的支持列表。

| 实测设备 | 系统版本 | 开发镜像与功能范围 |
| --- | --- | --- |
| iPhone 13 mini（`iPhone14,4`） | iOS 27.0 | USB / Wi-Fi 实时视频、音频路由、截图、触摸、键盘、剪贴板、设备按键及方向控制已有实测记录；macOS、Debian ARM64 和 Windows ARM64 的具体范围见下表 |
| iPhone 13 mini（`iPhone14,4`） | iOS 27.0.1 | Cryptex DDI `27A5228h` 安装后恢复屏幕服务，30 秒 USB 视频录制通过；旧 Xcode 26.2 的 `17C52` 镜像缺少屏幕、截图、HID 等服务，不能仅凭“兼容”判断可用；尚未在更新后的系统重测所有功能 |
| iPad mini 5（`iPad11,2`） | iPadOS 18.7.8 | Personalized DDI 挂载后，截图轮询（约 2–3 fps）、窗口触摸、硬件按键、ASCII 键盘、剪贴板往返、应用列表与启动已验证；实时视频和音频被设备以错误 `9021` 拒绝，明确要求 iOS 27.0 或更高版本；Wi-Fi 配对与连接未在这台设备验收 |
| 其他机型 / 系统组合 | — | 尚无对应真机验收；应分别检查镜像身份、设备服务目录与功能请求结果 |

iPad mini 的记录见 [协议记录 §23、§24](docs/coredevice.md#23-那台-ipad-mini-到手了21-留的-ab-两问都有答案了实测ipad112--ipados-1878--usb)，
系统更新后的镜像替换见 [§34](docs/coredevice.md#34-ios-2701-更新后的开发镜像准备)。
实时媒体的版本拒绝可触发截图轮询；截图模式不采集音频，也没有可录制的 HEVC
码流。不同设备上相同系统大版本的能力仍须以实际服务与请求结果确认。

### 电脑平台

| 平台 | 当前范围 |
| --- | --- |
| macOS | 有真机连接、镜像、输入和音频记录；本轮验证结果见重构记录 |
| Linux | Debian ARM64 的构建、离线回归及 Wi-Fi 软件解码、Wayland 窗口、竖横屏鼠标落点、缩放、键盘、Unicode 粘贴、设备按钮和音频路由短测已通过；直接重插接入 VM 后 USB 30 秒录制通过，归属切换仍曾出现 STALL；Cryptex DDI 的 Wi-Fi 原生替换安装及新连接视频录制通过，旧系统 DDI 新装与物理断线恢复仍待验证 |
| Windows | x64 / CLANG64 的构建、73 项离线测试、独立安装与对应源码交付已由 CI 验证，x64 真机未测；ARM64 构建与离线测试、USB / Wi-Fi 镜像、截图切换及竖横屏鼠标触摸已验证；FFmpeg 真实音乐解码、短时 USB 播放及新版 Wi-Fi 默认、双端、无音频三种行为已确认；无线首次 PIN 配对和独立重连、Cryptex DDI 的 Wi-Fi 原生替换安装及新连接视频录制已通过，实际长暂停恢复、USB 持续连接与 USB 初始配对准备待验证，见 [安装说明](docs/WINDOWS.md) |

以下音频后端数据来自迁移前的测试；当前音频解码由 XDB Server 负责，客户端只输出 PCM。
历史实现在 macOS 使用 AudioToolbox，Windows / Linux 使用 FFmpeg 的原生 AAC-ELD
解码器与 libswresample。当前 FFmpeg 适配支持 48 kHz、双声道、每帧每声道 480 或
512 个采样；真实音乐的 480 配置已与 AudioToolbox 逐样本对照，最大差异为 1 个
16 位量化单位。旧实验使用了错误的编码配置，其“不支持 ELD”的结论已修正，见
[音频记录](docs/coredevice.md#171-音频解码后端只能选-audiotoolbox实测同一份-dump-四路对照)。
本机播放音量由电脑控制，也可使用 `--no-audio-playback` 关闭；手机媒体音量可能
同时影响捕获声音，不能靠将手机静音来实现只在电脑播放。

音频默认转到电脑，手机停止发声；`--audio-dup` 保留手机播放，同时把音频转发到
电脑。切换路由时播放器可能暂停；本轮 QQ 音乐测试观察到路由激活时暂停一次，
需要在手机上手动继续。退出后若播放器仍暂停，也需要手动继续。程序不调整手机
音量、不自动按播放键；默认路由不可用时会报错并继续视频，不自动改为双端播放。

`--no-audio` 与 scrcpy 的含义一致：跳过音频采集和路由请求，保持手机原有的
播放与输出状态，也不恢复已经暂停的播放器。`--no-audio-playback` 关闭电脑
播放；没有 MP4/MKV 音轨录制时，也不采集音频，效果与 `--no-audio` 相同。
裸 HEVC 录制不包含音轨。

录制到 MP4/MKV 时，`--no-audio-playback` 仍采集音频并遵循所选路由：默认
将音频转到电脑，手机无声；加上 `--audio-dup` 可保留手机播放并录下音轨。
音频采集已关闭时使用 `--audio-dup` 会在连接设备前报错。

播放后端初始化失败时不启动音频；请求含音频的容器录制时会在连接设备前报错，
可显式改用 `--no-audio-playback` 只采集，或 `--no-audio` 只录视频。起流后声卡
打开失败时，容器录制保留音频接收；未录制时则停止音频接收和续期。设备答复
无效（含路由不符）时保留视频，但结束本次音视频录制。已建立的音频路由可能需等待会话到期
（当前音频租期 20 秒），之后播放器仍可能需要手动继续；恢复时协商答复无效也会
停止音频重试。具体回收时刻尚未在所有设备上验证。

默认路由已在 macOS、iPhone14,4 / iOS 27 的 USB 与 Wi-Fi 音乐播放中验证：电脑有声、
手机无声，结束后手动继续可恢复手机播放。Debian ARM64 的 Wi-Fi / PulseAudio
短测也确认默认路由、`--no-audio` 和退出后手机声音恢复；`--audio-dup` 已确认电脑
播放正常。Windows ARM64 / WASAPI 的新版 Wi-Fi、200 ms 缓冲已确认默认路由四分钟
电脑有声、手机无声，双端模式两边有声，无音频模式手机继续播放、电脑无音乐；
两轮电脑播放听感正常、无播放欠载。该本地 Release 的音频源码与 `8a905f9` 一致，
不将其当作该提交的 CI 二进制。Windows 真实长暂停恢复及旧系统仍待验收。
音频转发已开始不代表播放器在切换期间从未暂停。

启用 FFmpeg 的构建使用 libswresample 平滑补偿音频时钟差异，macOS 的 AudioToolbox
解码也共用这层补偿。`--audio-buffer` 默认 50 ms，目标保持固定；网络或调度抖动
较大时可增加到 `--audio-buffer=200`，以较高延迟换取更连续的播放。

[协议调研记录](docs/coredevice.md) 保留原始观察与实验过程；
[重构记录](docs/REFACTOR.md) 说明当前实现及验证边界；
[路线图](docs/ROADMAP.md) 列出剩余工作。
提交 `80a549f` 的 [GitHub Actions](https://github.com/YJBeetle/scrctl/actions/runs/38043461515)
中，macOS、Ubuntu、Windows ARM64 和 ASan / UBSan 各实际执行 65/65 项离线测试并通过。
该提交的 macOS 与 Windows 安装包均通过搬移启动检查，分别包含 25 dylib 与
98 DLL；Windows 下载包和虚拟机中使用的 EXE、源码及全部 DLL 已核对一致。
包审计不代替真机测试或完整第三方源码分发。

## 实现结构

| 模块 | 职责 |
| --- | --- |
| `xdb` 子模块 | 轻量客户端 C ABI；服务端设备协议在 XDeviceBridge 仓库维护 |
| `src/app` | CLI、SDL 显示、输入映射、IPC 媒体接收和声音输出 |
| `src/bitstream`、`src/decode` | 本地 HEVC 文件解析与回放 |
| `src/i18n`、`po` | gettext 中英文消息和语言选择 |

MaaFramework 与 scrctl 共同使用 XDeviceBridge 的客户端协议。

## 窗口操作与 scrcpy 用法

常用参数和窗口快捷键以 scrcpy 为参照，当前支持：

| 操作 | 快捷键 |
| --- | --- |
| 退出 | MOD+Q，或关闭窗口 |
| 切换全屏 | MOD+F，或不带修饰键的 F11 |
| 设备触摸 | 鼠标左键点击、拖动 |
| 设备键盘 | 普通按键按下 / 松开；字母、数字、标点及常用编辑键 |
| 粘贴电脑剪贴板 | MOD+V，支持中文、emoji 和换行 |
| 回到主屏 | MOD+H，或鼠标中键 |
| 调整音量 | MOD+↑ / ↓ |
| 侧键 / 锁屏 | MOD+P |
| 恢复原尺寸 | MOD+G |
| 去掉画面留边 | MOD+W，或双击画面外的留边 |
| 旋转电脑画面 | MOD+← / →，向左 / 右旋转 90°，不改变录制 |
| 水平镜像电脑画面 | MOD+Shift+← / → |
| 垂直镜像电脑画面 | MOD+Shift+↑ / ↓ |

MOD 默认是左 Alt 或左 Super（Windows 键 / Mac Command），可以通过
`--shortcut-mod=rctrl` 等配置。普通 Q 和 Esc 不退出。窗口转发物理按键，使用手机
的键盘布局与输入法；不同时注入主机输入法的提交文字。窗口失焦、隐藏、最小化、
转屏或退出时松开按键和触点。发送失败后停止新输入，并用已有连接尽力松开。
默认左 Command 属于本地 MOD；需要设备 Command 组合时可用 `--shortcut-mod=lalt`。

设备按钮跟随实际按下和松开；音量快捷键会转发主机重复事件，HOME 和侧键不重复
发送。普通 H、P 和方向键仍发给手机。按钮快捷键只在启用设备控制时生效，
无视频背景窗口也能使用。Debian Wi-Fi 窗口已验证 HOME、音量长按重复和锁屏；
其他平台的物理按钮路径及无视频背景窗口仍需分别验证。

显示旋转和镜像按当前电脑画面的方向组合，后续帧和设备转屏仍保留这个变换；
静止画面也立即更新。镜像不改变窗口尺寸、手机方向或录制。普通方向键仍发给
手机。MOD+R 请求手机在横屏和竖屏间切换，需启用设备控制且应用支持转屏；
请求异步执行，不主动修改方向锁，也不自动重复失败请求。
本台 iOS 27 的无边记在竖排方向锁开启时不会响应远程转屏；需先关闭方向锁。

设备转屏时复用原窗口，保留移动后的位置和调整后的显示尺度，按新画面比例
适配普通窗口尺寸。启动参数不会重新应用。全屏、最大化和最小化保持当前模式，
恢复普通窗口后再适配最终画面；转屏前已排队的输入不会用于新的画面。

MOD+V 只在明确按下时读取电脑剪贴板。后台写入手机后另开连接读回核对，随后
由窗口发送粘贴组合键；有键或鼠标按住时不粘贴。窗口失活、转屏、输入失败或
退出会取消旧请求，避免迟到按键。最多一个在途请求，文本上限 1 MiB，窗口循环
在约五秒后请求取消；取消不能撤销手机已经接受的剪贴板写入。目标控件是否允许
粘贴由手机决定。系统可能提示从 `dtpasteboardd` 粘贴，这是苹果的远程剪贴板
服务；允许后才能完成粘贴。Mac USB 已验证中文和符号实际进入 Safari 输入框，
Debian Wi-Fi 窗口已验证中文、符号和 emoji；Windows Wi-Fi 窗口的 Unicode 粘贴
已在临时隔离 Parallels 共享剪贴板后通过，测试结束已恢复共享模式。
Windows 的普通 Q、字母、数字和退格已实际进入手机输入框且不退出窗口；更多布局、
组合键、多行粘贴及共享剪贴板开启时的互操作仍需分别验证。
`--test-type` 仅是 ASCII 诊断入口。
与 scrcpy 的具体差异和后续对齐项见
[用法对照](docs/SCRCPY_USAGE.md)。

## 录制

```bash
scrctl --record capture.mkv                # HEVC 视频和 AAC-ELD 音频
scrctl --record capture.mp4                # 同样保存音视频，使用 MP4 容器
scrctl --record capture --record-format=mkv # 显式指定容器，不依赖扩展名
scrctl --record capture.mkv --no-audio     # 只录视频，不改变手机音频路由
scrctl --record capture.mkv --audio-dup --no-audio-playback  # 保留手机声音，电脑只录制
scrctl --no-video --record audio.mkv --audio-dup --no-audio-playback --no-window  # 只录音频
scrctl --record capture.hevc              # 原有裸 HEVC 方式，不录音频
```

`--no-audio-playback` 录制 MP4/MKV 时直接保存原始 AAC 音轨，不执行 PCM 解码、
播放时钟补偿或播放缓冲。音频来源检查、序号、时间戳与会话续期仍然保留；
它只关闭电脑播放，不改变所选的手机音频路由。

`--no-video` 不采集视频，可单独播放音频。`--no-video-playback` 不显示视频，
录制时仍接收原编码包；没有录制或帧诊断消费者时也会关闭视频采集。
`-N / --no-playback` 关闭电脑的音视频播放，保留录制或键盘控制。
这些模式默认保留 256×256 点的背景窗口，可用普通按键、粘贴、设备按钮、退出和全屏；
鼠标坐标触摸和视频尺寸快捷键停用。显式窗口宽高分别覆盖默认值。
`--no-window` 关闭窗口，也关闭视频播放；不影响有消费者的音频或录制。
没有音视频、录制、窗口控制或显式启动操作时会报错，不建立空会话。
`--no-video -r audio.mp4` 或 `audio.mkv` 只保存 AAC-ELD 音轨，不采集视频。
仅录音频不能同时指定 `--no-audio` 或非零录制方向，也不支持裸 HEVC 或 `.aac`。
`--no-window -r` 仍按默认选择保存音视频；需要仅录音频时请显式使用 `--no-video`。

`--no-window` 配合录制时，有 IDR 语法检查能力的构建会直接保存编码视频，
不创建显示解码器或 BGRA 帧。完整 IDR 通过参数和切片语法检查后才确认就绪；
收包、时间戳、丢包恢复与会话续期照常运行。`--exit-after`、`--hw-decode`
或 `--test-degrade` 仍使用解码路径；缺少该检查能力时也保留原解码路径。
这项优化仍依赖 FFmpeg 的公开参数与语法检查接口，不等于取消 FFmpeg 依赖。

默认按扩展名选择格式：`.mp4` / `.mkv`（不区分大小写）为容器录制，其他扩展名
保留裸 HEVC 行为，建议使用 `.hevc`。`--record-format=mp4|mkv|hevc` 显式覆盖扩展名，
必须同时指定非空 `--record` 文件；音轨和录制方向限制依据所选格式。`mp4` / `mkv`
与 scrcpy 的格式选择用法一致，`hevc` 是 scrctl 的裸视频扩展，不含音轨。
`--play` 只支持裸 HEVC，不能与 `--record` 同时使用；
播放容器文件请使用支持 HEVC / AAC-ELD 的播放器。FFmpeg 解码已验证，其他播放器
兼容性需要单独确认。

文件回放默认使用软件解码，`--hw-decode` 选择平台后端；未编入软件后端时会提示
兼容回退。输入须为普通文件，空文件、读取失败、解码器初始化失败或始终未输出
视频帧均返回非零退出码。回放逐块读取并按顺序解码，不预先加载整个文件或积压
大量 BGRA 帧；异常大的 NAL 仍可能占用较多内存。

容器使用设备的 RTP / RTCP 时钟保留可变视频间隔和音视频偏移，显示裁剪和
`--display-orientation` 不改变录制。首版限于无重排、单层 HEVC 和 48 kHz 双声道、480-sample AAC-ELD。
每个所选轨道都需要有效媒体及至少两条可信时钟报告；等待最多 5 秒，录制器共用
16 MiB 编码缓存预算。此预算不包括 FFmpeg 内部缓存，也不限制进程总内存。

MP4 中，前一帧的显示时长取两个已确认视频时间点的间隔；最后一帧显示 100 ms，
与 [scrcpy 的收尾规则](https://github.com/Genymobile/scrcpy/blob/v5.0/app/src/recorder.c#L393-L405)
一致。这是文件展示规则，不是设备报告的最终显示时长。设备长时间不发新画面时，
视频轨可能早于音轨结束；只有一帧的纯视频录制长 100 ms。MKV 保留未知末帧时长。

同一会话中的 HEVC 参数和尺寸变化仅在完整 IDR 携带有效无重排配置时继续写入，
MP4 使用 `hev1` 保留带内参数，容器头仍保留首配置；播放器支持需单独确认。
缺片、时钟异常、不受支持的配置变化、会话重建、截图降级、磁盘错误会结束本次容器录制。
解码模式的镜像可继续并自行恢复；仅录制模式结束任务，两种情况退出码均为非零。
程序不把恢复后的另一段自动拼入同一文件；
失败时文件可能不存在或不完整。退出会先等待两个收包线程，再写容器尾部。
持续音频不需要等待下一张视频画面。

录制直接封装设备提供的原始编码，不旋正或缩放像素、不补黑边，也不重编码。
视频容器默认将起录时的设备方向写为固定旋转元数据，让开始录制时的画面正向显示；
之后转屏不更新该元数据。方向查询失败时提示并使用 0 度。
与 scrcpy 默认转屏录制的差异来自设备端：scrcpy 可在 Android 端随转屏调整采集和
编码尺寸；scrctl 当前尚未找到经过验证、能让苹果端随转屏交换编码宽高的接口。
实测 iPhone 13 mini / iOS 27.0.1 保持 1136×2464 编码画布，横屏内容侧向保存在其中。
这限定当前采集方式和已测设备，不表示苹果协议绝对不支持其它输出方式。
`--record-orientation` 仅支持固定数字角度，不支持 `auto`。详见
[参数差异与验证范围](docs/SCRCPY_USAGE.md)。

### 显示与录制方向

```bash
scrctl -r capture.mp4 --orientation=90        # 显示和录制都顺时针转 90°
scrctl -r capture.mp4 --display-orientation=90 # 只转电脑画面
scrctl -r capture.mp4 --record-orientation=90  # 只转录制文件的显示方向
scrctl --display-orientation=flip90          # 水平翻转电脑画面，再顺时针转 90°
```

旋转支持 `0` / `90` / `180` / `270`；显示还支持 `flip0` / `flip90` / `flip180` /
`flip270`，先水平翻转裁剪后的源画面，再顺时针旋转。鼠标映射会撤销这两个变换，
点击仍对应画面中的设备位置。组合参数按出现顺序覆盖各自涉及的方向。
显示默认 `auto` 跟随设备；视频容器录制默认采用起录时设备角度并保持固定。
显式指定 `--record-orientation=0/90/180/270` 或数字 `--orientation` 时，以指定值为准。兼容用法 `--orientation=auto` 只设置
显示并取消显示翻转，不改变已设置的录制方向；`--record-orientation` 不支持 `auto`。

录制不支持翻转。可用 `--display-orientation=flip90` 单独翻转显示；若用
`--orientation=flip90` 并录制，需在后面用 `--record-orientation=0` 等旋转值覆盖
录制方向。最终录制方向仍为 flip 时会在连接设备前报错。

数字角度的录制旋转通过容器元数据实现，相对于设备编码像素固定，不重编码，也不改变音频、
时间戳或触摸映射。播放器需要支持方向元数据；MKV 非零旋转需要 FFmpeg 6.1 或更新版本。
裸 HEVC 不含容器方向元数据，非零录制方向会报错；可以改用容器，或用
`--display-orientation` 单独旋转窗口。

## 命令行语言

支持英文和简体中文，注释保持中文。默认 `--lang auto`，按首个非空的 `LC_ALL`、
`LC_MESSAGES`、`LANG` 选择语言；`zh` 系列 locale 使用简体中文，其他语言、
`C`、`POSIX` 或未设置时回退英文。`--lang en` / `--lang zh-CN` 显式覆盖环境。
`LANGUAGE` 不参与 scrctl 的自动选择，避免与上述规则冲突。

```bash
LANG=zh_CN.UTF-8 scrctl --help
scrctl --lang en --help
scrctl --lang zh-CN --stats
```

中文目录缺失时显示英文。安装会同时安装 `share/locale/zh_CN/LC_MESSAGES/scrctl.mo`；
保留 `bin` 与 `share` 的相对目录后可以移动安装目录。开发构建直接读取构建目录的翻译，
也可用 `SCRCTL_LOCALEDIR` 指定目录根。
项目自己的帮助、状态和错误提示已接入翻译；设备响应、系统错误与 CLI11 参数校验正文保留其原文。
Linux/glibc 需要至少安装一套中文或英文 UTF-8 locale 才能加载 gettext 中文翻译；
只有 C / C.UTF-8 时回退英文。macOS 可搬移安装包及检查方法见 [macOS 说明](docs/MACOS.md)。
十个常用诊断工具也已接入语言选择与消息目录：`screenshot_probe`、`rr_keepalive_probe`、
`wifi_probe`、`stall_probe`、`audio_pump_probe`、`audio_decode_probe`、`feature_probe`、
`applist_probe`、`app_launch_probe` 和 `hid_probe`。归档实验的独立输出仍在整理。

修改用户提示时，英文消息使用 `SCRCTL_TR`（延后翻译的常量使用 `SCRCTL_N_`）。
运行 `python3 tools/update_translations.py` 更新模板与中文目录，补齐 `po/zh_CN.po`
后再运行 `python3 tools/update_translations.py --check`。该检查使用 gettext 的
`msgcmp` / `msgfmt`，会发现源消息未同步、缺失译文或 printf 占位符错误；离线测试也会执行。

## 构建

需要 C++20、CMake、Python 3、SDL2 2.0.18+ 和 gettext；FFmpeg 可选，用于本地 HEVC 文件回放。

```bash
git submodule update --init --recursive
brew install sdl2 ffmpeg pkg-config gettext
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake --parallel
ctest --test-dir build-cmake --output-on-failure
./build-cmake/scrctl --help
```

Debian/Ubuntu 的开发依赖为 `gettext python3 libsdl2-dev libavcodec-dev libavutil-dev libswscale-dev`。
JSON 和命令行使用 nlohmann/json 与 CLI11，缺少系统包时下载固定源码。
离线可用 `FETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON` / `FETCHCONTENT_SOURCE_DIR_CLI11`
指定源码；`SCRCTL_FETCH_DEPENDENCIES=OFF` 禁止下载缺失依赖。
`SCRCTL_LIBAV=OFF` 关闭本地 FFmpeg，`SCRCTL_BUILD_TESTS=OFF` 关闭前端测试。

设备连接、DDI、实时媒体、音频解码和录制的依赖及构建开关属于独立的
[XDeviceBridge](https://github.com/YJBeetle/XDeviceBridge)。scrctl 构建只启用客户端，
不会构建或打包 Server；请先安装独立的 `xdb`，运行时自动启动 Server。
macOS / Windows 安装包配套提供同一轮 CI 的[对应源码、配方与补丁](docs/SOURCE_MATERIALS.md)。
平台构建与安装检查见 [macOS](docs/MACOS.md) / [Windows](docs/WINDOWS.md)。

## 首次连接设备

USB 连接通常需要以下设备准备：

1. 开启开发者模式，按设备提示重启。
2. 插线后在设备上信任这台电脑。
3. 准备与系统和机型适配的开发者磁盘镜像（DDI）。运行
   `scrctl --install-ddi -s <UDID>` 原生下载、校验并安装；
   iOS / iPadOS 17–26 选择 Personalized，27 选择 Cryptex；16 及以下可下载已有的
   Classic 镜像，但这些旧系统的 CoreDevice 功能尚未验证。
   见 [DDI 下载与安装](docs/DDI.md)。

系统更新后需要重新检查 DDI。iOS 27.0.1 实测中，旧 Xcode 26.2 的 17C52
镜像仍被报告为可用，但不提供屏幕、截图和 HID 服务。改用新版 Cryptex DDI 后
恢复起流；具体检查和替换步骤见 [开发镜像准备](docs/coredevice.md#34-ios-2701-更新后的开发镜像准备)。

DDI 会影响 `com.apple.coredevice.*` 服务是否出现在目录中。服务已出现也不保证某个
feature 可用：已有 iPadOS 18.7.8 记录中，设备拒绝实时媒体流并要求 iOS 27.0，
但截图和 HID 可用。应用遇到对应版本拒绝时可切换截图轮询，也可使用
`--video-source screenshot` 强制选择；截图模式不录制 Annex-B，也不启动音频。
该设备的验证过程见协议记录第 23、24 节。

Wi-Fi 需要设备可达和已有 RemotePairing 记录。使用 `scrctl --pair -s <UDID>`
经 USB 建立或验证记录；手机可能要求确认。旧记录缺少设备身份或被拒绝时，
使用 `scrctl --pair --repair-pairing -s <UDID>` 显式重配。
新记录通过独立连接验证后才保存，已有有效记录直接复用。验证边界见
[路线图](docs/ROADMAP.md)。
记录现在需要包含首次配对时校验并保存的设备标识和长期公钥；旧记录缺少这些字段时，
请通过 USB 或无线 PIN 重新配对。探针的 `--pmd3-record` 当前只导入主机密钥，因此也不能直接用于
设备身份验证。也可用 `scrctl --pair --wifi auto` 完成手机发起的首次无线 PIN 配对：
在手机“设置 → 隐私与安全性 → 开发者模式”中选择与 scrctl 电脑配对，输入电脑显示的 PIN。
本台 iOS 27 iPhone 已在 Mac、Debian ARM64 和 Windows ARM64 上完成首次无线配对、
新连接身份验证及独立新进程 `--wifi auto` 重连录制；测试记录保存于独立私有目录，
默认旧记录未替换。测试期间手机 USB 保留在 Mac，未做拔线验收。
手机撤销配对而电脑仍有旧记录时，可直接重新运行 `scrctl --pair --wifi auto`。
新的 PIN 配对通过独立连接验证后，会原子替换同一 UDID 的旧记录；验证失败或取消
保留旧记录。无需先手动删除本地记录。
`--pairing-timeout=300000` 可将配对预算增加到五分钟，默认两分钟；无线预算包含发现、
输入 PIN 和独立验证。`-s` 在首次无线配对时可省略；指定时会在身份验证后、
发送成功配对答复前核对设备 UDID。已有记录必须可读取、属于同一设备且为普通文件；
不同设备、损坏记录及符号链接会拒绝。新路径独占创建；已有同设备记录在新连接验证
后重新检查，若验证期间记录发生变化则拒绝写入。
广播和配对列表中的电脑名称直接使用本机主机名，不加 `scrctl` 前缀或后缀。
手机中的电脑型号显示为 `scrctl-macos`、`scrctl-linux` 或 `scrctl-windows`，
显示序列号为 `SCRCTL-` 加 16 位大写十六进制。后缀使用应用专属 SHA-256：
优先机器 UUID，读取不到时使用 Linux machine-id / Windows MachineGuid，
最后才使用当前配对身份。它是稳定的显示标识；虚拟机克隆或系统重装可能改变或重复，
不保证硬件唯一。原始机器标识不发送给手机；配对认证仍使用身份和密钥，
已有手机条目不会通过普通重连自动改名。
`wifi_probe --pair-setup --address <IP> --no-save` 仍仅用于协议实验。

```bash
scrctl --list-devices
scrctl --list-devices --discovery-timeout=0  # 只列 usbmux 设备
scrctl --stats
scrctl --wifi 192.168.1.50 --stats
scrctl --video-source screenshot
scrctl --help
```

`--list-devices` 合并 usbmux 与 RemotePairing 的发现结果，默认扫描无线服务约 3 秒；
`--discovery-timeout` 可以设置 0..60000 毫秒。USB 枚举另有默认 1 秒 I/O 预算；
异常 USB 来源会报告警告并继续无线扫描。每台设备保留所有地址、实际服务端口和
网络接口；IPv6 link-local 地址包含 `%接口索引`。USB 网络地址也可能出现在发现结果中，
列出地址不代表已经验证其可达性。

无线广播的标识与 UDID 不同。只有 `authTag` 与一份本地配对记录唯一匹配，才将无线
候选归到该记录的 UDID；未知设备、旧记录线索和匹配冲突分别显示状态。
“配对记录可用”表示本地材料完整，连接时仍需通过 PairVerify 验证设备身份。
扫描不会配对或启动媒体会话；多播被网络阻止时可以继续使用手动 `--wifi` 地址。
默认先在一秒预算内枚举 usbmux 中匹配的设备；确认没有候选时再发现已配对的无线设备。
枚举不可用或超时会显示警告并继续无线发现；完整列表存在歧义或 USB 协议连接失败时
直接报错。预算仅覆盖先导枚举，后续认证与连接仍使用各自的等待规则。
`--wifi auto -s <UDID>` 只走无线发现；`--wifi <地址> -s <UDID>` 跳过发现，
可用 `--wifi-port` 指定端口。`--wifi` 必须有非空地址或 `auto`，空值或全空白会报错。
未指定 `-s` 时只接受唯一可用设备，不任取第一台。
设备已出现在 usbmux 中但连接失败时，保留原错误，不悄悄改连无线会话。

核心接口为 `remote::discover_devices()`（`remote/Discovery.h`），返回设备、连接候选、
来源可用状态和诊断；调用方通过 `should_cancel` 回调取消，保留已经发现的快照。
该回调可读取原子标志，或捕获工具链支持的 `std::stop_token`，不要求系统 C++ 库提供后者。

## 剪贴板

`--copy TEXT` 请求写入设备剪贴板，`--paste` 读取设备剪贴板并输出文本。
这些命令完成后退出，可用于传递中文和 emoji；它们不会模拟设备上的粘贴动作。
同时指定两者时，先写入再读回，便于检查设备实际保存的内容。

```bash
scrctl --copy "中文🙂"
scrctl --paste
scrctl --copy "" --paste
```

空文本应作为独立参数传入，例如 `--copy ""`；`--copy=` 按缺少参数处理。
存在但长度为零的文本表示会成功读回；只有图片或没有文本表示时给出错误原因。
输出按文本字节长度写入，包含内嵌零字节，终端未必能直接显示这类字符。

## 研究工具

设备后端的研究记录保留在 [coredevice.md](docs/coredevice.md)。记录中的旧探针和
源码路径对应重构前版本，可从 `codex/scrctl-before-xdb-refactor` 查阅。
当前后端代码与离线协议测试在 XDeviceBridge；scrctl 只保留前端及打包验证。

## License

Apache-2.0
