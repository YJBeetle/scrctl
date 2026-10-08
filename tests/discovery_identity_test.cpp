#include "wifi/DiscoveryIdentity.h"

#include <array>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/evp.h>
#include <openssl/opensslv.h>

namespace {
using namespace scrctl::wifi;
int failures = 0;
int checks = 0;

void check(bool ok, const char *message) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", message);
    }
}

Bytes test_key() {
    Bytes key(16);
    for (size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<uint8_t>(i);
    }
    return key;
}

void siphash_vectors() {
    // 算法发布者的 64 位向量：key=00..0f，message=00..(length-1)。
    // 选择空输入、块边界前后与多块输入，期望值写为数值，再逐字节检查小端输出。
    // https://github.com/veorq/SipHash/blob/master/vectors.h (vectors_sip64)
    struct Vector {
        size_t length;
        uint64_t result;
    };
    static constexpr std::array<Vector, 9> vectors = {{
        {0, 0x726fdb47dd0e0e31ULL},
        {1, 0x74f839c593dc67fdULL},
        {7, 0xab0200f58b01d137ULL},
        {8, 0x93f5f5799a932462ULL},
        {15, 0xa129ca6149be45e5ULL},
        {16, 0x3f2acc7f57c29bdbULL},
        {31, 0x32d892fad841c342ULL},
        {32, 0x7127512f72f27cceULL},
        {63, 0x958a324ceb064572ULL},
    }};
    const auto key = test_key();
    Bytes message(63);
    for (size_t i = 0; i < message.size(); ++i) {
        message[i] = static_cast<uint8_t>(i);
    }
    for (const auto &vector : vectors) {
        std::string error = "previous error";
        const auto result = siphash24(sv(key), sv(message).substr(0, vector.length), error);
        check(result.has_value() && error.empty(), "SipHash vector computes and clears error");
        if (result) {
            bool same = true;
            for (size_t i = 0; i < result->size(); ++i) {
                same = same && (*result)[i] == ((vector.result >> (8 * i)) & 0xff);
            }
            check(same, "SipHash-2-4 matches published 64-bit little-endian vector");
        }
    }
    for (const size_t length : {size_t{0}, size_t{15}, size_t{17}}) {
        std::string error;
        const std::string wrong_key(length, 'x');
        check(!siphash24(wrong_key, "message", error) && !error.empty(),
              "SipHash rejects wrong key length");
    }
}

void advertisement_matching() {
    // 仅使用公开的递增测试密钥；这里不使用真实配对记录或设备标识。
    // OpenSSL CLI 对拍：原始 8 字节为 35 68 34 32 d6 a5 e1 dc，前六字节
    // 反转后为 a5 d6 32 34 68 35，标准 Base64 为 pdYyNGg1。
    // 协议字节规则依据：pymobiledevice3 官方 misc/RemoteXPC.md 的 authTag 段。
    // https://github.com/doronz88/pymobiledevice3/blob/master/misc/RemoteXPC.md
    static constexpr std::string_view identifier = "00000000-0000-0000-0000-000000000000";
    static constexpr std::string_view tag = "pdYyNGg1";
    PairRecord paired;
    paired.udid = "synthetic-device";
    paired.host_identifier = "synthetic-host";
    paired.host_private_key.assign(32, 1);
    paired.host_public_key.assign(32, 2);
    paired.peer_identifier = bytes_of("synthetic-peer");
    paired.peer_public_key.assign(32, 3);
    paired.advertised_identifier = "old-advertisement";
    paired.peer_alt_irk = test_key();

    auto result = match_advertisement(identifier, tag, {paired});
    check(result.status == DiscoveryIdentityStatus::matched && result.record_index == 0 &&
              result.auth_tag_matches == std::vector<size_t>{0} && result.error.empty(),
          "six-byte reverse and Base64 match a rotated identifier");
    check(result.identifier_hints.empty(), "tag match does not require cached identifier equality");

    paired.advertised_identifier = identifier;
    result = match_advertisement(identifier, "NWg0Mtal", {paired});
    check(result.status == DiscoveryIdentityStatus::unmatched && !result.record_index &&
              result.auth_tag_matches.empty() && result.identifier_hints == std::vector<size_t>{0},
          "unreversed tag never falls back to cached identifier");
    result = match_advertisement(identifier, "AAAAAAAA", {paired});
    check(result.status == DiscoveryIdentityStatus::unmatched && !result.record_index,
          "valid but incorrect tag does not select a matching identifier");
    result = match_advertisement(identifier, "", {paired});
    check(result.status == DiscoveryIdentityStatus::unmatched && !result.record_index &&
              result.identifier_hints == std::vector<size_t>{0},
          "missing tag yields identifier hint without selecting a record");

    PairRecord duplicate = paired;
    duplicate.udid = "other-synthetic-device";
    result = match_advertisement(identifier, tag, {paired, duplicate});
    check(result.status == DiscoveryIdentityStatus::ambiguous && !result.record_index &&
              result.auth_tag_matches == std::vector<size_t>({0, 1}),
          "multiple matching records remain ambiguous instead of selecting first");

    PairRecord old = paired;
    old.peer_alt_irk.clear();
    result = match_advertisement(identifier, tag, {old});
    check(result.status == DiscoveryIdentityStatus::unmatched && !result.record_index &&
              result.identifier_hints == std::vector<size_t>{0},
          "legacy record without IRK remains an untrusted identifier hint");
    old.peer_alt_irk.assign(15, 1);
    result = match_advertisement(identifier, tag, {old, paired});
    check(result.status == DiscoveryIdentityStatus::matched && result.record_index == 1,
          "malformed IRK cannot shadow a usable record");

    old = paired;
    old.peer_identifier.clear();
    old.peer_public_key.clear();
    result = match_advertisement(identifier, tag, {old});
    check(result.status == DiscoveryIdentityStatus::matched && result.record_index == 0 &&
              !old.has_peer_identity(),
          "discovery match does not imply readiness for authenticated PairVerify");
    old.host_private_key.clear();
    result = match_advertisement(identifier, tag, {old});
    check(result.status == DiscoveryIdentityStatus::matched && result.record_index == 0 &&
              !old.complete(),
          "discovery match does not imply a complete pairing record");
    result = match_advertisement(identifier, tag, {});
    check(result.status == DiscoveryIdentityStatus::unmatched && !result.record_index,
          "valid advertisement without records is unmatched");

    for (const std::string malformed : {"pdYyNGg1=", "pdYyNGg", "pdYyNGg1\n", "pdYy NGg1",
                                        "a5d632346835", "pdYyNGg-", "pdYyNGg_"}) {
        result = match_advertisement(identifier, malformed, {paired});
        check(result.status == DiscoveryIdentityStatus::invalid_advertisement &&
                  !result.record_index && !result.error.empty(),
              "malformed tag encoding is rejected before matching");
    }
    std::vector<std::string> malformed_identifiers = {
        "", " ", "device id", "device\n", std::string("dev\0ice", 7), std::string(256, 'x'),
    };
    for (const auto &malformed : malformed_identifiers) {
        result = match_advertisement(malformed, tag, {paired});
        check(result.status == DiscoveryIdentityStatus::invalid_advertisement &&
                  !result.record_index && !result.error.empty(),
              "invalid identifier field is rejected");
    }
    result = match_advertisement("opaque-device-ID", "", {});
    check(result.status == DiscoveryIdentityStatus::unmatched && result.error.empty(),
          "identifier need not be parsed as a UUID");
    result = match_advertisement(std::string(255, 'x'), "", {});
    check(result.status == DiscoveryIdentityStatus::unmatched && result.error.empty(),
          "identifier at TXT string bound is accepted");

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    // 本测试进程内暂时禁止算法获取；缺少 SipHash provider 不能被误报成未配对。
    check(EVP_set_default_properties(nullptr, "provider=scrctl_nonexistent_provider") == 1,
          "set unavailable provider constraint for failure-path test");
    result = match_advertisement(identifier, tag, {paired});
    check(result.status == DiscoveryIdentityStatus::crypto_error && !result.record_index &&
              result.auth_tag_matches.empty() && !result.error.empty(),
          "SipHash provider failure is distinct from unmatched advertisement");
    check(EVP_set_default_properties(nullptr, nullptr) == 1,
          "restore provider properties after failure-path test");
#endif
}
} // namespace

int main() {
    siphash_vectors();
    advertisement_matching();
    std::printf("discovery_identity: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
