// 设备身份内层 TLV 与签名的离线测试。AEAD、载体和记录文件由其他测试覆盖。
#include "wifi/PairingIdentity.h"
#include "wifi/Tlv.h"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using scrctl::wifi::Bytes;
using scrctl::wifi::TlvType;
using Items = std::vector<std::pair<TlvType, Bytes>>;

int checks = 0;
int failures = 0;

void check(bool ok, const std::string &label) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", label.c_str());
    }
}

Bytes unhex(std::string_view text) {
    Bytes out;
    for (size_t i = 0; i < text.size(); i += 2) {
        out.push_back(static_cast<uint8_t>(std::stoul(std::string(text.substr(i, 2)), nullptr, 16)));
    }
    return out;
}

Bytes signed_message(const Bytes &seed, const Bytes &message) {
    std::string err;
    auto signature = scrctl::wifi::ed25519_sign(scrctl::wifi::sv(seed), message, err);
    if (!signature) throw std::runtime_error("Cannot construct signature fixture: " + err);
    return *signature;
}

struct Fixture {
    // RFC 8032 §7.1 TEST 1 的 seed/public；算法向量在 wifi_test 中单独校验。
    Bytes seed = unhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
    Bytes public_key = unhex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
    Bytes session_key = Bytes(64, 0x42);
    Bytes identifier{'D', 0, '\r', '\n', '=', 0xff};
    Bytes info{0, 0xff, 0x11, 0x22};
    Bytes peer_ephemeral = Bytes(32, 0xa1);
    Bytes host_ephemeral = Bytes(32, 0xb2);

    Items setup_fields() const {
        std::string err;
        const auto prefix = scrctl::wifi::hkdf_sha512(
            session_key, "Pair-Setup-Accessory-Sign-Salt", "Pair-Setup-Accessory-Sign-Info", 32, err);
        if (!prefix) throw std::runtime_error("Cannot construct M6 fixture: " + err);
        Bytes message = *prefix;
        message.insert(message.end(), identifier.begin(), identifier.end());
        message.insert(message.end(), public_key.begin(), public_key.end());
        return {{TlvType::Identifier, identifier}, {TlvType::PublicKey, public_key},
                {TlvType::Signature, signed_message(seed, message)}, {TlvType::Info, info}};
    }

    Items verify_fields(bool include_public_key = false) const {
        Bytes message = peer_ephemeral;
        message.insert(message.end(), identifier.begin(), identifier.end());
        message.insert(message.end(), host_ephemeral.begin(), host_ephemeral.end());
        Items fields{{TlvType::Identifier, identifier},
                     {TlvType::Signature, signed_message(seed, message)}};
        if (include_public_key) fields.emplace_back(TlvType::PublicKey, public_key);
        return fields;
    }
};

Bytes &value(Items &items, TlvType type) {
    for (auto &[item_type, bytes] : items) {
        if (item_type == type) return bytes;
    }
    throw std::runtime_error("Missing fixture field");
}

void omit(Items &items, TlvType type) {
    std::erase_if(items, [type](const auto &item) { return item.first == type; });
}

bool verify(const Fixture &trusted, const Bytes &plain, std::string &err) {
    return scrctl::wifi::authenticate_verify_identity(
        trusted.identifier, trusted.public_key, trusted.peer_ephemeral,
        trusted.host_ephemeral, plain, err);
}

void reject_setup(const Fixture &fixture, const Bytes &plain, const std::string &label,
                  std::string_view detail = {}) {
    std::string err = "stale error";
    const auto identity = scrctl::wifi::authenticate_setup_identity(fixture.session_key, plain, err);
    check(!identity && err.find("PS-Msg06") != std::string::npos &&
              (detail.empty() || err.find(detail) != std::string::npos), label);
}

void reject_verify(const Fixture &trusted, const Bytes &plain, const std::string &label,
                   std::string_view detail = {}) {
    std::string err = "stale error";
    check(!verify(trusted, plain, err) && err.find("PV-Msg02") != std::string::npos &&
              (detail.empty() || err.find(detail) != std::string::npos), label);
}

void test_valid_identity() {
    const Fixture fixture;
    std::string err = "previous failure";
    const auto setup = scrctl::wifi::authenticate_setup_identity(
        fixture.session_key, scrctl::wifi::tlv_build(fixture.setup_fields()), err);
    check(setup && setup->identifier == fixture.identifier &&
              setup->public_key == fixture.public_key && setup->info == fixture.info && err.empty(),
          "M6 authenticates and preserves raw Identifier, public key and Info");

    auto fields = fixture.setup_fields();
    omit(fields, TlvType::Info);
    err = "previous failure";
    const auto no_info = scrctl::wifi::authenticate_setup_identity(
        fixture.session_key, scrctl::wifi::tlv_build(fields), err);
    check(no_info && no_info->info.empty() && err.empty(), "M6 permits missing optional Info");

    // Info 和未知字段沿用现有拼接规则，不套用关键身份字段的重复限制。
    fields = fixture.setup_fields();
    omit(fields, TlvType::Info);
    fields.emplace_back(TlvType::Info, Bytes{0, 0xff});
    fields.emplace_back(static_cast<TlvType>(0x70), Bytes{1});
    fields.emplace_back(TlvType::Info, Bytes{0x11, 0x22});
    fields.emplace_back(static_cast<TlvType>(0x70), Bytes{2});
    const auto repeated_info = scrctl::wifi::authenticate_setup_identity(
        fixture.session_key, scrctl::wifi::tlv_build(fields), err);
    check(repeated_info && repeated_info->info == fixture.info,
          "M6 preserves existing Info and unknown-field repetition semantics");

    for (const bool include_key : {false, true}) {
        err = "previous failure";
        check(verify(fixture, scrctl::wifi::tlv_build(fixture.verify_fields(include_key)), err) && err.empty(),
              include_key ? "M2 accepts the pinned optional public key and clears error"
                          : "M2 authenticates without an optional public key and clears error");
    }
}

void test_signature_binding() {
    const Fixture fixture;
    const auto setup = fixture.setup_fields();
    const auto verify_fields = fixture.verify_fields();
    for (const size_t index : {size_t{0}, size_t{63}}) {
        auto fields = setup;
        value(fields, TlvType::Signature)[index] ^= 1;
        reject_setup(fixture, scrctl::wifi::tlv_build(fields), "M6 rejects changed signature", "signature");
        fields = verify_fields;
        value(fields, TlvType::Signature)[index] ^= 1;
        reject_verify(fixture, scrctl::wifi::tlv_build(fields), "M2 rejects changed signature", "signature");
    }
    for (const auto type : {TlvType::Identifier, TlvType::PublicKey}) {
        auto fields = setup;
        value(fields, type)[0] ^= 1;
        reject_setup(fixture, scrctl::wifi::tlv_build(fields), "M6 signature binds Identifier and public key", "signature");
    }
    auto changed = fixture;
    changed.session_key[0] ^= 1;
    reject_setup(changed, scrctl::wifi::tlv_build(setup), "M6 signature binds the SRP session", "signature");

    const Bytes plain = scrctl::wifi::tlv_build(verify_fields);
    changed = fixture;
    changed.identifier[0] ^= 1;
    reject_verify(changed, plain, "M2 rejects a different saved Identifier", "Identifier");
    changed = fixture;
    changed.public_key = unhex("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c");
    reject_verify(changed, plain, "M2 rejects a different pinned long-term key", "signature");
    changed = fixture;
    std::swap(changed.peer_ephemeral, changed.host_ephemeral);
    reject_verify(changed, plain, "M2 rejects reversed ephemeral-key order", "signature");
    changed = fixture;
    changed.host_ephemeral[0] ^= 1;
    reject_verify(changed, plain, "M2 rejects replay into another host ephemeral key", "signature");
    changed = fixture;
    changed.peer_ephemeral[0] ^= 1;
    reject_verify(changed, plain, "M2 rejects another peer ephemeral key", "signature");

    auto fields = fixture.verify_fields(true);
    value(fields, TlvType::PublicKey)[0] ^= 1;
    reject_verify(fixture, scrctl::wifi::tlv_build(fields), "M2 rejects a different optional public key", "public key");
}

void test_fields_and_lengths() {
    const Fixture fixture;
    for (const auto type : {TlvType::Identifier, TlvType::PublicKey, TlvType::Signature}) {
        auto fields = fixture.setup_fields();
        omit(fields, type);
        reject_setup(fixture, scrctl::wifi::tlv_build(fields), "M6 rejects a missing required identity field");
    }
    for (const auto type : {TlvType::Identifier, TlvType::Signature}) {
        auto fields = fixture.verify_fields();
        omit(fields, type);
        reject_verify(fixture, scrctl::wifi::tlv_build(fields), "M2 rejects a missing required identity field");
    }
    auto fields = fixture.setup_fields();
    value(fields, TlvType::Identifier).clear();
    reject_setup(fixture, scrctl::wifi::tlv_build(fields), "M6 rejects an empty Identifier");
    fields = fixture.verify_fields();
    value(fields, TlvType::Identifier).clear();
    reject_verify(fixture, scrctl::wifi::tlv_build(fields), "M2 rejects an empty Identifier");

    for (const size_t length : {size_t{0}, size_t{31}, size_t{33}}) {
        fields = fixture.setup_fields();
        value(fields, TlvType::PublicKey).resize(length);
        reject_setup(fixture, scrctl::wifi::tlv_build(fields), "M6 rejects wrong public-key length");
        fields = fixture.verify_fields(true);
        value(fields, TlvType::PublicKey).resize(length);
        reject_verify(fixture, scrctl::wifi::tlv_build(fields), "M2 rejects wrong optional public-key length");
        auto changed = fixture;
        changed.public_key.resize(length);
        reject_verify(changed, scrctl::wifi::tlv_build(fixture.verify_fields()), "M2 rejects wrong pinned key length");
        changed = fixture;
        changed.peer_ephemeral.resize(length);
        reject_verify(changed, scrctl::wifi::tlv_build(fixture.verify_fields()), "M2 rejects wrong peer ephemeral length");
        changed = fixture;
        changed.host_ephemeral.resize(length);
        reject_verify(changed, scrctl::wifi::tlv_build(fixture.verify_fields()), "M2 rejects wrong host ephemeral length");
    }
    for (const size_t length : {size_t{0}, size_t{63}, size_t{65}}) {
        fields = fixture.setup_fields();
        value(fields, TlvType::Signature).resize(length);
        reject_setup(fixture, scrctl::wifi::tlv_build(fields), "M6 rejects wrong signature length");
        fields = fixture.verify_fields();
        value(fields, TlvType::Signature).resize(length);
        reject_verify(fixture, scrctl::wifi::tlv_build(fields), "M2 rejects wrong signature length");
        auto changed = fixture;
        changed.session_key.resize(length);
        reject_setup(changed, scrctl::wifi::tlv_build(fixture.setup_fields()), "M6 rejects wrong SRP session-key length");
    }
    auto changed = fixture;
    changed.identifier.clear();
    reject_verify(changed, scrctl::wifi::tlv_build(fixture.verify_fields()), "M2 rejects missing saved device Identifier");
}

void test_tlv_integrity() {
    const Fixture fixture;
    std::vector<Bytes> malformed{{}, {0x01}, {0x01, 0x02, 'X'}};
    auto setup = scrctl::wifi::tlv_build(fixture.setup_fields());
    setup.push_back(0x01);
    malformed.push_back(setup);
    for (const auto &plain : malformed) {
        reject_setup(fixture, plain, "M6 rejects empty or truncated identity TLV");
        reject_verify(fixture, plain, "M2 rejects empty or truncated identity TLV");
    }

    for (const auto type : {TlvType::Identifier, TlvType::PublicKey, TlvType::Signature}) {
        for (const bool setup_stage : {true, false}) {
            auto fields = setup_stage ? fixture.setup_fields() : fixture.verify_fields(true);
            const Bytes original = value(fields, type);
            fields.emplace_back(type, original);
            const auto reject = [&](const Items &items, const char *label) {
                const Bytes plain = scrctl::wifi::tlv_build(items);
                if (setup_stage) reject_setup(fixture, plain, label, "continuation");
                else reject_verify(fixture, plain, label, "continuation");
            };
            reject(fields, "Duplicate critical identity TLV is rejected");
            fields = setup_stage ? fixture.setup_fields() : fixture.verify_fields(true);
            omit(fields, type);
            const auto middle = original.begin() + static_cast<Bytes::difference_type>(original.size() / 2);
            fields.emplace_back(type, Bytes(original.begin(), middle));
            fields.emplace_back(type, Bytes(middle, original.end()));
            reject(fields, "Short consecutive critical fragments cannot be silently concatenated");
        }
    }
}

void test_identifier_fragments() {
    for (const size_t length : {size_t{255}, size_t{600}}) {
        Fixture fixture;
        fixture.identifier.resize(length);
        for (size_t i = 0; i < length; ++i) fixture.identifier[i] = static_cast<uint8_t>(i);
        std::string err = "previous failure";
        const auto setup = scrctl::wifi::authenticate_setup_identity(
            fixture.session_key, scrctl::wifi::tlv_build(fixture.setup_fields()), err);
        check(setup && setup->identifier == fixture.identifier && err.empty(),
              "M6 accepts a full 255-byte chunk and valid consecutive identifier fragments");
        err = "previous failure";
        check(verify(fixture, scrctl::wifi::tlv_build(fixture.verify_fields()), err) && err.empty(),
              "M2 accepts a full 255-byte chunk and valid consecutive identifier fragments");
        if (length == 255) {
            for (const bool setup_stage : {true, false}) {
                auto fields = setup_stage ? fixture.setup_fields() : fixture.verify_fields();
                fields.insert(fields.begin() + 1, {TlvType::Identifier, {}});
                const Bytes plain = scrctl::wifi::tlv_build(fields);
                if (setup_stage) reject_setup(fixture, plain, "M6 rejects an empty continuation", "continuation");
                else reject_verify(fixture, plain, "M2 rejects an empty continuation", "continuation");
            }
            continue;
        }
        for (const bool setup_stage : {true, false}) {
            auto fields = setup_stage ? fixture.setup_fields() : fixture.verify_fields();
            omit(fields, TlvType::Identifier);
            const auto middle = fixture.identifier.begin() + 255;
            fields.emplace_back(TlvType::Identifier, Bytes(fixture.identifier.begin(), middle));
            fields.emplace_back(static_cast<TlvType>(0x70), Bytes{1});
            fields.emplace_back(TlvType::Identifier, Bytes(middle, fixture.identifier.end()));
            const Bytes plain = scrctl::wifi::tlv_build(fields);
            if (setup_stage) reject_setup(fixture, plain, "M6 rejects separated identifier fragments", "continuation");
            else reject_verify(fixture, plain, "M2 rejects separated identifier fragments", "continuation");
        }
    }
}

}  // namespace

int main() {
    try {
        test_valid_identity();
        test_signature_binding();
        test_fields_and_lengths();
        test_tlv_integrity();
        test_identifier_fragments();
    } catch (const std::exception &err) {
        check(false, std::string("Fixture construction failed: ") + err.what());
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
