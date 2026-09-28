#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "wifi/Crypto.h"

namespace scrctl::wifi {

/// SRP-6a（3072 位模数、SHA-512）客户端，公式与参考实现逐条对齐——pair-setup 的
/// M1/M3 两轮要和设备手里的服务端实现互认，差一个填充字节就握手失败。
///
/// 对齐点（都是量/读出来的，不是 SRP 论文的默认写法）：
///  - 整数进哈希前是**最小大端字节、hex 奇数位时补一个前导 0**（不是定长填充）；
///    只有 u = H(PAD(A)|PAD(B)) 与 k = H(N|PAD(g)) 用 384 字节定长 PAD。
///  - x = H(s | H(I ":" P))，内层是字节、外层把 salt 原字节拼在前面。
///  - M1 = H( (H(N) xor H(g)) | H(I) | s | A | B | K )，M2 = H(A | M1 | K)。
class SrpClient {
public:
    /// `private_hex` 非空时用它当私钥 a（离线自检注入 oracle 用）；空则随机 1024 位。
    SrpClient(std::string user, std::string password, std::string private_hex = "");

    /// 喂入设备 M2 里的 salt 与 B。B % N == 0 或 B 越界返回 false。
    bool process(const Bytes &salt, const Bytes &server_public, std::string &err);

    [[nodiscard]] const Bytes &client_public() const { return a_public_; }  // A
    [[nodiscard]] const Bytes &session_key() const { return k_; }           // K
    [[nodiscard]] const Bytes &client_proof() const { return m1_; }         // M1
    /// 校验设备回的 proof（M2）。
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
