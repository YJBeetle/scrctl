#pragma once

#include <optional>
#include <string>

#include "json/Json.h"
#include "wifi/Crypto.h"
#include "wifi/PairRecord.h"
#include "wifi/Rppairing.h"

namespace scrctl::wifi {

/// Paired 表示流程完成并安装主密钥；NotPaired 表示本地记录不完整或设备返回
/// 配对错误。TransportFailure 是其余失败路径的默认值，也包括消息格式及密码学
/// 操作失败，不能仅凭此值判断网络故障或保证重试会成功。
enum class VerifyOutcome { Paired, NotPaired, TransportFailure };

struct PairVerifyResult {
    VerifyOutcome outcome = VerifyOutcome::TransportFailure;
    /// X25519 共享秘密，用于主密钥派生及后续 TLS-PSK 隧道；应按敏感数据处理。
    /// 它可在后续阶段失败前已填入，调用方必须先检查 outcome，再使用结果。
    Bytes shared_secret;
    /// 设备 handshake 响应体，包含 wireProtocolVersion、deviceOptions 等信息。
    json::Value device_handshake;
    std::string error;
};

/// 在已连接的控制面执行 handshake + pair-verify，成功后安装双向主密钥。
/// 主机 Ed25519 签名输入按固定顺序拼接：本次主机 X25519 公钥、记录中的
/// host_identifier、设备 X25519 公钥；标识必须与 setup 注册时一致。
/// 当前实现不解密或校验 PV-Msg02 的设备身份材料，不能据 Paired 推断已独立验证
/// 设备长期身份。失败时也不保证 channel 的状态可直接用于下一次握手。
/// announce_failure 控制设备返回 Error TLV 后是否尽力发送 pairVerifyFailed；
/// 发送失败不覆盖原配对错误。产品默认开启，实验入口可关闭，以研究同连接后续流程。
/// 已测 iOS 27 中该事件可能伴随连接关闭，具体入口限制见 docs §25.2/§25.6。
PairVerifyResult pair_verify(Rppairing &channel, const PairRecord &host, std::string &err,
                             bool announce_failure = true);

}  // namespace scrctl::wifi
