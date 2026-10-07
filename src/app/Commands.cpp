#include "app/Commands.h"

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

#include "app/DeviceConnection.h"
#include "app/Options.h"
#include "i18n/Translation.h"
#include "remote/App.h"
#include "remote/Pasteboard.h"

namespace scrctl::app {

std::optional<int> run_standalone_command(const Options &o) {
    if (o.show_version) {
        // 版本号由 CMake 的 project(VERSION) 生成，供二进制输出和问题报告使用。
        std::printf("scrctl %s\n", SCRCTL_VERSION_STRING);
        return 0;
    }

    if (o.list_devices) {
        std::string err;
        auto devices = scrctl::remote::Device::list(err);
        if (!err.empty()) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        for (const auto &d : devices) {
            std::printf("%s  %s\n", d.udid.c_str(), d.connection_type.c_str());
        }
        return 0;
    }

    if (o.list_apps) {
        std::string err;
        auto dev = open_device(o.serial, o.wifi, err);
        if (!dev) {
            std::fprintf(stderr, SCRCTL_TR("Failed to establish session: %s\n"), err.c_str());
            return 1;
        }
        std::vector<scrctl::remote::App::Entry> apps;
        if (!scrctl::remote::App::list(*dev, apps, err)) {
            std::fprintf(stderr, SCRCTL_TR("Failed to list apps: %s\n"), err.c_str());
            return 1;
        }
        for (const auto &e : apps) {
            std::printf("%s\t%s\n", e.bundle_id.c_str(), e.name.c_str());
        }
        std::printf(SCRCTL_TR("Total: %zu\n"), apps.size());
        return 0;
    }

    // 剪贴板命令不需要视频或窗口，执行后直接退出。键盘注入仅支持 US 布局的
    // ASCII；中文和 emoji 应使用剪贴板。
    // 同时指定 --copy 和 --paste 时，写入后读回验证。设备可能对无效结构返回
    // SET_REPLY 后丢弃内容，例如 types 为空时，仅检查写入回复不能确认保存成功。
    if (!o.copy_text.empty() || o.paste) {
        std::string err;
        auto dev = open_device(o.serial, o.wifi, err);
        if (!dev) {
            std::fprintf(stderr, SCRCTL_TR("Failed to establish session: %s\n"), err.c_str());
            return 1;
        }
        int rc = 0;
        if (!o.copy_text.empty()) {
            if (scrctl::remote::Pasteboard::set_text(*dev, o.copy_text, err)) {
                std::printf(SCRCTL_TR("Wrote %zu bytes to device clipboard\n"), o.copy_text.size());
            } else {
                std::fprintf(stderr, SCRCTL_TR("--copy failed: %s\n"), err.c_str());
                rc = 1;
            }
        }
        if (o.paste) {
            std::string text;
            if (scrctl::remote::Pasteboard::get_text(*dev, text, err)) {
                std::printf(SCRCTL_TR("Device clipboard (%zu bytes): %s\n"), text.size(), text.c_str());
            } else {
                std::fprintf(stderr, SCRCTL_TR("--paste failed: %s\n"), err.c_str());
                rc = 1;
            }
        }
        return rc;
    }

    return std::nullopt;
}

int launch_app(remote::Device &device, const std::string &app_spec) {
    std::string spec = app_spec;
    bool by_name = false;
    bool terminate = false;
    // 应用名称前缀使用 scrcpy 的顺序：先 ?（按名称），再 +（终止已有实例）。
    while (!spec.empty()) {
        if (spec.front() == '?') {
            by_name = true;
        } else if (spec.front() == '+') {
            terminate = true;
        } else {
            break;
        }
        spec.erase(spec.begin());
    }
    if (spec.empty()) {
        std::fprintf(stderr, SCRCTL_TR("--start-app requires a nonempty app name\n"));
        return 2;
    }
    std::string target = spec;
    if (by_name) {
        // 按名称启动需要先获取完整应用列表，回复可能有数 MiB；按 bundle ID 启动
        // 不需要这一步，因此通常更快。
        std::string lerr;
        std::vector<scrctl::remote::App::Entry> apps;
        if (!scrctl::remote::App::list(device, apps, lerr)) {
            std::fprintf(stderr, SCRCTL_TR("Failed to find app by name: %s\n"), lerr.c_str());
            return 1;
        }
        auto lower = [](std::string v) {
            for (char &c : v) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return v;
        };
        const std::string want = lower(spec);
        const scrctl::remote::App::Entry *hit = nullptr;
        for (const auto &e : apps) {
            if (lower(e.name).rfind(want, 0) == 0) {
                hit = &e;
                break;
            }
        }
        if (hit == nullptr) {
            std::fprintf(stderr, SCRCTL_TR("No app name starts with %s\n"), spec.c_str());
            return 1;
        }
        target = hit->bundle_id;
        std::printf("--start-app=?%s -> %s\n", spec.c_str(), target.c_str());
    }
    std::string lerr;
    if (!scrctl::remote::App::launch(device, target, lerr, terminate)) {
        std::fprintf(stderr, SCRCTL_TR("Failed to launch %s: %s\n"), target.c_str(), lerr.c_str());
        return 1;
    }
    std::printf(SCRCTL_TR("Launched %s%s\n"), target.c_str(), terminate ? SCRCTL_TR(" (previous instance terminated)") : "");
    return 0;
}

} // namespace scrctl::app
