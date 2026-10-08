#pragma once

#include <string_view>

namespace scrctl::app {

/// 第一版容器录制按扩展名选择 MKV；其他路径继续使用原有裸 HEVC 入口。
inline bool is_matroska_path(std::string_view path) {
    if (path.size() < 4) return false;
    const auto suffix = path.substr(path.size() - 4);
    return suffix[0] == '.' && (suffix[1] == 'm' || suffix[1] == 'M') &&
           (suffix[2] == 'k' || suffix[2] == 'K') && (suffix[3] == 'v' || suffix[3] == 'V');
}

} // namespace scrctl::app
