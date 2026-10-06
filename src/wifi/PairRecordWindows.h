#pragma once
#include <string>
#include <string_view>

namespace scrctl::wifi {
// 新建文件从第一刻就使用当前用户的受保护 DACL，再在同目录替换目标文件。
bool write_private_record(const std::string &temporary, const std::string &path,
                          std::string_view text, std::string &err);
bool protect_record_directory(const std::string &path, std::string &err);
} // namespace scrctl::wifi
