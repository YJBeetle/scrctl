// M5 离线判据：RemotePairing 的密码学、TLV、配对记录、帧与序号规矩。
//
// 期望值的来源分两级，逐条标出来：
//   [RFC]   = 规范里的测试向量；
//   [对拍]  = 由参考实现（python cryptography，Rust + OpenSSL 后端）生成，不是手推的。
// 最终说了算的判据还是真机那一趟（`tools/wifi_probe`）；这里只保证不把明显错的东西
// 发到设备上。
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "remote/PairingChannel.h"
#include "wifi/Crypto.h"
#include "wifi/Opack.h"
#include "wifi/Srp.h"
#include "wifi/PairRecord.h"
#include "wifi/PairSetup.h"
#include "wifi/PairVerify.h"
#include "wifi/Rppairing.h"
#include "wifi/Tlv.h"

namespace {

using scrctl::wifi::Bytes;
using scrctl::wifi::j_bool;
using scrctl::wifi::j_int;
using scrctl::wifi::j_obj;
using scrctl::wifi::j_str;

int failures = 0;
int checks = 0;

void check(bool ok, const char *what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}

Bytes from_hex(std::string_view text) {
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        return (c >= 'a' ? c - 'a' : c - 'A') + 10;
    };
    Bytes out;
    for (size_t i = 0; i + 1 < text.size(); i += 2) {
        out.push_back(static_cast<uint8_t>((val(text[i]) << 4) | val(text[i + 1])));
    }
    return out;
}

std::string to_hex(const Bytes &data) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    for (const uint8_t b : data) {
        out += kDigits[b >> 4];
        out += kDigits[b & 0xF];
    }
    return out;
}

std::string_view bv(const Bytes &data) {
    return scrctl::wifi::sv(data);
}

Bytes sb(std::string_view text) {
    return scrctl::wifi::bytes_of(text);
}

/// ---- 1. 密码学 ----
void test_crypto() {
    std::string err;

    const Bytes ikm(22, 0x0b);
    Bytes salt;
    for (int i = 0; i < 16; ++i) {
        salt.push_back(static_cast<uint8_t>(0xf0 + i));
    }
    const Bytes info = from_hex("f3f4f1f5f7f6f2f3f8f9");
    const std::optional<Bytes> okm = scrctl::wifi::hkdf_sha512(ikm, bv(salt), bv(info), 42, err);
    check(okm.has_value() &&
              to_hex(*okm) ==
                  "69fe83b9386d5a7cfdb29ef5daa0a34bb07941f75bb65df65aefb6942c1991950c21fd71ab1f5"
                  "435b440",
          "HKDF-SHA512 出 42 字节 [对拍]");

    Bytes ikm2;
    for (int i = 0; i < 32; ++i) {
        ikm2.push_back(static_cast<uint8_t>(i));
    }
    const std::optional<Bytes> empty_salt =
        scrctl::wifi::hkdf_sha512(ikm2, "", "ClientEncrypt-main", 16, err);
    check(empty_salt.has_value() && to_hex(*empty_salt) == "9f8aa265911271e6d6e90daf564d82b5",
          "HKDF 的空 salt 等于全零盐（主密钥那条路）[对拍]");

    const Bytes pt = sb("Ladies and Gentlemen of the class of '99: If I could offer you only one "
                        "tip for the future, sunscreen would be it.");
    // nonce 以 NUL 开头，所以**不能**用 `string_view("…")`——那会按 strlen 截成零长。
    static constexpr char kNonceBytes[] = "\x00\x00\x00\x00\x00\x00\x00\x4a\x00\x00\x00\x00";
    const std::string_view kNonce(kNonceBytes, sizeof(kNonceBytes) - 1);
    const std::optional<Bytes> sealed = scrctl::wifi::chacha_seal(bv(ikm2), kNonce, pt, err);
    check(sealed.has_value() &&
              to_hex(*sealed) ==
                  "6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0bf91b65c552473"
                  "3ab8f593dabcd62b3571639d624e65152ab8f530c359f0861d807ca0dbf500d6a6156a38e088a2"
                  "2b65e52bc514d16ccf806818ce91ab77937365af90bbf74a35be6b40b8eedf2785e42874d81db6"
                  "3fcb189a03121ae0ac72a3f1f36",
          "ChaCha20-Poly1305 密文+标签全量 [对拍]");

    std::string open_err;
    const std::optional<Bytes> opened = scrctl::wifi::chacha_open(bv(ikm2), kNonce, *sealed, open_err);
    check(opened.has_value() && *opened == pt, "ChaCha20-Poly1305 往回解");

    Bytes tampered = *sealed;
    tampered[10] ^= 0x01;
    std::string tamper_err;
    check(!scrctl::wifi::chacha_open(bv(ikm2), kNonce, tampered, tamper_err), "改一个字节必须解不开");
    check(tamper_err.find("标签") != std::string::npos,
          "标签校验失败要说明是校验不过，不能只丢一句「解密失败」");

    const Bytes empty_ct = from_hex("e9dfc72a53d4cec416165db4717cb0c2");
    const std::optional<Bytes> empty_open = scrctl::wifi::chacha_open(bv(ikm2), kNonce, empty_ct, err);
    check(empty_open.has_value() && empty_open->empty(), "空明文（0 长 + 16 字节标签）[对拍]");

    std::array<uint8_t, 32> priv{};
    priv.fill(0x77);
    const std::optional<Bytes> shared = scrctl::wifi::x25519_shared(
        priv, bv(from_hex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c")), err);
    check(shared.has_value() &&
              to_hex(*shared) == "826d8b0b96d0c68fda11252141eb05fbfc5364752d4c21f7f13fc994870bfa0e",
          "X25519 共享密钥 [对拍]");

    const Bytes zero_peer(32, 0x00);
    std::string zero_err;
    check(!scrctl::wifi::x25519_shared(priv, bv(zero_peer), zero_err),
          "全零（低阶点）对端公钥必须拒绝");
    check(!zero_err.empty(), "拒绝低阶点时要给得出原因");
    const Bytes short_peer(31, 0x01);
    std::string short_err;
    check(!scrctl::wifi::x25519_shared(priv, bv(short_peer), short_err),
          "公钥长度不对要直接拒");

    const Bytes seed =
        from_hex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
    const std::optional<Bytes> sig = scrctl::wifi::ed25519_sign(bv(seed), {}, err);
    check(sig.has_value() &&
              to_hex(*sig) ==
                  "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33"
                  "bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
          "Ed25519 空消息签名（RFC 8032 7.1 第一条）[RFC]");
    const std::optional<Bytes> sig2 = scrctl::wifi::ed25519_sign(bv(seed), sb("hello"), err);
    check(sig2.has_value() &&
              to_hex(*sig2) ==
                  "511ca497c4d4270b098b1afd5ae4e3b951a5da2c9da6e9c0528f5761883676e7df6e4c0f0e1b5"
                  "a0a4444f4298b1882dd822fb1133cbd49abfb996c87cd5b8506",
          "Ed25519 非空消息签名 [对拍]");

    check(scrctl::wifi::b64_encode("hello world!") == "aGVsbG8gd29ybGQh", "base64 三种余数之一");
    check(scrctl::wifi::b64_encode("hi") == "aGk=", "base64 两种余数");
    check(scrctl::wifi::b64_encode("h") == "aA==", "base64 一种余数");
    const std::optional<Bytes> back = scrctl::wifi::b64_decode("aGVsbG8g\r\n d29ybGQh", err);
    check(back.has_value() && std::string(bv(*back)) == "hello world!",
          "base64 解码要容忍折行：对端按 MIME 发过来时不能解不开");
    std::string bad_b64;
    check(!scrctl::wifi::b64_decode("aGVsbG9***", bad_b64), "base64 非法字符要报错，不能默默吞");

    const std::optional<scrctl::wifi::X25519KeyPair> fresh = scrctl::wifi::x25519_keypair(err);
    check(fresh.has_value() && fresh->pub != fresh->priv, "临时密钥对生成得出来");
    const std::optional<scrctl::wifi::X25519KeyPair> fresh2 = scrctl::wifi::x25519_keypair(err);
    check(fresh2.has_value() && fresh->pub != fresh2->pub, "每次生成的临时公钥要不一样");
}

/// ---- 1b. SRP-6a(3072, SHA-512) ----
void test_srp() {
    const auto from_hex = [](const char *h) {
        Bytes out;
        for (const char *p = h; p[0] && p[1]; p += 2) {
            out.push_back(static_cast<uint8_t>(std::stoi(std::string(p, 2), nullptr, 16)));
        }
        return out;
    };
    // oracle：参考实现 pair-setup 用的那套 srptools（同一条公式链），固定私钥/盐/B 现算。
    // 设备手里的服务端认的就是这套填充——差一个前导 0，M1 就对不上。
    const Bytes salt = from_hex("000102030405060708090a0b0c0d0e0f");
    const Bytes B = from_hex(
        "7bb0e27952068343ce27c513d1de76f44c900cb3c455162f1fe6e46d8024a2d2f363b2ef4c9bb58001bcea2be8892004"
        "06c3e69b84a2cc263cca1b710ad2bdcc788138e2b5fd36ffee1dfdf9e2e86d70b825b52185d54b39ab09381c632d7a19"
        "ccaf7ef37c890ee6e699a5845c373e36d5c557071e741bca9e01bec0ed8f6de8d6f4d2ed9d852af6252117dbb19ca7b2"
        "362710dd552708d484e28b80c115f0fcf24217aa68632d3a561b27ee8a41f8df7e6f52cf52fd2b53a1ca154206fe4f2e"
        "27aa03c3c41956d5dd1b0b80c815efad10bb028e62a8b0b1b3f5c82d29fce74f7e805740517582d41f37f9fc9ed118c4"
        "52429009f96f870e12f75dc7f788395b657a6232c9ba4c58a63293c03aff353ac059c2a3f84cb5ca2c5de1c93c4e1b01"
        "f13b5587601a7486955353e6c3dac0f45162426d7b2109cdfe6b8b1bcb851f71c22484a0a75b113a30f6cd53f49f36c0"
        "1fbfe6dde88264e885fb51f3b14a9fbad7c91b42f614733c5c9af98714b755f706ab10ad5280dbc8181da0917db11e53");
    scrctl::wifi::SrpClient srp("Pair-Setup", "000000",
                                "abababababababababababababababababababababababababababababababab");
    std::string err;
    check(srp.process(salt, B, err), ("SRP process 要成功: " + err).c_str());
    check(to_hex(srp.client_public()) ==
              "26b65994a9146042a74c1a4439b43114ca79c2420955d8cad49ffdfab89e5bfa3e7f9241b23ee3bf85746a025d206e9bf"
              "cda31c8c0e695f4a848bff61599c24c3b6550c0ddaf7c5511bbc5cd79134683c2b0c52abf7fd90dc7501b3061b5156f0"
              "cfdd73ea4e979d252a50a37e2d2a324af6ee57e213522f103402ca1b744978d1ef3dada01bd636512f3ba75046010bc5"
              "3f8055049aad33cf0eeea4c404a6a2bd2bc7344b11f2306dfe07b97f0ae15e7ea1932fcba81f8fe34d46da54703b662a1"
              "f30d7271ff3692198b61172ac3886eacf3e61af1f33232c3b6402363d7fffa4783e81542df9e4c3a81c8026a3a4bdd239"
              "572e83d8fae91873d11a9b1ec0ed482a7651bbf9dcfb3534eff1055a497a779d08f82478ce02ec934eff7a9c773ee92f0"
              "59e906a36c63f118ec5fe076a3b6d18104a257dd945a255237267d62dc5d510d41607f06be7c921efddb8ee454a61b5aa"
              "37408127479db34e1f669221c8704d06cac2af1dc79c892a49fbc73a0019ee5464e25ebeb62be9eb86900c32e7c",
          "A 与 oracle 逐字节一致");
    check(to_hex(srp.session_key()) ==
              "9f26786a70eb90396c14f88f919919471f4b3c12cf59b46079d1fec46f98ee47dcdd12cd44d352af965e10fb3b42c2aa"
              "368dfa08804cb306c3e025ccae1672a7",
          "会话键 K 与 oracle 一致");
    check(to_hex(srp.client_proof()) ==
              "b3045fb6c763873a1b41ac5fae20e8f2565451b82e924757c8b0ad1c93f859bae28506245376c4b18ac57ccbc8463ac8"
              "802b074c731697b3011a044f5f3c5e44",
          "M1 与 oracle 一致");
    const Bytes m2 = from_hex(
        "06c43f139eac5e0f614ea41bb6af6de080c586d4ae44ae29fa87788023b44029329a7fc6b3f113fc369a6f420a193f7e"
        "fc805d59abb04f840d5c01818661a92a");
    check(srp.verify_server_proof(m2), "M2 要验得过");
    Bytes wrong = m2;
    wrong[0] ^= 0xff;
    check(!srp.verify_server_proof(wrong), "改一个字节的 M2 要验不过");
}


/// ---- 1c. OPACK ----
void test_opack() {
    const auto from_hex = [](const char *h) {
        Bytes out;
        for (const char *p = h; p[0] && p[1]; p += 2) {
            out.push_back(static_cast<uint8_t>(std::stoi(std::string(p, 2), nullptr, 16)));
        }
        return out;
    };
    // oracle：参考实现用的 opack2 编码器对同一份字典现算的字节（pair-setup 的 M5 INFO 形状）。
    scrctl::wifi::OpackValue info;
    info.kind = scrctl::wifi::OpackValue::Kind::kDict;
    const auto kv = [&](const char *k, scrctl::wifi::OpackValue v) {
        info.dict.emplace_back(scrctl::wifi::OpackValue::of_string(k), std::move(v));
    };
    kv("altIRK", scrctl::wifi::OpackValue::of_bytes(from_hex("e9e82dc06a49796b566f540019b1c77b")));
    kv("btAddr", scrctl::wifi::OpackValue::of_string("11:22:33:44:55:66"));
    kv("mac", scrctl::wifi::OpackValue::of_bytes(from_hex("112233445566")));
    kv("remotepairing_serial_number", scrctl::wifi::OpackValue::of_string("AAAAAAAAAAAA"));
    kv("accountID",
       scrctl::wifi::OpackValue::of_string("AC106655-9E9F-3445-96B3-075257AF1912"));
    kv("model", scrctl::wifi::OpackValue::of_string("computer-model"));
    kv("name", scrctl::wifi::OpackValue::of_string("test-host"));
    Bytes enc;
    std::string err;
    check(scrctl::wifi::opack_encode(info, enc, err), ("OPACK 编码要成功: " + err).c_str());
    check(to_hex(enc) ==
              "e746616c7449524b80e9e82dc06a49796b566f540019b1c77b466274416464725131313a32323a33333a34343a3535"
              "3a3636436d6163761122334455665b72656d6f746570616972696e675f73657269616c5f6e756d6265724c4141414141"
              "41414141414141496163636f756e744944612441433130363635352d394539462d333434352d393642332d3037353235"
              "37414631393132456d6f64656c4e636f6d70757465722d6d6f64656c446e616d6549746573742d686f7374",
          "编码字节与 oracle 逐字节一致（含 0x61 长串档与 0x80 短字节串档）");
    scrctl::wifi::OpackValue back;
    check(scrctl::wifi::opack_decode(enc, back, err), ("自己的字节要能解回来: " + err).c_str());
    const auto *alt = back.find("altIRK");
    check(alt != nullptr && alt->kind == scrctl::wifi::OpackValue::Kind::kBytes &&
              to_hex(alt->bytes) == "e9e82dc06a49796b566f540019b1c77b",
          "解回来的 altIRK 要是原字节");
    // 第二份 oracle：嵌套 + 小整数 + bool + 40 字节串 + 40 字符串（0x91/0x61 两档长度前缀）。
    const Bytes nested = from_hex(
        "e24161d309019128000000000000000000000000000000000000000000000000000000000000000000000000000000"
        "004162e14163612878787878787878787878787878787878787878787878787878787878787878787878787878787878");
    scrctl::wifi::OpackValue n;
    check(scrctl::wifi::opack_decode(nested, n, err), ("嵌套 oracle 要能解: " + err).c_str());
    const auto *a = n.find("a");
    check(a != nullptr && a->kind == scrctl::wifi::OpackValue::Kind::kList && a->list.size() == 3 &&
              a->list[0].integer == 1 && a->list[0].kind == scrctl::wifi::OpackValue::Kind::kInt &&
              a->list[1].boolean && a->list[2].bytes.size() == 40,
          "数组三档（小整数/bool/长字节串）都要对");
    const auto *b = n.find("b");
    const auto *c = b != nullptr ? b->find("c") : nullptr;
    check(c != nullptr && c->str.size() == 40, "嵌套字典里的 40 字符串要走 0x61 档");
    Bytes truncated(nested.begin(), nested.end() - 5);
    scrctl::wifi::OpackValue t;
    check(!scrctl::wifi::opack_decode(truncated, t, err), "截断的 OPACK 要报错，不能解出半截");
}

/// ---- 2. TLV ----
void test_tlv() {
    const Bytes big(300, 0xAB);
    const Bytes built = scrctl::wifi::tlv_build({
        {scrctl::wifi::TlvType::State, Bytes{0x03}},
        {scrctl::wifi::TlvType::EncryptedData, big},
    });
    // State 那条 = 1+1+1；EncryptedData 300 拆成 255+45 两条 = (1+1+255)+(1+1+45)。
    check(built.size() == 3 + 257 + 47, "超过 255 字节的值要拆成两条同类型 TLV");
    check(built[0] == 0x06 && built[1] == 0x01 && built[2] == 0x03 && built[3] == 0x05 &&
              built[4] == 0xFF && built[260] == 0x05 && built[261] == 45,
          "拆分后的字节布局：type,len");

    std::string err;
    const auto fields = scrctl::wifi::tlv_parse(built, err);
    check(err.empty(), "正常 TLV 不该报错");
    const Bytes *got = scrctl::wifi::tlv_get(fields, scrctl::wifi::TlvType::EncryptedData);
    check(got != nullptr && *got == big, "拆开写的同类型 TLV 要拼回来，不是后者覆盖前者");
    check(scrctl::wifi::tlv_state(fields) == 0x03, "状态字段取首字节");

    Bytes truncated = built;
    truncated.resize(built.size() - 20);
    std::string trunc_err;
    const auto partial = scrctl::wifi::tlv_parse(truncated, trunc_err);
    check(!trunc_err.empty(), "长度超出缓冲区的 TLV 必须报截断，不能给出一半数据");
    check(scrctl::wifi::tlv_get(partial, scrctl::wifi::TlvType::State) != nullptr,
          "截断之前已完整的那条仍然要收下来");

    const Bytes unknown = {0x7F, 0x02, 'h', 'i'};
    std::string ignore_err;
    const auto uf = scrctl::wifi::tlv_parse(unknown, ignore_err);
    check(uf.count(0x7F) == 1 && uf.at(0x7F).size() == 2,
          "不认识的类型码原样留着：设备的原文是排查线索");
}

/// ---- 3. 配对记录 ----
void test_pair_record() {
    scrctl::wifi::PairRecord rec;
    rec.udid = "00008110-000429000209801E";
    rec.host_identifier = "AC106655-9E9F-3445-96B3-075257AF1912";
    rec.host_private_key = Bytes(32, 0x11);
    rec.host_public_key = Bytes(32, 0x22);

    std::string err;
    const auto parsed = scrctl::wifi::parse_record(scrctl::wifi::format_record(rec), err);
    check(parsed.has_value() && parsed->udid == rec.udid &&
              parsed->host_identifier == rec.host_identifier &&
              parsed->host_private_key == rec.host_private_key &&
              parsed->host_public_key == rec.host_public_key && parsed->complete(),
          "记录存下去再读回来必须一模一样");
    check(scrctl::wifi::format_record(rec).find("peer_alt_irk") == std::string::npos,
          "空的可空字段不要写出去（读的时候按缺省处理）");

    scrctl::wifi::PairRecord full = rec;
    full.advertised_identifier = "32567CFA-1462-41EE-94AD-182C31AAA6C3";
    full.peer_alt_irk = Bytes(16, 0x33);
    full.remote_unlock_host_key = "b879==";
    const auto again = scrctl::wifi::parse_record(scrctl::wifi::format_record(full), err);
    check(again.has_value() && again->advertised_identifier == full.advertised_identifier &&
              again->peer_alt_irk == full.peer_alt_irk &&
              again->remote_unlock_host_key == full.remote_unlock_host_key,
          "可选字段（含 altIRK）也要过一遍存读");

    std::string bad_err;
    check(!scrctl::wifi::parse_record("udid=x\nhost_private_key=0011", bad_err),
          "首部不认就不是配对记录");
    check(!scrctl::wifi::parse_record(scrctl::wifi::format_record(rec).substr(0, 40), bad_err),
          "缺私钥字段的记录不能算完整");
    check(!scrctl::wifi::parse_record("scrctl-pair-record 1\nudid=x\nhost_private_key=0011\n",
                                      bad_err),
          "密钥长度不对要拒：32 字节是硬要求");
    const std::string with_unknown = scrctl::wifi::format_record(rec) + "future_field=whatever\n";
    std::string unknown_err;
    check(scrctl::wifi::parse_record(with_unknown, unknown_err).has_value(),
          "认不出的键要忽略：老版本读新记录得能继续用");

    const std::string path = scrctl::wifi::record_path("/d", "../../etc/x");
    check(path.rfind("/d/remote-", 0) == 0 && path.find("..") == std::string::npos &&
              path.substr(3).find('/') == std::string::npos,
          "UDID 是设备给的外部输入，直接拼进路径就是任意路径写");
}

/// ---- 4. RPPairing 帧与序号 ----
class MemStream final : public scrctl::wifi::ByteStream {
public:
    bool write_all(const void *data, size_t len, std::string &) override {
        const auto *p = static_cast<const uint8_t *>(data);
        written.append(reinterpret_cast<const char *>(p), len);  // NOLINT
        return true;
    }
    bool read_exact(void *data, size_t len, std::string &err) override {
        if (in_.size() - pos_ < len) {
            err = "测试喂进去的字节不够";
            return false;
        }
        auto *p = static_cast<uint8_t *>(data);
        for (size_t i = 0; i < len; ++i) {
            p[i] = static_cast<uint8_t>(in_[pos_ + i]);
        }
        pos_ += len;
        return true;
    }
    void feed(std::string_view bytes) { in_ += bytes; }
    std::string take_written() {
        std::string out = written;
        written.clear();
        return out;
    }

    std::string written;

private:
    std::string in_;
    size_t pos_ = 0;
};

std::string device_frame(const std::string &json_text) {
    std::string out = "RPPairing";
    out += static_cast<char>(json_text.size() >> 8);
    out += static_cast<char>(json_text.size() & 0xFF);
    out += json_text;
    return out;
}

/// 设备回的一条 streamEncrypted 信封：用 server 主密钥 + 该次交换的 nonce 封。
std::string sealed_frame(const Bytes &server_key, uint64_t counter, const std::string &plain_json) {
    std::string nonce(12, '\0');
    for (size_t i = 0; i < 8; ++i) {
        nonce[i] = static_cast<char>((counter >> (8 * i)) & 0xFF);
    }
    std::string err;
    const std::optional<Bytes> sealed =
        scrctl::wifi::chacha_seal(bv(server_key), nonce, sb(plain_json), err);
    if (!sealed) {
        return "";
    }
    return R"({"message":{"streamEncrypted":{"_0":")" + scrctl::wifi::b64_encode(*sealed) +
           R"("}},"originatedBy":"device","sequenceNumber":9})";
}

void test_rppairing() {
    MemStream io;
    scrctl::wifi::FramedCarrier carrier(io);
    scrctl::wifi::Rppairing channel(carrier);
    std::string err;

    check(channel.send_plain(j_obj({{"event", j_obj({{"ping", j_obj({})}})}}), err),
          "第一条明文要发得出去");
    std::string sent = io.take_written();
    check(sent.rfind("RPPairing", 0) == 0, "帧头是 RPPairing");
    const size_t len = (static_cast<unsigned char>(sent[9]) << 8) |
                       static_cast<unsigned char>(sent[10]);
    check(len == sent.size() - 11, "u16 大端长度要正好是后面 JSON 的字节数");
    check(sent.find("\"sequenceNumber\":0") != std::string::npos, "第一条明文序号是 0");
    check(sent.find("\"originatedBy\":\"host\"") != std::string::npos, "originatedBy 是 host");

    check(channel.send_plain(j_obj({{"event", j_obj({{"ping", j_obj({})}})}}), err),
          "第二条明文也要发得出去");
    sent = io.take_written();
    check(sent.find("\"sequenceNumber\":1") != std::string::npos, "明文发送推进序号");
    check(channel.sequence() == 2, "计数与发出去的一致");

    io.feed(device_frame(
        R"({"message":{"plain":{"_0":{"response":{"_1":{"handshake":{"_0":{"wireProtocolVersion":26}}}}}}}})"));
    const auto reply = channel.receive(err);
    check(reply.has_value(), "收一条明文回信");
    if (reply) {
        const auto *response = scrctl::json::find(*reply, "response");
        const auto *one = response != nullptr ? scrctl::json::find(*response, "_1") : nullptr;
        const auto *hs = one != nullptr ? scrctl::json::find(*one, "handshake") : nullptr;
        const auto *zero = hs != nullptr ? scrctl::json::find(*hs, "_0") : nullptr;
        const auto *version = zero != nullptr ? scrctl::json::find(*zero, "wireProtocolVersion") : nullptr;
        check(version != nullptr && scrctl::json::as_int_or(*version, 0) == 26, "握手回信里的设备版本号要取得到");
    }

    // 帧头不对的那一条要**单独一个流**：坏帧后面那些字节是没人消费的，
    // 接着用同一条流测后面的东西，测出来的是"错位"，不是本来想测的判据。
    MemStream junk_io;
    scrctl::wifi::FramedCarrier junk_carrier(junk_io);
    scrctl::wifi::Rppairing junk(junk_carrier);
    // 喂的字节**不能**带真 magic（带了先撞上的就是 JSON 解析，测不到帧头判据）。
    junk_io.feed(std::string(30, 'X'));
    std::string magic_err;
    check(!junk.receive(magic_err), "帧头不对必须报错");
    check(magic_err.find("RPPairing") != std::string::npos,
          "帧头不对要说清是帧头不对——连错端口就是这个症状");

    const Bytes client_key(32, 0x01);
    const Bytes server_key(32, 0x02);
    channel.install_main_keys(client_key, server_key);

    io.feed(device_frame(sealed_frame(server_key, 0,
                                      R"({"response":{"_1":{"createListener":{"port":55830}}}})")));
    const auto listener = channel.encrypted_roundtrip(
        j_obj({{"request", j_obj({{"_0", j_obj({{"createListener", j_obj({})}})}})}}), err);
    check(listener.has_value(), "加密往返要成功");
    if (listener) {
        const auto *created = scrctl::json::find(*listener, "createListener");
        const auto *port = created != nullptr ? scrctl::json::find(*created, "port") : nullptr;
        check(port != nullptr && scrctl::json::as_int_or(*port, 0) == 55830, "解出来的端口要对得上");
    }
    const std::string enc_sent = io.take_written();
    check(enc_sent.find("\"sequenceNumber\":2") != std::string::npos,
          "加密请求沿用的是明文停下来时的那个序号");
    check(channel.sequence() == 2 && channel.encrypted_sequence() == 1,
          "加密请求不动明文序号，只动加密计数");

    // 第二次的 nonce 必须是 1，若两边都还按 0 就会在这里暴露出来。
    io.feed(device_frame(sealed_frame(server_key, 1, R"({"response":{"_1":{"ok":1}}})")));
    const auto second = channel.encrypted_roundtrip(
        j_obj({{"request", j_obj({{"_0", j_obj({{"ping", j_obj({})}})}})}}), err);
    check(second.has_value(), "第二次加密往返要成功（nonce 要跟着计数走）");
    io.take_written();

    io.feed(device_frame(sealed_frame(
        server_key, 2,
        R"({"response":{"_1":{"errorExtended":{"_0":{"userInfo":{"NSLocalizedDescription":"Tunnel listener creator not set"}}}}}})")));
    std::string reject_err;
    check(!channel.encrypted_roundtrip(j_obj({{"request", j_obj({{"_0", j_obj({{"x", j_obj({})}})}})}}),
                                       reject_err),
          "设备拒绝要当成失败");
    check(reject_err.find("Tunnel listener creator not set") != std::string::npos,
          "设备给的人话原因要抬出来，不能只剩「缺字段」");
    check(channel.encrypted_sequence() == 2, "被拒的那一次不推进加密计数");

    MemStream cold_io;
    scrctl::wifi::FramedCarrier cold_carrier(cold_io);
    scrctl::wifi::Rppairing cold(cold_carrier);
    cold_io.feed(device_frame(
        R"({"message":{"streamEncrypted":{"_0":"AAAAAAAAAAAAAAAAAAAAAAAAAAA="}},"originatedBy":"device"})"));
    std::string cold_err;
    check(!cold.receive(cold_err), "没配对就收到加密帧要报错");
    check(cold_err.find("主密钥") != std::string::npos, "这种情况要说清是没装主密钥");
}

/// ---- 5. pair-verify 的消息形状（对着真机抓下来的字节判） ----
///
/// 这一节存在的唯一理由：真机踩的那个坑（`event` 少一层 `_0`）在这里判得住。
/// 帧的**内容**对不对只有设备说了算，但形状错了设备是"直接关连接、不给原因"，
/// 所以在离线这侧把形状钉死，比在现场靠猜便宜两个数量级。
void test_pair_verify_shape() {
    // 设备的回信用 j_obj 现搭，而不是手写一大串花括号：今晚这个测试自己就先被
    // "少写一个 }" 绊了一次，而 JSON 括号数错在源码里根本看不出来。搭出来的内容与
    // 真机上抓到的回信同构（handshake 回复 + pairingData 事件），字段值是实测的。
    const scrctl::json::Value device_handshake = j_obj(
        {{"minimumSupportedWireProtocolVersion", j_int(8)},
         {"wireProtocolVersion", j_int(26)},
         {"deviceOptions",
          j_obj({{"allowsIncomingTunnelConnections", j_bool(true)},
                 {"allowsPairSetup", j_bool(false)}})}});
    // 一层一个语句地搭，不在一行里数括号——今晚这个测试自己就先被"少一个 }"绊了一次。
    scrctl::json::Value hs_slot = j_obj({{"_0", device_handshake}});
    scrctl::json::Value hs = j_obj({{"handshake", std::move(hs_slot)}});
    scrctl::json::Value body = j_obj({{"_1", std::move(hs)}, {"forRequestIdentifier", j_int(0)}});
    scrctl::json::Value response = j_obj({{"response", std::move(body)}});
    scrctl::json::Value plain_slot = j_obj({{"_0", std::move(response)}});
    scrctl::json::Value plain = j_obj({{"plain", std::move(plain_slot)}});
    scrctl::json::Value envelope = j_obj({{"originatedBy", j_str("device")},
                                  {"sequenceNumber", j_int(0)},
                                  {"message", std::move(plain)}});
    const std::string handshake_reply = scrctl::json::write(envelope);

    // PV-Msg02：STATE=2 + 一个合法的 X25519 公钥（RFC 7748 5.2 里 Bob 的）。
    const Bytes peer_pub =
        from_hex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba9a994576788a8");
    const Bytes msg02 = scrctl::wifi::tlv_build({{scrctl::wifi::TlvType::State, Bytes{0x02}},
                                                 {scrctl::wifi::TlvType::PublicKey, peer_pub},
                                                 {scrctl::wifi::TlvType::EncryptedData,
                                                  Bytes(16, 0x5A)}});
    const auto pairing_reply = [&](const Bytes &tlv) {
        const scrctl::json::Value payload =
            j_obj({{"data", j_str(scrctl::wifi::b64_encode(tlv))},
                   {"kind", j_str("verifyManualPairing")}});
        scrctl::json::Value data_slot = j_obj({{"_0", payload}});
        scrctl::json::Value pairing = j_obj({{"pairingData", std::move(data_slot)}});
        scrctl::json::Value event_slot = j_obj({{"_0", std::move(pairing)}});
        scrctl::json::Value event = j_obj({{"event", std::move(event_slot)}});
        scrctl::json::Value plain_slot = j_obj({{"_0", std::move(event)}});
        scrctl::json::Value plain = j_obj({{"plain", std::move(plain_slot)}});
        scrctl::json::Value envelope = j_obj({{"originatedBy", j_str("device")},
                                      {"sequenceNumber", j_int(1)},
                                      {"message", std::move(plain)}});
        return scrctl::json::write(envelope);
    };
    const auto reply_with = [&](const Bytes &tlv) { return device_frame(pairing_reply(tlv)); };

    scrctl::wifi::PairRecord record;
    record.udid = "U";
    record.host_identifier = "AC106655-9E9F-3445-96B3-075257AF1912";
    record.host_private_key =
        from_hex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
    record.host_public_key = Bytes(32, 0x22);

    {
        MemStream io;
        scrctl::wifi::FramedCarrier carrier(io);
        scrctl::wifi::Rppairing channel(carrier);
        io.feed(device_frame(handshake_reply));
        io.feed(reply_with(msg02));
        io.feed(reply_with(scrctl::wifi::tlv_build({{scrctl::wifi::TlvType::State, Bytes{0x04}}})));

        std::string err;
        const scrctl::wifi::PairVerifyResult result = scrctl::wifi::pair_verify(channel, record, err);
        check(result.outcome == scrctl::wifi::VerifyOutcome::Paired,
              "回信里没有 ERROR 就该判成已配对");
        check(result.shared_secret.size() == 32, "共享密钥 32 字节");
        check(scrctl::json::find(result.device_handshake, "wireProtocolVersion") != nullptr &&
                  scrctl::json::as_int_or(*scrctl::json::find(result.device_handshake, "wireProtocolVersion"), 0) == 26,
              "设备握手里那个 26 要留档（它是设备的版本，不是我们该发的）");

        // 三条发出去的帧：handshake 请求、PV-Msg01、PV-Msg03。
        const std::string sent = io.take_written();
        check(sent.find(R"JSON("event":{"_0":{"pairingData":{"_0":{"data":)JSON") !=
                  std::string::npos,
              "pairingData 事件必须有 event._0.pairingData._0 这两层联合体包装");
        check(sent.find(R"JSON("kind":"verifyManualPairing","startNewSession":true)JSON") !=
                  std::string::npos,
              "第一条 verify 要 startNewSession=true");
        check(sent.find(R"JSON("kind":"verifyManualPairing","startNewSession":false)JSON") !=
                  std::string::npos,
              "第三条（带签名的那条）要 startNewSession=false");
        // 我们自己的 PV-Msg01 要能按同样的规矩解回来：STATE=1 + 32 字节临时公钥。
        const size_t data_at = sent.find(R"JSON("data":")JSON") + 8;
        const size_t data_end = sent.find('"', data_at);
        std::string decode_err;
        const auto tlv_bytes =
            scrctl::wifi::b64_decode(sent.substr(data_at, data_end - data_at), decode_err);
        std::string parse_err;
        const auto fields = scrctl::wifi::tlv_parse(tlv_bytes.value_or(Bytes()), parse_err);
        check(scrctl::wifi::tlv_state(fields) == 0x01, "第一步的 STATE 是 1");
        const Bytes *our_pub = scrctl::wifi::tlv_get(fields, scrctl::wifi::TlvType::PublicKey);
        check(our_pub != nullptr && our_pub->size() == 32, "第一步要带 32 字节的临时公钥");
    }

    {
        // 设备回 ERROR：要判成"没配对"，还要补一句 pairVerifyFailed（同样两层包装）。
        MemStream io;
        scrctl::wifi::FramedCarrier carrier(io);
        scrctl::wifi::Rppairing channel(carrier);
        io.feed(device_frame(handshake_reply));
        io.feed(reply_with(msg02));
        io.feed(reply_with(scrctl::wifi::tlv_build({{scrctl::wifi::TlvType::State, Bytes{0x06}},
                                                    {scrctl::wifi::TlvType::Error, Bytes{0x02}}})));

        std::string err;
        const scrctl::wifi::PairVerifyResult result = scrctl::wifi::pair_verify(channel, record, err);
        check(result.outcome == scrctl::wifi::VerifyOutcome::NotPaired,
              "设备答了但带 ERROR，要判成没配对，不能算传输失败");
        check(!err.empty(), "这种情况要给得出原因");
        const std::string sent = io.take_written();
        check(sent.find(R"JSON("event":{"_0":{"pairVerifyFailed":{}}})JSON") != std::string::npos,
              "要补一句 pairVerifyFailed 让设备把会话收干净");
    }
}

/// ---- 7. pair-setup 的离线判据 ----
///
/// SRP 与 OPACK 各自有 oracle 向量（上面两节），这里判的是**接线**：identifier 的
/// 算法、密钥生成、以及 pairingData 那条管道对三种回信形状的处理。真机那一趟才是
/// 最终判据，但"设备在等你点信任"这种一帧之差的东西，离线钉住比在现场对着静默的
/// 连接猜便宜得多。
void test_pair_setup() {
    // uuid3(DNS, ...) 的期望值由 python 的 uuid 模块生成 [对拍]。
    check(scrctl::wifi::host_identifier_uuid3("python.org") == "6FA459EA-EE8A-3CA4-894E-DB77E160355E",
          "uuid3(DNS) 要对得上（版本位与变体位最容易写错）[对拍]");
    check(scrctl::wifi::host_identifier_uuid3("localhost") == "DD8A91F6-CA32-30E0-983C-8F309D653045",
          "uuid3 另一组 [对拍]");
    check(scrctl::wifi::host_identifier_uuid3("YJBeetle-M2.local") ==
              "AC106655-9E9F-3445-96B3-075257AF1912",
          "uuid3 与参考实现在本机主机名上的取值一致 [对拍]");
    check(scrctl::wifi::host_identifier_uuid3("a") != scrctl::wifi::host_identifier_uuid3("b"),
          "不同主机名要给出不同 identifier（否则两台机器会互相顶掉记录）");

    std::string err;
    const std::optional<scrctl::wifi::Ed25519KeyPair> key = scrctl::wifi::ed25519_keypair(err);
    check(key.has_value(), "Ed25519 身份密钥要生成得出来");
    if (key) {
        check(key->pub != key->seed, "公钥不等于种子");
        const std::optional<Bytes> sig = scrctl::wifi::ed25519_sign(
            std::string_view(reinterpret_cast<const char *>(key->seed.data()), key->seed.size()),
            Bytes{0x01, 0x02}, err);
        check(sig.has_value() && sig->size() == 64, "生成出来的种子要能直接拿去签名");
    }
    const std::optional<scrctl::wifi::Ed25519KeyPair> key2 = scrctl::wifi::ed25519_keypair(err);
    check(key2.has_value() && key && key2->pub != key->pub, "每次生成的身份密钥要不一样");
    const std::optional<Bytes> r1 = scrctl::wifi::random_bytes(16, err);
    const std::optional<Bytes> r2 = scrctl::wifi::random_bytes(16, err);
    check(r1 && r2 && r1->size() == 16 && *r1 != *r2, "随机字节（altIRK 用）要真随机");

    // pairingData 管道的三种回信形状。
    const auto event_frame = [](const scrctl::json::Value &event_body) {
        scrctl::json::Value event_slot = j_obj({{"_0", event_body}});
        scrctl::json::Value event = j_obj({{"event", std::move(event_slot)}});
        scrctl::json::Value plain_slot = j_obj({{"_0", std::move(event)}});
        scrctl::json::Value plain = j_obj({{"plain", std::move(plain_slot)}});
        scrctl::json::Value envelope = j_obj({{"originatedBy", j_str("device")},
                                              {"sequenceNumber", j_int(0)},
                                              {"message", std::move(plain)}});
        return device_frame(scrctl::json::write(envelope));
    };
    const Bytes payload = Bytes{0x06, 0x01, 0x02};
    const auto data_reply = [&]() {
        const scrctl::json::Value body = j_obj(
            {{"data", j_str(scrctl::wifi::b64_encode(payload))},
             {"kind", j_str("setupManualPairing")}});
        scrctl::json::Value slot = j_obj({{"_0", body}});
        return event_frame(j_obj({{"pairingData", std::move(slot)}}));
    };

    {
        // 先回 awaitingUserConsent、再回 pairingData：两帧都得消费掉，第二帧才是数据。
        MemStream io;
        scrctl::wifi::FramedCarrier carrier(io);
        scrctl::wifi::Rppairing channel(carrier);
        io.feed(event_frame(j_obj({{"awaitingUserConsent", j_obj({})}})));
        io.feed(data_reply());
        int progress_calls = 0;
        std::string roundtrip_err;
        const std::optional<Bytes> out = scrctl::wifi::pairing_data_roundtrip(
            channel, Bytes{0x00, 0x01, 0x00}, "setupManualPairing", true, roundtrip_err,
            "host.local", [&](std::string_view) { ++progress_calls; });
        check(out.has_value() && *out == payload,
              "设备先说在等用户同意时，要接着收下一帧，不能当成失败");
        check(progress_calls == 1, "等用户点信任这件事要报出来（不报就是一次静默卡住）");
        const std::string sent = io.take_written();
        check(sent.find(R"JSON("sendingHost":"host.local")JSON") != std::string::npos,
              "pair-setup 的 pairingData 要带 sendingHost");
        check(sent.find(R"JSON("startNewSession":true)JSON") != std::string::npos,
              "M1 要 startNewSession=true");
    }
    {
        // 拒绝：那句 NSLocalizedDescription 是人话，必须原样抬出去。
        MemStream io;
        scrctl::wifi::FramedCarrier carrier(io);
        scrctl::wifi::Rppairing channel(carrier);
        scrctl::json::Value user_info = j_obj({{"NSLocalizedDescription", j_str("User denied pairing")}});
        scrctl::json::Value wrapped = j_obj({{"userInfo", std::move(user_info)}});
        scrctl::json::Value rejected = j_obj({{"wrappedError", std::move(wrapped)}});
        io.feed(event_frame(j_obj({{"pairingRejectedWithError", std::move(rejected)}})));
        std::string deny_err;
        check(!scrctl::wifi::pairing_data_roundtrip(channel, Bytes{0x00}, "setupManualPairing", true,
                                                    deny_err, "host.local", nullptr),
              "用户点了「不信任」要判成失败");
        check(deny_err.find("User denied pairing") != std::string::npos,
              "设备给的原因要原样带出来，不能只剩「没有 pairingData」");
    }
    {
        // 既不是数据也不是已知状态：报出来的是设备实际给了哪些字段，方便现场对上号。
        MemStream io;
        scrctl::wifi::FramedCarrier carrier(io);
        scrctl::wifi::Rppairing channel(carrier);
        io.feed(event_frame(j_obj({{"somethingNew", j_obj({})}})));
        std::string odd_err;
        check(!scrctl::wifi::pairing_data_roundtrip(channel, Bytes{0x00}, "setupManualPairing", true,
                                                    odd_err, "host.local", nullptr),
              "认不出的回信形状要判成失败");
        check(odd_err.find("somethingNew") != std::string::npos,
              "认不出时要把设备给的字段名列出来");
    }
    {
        // 设备在 handshake 里自报"这条面不收 pair-setup"（iOS 27 的字节流面就是这样，
        // docs §25.8）时，要当场把话说明白，而不是把 M1 发出去等它掐线——后者的症状与
        // "字段不对"一模一样，25.1–25.7 那十一条被否证的假设就是这么烧掉的。
        const auto handshake_with = [](bool allows_pair_setup) {
            scrctl::json::Value options =
                j_obj({{"allowsPairSetup", j_bool(allows_pair_setup)},
                       {"allowsIncomingTunnelConnections", j_bool(true)}});
            scrctl::json::Value hs_body = j_obj({{"wireProtocolVersion", j_int(26)},
                                                 {"deviceOptions", std::move(options)}});
            scrctl::json::Value hs = j_obj({{"handshake", j_obj({{"_0", std::move(hs_body)}})}});
            scrctl::json::Value response =
                j_obj({{"response", j_obj({{"_1", std::move(hs)}})}});
            scrctl::json::Value plain =
                j_obj({{"plain", j_obj({{"_0", std::move(response)}})}});
            scrctl::json::Value envelope = j_obj({{"originatedBy", j_str("device")},
                                                  {"sequenceNumber", j_int(0)},
                                                  {"message", std::move(plain)}});
            return device_frame(scrctl::json::write(envelope));
        };
        MemStream io;
        scrctl::wifi::FramedCarrier carrier(io);
        scrctl::wifi::Rppairing channel(carrier);
        io.feed(handshake_with(false));
        std::string setup_err;
        const scrctl::wifi::PairSetupResult result = scrctl::wifi::pair_setup(
            channel, "HOST-IDENTIFIER", "host.local", "UDID", nullptr, {}, setup_err);
        check(!result.ok, "设备自报不收 pair-setup 的面要判失败");
        check(setup_err.find("allowsPairSetup") != std::string::npos,
              "失败原因要指名设备那句自报，而不是一句含糊的'没回信'");
        check(io.written.find("pairingData") == std::string::npos,
              "判失败之前不能把 M1 发出去");
    }
}


/// json::Value 只有 find（返回指针），链式取值读起来太吵；测试里用这个。
const scrctl::json::Value &jat(const scrctl::json::Value &value, std::string_view key) {
    static const scrctl::json::Value kMissing{};
    const auto *found = scrctl::json::find(value, key);
    return found != nullptr ? *found : kMissing;
}

/// 配对信封在 RemoteXPC 载体上的类型规则。
///
/// 为什么这三条值得单独立判据：类型发错的症状与"设备不喜欢我们的字段"完全一样
/// （连接当场 invalidated），而 iOS 27 只在这条载体上收 pair-setup（docs §25.8），
/// 所以这里错了，真机上看到的又是那十条已否证假设的样子。
void test_record_listing() {
    // `--wifi` 不给 -s 时靠这个挑记录，所以"哪些算记录、哪些不算"要有判据：
    // 前缀/后缀不对的、以及空 UDID 那个 `remote-.pair`，都不能被当成一条记录。
    std::error_code ec;
    const auto root = std::filesystem::temp_directory_path(ec) / "scrctl-record-list-test";
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    for (const char *name : {"remote-BBB.pair", "remote-AAA.pair", "other.pair",
                             "remote-.pair", "notes.txt", "remote-CCC.pair.bak"}) {
        std::ofstream out(root / name);
        out << "x=1\n";
    }
    std::string err;
    const std::vector<std::string> found = scrctl::wifi::list_record_udids(root.string(), err);
    // 诱饵三个：别的扩展名、空 UDID 的 `remote-.pair`、以及 `.pair.bak` 备份。
    check(found.size() == 2, "只认 remote-<udid>.pair 这一种文件名（.bak 备份不算）");
    check(found.size() == 2 && found[0] == "AAA" && found[1] == "BBB",
          "结果要排好序（多于一条时报候选的顺序才稳定）");
    const std::vector<std::string> none =
        scrctl::wifi::list_record_udids((root / "nope").string(), err);
    check(none.empty(), "目录不存在要当成'一条都没有'，不是错误");
    std::filesystem::remove_all(root, ec);

    {
        // 父目录不存在时也要能落盘：设备那头已经点过「信任」，这一步失败意味着整趟
        // 配对白跑（记录没落盘，下次还得再点一次 29 秒的弹窗）。
        const auto deep_root = std::filesystem::temp_directory_path(ec) / "scrctl-record-mkdir";
        std::filesystem::remove_all(deep_root, ec);
        const auto deep = deep_root / "a" / "b";
        scrctl::wifi::PairRecord rec;
        rec.udid = "UDID";
        rec.host_private_key = Bytes(32, 0x11);
        rec.host_public_key = Bytes(32, 0x22);
        const std::string deep_path = (deep / "remote-X.pair").string();
        check(scrctl::wifi::save_record(deep_path, rec, err),
              "父目录不存在时要逐层建出来再落盘");
        const auto back = scrctl::wifi::load_record(deep_path, err);
        check(back.has_value() && back->host_private_key == rec.host_private_key,
              "逐层建出来的目录里记录要能读回原样");
#ifndef _WIN32
        // 叶子 0700 是头文件里的承诺；中间层（~/.local、~/.local/share 那类）是共享
        // 路径，权限必须与系统默认一致——收紧会弄坏别的程序，放宽会漏手柄。
        const auto perms = std::filesystem::status(deep).permissions();
        check((perms & std::filesystem::perms::group_all) == std::filesystem::perms::none,
              "叶子目录维持 0700（里面是能让对方在设备上打字的手柄）");
        const auto control = deep_root / "ctrl" / "leaf";
        std::filesystem::create_directories(control, ec);
        check(std::filesystem::status(deep_root / "a").permissions() ==
                  std::filesystem::status(deep_root / "ctrl").permissions(),
              "中间层权限与系统默认一致（拿同一次 create_directories 当对照，不吃 umask）");
#endif
        std::filesystem::remove_all(deep_root, ec);
    }
}

void test_pairing_xpc() {
    using scrctl::xpc::Type;
    std::string err;

    const Bytes tlv = from_hex("01010203040506");
    {
        const auto payload = j_obj({{"data", j_str(scrctl::wifi::b64_encode(tlv))},
                                    {"kind", j_str("setupManualPairing")},
                                    {"startNewSession", j_bool(false)},
                                    {"sendingHost", j_str("YJBeetle-M2")}});
        const auto inner = j_obj(
            {{"event", j_obj({{"_0", j_obj({{"pairingData", j_obj({{"_0", payload}})}})}})}});
        const auto envelope = j_obj({{"message", j_obj({{"plain", j_obj({{"_0", inner}})}})},
                                     {"originatedBy", j_str("host")},
                                     {"sequenceNumber", j_int(7)}});
        const auto x = scrctl::remote::json_to_xpc(envelope, err);
        check(x.has_value(), "配对信封要能转成 XPC 字典");
        if (!x) {
            return;
        }
        const auto &seq = x->at("sequenceNumber");
        check(seq.type == Type::UInt64 && seq.uint64 == 7, "sequenceNumber 得是 XPC uint64");
        const auto &data = x->at("message").at("plain").at("_0").at("event").at("_0")
                               .at("pairingData").at("_0").at("data");
        check(data.type == Type::Data && data.data == tlv,
              "pairingData._0.data 得是 XPC data，字节要一模一样");
        const auto &kind = x->at("message").at("plain").at("_0").at("event").at("_0")
                               .at("pairingData").at("_0").at("kind");
        check(kind.type == Type::String && kind.string == "setupManualPairing",
              "同一层的 kind 仍是字符串（规则是看路径的，不是看类型猜的）");
        const auto back = scrctl::remote::xpc_to_json(*x, err);
        check(back.has_value() && scrctl::json::write(*back) == scrctl::json::write(envelope),
              "转回 JSON 要与原信封逐字节等价（data 还原成同一份 base64）");
    }
    {
        const Bytes cipher = from_hex("aabbccdd");
        const auto envelope =
            j_obj({{"message", j_obj({{"streamEncrypted",
                                       j_obj({{"_0", j_str(scrctl::wifi::b64_encode(cipher))}})}})},
                   {"originatedBy", j_str("host")},
                   {"sequenceNumber", j_int(3)}});
        const auto x = scrctl::remote::json_to_xpc(envelope, err);
        check(x.has_value(), "加密信封要能转成 XPC");
        const auto &slot = x->at("message").at("streamEncrypted").at("_0");
        check(slot.type == Type::Data && slot.data == cipher, "streamEncrypted._0 得是 XPC data");
    }
    {
        // handshake 那条：wireProtocolVersion 是有符号 int64，不是 uint64。
        const auto handshake = j_obj(
            {{"hostOptions", j_obj({{"attemptPairVerify", j_bool(true)}})},
             {"wireProtocolVersion", j_int(scrctl::wifi::kWireProtocolVersion)}});
        const auto request = j_obj({{"request", j_obj({{"_0", j_obj(
            {{"handshake", j_obj({{"_0", handshake}})}})}})}});
        const auto envelope = j_obj({{"message", j_obj({{"plain", j_obj({{"_0", request}})}})},
                                     {"originatedBy", j_str("host")},
                                     {"sequenceNumber", j_int(0)}});
        const auto x = scrctl::remote::json_to_xpc(envelope, err);
        const auto &ver = x->at("message").at("plain").at("_0").at("request").at("_0")
                              .at("handshake").at("_0").at("wireProtocolVersion");
        check(ver.type == Type::Int64 && ver.int64 == scrctl::wifi::kWireProtocolVersion,
              "wireProtocolVersion 得是 XPC int64");
        const auto &attempt = x->at("message").at("plain").at("_0").at("request").at("_0")
                                    .at("handshake").at("_0").at("hostOptions").at("attemptPairVerify");
        check(attempt.type == Type::Bool && attempt.boolean, "attemptPairVerify 得是 XPC bool");
    }
    {
        // 别处的 data 字符串不该被当成二进制：那条 base64 解不出来，误判会直接报错。
        const auto envelope = j_obj(
            {{"response", j_obj({{"_1", j_obj({{"data", j_str("not base64 at all!")}})}})}});
        const auto x = scrctl::remote::json_to_xpc(envelope, err);
        check(x.has_value() && x->at("response").at("_1").at("data").type == Type::String,
              "不在 pairingData 下的 data 仍按字符串转");
    }
    {
        // 设备的 identifier 在这条载体上可能是 UUID 对象；还原成与字节流载体同一种文本，
        // 上层读到的才是同一个东西。
        scrctl::xpc::Value dict = scrctl::xpc::make_dict();
        scrctl::xpc::dict_set(dict, "identifier",
                              scrctl::xpc::make_uuid(from_hex("ac1066559e9f344596b3075257af1912")));
        scrctl::xpc::dict_set(dict, "port", scrctl::xpc::make_uint64(49152));
        const auto j = scrctl::remote::xpc_to_json(dict, err);
        check(j.has_value(), "XPC 字典要能转成 JSON");
        check(j && scrctl::json::as_string_or(jat(*j, "identifier")) == "ac106655-9e9f-3445-96b3-075257af1912",
              "UUID 要还原成 8-4-4-4-12 文本");
        check(j && scrctl::json::as_int_or(jat(*j, "port")) == 49152, "uint64 要变成 JSON 整数");
    }
}

}  // namespace

int main() {
    test_crypto();
    test_srp();
    test_opack();
    test_tlv();
    test_pair_record();
    test_rppairing();
    test_pair_verify_shape();
    test_pair_setup();
    test_record_listing();
    test_pairing_xpc();
    std::printf("%d 条判据，%d 条不通过\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
