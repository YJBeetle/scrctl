#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "wifi/Crypto.h"

namespace scrctl::wifi {

/// Apple 的 OPACK 二进制对象格式：pair-setup 的 M5/M6 用它在 TLV 的 INFO 里塞
/// 设备信息字典（altIRK / mac / name 那一堆）。这里只实现用得着的那个子集：
/// bool / 小整数 / 字符串 / 字节串 / 数组 / 字典（含 15 项以上的终止符形态）。
///
/// 字节序是这套格式里最容易错的一半：**整数载荷是小端**（0x30/0x32/0x33），
/// 而字符串与字节串的**长度前缀是大端**（0x61/0x62/0x91/0x92）。离线自检拿参考
/// 实现的编码器当 oracle 逐字节对过（tests/wifi_test.cpp）。
struct OpackValue {
    enum class Kind { kBool, kInt, kBytes, kString, kList, kDict } kind = Kind::kBool;
    bool boolean = false;
    int64_t integer = 0;
    Bytes bytes;
    std::string str;
    std::vector<OpackValue> list;
    std::vector<std::pair<OpackValue, OpackValue>> dict;  ///< 保序：设备会按顺序读

    static OpackValue of_bytes(Bytes b) {
        OpackValue v;
        v.kind = Kind::kBytes;
        v.bytes = std::move(b);
        return v;
    }
    static OpackValue of_string(std::string s) {
        OpackValue v;
        v.kind = Kind::kString;
        v.str = std::move(s);
        return v;
    }
    /// 按字符串键找字典值。找不到返回 nullptr。
    [[nodiscard]] const OpackValue *find(std::string_view key) const;
};

bool opack_encode(const OpackValue &value, Bytes &out, std::string &err);
bool opack_decode(const Bytes &in, OpackValue &out, std::string &err);

}  // namespace scrctl::wifi
