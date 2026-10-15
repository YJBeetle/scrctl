#include "ddi/Tss.h"
#include "ddi/AppleTssTrust.h"
#include "ddi_tss_certificates.h"
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <algorithm>
#include <array>
#include <ctime>
#include <cstdio>
#include <limits>
#include <memory>

namespace {
using V = scrctl::plist::Value;
namespace ddi = scrctl::ddi;
int failed = 0;
void check(bool ok, const char *name) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); if (!ok) ++failed; }
V component(bool personalized, bool rules = false) {
    auto value = V::Dict(), info = V::Dict();
    info.set("Personalize", V::Bool(personalized));
    if (rules) {
        auto list = V::Array(), rule = V::Dict(), conditions = V::Dict(), actions = V::Dict();
        conditions.set("ApRequiresImage4", V::Bool(true)); conditions.set("ApCurrentProductionMode", V::Bool(true));
        actions.set("EPRO", V::Bool(true)); actions.set("ESEC", V::Bool(true)); actions.set("Keep", V::Int(255));
        rule.set("Conditions", conditions); rule.set("Actions", actions); list.push(rule);
        auto demotion = V::Dict(); conditions.set("ApDemotionPolicyOverride", V::Str("Demote"));
        actions.set("EPRO", V::Bool(false)); demotion.set("Conditions", conditions); demotion.set("Actions", actions); list.push(demotion);
        info.set("RestoreRequestRules", list);
    }
    value.set("Info", info); value.set("Trusted", V::Bool(true));
    value.set("Digest", V::OfData(std::vector<uint8_t>(48, 17))); value.set("Keep", V::Int(7)); return value;
}
V personalized_manifest() {
    auto manifest = V::Dict(), identity = V::Dict(), identities = V::Array(), info = V::Dict(), entries = V::Dict();
    info.set("Variant", V::Str("Customer iOS Developer PDI")); identity.set("Info", info);
    identity.set("ApBoardID", V::Str("0x16")); identity.set("ApChipID", V::Str("0x8020"));
    entries.set("PersonalizedDMG", component(true)); entries.set("LoadableTrustCache", component(true, true));
    auto untrusted = component(true); untrusted.set("Trusted", V::Bool(false)); entries.set("Untrusted", untrusted);
    identity.set("Manifest", entries); identities.push(identity); manifest.set("BuildIdentities", identities); return manifest;
}
V chip() {
    auto chip = V::Dict(); chip.set("img4_chip_chip", V::Int(0x8110));
    chip.set("img4_chip_ecid", V::UInt(UINT64_MAX)); chip.set("img4_chip_cpro", V::Bool(true)); return chip;
}
V cryptex_identity() {
    auto identity = V::Dict(); identity.set("Cryptex1,ChipID", V::Str("0xF00D"));
    identity.set("Cryptex1,ProductClass", V::Str("0x1")); identity.set("Cryptex1,UseProductClass", V::Bool(true));
    identity.set("Cryptex1,Type", V::Int(2)); identity.set("Cryptex1,SubType", V::Int(3));
    identity.set("Cryptex1,NonceDomain", V::Int(4)); identity.set("Cryptex1,Version", V::Str("39.999.999.0.0,0"));
    identity.set("Cryptex1,PreauthorizationVersion", V::Str("39.999.999.0.0,0"));
    auto manifest = V::Dict(); manifest.set("Cryptex1,GenericDmg", component(false));
    manifest.set("Cryptex1,GenericTrustCache", component(true)); manifest.set("Cryptex1,GenericVolume", component(true));
    manifest.set("OtherTrustedComponent", component(true)); identity.set("Manifest", manifest); return identity;
}
std::string response(std::string key, V ticket) {
    auto dictionary = V::Dict(); dictionary.set(key, ticket);
    dictionary.set("IgnoredText", V::Str("xml & character"));
    return "STATUS=0&MESSAGE=SUCCESS&REQUEST_STRING=" + scrctl::plist::write(dictionary);
}
using Certificate = std::unique_ptr<X509, decltype(&X509_free)>;
Certificate read_certificate(std::string_view pem) {
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), BIO_free);
    return Certificate(bio ? PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr) : nullptr, X509_free);
}
// An independent OpenSSL path verifies actual certificate signatures and DNS
// identities; these fixtures are not generated to mirror the production code.
constexpr time_t fixture_time = 1780272000; // 2026-06-01 00:00:00 UTC.
bool verify_certificate(X509 *root, X509 *leaf, X509 *intermediate, const char *hostname,
                        time_t when, int &error) {
    error = X509_V_ERR_UNSPECIFIED;
    std::unique_ptr<X509_STORE, decltype(&X509_STORE_free)> store(X509_STORE_new(), X509_STORE_free);
    std::unique_ptr<X509_STORE_CTX, decltype(&X509_STORE_CTX_free)> context(X509_STORE_CTX_new(), X509_STORE_CTX_free);
    auto free_chain = [](STACK_OF(X509) *chain) { sk_X509_free(chain); };
    std::unique_ptr<STACK_OF(X509), decltype(free_chain)> chain(sk_X509_new_null(), free_chain);
    if (!store || !context || !chain || !leaf || (root && X509_STORE_add_cert(store.get(), root) != 1)) return false;
    if (intermediate && !sk_X509_push(chain.get(), intermediate)) return false;
    if (X509_STORE_CTX_init(context.get(), store.get(), leaf, chain.get()) != 1 ||
        X509_STORE_CTX_set_purpose(context.get(), X509_PURPOSE_SSL_SERVER) != 1) return false;
    auto *parameters = X509_STORE_CTX_get0_param(context.get());
    X509_VERIFY_PARAM_set_time(parameters, when);
    X509_VERIFY_PARAM_set_hostflags(parameters, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    if (X509_VERIFY_PARAM_set1_host(parameters, hostname, 0) != 1) return false;
    const bool result = X509_verify_cert(context.get()) == 1;
    error = X509_STORE_CTX_get_error(context.get());
    return result;
}
Certificate unrelated_certificate() {
    Certificate certificate(X509_new(), X509_free);
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> key_context(EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr), EVP_PKEY_CTX_free);
    EVP_PKEY *raw_key = nullptr;
    if (!certificate || !key_context || EVP_PKEY_keygen_init(key_context.get()) != 1 ||
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(key_context.get(), NID_X9_62_prime256v1) != 1 ||
        EVP_PKEY_keygen(key_context.get(), &raw_key) != 1) return Certificate(nullptr, X509_free);
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw_key, EVP_PKEY_free);
    std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)> subject(X509_NAME_new(), X509_NAME_free);
    if (!subject || X509_set_version(certificate.get(), 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) != 1 ||
        !ASN1_TIME_set(X509_getm_notBefore(certificate.get()), fixture_time - 3600) ||
        !ASN1_TIME_set(X509_getm_notAfter(certificate.get()), fixture_time + 3600) ||
        X509_NAME_add_entry_by_txt(subject.get(), "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char *>("gs.apple.com"), -1, -1, 0) != 1 ||
        X509_set_subject_name(certificate.get(), subject.get()) != 1 ||
        X509_set_issuer_name(certificate.get(), subject.get()) != 1 || X509_set_pubkey(certificate.get(), key.get()) != 1 ||
        X509_sign(certificate.get(), key.get(), EVP_sha256()) <= 0) return Certificate(nullptr, X509_free);
    return certificate;
}
void trust_checks() {
    auto root = read_certificate(ddi::detail::kAppleTssRootPem);
    auto leaf = read_certificate(ddi_tss_fixture::leaf);
    auto intermediate = read_certificate(ddi_tss_fixture::intermediate);
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{}; unsigned int length = 0;
    const std::array<unsigned char, 32> official_digest{
        0xb0,0xb1,0x73,0x0e,0xcb,0xc7,0xff,0x45,0x05,0x14,0x2c,0x49,0xf1,0x29,0x5e,0x6e,
        0xda,0x6b,0xca,0xed,0x7e,0x2c,0x68,0xc5,0xbe,0x91,0xb5,0xa1,0x10,0x01,0xf0,0x24};
    check(root && X509_digest(root.get(), EVP_sha256(), digest.data(), &length) == 1 &&
        length == official_digest.size() && std::equal(official_digest.begin(), official_digest.end(), digest.begin()),
        "Bundled root matches the official Apple DER SHA256 fingerprint");
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> root_key(root ? X509_get_pubkey(root.get()) : nullptr, EVP_PKEY_free);
    check(root && root_key && X509_check_ca(root.get()) > 0 &&
        (X509_get_key_usage(root.get()) & KU_KEY_CERT_SIGN) &&
        X509_NAME_cmp(X509_get_subject_name(root.get()), X509_get_issuer_name(root.get())) == 0 &&
        X509_verify(root.get(), root_key.get()) == 1,
        "Bundled Apple trust anchor is a self-signed CA with certificate-signing usage");
    int error = 0;
    check(root && leaf && intermediate && verify_certificate(root.get(), leaf.get(), intermediate.get(), "gs.apple.com", fixture_time, error),
        "Actual Apple TSS server chain verifies under the bundled root for gs.apple.com");
    check(root && leaf && intermediate && !verify_certificate(root.get(), leaf.get(), intermediate.get(), "wrong.apple.com", fixture_time, error) &&
        error == X509_V_ERR_HOSTNAME_MISMATCH,
        "A valid Apple chain for the wrong hostname is rejected");
    check(leaf && intermediate && !verify_certificate(nullptr, leaf.get(), intermediate.get(), "gs.apple.com", fixture_time, error),
        "The same server chain fails without its trusted Apple root");
    auto unrelated = unrelated_certificate();
    check(root && unrelated && X509_check_host(unrelated.get(), "gs.apple.com", 0, 0, nullptr) == 1 &&
        !verify_certificate(root.get(), unrelated.get(), nullptr, "gs.apple.com", fixture_time, error),
        "An unrelated self-signed gs.apple.com certificate is not trusted");
    check(root && leaf && intermediate && !verify_certificate(root.get(), leaf.get(), intermediate.get(), "gs.apple.com", 1811808000, error) &&
        error == X509_V_ERR_CERT_HAS_EXPIRED,
        "Trust does not bypass server certificate expiry");
}
}
int main() {
    trust_checks();
    std::string error;
    const std::vector<uint8_t> nonce(32, 23);
    auto identifiers = V::Dict(); identifiers.set("BoardId", V::Int(0x16)); identifiers.set("ChipID", V::Int(0x8020));
    identifiers.set("Ap,ProductType", V::Str("iPad11,2"));
    auto p = ddi::build_personalized_tss_request(personalized_manifest(), identifiers, UINT64_MAX, nonce, error);
    check(p && p->find("ApECID")->is_uint() && p->find("ApECID")->unsigned_integer == UINT64_MAX &&
          p->find("Ap,ProductType")->string == "iPad11,2", "Personalized hardware selection preserves full-width ECID and product identifiers");
    const auto *dmg = p ? p->find("PersonalizedDMG") : nullptr;
    check(dmg && dmg->find("EPRO")->boolean && dmg->find("ESEC")->boolean && dmg->find("Keep")->integer == 7 &&
          !dmg->find("Info") && !p->find("Untrusted"), "Shared PDI restore rules sign the DMG, ignore demotion and 255, and strip Info/untrusted payloads");
    check(p && p->find("SepNonce")->data == std::vector<uint8_t>(20, 0) &&
          p->find("@ApImg4Ticket")->boolean && p->find("@BBTicket")->boolean && !p->find("UID_MODE")->boolean,
          "Personalized request includes developer-image signing mode and SEP nonce");
    identifiers.set("BoardId", V::Int(99));
    check(!ddi::build_personalized_tss_request(personalized_manifest(), identifiers, 1, nonce, error), "Wrong board is rejected before contacting Apple");
    check(!ddi::build_cryptex_tss_request(cryptex_identity(), chip(), {}, error), "Empty nonce is rejected before signing");
    auto c = ddi::build_cryptex_tss_request(cryptex_identity(), chip(), nonce, error);
    const std::vector<uint8_t> udid{0,0,0,0,0,0,0x81,0x10,255,255,255,255,255,255,255,255};
    check(c && c->find("Cryptex1,UDID")->data == udid && c->find("Cryptex1,ChipID")->integer == 0xF00D &&
          c->find("Cryptex1,ProductionMode")->is_bool() && c->find("Cryptex1,NonceDomain")->integer == 4 &&
          c->find("Cryptex1,Version")->string == "39.999.999.0.0,0", "Cryptex request encodes chip/ECID as big-endian UDID with numeric, string and boolean types");
    check(c && !c->find("Cryptex1,GenericDmg") && !c->find("OtherTrustedComponent") &&
          c->find("Cryptex1,GenericTrustCache")->keys == std::vector<std::string>{"Digest"} &&
          !c->find("ApECID") && c->find("@Cryptex1,Ticket")->boolean,
          "Cryptex request contains only personalized component digests and no AP identity fields");
    const std::vector<uint8_t> ticket{0x30, 1, 2, 3};
    check(ddi::detail::parse_tss_response(response("ApImg4Ticket", V::OfData(ticket)), false, error) == ticket,
          "Raw XML ticket response with ampersands is decoded intact");
    check(ddi::detail::parse_tss_response(response("Cryptex1,Ticket", V::OfData(ticket)), true, error) == ticket &&
          ddi::detail::parse_tss_response(response("ApImg4Ticket", V::OfData(ticket)), true, error) == ticket,
          "Cryptex ticket and Apple's AP-ticket fallback are supported");
    check(!ddi::detail::parse_tss_response("STATUS=94&MESSAGE=secret nonce", false, error) && error.find("secret") == std::string::npos,
          "Apple refusal returns status without leaking server signing material");
    check(!ddi::detail::parse_tss_response("STATUS=0&STATUS=0&MESSAGE=SUCCESS&REQUEST_STRING=<plist/>", false, error) &&
          !ddi::detail::parse_tss_response("MESSAGE=SUCCESS&REQUEST_STRING=<plist/>", false, error),
          "Malformed or incomplete response envelopes cannot be accepted as success");
    check(!ddi::detail::parse_tss_response(response("ApImg4Ticket", V::Str("not data")), false, error) &&
          !ddi::detail::parse_tss_response(response("ApImg4Ticket", V::OfData({})), false, error) &&
          !ddi::detail::parse_tss_response(response("ApImg4Ticket", V::OfData(std::vector<uint8_t>((1u << 20) + 1))), false, error),
          "Wrong ticket type, empty ticket and oversized ticket are rejected");
    check(!ddi::detail::parse_tss_response(std::string((4u << 20) + 1, 'x'), false, error), "Oversized HTTP response is rejected before XML parsing");
    ddi::TssOptions options; options.cancelled = [] { return true; };
    check(c && !ddi::request_tss_ticket(*c, options, error) && error.find("cancelled") != std::string::npos,
          "Cancellation prevents all signing HTTP activity");
    options.cancelled = {}; options.timeout = std::chrono::milliseconds(0);
    check(c && !ddi::request_tss_ticket(*c, options, error), "Expired signing budget prevents all HTTP activity");
    return failed ? 1 : 0;
}
