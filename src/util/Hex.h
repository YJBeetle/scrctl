#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace scrctl::util {
std::string hex_encode(std::span<const uint8_t> bytes);
// Accepts upper- and lowercase digits; failure reports an error without output.
std::optional<std::vector<uint8_t>> hex_decode(std::string_view text, std::string &error);
bool is_lower_hex(std::string_view text);
} // namespace scrctl::util
