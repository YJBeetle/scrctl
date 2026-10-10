#pragma once
#include "ddi/Download.h"
#include <string_view>
#include <vector>

namespace scrctl::ddi::detail {
// Test seam: production always supplies the built-in HTTPS-only catalog/transport.
struct Asset {
    std::string name;
    uint64_t size;
    std::string sha256;
};
struct Catalog {
    std::string build;
    std::string source_url;
    std::vector<Asset> assets;
};
using Sink = std::function<bool(const char *, size_t)>;
using Transfer = std::function<bool(const std::string &, uint64_t, std::chrono::milliseconds,
                                    const std::function<bool()> &, const Sink &, std::string &)>;
const Catalog &cryptex_catalog();
std::optional<DownloadResult> download(const DownloadOptions &, const Catalog &, const Transfer &,
                                     std::string &error);
bool validate(const std::filesystem::path &, const Catalog &, std::string &error);
} // namespace scrctl::ddi::detail
