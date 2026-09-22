#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "plist/Plist.h"

namespace scrctl::plist {

/// 二进制 plist（bplist00）的读与写。
///
/// 为什么需要它：XML 形态只在 macOS 的配对记录上见到，其他地方——设备广播的
/// `deviceKVSData`、CoreDevice 媒体协商的 `negotiatorOffer` / `negotiatorAnswer`
/// ——都是二进制。写媒体协商包必须先能把 bplist 写对。
///
/// 依然不引 libplist（LGPL-2.1 与本项目的 Apache-2.0 静态链接不兼容），类型
/// 覆盖与 XML 那份同样的子集：bool / int / real / string / data / array / dict。
///
/// 格式（全大端）：`bplist00` + 一串对象 + 32 字节尾部 + 偏移表。每个对象以
/// 一个标志字节开头，高 4 位是类型、低 4 位是长度；低 4 位为 0xF 时改用
/// 「0x1X + X 字节大端计数」的长形态。尾部给出对象数、根对象编号、偏移表位置，
/// 以及偏移表和对象引用各占几字节。

/// 解析 bplist00。失败返回 nullopt 并给出原因。
std::optional<Value> parse_binary(const std::vector<uint8_t> &bytes, std::string *err = nullptr);
std::optional<Value> parse_binary(const uint8_t *data, std::size_t len, std::string *err);

/// 序列化成 bplist00。子集之外的输入（目前没有）返回空。
[[nodiscard]] std::vector<uint8_t> write_binary(const Value &v);

}  // namespace scrctl::plist
