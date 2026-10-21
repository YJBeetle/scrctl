#include "util/Hex.h"
#include "i18n/Translation.h"

#include <algorithm>

namespace scrctl::util {
namespace {
int nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}
std::string hex_encode(std::span<const uint8_t> bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (auto b : bytes) { result += digits[b >> 4]; result += digits[b & 15]; }
    return result;
}
std::optional<std::vector<uint8_t>> hex_decode(std::string_view text, std::string &error) {
    if (text.size() % 2 != 0) { error = SCRCTL_TR("Hex length must be even"); return std::nullopt; }
    std::vector<uint8_t> bytes;
    bytes.reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2) {
        const int hi = nibble(text[i]), lo = nibble(text[i + 1]);
        if (hi < 0 || lo < 0) { error = SCRCTL_TR("Invalid hex character"); return std::nullopt; }
        bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return bytes;
}
bool is_lower_hex(std::string_view text) {
    return std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
} // namespace scrctl::util
