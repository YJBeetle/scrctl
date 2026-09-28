#include "wifi/Srp.h"

#include <openssl/bn.h>
#include <openssl/sha.h>

#include <cstring>

namespace scrctl::wifi {
namespace {

/// RFC 5054 的 3072 位模数，与参考实现用的同一份常量。
constexpr const char *kPrimeHex =
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74"
    "020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F1437"
    "4FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
    "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163BF05"
    "98DA48361C55D39A69163FA8FD24CF5F83655D23DCA3AD961C62F356208552BB"
    "9ED529077096966D670C354E4ABC9804F1746C08CA18217C32905E462E36CE3B"
    "E39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF695581718"
    "3995497CEA956AE515D2261898FA051015728E5A8AAAC42DAD33170D04507A33"
    "A85521ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7"
    "ABF5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6BF12FFA06D98A0864"
    "D87602733EC86A64521F2B18177B200CBBE117577A615D6C770988C0BAD946E2"
    "08E24FA074E5AB3143DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF";
constexpr int kGenerator = 5;

struct Bn {
    BIGNUM *v = nullptr;
    Bn() { v = BN_new(); }
    ~Bn() { BN_free(v); }
    Bn(const Bn &) = delete;
    Bn &operator=(const Bn &) = delete;
};

Bytes sha512(const std::vector<Bytes> &parts) {
    SHA512_CTX ctx;
    SHA512_Init(&ctx);
    for (const auto &p : parts) {
        SHA512_Update(&ctx, p.data(), p.size());
    }
    Bytes out(SHA512_DIGEST_LENGTH);
    SHA512_Final(out.data(), &ctx);
    return out;
}

/// 参考实现的 int_to_bytes：hex 串奇数位补 0 再 unhexlify——落到字节数组上与**最小大端**
/// 完全等价（最小大端的首字节不可能是 0），所以就是 BN_bn2bin。第一版在这里多补了一个
/// 前导 0 字节，K 对而 M1 不对，就是它。
Bytes bytes_of_bn(const BIGNUM *n) {
    const int len = BN_num_bytes(n);
    Bytes out(static_cast<size_t>(len));
    BN_bn2bin(n, out.data());
    return out;
}

/// 384 字节定长大端（参考实现的 pad()）。
Bytes pad_bn(const BIGNUM *n, int width) {
    Bytes out(static_cast<size_t>(width), 0);
    BN_bn2binpad(n, out.data(), width);
    return out;
}

void bn_from_hex(const char *hex, BIGNUM *dst) { BN_hex2bn(&dst, hex); }

Bytes sha512_int(const BIGNUM *n) { return sha512({bytes_of_bn(n)}); }

/// 两个哈希值（当作整数）的异或，再取最小大端字节——参考实现里是 Python 整数 xor，
/// 所以前导零要去掉、奇数位 hex 还要补回一个前导 0（与 bytes_of_bn 同一套规矩）。
Bytes xor_hashes(const Bytes &a, const Bytes &b) {
    Bytes r(std::max(a.size(), b.size()), 0);
    for (size_t i = 0; i < a.size(); ++i) {
        r[r.size() - a.size() + i] ^= a[i];
    }
    for (size_t i = 0; i < b.size(); ++i) {
        r[r.size() - b.size() + i] ^= b[i];
    }
    size_t lead = 0;
    while (lead + 1 < r.size() && r[lead] == 0) {
        ++lead;
    }
    r.erase(r.begin(), r.begin() + static_cast<long>(lead));
    return r;
}

}  // namespace

SrpClient::SrpClient(std::string user, std::string password, std::string private_hex)
    : user_(std::move(user)), password_(std::move(password)), private_hex_(std::move(private_hex)) {}

bool SrpClient::process(const Bytes &salt, const Bytes &server_public, std::string &err) {
    Bn N;
    bn_from_hex(kPrimeHex, N.v);
    const int width = BN_num_bytes(N.v);
    Bn g;
    BN_set_word(g.v, kGenerator);

    // k = H(N | PAD(g))
    const Bytes k = sha512({bytes_of_bn(N.v), pad_bn(g.v, width)});
    Bn kb;
    BN_bin2bn(k.data(), static_cast<int>(k.size()), kb.v);

    // a：私钥。离线自检要能注入，真机跑要真随机。
    Bn a;
    if (!private_hex_.empty()) {
        bn_from_hex(private_hex_.c_str(), a.v);
    } else {
        BN_rand(a.v, 1024, -1, 0);
    }
    // A = g^a mod N
    Bn A;
    {
        BN_CTX *ctx = BN_CTX_new();
        BN_mod_exp(A.v, g.v, a.v, N.v, ctx);
        BN_CTX_free(ctx);
    }
    a_public_ = bytes_of_bn(A.v);

    Bn B;
    BN_bin2bn(server_public.data(), static_cast<int>(server_public.size()), B.v);
    {
        // BN_mod 要真 ctx，传空指针直接崩（第一版就崩在这里）。
        BN_CTX *ctx = BN_CTX_new();
        Bn mod;
        BN_mod(mod.v, B.v, N.v, ctx);
        BN_CTX_free(ctx);
        if (BN_is_zero(mod.v)) {
            err = "SRP 的 B 是 N 的倍数，拒收";
            return false;
        }
    }

    // x = H(s | H(I ":" P))
    const Bytes inner = sha512({bytes_of(user_), Bytes { ':' }, bytes_of(password_)});
    Bytes outer_input = salt;
    outer_input.insert(outer_input.end(), inner.begin(), inner.end());
    const Bytes x = sha512({outer_input});
    Bn xb;
    BN_bin2bn(x.data(), static_cast<int>(x.size()), xb.v);

    // u = H(PAD(A) | PAD(B))
    const Bytes u = sha512({pad_bn(A.v, width), pad_bn(B.v, width)});
    Bn ub;
    BN_bin2bn(u.data(), static_cast<int>(u.size()), ub.v);

    // v = g^x mod N
    Bn v;
    {
        BN_CTX *ctx = BN_CTX_new();
        BN_mod_exp(v.v, g.v, xb.v, N.v, ctx);
        BN_CTX_free(ctx);
    }

    // S = (B - k*v)^(a + u*x) mod N
    Bn S;
    {
        BN_CTX *ctx = BN_CTX_new();
        Bn kv;
        BN_mod_mul(kv.v, kb.v, v.v, N.v, ctx);
        Bn base;
        BN_mod_sub(base.v, B.v, kv.v, N.v, ctx);
        Bn ux;
        BN_mul(ux.v, ub.v, xb.v, ctx);
        Bn exp;
        BN_add(exp.v, a.v, ux.v);
        BN_mod_exp(S.v, base.v, exp.v, N.v, ctx);
        BN_CTX_free(ctx);
    }

    // K = H(S)
    k_ = sha512({bytes_of_bn(S.v)});

    // M1 = H( (H(N) xor H(g)) | H(I) | s | A | B | K )
    Bn hn;
    BN_bin2bn(sha512_int(N.v).data(), SHA512_DIGEST_LENGTH, hn.v);
    Bn hg;
    BN_bin2bn(sha512_int(g.v).data(), SHA512_DIGEST_LENGTH, hg.v);
    const Bytes hxor = xor_hashes(sha512_int(N.v), sha512_int(g.v));
    const Bytes huser = sha512({bytes_of(user_)});
    m1_ = sha512({hxor, huser, salt, bytes_of_bn(A.v), bytes_of_bn(B.v), k_});

    // M2 = H(A | M1 | K)
    m2_ = sha512({bytes_of_bn(A.v), m1_, k_});
    return true;
}

bool SrpClient::verify_server_proof(const Bytes &m2) const { return m2 == m2_; }

}  // namespace scrctl::wifi
