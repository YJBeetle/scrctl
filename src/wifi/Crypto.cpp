#include "wifi/Crypto.h"
#include "util/Base64.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cstring>
#include <memory>

namespace scrctl::wifi {
namespace {

struct EvpPkeyDeleter {
    void operator()(EVP_PKEY *p) const { EVP_PKEY_free(p); }
};
struct EvpCtxDeleter {
    void operator()(EVP_PKEY_CTX *p) const { EVP_PKEY_CTX_free(p); }
};
struct EvpMdCtxDeleter {
    void operator()(EVP_MD_CTX *p) const { EVP_MD_CTX_free(p); }
};
struct CipherCtxDeleter {
    void operator()(EVP_CIPHER_CTX *p) const { EVP_CIPHER_CTX_free(p); }
};
using PkeyUp = std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>;
using PctxUp = std::unique_ptr<EVP_PKEY_CTX, EvpCtxDeleter>;
using MdCtxUp = std::unique_ptr<EVP_MD_CTX, EvpMdCtxDeleter>;
using CtxUp = std::unique_ptr<EVP_CIPHER_CTX, CipherCtxDeleter>;

const unsigned char *u8(std::string_view s) {
    return reinterpret_cast<const unsigned char *>(s.data());  // NOLINT: 只读视图转字节
}

/// 裸密钥（32 字节，非 PEM/DER）转 EVP_PKEY。`as_private=false` 时按公钥构造。
PkeyUp raw_key(int pkey_type, std::string_view data, bool as_private, std::string &err) {
    PkeyUp p(as_private ? EVP_PKEY_new_raw_private_key(pkey_type, nullptr, u8(data),
                                                       static_cast<int>(data.size()))
                        : EVP_PKEY_new_raw_public_key(pkey_type, nullptr, u8(data),
                                                      static_cast<int>(data.size())));
    if (!p) {
        err = as_private ? "构造私钥失败（长度或平台不支持）" : "构造公钥失败（长度或平台不支持）";
    }
    return p;
}

bool hmac_sha512(const Bytes &key, const Bytes &data, Bytes &out, std::string &err) {
    out.assign(EVP_MD_size(EVP_sha512()), 0);
    unsigned int len = 0;
    if (HMAC(EVP_sha512(), key.data(), static_cast<int>(key.size()), data.data(), data.size(),
            out.data(), &len) == nullptr) {
        err = "HMAC-SHA512 失败";
        return false;
    }
    out.resize(len);
    return true;
}

/// ChaCha20-Poly1305（12 字节 nonce，16 字节标签附在密文尾部）。
///
/// 两个方向最容易写反的地方都在这里：解密时喂给 CipherUpdate 的必须**不含标签**，
/// 而 SET_TAG 要在 CipherFinal 之前——标签正是在 final 那一步校验的，所以
/// "密钥对不上"和"帧错位"都表现为 final 失败，错误文案要分得开。
bool chacha(bool encrypt, std::string_view key, std::string_view nonce, const Bytes &in, Bytes &out,
            std::string &err) {
    static constexpr size_t kTagLen = 16;
    if (key.size() != 32 || nonce.size() != 12) {
        err = "ChaCha20-Poly1305 要 32 字节密钥 + 12 字节 nonce";
        return false;
    }
    if (!encrypt && in.size() < kTagLen) {
        err = "密文比 Poly1305 标签还短";
        return false;
    }
    CtxUp ctx(EVP_CIPHER_CTX_new());
    if (!ctx) {
        err = "分配 EVP_CIPHER_CTX 失败";
        return false;
    }
    if (EVP_CipherInit_ex(ctx.get(), EVP_chacha20_poly1305(), nullptr, nullptr, nullptr,
                          encrypt ? 1 : 0) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_SET_IVLEN, static_cast<int>(nonce.size()),
                            nullptr) != 1 ||
        EVP_CipherInit_ex(ctx.get(), nullptr, nullptr, u8(key), u8(nonce), encrypt ? 1 : 0) != 1) {
        err = "ChaCha20-Poly1305 初始化失败（这个 OpenSSL 可能没编 CHACHA）";
        return false;
    }
    const size_t body = encrypt ? in.size() : in.size() - kTagLen;
    out.assign(body + kTagLen, 0);
    int produced = 0;
    if (body > 0 &&
        EVP_CipherUpdate(ctx.get(), out.data(), &produced, in.data(),
                         static_cast<int>(body)) != 1) {
        err = "ChaCha20-Poly1305 更新失败";
        return false;
    }
    int tail = 0;
    if (!encrypt) {
        if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_SET_TAG, static_cast<int>(kTagLen),
                                const_cast<unsigned char *>(in.data() + body)) != 1) {  // NOLINT
            err = "设 Poly1305 标签失败";
            return false;
        }
        if (EVP_CipherFinal_ex(ctx.get(), out.data() + produced, &tail) != 1) {
            err = "Poly1305 标签校验失败：要么密钥不对，要么帧取错了字节";
            return false;
        }
        out.resize(static_cast<size_t>(produced) + tail);
        return true;
    }
    if (EVP_CipherFinal_ex(ctx.get(), out.data() + produced, &tail) != 1) {
        err = "ChaCha20-Poly1305 收尾失败";
        return false;
    }
    produced += tail;
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_GET_TAG, static_cast<int>(kTagLen),
                            out.data() + produced) != 1) {
        err = "取 Poly1305 标签失败";
        return false;
    }
    out.resize(static_cast<size_t>(produced) + kTagLen);
    return true;
}

}  // namespace

std::optional<X25519KeyPair> x25519_keypair(std::string &err) {
    PctxUp pctx(EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr));
    if (!pctx || EVP_PKEY_keygen_init(pctx.get()) <= 0) {
        err = "初始化 X25519 密钥生成失败";
        return std::nullopt;
    }
    EVP_PKEY *raw = nullptr;
    if (EVP_PKEY_keygen(pctx.get(), &raw) <= 0) {
        err = "生成 X25519 密钥失败";
        return std::nullopt;
    }
    PkeyUp pkey(raw);
    X25519KeyPair out;
    size_t len = out.pub.size();
    if (EVP_PKEY_get_raw_public_key(pkey.get(), out.pub.data(), &len) != 1 ||
        len != out.pub.size()) {
        err = "取 X25519 公钥失败";
        return std::nullopt;
    }
    len = out.priv.size();
    if (EVP_PKEY_get_raw_private_key(pkey.get(), out.priv.data(), &len) != 1 ||
        len != out.priv.size()) {
        err = "取 X25519 私钥失败";
        return std::nullopt;
    }
    return out;
}

std::optional<Bytes> random_bytes(size_t n, std::string &err) {
    Bytes out(n);
    if (n > 0 && RAND_bytes(out.data(), static_cast<int>(n)) != 1) {
        err = "取随机字节失败（CSPRNG 没播种？）";
        return std::nullopt;
    }
    return out;
}

std::optional<Ed25519KeyPair> ed25519_keypair(std::string &err) {
    const std::optional<Bytes> seed = random_bytes(32, err);
    if (!seed) {
        return std::nullopt;
    }
    const std::string_view seed_view(sv(*seed));
    PkeyUp key = raw_key(EVP_PKEY_ED25519, seed_view, true, err);
    if (!key) {
        return std::nullopt;
    }
    Ed25519KeyPair out;
    std::memcpy(out.seed.data(), seed->data(), out.seed.size());
    size_t len = out.pub.size();
    if (EVP_PKEY_get_raw_public_key(key.get(), out.pub.data(), &len) != 1 || len != out.pub.size()) {
        err = "取 Ed25519 公钥失败";
        return std::nullopt;
    }
    return out;
}

std::optional<Bytes> x25519_shared(const std::array<uint8_t, 32> &priv, std::string_view peer_pub,
                                   std::string &err) {    if (peer_pub.size() != 32) {
        err = "对端 X25519 公钥长度不是 32";
        return std::nullopt;
    }
    const std::string_view priv_view(reinterpret_cast<const char *>(priv.data()),  // NOLINT
                                     priv.size());
    PkeyUp ours = raw_key(EVP_PKEY_X25519, priv_view, true, err);
    if (!ours) {
        return std::nullopt;
    }
    PkeyUp theirs = raw_key(EVP_PKEY_X25519, peer_pub, false, err);
    if (!theirs) {
        return std::nullopt;
    }
    PctxUp dctx(EVP_PKEY_CTX_new(ours.get(), nullptr));
    size_t len = 0;
    if (!dctx || EVP_PKEY_derive_init(dctx.get()) <= 0 ||
        EVP_PKEY_derive_set_peer(dctx.get(), theirs.get()) <= 0 ||
        EVP_PKEY_derive(dctx.get(), nullptr, &len) <= 0 || len != 32) {
        err = "X25519 共享密钥计算失败";
        return std::nullopt;
    }
    Bytes out(len);
    if (EVP_PKEY_derive(dctx.get(), out.data(), &len) <= 0) {
        err = "X25519 共享密钥计算失败";
        return std::nullopt;
    }
    // RFC 7748 §6.1：全零意味着对端是低阶点。不挡掉的话，后面所有密钥都从一个
    // 攻击者可预测的值派生——这就是"看着握上了手"的空会话。
    if (std::all_of(out.begin(), out.end(), [](uint8_t b) { return b == 0; })) {
        err = "X25519 共享密钥全零（对端公钥是低阶点）";
        return std::nullopt;
    }
    return out;
}

std::optional<Bytes> ed25519_sign(std::string_view seed, const Bytes &msg, std::string &err) {
    if (seed.size() != 32) {
        err = "Ed25519 私钥种子长度不是 32";
        return std::nullopt;
    }
    PkeyUp key = raw_key(EVP_PKEY_ED25519, seed, true, err);
    if (!key) {
        return std::nullopt;
    }
    MdCtxUp mdctx(EVP_MD_CTX_new());
    if (!mdctx || EVP_DigestSignInit(mdctx.get(), nullptr, nullptr, nullptr, key.get()) <= 0) {
        err = "初始化 Ed25519 签名失败";
        return std::nullopt;
    }
    size_t len = 0;
    if (EVP_DigestSign(mdctx.get(), nullptr, &len, msg.data(), msg.size()) <= 0 || len != 64) {
        err = "取 Ed25519 签名长度失败";
        return std::nullopt;
    }
    Bytes out(len);
    if (EVP_DigestSign(mdctx.get(), out.data(), &len, msg.data(), msg.size()) <= 0) {
        err = "Ed25519 签名失败";
        return std::nullopt;
    }
    return out;
}

std::optional<Bytes> hkdf_sha512(const Bytes &ikm, std::string_view salt, std::string_view info,
                                 size_t out_len, std::string &err) {
    // 手写 extract+expand 而不是 EVP_KDF：后者要 OpenSSL 3，而这条路径还要在
    // 老版本 OpenSSL 的发行版上编得出来。HMAC 一次调用哪版都有。
    static constexpr size_t kHashLen = 64;
    Bytes salt_bytes = salt.empty() ? Bytes(kHashLen, 0) : Bytes(salt.begin(), salt.end());
    Bytes prk;
    if (!hmac_sha512(salt_bytes, ikm, prk, err)) {
        return std::nullopt;
    }
    Bytes info_bytes(info.begin(), info.end());
    Bytes out, t;
    uint8_t counter = 1;
    while (out.size() < out_len) {
        Bytes input = t;
        input.insert(input.end(), info_bytes.begin(), info_bytes.end());
        input.push_back(counter++);
        if (!hmac_sha512(prk, input, t, err)) {
            return std::nullopt;
        }
        out.insert(out.end(), t.begin(), t.end());
    }
    out.resize(out_len);
    return out;
}

std::optional<Bytes> chacha_seal(std::string_view key, std::string_view nonce, const Bytes &plain,
                                 std::string &err) {
    Bytes out;
    if (!chacha(true, key, nonce, plain, out, err)) {
        return std::nullopt;
    }
    return out;
}

std::optional<Bytes> chacha_open(std::string_view key, std::string_view nonce, const Bytes &sealed,
                                 std::string &err) {
    Bytes out;
    if (!chacha(false, key, nonce, sealed, out, err)) {
        return std::nullopt;
    }
    return out;
}

std::string b64_encode(std::string_view data) { return util::base64_encode(data); }
std::string b64_encode(const Bytes &data) { return util::base64_encode(data); }
std::optional<Bytes> b64_decode(std::string_view text, std::string &err) {
    return util::base64_decode(text, err);
}

}  // namespace scrctl::wifi
