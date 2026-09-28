#include "wifi/PairSetup.h"

#include <openssl/evp.h>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <unistd.h>
#endif

#include <cstdio>
#include <map>

#include "wifi/Opack.h"
#include "wifi/PairVerify.h"
#include "wifi/Srp.h"
#include "wifi/Tlv.h"

namespace scrctl::wifi {
namespace {

/// RFC 4122 的 DNS 命名空间 UUID（`6ba7b810-9dad-11d1-80b4-00c04fd430c8`）。
constexpr uint8_t kNamespaceDns[16] = {0x6b, 0xa7, 0xb8, 0x10, 0x9d, 0xad, 0x11, 0xd1,
                                       0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8};

std::string hex_upper(const uint8_t *data, size_t len) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kDigits[data[i] >> 4]);
        out.push_back(kDigits[data[i] & 0x0F]);
    }
    return out;
}

/// uuid3 用的 MD5。这里 MD5 不是做密码学用途，只是 UUID 版本 3 的定义本身——
/// 换成 SHA-256 得到的就是另一个 UUID，设备认的是那一个。
bool md5_of(std::string_view data, Bytes &out, std::string &err) {
    out.assign(16, 0);
    unsigned int len = 0;
    if (EVP_Digest(data.data(), data.size(), out.data(), &len, EVP_md5(), nullptr) != 1 ||
        len != 16) {
        err = "算不出 MD5（这个 OpenSSL 把 MD5 挪进 legacy provider 了？）";
        return false;
    }
    return true;
}

/// handshake：与 pair-verify 同一条开场白，只是 `attemptPairVerify` 由调用方定。
/// 设备在这里自报 identifier 与 model，前者正是记录里 advertised_identifier 的来源。
std::optional<json::Value> do_handshake(Rppairing &channel, bool attempt_verify,
                                        std::string &err) {
    const json::Value host_options = j_obj({{"attemptPairVerify", j_bool(attempt_verify)}});
    const json::Value body = j_obj({{"hostOptions", host_options},
                                    {"wireProtocolVersion", j_int(kWireProtocolVersion)}});
    const json::Value request =
        j_obj({{"request", j_obj({{"_0", j_obj({{"handshake", j_obj({{"_0", body}})}})}})}});
    const std::optional<json::Value> reply = channel.plain_roundtrip(request, err);
    if (!reply) {
        return std::nullopt;
    }
    const json::Value *response = reply->find("response");
    const json::Value *one = response != nullptr ? response->find("_1") : nullptr;
    const json::Value *handshake = one != nullptr ? one->find("handshake") : nullptr;
    const json::Value *zero = handshake != nullptr ? handshake->find("_0") : nullptr;
    if (zero == nullptr) {
        err = "handshake 回信里没有 response._1.handshake._0";
        return std::nullopt;
    }
    return *zero;
}

/// 把 M2/M4/M6 这类回信解开，顺手挡掉设备塞在里面的错误码。
std::optional<std::map<uint8_t, Bytes>> parse_reply(const Bytes &raw, const char *which,
                                                    std::string &err) {
    std::string tlv_err;
    std::map<uint8_t, Bytes> fields = tlv_parse(raw, tlv_err);
    if (!tlv_err.empty()) {
        err = std::string(which) + " 的 TLV 没解干净: " + tlv_err;
        return std::nullopt;
    }
    if (const Bytes *code = tlv_get(fields, TlvType::Error)) {
        err = std::string(which) + " 带错误码 0x";
        err += hex_upper(code->data(), code->size());
        return std::nullopt;
    }
    return fields;
}

}  // namespace

std::string host_identifier_uuid3(std::string_view hostname) {
    Bytes input(kNamespaceDns, kNamespaceDns + sizeof(kNamespaceDns));
    input.insert(input.end(), hostname.begin(), hostname.end());
    Bytes digest;
    std::string err;
    if (!md5_of(sv(input), digest, err)) {
        return {};
    }
    digest[6] = static_cast<uint8_t>((digest[6] & 0x0F) | 0x30);  // version 3
    digest[8] = static_cast<uint8_t>((digest[8] & 0x3F) | 0x80);  // RFC 4122 variant
    std::string out = hex_upper(digest.data(), 4) + "-" + hex_upper(digest.data() + 4, 2) + "-" +
                      hex_upper(digest.data() + 6, 2) + "-" + hex_upper(digest.data() + 8, 2) +
                      "-" + hex_upper(digest.data() + 10, 6);
    return out;
}

std::string local_hostname() {
    char buf[256] = {0};
    if (::gethostname(buf, sizeof(buf) - 1) != 0) {
        return {};
    }
    return std::string(buf);
}

PairSetupResult pair_setup(Rppairing &channel, std::string_view host_identifier,
                           std::string_view hostname, std::string_view udid,
                           const ProgressFn &progress, const PairSetupOptions &options,
                           std::string &err) {
    PairSetupResult result;
    const auto fail = [&result, &err]() {
        result.error = err;
        return result;
    };
    if (host_identifier.empty()) {
        err = "没有 host identifier，注册不了（主机名取不到时可以显式给一个）";
        return fail();
    }
    // host 密钥一开始就生成：verify 探针的签名必须用它（真钥匙），M5 注册的是同一把。
    const std::optional<Ed25519KeyPair> host_key = ed25519_keypair(err);
    if (!host_key) {
        return fail();
    }
    const Bytes host_public(host_key->pub.begin(), host_key->pub.end());
    // 苹果客户端在 pairingData 里报的主机名不带 ".local"（oslog 实测 sendingHost 是
    // "YJBeetle-M2"），照它来。
    std::string host_label(hostname);
    if (host_label.size() > 6 && host_label.compare(host_label.size() - 6, 6, ".local") == 0) {
        host_label.erase(host_label.size() - 6);
    }

    // 1) 开场。两档（见 PairSetupOptions）：
    //    probe_verify_first —— 先按 verify 问一轮"认不认识我"（参考实现的走法）；
    //    否则 —— handshake 里直接报 attemptPairVerify=false，然后发 setup 的 M1。
    //    iOS 27 上这两档实测都被设备掐掉（docs §25.3），留开关是为了下一台设备/下一次
    //    现场能一行命令换着试，不用重新编译。
    std::string advertised;
    if (options.probe_verify_first) {
        const std::optional<json::Value> device_handshake =
            do_handshake(channel, /*attempt_verify=*/true, err);
        if (!device_handshake) {
            return fail();
        }
        result.device_handshake = *device_handshake;
        if (const json::Value *peer = device_handshake->find("peerDeviceInfo")) {
            if (const json::Value *identifier = peer->find("identifier")) {
                advertised = identifier->as_string_or();
            }
        }
        // verify 探针。签名必须用**真钥匙**：全零钥匙的签名在密码学上无效，设备会走错误
        // 路径、状态卡在 verifyManualPairingInProgress，之后的 upgrade M1 一律被掐；
        // 真钥匙 + 未知 identifier 才走到干净的 unauthenticated（docs §25.6，苹果成功
        // 样本与我们的失败样本在设备 oslog 里逐行对出来的差别）。
        const std::optional<X25519KeyPair> vk = x25519_keypair(err);
        if (!vk) {
            return fail();
        }
        const Bytes our_x_pub(vk->pub.begin(), vk->pub.end());
        const Bytes v1 =
            tlv_build({{TlvType::State, Bytes{0x01}}, {TlvType::PublicKey, our_x_pub}});
        const std::optional<Bytes> raw_v2 =
            pairing_data_roundtrip(channel, v1, "verifyManualPairing", true, err);
        if (!raw_v2) {
            return fail();
        }
        const std::optional<std::map<uint8_t, Bytes>> vf = parse_reply(*raw_v2, "verify-M2", err);
        if (!vf) {
            return fail();
        }
        const auto send_verify_failed = [&channel]() {
            std::string ignored;
            json::Value body = j_obj({{"pairVerifyFailed", j_obj({})}});
            channel.send_plain(j_obj({{"event", j_obj({{"_0", std::move(body)}})}}), ignored);
        };
        if (tlv_get(*vf, TlvType::Error) != nullptr) {
            // 设备连试都不试（"Not paired with anyone"）：回一句 pairVerifyFailed 就落到
            // unauthenticated，**千万别发 Msg03**（docs §25.6）。
            send_verify_failed();
        } else {
            // identifier 可能在设备那边挂着（含已撤销的）：走完 Msg03 看它认不认。
            const Bytes *peer_x = tlv_get(*vf, TlvType::PublicKey);
            if (peer_x == nullptr || peer_x->size() != 32) {
                err = "verify 的 M2 里没有 32 字节公钥";
                return fail();
            }
            const std::optional<Bytes> shared = x25519_shared(vk->priv, sv(*peer_x), err);
            if (!shared) {
                return fail();
            }
            const std::optional<Bytes> vkey = hkdf_sha512(*shared, "Pair-Verify-Encrypt-Salt",
                                                          "Pair-Verify-Encrypt-Info", 32, err);
            if (!vkey) {
                return fail();
            }
            Bytes signbuf = our_x_pub;
            signbuf.insert(signbuf.end(), host_identifier.begin(), host_identifier.end());
            signbuf.insert(signbuf.end(), peer_x->begin(), peer_x->end());
            const std::optional<Bytes> sig =
                ed25519_sign(std::string_view(reinterpret_cast<const char *>(host_key->seed.data()),  // NOLINT
                                              host_key->seed.size()),
                             signbuf, err);
            if (!sig) {
                return fail();
            }
            const Bytes identity = tlv_build({{TlvType::Identifier, bytes_of(host_identifier)},
                                              {TlvType::Signature, *sig}});
            static constexpr char kPvMsg03[] = "\x00\x00\x00\x00PV-Msg03";
            const std::optional<Bytes> sealed = chacha_seal(
                sv(*vkey), std::string_view(kPvMsg03, sizeof(kPvMsg03) - 1), identity, err);
            if (!sealed) {
                return fail();
            }
            const Bytes v3 =
                tlv_build({{TlvType::State, Bytes{0x03}}, {TlvType::EncryptedData, *sealed}});
            const std::optional<Bytes> raw_v4 =
                pairing_data_roundtrip(channel, v3, "verifyManualPairing", false, err);
            if (!raw_v4) {
                return fail();
            }
            std::string v4_tlv_err;
            const std::map<uint8_t, Bytes> v4f = tlv_parse(*raw_v4, v4_tlv_err);
            if (!v4_tlv_err.empty()) {
                err = "verify-M4 的 TLV 没解干净: " + v4_tlv_err;
                return fail();
            }
            if (tlv_get(v4f, TlvType::Error) != nullptr) {
                // ERROR = 不认这把钥匙，正是我们要的正常结局。
                send_verify_failed();
            } else {
                err = "设备认这个 identifier：已经配过了，不需要 pair-setup";
                return fail();
            }
        }
        if (progress) {
            progress("verify 探针干净落地（设备没认这把钥匙），发 upgrade M1");
        }
    } else {
        const std::optional<json::Value> device_handshake =
            do_handshake(channel, /*attempt_verify=*/false, err);
        if (!device_handshake) {
            return fail();
        }
        result.device_handshake = *device_handshake;
        if (const json::Value *peer = device_handshake->find("peerDeviceInfo")) {
            if (const json::Value *identifier = peer->find("identifier")) {
                advertised = identifier->as_string_or();
            }
        }
        if (progress) {
            progress("handshake（attemptPairVerify=false）完成，发 M1");
        }
    }
    // 2) M1 → M2：设备给出 SRP 的 salt 与 B。kind 见 PairSetupOptions::pairing_kind。
    const std::string kind = options.pairing_kind;
    const Bytes m1 = tlv_build({{TlvType::Method, Bytes{0x00}}, {TlvType::State, Bytes{0x01}}});
    const std::optional<Bytes> raw_m2 =
        pairing_data_roundtrip(channel, m1, kind, true, err, host_label, progress);
    if (!raw_m2) {
        return fail();
    }
    const std::optional<std::map<uint8_t, Bytes>> fields2 = parse_reply(*raw_m2, "M2", err);
    if (!fields2) {
        return fail();
    }
    const Bytes *server_public = tlv_get(*fields2, TlvType::PublicKey);
    const Bytes *salt = tlv_get(*fields2, TlvType::Salt);
    if (server_public == nullptr || salt == nullptr || server_public->empty() || salt->empty()) {
        err = "M2 里缺 PUBLIC_KEY 或 SALT";
        return fail();
    }

    // 3) SRP-6a。PIN 固定 "000000"（见头注释）。
    SrpClient srp("Pair-Setup", "000000");
    if (!srp.process(*salt, *server_public, err)) {
        return fail();
    }

    // 4) M3 → M4：我们给 A 与 M1 证明，设备回它自己的 M2 证明。
    const Bytes m3 = tlv_build({{TlvType::State, Bytes{0x03}},
                                {TlvType::PublicKey, srp.client_public()},
                                {TlvType::Proof, srp.client_proof()}});
    const std::optional<Bytes> raw_m4 =
        pairing_data_roundtrip(channel, m3, kind, false, err, host_label, progress);
    if (!raw_m4) {
        return fail();
    }
    const std::optional<std::map<uint8_t, Bytes>> fields4 = parse_reply(*raw_m4, "M4", err);
    if (!fields4) {
        return fail();
    }
    const Bytes *server_proof = tlv_get(*fields4, TlvType::Proof);
    if (server_proof == nullptr) {
        err = "M4 里没有 PROOF";
        return fail();
    }
    // 这一条是 pair-setup 唯一能挡住"中间人接了这条控制面"的地方：设备若不知道 PIN，
    // 就算不出 K，也就给不出对的 M2。不过就得往下走等于把 host 密钥交给陌生人。
    if (!srp.verify_server_proof(*server_proof)) {
        err = "设备的 M2 证明对不上：要么它不知道 PIN，要么这条面被中间人接了，不能继续注册";
        return fail();
    }
    if (progress) {
        progress("SRP 双向证明通过，正在把我们的 host 密钥注册到设备上（M5）");
    }

    const Bytes &session_key = srp.session_key();
    const std::optional<Bytes> setup_key =
        hkdf_sha512(session_key, "Pair-Setup-Encrypt-Salt", "Pair-Setup-Encrypt-Info", 32, err);
    if (!setup_key) {
        return fail();
    }
    const std::optional<Bytes> sign_prefix = hkdf_sha512(
        session_key, "Pair-Setup-Controller-Sign-Salt", "Pair-Setup-Controller-Sign-Info", 32, err);
    if (!sign_prefix) {
        return fail();
    }
    Bytes signbuf = *sign_prefix;
    signbuf.insert(signbuf.end(), host_identifier.begin(), host_identifier.end());
    signbuf.insert(signbuf.end(), host_public.begin(), host_public.end());
    const std::optional<Bytes> signature =
        ed25519_sign(std::string_view(reinterpret_cast<const char *>(host_key->seed.data()),  // NOLINT
                                      host_key->seed.size()),
                     signbuf, err);
    if (!signature) {
        return fail();
    }

    // 5) M5 里的设备信息（OPACK 字典）。altIRK 是**我们**那把身份解析密钥，设备把它
    //    存进这份配对记录；mac/btAddr 设备到底看不看我们没量过，照参考实现带上，
    //    填的是随机值——真网卡地址没有理由交给设备。
    const std::optional<Bytes> our_alt_irk = random_bytes(16, err);
    if (!our_alt_irk) {
        return fail();
    }
    const std::optional<Bytes> fake_mac = random_bytes(6, err);
    if (!fake_mac) {
        return fail();
    }
    char mac_text[18] = {0};
    std::snprintf(mac_text, sizeof(mac_text), "%02x:%02x:%02x:%02x:%02x:%02x", (*fake_mac)[0],
                  (*fake_mac)[1], (*fake_mac)[2], (*fake_mac)[3], (*fake_mac)[4], (*fake_mac)[5]);
    OpackValue info;
    info.kind = OpackValue::Kind::kDict;
    info.dict = {
        {OpackValue::of_string("altIRK"), OpackValue::of_bytes(*our_alt_irk)},
        {OpackValue::of_string("btAddr"), OpackValue::of_string(mac_text)},
        {OpackValue::of_string("mac"), OpackValue::of_bytes(*fake_mac)},
        {OpackValue::of_string("remotepairing_serial_number"),
         OpackValue::of_string("AAAAAAAAAAAA")},
        {OpackValue::of_string("accountID"), OpackValue::of_string(std::string(host_identifier))},
        {OpackValue::of_string("model"), OpackValue::of_string("computer-model")},
        {OpackValue::of_string("name"), OpackValue::of_string(host_label)},
    };
    Bytes info_bytes;
    if (!opack_encode(info, info_bytes, err)) {
        return fail();
    }
    const Bytes identity_tlv = tlv_build({{TlvType::Identifier, bytes_of(host_identifier)},
                                          {TlvType::PublicKey, host_public},
                                          {TlvType::Signature, *signature},
                                          {TlvType::Info, info_bytes}});
    static constexpr char kPsMsg05[] = "\x00\x00\x00\x00PS-Msg05";
    const std::optional<Bytes> sealed = chacha_seal(
        sv(*setup_key), std::string_view(kPsMsg05, sizeof(kPsMsg05) - 1), identity_tlv, err);
    if (!sealed) {
        return fail();
    }
    const Bytes m5 =
        tlv_build({{TlvType::EncryptedData, *sealed}, {TlvType::State, Bytes{0x05}}});
    const std::optional<Bytes> raw_m6 =
        pairing_data_roundtrip(channel, m5, kind, false, err, host_label, progress);
    if (!raw_m6) {
        return fail();
    }
    const std::optional<std::map<uint8_t, Bytes>> fields6 = parse_reply(*raw_m6, "M6", err);
    if (!fields6) {
        return fail();
    }

    // 6) M6：设备那份身份材料。我们只从里面取 altIRK（mDNS 广播里的 authTag 由它
    //    派生，见 #49）。设备自己的签名这里**不验**：它用的 salt 我们没量过，而身份
    //    已经由上面那条 M2 证明 + "我们签的东西设备收下了"两件事确立。
    Bytes peer_alt_irk;
    const Bytes *sealed6 = tlv_get(*fields6, TlvType::EncryptedData);
    if (sealed6 != nullptr) {
        static constexpr char kPsMsg06[] = "\x00\x00\x00\x00PS-Msg06";
        std::string open_err;
        const std::optional<Bytes> plain6 =
            chacha_open(sv(*setup_key), std::string_view(kPsMsg06, sizeof(kPsMsg06) - 1), *sealed6,
                        open_err);
        if (!plain6) {
            err = "M6 解不开（setup 密钥不对？）: " + open_err;
            return fail();
        }
        std::string inner_err;
        const std::map<uint8_t, Bytes> inner = tlv_parse(*plain6, inner_err);
        if (const Bytes *peer_info = tlv_get(inner, TlvType::Info)) {
            OpackValue parsed;
            std::string opack_err;
            if (opack_decode(*peer_info, parsed, opack_err)) {
                if (const OpackValue *irk = parsed.find("altIRK")) {
                    if (irk->kind == OpackValue::Kind::kBytes && irk->bytes.size() == 16) {
                        peer_alt_irk = irk->bytes;
                    }
                }
            }
        }
    }

    // 7) 主密钥。salt 是**空**串（等于全零盐），与上面那两条带 salt 的不是一回事；
    //    这一对密钥既加密这条控制面之后的请求，也派生出隧道用的东西。
    const std::optional<Bytes> client_key =
        hkdf_sha512(session_key, "", "ClientEncrypt-main", 32, err);
    if (!client_key) {
        return fail();
    }
    const std::optional<Bytes> server_key =
        hkdf_sha512(session_key, "", "ServerEncrypt-main", 32, err);
    if (!server_key) {
        return fail();
    }
    channel.install_main_keys(*client_key, *server_key);

    PairRecord record;
    record.udid = std::string(udid);
    record.host_identifier = std::string(host_identifier);
    record.host_private_key.assign(host_key->seed.begin(), host_key->seed.end());
    record.host_public_key = host_public;
    record.advertised_identifier = advertised;
    record.peer_alt_irk = peer_alt_irk;

    // 8) 远程解锁密钥。这一步失败不致命：老设备/tvOS 没有这个功能（参考实现也是
    //    try/except 吞掉），而它跟"能不能镜像+控制"无关。
    std::string unlock_err;
    const json::Value unlock_request =
        j_obj({{"request", j_obj({{"_0", j_obj({{"createRemoteUnlockKey", j_obj({})}})}})}});
    const std::optional<json::Value> unlock = channel.encrypted_roundtrip(unlock_request, unlock_err);
    if (unlock) {
        const json::Value *created = unlock->find("createRemoteUnlockKey");
        const json::Value *host_key_field = created != nullptr ? created->find("hostKey") : nullptr;
        if (host_key_field != nullptr) {
            record.remote_unlock_host_key = host_key_field->as_string_or();
        }
    } else if (progress) {
        progress(std::string("createRemoteUnlockKey 没成（") + unlock_err + "），不影响配对本身");
    }

    result.record = std::move(record);
    result.ok = true;
    return result;
}

}  // namespace scrctl::wifi
