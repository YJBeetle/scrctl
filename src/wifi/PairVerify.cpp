#include "wifi/PairVerify.h"

#include "wifi/Tlv.h"

namespace scrctl::wifi {

PairVerifyResult pair_verify(Rppairing &channel, const PairRecord &host, std::string &err,
                             bool announce_failure) {
    PairVerifyResult result;
    if (!host.complete()) {
        err = "配对记录不完整，没法签名";
        result.outcome = VerifyOutcome::NotPaired;
        result.error = err;
        return result;
    }

    // 1) handshake：告诉设备"我配过你了，走 verify 这条路"。
    const json::Value host_options = j_obj({{"attemptPairVerify", j_bool(true)}});
    const json::Value handshake_body = j_obj({{"hostOptions", host_options},
                                              {"wireProtocolVersion", j_int(kWireProtocolVersion)}});
    const json::Value request = j_obj({{"request", j_obj({{"_0", j_obj({{"handshake", j_obj({{"_0", handshake_body}})}})}})}});
    const std::optional<json::Value> handshake = channel.plain_roundtrip(request, err);
    if (!handshake) {
        result.error = err;
        return result;
    }
    if (const json::Value *response = handshake->find("response")) {
        const json::Value *one = response->find("_1");
        const json::Value *hs = one != nullptr ? one->find("handshake") : nullptr;
        const json::Value *zero = hs != nullptr ? hs->find("_0") : nullptr;
        if (zero != nullptr) {
            result.device_handshake = *zero;
        }
    }

    // 2) PV-Msg01：我们这一步的临时 X25519 公钥（每次握手都新生成，这是前向保密的来源）。
    const std::optional<X25519KeyPair> keypair = x25519_keypair(err);
    if (!keypair) {
        result.error = err;
        return result;
    }
    const Bytes our_pub(keypair->pub.begin(), keypair->pub.end());
    const Bytes first = tlv_build({{TlvType::State, Bytes{0x01}}, {TlvType::PublicKey, our_pub}});
    const std::optional<Bytes> reply1 =
        pairing_data_roundtrip(channel, first, "verifyManualPairing", true, err);
    if (!reply1) {
        result.error = err;
        return result;
    }

    // 3) PV-Msg02。注意这一段的 encryptedData 我们**不解**——那是设备的身份材料，
    //    而"对端是不是那台设备"在这一步是由"我们签的东西对方验不验得过"来证明的。
    //    苹果那套实现同样没解（它的 TODO 里明写着），所以这里不解不是偷懒，是与对端一致。
    std::string tlv_err;
    const std::map<uint8_t, Bytes> second = tlv_parse(*reply1, tlv_err);
    // 设备在 M2 里就带 ERROR = 它连试都不试（"Not paired with anyone"，设备 oslog 原话）。
    // 这时**不能**再发 PV-Msg03：那条序外消息会让设备把连接掐掉（docs §25.6，我们踩了
    // 一整晚）。正确收尾是一句 pairVerifyFailed，然后报"没配对"。
    if (tlv_get(second, TlvType::Error) != nullptr) {
        if (announce_failure) {
            std::string ignored;
            json::Value body = j_obj({{"pairVerifyFailed", j_obj({})}});
            channel.send_plain(j_obj({{"event", j_obj({{"_0", std::move(body)}})}}), ignored);
        }
        err = "设备不认识这条配对记录（没配过，或者在设备上被删了）";
        result.outcome = VerifyOutcome::NotPaired;
        result.error = err;
        return result;
    }
    const Bytes *peer_pub = tlv_get(second, TlvType::PublicKey);
    if (peer_pub == nullptr || peer_pub->size() != 32) {
        err = "PV-Msg02 里没有 32 字节的公钥" +
              (tlv_err.empty() ? std::string() : std::string("：") + tlv_err);
        result.error = err;
        return result;
    }
    const std::optional<Bytes> shared = x25519_shared(keypair->priv, sv(*peer_pub), err);
    if (!shared) {
        result.error = err;
        return result;
    }
    result.shared_secret = *shared;

    const std::optional<Bytes> verify_key =
        hkdf_sha512(*shared, "Pair-Verify-Encrypt-Salt", "Pair-Verify-Encrypt-Info", 32, err);
    if (!verify_key) {
        result.error = err;
        return result;
    }
    Bytes sign_buf = our_pub;
    sign_buf.insert(sign_buf.end(), host.host_identifier.begin(), host.host_identifier.end());
    sign_buf.insert(sign_buf.end(), peer_pub->begin(), peer_pub->end());
    const std::optional<Bytes> signature = ed25519_sign(sv(host.host_private_key), sign_buf, err);
    if (!signature) {
        result.error = err;
        return result;
    }
    const Bytes identity = tlv_build(
        {{TlvType::Identifier, bytes_of(host.host_identifier)}, {TlvType::Signature, *signature}});
    static constexpr char kPvMsg03[] = "\x00\x00\x00\x00PV-Msg03";
    const std::string_view msg03_nonce(kPvMsg03, sizeof(kPvMsg03) - 1);
    const std::optional<Bytes> sealed = chacha_seal(sv(*verify_key), msg03_nonce, identity, err);
    if (!sealed) {
        result.error = err;
        return result;
    }

    // 4) PV-Msg03 → PV-Msg04。到这里设备才真正"认人"。
    const Bytes third =
        tlv_build({{TlvType::State, Bytes{0x03}}, {TlvType::EncryptedData, *sealed}});
    const std::optional<Bytes> reply3 =
        pairing_data_roundtrip(channel, third, "verifyManualPairing", false, err);
    if (!reply3) {
        result.error = err;
        return result;
    }
    std::string final_err;
    const std::map<uint8_t, Bytes> final_fields = tlv_parse(*reply3, final_err);
    if (tlv_get(final_fields, TlvType::Error) != nullptr) {
        // 设备答了、但说不认识这把钥匙。补一句 pairVerifyFailed 让对端把会话收干净，
        // 然后**不要**重连重试——重试一万次也是同一句。
        if (announce_failure) {
            std::string ignored;
            json::Value body = j_obj({{"pairVerifyFailed", j_obj({})}});
            channel.send_plain(j_obj({{"event", j_obj({{"_0", std::move(body)}})}}), ignored);
        }
        err = "设备不认识这条配对记录（没配过，或者在设备上被删了）";
        result.outcome = VerifyOutcome::NotPaired;
        result.error = err;
        return result;
    }

    // 5) 主密钥。这两条 HKDF 的 salt 是**空**（RFC 5869 下等于全零盐），和第 3 步
    //    那条带 salt 的不是一回事；写串一个字节就是"设备回的所有帧都解不开"。
    const std::optional<Bytes> client_key = hkdf_sha512(*shared, "", "ClientEncrypt-main", 32, err);
    if (!client_key) {
        result.error = err;
        return result;
    }
    const std::optional<Bytes> server_key = hkdf_sha512(*shared, "", "ServerEncrypt-main", 32, err);
    if (!server_key) {
        result.error = err;
        return result;
    }
    channel.install_main_keys(*client_key, *server_key);
    result.outcome = VerifyOutcome::Paired;
    return result;
}

}  // namespace scrctl::wifi
