#include "i18n/Translation.h"
#include "app/DeviceConnection.h"
#include "wifi/PairRecord.h"
#include <cstdio>

namespace scrctl::app {

/// 日志只显示 UDID 最后四位，避免问题报告包含完整设备标识。
std::string tail4(std::string_view s) {
    return s.size() <= 4 ? std::string(s) : "****" + std::string(s.substr(s.size() - 4));
}

/// 指定 --wifi 时建立局域网会话，否则使用 USB。无线连接需要本机已有远程
/// 配对记录，当前可用 wifi_probe --pair-setup-xpc 创建。
/// 未指定 -s 时只自动选择唯一记录；有多条记录则要求指定设备，并列出尾号。
/// 后续可用 mDNS authTag 与记录 altIRK 匹配设备。
std::optional<scrctl::remote::Device> open_device(const std::string &serial,
                                                  const std::string &wifi, std::string &err) {
    if (wifi.empty()) {
        return scrctl::remote::Device::establish(serial, err);
    }
    const std::string dir = scrctl::wifi::default_record_dir();
    std::string which = serial;
    if (which.empty()) {
        std::string list_err;
        const std::vector<std::string> found = scrctl::wifi::list_record_udids(dir, list_err);
        if (found.size() == 1) {
            which = found.front();
            std::printf(SCRCTL_TR("Using the only remote pairing record (device suffix %s)\n"), tail4(which).c_str());
        } else if (found.size() > 1) {
            err = SCRCTL_TR("Directory ") + dir + SCRCTL_TR(" contains ") + std::to_string(found.size()) +
                  SCRCTL_TR(" pairing records; specify the device with -s. Candidate suffixes: ");
            for (const auto &udid : found) {
                err += " " + tail4(udid);
            }
            return std::nullopt;
        }
    }
    std::string load_err;
    auto record = scrctl::wifi::load_record(scrctl::wifi::record_path(dir, which), load_err);
    if (!record) {
        // 报错只显示记录目录和设备尾号，完整文件路径包含 UDID。
        err = SCRCTL_TR("Cannot read ") + tail4(which) + SCRCTL_TR(" remote pairing record (directory ") + dir + SCRCTL_TR("): ") + load_err +
              SCRCTL_TR(". Establish a remote pairing record over USB first.");
        return std::nullopt;
    }
    return scrctl::remote::Device::establish_wifi(wifi, *record, err);
}

} // namespace scrctl::app
