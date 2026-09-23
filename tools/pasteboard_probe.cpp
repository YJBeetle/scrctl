// 探针：读写设备剪贴板，并把它当"输入中文/任意文本"的路径验证一遍。
#include <cstdio>
#include <string>

#include "remote/Device.h"
#include "remote/Pasteboard.h"

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    bool verbose = false;
    std::string set_to;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-v" || a == "--verbose") {
            verbose = true;
        } else if (a == "--set" && i + 1 < argc) {
            set_to = argv[++i];
        }
    }

    std::string err;
    auto dev = scrctl::remote::Device::establish({}, err, verbose);
    if (!dev) {
        std::fprintf(stderr, "建立会话失败: %s\n", err.c_str());
        return 1;
    }
    std::printf("会话就绪：%s / iOS %s\n", dev->property("ProductType").c_str(),
                dev->property("OSVersion").c_str());

    if (!set_to.empty()) {
        if (!scrctl::remote::Pasteboard::set_text(*dev, set_to, err, verbose)) {
            std::fprintf(stderr, "写入失败: %s\n", err.c_str());
            return 1;
        }
        std::printf("已写入剪贴板：%zu 字节\n", set_to.size());
    }

    std::string text;
    if (!scrctl::remote::Pasteboard::get_text(*dev, text, err, verbose)) {
        std::fprintf(stderr, "读回失败: %s\n", err.c_str());
        return 1;
    }
    std::printf("剪贴板内容（%zu 字节）：%s\n", text.size(), text.c_str());
    return 0;
}
