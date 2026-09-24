// 探针：把 launchapplication 这条路走通，并把设备的回信原样打出来。
//
// 为什么单独验：这条 RPC 错一个键的表現不是"报字段错"，而是退到别的分支给一句
// 误导的话（把 bundle id 塞进 options，设备回的是"A URL to open must be specified"）。
// 所以判据必须是"屏幕上真的换成了那个 App"，而不是"设备没说不行"。
//
// 用法：app_launch_probe [--shape] [--keep-running] [-v] <bundle id> [UDID]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "remote/App.h"
#include "remote/Device.h"
#include "xpc/XpcValue.h"

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string_view udid;
    std::string bundle;
    bool verbose = false;
    bool show_shape = false;
    bool terminate_existing = true;
    bool do_stop = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-v" || a == "--verbose") {
            verbose = true;
        } else if (a == "--shape") {
            show_shape = true;
        } else if (a == "--stop") {
            do_stop = true;
        } else if (a == "--keep-running") {
            // 不动在跑的实例，只是把它带到前台。Android 那边 start_app 是
            // `monkey -p <pkg> 1`，语义就是这个，不是冷启动。
            terminate_existing = false;
        } else if (a == "-h" || a == "--help") {
            std::printf("用法: %s [--shape] [--stop] [--keep-running] [-v] <bundle id> [UDID]\n",
                        argv[0]);
            return 0;
        } else if (a.starts_with("-")) {
            std::fprintf(stderr, "未知选项 %s\n", a.c_str());
            return 2;
        } else if (bundle.empty()) {
            bundle = a;
        } else {
            udid = a;
        }
    }

    if (show_shape) {
        // 只看形状，不碰设备：改键名时先用它自检，省一次真机往返。
        std::printf("%s\n",
                    scrctl::xpc::describe(scrctl::remote::App::build_launch(
                                "com.example.App", terminate_existing))
                        .c_str());
        return 0;
    }
    if (bundle.empty()) {
        std::fprintf(stderr, "要一个 bundle id（--help）\n");
        return 2;
    }

    std::string err;
    auto device = scrctl::remote::Device::establish(udid, err, verbose);
    if (!device) {
        std::fprintf(stderr, "建立会话失败: %s\n", err.c_str());
        return 1;
    }
    std::printf("会话就绪：%s / iOS %s\n", device->property("ProductType").c_str(),
                device->property("OSVersion").c_str());

    if (do_stop) {
        if (!scrctl::remote::App::stop(*device, bundle, err, verbose)) {
            std::fprintf(stderr, "stop %s 失败: %s\n", bundle.c_str(), err.c_str());
            return 1;
        }
        std::printf("已发出 SIGKILL\n");
        return 0;
    }

    const auto t0 = std::chrono::steady_clock::now();
    // 直接走 feature() 而不是 App::launch()：这条探针要的就是**回信原文**，
    // 想知道里面有没有进程句柄（有的话 stop_app 就不用绕 streamapplist 了）。
    scrctl::xpc::Value reply;
    if (!device->feature("com.apple.coredevice.appservice",
                         "com.apple.coredevice.feature.launchapplication",
                         "com.apple.coredevice.action.launch",
                         scrctl::remote::App::build_launch(bundle, terminate_existing), reply, err,
                         verbose, 60000)) {
        std::fprintf(stderr, "启动 %s 失败: %s\n", bundle.c_str(), err.c_str());
        return 1;
    }
    std::printf("回信原文: %s\n", scrctl::xpc::describe(reply).substr(0, 1500).c_str());
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    std::printf("设备接受了 %s（%lldms）。屏幕该换成它了——用 screenshot_probe 核对。\n",
                bundle.c_str(), static_cast<long long>(ms));
    return 0;
}
