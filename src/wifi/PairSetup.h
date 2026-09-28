#pragma once

#include <string>
#include <string_view>

#include "jsonlite/Jsonlite.h"
#include "wifi/PairRecord.h"
#include "wifi/Rppairing.h"

namespace scrctl::wifi {

/// 苹果那套实现的主机标识：`uuid3(DNS, hostname)` 的大写文本形式。
///
/// 为什么非得是这一个算法：pair-setup 注册时发出去的 identifier、M5 签名缓冲里放的
/// identifier、以及之后每次 pair-verify 签名里放的 identifier 必须是**同一个字符串**，
/// 而设备那边把它当不透明串存着。跟着苹果现算的规矩走，好处是同一台主机上任何工具
/// （Xcode、参考实现、我们）算出来的都是它，不会因为"各自随机生成一个"而互相顶掉。
std::string host_identifier_uuid3(std::string_view hostname);

/// 本机主机名。取不到时返回空串（调用方要么报错，要么自己给一个稳定值）。
std::string local_hostname();

struct PairSetupResult {
    bool ok = false;
    /// 成功时填好、可直接 `save_record` 的记录。
    PairRecord record;
    /// 设备 handshake 回信原文（`peerDeviceInfo` 等），给日志和判能力用。
    json::Value device_handshake;
    std::string error;
};

/// 在一条已连上的 RPPairing 控制面上走完整的 pair-setup：M1..M6 + createRemoteUnlockKey。
///
/// 成功时两条主密钥已经装进 `channel`（当场就能接着 createListener），但**设备会在
/// 配对结束后关掉这条连接**（实测），所以真要起隧道得重连一次。
///
/// PIN 一律按 `"000000"`：iOS 上手动配对不核对 PIN，身份是靠"我们签的东西设备验得
/// 过、设备签的东西我们验得过"这两条建立的（tvOS 才要人输 PIN，这里不支持）。
///
/// `progress` 用来把"设备在等你在屏幕上点信任"这类中间状态说给人听。Wi-Fi 那条面会
/// 弹这个框；USB lockdown 那条实测不弹（它跑在已信任的 lockdownd 之上）。
struct PairSetupOptions {
    /// 要不要先按 verify 的路数问一轮"认不认识我"（handshake 里报 attemptPairVerify=true）。
    /// 参考实现是这么做的；但 iOS 27 上 verify 会话一旦开起来就不肯在同一条连接上换到
    /// setup（docs §25.2/§25.3），所以这条路留作开关，另一档是 handshake 直接报
    /// attemptPairVerify=false、然后发 setup 的 M1。
    bool probe_verify_first = true;
};

PairSetupResult pair_setup(Rppairing &channel, std::string_view host_identifier,
                           std::string_view hostname, std::string_view udid,
                           const ProgressFn &progress, const PairSetupOptions &options,
                           std::string &err);

}  // namespace scrctl::wifi
