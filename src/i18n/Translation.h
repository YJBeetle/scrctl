#pragma once

#include <string_view>

namespace scrctl::i18n {
enum class Language { English, Chinese };

/// 识别 locale 的语言部分；未支持、为空、C 及 POSIX 均回退英文。
Language language_for_locale(std::string_view locale);

/// 启动工作线程前选择语言。auto 按 LC_ALL、LC_MESSAGES、LANG 的顺序读取
/// 首个非空值；en / zh-CN 显式覆盖。只修改消息类别，不改变数字解析规则。
/// 返回 false 表示请求无效或运行环境无法启用中文，此时采用英文。
bool initialize(std::string_view requested = "auto");
Language language();
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format_arg(1)))
#endif
const char *translate(const char *message);
} // namespace scrctl::i18n

#define SCRCTL_TR(message) ::scrctl::i18n::translate(message)
#define SCRCTL_N_(message) message
