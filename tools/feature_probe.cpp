// 探针：走产品真正会用的那条路，调一个 CoreDevice feature 看回什么。
//
// 判据分两层：先确认「能开服务连接、发得出外壳、收得回 CoreDevice.output」，
// 再去碰带大载荷的路径。把这两件事分开设，是因为大载荷失败的样子和外壳写错
// 很像（都是"没回信"），一起测就分不清该改哪边。
#include <cstdio>
#include <string>

#include "remote/Device.h"
#include "xpc/XpcValue.h"

namespace {

using namespace scrctl::remote;

void show_features(const Rsd &rsd, std::string_view name) {
    const auto info = rsd.service(name);
    if (!info) {
        std::printf("  %-46s 目录里没有\n", std::string(name).c_str());
        return;
    }
    std::printf("  %-46s 端口=%u RemoteXPC=%s 加密=%s\n", std::string(name).c_str(),
                static_cast<unsigned>(info->port), info->uses_remote_xpc ? "是" : "否",
                info->encrypt_socket_data ? "是" : "否");
    for (const auto &f : info->features) {
        std::printf("      · %s\n", f.c_str());
    }
}

bool verbose_ = false;

bool call_and_dump(Device &dev, std::string_view service, std::string_view feature,
                   std::string_view action, const scrctl::xpc::Value &input) {
    std::string err;
    scrctl::xpc::Value out;
    std::printf("\n调用 %s\n", std::string(feature).c_str());
    if (!dev.feature(service, feature, action, input, out, err, verbose_)) {
        std::fprintf(stderr, "  失败: %s\n", err.c_str());
        return false;
    }
    const auto text = scrctl::xpc::describe(out);
    std::printf("  成功，输出 %zu 字节可读化后:\n    %s\n", text.size(),
                text.substr(0, 9000).c_str());
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // 用法：feature_probe [UDID] [--verbose] [--all]
    std::string_view udid;
    bool verbose = false;
    bool all = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "-v" || a == "--verbose") {
            verbose = true;
        } else if (a == "--all") {
            all = true;
        } else {
            udid = a;
        }
    }

    std::string err;
    auto devices = Device::list(err);
    if (devices.empty()) {
        std::fprintf(stderr, "没有在连设备: %s\n", err.c_str());
        return 1;
    }
    for (const auto &d : devices) {
        std::printf("在连: DeviceID=%u %s UDID=%s\n", d.device_id, d.connection_type.c_str(),
                    mask(d.udid).c_str());
    }

    auto dev = Device::establish(udid, err, verbose);
    if (!dev) {
        std::fprintf(stderr, "建立会话失败: %s\n", err.c_str());
        return 1;
    }
    std::printf("\n会话已建立：%s / %s / iOS %s（UDID %s）\n",
                dev->property("ProductType").c_str(), dev->property("HWModel").c_str(),
                dev->property("OSVersion").c_str(), mask(dev->udid(), 2).c_str());
    std::printf("RSD 目录共 %zu 个服务，本项目关心的：\n", dev->rsd().services().size());
    if (all) {
        // 全量列一遍：想知道"有没有能读设备日志的服务"这类问题时，
        // 只看自己预先想到的那几个名字是找不到的。
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

    // 目录条目的原始形状也打一份：像 UsesTLS 这种没被解读的字段，只看自己
    // 预先想到的那几个键是发现不了的。
    if (verbose) {
        const auto *entry = dev->rsd().service_entry("com.apple.coredevice.displayservice");
        std::printf("\n目录条目原文（displayservice）:\n  %s\n",
                    entry != nullptr ? scrctl::xpc::describe(*entry).c_str() : "(缺失)");
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

    std::printf("\n%s\n", all_ok ? "CoreDevice feature 调用链已通" : "存在失败项");
    return all_ok ? 0 : 1;
}
