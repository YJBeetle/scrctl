#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace scrctl::ddi {

struct DownloadOptions {
    // Empty selects the platform cache directory. A supplied root must be absolute.
    std::filesystem::path cache_root;
    std::chrono::seconds timeout{300};
    std::function<bool()> cancelled;
    // File name, bytes downloaded for this file, expected file size.
    std::function<void(const std::string &, uint64_t, uint64_t)> progress;
};

struct DownloadResult {
    std::filesystem::path directory;
    std::string build;
    std::string source_url;
    bool cache_hit = false;
};

// Downloads the pinned Cryptex DDI only. Does not connect to or modify a device.
// Returns nullopt and a diagnostic on failure, timeout or cancellation.
std::optional<DownloadResult> download_cryptex(const DownloadOptions &options, std::string &error);
std::optional<std::filesystem::path> default_cache_root(std::string &error);
// Validates every pinned file, its digest and the Cryptex manifest structure.
bool validate_cryptex_cache(const std::filesystem::path &directory, std::string &error);

} // namespace scrctl::ddi
