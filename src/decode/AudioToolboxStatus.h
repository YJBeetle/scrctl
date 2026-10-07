#pragma once

#include <cctype>
#include <cstdint>
#include <string>

namespace scrctl::decode::detail {

/// 内部诊断辅助函数，不依赖 Apple 类型。OSStatus 按有符号十进制显示；
/// 四个字节均可打印时附加完整四字符码，保留现有字符分类规则。
inline std::string osstatus_text(int32_t st) {
    const auto u = static_cast<uint32_t>(st);
    const char raw[4] = {static_cast<char>((u >> 24) & 0xFF), static_cast<char>((u >> 16) & 0xFF),
                         static_cast<char>((u >> 8) & 0xFF), static_cast<char>(u & 0xFF)};
    std::string text = std::to_string(st);
    bool printable = true;
    for (const char c : raw) {
        if (std::isprint(static_cast<unsigned char>(c)) == 0) {
            printable = false;
        }
    }
    if (printable) {
        text += std::string(" ('") + std::string(raw, sizeof raw) + "')";
    }
    return text;
}

}  // namespace scrctl::decode::detail
