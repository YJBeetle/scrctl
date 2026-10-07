// 经真实 RPPairing 字节流测试 setup/verify 探测的阶段与错误分支，不需要设备。
#include "wifi/PairSetup.h"
#include "wifi/Opack.h"
#include "wifi/Rppairing.h"
#include "wifi/Tlv.h"

#include <openssl/bn.h>
#include <openssl/evp.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace scrctl::wifi;
using Check = void (*)(bool, const char *);
using Bn = std::unique_ptr<BIGNUM, decltype(&BN_clear_free)>;

void require(bool ok) {
    if (!ok) throw std::runtime_error("pair-setup test fixture failed");
}
Bn bn() { return Bn(BN_new(), BN_clear_free); }
Bn integer(const Bytes &bytes) {
    auto out = bn();
    require(out && BN_bin2bn(bytes.data(), static_cast<int>(bytes.size()), out.get()));
    return out;
}
Bytes encoded(const BIGNUM *n, bool padded = false) {
    Bytes out(padded ? 384 : static_cast<size_t>(BN_num_bytes(n)));
    require(BN_bn2binpad(n, out.data(), static_cast<int>(out.size())) == static_cast<int>(out.size()));
    return out;
}
Bytes hash(const std::vector<Bytes> &parts) {
    Bytes in;
    for (const auto &part : parts) in.insert(in.end(), part.begin(), part.end());
    Bytes out(64);
    unsigned int size = 0;
    require(EVP_Digest(in.data(), in.size(), out.data(), &size, EVP_sha512(), nullptr) == 1 && size == 64);
    return out;
}

// 仅用于内存流夹具的 SRP 服务端，固定 b=1：B=(k*v+g) mod N，S=A*v^u mod N。
// 根据真实客户端发来的随机 A 和证明生成 M4，避免绕过客户端的服务端证明校验。
struct SrpPeer {
    Bn n{BN_get_rfc3526_prime_3072(nullptr), BN_clear_free};
    Bn v = bn();
    Bytes salt = Bytes(16, 0x42);
    Bytes public_key;
    Bytes session_key;
    SrpPeer() {
        std::unique_ptr<BN_CTX, decltype(&BN_CTX_free)> ctx(BN_CTX_new(), BN_CTX_free);
        auto g = bn(), kv = bn(), b = bn();
        require(n && v && ctx && g && kv && b && BN_set_word(g.get(), 5) == 1);
        const auto x = integer(hash({salt, hash({bytes_of("Pair-Setup:000000")})}));
        const auto k = integer(hash({encoded(n.get()), encoded(g.get(), true)}));
        require(BN_mod_exp(v.get(), g.get(), x.get(), n.get(), ctx.get()) == 1);
        require(BN_mod_mul(kv.get(), k.get(), v.get(), n.get(), ctx.get()) == 1);
        require(BN_mod_add(b.get(), kv.get(), g.get(), n.get(), ctx.get()) == 1);
        public_key = encoded(b.get());
    }
    Bytes proof(const Bytes &client_public, const Bytes &client_proof) {
        std::unique_ptr<BN_CTX, decltype(&BN_CTX_free)> ctx(BN_CTX_new(), BN_CTX_free);
        const auto a = integer(client_public), b = integer(public_key);
        const auto u = integer(hash({encoded(a.get(), true), encoded(b.get(), true)}));
        auto vu = bn(), s = bn();
        require(ctx && vu && s);
        require(BN_mod_exp(vu.get(), v.get(), u.get(), n.get(), ctx.get()) == 1);
        require(BN_mod_mul(s.get(), a.get(), vu.get(), n.get(), ctx.get()) == 1);
        session_key = hash({encoded(s.get())});
        return hash({client_public, client_proof, session_key});
    }
};

enum class Phase { VerifyM2, VerifyM4, SetupM2, SetupM4, SetupM6, Normal };
enum class Form { Missing, Empty, Wrong, Multiple, Truncated, Error, ErrorTruncated,
                  InnerTruncated, MissingCiphertext, BadTag, MissingIdentifier,
                  EmptyIdentifier, MissingPublicKey, ShortPublicKey, MissingSignature,
                  ShortSignature, BadSignature, OtherSessionSignature, InfoAltIrk,
                  InfoMalformed, Normal };

const char *phase_name(Phase phase) {
    switch (phase) {
    case Phase::VerifyM2: return "verify-M2";
    case Phase::VerifyM4: return "verify-M4";
    case Phase::SetupM2: return "setup-M2";
    case Phase::SetupM4: return "setup-M4";
    case Phase::SetupM6: return "setup-M6";
    default: return "normal";
    }
}
const char *form_name(Form form) {
    switch (form) {
    case Form::Missing: return "missing State";
    case Form::Empty: return "empty State";
    case Form::Wrong: return "wrong State";
    case Form::Multiple: return "two-byte State";
    case Form::Truncated: return "truncated TLV";
    case Form::Error: return "explicit Error";
    case Form::ErrorTruncated: return "Error plus truncated TLV";
    case Form::InnerTruncated: return "authenticated truncated inner TLV";
    case Form::MissingCiphertext: return "missing ciphertext";
    case Form::BadTag: return "bad AEAD tag";
    case Form::MissingIdentifier: return "missing Identifier";
    case Form::EmptyIdentifier: return "empty Identifier";
    case Form::MissingPublicKey: return "missing PublicKey";
    case Form::ShortPublicKey: return "short PublicKey";
    case Form::MissingSignature: return "missing Signature";
    case Form::ShortSignature: return "short Signature";
    case Form::BadSignature: return "bad device signature";
    case Form::OtherSessionSignature: return "signature from another SRP session";
    case Form::InfoAltIrk: return "optional altIRK";
    case Form::InfoMalformed: return "malformed optional Info";
    default: return "normal";
    }
}

class MemStream final : public ByteStream {
public:
    Phase phase;
    Form form;
    int verify_m1 = 0, verify_m3 = 0, setup_m1 = 0, setup_m3 = 0, setup_m5 = 0;
    int notifications = 0, encrypted = 0;
    bool host_signature_valid = false;
    Bytes device_identifier = bytes_of("SYNTHETIC-DEVICE");
    Bytes device_public_key;
    explicit MemStream(Phase p, Form f) : phase(p), form(f) {}

    bool read_exact(void *data, size_t len, std::string &err) override {
        if (len > input_.size() - pos_) {
            err = "pair-setup test input exhausted";
            return false;
        }
        std::memcpy(data, input_.data() + pos_, len);
        pos_ += len;
        return true;
    }
    bool write_all(const void *data, size_t len, std::string &err) override {
        const std::string_view frame(static_cast<const char *>(data), len);
        require(frame.size() >= 11 && frame.substr(0, 9) == "RPPairing");
        const size_t body_size = (static_cast<uint8_t>(frame[9]) << 8) | static_cast<uint8_t>(frame[10]);
        require(body_size == frame.size() - 11);
        const auto envelope = scrctl::json::parse(frame.substr(11), &err);
        require(envelope.has_value());
        const auto &message = envelope->at("message");
        if (message.contains("streamEncrypted")) {
            ++encrypted;  // 无远程解锁响应；该可选步骤失败仍允许 setup 成功。
            return true;
        }
        const auto &inner = message.at("plain").at("_0");
        if (inner.contains("request")) {
            feed(j_obj({{"response", j_obj({{"_1", j_obj({{"handshake", j_obj({{"_0",
                j_obj({{"deviceOptions", j_obj({{"allowsPairSetup", j_bool(true)}})}})}})}})}})}}));
            return true;
        }
        const auto &event = inner.at("event").at("_0");
        if (event.contains("pairVerifyFailed")) {
            ++notifications;
            return true;
        }
        const auto &pairing = event.at("pairingData").at("_0");
        const auto raw = b64_decode(pairing.at("data").get<std::string>(), err);
        require(raw.has_value());
        const auto fields = tlv_parse(*raw, err);
        require(err.empty());
        const auto state = tlv_state(fields);
        const auto kind = pairing.at("kind").get<std::string>();
        if (kind == "verifyManualPairing") {
            if (state == 1) {
                ++verify_m1;
                const auto key = x25519_keypair(err);
                require(key.has_value());
                reply(Phase::VerifyM2, 2, {{TlvType::PublicKey, Bytes(key->pub.begin(), key->pub.end())}});
            } else {
                require(state == 3);
                ++verify_m3;
                reply(Phase::VerifyM4, 4, {});
            }
        } else if (state == 1) {
            ++setup_m1;
            if (phase == Phase::VerifyM2 || phase == Phase::VerifyM4) {
                // verify 被拒后的 setup M1 已到达；用显式错误结束夹具，避免另起配对流程。
                feed_data(tlv_build({{TlvType::Error, Bytes{0x02}}}));
            } else {
                srp_ = std::make_unique<SrpPeer>();
                reply(Phase::SetupM2, 2, {{TlvType::PublicKey, srp_->public_key}, {TlvType::Salt, srp_->salt}});
            }
        } else if (state == 3) {
            ++setup_m3;
            const auto *a = tlv_get(fields, TlvType::PublicKey);
            const auto *proof = tlv_get(fields, TlvType::Proof);
            require(srp_ && a && proof);
            reply(Phase::SetupM4, 4, {{TlvType::Proof, srp_->proof(*a, *proof)}});
        } else {
            require(state == 5);
            ++setup_m5;
            verify_m5(fields);
            reply(Phase::SetupM6, 6, {});
        }
        return true;
    }
private:
    Bytes input_;
    size_t pos_ = 0;
    std::unique_ptr<SrpPeer> srp_;
    void verify_m5(const std::map<uint8_t, Bytes> &fields) {
        require(srp_ != nullptr);
        std::string err;
        const auto key = hkdf_sha512(srp_->session_key, "Pair-Setup-Encrypt-Salt",
                                    "Pair-Setup-Encrypt-Info", 32, err);
        const auto *ciphertext = tlv_get(fields, TlvType::EncryptedData);
        require(key && ciphertext);
        static constexpr char nonce[] = "\x00\x00\x00\x00PS-Msg05";
        const auto plain = chacha_open(sv(*key), std::string_view(nonce, sizeof(nonce) - 1),
                                       *ciphertext, err);
        require(plain.has_value());
        const auto inner = tlv_parse(*plain, err);
        const auto *identifier = tlv_get(inner, TlvType::Identifier);
        const auto *public_key = tlv_get(inner, TlvType::PublicKey);
        const auto *signature = tlv_get(inner, TlvType::Signature);
        require(err.empty() && identifier && public_key && signature);
        const auto prefix = hkdf_sha512(srp_->session_key, "Pair-Setup-Controller-Sign-Salt",
                                        "Pair-Setup-Controller-Sign-Info", 32, err);
        require(prefix.has_value());
        Bytes message = *prefix;
        message.insert(message.end(), identifier->begin(), identifier->end());
        message.insert(message.end(), public_key->begin(), public_key->end());
        host_signature_valid = ed25519_verify(sv(*public_key), message, *signature, err);
        require(host_signature_valid);
    }
    Bytes signed_m6(Form selected) {
        require(srp_ != nullptr);
        std::string err;
        const auto peer = ed25519_keypair(err);
        require(peer.has_value());
        device_public_key.assign(peer->pub.begin(), peer->pub.end());
        Bytes signing_key = srp_->session_key;
        if (selected == Form::OtherSessionSignature) signing_key[0] ^= 1;
        const auto prefix = hkdf_sha512(signing_key, "Pair-Setup-Accessory-Sign-Salt",
                                        "Pair-Setup-Accessory-Sign-Info", 32, err);
        require(prefix.has_value());
        Bytes message = *prefix;
        message.insert(message.end(), device_identifier.begin(), device_identifier.end());
        message.insert(message.end(), device_public_key.begin(), device_public_key.end());
        auto signature = ed25519_sign(
            std::string_view(reinterpret_cast<const char *>(peer->seed.data()), peer->seed.size()),
            message, err);
        require(signature.has_value());
        if (selected == Form::BadSignature) (*signature)[0] ^= 1;
        if (selected == Form::ShortSignature) signature->pop_back();
        auto identifier = device_identifier;
        if (selected == Form::EmptyIdentifier) identifier.clear();
        auto public_key = device_public_key;
        if (selected == Form::ShortPublicKey) public_key.pop_back();
        std::vector<std::pair<TlvType, Bytes>> fields;
        if (selected != Form::MissingIdentifier) fields.emplace_back(TlvType::Identifier, identifier);
        if (selected != Form::MissingPublicKey) fields.emplace_back(TlvType::PublicKey, public_key);
        if (selected != Form::MissingSignature) fields.emplace_back(TlvType::Signature, *signature);
        if (selected == Form::InfoAltIrk) {
            OpackValue info;
            info.kind = OpackValue::Kind::kDict;
            info.dict = {{OpackValue::of_string("altIRK"), OpackValue::of_bytes(Bytes(16, 0x42))}};
            Bytes raw;
            require(opack_encode(info, raw, err));
            fields.emplace_back(TlvType::Info, raw);
        } else if (selected == Form::InfoMalformed) {
            fields.emplace_back(TlvType::Info, Bytes{0xff});
        }
        return selected == Form::InnerTruncated ? Bytes{0x11} : tlv_build(fields);
    }
    void feed(const scrctl::json::Value &inner) {
        const auto envelope = j_obj({{"originatedBy", j_str("device")}, {"sequenceNumber", j_int(0)},
            {"message", j_obj({{"plain", j_obj({{"_0", inner}})}})}});
        const auto text = scrctl::json::write(envelope);
        input_.insert(input_.end(), kRpPairingMagic.begin(), kRpPairingMagic.end());
        input_.push_back(static_cast<uint8_t>(text.size() >> 8));
        input_.push_back(static_cast<uint8_t>(text.size()));
        input_.insert(input_.end(), text.begin(), text.end());
    }
    void feed_data(const Bytes &raw) {
        feed(j_obj({{"event", j_obj({{"_0", j_obj({{"pairingData", j_obj({{"_0",
            j_obj({{"data", j_str(b64_encode(raw))}})}})}})}})}}));
    }
    void reply(Phase target, uint8_t expected, std::vector<std::pair<TlvType, Bytes>> fields) {
        const auto selected = target == phase ? form : Form::Normal;
        if (target == Phase::SetupM6 && selected != Form::MissingCiphertext) {
            require(srp_ != nullptr);
            std::string err;
            const auto key = hkdf_sha512(srp_->session_key, "Pair-Setup-Encrypt-Salt",
                                        "Pair-Setup-Encrypt-Info", 32, err);
            require(key.has_value());
            const Bytes inner = signed_m6(selected);
            static constexpr char nonce[] = "\x00\x00\x00\x00PS-Msg06";
            auto sealed = chacha_seal(sv(*key), std::string_view(nonce, sizeof(nonce) - 1), inner, err);
            require(sealed.has_value());
            if (selected == Form::BadTag) sealed->back() ^= 1;
            fields.emplace_back(TlvType::EncryptedData, *sealed);
        }
        if (selected == Form::Error || selected == Form::ErrorTruncated) {
            // 错误回复无成功 State；生产代码应进入现有拒绝分支。
            fields = {{TlvType::Error, Bytes{0x02}}};
        } else if (selected != Form::Missing) {
            Bytes state{expected};
            if (selected == Form::Empty) state.clear();
            if (selected == Form::Wrong) state[0] = static_cast<uint8_t>(expected + 1);
            if (selected == Form::Multiple) state.push_back(0);
            fields.emplace_back(TlvType::State, std::move(state));
        }
        auto raw = tlv_build(fields);
        if (selected == Form::Truncated || selected == Form::ErrorTruncated) raw.push_back(0x11);
        feed_data(raw);
    }
};

PairSetupResult run(MemStream &io, std::string &err) {
    FramedCarrier carrier(io);
    Rppairing channel(carrier);
    PairSetupOptions options;
    options.probe_verify_first = io.phase == Phase::VerifyM2 || io.phase == Phase::VerifyM4;
    return pair_setup(channel, "HOST-IDENTIFIER", "host.local", "UDID", nullptr, options, err);
}
}  // namespace

// 由现有 wifi_test 传入 check 回调，沿用其判据计数及失败退出码，不新增 CTest target。
void run_pair_setup_reply_tests(void (*check)(bool, const char *)) {
    try {
        for (const auto phase : {Phase::VerifyM2, Phase::VerifyM4, Phase::SetupM2, Phase::SetupM4, Phase::SetupM6}) {
            for (const auto form : {Form::Missing, Form::Empty, Form::Wrong, Form::Multiple, Form::Truncated}) {
                MemStream io(phase, form);
                std::string err;
                const auto result = run(io, err);
                const std::string label = std::string(phase_name(phase)) + " " + form_name(form);
                check(!result.ok && !err.empty(), (label + " 拒绝成功").c_str());
                check(form == Form::Truncated ? err.find("TLV") != std::string::npos : err.find("State") != std::string::npos,
                      (label + " 明确指出格式或阶段错误").c_str());
                const bool stopped = phase == Phase::VerifyM2 ? io.verify_m3 == 0 && io.setup_m1 == 0 :
                    phase == Phase::VerifyM4 ? io.setup_m1 == 0 :
                    phase == Phase::SetupM2 ? io.setup_m3 == 0 :
                    phase == Phase::SetupM4 ? io.setup_m5 == 0 : io.encrypted == 0;
                check(stopped && io.notifications == 0, (label + " 不推进下一阶段或通知配对拒绝").c_str());
            }
        }
        for (const auto phase : {Phase::VerifyM2, Phase::VerifyM4}) {
            for (const auto form : {Form::Error, Form::ErrorTruncated}) {
                MemStream io(phase, form);
                std::string err;
                const auto result = run(io, err);
                const bool error = form == Form::Error;
                check(!result.ok, "夹具在 setup M1 后显式结束，不声称完成完整设备配对");
                check(io.notifications == (error ? 1 : 0) && io.setup_m1 == (error ? 1 : 0),
                      (std::string(phase_name(phase)) + " " + form_name(form) + " 仅完整显式拒绝继续 setup M1").c_str());
                if (phase == Phase::VerifyM2) check(io.verify_m3 == 0, "verify M2 Error 不发送 PV-Msg03");
                check(error ? err.find("M2 returned error") != std::string::npos : err.find("TLV") != std::string::npos,
                      "verify 拒绝进入 setup，畸形 Error 回复保留解析失败原因");
            }
        }
        for (const auto phase : {Phase::SetupM2, Phase::SetupM4, Phase::SetupM6}) {
            MemStream io(phase, Form::Error);
            std::string err;
            const auto result = run(io, err);
            check(!result.ok && err.find("returned error") != std::string::npos && io.encrypted == 0,
                  (std::string(phase_name(phase)) + " 显式 Error 仍终止 setup").c_str());
        }
        for (const auto form : {Form::InnerTruncated, Form::MissingCiphertext, Form::BadTag,
                               Form::MissingIdentifier, Form::EmptyIdentifier, Form::MissingPublicKey,
                               Form::ShortPublicKey, Form::MissingSignature, Form::ShortSignature,
                               Form::BadSignature, Form::OtherSessionSignature}) {
            MemStream io(Phase::SetupM6, form);
            FramedCarrier carrier(io);
            Rppairing channel(carrier);
            PairSetupOptions options;
            options.probe_verify_first = false;
            std::string err;
            const auto result = pair_setup(channel, "HOST-IDENTIFIER", "host.local", "UDID",
                                           nullptr, options, err);
            const std::string label = std::string("M6 ") + form_name(form);
            check(!result.ok && !err.empty(), (label + " 必须失败").c_str());
            if (form == Form::InnerTruncated)
                check(err.find("TLV") != std::string::npos, "M6 截断明确指出 TLV 格式错误");
            check(result.record.host_private_key.empty() && !result.record.complete() &&
                  !result.record.has_peer_identity(), (label + " 不发布主机凭据或设备身份").c_str());
            std::string key_err;
            channel.encrypted_roundtrip(j_obj({}), key_err);
            check(io.encrypted == 0 && key_err.find("Main key not installed") != std::string::npos,
                  (label + " 不安装主密钥或发送解锁请求").c_str());
        }
        for (const auto form : {Form::Normal, Form::InfoAltIrk, Form::InfoMalformed}) {
            MemStream io(Phase::SetupM6, form);
            std::string err;
            const auto result = run(io, err);
            check(result.ok && result.record.complete() && result.record.has_peer_identity() &&
                  result.record.peer_identifier == io.device_identifier &&
                  result.record.peer_public_key == io.device_public_key,
                  "合法 M6 签名后保存设备原始标识和长期公钥");
            check(form == Form::InfoAltIrk ? result.record.peer_alt_irk == Bytes(16, 0x42) :
                      result.record.peer_alt_irk.empty(),
                  "Info/altIRK 保持可选，格式有效时才保存 altIRK");
        }
        {
            MemStream io(Phase::Normal, Form::Normal);
            std::string err;
            const auto result = run(io, err);
            check(result.ok && result.record.complete() && result.record.has_peer_identity(),
                  "合法 setup State 2/4/6、真实 SRP 证明与设备身份签名通过");
            check(io.setup_m1 == 1 && io.setup_m3 == 1 && io.setup_m5 == 1 && io.host_signature_valid &&
                  io.encrypted == 1, "正常 setup 验证双方身份签名后仍尝试可选解锁请求");
        }
        {
            MemStream io(Phase::VerifyM4, Form::Normal);
            std::string err;
            const auto result = run(io, err);
            check(!result.ok && err.find("already paired") != std::string::npos && io.setup_m1 == 0,
                  "合法非 Error verify M4 保持已配对结论，不继续 setup");
        }
    } catch (const std::exception &err) {
        check(false, err.what());
    }
}
