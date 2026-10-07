// 获取一张设备截图，查看 XPC 大回复及 PNG 头部尺寸。
// image 可直接包含 Data，也可通过 FileTransfer 在独立流上交付；具体形式由设备决定。
// 2026-10-03 的 iOS 27 实测返回约 4 MiB 的内联 Data。
#include <CLI/CLI.hpp>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "i18n/Translation.h"
#include "i18n/CliLanguage.h"
#include "remote/Device.h"
#include "xpc/XpcValue.h"

namespace {

/// PNG 签名之后的 IHDR 宽高使用大端编码。这里读取签名与尺寸，
/// 不替代解码器对完整文件、各区块长度和校验和的验证。
bool png_size(const std::vector<uint8_t> &b, uint32_t &w, uint32_t &h) {
    if (b.size() < 33) {
        return false;
    }
    static constexpr uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    for (int i = 0; i < 8; ++i) {
        if (b[static_cast<std::size_t>(i)] != kSig[i]) {
            return false;
        }
    }
    auto be32 = [&b](std::size_t at) {
        return static_cast<uint32_t>(b[at]) << 24 | static_cast<uint32_t>(b[at + 1]) << 16 |
               static_cast<uint32_t>(b[at + 2]) << 8 | static_cast<uint32_t>(b[at + 3]);
    };
    if (be32(8) != 13 || b[12] != 'I' || b[13] != 'H' || b[14] != 'D' || b[15] != 'R') {
        return false;
    }
    w = be32(16);
    h = be32(20);
    return w != 0 && h != 0;
}

}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string udid;
    std::string out_path = "/tmp/scrctl-shot.png";
    bool verbose = false;
    CLI::App app{SCRCTL_N_("Capture a device screenshot and save it as PNG")};
    app.footer(SCRCTL_N_(
        "With no UDID, use the connected device. --help does not connect to a device."));
    app.set_help_flag("-h,--help", SCRCTL_N_("Show help"));
    app.add_flag("-v,--verbose", verbose, SCRCTL_N_("Print device connection and request details"));
    app.add_option("-o,--out", out_path,
                   SCRCTL_N_("Output PNG path (default: /tmp/scrctl-shot.png)"))
        ->multi_option_policy(CLI::MultiOptionPolicy::TakeLast);
    // 位置参数只有 UDID；多个 UDID 必须报错，不能静默选择最后一个。
    app.add_option("UDID", udid, SCRCTL_N_("Device UDID"));
    scrctl::i18n::CliLanguage language(app);
    try {
        app.parse(argc, argv);
        if (!language.select()) return 2;
    } catch (const CLI::CallForHelp &) {
        if (!language.select()) return 2;
        std::printf("%s", language.help().c_str());
        return 0;
    } catch (const CLI::ParseError &e) {
        if (language.select()) {
            std::fprintf(stderr, SCRCTL_TR("Invalid arguments: %s\n"), e.what());
        }
        return 2;
    }

    std::string err;
    auto dev = scrctl::remote::Device::establish(udid, err, verbose);
    if (!dev) {
        std::fprintf(stderr, SCRCTL_TR("Failed to establish device session: %s\n"), err.c_str());
        return 1;
    }
    std::printf(SCRCTL_TR("Device: %s / iOS %s\n"), dev->property("ProductType").c_str(),
                dev->property("OSVersion").c_str());

    auto input = scrctl::xpc::make_dict();
    scrctl::xpc::dict_set(input, "displayUniqueID", scrctl::xpc::make_null());
    scrctl::xpc::dict_set(input, "requestedFormat", scrctl::xpc::make_string("png"));

    scrctl::xpc::Value out;
    if (!dev->feature("com.apple.coredevice.screencaptureservice",
                      "com.apple.coredevice.feature.capturescreenshot",
                      "com.apple.coredevice.action.capturescreenshot", input, out, err, verbose,
                      30000)) {
        std::fprintf(stderr, SCRCTL_TR("Failed to capture screenshot: %s\n"), err.c_str());
        return 1;
    }
    std::printf(SCRCTL_TR("Reply structure: %s\n"), scrctl::xpc::describe(out).substr(0, 400).c_str());

    const auto *image = out.find("image");
    if (image == nullptr) {
        std::fprintf(stderr, "%s\n", SCRCTL_TR("Screenshot reply contains no image field"));
        return 1;
    }
    if (image->data.empty()) {
        std::fprintf(stderr, "%s\n", SCRCTL_TR("Screenshot image contains no bytes"));
        return 1;
    }
    uint32_t w = 0, h = 0;
    const bool is_png = png_size(image->data, w, h);
    std::printf(SCRCTL_TR("Image: %zu bytes, %s\n"), image->data.size(),
                is_png ? SCRCTL_TR("PNG header recognized") : SCRCTL_TR("PNG header not recognized"));
    if (is_png) {
        std::printf(SCRCTL_TR("Image dimensions: %ux%u\n"), w, h);
    }

    FILE *f = std::fopen(out_path.c_str(), "wb");
    if (f == nullptr) {
        const int open_error = errno;
        std::fprintf(stderr, SCRCTL_TR("Failed to open %s: %s\n"), out_path.c_str(),
                     std::strerror(open_error));
        return 1;
    }
    errno = 0;
    const bool written = std::fwrite(image->data.data(), 1, image->data.size(), f) == image->data.size();
    const int write_error = written ? 0 : (errno != 0 ? errno : EIO);
    errno = 0;
    const bool closed = std::fclose(f) == 0;
    // 关闭可能刷新缓冲而失败；短写的首个原因不能被随后 fclose 的结果覆盖。
    const int save_error = write_error != 0 ? write_error : (closed ? 0 : (errno != 0 ? errno : EIO));
    if (save_error != 0) {
        std::fprintf(stderr, SCRCTL_TR("Failed to save %s: %s\n"), out_path.c_str(),
                     std::strerror(save_error));
        return 1;
    }
    std::printf(SCRCTL_TR("Saved to %s\n"), out_path.c_str());
    return is_png ? 0 : 1;
}
