#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "wifi/Crypto.h"

namespace scrctl::wifi {

/// Apple pair-setup 使用的 SRP-6a 客户端（3072 位模数、SHA-512）。
/// 大整数运算和摘要使用 OpenSSL；下面的编码规则是设备协议适配的一部分。
///
/// 已有配对实现及离线向量验证的规则：
///  - 整数进哈希前使用最小大端字节，不添加符号字节；
///    只有 u = H(PAD(A)|PAD(B)) 与 k = H(N|PAD(g)) 用 384 字节定长 PAD。
///  - x = H(s | H(I ":" P))，内层是字节、外层把 salt 原字节拼在前面。
///  - M1 = H( (H(N) xor H(g)) | H(I) | s | A | B | K )，M2 = H(A | M1 | K)。
class SrpClient {
public:
    /// private_hex 仅供测试注入正整数私钥；空则生成随机 1024 位私钥。
    SrpClient(std::string user, std::string password, std::string private_hex = "");

    /// 输入设备 pair-setup M2 的 salt 与 B。B 必须为 1..384 字节且 B % N != 0。
    /// 只有全部计算成功后才发布 A、K、M1、M2；失败时清除已有结果并通过 err 报告。
    bool process(const Bytes &salt, const Bytes &server_public, std::string &err);

    [[nodiscard]] const Bytes &client_public() const { return a_public_; }  // A
    [[nodiscard]] const Bytes &session_key() const { return k_; }           // K
    [[nodiscard]] const Bytes &client_proof() const { return m1_; }         // M1
    /// 计算成功后校验设备的 64 字节证明 M2；未计算或失败后不接受任何证明。
    [[nodiscard]] bool verify_server_proof(const Bytes &m2) const;

private:
    std::string user_;
    std::string password_;
    std::string private_hex_;
    Bytes a_public_;
    Bytes k_;
    Bytes m1_;
    Bytes m2_;
};

}  // namespace scrctl::wifi
