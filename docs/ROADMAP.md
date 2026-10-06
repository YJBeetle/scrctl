# 待办与路线图

当前实现和逐轮验证见 [REFACTOR.md](REFACTOR.md)。协议原始观察保留在
[coredevice.md](coredevice.md)，早期方案保留在 [SCOPE.md](SCOPE.md)。
旧记录中的手写 TCP、不做乱序缓存、稳定 C ABI 等说明已不代表当前方向。

## 继续推进

### 1. 平台与真实网络验证

- Linux：构建和离线测试已有 CI 作业，仍需 Linux 机器上的设备连接、DDI、镜像、输入
  及断线退出验证。本轮本机结果来自 macOS，尚未推送取得新 CI 结果。
- Windows：ARM64 构建、24 项离线回归及 Wi-Fi 镜像 / 截图切换已通过，新增 ARM64 CI
  及安装检查尚未取得远端结果。原生 AMDS USB、音频、输入控制、新建配对、x64 和 MSVC
  待验证，见 [Windows 说明](WINDOWS.md)。发布产物还需完善对应第三方源码及分发材料。
- lwIP：离线模拟已覆盖丢包、重传、多个连接 / 网络接口和取消等待；USB / Wi-Fi
  真机短测及三分钟强制截图往返已通过。真实弱网、物理拔插、多台设备及长时间运行待测。
- SRP：原参考向量和库失败注入已通过，摘要接口修改后仍需重新建立一次真机配对。
  使用已有记录的 PairVerify 不覆盖 SRP pair-setup。

### 2. 协议与生命周期复审

- HTTP/2：nghttp2 可用于已验证的控制流，但偶数文件流不符合其客户端会话模型。
  当前保留帧子集；文件子流映射和真机 FileTransfer 尚缺验证，见
  [适配记录](NGHTTP2_COMPATIBILITY.md)。后续还需复审输入缓冲与流状态边界。
- binary plist：继续保留并维护受限实现。libplist 的内嵌 NUL 和 PlistCpp 的 Unicode
  兼容性尚不能满足现有数据；输入、展开和编码边界已有离线判据，见
  [评估记录](BPLIST_COMPATIBILITY.md)。
- 应用：继续收敛 Application 命令分支和 LiveSource 统计职责，并对照旧 review
  逐项确认仍有效的问题；模块拆分不等于所有生命周期路径已经验证。
- 库嵌入：TLS 当前使用进程级 SIGPIPE 忽略策略，会影响宿主信号处理。MaaFramework
  接入暂缓，后续需要在宿主场景中评估这一行为；当前不建立公共 SDK 或稳定 C ABI。

### 3. 文案和功能完善

- 应用与核心输出已有中英文 gettext 消息；默认 auto 跟随 locale，未支持时回退英文。
  应用、媒体、RemoteXPC 及部分密码代码注释已整理，其他底层注释、研究探针的独立输出
  和历史文档还需继续整理。新增提示使用翻译目录维护检查。
- 图像裁剪：评估读取 SPS conformance window，减少机型尺寸表依赖。需要保留设备可见区、
  编码填充、界面朝向和触摸坐标之间的对应关系。
- 远程配对：产品当前只有连接已有记录的入口；新建配对仍通过研究探针。产品命令需要
  明确记录选择、用户确认、错误和保存流程，再做真机验证。
- 音频：当前仅有 macOS AudioToolbox 后端。已有 FFmpeg 测试不能证明其他库不支持 ELD；
  [FDK AAC 的接口说明](https://github.com/mstorsjo/fdk-aac/blob/master/libAACdec/include/aacdecoder_lib.h)
  提供低延迟 AAC 支持，可独立评估配置与真机码流兼容性，尚未接入生产。
- 其他 CLI 功能：录制方向、水平翻转、录制随时起停等需要按具体需求推进；参数范围以
  当前 `scrctl --help` 为准。

## 暂缓

### 无线发现与记录匹配

当前使用 `--wifi <地址>`，未指定 serial 时仅在存在唯一配对记录时自动选择。
发现需要处理多网卡、IPv6 scope、广播地址和配对记录匹配，当前暂缓。
恢复时优先评估现成 DNS-SD 与 SipHash 实现，不将旧文档的手写方案视为约束。

已有设备观察可供后续验证使用：

- 服务类型 `_remotepairing._tcp`，端口应从 SRV 获取；49152 只是已有设备的观察值。
- TXT `identifier` 为不透明 UUID，不是 UDID；记录中分别保留 UDID 与广播 identifier。
- `authTag` 使用记录的 altIRK 与 identifier 做 SipHash-2-4，再按设备约定取六字节。
- 一次 browse 可能缺少 Wi-Fi 地址，需多轮累积并按 SRV target 聚合；USB NCM 地址
  与 Wi-Fi 地址可能同时出现，连接后仍需确认实际路径。
- mDNS 组地址为 IPv4 `224.0.0.251`、IPv6 `ff02::fb`，端口 5353；
  规范见 [RFC 6762](https://www.rfc-editor.org/rfc/rfc6762.html)。

此前的 CLI 方案为先列发现结果，再按设备选择连接。恢复开发时重新确认方案，
日志只显示设备尾号，不输出配对记录或密钥。

### 容器录制

当前 `--record` 保存裸 Annex-B HEVC，无容器和音轨，`--play` 播放同一格式。
MP4 等容器录制暂缓。恢复时优先评估 libavformat，不以已有手写协议实现作为
再写 muxer 的理由；需要验证 VPS / SPS / PPS、AU 转换、帧数和时间戳。

音轨需另行确定：原样保存 AAC-ELD、解码为 PCM、转码，或先只录视频。
已有 FFmpeg 后端测试只说明所测配置不兼容，不能推定所有非 Apple 播放器或库均不可用。

### MaaFramework

后续倾向参考 scrctl 已验证的协议独立实现；scrctl 同时作为独立产品发展。
当前按用户安排先完成 scrctl，不修改 MaaFramework。

## 验证方法

- 一台设备测试时只运行一个媒体探针，避免会话竞争影响结果；仅检查本任务相关进程。
- 输入落地通过目标区域变化验证，RPC 成功不能单独证明画面或设备状态改变。
- 故障注入需确认破坏了被测变量，保留修复前失败与修复后通过的对照。
- 统计同时记录计数范围、分母和采样时长；不同采样窗口的速率不能直接相减。
- 离线测试、CI、真机短测和长期运行分别记录，不能相互替代。
- 无窗口测试使用 `--no-window`；需要回读时可用 SDL dummy 驱动。
- 长期使用的码流夹具放在稳定的仓库外目录，避免临时目录自动清理。
- pymobiledevice3 仅作研究工具，不复制其源码或作为运行依赖。
