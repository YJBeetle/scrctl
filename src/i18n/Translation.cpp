#include "i18n/Translation.h"

#include <atomic>
#include <cctype>
#include <cerrno>
#include <clocale>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <libintl.h>

namespace scrctl::i18n {
namespace {
std::atomic<Language> selected{Language::English};

std::string_view environment_locale() {
    for (const char *name : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char *value = std::getenv(name);
        if (value && *value)
            return value;
    }
    return {};
}

bool message_locale() {
    // GNU gettext 在 C / POSIX 下不加载翻译。优先使用中文 locale，系统未安装
    // 时使用其他 UTF-8 消息 locale，再通过 LANGUAGE 指向中文目录。
    for (const char *candidate : {"zh_CN.UTF-8", "zh_CN.utf8", "en_US.UTF-8", "en_US.utf8"}) {
        if (::setlocale(LC_MESSAGES, candidate))
            return true;
    }
#if !defined(__GLIBC__)
    // glibc 的 C.UTF-8 与 C 一样忽略 LANGUAGE，不能用它启用中文翻译。
    // macOS / Windows 的 libintl 保留原来的 UTF-8 回退行为。
    if (::setlocale(LC_MESSAGES, "C.UTF-8"))
        return true;
#endif
    return false;
}

std::filesystem::path executable_path() {
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) == 0)
        return buffer.data();
#elif defined(_WIN32)
    std::vector<wchar_t> buffer(32768);
    const auto size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size > 0 && size < buffer.size())
        return std::wstring(buffer.data(), size);
#elif defined(__linux__)
    std::error_code ec;
    auto result = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec)
        return result;
#endif
    return {};
}

std::string catalog_directory() {
    if (const char *value = std::getenv("SCRCTL_LOCALEDIR"); value && *value)
        return value;
    std::error_code ec;
    const auto executable = executable_path();
    if (!executable.empty()) {
        const auto beside_binary = executable.parent_path() / SCRCTL_RELATIVE_LOCALEDIR;
        if (std::filesystem::is_regular_file(beside_binary / "zh_CN/LC_MESSAGES/scrctl.mo", ec))
            return beside_binary.string();
    }
    if (std::filesystem::is_regular_file(
            std::filesystem::path(SCRCTL_INSTALL_LOCALEDIR) / "zh_CN/LC_MESSAGES/scrctl.mo", ec))
        return SCRCTL_INSTALL_LOCALEDIR;
    return SCRCTL_BUILD_LOCALEDIR;
}
} // namespace

Language language_for_locale(std::string_view locale) {
    const auto end = locale.find_first_of("_.@-");
    std::string prefix(locale.substr(0, end));
    for (char &c : prefix)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return prefix == "zh" ? Language::Chinese : Language::English;
}

bool initialize(std::string_view requested) {
    selected.store(Language::English, std::memory_order_release);
    if (requested != "auto" && requested != "en" && requested != "zh-CN")
        return false;
    const auto target = requested == "auto"    ? language_for_locale(environment_locale())
                        : requested == "zh-CN" ? Language::Chinese
                                               : Language::English;
    if (target == Language::English)
        return true;
    // 只有 C 类消息 locale 的系统无法通过 glibc gettext 加载中文；保留英文
    // 兜底，不把合法的中文选项误报为参数错误。安装中文或英文 UTF-8 locale 后可翻译。
    if (!message_locale()) {
#if defined(__GLIBC__)
        return true;
#else
        return false;
#endif
    }
#ifdef _WIN32
    if (_putenv_s("LANGUAGE", "zh_CN") != 0)
        return false;
#else
    if (setenv("LANGUAGE", "zh_CN", 1) != 0)
        return false;
#endif
    // 显式中文可以覆盖 LC_ALL=C。部分 libintl 实现仍从环境读取消息
    // locale，即使 setlocale 已成功，因此同步消息环境。初始化必须在线程前。
#ifdef _WIN32
    if (_putenv_s("LC_ALL", "") != 0 || _putenv_s("LC_MESSAGES", "zh_CN.UTF-8") != 0)
        return false;
#else
    if (unsetenv("LC_ALL") != 0 || setenv("LC_MESSAGES", "zh_CN.UTF-8", 1) != 0)
        return false;
#endif
    const auto directory = catalog_directory();
    if (!bindtextdomain("scrctl", directory.c_str()) || !bind_textdomain_codeset("scrctl", "UTF-8"))
        return false;
    selected.store(Language::Chinese, std::memory_order_release);
    return true;
}

Language language() { return selected.load(std::memory_order_acquire); }

const char *translate(const char *message) {
    if (language() == Language::English)
        return message;
    const int saved_errno = errno;
    const char *result = dgettext("scrctl", message);
    errno = saved_errno;
    return result;
}
} // namespace scrctl::i18n
