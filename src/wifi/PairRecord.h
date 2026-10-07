#pragma once

#include <string>
#include <vector>

#include "wifi/Crypto.h"

namespace scrctl::wifi {

/// 一台设备的远程配对记录。
///
/// host_identifier 保存注册时的主机标识，pair-verify 将同一字符串纳入签名。
/// 主机名变化或迁移记录时应继续使用记录中的值，不能根据当前主机名重新生成。
/// advertised_identifier 保存设备握手中的广播标识；已有样本中它与 USB 的 UDID
/// 不同（docs §22.1）。局域网发现与 lockdown 连接使用不同标识，不能相互替代。
/// 记录包含主机私钥及配对相关密钥，保存位置和访问权限应按敏感数据管理。
struct PairRecord {
    std::string udid;
    std::string host_identifier;
    Bytes host_private_key;  ///< Ed25519 种子，32 字节
    Bytes host_public_key;   ///< Ed25519 公钥，32 字节
    std::string advertised_identifier;
    Bytes peer_alt_irk;  ///< 设备的身份解析密钥，16 字节；未提供时可空，兼容旧记录
    std::string remote_unlock_host_key;

    /// 检查 verify 所需标识和密钥长度，不验证公私钥是否匹配，也不检查设备端信任状态。
    [[nodiscard]] bool complete() const {
        return !udid.empty() && !host_identifier.empty() && host_private_key.size() == 32 &&
               host_public_key.size() == 32;
    }
};

/// 版本 1 文本格式：首个非空行为固定头，后续为 key=value，二进制字段使用十六进制。
/// parse_record 检查已知二进制字段长度并要求私钥存在，忽略未知键；成功解析不等于
/// complete()。调用方仍需检查用途所需的字段。format_record 不校验输入记录。
std::string format_record(const PairRecord &record);
std::optional<PairRecord> parse_record(std::string_view text, std::string &err);

/// 同目录临时文件写入后替换目标。POSIX 文件在写入内容前设为 0600，新建叶子目录
/// 设为 0700；Windows 为文件和新建叶子目录设置当前进程用户 SID 的受保护 DACL。
/// 已有目录和上级目录权限不在此调整。失败返回 false 并填 err，尽量移除临时文件。
bool save_record(const std::string &path, const PairRecord &record, std::string &err);
/// 读取文本后检查 16 KiB 上限，再解析记录；此上限不是读取前的内存分配限制。
std::optional<PairRecord> load_record(const std::string &path, std::string &err);

/// 非空 XDG_DATA_HOME 优先，目录为其下的 scrctl；否则 Windows 尝试
/// LOCALAPPDATA/scrctl。其余情况使用 HOME/.local/share/scrctl，HOME 未设置时
/// 以当前目录为根（./.local/share/scrctl）。这里只生成路径，不创建目录。
std::string default_record_dir();
/// <dir>/remote-<完整 UDID 的净化结果>.pair：仅保留 ASCII 字母和数字，其余字符
/// 均替换为下划线，避免 UDID 引入路径分隔符；不同原始字符串可能得到同一文件名。
std::string record_path(const std::string &dir, const std::string &udid);

/// 从目录的普通文件名中提取 remote- 与 .pair 之间的候选标识并排序，不解析文件内容。
/// 返回值已净化，可再次传给 record_path；不能据此还原原始 UDID 的标点。
/// 目录未被识别为目录时返回空列表并清除 err，包括不存在或目录查询失败的情况。
/// 枚举过程中检测到的错误通过 err 返回；列表可用于 Wi-Fi 连接选择本地记录。
std::vector<std::string> list_record_udids(const std::string &dir, std::string &err);

}  // namespace scrctl::wifi
