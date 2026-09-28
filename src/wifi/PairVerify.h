#pragma once

#include <optional>
#include <string>

#include "jsonlite/Jsonlite.h"
#include "wifi/Crypto.h"
#include "wifi/PairRecord.h"
#include "wifi/Rppairing.h"

namespace scrctl::wifi {

/// 握手的三种结局。**必须**把"设备答了、说不认识这把钥匙"和"根本没答上"分开：
/// 前者重试多少次都是同一句（要么没配过、要么记录在设备上被删了），后者换条连接
/// 再来一次往往就成了。混成一个 bool 就只能对着日志猜。
enum class VerifyOutcome { Paired, NotPaired, TransportFailure };

struct PairVerifyResult {
    VerifyOutcome outcome = VerifyOutcome::TransportFailure;
    /// X25519 共享密钥：既是主密钥的输入，也是之后 TLS-PSK 隧道的那把 PSK。
    Bytes shared_secret;
    /// 设备握手回信原文（`wireProtocolVersion`、`deviceOptions` 等），给日志和判能力用。
    json::Value device_handshake;
    std::string error;
};

/// 走完 handshake + pair-verify，成功后把两条主密钥装进 `channel`。
///
/// 签名的内容只有三样、顺序固定：`我们的X25519公钥 || host_identifier || 设备的
/// X25519公钥`。少一样、多一样、换个顺序，设备都只回一个不带解释的错误码——这一条
/// 是从"改一个字节就换不回密钥"的现场里量出来的，不是推的。
/// `announce_failure`：设备回 ERROR（不认识这把钥匙）时要不要补一句 `pairVerifyFailed`。
/// 产品路径要补（让设备把会话收干净）；pair-setup 的那一轮探针**不能**补——iOS 27 上
/// 这句事件本身就会把连接掐掉（docs §25.2），而 setup 的 M1 还得在同一条连接上发。
PairVerifyResult pair_verify(Rppairing &channel, const PairRecord &host, std::string &err,
                             bool announce_failure = true);

}  // namespace scrctl::wifi
