#pragma once

#include "media/RecordingMuxer.h"

#include <optional>
#include <string_view>

namespace scrctl::app {

/// 容器格式由扩展名选择，大小写均可；其他路径继续使用原有裸 HEVC 入口。
inline std::optional<media::RecordingMuxer::Format> record_container_format(std::string_view path) {
    if (path.size() < 4) return std::nullopt;
    const auto suffix = path.substr(path.size() - 4);
    if (suffix[0] != '.' || (suffix[1] != 'm' && suffix[1] != 'M')) return std::nullopt;
    if ((suffix[2] == 'k' || suffix[2] == 'K') && (suffix[3] == 'v' || suffix[3] == 'V'))
        return media::RecordingMuxer::Format::Matroska;
    if ((suffix[2] == 'p' || suffix[2] == 'P') && suffix[3] == '4')
        return media::RecordingMuxer::Format::Mp4;
    return std::nullopt;
}

} // namespace scrctl::app
