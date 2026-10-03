# scrctl 第一轮重构

scrctl 是独立产品，也是设备协议与恢复行为的验证项目。MaaFramework 后续倾向于
参考验证过的实现独立接入；本轮不引入公共 SDK、稳定 C ABI 或插件机制。

## 应用职责

| 模块 | 职责 |
| --- | --- |
| `main.cpp` | 调用应用入口 |
| `Application` | 处理命令、组装源、协调事件循环和退出 |
| `Options` / `Cli` | 参数值、CLI11 声明与校验、生成帮助 |
| `DeviceConnection` | USB / Wi-Fi 连接选择与配对记录选择 |
| `Presenter` | SDL 窗口、渲染、回读和鼠标坐标映射 |
| `FrameSource` | 应用内统一的已解码画面来源 |
| `FileSource` | Annex-B 文件解析、解码和队列背压 |
| `LiveSource` | 设备会话、画面源切换、输入与统计 |
| `AudioOut` | SDL 声卡输出；音频接收与解码仍在 AudioPump |

`src/app` 内这些类型是应用实现，不承诺外部接口稳定。
媒体恢复状态机继续由 FramePump / ScreenshotSource 承担，模块拆分保留其调用顺序、
退避、线程回收和停止逻辑。LiveSource 的恢复编排和 Application 的命令分支仍可继续细化。

## 通用解析

- JSON 语法、序列化和值存储使用 nlohmann/json，旧 jsonlite 实现已删除。
- `src/json/Json` 只保留协议输入限制及非抛异常的取值辅助，不维护第二套值类型。
- 输入最多 4 MiB、嵌套最多 64 层；深度超限立即终止解析。
- JSON 整数向 int64 转换时检查溢出。配对 XPC 的 sequenceNumber 继续使用 uint64，
  其他整数字段超出 XPC int64 范围时报告错误。
- CLI11 处理短选项、别名、等号语法、参数个数、类型和帮助；项目只处理 crop、颜色、
  方向、verify 等领域规则。保留现有选项名称和有效值的含义。
- 非法数字、非有限/非正 scale、负尺寸/时间和带尾部垃圾的 crop / 颜色现在直接报错，
  不再像原实现一样静默转成零或接受部分字符串。参数错误退出码仍为 2。
- JSON 现在严格拒绝原解析器可能接受的非法数字、控制字符及孤立代理项。
  JSON 小数的文本表示由库决定，协议验证应比较值和 XPC 类型，不依赖小数拼写。

依赖优先用系统 CMake 包；缺失时 FetchContent 使用固定版本、SHA256 校验的上游归档。
不将上游源码复制进仓库。离线方式见 README。

## 后续阶段

1. 对接 lwIP 自定义 netif 的可行性验证：覆盖隧道内 TCP 重传、乱序、连接关闭及多连接。
2. 验证 nghttp2 与 RemoteXPC 空 HEADERS、双向流、流控和文件传输的兼容性。
   nghttp2 是 MIT 许可，旧注释写成 LGPL 已纠正；本轮尚未引入它。
3. 媒体源切换和重连状态的进一步简化。

这些替换必须各自建立协议判据，再切换实现，不能只凭一次握手成功判断完整兼容。

## 验证边界

本轮需要完整构建、现有离线测试、新增 CLI 校验和 JSON 输入边界测试通过。
这些测试不证明真机上的持续音画、触摸落地、反复断流恢复或 Wi-Fi 重连。
真机回归应覆盖 USB / Wi-Fi 握手、实时流与强制截图、旋转、音频、输入、录制和
`--test-degrade` 的反复切换与退出。

## 第二轮：XML plist / Base64

- XML plist 的语法和转义交给 pugixml 1.16（MIT）。项目保留 plist 类型映射、
  单根结构校验、8 MiB 输入和 64 层值嵌套上限；不把非验证 XML 解析器当成完整 XML 校验器。
- binary plist 编解码保持原实现，共享 Value 移到 PlistValue.cpp。
- XML 输出的缩进和空元素拼写由库决定。数值尾部垃圾和非有限 real 现在直接拒绝。
- plist 和无线协议的 Base64 共用 OpenSSL EVP；保留 MIME 换行及省略尾部 padding 的支持，
  拒绝错误字符、长度和 padding。
- 真机短测：USB 截图 6 张、实时流 12 秒 222 帧；热点 Wi-Fi 截图 5 张、
  实时流 12 秒 222 帧。两条实时路径各完成两轮强制截图降级和恢复，正常退出。
- USB 手动 RemoteXPC 配对成功，新记录重新 pair-verify 通过，再使用该记录建立 Wi-Fi 会话。
  USB 免确认升级配对被设备拒绝；热点 RemotePairing 端口不接受直接 pair-setup。
- 本轮未验证长时间运行、物理断线、音频、输入、旋转和录制。强制切换测试也不等同于真实断网恢复。

## 第三轮：SDL 生命周期

- SdlRuntime 统一管理 SDL 初始化、部分初始化失败后的清理和 SIGINT / SIGTERM。
  初始化前保存调用者处理器，初始化后接管退出请求，SDL_Quit 后恢复原处理器。
- Application 通过声明顺序保证窗口、媒体源及声卡先析构，SDL 最后退出。
  输入测试、窗口创建失败等提前返回路径不再遗漏 SDL_Quit。
- 退出标志采用保证无锁的 atomic_flag；每次初始化清零，避免连续运行继承退出状态。
- 新增离线生命周期测试：信号转换和处理器恢复、重复初始化运行、视频初始化失败、
  无窗口回放退出后的 SDL 子系统清理。19 项离线测试通过。
- 真机使用热点 Wi-Fi 验证带音频流的 SIGINT、SIGTERM 和无效按键参数提前返回。
  音频出口使用 dummy 后端，验证开启与退出过程，不证明真实声卡输出或听感。
  本轮 USB 列表为空，未能重测 USB；上一轮 USB 短测不能代替本轮运行证明。
