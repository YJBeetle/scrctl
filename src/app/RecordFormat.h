#pragma once

#include "media/RecordingMuxer.h"

#include <optional>
#include <string_view>

namespace scrctl::app {

/// HEVC 是本项目的裸视频扩展；容器格式沿用 RecordingMuxer 的格式类型。
enum class RecordFormat { Hevc, Mp4, Matroska };

/// 显式格式优先于扩展名。未指定时保留原有选择规则；裸 HEVC 没有容器音轨。
inline std::optional<media::RecordingMuxer::Format> record_container_format(
        std::string_view path, std::optional<RecordFormat> format = std::nullopt) {
    if (path.empty()) return std::nullopt;
    if (format) {
        switch (*format) {
        case RecordFormat::Mp4: return media::RecordingMuxer::Format::Mp4;
        case RecordFormat::Matroska: return media::RecordingMuxer::Format::Matroska;
        case RecordFormat::Hevc: return std::nullopt;
        }
        return std::nullopt;
    }
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
