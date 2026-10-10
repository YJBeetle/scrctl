#include "wifi/HostMetadata.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace {
using namespace scrctl::wifi;
int checks = 0, failures = 0;
void check(bool ok, const char *label) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", label); }
}

Bytes rsmb(const detail::MachineUuid &uuid, uint8_t minor) {
    Bytes raw{0, 2, minor, 0, 32, 0, 0, 0,
              0, 4, 0, 0, 0, 0, // complete preceding structure
              1, 24, 1, 0, 0, 0, 0, 0};
    raw.insert(raw.end(), uuid.begin(), uuid.end());
    raw.insert(raw.end(), {0, 0});
    return raw;
}

void test_uuid_sources() {
    const auto uuid = detail::parse_machine_uuid("00112233-4455-6677-8899-AABBCCDDEEFF\n");
    check(uuid.has_value(), "canonical machine UUID with sysfs newline parses");
    check(uuid == detail::parse_machine_uuid(" \t00112233445566778899aabbccddeeff\r\n"),
          "case, separators and surrounding whitespace preserve canonical bytes");
    for (const auto *bad : {"", "00000000-0000-0000-0000-000000000000",
                           "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF", "00112233_4455-6677-8899-AABBCCDDEEFF",
                           "00112233-4455-6677-8899-AABBCCDDEEFG", "00112233445566778899aabbccddeef",
                           "00112233445566778899aabbccddeeff00"}) {
        check(!detail::parse_machine_uuid(bad), "missing, placeholder or malformed UUID is rejected");
    }
    if (!uuid) return;
    auto little = *uuid;
    std::reverse(little.begin(), little.begin() + 4);
    std::reverse(little.begin() + 4, little.begin() + 6);
    std::reverse(little.begin() + 6, little.begin() + 8);
    check(detail::smbios_machine_uuid(rsmb(little, 6)) == uuid,
          "SMBIOS 2.6 canonicalizes little-endian UUID fields after an earlier record");
    check(detail::smbios_machine_uuid(rsmb(*uuid, 5)) == uuid,
          "older SMBIOS preserves network byte order");
    auto newer = rsmb(little, 0); newer[1] = 3;
    check(detail::smbios_machine_uuid(newer) == uuid, "SMBIOS 3.x uses the 2.6+ UUID byte order");
    const auto valid = rsmb(little, 6);
    for (size_t size = 0; size < valid.size(); ++size) {
        check(!detail::smbios_machine_uuid(std::span(valid).first(size)),
              "every truncated RSMB buffer falls back instead of reading outside it");
    }
    auto bad = valid; bad[15] = 3;
    check(!detail::smbios_machine_uuid(bad), "invalid formatted structure length is rejected");
    bad = valid; bad[15] = 25;
    check(!detail::smbios_machine_uuid(bad), "missing complete type-1 string terminator is rejected");
    check(!detail::smbios_machine_uuid(rsmb(detail::MachineUuid{}, 6)),
          "zero firmware UUID is not claimed as hardware identity");
    detail::MachineUuid ones; ones.fill(0xff);
    check(!detail::smbios_machine_uuid(rsmb(ones, 6)), "FF firmware UUID is not claimed as hardware identity");
}

void test_serial_derivation() {
    const auto uuid = detail::parse_machine_uuid("00112233-4455-6677-8899-AABBCCDDEEFF");
    std::string err = "stale";
    const auto hardware = detail::host_metadata_for_uuid("HOST-ID-OWNED", uuid, err);
    // Constants independently computed with Python hashlib.sha256, not a second C++ implementation.
    check(hardware && hardware->serial_number == "SCRCTL-CF9C221BAEFFB42D" &&
              hardware->source == HostMetadataSource::MachineUuid && err.empty(),
          "machine UUID matches independent SHA-256 vector");
    const auto other_id = detail::host_metadata_for_uuid("ANOTHER-PER-PAIR-ID", uuid, err);
    check(hardware && other_id && hardware->serial_number == other_id->serial_number,
          "available machine UUID keeps the display serial stable across fresh pairing IDs");
    const auto system = detail::host_metadata_for_uuid("HOST-ID-OWNED", uuid, err,
                                                      HostMetadataSource::SystemInstance);
    const auto system_other = detail::host_metadata_for_uuid("ANOTHER-PER-PAIR-ID", uuid, err,
                                                            HostMetadataSource::SystemInstance);
    check(system && system->serial_number == "SCRCTL-1670A5559FA874B6" &&
              system->source == HostMetadataSource::SystemInstance,
          "system-instance identifier uses its own independent SHA-256 vector and source");
    check(system && system_other && system->serial_number == system_other->serial_number &&
              hardware && system->serial_number != hardware->serial_number,
          "system-instance fallback is stable across fresh pairing IDs and distinct from hardware domain");
    const auto fallback = detail::host_metadata_for_uuid("HOST-ID-OWNED", std::nullopt, err);
    check(fallback && fallback->serial_number == "SCRCTL-E6EFE429642D74F6" &&
              fallback->source == HostMetadataSource::HostIdentifier,
          "unavailable platform identity falls back to exact host identity vector");
    const auto invalid = detail::host_metadata_for_uuid("HOST-ID-OWNED", detail::MachineUuid{}, err);
    check(invalid && fallback && invalid->serial_number == fallback->serial_number &&
              invalid->source == HostMetadataSource::HostIdentifier,
          "invalid hardware UUID also uses the identity fallback");
    const auto changed = detail::host_metadata_for_uuid("host-id-owned", std::nullopt, err);
    check(changed && fallback && changed->serial_number != fallback->serial_number,
          "fallback does not normalize or replace the signed host identifier");
    check(!detail::host_metadata_for_uuid("", std::nullopt, err) && !err.empty(),
          "missing both metadata inputs is an explicit failure");
    check(hardware && fallback && hardware->model == host_model() && fallback->model == host_model(),
          "platform model is identical for hardware and fallback sources");
    check(hardware && hardware->serial_number.size() == 23 &&
              hardware->serial_number.starts_with("SCRCTL-"), "display serial has the bounded ASCII wire format");
    const auto first = host_metadata("OWNED-RUNTIME-FALLBACK", err);
    const auto second = host_metadata("OWNED-RUNTIME-FALLBACK", err);
    check(first && second && first->serial_number == second->serial_number && first->model == host_model(),
          "native platform adapter gives stable metadata without exposing raw machine values");
}
} // namespace

int main() {
    test_uuid_sources();
    test_serial_derivation();
    std::printf("host_metadata_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
