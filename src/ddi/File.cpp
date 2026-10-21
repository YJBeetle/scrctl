#include "ddi/File.h"
#include "i18n/Translation.h"

#include <fstream>
#include <limits>
#include <stdexcept>

namespace scrctl::ddi::detail {
std::string read_file(const std::filesystem::path &path, uint64_t limit) {
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(path)) ||
        !std::filesystem::is_regular_file(path))
        throw std::runtime_error(SCRCTL_TR("DDI cache contains a nonregular file"));
    const auto size = std::filesystem::file_size(path);
    if (size > limit || size > std::numeric_limits<size_t>::max() ||
        size > static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max()))
        throw std::runtime_error(SCRCTL_TR("DDI file exceeds its size limit"));
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error(SCRCTL_TR("Cannot open DDI file"));
    std::string bytes(static_cast<size_t>(size), '\0');
    file.read(bytes.data(), static_cast<std::streamsize>(size));
    if (!file || file.gcount() != static_cast<std::streamsize>(size) || file.peek() != EOF)
        throw std::runtime_error(SCRCTL_TR("Cannot read complete DDI file"));
    return bytes;
}
} // namespace scrctl::ddi::detail
