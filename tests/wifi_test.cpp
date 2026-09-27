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
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wifi/Crypto.h"
#include "wifi/PairRecord.h"
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
    scrctl::wifi::Rppairing channel(io);
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
        const auto *response = reply->find("response");
        const auto *one = response != nullptr ? response->find("_1") : nullptr;
        const auto *hs = one != nullptr ? one->find("handshake") : nullptr;
        const auto *zero = hs != nullptr ? hs->find("_0") : nullptr;
        const auto *version = zero != nullptr ? zero->find("wireProtocolVersion") : nullptr;
        check(version != nullptr && version->as_int_or(0) == 26, "握手回信里的设备版本号要取得到");
    }

    // 帧头不对的那一条要**单独一个流**：坏帧后面那些字节是没人消费的，
    // 接着用同一条流测后面的东西，测出来的是"错位"，不是本来想测的判据。
    MemStream junk_io;
    scrctl::wifi::Rppairing junk(junk_io);
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
        const auto *created = listener->find("createListener");
        const auto *port = created != nullptr ? created->find("port") : nullptr;
        check(port != nullptr && port->as_int_or(0) == 55830, "解出来的端口要对得上");
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
    scrctl::wifi::Rppairing cold(cold_io);
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
        scrctl::wifi::Rppairing channel(io);
        io.feed(device_frame(handshake_reply));
        io.feed(reply_with(msg02));
        io.feed(reply_with(scrctl::wifi::tlv_build({{scrctl::wifi::TlvType::State, Bytes{0x04}}})));

        std::string err;
        const scrctl::wifi::PairVerifyResult result = scrctl::wifi::pair_verify(channel, record, err);
        check(result.outcome == scrctl::wifi::VerifyOutcome::Paired,
              "回信里没有 ERROR 就该判成已配对");
        check(result.shared_secret.size() == 32, "共享密钥 32 字节");
        check(result.device_handshake.find("wireProtocolVersion") != nullptr &&
                  result.device_handshake.find("wireProtocolVersion")->as_int_or(0) == 26,
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
        scrctl::wifi::Rppairing channel(io);
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

}  // namespace

int main() {
    test_crypto();
    test_tlv();
    test_pair_record();
    test_rppairing();
    test_pair_verify_shape();
    std::printf("%d 条判据，%d 条不通过\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
