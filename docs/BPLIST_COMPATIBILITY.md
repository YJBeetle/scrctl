# 媒体 offer 格式与 binary plist 评估（2026-10-07）

## 当前结论

起流不必在客户端生成 binary plist。本次 macOS USB 真机对照确认，同一份视频和音频
参数使用 XML plist 也能协商并收包；生产 offer 已改用现有 pugixml 写入器。
`negotiatorOffer` 仍是 XPC Data，其中两个 Data 字段使用 XML 的 Base64 表示，
zlib 媒体参数和 endpoint protobuf 的字节内容不变。

删除 `Bplist.cpp`、`Bplist.h`（合计 672 行）及 300 行独立测试，不再维护通用
binary plist 解析或序列化。研究工具 `feature_schema_probe` 只保留一份 42 字节的
空 bplist 字典样本，用于未知 Data 字段的类型探测；其原有探测行为不变。
媒体格式验证不代表其他 CoreDevice feature 的任意 Data 字段都接受 XML。

## 真机对照

设备：iPhone14,4 / iOS 27.0，macOS USB，通过已有信任与 DDI 连接。
先由旧构造器生成固定 SSRC / CallID 的 binary offer，再由项目 pugixml 写入器生成
相同 Value 的 XML；用 Python plistlib 逐值确认字典及两个 Data 载荷完全一致。
视频为 566 → 956 字节，音频为 461 → 816 字节。XML 多出的几百字节只在起流时传输。

每种媒体依次发送 binary → XML → binary；各观察 5 秒，租期 20 秒，不发送 RTCP，
避免重放 offer 与探针生成的 SSRC 不一致影响判断。每轮结束执行 stopAll。

| 顺序 | 视频 | 音频 |
| --- | --- | --- |
| binary 基线 | 起流成功，真实 IDR 1 个，会话仍存活 | RTP 501 个，RTCP 5 个，会话仍存活 |
| XML | 起流成功，真实 IDR 1 个，会话仍存活 | RTP 500 个，RTCP 5 个，会话仍存活 |
| binary 复测 | 起流成功，真实 IDR 1 个，会话仍存活 | RTP 500 个，RTCP 5 个，会话仍存活 |

随后用修改后的生产程序运行：

```bash
./build/scrctl --lang en --no-window --no-audio-playback --stats \
  --time-limit 45 --test-degrade 8,14
```

输出 2230 帧，强制截图降级后恢复视频，截图失败为 0；视频和音频跨过 20 秒租期，
最后统计视频 RR 48 次、音频 RR 44 次，发送失败为 0。音频约 100 包/秒且持续解码，
解码失败、丢包和重启计数为 0。本次禁用了本地声卡输出，不作为听感验证。

以上结论覆盖本次设备与系统。其他受支持 iOS / iPadOS 版本的 XML 起流仍需设备回归。
不根据 Apple 通用 plist API 或第三方客户端选择 binary 的代码推断所有服务的格式要求。

## 移除前的库评估

此前先评估了替换 binary 编解码库；以下是当时保留实现并修复边界的依据。
后来真机 XML 对照通过，生产流程不再需要 binary 编解码，因此这些候选没有引入。

| 候选 | 可用部分 | 影响替换的限制 |
| --- | --- | --- |
| libplist 2.7.0 | 跨平台 C API，支持当前需要的常规类型、中文和 emoji | 内嵌 NUL 被截断；解析前无法设置项目的深度及展开量上限 |
| PlistCpp | MIT，支持 XML / binary | 上游 README 明确不支持 Unicode，无法接替当前字符串语义 |
| CoreFoundation | 当前 macOS 已使用该框架 | 不能作为项目跨平台的统一后端；本轮未新增平台分支 |

本机的 libplist 2.7.0 已实际解析中文、emoji、整数和内嵌 NUL 样本。
`A\0B` 的解析返回成功，但字符串 getter 的长度只有 1，内容为 `A`。
上游 `parse_string_node` 复制载荷后，用 `strlen` 存储长度；因此换成库之后会静默丢失数据。

libplist 的对象循环检查不能代替项目的资源限制。递归解析会复制重复引用的节点，
公开接口没有输入深度或展开节点预算参数。若直接调用后再检查 Value，检查发生时
库内递归和内存分配已经完成；要在之前限制这些行为，仍需要读取对象图。

原代码注释仅以 LGPL 名称认定 libplist 不可用，没有区分链接与分发安排，已删除。
上游许可为 LGPL-2.1-or-later；未来若采用，需要明确实际链接方式和分发声明。
本轮未增加 libplist 生产依赖，当时保留实现的理由是兼容性与资源控制。

## 本轮修复

- 长度整数只能使用 1、2、4、8 字节。原写入方对 65536..16777215 的长度写出
  3 字节内容，却用 4 字节标志，读回会取错长度。新增 65536 字节 data 往返验证。
- 对象内容和引用只能落在对象区，不能读取偏移表或尾部作为载荷。
- 只接受 bplist00，拒绝无法表示的 16 字节整数及无效 UTF-16 代理项。
- 输入上限 8 MiB，展开上限 65536 个节点 / 16 MiB 字符串和 data，嵌套最多 64 层。
  21 个对象组成的重复引用样本会展开超过百万个节点，现在提前返回错误。
- 写入前检查结构、深度与大小；UTF-8 有效性复用 OpenSSL 的 ASN.1 字符串校验，
  避免原 UTF-16 转换直接读取截断 UTF-8 后面的字节。
- 内嵌 NUL 的 binary 字符串继续完整往返；成功解析会清空上次错误文本。

新增边界测试和现有媒体协商测试通过；ASan 构建也通过这两项测试。
这些离线结果不证明所有未支持的 plist 类型可用。

## NUL 问题复现

安装 libplist 2.7.0 后，用 Python 的 plistlib 生成 `A\0B` 的 binary 文件，
调用 `plist_from_bin`，再用 `plist_get_string_ptr` 读取其长度和内容。
解析返回 PLIST_ERR_SUCCESS，但长度为 1。参见上游源码中 `parse_string_node`。

## 上游资料

- [libplist 2.7.0 binary 解析器](https://github.com/libimobiledevice/libplist/blob/2.7.0/src/bplist.c)
- [libplist API](https://github.com/libimobiledevice/libplist/blob/2.7.0/include/plist/plist.h)
- [libplist 许可](https://github.com/libimobiledevice/libplist/blob/2.7.0/COPYING)
- [PlistCpp README 的 Unicode 限制](https://github.com/animetrics/PlistCpp)
