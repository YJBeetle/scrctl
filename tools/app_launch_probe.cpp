// 检查应用启动请求和设备原始回复；--stop 改为调用应用停止接口。
// 启动 RPC 成功不保证应用画面已经显示，应另用截图确认。
// --shape 只构造固定示例应用的请求，不建立设备会话，也不发送启动或停止请求。
#include <CLI/CLI.hpp>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "i18n/CliLanguage.h"
#include "i18n/Translation.h"
#include "remote/App.h"
#include "remote/Device.h"
#include "xpc/XpcValue.h"

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string udid;
    std::vector<std::string> udids;
    std::string bundle;
    bool verbose = false;
    bool show_shape = false;
    bool terminate_existing = true;
    bool keep_running = false;
    bool do_stop = false;
    CLI::App app{SCRCTL_N_("Inspect app launch requests or stop an app")};
    app.footer(SCRCTL_N_(
        "Launch requests terminate an existing instance by default; --keep-running leaves it running. "
        "--shape prints a launch request for com.example.App and does not connect to a device. "
        "--help also runs without a device."));
    app.set_help_flag("-h,--help", SCRCTL_N_("Show help"));
    app.add_flag("-v,--verbose", verbose, SCRCTL_N_("Print connection and request details"));
    app.add_flag("--shape", show_shape, SCRCTL_N_("Print the example launch request without connecting"));
    app.add_flag("--stop", do_stop, SCRCTL_N_("Stop the app instead of launching it"));
    app.add_flag("--keep-running", keep_running,
                 SCRCTL_N_("Do not terminate an existing instance before launching"));
    app.add_option("BUNDLE_ID", bundle, SCRCTL_N_("App bundle ID (required unless --shape is used)"));
    app.add_option("UDID", udids, SCRCTL_N_("Device identifier (optional; last value is used)"))
        ->expected(-1);
    scrctl::i18n::CliLanguage language(app);
    try {
        app.parse(argc, argv);
        if (!language.select()) return 2;
    } catch (const CLI::CallForHelp &) {
        if (!language.select()) return 2;
        std::printf("%s", language.help().c_str());
        return 0;
    } catch (const CLI::ParseError &error) {
        if (language.select())
            std::fprintf(stderr, SCRCTL_TR("Invalid arguments: %s\n"), error.what());
        return 2;
    }
    if (keep_running) terminate_existing = false;
    // UDID 必须由字符串持有；多个位置 UDID 仍取最后一个。
    if (!udids.empty()) udid = udids.back();

    if (show_shape) {
        // 无论是否提供 bundle ID，都使用同一个离线样本检查请求字段及嵌套结构。
        std::printf("%s\n",
                    scrctl::xpc::describe(scrctl::remote::App::build_launch(
                                "com.example.App", terminate_existing))
                        .c_str());
        return 0;
    }
    if (bundle.empty()) {
        std::fprintf(stderr, SCRCTL_TR("A bundle ID is required unless --shape is used (see --help)\n"));
        return 2;
    }

    std::string err;
    auto device = scrctl::remote::Device::establish(udid, err, verbose);
    if (!device) {
        std::fprintf(stderr, SCRCTL_TR("Failed to establish device session: %s\n"), err.c_str());
        return 1;
    }
    std::printf(SCRCTL_TR("Device session ready: %s / iOS %s\n"), device->property("ProductType").c_str(),
                device->property("OSVersion").c_str());

    if (do_stop) {
        if (!scrctl::remote::App::stop(*device, bundle, err, verbose)) {
            std::fprintf(stderr, SCRCTL_TR("Failed to stop %s: %s\n"), bundle.c_str(), err.c_str());
            return 1;
        }
        // stop 返回成功可能表示没有匹配进程，也可能只确认信号请求成功；
        // 不据此声称发送过 SIGKILL 或已确认所有进程退出。
        std::printf(SCRCTL_TR(
            "Stop operation for %s completed without an error. To check for remaining processes, "
            "run applist_probe --grep with part of the executable path.\n"), bundle.c_str());
        return 0;
    }

    const auto t0 = std::chrono::steady_clock::now();
    // 直接保留 feature() 的回复，用于检查启动结果及进程字段。
    scrctl::xpc::Value reply;
    if (!device->feature("com.apple.coredevice.appservice",
                         "com.apple.coredevice.feature.launchapplication",
                         "com.apple.coredevice.action.launch",
                         scrctl::remote::App::build_launch(bundle, terminate_existing), reply, err,
                         verbose, 60000)) {
        std::fprintf(stderr, SCRCTL_TR("Failed to launch %s: %s\n"), bundle.c_str(), err.c_str());
        return 1;
    }
    std::printf(SCRCTL_TR("Raw reply: %s\n"), scrctl::xpc::describe(reply).substr(0, 1500).c_str());
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    std::printf(SCRCTL_TR(
                    "Launch request for %s succeeded (%lld ms). Use screenshot_probe to confirm "
                    "the app is visible.\n"),
                bundle.c_str(), static_cast<long long>(ms));
    return 0;
}
