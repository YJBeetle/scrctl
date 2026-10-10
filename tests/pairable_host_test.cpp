#include "wifi/PairableHost.h"

#include <cstdio>
#include <exception>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "wifi/Opack.h"
#include "wifi/PairingIdentity.h"
#include "wifi/Srp.h"
#include "wifi/Tlv.h"

namespace {
using namespace scrctl::wifi;
using scrctl::json::Value;
using Items = std::vector<std::pair<TlvType, Bytes>>;
using Fields = std::map<uint8_t, Bytes>;
int checks = 0;
int failures = 0;

void check(bool ok, const char *label) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", label); }
}

Bytes unhex(std::string_view text) {
    Bytes result;
    for (size_t i = 0; i < text.size(); i += 2) {
        result.push_back(static_cast<uint8_t>(std::stoul(std::string(text.substr(i, 2)), nullptr, 16)));
    }
    return result;
}

PairableHostOptions options() {
    PairableHostOptions o;
    o.host_identifier = "HOST-NEW-IDENTITY";
    o.host_name = "Owned offline host";
    o.host_udid = "HOST-UDID";
    o.host_alt_irk = Bytes(16, 0x5a);
    // RFC8032 TEST1 seed/public, never a real host identity.
    o.host_private_key = unhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
    o.host_public_key = unhex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
    o.setup_pin = "314159";
    return o;
}

Value envelope(Value body, int sequence) {
    return {{"message", {{"plain", {{"_0", std::move(body)}}}}},
            {"originatedBy", "host"}, {"sequenceNumber", sequence}};
}

Value pairing(const Bytes &bytes, int sequence, bool fresh) {
    Value body = {{"event", {{"_0", {{"pairingData", {{"_0", {
        {"data", b64_encode(bytes)}, {"kind", "setupManualPairing"},
        {"startNewSession", fresh}}}}}}}}}};
    return envelope(std::move(body), sequence);
}

class Client final : public EnvelopeCarrier {
public:
    // The real existing SRP client supplies A/proof/K. This fixture does not
    // implement SRP, AEAD, signature verification, TLV or OPACK algorithms.
    explicit Client(std::string pin = "314159") : srp("Pair-Setup", std::move(pin), "123456789abcdef") {}
    SrpClient srp;
    std::vector<Value> responses;
    std::function<void(int, Value &)> mutate_envelope;
    std::function<void(Items &, OpackValue &)> mutate_identity;
    bool corrupt_aead = false;
    bool truncated_identity = false;
    bool trailing_info = false;
    bool fail_m6_write = false;
    bool authenticated_host = false;
    Bytes received_host_info;
    Bytes device_identifier = bytes_of("SIGNED-DEVICE-ACCOUNT-ID");
    Bytes device_public = options().host_public_key;
    int reads = 0;

    bool write_envelope(const Value &value, std::string &err) override {
        check(value.at("originatedBy") == "device" &&
                  value.at("sequenceNumber") == responses.size(),
              "responses use device role and contiguous own sequence");
        if (responses.size() == 3 && fail_m6_write) {
            err = "fixture M6 write failure";
            return false;
        }
        responses.push_back(value);
        if (responses.size() == 2) {
            const auto fields = response_fields(value);
            const auto *salt = tlv_get(fields, TlvType::Salt);
            const auto *public_key = tlv_get(fields, TlvType::PublicKey);
            if (salt == nullptr || public_key == nullptr || !srp.process(*salt, *public_key, err)) {
                throw std::runtime_error("Real SRP client could not process challenge");
            }
        } else if (responses.size() == 3) {
            const auto fields = response_fields(value);
            if (tlv_get(fields, TlvType::Error) == nullptr) {
                const auto *proof = tlv_get(fields, TlvType::Proof);
                check(proof != nullptr && srp.verify_server_proof(*proof),
                      "real SRP client verifies server mutual proof");
            }
        } else if (responses.size() == 4) {
            const auto fields = response_fields(value);
            const auto *sealed = tlv_get(fields, TlvType::EncryptedData);
            const auto key = hkdf_sha512(srp.session_key(), "Pair-Setup-Encrypt-Salt",
                                         "Pair-Setup-Encrypt-Info", 32, err);
            constexpr char nonce[] = "\x00\x00\x00\x00PS-Msg06";
            const auto plain = sealed && key ? chacha_open(sv(*key), std::string_view(nonce, sizeof(nonce) - 1), *sealed, err) : std::nullopt;
            const auto identity = plain ? authenticate_setup_identity(srp.session_key(), *plain, err) : std::nullopt;
            authenticated_host = identity && identity->identifier == bytes_of(options().host_identifier) &&
                                 identity->public_key == options().host_public_key;
            if (identity) received_host_info = identity->info;
        }
        return true;
    }

    std::optional<Value> read_envelope(std::string &err) override {
        Value value;
        if (reads == 0) {
            value = envelope({{"request", {{"_0", {{"handshake", {{"_0", {
                {"hostOptions", {{"attemptPairVerify", false}}}, {"wireProtocolVersion", 26}}}}}}}}}}, 0);
        } else if (reads == 1) {
            value = pairing(tlv_build({{TlvType::State, Bytes{1}}, {TlvType::Method, Bytes{0}}}), 1, true);
        } else if (reads == 2) {
            value = pairing(tlv_build({{TlvType::State, Bytes{3}}, {TlvType::PublicKey, srp.client_public()},
                                       {TlvType::Proof, srp.client_proof()}}), 2, false);
        } else if (reads == 3) {
            const auto prefix = hkdf_sha512(srp.session_key(), "Pair-Setup-Controller-Sign-Salt",
                                            "Pair-Setup-Controller-Sign-Info", 32, err);
            if (!prefix) throw std::runtime_error("Cannot derive client signature prefix");
            Bytes message = *prefix;
            message.insert(message.end(), device_identifier.begin(), device_identifier.end());
            message.insert(message.end(), device_public.begin(), device_public.end());
            const auto signature = ed25519_sign(sv(options().host_private_key), message, err);
            if (!signature) throw std::runtime_error("Cannot sign fixture M5");
            OpackValue info;
            info.kind = OpackValue::Kind::kDict;
            info.dict = {{OpackValue::of_string("accountID"), OpackValue::of_string(std::string(sv(device_identifier)))},
                         {OpackValue::of_string("remotepairing_udid"), OpackValue::of_string("OWNED-DEVICE-UDID")},
                         {OpackValue::of_string("altIRK"), OpackValue::of_bytes(Bytes(16, 0xa5))},
                         {OpackValue::of_string("model"), OpackValue::of_string("iPhone-fixture")},
                         {OpackValue::of_string("name"), OpackValue::of_string("Owned offline phone")}};
            Items fields{{TlvType::Identifier, device_identifier}, {TlvType::PublicKey, device_public},
                         {TlvType::Signature, *signature}};
            if (mutate_identity) mutate_identity(fields, info);
            Bytes encoded;
            if (!opack_encode(info, encoded, err)) throw std::runtime_error("Cannot encode fixture INFO");
            if (trailing_info) encoded.push_back(0x08);
            fields.emplace_back(TlvType::Info, std::move(encoded));
            const auto key = hkdf_sha512(srp.session_key(), "Pair-Setup-Encrypt-Salt",
                                         "Pair-Setup-Encrypt-Info", 32, err);
            constexpr char nonce[] = "\x00\x00\x00\x00PS-Msg05";
            auto plain = tlv_build(fields);
            if (truncated_identity) plain.push_back(static_cast<uint8_t>(TlvType::Identifier));
            auto sealed = key ? chacha_seal(sv(*key), std::string_view(nonce, sizeof(nonce) - 1), plain, err) : std::nullopt;
            if (!sealed) throw std::runtime_error("Cannot encrypt fixture M5");
            if (corrupt_aead) sealed->back() ^= 1;
            value = pairing(tlv_build({{TlvType::State, Bytes{5}}, {TlvType::EncryptedData, *sealed}}), 3, false);
        } else {
            err = "unexpected fixture read";
            return std::nullopt;
        }
        if (mutate_envelope) mutate_envelope(reads, value);
        ++reads;
        return value;
    }
    bool wait_readable(int, std::string &) override { return true; }

    static Fields response_fields(const Value &value) {
        std::string err;
        const auto raw = b64_decode(value.at("message").at("plain").at("_0").at("event").at("_0")
                                    .at("pairingData").at("_0").at("data").get<std::string>(), err);
        if (!raw) throw std::runtime_error("Invalid fixture response base64");
        const auto fields = tlv_parse(*raw, err);
        if (!err.empty()) throw std::runtime_error("Invalid fixture response TLV");
        return fields;
    }
};

Value &handshake(Value &value) { return value["message"]["plain"]["_0"]["request"]["_0"]["handshake"]["_0"]; }
Value &data(Value &value) { return value["message"]["plain"]["_0"]["event"]["_0"]["pairingData"]["_0"]; }

void reject(Client &client, const char *label) {
    std::string err;
    const auto result = accept_pairable_host(client, options(), [](std::string_view, std::string &) { return true; }, err);
    check(!result && !err.empty() && !client.authenticated_host && client.responses.size() <= 3, label);
}

void test_complete_crypto_exchange() {
    Client client;
    // Raw signed Identifier is independent of UDID; retain its exact bytes.
    client.device_identifier = Bytes{'I', 'D', 0, 'R', 'A', 'W'};
    std::string err = "stale failure";
    int pins = 0;
    const auto result = accept_pairable_host(client, options(), [&](std::string_view pin, std::string &) {
        ++pins; check(pin == options().setup_pin, "callback displays this attempt's six-digit PIN"); return true;
    }, err);
    check(result && err.empty() && pins == 1 && client.reads == 4 && client.responses.size() == 4,
          "complete device-initiated exchange succeeds exactly once");
    check(client.authenticated_host, "client authenticates host Accessory-Sign M6 with production validator");
    if (result) {
        check(result->record.complete() && result->record.has_peer_identity() &&
                  result->record.udid == "OWNED-DEVICE-UDID" &&
                  result->record.peer_identifier == client.device_identifier &&
                  result->record.peer_public_key == client.device_public &&
                  result->record.peer_alt_irk == Bytes(16, 0xa5) &&
                  result->record.host_private_key == options().host_private_key &&
                  result->record.host_identifier == options().host_identifier &&
                  result->record.remote_unlock_host_key.empty(),
              "result binds signed peer identity, distinct UDID and altIRK without an unlock key");
        check(result->peer_name == "Owned offline phone" && result->peer_model == "iPhone-fixture",
              "only authenticated INFO provides peer display metadata");
    }
    const auto &reply = client.responses.at(0).at("message").at("plain").at("_0").at("response").at("_1").at("handshake").at("_0");
    check(reply.at("deviceOptions").at("allowsPairSetup") == true &&
              reply.at("deviceOptions").at("allowsPinlessPairing") == false &&
              reply.at("deviceOptions").at("allowsIncomingTunnelConnections") == false &&
              reply.at("wireProtocolVersion") == 26,
          "handshake advertises PIN pairing and no inbound tunnel capability");
    OpackValue info;
    check(opack_decode(client.received_host_info, info, err) && info.find("altIRK") != nullptr &&
              info.find("altIRK")->bytes == options().host_alt_irk,
          "M6 carries the same stable advertised host altIRK");
}

void test_envelope_and_phase_rejections() {
    const std::vector<std::pair<const char *, std::function<void(int, Value &)>>> cases{
        {"reject wrong protocol origin", [](int n, Value &v) { if (n == 0) v["originatedBy"] = "device"; }},
        {"reject nonconsecutive sequence", [](int n, Value &v) { if (n == 2) v["sequenceNumber"] = 1; }},
        {"reject noninteger sequence", [](int n, Value &v) { if (n == 0) v["sequenceNumber"] = true; }},
        {"reject encrypted initial envelope", [](int n, Value &v) { if (n == 0) v["message"] = {{"streamEncrypted", {{"_0", ""}}}}; }},
        {"reject pair-verify initial request", [](int n, Value &v) { if (n == 0) handshake(v)["hostOptions"]["attemptPairVerify"] = true; }},
        {"reject unsupported wire version", [](int n, Value &v) { if (n == 0) handshake(v)["wireProtocolVersion"] = 7; }},
        {"reject incompatible minimum version", [](int n, Value &v) { if (n == 0) { handshake(v)["wireProtocolVersion"] = 30; handshake(v)["minimumSupportedWireProtocolVersion"] = 27; } }},
        {"reject boolean wire version", [](int n, Value &v) { if (n == 0) handshake(v)["wireProtocolVersion"] = true; }},
        {"reject wrong pairing kind", [](int n, Value &v) { if (n == 1) data(v)["kind"] = "verifyManualPairing"; }},
        {"reject renewed M3 session", [](int n, Value &v) { if (n == 2) data(v)["startNewSession"] = true; }},
        {"reject bad base64", [](int n, Value &v) { if (n == 1) data(v)["data"] = "!"; }},
        {"reject wrong M1 method", [](int n, Value &v) { if (n == 1) data(v)["data"] = b64_encode(tlv_build({{TlvType::State, Bytes{1}}, {TlvType::Method, Bytes{1}}})); }},
        {"reject truncated TLV", [](int n, Value &v) { if (n == 1) data(v)["data"] = b64_encode(Bytes{6, 2, 1}); }},
        {"reject repeated state", [](int n, Value &v) { if (n == 1) data(v)["data"] = b64_encode(tlv_build({{TlvType::State, Bytes{1}}, {TlvType::Method, Bytes{0}}, {TlvType::State, Bytes{1}}})); }},
        {"reject wrong phase", [](int n, Value &v) { if (n == 1) data(v)["data"] = b64_encode(tlv_build({{TlvType::State, Bytes{3}}, {TlvType::Method, Bytes{0}}})); }}
    };
    for (const auto &[label, mutation] : cases) { Client client; client.mutate_envelope = mutation; reject(client, label); }
    Client wrong_pin("271828"); reject(wrong_pin, "real wrong-PIN SRP proof never publishes a record");
    Client damaged; damaged.corrupt_aead = true; reject(damaged, "damaged M5 AEAD never publishes a record");
    Client failed_write; failed_write.fail_m6_write = true; reject(failed_write, "M6 write failure never publishes a record");
}

void test_identity_rejections() {
    const std::vector<std::pair<const char *, std::function<void(Items &, OpackValue &)>>> cases{
        {"reject bad device signature", [](Items &f, OpackValue &) { f[2].second[0] ^= 1; }},
        {"reject changed signed Identifier", [](Items &f, OpackValue &) { f[0].second[0] ^= 1; }},
        {"reject changed device public key", [](Items &f, OpackValue &) { f[1].second[0] ^= 1; }},
        {"reject missing identity signature", [](Items &f, OpackValue &) { f.pop_back(); }},
        {"reject repeated identity Identifier", [](Items &f, OpackValue &) { f.push_back(f.front()); }},
        {"reject missing UDID without guessing accountID", [](Items &, OpackValue &i) { i.dict.erase(i.dict.begin() + 1); }},
        {"reject non-16-byte altIRK", [](Items &, OpackValue &i) { i.dict[2].second.bytes.pop_back(); }},
        {"reject duplicate INFO identity key", [](Items &, OpackValue &i) { i.dict.push_back(i.dict.front()); }},
        {"reject INFO account contradicting signed Identifier", [](Items &, OpackValue &i) { i.dict[0].second.str = "UNSIGNED-OTHER-ID"; }}
    };
    for (const auto &[label, mutation] : cases) { Client client; client.mutate_identity = mutation; reject(client, label); }
    Client truncated; truncated.truncated_identity = true;
    reject(truncated, "authenticated ciphertext with truncated inner identity TLV is rejected");
    Client trailing; trailing.trailing_info = true;
    reject(trailing, "authenticated identity with trailing OPACK bytes is rejected");
    Client wrong_role;
    wrong_role.mutate_identity = [&](Items &fields, OpackValue &) {
        std::string err;
        const auto prefix = hkdf_sha512(wrong_role.srp.session_key(), "Pair-Setup-Accessory-Sign-Salt",
                                         "Pair-Setup-Accessory-Sign-Info", 32, err);
        if (!prefix) throw std::runtime_error("Cannot derive wrong-role fixture prefix");
        Bytes message = *prefix;
        message.insert(message.end(), fields[0].second.begin(), fields[0].second.end());
        message.insert(message.end(), fields[1].second.begin(), fields[1].second.end());
        const auto signature = ed25519_sign(sv(options().host_private_key), message, err);
        if (!signature) throw std::runtime_error("Cannot sign wrong-role fixture identity");
        fields[2].second = *signature;
    };
    reject(wrong_role, "valid Ed25519 signature with Accessory role cannot authenticate phone M5");
}

void test_local_preconditions_and_cancel() {
    for (const int which : {0, 1, 2, 3}) {
        auto o = options();
        if (which == 0) o.setup_pin = "00000";
        if (which == 1) o.host_alt_irk.pop_back();
        if (which == 2) o.host_public_key[0] ^= 1;
        if (which == 3) o.minimum_wire_protocol_version = 27;
        Client client; std::string err;
        check(!accept_pairable_host(client, o, [](std::string_view, std::string &) { return true; }, err) &&
                  client.reads == 0 && client.responses.empty() && !err.empty(),
              "invalid local config sends no handshake or challenge");
    }
    Client client; std::string err;
    const auto result = accept_pairable_host(client, options(), [](std::string_view, std::string &error) {
        error = "owned callback cancelled"; return false;
    }, err);
    check(!result && err == "owned callback cancelled" && client.responses.size() == 1,
          "PIN callback cancellation preserves first error and sends no SRP challenge");
}
}  // namespace

int main() {
    try {
        test_complete_crypto_exchange();
        test_envelope_and_phase_rejections();
        test_identity_rejections();
        test_local_preconditions_and_cancel();
    } catch (const std::exception &e) {
        ++failures; std::printf("Fixture exception: %s\n", e.what());
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
