#pragma once

#include <string>
#include <vector>

#include "wifi/Crypto.h"

namespace scrctl::wifi {

/// 一台设备的远程配对记录。
///
/// 为什么要单独存 `host_identifier`：pair-verify 的签名里要放"我们注册时用的那个
/// identifier"，设备拿它和公钥一起认人。它可以是从主机名推出来的（苹果那套实现就是
/// `uuid3(DNS, hostname)` 大写），也可以是设备主动配对时我们另取的——**不能在读的时候
/// 现算**，否则换台机器、改个主机名，记录还在但设备不认。
///
/// 为什么要单独存 `advertised_identifier`：同一台设备在局域网广播里那个 identifier 是
/// 一个不透明 UUID，跟它在 USB 控制面上报的 UDID 不是一回事（实测，见 docs §22.1）。
/// 靠广播认设备要用它，靠 lockdown 认设备要用 UDID，两个都得留。
struct PairRecord {
    std::string udid;
    std::string host_identifier;
    Bytes host_private_key;  ///< Ed25519 种子，32 字节
    Bytes host_public_key;   ///< 32 字节
    std::string advertised_identifier;
    Bytes peer_alt_irk;  ///< 16 字节，可空（老记录里没有）
    std::string remote_unlock_host_key;

    [[nodiscard]] bool complete() const {
        return !udid.empty() && !host_identifier.empty() && host_private_key.size() == 32 &&
               host_public_key.size() == 32;
    }
};

/// 记录的文本形式（`key=value`，二进制字段用 hex）。单独抽出来是为了能在不碰文件
/// 的前提下判"存回去再读出来是不是同一个东西"。
std::string format_record(const PairRecord &record);
std::optional<PairRecord> parse_record(std::string_view text, std::string &err);

/// 落盘/读取。文件权限 0600、目录 0700——这是一把能让对方在设备上打字的手柄。
bool save_record(const std::string &path, const PairRecord &record, std::string &err);
std::optional<PairRecord> load_record(const std::string &path, std::string &err);

/// 默认目录：`$XDG_DATA_HOME/scrctl` 或 `~/.local/share/scrctl`。
std::string default_record_dir();
/// `<dir>/remote-<UDID 末段>.pair`。UDID 里有连字符，路径里统一换成下划线。
std::string record_path(const std::string &dir, const std::string &udid);

/// 目录里已有的记录，按文件名里那段 UDID 返回（已排序；目录不存在=空，不算错误）。
///
/// 为什么需要它：`--wifi <地址>` 手上只有一个 IP，而记录是按 UDID 存的。在做 mDNS
/// 发现（把广播里的 authTag 对回记录的 altIRK）之前，"目录里只有一条就用它"是唯一
/// 不用用户抄 UDID 的办法；多于一条时也要能把候选报出来，而不是报一个空文件名。
/// 返回的串可以直接再喂给 `record_path`（sanitize 是幂等的）。
std::vector<std::string> list_record_udids(const std::string &dir, std::string &err);

}  // namespace scrctl::wifi
