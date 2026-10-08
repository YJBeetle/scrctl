#pragma once

#include <CLI/CLI.hpp>
#include <charconv>
#include <string>
#include <string_view>

namespace scrctl::probe {

inline bool parse_decimal_integer(std::string_view input, int &value, int minimum, int maximum) {
    constexpr std::string_view whitespace = " \t\r\n\v\f";
    while (!input.empty() && whitespace.find(input.front()) != std::string_view::npos) { input.remove_prefix(1); }
    while (!input.empty() && whitespace.find(input.back()) != std::string_view::npos) { input.remove_suffix(1); }
    if (!input.empty() && input.front() == '+') {
        input.remove_prefix(1);
        if (!input.empty() && (input.front() == '-' || input.front() == '+')) { return false; }
    }
    if (input.empty()) { return false; }
    int parsed = 0;
    const auto result = std::from_chars(input.data(), input.data() + input.size(), parsed, 10);
    if (result.ec != std::errc{} || result.ptr != input.data() + input.size() ||
        parsed < minimum || parsed > maximum) { return false; }
    value = parsed;
    return true;
}

// CLI11 的整数转换可能识别八进制/十六进制。旧探针的 stoi/atoi 默认按十进制；
// 先完整解析十进制并规范化，再交给 CLI11 保存，保留 010=10 和 +10=10 的合法行为。
inline CLI::Validator decimal_integer(int minimum, int maximum) {
    return CLI::Validator([minimum, maximum](std::string &text) -> std::string {
        int value = 0;
        if (!parse_decimal_integer(text, value, minimum, maximum)) {
            return "expected a complete decimal integer in " + std::to_string(minimum) + ".." + std::to_string(maximum);
        }
        text = std::to_string(value);
        return {};
    }, "DECIMAL", "decimal_integer");
}

}  // namespace scrctl::probe
