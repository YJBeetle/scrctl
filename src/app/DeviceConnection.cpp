#include "app/DeviceConnection.h"
#include "wifi/PairRecord.h"
#include <cstdio>

namespace scrctl::app {

/// UDID 只打尾巴四位：日志会被贴到 issue 里，全号不该跟着出去。
std::string tail4(std::string_view s) {
    return s.size() <= 4 ? std::string(s) : "****" + std::string(s.substr(s.size() - 4));
}

/// 打开一个会话：`--wifi` 给了地址就走局域网那条，否则走 USB。
///
/// 无线这条需要一台**之前配过对**的设备：记录（`~/.local/share/scrctl/remote-*.pair`）
/// 由 `wifi_probe --pair-setup-xpc`（或产品里将来的 `--pair`）落下来。
///
/// 没给 `-s` 时的取舍：手上只有一个 IP，而记录按 UDID 存，所以要么让用户抄 UDID，
/// 要么在"目录里正好一条"时用它。选后者——配过对的设备通常就一台，而报错里那句
/// `remote-.pair`（UDID 为空）对读者毫无用处。多于一条时把候选尾号列出来。
/// mDNS 发现（#49：广播里的 authTag 对回记录的 altIRK）做完之后这一段就能删掉。
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
            std::printf("用目录里唯一一条配对记录（设备尾号 %s）\n", tail4(which).c_str());
        } else if (found.size() > 1) {
            err = "目录 " + dir + " 里有 " + std::to_string(found.size()) +
                  " 条配对记录，得用 -s 指一台：";
            for (const auto &udid : found) {
                err += " " + tail4(udid);
            }
            return std::nullopt;
        }
    }
    std::string load_err;
    auto record = scrctl::wifi::load_record(scrctl::wifi::record_path(dir, which), load_err);
    if (!record) {
        // 路径里带 UDID，所以这里只报目录，让读者自己去对文件名。
        err = "读不到 " + tail4(which) + " 的远程配对记录（目录 " + dir + "）：" + load_err +
              "。无线这条路要先配一次对——这台设备插过这台机器并配过对吗？";
        return std::nullopt;
    }
    return scrctl::remote::Device::establish_wifi(wifi, *record, err);
}

} // namespace scrctl::app
