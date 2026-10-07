#include "i18n/Translation.h"
#include "wifi/PairRecord.h"

#include <algorithm>
#include <filesystem>

#ifdef _WIN32
#include "wifi/PairRecordWindows.h"
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "wifi/Tlv.h"

namespace scrctl::wifi {
namespace {

constexpr char kHeader[] = "scrctl-pair-record 1";
// 最多读取上限加一个检测字节，避免超大文件在大小校验前占用大量内存。
constexpr size_t kMaxRecordText = 16384;

std::string hex(const Bytes &data) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (const uint8_t b : data) {
        out += kDigits[b >> 4];
        out += kDigits[b & 0xF];
    }
    return out;
}

std::optional<Bytes> unhex(std::string_view text, std::string &err) {
    if (text.size() % 2 != 0) {
        err = SCRCTL_TR("Hex length must be even");
        return std::nullopt;
    }
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    Bytes out;
    out.reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2) {
        const int hi = nibble(text[i]), lo = nibble(text[i + 1]);
        if (hi < 0 || lo < 0) {
            err = SCRCTL_TR("Invalid hex character");
            return std::nullopt;
        }
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

/// 将外部提供的 UDID 转为文件名片段，仅保留 ASCII 字母和数字，其余替换为下划线。
/// 净化防止 UDID 引入目录跳转或路径分隔符，但不保证不同原始标识之间没有重名。
std::string sanitize(const std::string &udid) {
    std::string out;
    out.reserve(udid.size());
    for (const char c : udid) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9');
        out += ok ? c : '_';
    }
    return out;
}

/// 随机后缀临时文件与目标位于同目录，成功关闭后通过 rename 替换。
/// POSIX 路径不调用 fsync，成功返回不能作为断电后持久性的保证。
bool write_file(const std::string &path, std::string_view text, std::string &err) {
    const auto nonce = random_bytes(8, err);
    if (!nonce) return false;
    const std::string tmp = path + ".tmp." + hex(*nonce);
#ifdef _WIN32
    return write_private_record(tmp, path, text, err);
#else
    std::FILE *f = std::fopen(tmp.c_str(), "wb");
    if (f == nullptr) {
        err = SCRCTL_TR("Cannot open temporary file ") + tmp;
        return false;
    }
    // 在写入含私钥的记录前将临时文件权限设为 0600；设置失败即关闭并移除文件。
    if (::fchmod(::fileno(f), 0600) != 0) {
        err = SCRCTL_TR("Failed to set record file permissions");
        std::fclose(f);
        ::remove(tmp.c_str());
        return false;
    }
    const size_t written = std::fwrite(text.data(), 1, text.size(), f);
    const int closed = std::fclose(f);
    if (written != text.size() || closed != 0) {
        err = SCRCTL_TR("Incomplete record file write");
        ::remove(tmp.c_str());
        return false;
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        err = SCRCTL_TR("Rename failed");
        ::remove(tmp.c_str());
        return false;
    }
    return true;
#endif
}

std::optional<std::string> read_file(const std::string &path, std::string &err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = SCRCTL_TR("Cannot open ") + path;
        return std::nullopt;
    }
    std::string text(kMaxRecordText + 1, '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    const auto count = static_cast<size_t>(in.gcount());
    if (in.bad() || (in.fail() && !in.eof())) {
        err = SCRCTL_TR("Failed to read pairing record file ") + path;
        return std::nullopt;
    }
    if (count > kMaxRecordText) {
        err = SCRCTL_TR("Pairing record file too large");
        return std::nullopt;
    }
    text.resize(count);
    return text;
}

}  // namespace

std::string format_record(const PairRecord &record) {
    std::ostringstream out;
    out << kHeader << '\n';
    out << "udid=" << record.udid << '\n';
    out << "host_identifier=" << record.host_identifier << '\n';
    out << "host_private_key=" << hex(record.host_private_key) << '\n';
    out << "host_public_key=" << hex(record.host_public_key) << '\n';
    if (!record.advertised_identifier.empty()) {
        out << "advertised_identifier=" << record.advertised_identifier << '\n';
    }
    if (!record.peer_alt_irk.empty()) {
        out << "peer_alt_irk=" << hex(record.peer_alt_irk) << '\n';
    }
    if (!record.remote_unlock_host_key.empty()) {
        out << "remote_unlock_host_key=" << record.remote_unlock_host_key << '\n';
    }
    return out.str();
}

std::optional<PairRecord> parse_record(std::string_view text, std::string &err) {
    PairRecord record;
    bool first = true;
    bool seen_private = false;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) {
            nl = text.size();
        }
        const std::string_view line = trim(text.substr(pos, nl - pos));
        pos = nl + 1;
        if (line.empty()) {
            continue;
        }
        if (first) {
            first = false;
            if (line != kHeader) {
                err = SCRCTL_TR("Invalid scrctl pairing record header");
                return std::nullopt;
            }
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            err = SCRCTL_TR("Record line is not key=value");
            return std::nullopt;
        }
        const std::string_view key = trim(line.substr(0, eq));
        const std::string_view value = trim(line.substr(eq + 1));
        auto set_hex = [&](Bytes &field, size_t want) -> bool {
            const std::optional<Bytes> bytes = unhex(value, err);
            if (!bytes) {
                return false;
            }
            if (bytes->size() != want) {
                err = SCRCTL_TR("Incorrect field length: ") + std::string(key) + SCRCTL_TR(" requires ") + std::to_string(want) +
                      SCRCTL_TR(" bytes; got ") + std::to_string(bytes->size());
                return false;
            }
            field = *bytes;
            return true;
        };
        if (key == "udid") {
            record.udid = value;
        } else if (key == "host_identifier") {
            record.host_identifier = value;
        } else if (key == "host_private_key") {
            if (!set_hex(record.host_private_key, 32)) {
                return std::nullopt;
            }
            seen_private = true;
        } else if (key == "host_public_key") {
            if (!set_hex(record.host_public_key, 32)) {
                return std::nullopt;
            }
        } else if (key == "advertised_identifier") {
            record.advertised_identifier = value;
        } else if (key == "peer_alt_irk") {
            if (!set_hex(record.peer_alt_irk, 16)) {
                return std::nullopt;
            }
        } else if (key == "remote_unlock_host_key") {
            record.remote_unlock_host_key = value;
        }
        // 忽略未知键，允许新增可选字段后的记录继续由旧版本解析。
    }
    if (!seen_private) {
        err = SCRCTL_TR("Record missing host_private_key");
        return std::nullopt;
    }
    return record;
}

bool save_record(const std::string &path, const PairRecord &record, std::string &err) {
    const auto parent = std::filesystem::path(path).parent_path();
    const std::string dir = parent.string();
    if (!dir.empty()) {
        // 创建缺失的父目录；任何目录创建错误都在写入记录前返回。
        std::error_code ec;
        const bool existed = std::filesystem::is_directory(dir, ec);
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            err = SCRCTL_TR("Failed to create directory ") + dir + ": " + ec.message();
            return false;
        }
        if (!existed) {
            // 只限制新建的叶子目录。共享父目录及已有目录的权限由用户管理。
#ifdef _WIN32
            if (!protect_record_directory(dir, err)) return false;
#else
            std::filesystem::permissions(dir, std::filesystem::perms::owner_all, ec);
            if (ec) {
                err = std::string(SCRCTL_TR("Failed to set record file permissions")) + ": " + ec.message();
                return false;
            }
#endif
        }
    }
    return write_file(path, format_record(record), err);
}

std::optional<PairRecord> load_record(const std::string &path, std::string &err) {
    const std::optional<std::string> text = read_file(path, err);
    if (!text) {
        return std::nullopt;
    }
    return parse_record(*text, err);
}

std::string default_record_dir() {
    const char *xdg = std::getenv("XDG_DATA_HOME");
    if (xdg != nullptr && *xdg != '\0') {
        return std::string(xdg) + "/scrctl";
    }
#ifdef _WIN32
    if (const char *local = std::getenv("LOCALAPPDATA"); local && *local)
        return (std::filesystem::path(local) / "scrctl").string();
#endif
    const char *home = std::getenv("HOME");
    return std::string(home != nullptr ? home : ".") + "/.local/share/scrctl";
}

std::string record_path(const std::string &dir, const std::string &udid) {
    return dir + "/remote-" + sanitize(udid) + ".pair";
}

std::vector<std::string> list_record_udids(const std::string &dir, std::string &err) {
    std::vector<std::string> out;
    static constexpr std::string_view kPrefix = "remote-";
    static constexpr std::string_view kSuffix = ".pair";
    std::error_code ec;
    const std::filesystem::path root(dir);
    if (!std::filesystem::is_directory(root, ec)) {
        // 未识别为目录时统一返回空列表，包括不存在和 is_directory 查询失败。
        // 当前接口在此清除 err，调用方无法用该结果区分不存在与查询失败。
        err.clear();
        return out;
    }
    for (const auto &entry : std::filesystem::directory_iterator(root, ec)) {
        if (ec) {
            err = SCRCTL_TR("Reading directory ") + dir + SCRCTL_TR(" failed: ") + ec.message();
            break;
        }
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (name.size() <= kPrefix.size() + kSuffix.size() ||
            name.compare(0, kPrefix.size(), kPrefix) != 0 ||
            name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
            continue;
        }
        out.push_back(
            name.substr(kPrefix.size(), name.size() - kPrefix.size() - kSuffix.size()));
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace scrctl::wifi
