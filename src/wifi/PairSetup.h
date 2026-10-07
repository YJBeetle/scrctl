#pragma once

#include <string>
#include <string_view>

#include "json/Json.h"
#include "wifi/PairRecord.h"
#include "wifi/Rppairing.h"

namespace scrctl::wifi {

/// 由主机名生成 DNS 命名空间的 UUID v3，输出大写文本；MD5 不可用时返回空串。
/// pair-setup 的标识字段、M5 签名和后续 pair-verify 签名应使用同一字符串。
/// 此函数提供确定性生成方式；读取已有记录时应使用保存的标识，不重新生成。
std::string host_identifier_uuid3(std::string_view hostname);

/// 获取本机主机名，失败时返回空串；调用方需报错或提供稳定的替代标识。
std::string local_hostname();

struct PairSetupResult {
    bool ok = false;
    /// 成功时生成的记录；保存由调用方负责，仍需核对传入的设备标识。
    PairRecord record;
    /// 设备 handshake 响应体，包含 peerDeviceInfo 等信息，可供调用方查询协商能力。
    json::Value device_handshake;
    std::string error;
};

/// pair-setup 的入口参数。流程包含可选 verify 探测、M1..M6 及远程解锁密钥请求。
/// 当前 SRP 用户名为 Pair-Setup、PIN 固定为 "000000"，不提供交互输入其他 PIN 的接口。
/// 已有设备记录显示不同控制面可能有不同确认流程；progress 用于报告中间状态。
/// 接入其他设备配置时需重新验证，不能假定所有 iOS/tvOS 都采用相同配对方式。
struct PairSetupOptions {
    /// 是否先发送 attemptPairVerify=true 的 handshake 并执行 verify 探测；false
    /// 表示直接以 attemptPairVerify=false 开始 setup。已测 iOS 27 字节流入口存在
    /// verify 后切换 setup 被关闭连接的情况（docs §25.2/§25.3），开关保留两种入口。
    bool probe_verify_first = true;
    /// pairingData 的 kind，默认升级已有 lockdown 信任到远程配对。
    /// 已测 iOS 27/已建立 USB 信任的配置接受 upgradeNonAutomationLockdownPairing，
    /// setupManualPairing 在所测入口被关闭连接（docs §25.6）；不将该样本外推为所有
    /// 设备的唯一选择。调用方可显式指定 kind，但设备是否允许仍由握手和响应决定。
    std::string pairing_kind = "upgradeNonAutomationLockdownPairing";
};

/// 在已连接的控制面执行配对，不负责建立连接或写入记录。成功时安装双向主密钥，
/// 可选 createRemoteUnlockKey 失败不改变 ok。失败时 ok=false，error 与 err 记录原因；
/// 已执行的设备端配对步骤不会由本函数回滚，channel 也不保证恢复到初始状态。
/// 已有设备在 setup 后关闭连接，后续起隧道需重连并 verify（验证范围见 docs §25）。
/// 当前实现检查 SRP 服务端证明；M6 携带密文时验证并解密，但不要求存在密文，
/// 也未校验其中设备的长期签名。
PairSetupResult pair_setup(Rppairing &channel, std::string_view host_identifier,
                           std::string_view hostname, std::string_view udid,
                           const ProgressFn &progress, const PairSetupOptions &options,
                           std::string &err);

}  // namespace scrctl::wifi
