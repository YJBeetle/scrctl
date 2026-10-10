#include "ddi/Tss.h"
#include <cstdio>
#include <limits>

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
}
int main() {
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
