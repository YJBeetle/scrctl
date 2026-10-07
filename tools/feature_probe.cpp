// 列出设备与 RSD 服务目录，并通过正式会话查询媒体支持和流服务状态。
// 回复按原始 XPC 结构显示，最多保留前 9000 字节，便于检查字段及错误内容。
#include <CLI/CLI.hpp>
#include <cstdio>
#include <string>
#include <vector>

#include "i18n/CliLanguage.h"
#include "i18n/Translation.h"
#include "remote/Device.h"
#include "xpc/XpcValue.h"

namespace {

using namespace scrctl::remote;

void show_features(const Rsd &rsd, std::string_view name) {
    const auto info = rsd.service(name);
    if (!info) {
        std::printf(SCRCTL_TR("  %-46s not found in service directory\n"),
                    std::string(name).c_str());
        return;
    }
    std::printf(SCRCTL_TR("  %-46s port=%u RemoteXPC=%s encrypted=%s\n"),
                std::string(name).c_str(), static_cast<unsigned>(info->port),
                info->uses_remote_xpc ? SCRCTL_TR("yes") : SCRCTL_TR("no"),
                info->encrypt_socket_data ? SCRCTL_TR("yes") : SCRCTL_TR("no"));
    for (const auto &f : info->features) {
        std::printf("      · %s\n", f.c_str());
    }
}

bool verbose_ = false;

bool call_and_dump(Device &dev, std::string_view service, std::string_view feature,
                   std::string_view action, const scrctl::xpc::Value &input) {
    std::string err;
    scrctl::xpc::Value out;
    std::printf(SCRCTL_TR("\nQuerying %s\n"), std::string(feature).c_str());
    if (!dev.feature(service, feature, action, input, out, err, verbose_)) {
        std::fprintf(stderr, SCRCTL_TR("  Query failed: %s\n"), err.c_str());
        return false;
    }
    const auto text = scrctl::xpc::describe(out);
    std::printf(SCRCTL_TR("  Query succeeded; formatted reply is %zu bytes:\n    %s\n"), text.size(),
                text.substr(0, 9000).c_str());
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::vector<std::string> udids;
    bool verbose = false;
    bool all = false;
    CLI::App app{SCRCTL_N_("Inspect CoreDevice services and query media support")};
    app.footer(SCRCTL_N_(
        "Lists connected devices and queries media support and stream status. "
        "--help does not connect to a device."));
    app.set_help_flag("-h,--help", SCRCTL_N_("Show help"));
    app.add_flag("-v,--verbose", verbose,
                 SCRCTL_N_("Print connection details and the raw service entry"));
    app.add_flag("--all", all, SCRCTL_N_("Also list every service name in the RSD directory"));
    app.add_option("UDID", udids, SCRCTL_N_("Device identifier (optional; last value is used)"))
        ->expected(-1);
    scrctl::i18n::CliLanguage language(app);
    if (auto code = language.parse(argc, argv)) return *code;
    // 参数处理结束后才枚举设备；多个位置参数仍取最后一个 UDID。
    const std::string udid = udids.empty() ? std::string{} : udids.back();

    std::string err;
    auto devices = Device::list(err);
    if (devices.empty()) {
        if (err.empty()) {
            std::fprintf(stderr, "%s\n", SCRCTL_TR("No connected devices"));
        } else {
            std::fprintf(stderr, SCRCTL_TR("Failed to list connected devices: %s\n"), err.c_str());
        }
        return 1;
    }
    for (const auto &d : devices) {
        std::printf(SCRCTL_TR("Connected device: DeviceID=%u %s UDID=%s\n"), d.device_id,
                    d.connection_type.c_str(), mask(d.udid).c_str());
    }

    auto dev = Device::establish(udid, err, verbose);
    if (!dev) {
        std::fprintf(stderr, SCRCTL_TR("Failed to establish device session: %s\n"), err.c_str());
        return 1;
    }
    std::printf(SCRCTL_TR("\nDevice session ready: %s / %s / iOS %s (UDID %s)\n"),
                dev->property("ProductType").c_str(), dev->property("HWModel").c_str(),
                dev->property("OSVersion").c_str(), mask(dev->udid(), 2).c_str());
    std::printf(SCRCTL_TR("RSD directory: %zu services; showing relevant entries below:\n"),
                dev->rsd().services().size());
    if (all) {
        // 先列全部服务名，再显示下方指定服务的详细信息。
        for (const auto &s : dev->rsd().services()) {
            std::printf("  %s\n", s.name.c_str());
        }
    }
    for (const auto *name : {"com.apple.coredevice.displayservice",
                             "com.apple.coredevice.screencaptureservice",
                             "com.apple.coredevice.hid.indigo",
                             "com.apple.coredevice.hid.universalhidservice",
                             "com.apple.coredevice.pasteboardservice",
                             "com.apple.coredevice.appservice",
                             "com.apple.coredevice.devicecontrol"}) {
        show_features(dev->rsd(), name);
    }

    verbose_ = verbose;
    const auto empty = scrctl::xpc::make_dict();

    // 保留未经本工具解释的目录字段，便于检查服务声明。
    if (verbose) {
        const auto *entry = dev->rsd().service_entry("com.apple.coredevice.displayservice");
        std::printf(SCRCTL_TR("\nRaw service entry (displayservice):\n  %s\n"),
                    entry != nullptr ? scrctl::xpc::describe(*entry).c_str()
                                     : SCRCTL_TR("(missing)"));
    }

    bool all_ok = true;
    all_ok &= call_and_dump(
        *dev, "com.apple.coredevice.displayservice",
        "com.apple.coredevice.feature.getmediasupportinfo",
        "com.apple.coredevice.action.mediastreamgetsupportinfo", empty);
    all_ok &= call_and_dump(
        *dev, "com.apple.coredevice.displayservice",
        "com.apple.coredevice.feature.getmediastreamserverstatus",
        "com.apple.coredevice.action.mediastreamstatus", empty);

    std::printf("\n%s\n", all_ok ? SCRCTL_TR("Both media queries succeeded")
                                : SCRCTL_TR("One or more media queries failed"));
    return all_ok ? 0 : 1;
}
