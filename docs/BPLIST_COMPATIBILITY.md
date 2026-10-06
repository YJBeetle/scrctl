# binary plist 库评估（2026-10-07）

## 结论

本轮保留当前 binary plist 实现，XML 继续使用 pugixml。
评估完成，不将“已经用库”记为已完成项。

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
本轮未增加 libplist 生产依赖，当前保留实现的理由是兼容性与资源控制。

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
