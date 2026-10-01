# 待办与路线图

写给**接手的人**。这里只列"还没做的"。每条都标了**判据层级**——离线能判满的、必须要真机的、
以及只有编译期证据的。这个仓库的规矩是：**别把"编译过了"当"验过了"**，也别把"仪器读到 0"
当"没问题"（下面最后一条规矩里有为什么）。

- **可做** = 没人拍板要停，捡起来就能开工。
- **暂缓** = 维护者已明确决定不做，理由写在条目里。别当推荐项再提，除非前提变了。

---

## 可做

### A. Linux 真机那一半（判据：Linux 机器 + 真机）

CI 的 Ubuntu 格已经是门槛（配置、链接、`scrctl --help` 起得来、离线 ctest 全绿），但它只覆盖
"不需要手机的那一半"：**libusb、DDI 挂载、隧道建立一次都没对着真机跑过**——写这些代码的机器上
没有 Linux。音频那一路在 Linux 上**没有可解的后端**：设备发的是苹果专有 AAC-ELD，自由实现里
没有一个能解（`src/decode/AudioDecoder.h:35-41` 记了实测数字，docs §17.1 有全部判据）。

### B. Windows AMDS（判据：Windows 机器；现在连编译都过不去）

`transport/Usbmux.cpp` 用的是 POSIX 套接字，要走 AMDS 得先换掉那一层。这是平台覆盖上最大的洞。

### C. 两条隧道同时开 + 弱网下的流（判据：真机 + 能制造弱网）

USB 与 Wi-Fi 各自测过，**同时**开没测过；弱网（丢包/乱序）下的流也没测。

相关的一条已定取舍：隧道内 TCP 只接受期望序号的段，乱序一律丢弃并回重复 ACK，**不做重组**。
这是量过之后的决定——Wi-Fi 上 536.32 MB 只丢了 1 段（docs §27）。占比读数在 `--stats`，逐段
日志在 `--debug-net` 后面。如果将来 Wi-Fi 变成主路径、或占比涨到百分位，再回来做重组；那时要
一并加"最多缓存多少字节"的闸，否则对端一个远序号就能把我们撑爆。

### D. 小欠账（各自独立，判据基本是离线）

- **裁剪改读 SPS 的 conformance window**（`src/media/FramePump.h:353-354`）。现在表外机型的处置
  是"整幅当可见区"——宁可留一条噪声边也不凭猜裁掉真内容；读 conformance window 就不必列机型数字。
- **产品里的 `--pair`**（`src/app/main.cpp:1474`）。配对记录现在要靠 `tools/wifi_probe
  --pair-setup-xpc` 落，产品没有对应命令。
- **scrcpy parity 的剩余项**：`--record-orientation`（scrcpy 的 `--orientation` =
  `--display-orientation` + `--record-orientation`，我们只有前者，见 `main.cpp:351`）、水平翻转
  （`main.cpp:362`）、录制的随时起停（scrcpy 是 MOD+i）。做 parity 检查的方法是**跑
  `scrcpy --help` 抄权威列表再 diff**，别凭印象。
- **当库用时 `SIG_IGN` 的代价**（`src/transport/TlsChannel.cpp:131-150`）。进程级 `SIG_IGN` 是必要
  的第二层防护（fd 级 `SO_NOSIGPIPE` 在对端已关时实测 EINVAL，而那种 fd 恰恰最需要防护），但
  scrctl 被链进别的进程（MaaFramework 控制单元）时会改掉宿主的 SIGPIPE 处置。要不要做成"进入时
  保存、退出时还原"还没定。

---

## 暂缓（已拍板，别当推荐项）

### E. mDNS 发现与 authTag 匹配

现状：无线这条路是 `scrctl --wifi <地址>`——地址靠人给，端口靠 `kAdvertisedPortFallback` 兜底
（`src/remote/Device.h:61-64`），没给 `-s` 时靠"目录里正好一条配对记录就用它"猜
（`src/app/main.cpp:1486-1501`，那段注释自己写着发现做完就能删）。

**为什么暂缓**：主体不在 SipHash（60 行 + 论文附录 A 的 KAT 就能判满），而在主机侧多播查询、
DNS-SD 解析与跨平台多播（多网卡 `IP_MULTICAST_IF`、接口 index、IPv6 link-local 的 scope）。
Windows 那条只有编译期证据，Ubuntu CI 只能判 fixture。投入产出不划算。

**恢复时直接用下面这些已量到的事实，别重做调研**（docs §22.1 / §22.2 / §24，都是一手）：

- 设备广播 `_remotepairing._tcp`，TXT 四键 `identifier` / `authTag` / `flags=0` / `minVer=8` /
  `ver`（iPhone 26、iPad 24）；端口两台都是 **49152**，但那是观察不是协议保证，要从 SRV 拿。
- 广播里的 `identifier` 是**不透明 UUID，不是 UDID**（所以 `PairRecord` 同时存 `udid` 与
  `advertised_identifier`）。
- `authTag` = base64( SipHash-2-4(key = `altIRK`[16], msg = `identifier` 的 UTF-8 串) 的小端 8 字节
  取**前 6 字节逆序** )。`altIRK` 由 pair-setup 的 M6 落盘（`src/wifi/PairSetup.cpp:414-464`），
  真机对上过一次（`cuHWkXdk`）。
- **一次 browse 拿到的地址条数不稳定**：同一台设备连跑三次给 3/3/5 条，Wi-Fi 那条 A 记录有时不在，
  所以不能"查一次就走"，要多轮累积、按 SRV target 聚合。⚠️ 这个数是用参考实现的 Bonjour 接口量的，
  **自己写的查询器要重记基线**，不能借用。
- 地址族选择要小心：候选里混着 USB NCM 的 `fe80::%en6`、`169.254.x.x` 与 Wi-Fi 的 IPv4/IPv6。
  挑错会连上一个"能握手但走回 USB"的隧道。判据是连上后回读 `connection_type` / 隧道地址族。
- CLI 形状已定：**新增 `--discover` 列表**（尾号 + 地址 + 端口 + 匹配上的记录），再用 `-s` 连；
  发现与连接分两步。UDID 一律 mask，不打印记录内容或密钥材料。
- 设计取舍（别再摇摆）：查询器**不绑 5353**（要跟 mDNSResponder 抢端口、要 `SO_REUSEPORT`，跨平台
  很脏），改成绑临时端口、发往 mDNS 的组地址 **`224.0.0.251:5353`**（IPv6 `ff02::fb:5353`），
  QCLASS 带 QU 位（0x8001）请对端单播回我们。
  ⚠️ 这一行原来写的是 `224.0.0.5`，那是**别的协议的链路本地组地址、不是 mDNS**（审查 P3）。
  规格以 RFC 6762 §3 为准：IPv4 `224.0.0.251` / IPv6 `FF02::FB` / UDP `5353`；本机
  `pymobiledevice3/bonjour.py` 里也是这个值（只当物证看，代码一行不进产品）。照 `224.0.0.5`
  实现的现场后果是 IPv4 那一路**静默发现不到任何东西**——查询发出去了，没人应答，与"这台没广播"
  在日志里长得一模一样，除非同时试 IPv6 那一路才分得开。
- 更便宜的替代（也只作记录）：lockdown 顶层 `WiFiAddress` 与那条 A 记录的 ARP MAC 一致（实测），
  插着 USB 时能问 lockdown 要地址；缺点是 DHCP 换址即失效、且要求插线，那正是无线想摆脱的。
- 红线：SipHash 自己写（公开算法，论文 KAT 当离线判据），**不 vendor / 不依赖 pymobiledevice3
  （GPL-3.0）**，它只当 oracle。

拆解顺序：SipHash + authTag → mDNS 查询与解析 → 广播到记录的匹配 → CLI → 真机一轮。

### F. `--record` 出容器（MP4）

现状：`-r/--record FILE` 写的是**裸 Annex-B HEVC**（无容器、无音轨），`--play` 播同一份裸流
（`tests/` 与 fixtures 走的就是这条路，改的时候不能弄坏）。

**探针结论（一手，它改变了这条的形状）**：

- 本机 ffmpeg 9.0.2 的原生 aac 编码器**造不出 ELD**（`-profile:a aac_eld` → `Profile not
  supported!`）。所以"离线合成一个 ELD-in-MP4 样本去测播放器兼容性"这条路不通，真 ELD 帧只能从
  设备抓一次。
- libavcodec 的原生 aac 解码器**不支持 ELD**：按规范拼的 AudioSpecificConfig 连 `avcodec_open2`
  都过不去（"AAC data resilience is not implemented"）；换苹果自己那份 cookie 打得开，但 1404 帧
  只出 109 帧、每帧 512 采样、峰值顶满，是解歪的样子。libav 里的 AudioToolbox 壳 `aac_at` 同一条
  dump 也只出 109/1406。数字与判据在 docs §17.1 与 `src/decode/AudioDecoder.h:35-41`。
- ⇒ **ELD 原样装进 MP4，只有 Apple 侧能播，ffmpeg 系（Linux 上的主流播放器）解不开。** 所以
  "带音轨的 MP4"不能照 scrcpy 抄——scrcpy 那边是 Opus / AAC-LC / FLAC，处处能解。

音轨的四个选项（未拍板）：

| | 做法 | 代价 |
| --- | --- | --- |
| A | ELD 原样进 MP4 | 只有 Apple 能播 |
| B | 解成 PCM 进 MP4 | 到处能播，但 48k/2ch/s16 ≈ 192 KB/s；且 **ELD 解码器只在 Apple 有**，别处连录都录不了音 |
| C | 视频-only MP4，音频另存裸流 sidecar（或明说不录） | 音轨能力缺失，但容器那半能先落地 |
| D | ELD → AAC-LC 转码 | 要编码器、加 LGPL 依赖与 CPU、损上加损 |

倾向：**先做 C 的视频那半**，音轨单独立一条，别让它挡住容器。

muxer 取舍（碰依赖红线，要拍板）：**自写 MP4 muxer**（约 600 行量级、无新依赖、三平台一致，与
这个仓库已有的手搓 HTTP/2 帧层 / bplist / OPACK / SRP / 用户态 IP+TCP 栈同一风格）vs **接
libavformat**（代码少，但多一个可选 LGPL 依赖 + CI 要装 avformat 开发件，而且**它对音轨那半帮不上
忙**，ELD 解不了）。注意 libavcodec 现在已经是**可选**依赖（`CMakeLists.txt:84` `SCRCTL_LIBAV`，
找不到就只留平台后端）。

离线判据（本机有 ffprobe 9.0.2）：录一段 → ffprobe 看流数 / 时长 / codec → 用我们自己的 HEVC
解码器把轨里样本取出来对帧数与 PTS 单调；hvcC 要与 Annex-B 里的 VPS/SPS/PPS 一致；AU 要从
Annex-B 改成 4 字节长度前缀；`--play` 与 fixtures 那条裸流路必须照旧。

scrcpy 4.1 的权威 record 列表（`scrcpy --help` 抄的）：`-r, --record=file.mp4`（格式由
`--record-format` 决定，否则**看文件扩展名**）、`--record-format=mp4|mkv|m4a|mka|opus|aac|flac|wav`、
`--record-orientation=0|90|180|270`、MOD+i 随时起停。后三者我们没有。

---

## 已知的验证空缺（诚实清单）

- **Linux 上的 SIGPIPE 三层只有编译期证据**。macOS 上量过：拔线 A/B，对照臂（无防护）在拔线那一秒
  **exit 141、没有任何错误消息**，产品臂 exit 0 有序退出。
- **隧道内 TCP 的收发/丢弃计数没有离线判据**——`net::Stack` 造不出脱离真隧道的实例，判据是真机
  `--stats`。
- **"设备断开就有序退出"同样没有离线判据**（判据是 `Stack::pump_error()` 非空这个结构性信号，不是
  匹配错误文本）。真机读数：拔线后 1 秒自己退出、退出码 0、日志 `设备断开了（隧道已死：…），走正常
  退出路径`、且**不是**被 `--time-limit`（当时 120 秒）收走。
- **Windows 全线。**

---

## 环境与判据的规矩（都是踩出来的，接手请照办）

- 真机测会话寿命前**先查进程并清掉竞争者**：一台设备只容一条流，第二个 `startmediastream` 会把
  第一个从设备会话表里顶掉。当场区分"到点死"与"被顶掉"。
- 判"注入落地"要用**区域差分**，不能信返回成功（实测过"报成功、整屏不变"）。画布被自己的实验画满
  之后这条判据会**静默失效**，要换目标区域。
- **仪器要能自证**：一个 "0" 读数必须能和"表根本没接上"区分开。`--stats` 里那行 TCP 占比第一段
  无条件打印（哪怕丢弃是 0），就是为了这个；自证的办法是把分母一起打出来。
- 每条修复配一个**负对照 / 变异臂**；做变异臂之前先问"它破坏的变量是不是我正在读的那个"（踩过：
  变异臂读的是活状态，被测的正是清掉它的那段代码，于是"没修"看起来像"假装失败没发生"）。
- 别拿两条**不同代码路径**的观测去推断网络行为（踩过：ICMP 走对了、UDP 走错了，症状看起来像
  "设备按协议号丢包"，真因是同一函数族里只有一条被测过）。
- 无头自检：`--no-window` 或 `SDL_VIDEODRIVER=dummy`（配 `--verify N FILE` 回读）、
  `SDL_AUDIO_DRIVER=disk`（音频写成 `sdlaudio.raw`，量长度与分带峰值即可判整条路在不在给连续实音）。
  **别在没许可的情况下弹窗口给用户看。**
- 测试夹具**别放 `/tmp`**：macOS 每日清理按 mtime 收走过 `SCRCTL_TEST_STREAM`，同一棵构建树上
  午全绿、半夜变红。放仓库外的 `../fixtures/`，并保留运行期 Skipped。
- 构建小坑：ninja 在源文件与目标文件**同一秒时间戳**时会跳过重建，做负对照前 `touch` 一下；macOS
  没有 `timeout`，用 `perl -e 'alarm N; exec @ARGV' --`。
- **依赖红线**：不 vendor / 不依赖 pymobiledevice3（GPL-3.0）、libplist、nghttp2（LGPL）。plist、
  HTTP/2 帧层、SRP、OPACK、bplist、用户态 IP/TCP 栈都是自己写的，理由见 `docs/SCOPE.md` 的
  "依赖策略"。pymobiledevice3 只当**黑盒仪器**用（`tools/probe/`）。
