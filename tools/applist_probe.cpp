// 默认查询设备进程，--list 则查询已安装应用的 bundle ID、名称及路径。
// 停止应用时需要把安装路径与进程的 executableURL 对应；本工具用于查看这两类数据。
// --grep 仅过滤进程回复，--list 始终显示完整应用列表。
#include <CLI/CLI.hpp>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "i18n/CliLanguage.h"
#include "i18n/Translation.h"
#include "remote/App.h"
#include "remote/Device.h"
#include "xpc/XpcValue.h"

namespace {

constexpr std::string_view kService = "com.apple.coredevice.appservice";

void unused_placeholder() {}
}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string udid;
    std::vector<std::string> udids;
    bool do_list = false;
    std::string grep;
    bool verbose = false;
    CLI::App app{SCRCTL_N_("Inspect device processes or installed apps")};
    app.footer(SCRCTL_N_(
        "Shows up to three processes by default. --grep shows all matching processes; "
        "--list shows every installed app and ignores --grep. --help does not connect to a device."));
    app.set_help_flag("-h,--help", SCRCTL_N_("Show help"));
    app.add_flag("-v,--verbose", verbose, SCRCTL_N_("Print connection and request details"));
    app.add_flag("--list", do_list, SCRCTL_N_("List installed apps instead of processes"));
    app.add_option("--grep", grep, SCRCTL_N_("Show processes containing TEXT; ignored with --list"))
        ->type_name("TEXT")
        ->multi_option_policy(CLI::MultiOptionPolicy::TakeLast);
    app.add_option("UDID", udids, SCRCTL_N_("Device identifier (optional; last value is used)"))
        ->expected(-1);
    scrctl::i18n::CliLanguage language(app);
    if (auto code = language.parse(argc, argv)) return *code;
    // UDID 使用拥有存储的字符串，不能引用参数解析中的临时字符串。
    // 保留多个位置 UDID 取最后一个的行为，帮助和错误在建立会话前返回。
    if (!udids.empty()) udid = udids.back();

    std::string err;
    auto device = scrctl::remote::Device::establish(udid, err, verbose);
    if (!device) {
        std::fprintf(stderr, SCRCTL_TR("Failed to establish device session: %s\n"), err.c_str());
        return 1;
    }
    std::printf(SCRCTL_TR("Device session ready: %s / iOS %s\n"),
                device->property("ProductType").c_str(),
                device->property("OSVersion").c_str());

    if (!do_list) {
        // 原样显示 processTokens，用于检查进程标识及 executableURL。
        scrctl::xpc::Value procs;
        if (!device->feature(kService, "com.apple.coredevice.feature.listprocesses", "",
                             scrctl::xpc::make_dict(), procs, err, verbose, 30000)) {
            std::fprintf(stderr, SCRCTL_TR("Failed to list device processes: %s\n"), err.c_str());
            return 1;
        }
        const auto *tokens = procs.find("processTokens");
        if (tokens == nullptr) {
            std::printf(SCRCTL_TR("Reply: %s\n"),
                        scrctl::xpc::describe(procs).substr(0, 400).c_str());
            return 0;
        }
        std::printf(SCRCTL_TR("Process tokens: %zu\n"), tokens->array.size());
        // 有筛选条件时显示全部匹配进程，便于确认某个应用是否仍在运行。
        // 无筛选条件时只显示前三条，避免大量进程信息淹没终端。
        int shown = 0;
        for (const auto &t : tokens->array) {
            const std::string line = scrctl::xpc::describe(t);
            if (!grep.empty()) {
                if (line.find(grep) != std::string::npos) {
                    std::printf("  %s\n", line.c_str());
                    ++shown;
                }
            } else if (shown < 3) {
                std::printf("  %s\n", line.c_str());
                ++shown;
            }
        }
        if (!grep.empty()) {
            std::printf(SCRCTL_TR("Processes matching %s: %d\n"), grep.c_str(), shown);
        }
        return 0;
    }

    std::vector<scrctl::remote::App::Entry> apps;
    if (!scrctl::remote::App::list(*device, apps, err, verbose)) {
        std::fprintf(stderr, SCRCTL_TR("Failed to list installed apps: %s\n"), err.c_str());
        return 1;
    }
    std::printf(SCRCTL_TR("Installed apps: %zu\n"), apps.size());
    for (const auto &e : apps) {
        std::printf("  %-52s %-28s %s\n", e.bundle_id.c_str(), e.name.c_str(),
                    e.path.c_str());
    }
    return 0;
}
