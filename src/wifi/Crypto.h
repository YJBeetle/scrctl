#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scrctl::wifi {

using Bytes = std::vector<uint8_t>;

/// `Bytes` → `string_view`。下面这几个 API 的密钥/nonce 一律收 `string_view`，
/// 而 `vector<uint8_t>` 到它是没有隐式转换的——与其在每一处手写 `reinterpret_cast`
/// （写错一次就是一条静默的错密钥），不如把这一道转换集中在这里。
[[nodiscard]] inline std::string_view sv(const Bytes &data) {
    return std::string_view(reinterpret_cast<const char *>(data.data()), data.size());  // NOLINT
}

/// 文本 → 字节（签名缓冲里要拼 identifier 这类字符串，写全一遍太吵）。
[[nodiscard]] inline Bytes bytes_of(std::string_view text) {
    return Bytes(text.begin(), text.end());
}

/// X25519 临时密钥对（pair-verify 每一步都用新的一对，这是协议的向前保密来源）。
struct X25519KeyPair {
    std::array<uint8_t, 32> priv{};
    std::array<uint8_t, 32> pub{};
};

std::optional<X25519KeyPair> x25519_keypair(std::string &err);

/// 共享密钥。任一公钥不是合法的 Curve25519 点时返回 nullopt——对端给的是外部输入，
/// 全零/低阶点必须在这里挡掉，否则后面所有密钥都从一个可预测的值派生。
std::optional<Bytes> x25519_shared(const std::array<uint8_t, 32> &priv, std::string_view peer_pub,
                                   std::string &err);

/// Ed25519 签名（64 字节）。`seed` 是 32 字节的私钥种子，不是 PEM、不是 DER。
std::optional<Bytes> ed25519_sign(std::string_view seed, const Bytes &msg, std::string &err);

/// HKDF-SHA512。`salt` 为空串时按 RFC 5869 用全零盐（与对端实现一致）。
std::optional<Bytes> hkdf_sha512(const Bytes &ikm, std::string_view salt, std::string_view info,
                                 size_t out_len, std::string &err);

/// ChaCha20-Poly1305-IETF（12 字节 nonce + 16 字节标签）。`nonce` 必须正好 12 字节。
std::optional<Bytes> chacha_seal(std::string_view key, std::string_view nonce, const Bytes &plain,
                                 std::string &err);
std::optional<Bytes> chacha_open(std::string_view key, std::string_view nonce, const Bytes &sealed,
                                 std::string &err);

std::string b64_encode(const Bytes &data);
std::string b64_encode(std::string_view data);
std::optional<Bytes> b64_decode(std::string_view text, std::string &err);

}  // namespace scrctl::wifi
