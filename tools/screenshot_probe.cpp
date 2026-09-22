// 探针：走 screencaptureservice 抓一张设备屏幕，同时验证 XPC 文件传输这条路。
//
// 截图回信里 `image` 不放消息本体，而是一个 FileTransfer 说明大小、真字节由设备
// 在另一条 HTTP/2 流上推。这条路必须单独验：它和视频流一样是「设备反向推数据」，
// 收不满字节就会卡住后面的所有调用。
#include <cstdio>
#include <string>
#include <vector>

#include "remote/Device.h"
#include "xpc/XpcValue.h"

namespace {

/// PNG 的宽高在 IHDR 里，大端。取出来是为了证明「字节完整且真是图」，
/// 而不是只看到一串长度对得上的数字。
bool png_size(const std::vector<uint8_t> &b, uint32_t &w, uint32_t &h) {
    if (b.size() < 24) {
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
    w = be32(16);
    h = be32(20);
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // 用法：screenshot_probe [-v] [-o 输出路径] [UDID]
    // 输出路径走 -o 而不是第二个位置参数：位置参数只有一个含义（UDID），
    // 否则 "-v /tmp/x.png" 会被当成指定了一台设备，报错信息看着像设备没连。
    std::string_view udid;
    std::string out_path = "/tmp/scrctl-shot.png";
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "-v" || a == "--verbose") {
            verbose = true;
        } else if (a == "-o" || a == "--out") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "-o 后面要跟路径\n");
                return 2;
            }
            out_path = argv[++i];
        } else if (a.starts_with("-")) {
            std::fprintf(stderr, "未知选项 %s\n", std::string(a).c_str());
            return 2;
        } else {
            udid = a;
        }
    }

    std::string err;
    auto dev = scrctl::remote::Device::establish(udid, err, verbose);
    if (!dev) {
        std::fprintf(stderr, "建立会话失败: %s\n", err.c_str());
        return 1;
    }
    std::printf("设备：%s / iOS %s\n", dev->property("ProductType").c_str(),
                dev->property("OSVersion").c_str());

    auto input = scrctl::xpc::make_dict();
    scrctl::xpc::dict_set(input, "displayUniqueID", scrctl::xpc::make_null());
    scrctl::xpc::dict_set(input, "requestedFormat", scrctl::xpc::make_string("png"));

    scrctl::xpc::Value out;
    if (!dev->feature("com.apple.coredevice.screencaptureservice",
                      "com.apple.coredevice.feature.capturescreenshot",
                      "com.apple.coredevice.action.capturescreenshot", input, out, err, verbose,
                      30000)) {
        std::fprintf(stderr, "截图失败: %s\n", err.c_str());
        return 1;
    }
    std::printf("回信结构: %s\n", scrctl::xpc::describe(out).substr(0, 400).c_str());

    const auto *image = out.find("image");
    if (image == nullptr) {
        std::fprintf(stderr, "回信里没有 image 字段\n");
        return 1;
    }
    if (image->data.empty()) {
        std::fprintf(stderr, "image 里没有字节（文件流没收回来）\n");
        return 1;
    }
    uint32_t w = 0, h = 0;
    const bool is_png = png_size(image->data, w, h);
    std::printf("image: %zu 字节，%s\n", image->data.size(),
                is_png ? "PNG 头校验通过" : "PNG 头校验失败");
    if (is_png) {
        std::printf("画面尺寸 %ux%u\n", w, h);
    }

    FILE *f = std::fopen(out_path.data(), "wb");
    if (f == nullptr) {
        std::fprintf(stderr, "写 %s 失败\n", out_path.c_str());
        return 1;
    }
    std::fwrite(image->data.data(), 1, image->data.size(), f);
    std::fclose(f);
    std::printf("已存 %s\n", out_path.c_str());
    return is_png ? 0 : 1;
}
