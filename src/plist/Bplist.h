#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "plist/Plist.h"

namespace scrctl::plist {

/// 二进制 plist（bplist00）的读与写。
///
/// 用于设备广播的 deviceKVSData 和媒体协商的 negotiatorOffer / negotiatorAnswer。
/// 支持 bool / int64 / real / string / data / array / dict，与 XML 共用 Value。
/// 字符串保留内嵌 NUL，写入前验证 UTF-8；不支持的格式版本返回错误。
///
/// 当前保留自实现。libplist 2.7.0 的字符串长度使用 strlen，不能完整保留 NUL；
/// PlistCpp 不支持 Unicode。评估及复现见 docs/BPLIST_COMPATIBILITY.md。
///
/// 格式（全大端）：`bplist00` + 对象区 + 偏移表 + 32 字节尾部。每个对象以
/// 一个标志字节开头，高 4 位是类型、低 4 位是长度；低 4 位为 0xF 时改用
/// 「0x1X + X 字节大端计数」的长形态。尾部给出对象数、根对象编号、偏移表位置，
/// 以及偏移表和对象引用各占几字节。

/// 解析 bplist00。输入最多 8 MiB，嵌套最多 64 层；展开最多 65536 个节点、
/// 16 MiB 字符串与 data。失败返回 nullopt 并给出原因。
std::optional<Value> parse_binary(const std::vector<uint8_t> &bytes, std::string *err = nullptr);
std::optional<Value> parse_binary(const uint8_t *data, std::size_t len, std::string *err);

/// 序列化成 bplist00。无效 UTF-8、键值不匹配或超限的输入返回空。
[[nodiscard]] std::vector<uint8_t> write_binary(const Value &v);

}  // namespace scrctl::plist
