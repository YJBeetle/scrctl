// 经真实 RPPairing 字节流测试 setup/verify 探测的阶段与错误分支，不需要设备。
#include "wifi/PairSetup.h"
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
                  InnerTruncated, InnerOptional, Normal };

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
    case Form::InnerOptional: return "authenticated optional inner TLV";
    default: return "normal";
    }
}

class MemStream final : public ByteStream {
public:
    Phase phase;
    Form form;
    int verify_m1 = 0, verify_m3 = 0, setup_m1 = 0, setup_m3 = 0, setup_m5 = 0;
    int notifications = 0, encrypted = 0;
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
            // M6 身份字段的要求不属于本次修复；合法 State、无密文保持现有成功语义。
            reply(Phase::SetupM6, 6, {});
        }
        return true;
    }
private:
    Bytes input_;
    size_t pos_ = 0;
    std::unique_ptr<SrpPeer> srp_;
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
        if (selected == Form::InnerTruncated || selected == Form::InnerOptional) {
            require(srp_ && target == Phase::SetupM6);
            std::string err;
            const auto key = hkdf_sha512(srp_->session_key, "Pair-Setup-Encrypt-Salt",
                                        "Pair-Setup-Encrypt-Info", 32, err);
            require(key.has_value());
            const Bytes inner = selected == Form::InnerTruncated ? Bytes{0x11} :
                tlv_build({{TlvType::Identifier, bytes_of("DEVICE")}});
            static constexpr char nonce[] = "\x00\x00\x00\x00PS-Msg06";
            const auto sealed = chacha_seal(sv(*key), std::string_view(nonce, sizeof(nonce) - 1), inner, err);
            require(sealed.has_value());
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
        {
            MemStream io(Phase::SetupM6, Form::InnerTruncated);
            FramedCarrier carrier(io);
            Rppairing channel(carrier);
            PairSetupOptions options;
            options.probe_verify_first = false;
            std::string err;
            const auto result = pair_setup(channel, "HOST-IDENTIFIER", "host.local", "UDID",
                                           nullptr, options, err);
            check(!result.ok && err.find("M6") != std::string::npos && err.find("TLV") != std::string::npos,
                  "已认证解密的 M6 内层 TLV 截断仍必须失败");
            check(result.record.host_private_key.empty() && !result.record.complete(),
                  "M6 内层 TLV 失败不发布配对记录");
            std::string key_err;
            channel.encrypted_roundtrip(j_obj({}), key_err);
            check(io.encrypted == 0 && key_err.find("Main key not installed") != std::string::npos,
                  "M6 内层 TLV 失败不安装主密钥或发送解锁请求");
        }
        {
            MemStream io(Phase::SetupM6, Form::InnerOptional);
            std::string err;
            const auto result = run(io, err);
            check(result.ok && result.record.complete() && result.record.peer_alt_irk.empty(),
                  "合法已认证内层 TLV 不含 altIRK 时仍允许配对成功");
        }
        {
            MemStream io(Phase::Normal, Form::Normal);
            std::string err;
            const auto result = run(io, err);
            check(result.ok && result.record.complete(), "合法 setup State 2/4/6 与真实 SRP 证明保持成功");
            check(io.setup_m1 == 1 && io.setup_m3 == 1 && io.setup_m5 == 1 && io.encrypted == 1,
                  "正常 setup 不新增 M6 身份字段要求，仍尝试可选解锁请求");
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
