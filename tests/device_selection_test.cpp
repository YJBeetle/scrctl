#include "app/DeviceConnection.h"
#include "remote/Device.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace scrctl;
int checks = 0, failures = 0;
void check(bool ok, const char *label) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", label); }
}

wifi::PairRecord record(std::string udid = "device:one-original") {
    wifi::PairRecord paired;
    paired.udid = std::move(udid);
    paired.host_identifier = "synthetic-host";
    paired.host_private_key.assign(32, 1);
    paired.host_public_key.assign(32, 2);
    paired.peer_identifier = wifi::bytes_of("synthetic-peer");
    paired.peer_public_key.assign(32, 3);
    return paired;
}

remote::DiscoveryCandidate candidate(std::string address, uint16_t port, uint32_t index = 0,
                                    std::string interface = {}) {
    remote::DiscoveryCandidate value;
    value.transport = remote::DiscoveryTransport::remote_pairing;
    value.connection_type = "RemotePairing";
    value.address = std::move(address);
    value.port = port;
    value.interface_index = index;
    value.interface_name = std::move(interface);
    value.instance = "synthetic._remotepairing._tcp.local.";
    value.identifier = "synthetic-advertisement";
    value.pairing = remote::DiscoveryPairing::ready;
    return value;
}

void test_record_selection() {
    std::string error;
    const auto paired = record();
    auto legacy = record("legacy-device");
    legacy.peer_public_key.clear();
    auto incomplete = record("incomplete-device");
    incomplete.host_private_key.clear();
    auto selected = app::detail::select_pairing_record({legacy, incomplete, paired}, {}, error);
    check(selected && selected->udid == paired.udid && error.empty(),
          "manual Wi-Fi chooses the only complete record with pinned identity");
    selected = app::detail::select_pairing_record({paired}, "device_one_original", error);
    check(!selected && !error.empty(), "sanitized filename text cannot stand in for original record UDID");
    selected = app::detail::select_pairing_record({paired, record("second")}, paired.udid, error);
    check(selected && selected->udid == paired.udid && error.empty(), "manual serial selects exact original record UDID");
    check(!app::detail::select_pairing_record({paired, record("second")}, {}, error),
          "multiple complete records require explicit selection");
    check(!app::detail::select_pairing_record({paired, paired}, paired.udid, error),
          "duplicate records with one UDID remain ambiguous rather than selecting first");
    check(!app::detail::select_pairing_record({legacy, incomplete}, {}, error),
          "records lacking host credentials or device identity are unusable for Wi-Fi");
}

void test_discovered_selection() {
    std::string error;
    remote::DiscoveredDevice device;
    device.udid = record().udid;
    device.candidates = {candidate("fe80::1234%7", 49159, 7, "en7"),
                         candidate("169.254.2.3", 49156, 7, "en7"),
                         candidate("2001:db8::1", 49157, 4, "en4"),
                         candidate("192.0.2.4", 49154, 4, "en4"),
                         candidate("192.0.2.1", 49151, 1, "Wi-Fi")};
    auto selection = app::detail::select_paired_wireless_device({device}, {}, error);
    check(selection && selection->udid == device.udid && selection->candidates.size() == 5,
          "single paired discovered device retains all endpoints");
    check(selection->candidates[0].address == "192.0.2.1" && selection->candidates[1].address == "192.0.2.4" &&
              selection->candidates[2].address == "2001:db8::1" &&
              selection->candidates[3].address == "169.254.2.3" && selection->candidates[4].address == "fe80::1234%7",
          "ordinary IPv4 with Wi-Fi preference precedes IPv6 and link-local endpoints");
    check(selection->candidates[0].port == 49151 && selection->candidates[4].port == 49159 &&
              selection->candidates[4].interface_index == 7,
          "discovery preserves each SRV port and scoped interface identity");

    auto other = device;
    other.udid = "second-device";
    check(!app::detail::select_paired_wireless_device({device, other}, {}, error),
          "multiple paired devices cannot be resolved by picking first");
    selection = app::detail::select_paired_wireless_device({device, other}, device.udid, error);
    check(selection && selection->udid == device.udid, "auto Wi-Fi accepts exact original serial filter");
    check(!app::detail::select_paired_wireless_device({device}, "synthetic-advertisement", error),
          "advertised identifier is not a device serial filter");
    check(!app::detail::select_paired_wireless_device({device}, "device_one_original", error),
          "sanitized record filename is not an auto Wi-Fi serial");

    auto untrusted = device;
    untrusted.udid.clear();
    check(!app::detail::select_paired_wireless_device({untrusted}, {}, error), "unknown untrusted UDID cannot connect automatically");
    for (const auto state : {remote::DiscoveryPairing::unmatched, remote::DiscoveryPairing::identifier_hint,
                            remote::DiscoveryPairing::needs_pairing, remote::DiscoveryPairing::ambiguous,
                            remote::DiscoveryPairing::invalid_advertisement, remote::DiscoveryPairing::crypto_error}) {
        untrusted = device;
        for (auto &value : untrusted.candidates) value.pairing = state;
        check(!app::detail::select_paired_wireless_device({untrusted}, {}, error),
              "only unique ready authTag match is an automatic connection candidate");
    }
    device.candidates = {candidate("", 49152), candidate("192.0.2.1", 0)};
    check(!app::detail::select_paired_wireless_device({device}, {}, error), "incomplete address or SRV port cannot be fabricated");
    device.candidates = {candidate("fe80::1", 49152, 9, "en9"), candidate("FEA0::2", 49153, 10, "en10"),
                         candidate("fe80::3%en4", 49154, 4, "en4")};
    selection = app::detail::select_paired_wireless_device({device}, {}, error);
    check(selection && selection->candidates[0].address == "fe80::1%9" &&
              selection->candidates[1].address == "FEA0::2%10" && selection->candidates[2].address == "fe80::3%en4",
          "missing numeric IPv6 scopes are restored and existing named scope remains unchanged");
    auto extra_instance = device;
    for (auto &value : extra_instance.candidates) value.instance = "other-instance";
    selection = app::detail::select_paired_wireless_device({device, extra_instance, device}, {}, error);
    check(selection && selection->candidates.size() == 6,
          "same real device across instances stays one choice and exact duplicates are removed");
}

void test_connection_attempts() {
    const auto paired = record();
    app::detail::WirelessSelection selection{paired.udid,
        {candidate("192.0.2.1", 50001), candidate("192.0.2.2", 50002), candidate("fe80::1%9", 50003, 9)}};
    std::vector<std::pair<std::string, uint16_t>> attempts;
    std::string error;
    auto connect = [&](const std::string &address, const wifi::PairRecord &used, uint16_t port, std::string &err)
        -> std::optional<remote::Device> {
        check(used.udid == paired.udid, "every address uses the selected pinned pairing record");
        attempts.emplace_back(address, port);
        if (attempts.size() == 2) return remote::Device{};
        err = "synthetic first failure";
        return std::nullopt;
    };
    auto device = app::detail::connect_wireless_candidates(selection, paired, error, connect);
    check(device && attempts == std::vector<std::pair<std::string, uint16_t>>{{"192.0.2.1", 50001}, {"192.0.2.2", 50002}} && error.empty(),
          "first endpoint failure tries next SRV endpoint and success stops remaining attempts");
    attempts.clear();
    auto fail_all = [&](const std::string &address, const wifi::PairRecord &, uint16_t port, std::string &err)
        -> std::optional<remote::Device> {
        attempts.emplace_back(address, port);
        err = "failure-" + std::to_string(port);
        return std::nullopt;
    };
    device = app::detail::connect_wireless_candidates(selection, paired, error, fail_all);
    check(!device && attempts.size() == 3 && attempts.back() == std::pair<std::string, uint16_t>{"fe80::1%9", 50003},
          "all endpoints are attempted including scoped IPv6 when earlier addresses fail");
    check(error.find("failure-50001") != std::string::npos && error.find("failure-50002") != std::string::npos &&
              error.find("failure-50003") != std::string::npos,
          "all failed attempts retain their errors for diagnosis");
    attempts.clear();
    device = app::detail::connect_wireless_candidates(selection, record("other-device"), error, fail_all);
    check(!device && attempts.empty(), "mismatched internal UDID fails before any network attempt");
    auto legacy = paired;
    legacy.peer_identifier.clear();
    device = app::detail::connect_wireless_candidates(selection, legacy, error, fail_all);
    check(!device && attempts.empty(), "missing peer identity fails before connection");
}

void test_usbmux_precedence() {
    const std::vector<transport::DeviceRecord> devices{{1, "usb-device", "USB", 0}, {2, "mux-network", "Network", 0}};
    check(app::detail::has_usbmux_match(devices, {}), "any existing usbmux device selects local connection before discovery");
    check(app::detail::has_usbmux_match(devices, "usb-device"), "matching USB serial selects USB protocol path");
    check(app::detail::has_usbmux_match(devices, "mux-network"), "existing usbmux Network entry retains its original transport path");
    check(!app::detail::has_usbmux_match(devices, "wireless-only"), "absent serial allows discovery instead of selecting unrelated USB device");
    check(!app::detail::has_usbmux_match({}, {}), "empty usbmux list allows wireless discovery");
}

void test_usbmux_device_selection() {
    const std::string first = "00008110-PRIVATE-FIRST-AAAA";
    const std::string second = "00008110-PRIVATE-SECOND-BBBB";
    const std::string absent = "00008110-PRIVATE-ABSENT-CCCC";
    const transport::DeviceRecord usb{10, first, "USB", 1};
    const transport::DeviceRecord network{11, first, "Network", 1};
    const transport::DeviceRecord other{12, second, "USB", 2};
    std::string error = "stale error";
    auto select = [&](const std::vector<transport::DeviceRecord> &records,
                      std::string_view serial = {}, bool usb_only = false) {
        return remote::detail::select_usbmux_device(records, serial, error, usb_only);
    };
    auto selected = select({network, usb});
    check(selected && selected->device_id == usb.device_id && selected->connection_type == "USB" && error.empty(),
          "same UDID USB and Network entries are one device and USB is preferred");
    selected = select({usb, network});
    check(selected && selected->device_id == usb.device_id,
          "USB preference is independent of usbmux list ordering");
    selected = select({network, other, usb}, first);
    check(selected && selected->device_id == usb.device_id && selected->udid == first && error.empty(),
          "explicit original serial prefers USB without selecting a different device");
    check(!select({network, usb, other}) && error.find("Connected devices: 2") != std::string::npos,
          "automatic selection counts distinct UDIDs rather than connection records");
    check(error.find(first) == std::string::npos && error.find(second) == std::string::npos &&
              error.find(remote::mask(first)) != std::string::npos && error.find(remote::mask(second)) != std::string::npos,
          "multiple-device error only displays masked UDIDs");
    check(!select({network, usb, other}, absent) && error.find(absent) == std::string::npos &&
              error.find(first) == std::string::npos && error.find(second) == std::string::npos &&
              error.find(remote::mask(absent)) != std::string::npos,
          "unmatched serial and candidates are masked in the selection error");
    selected = select({network});
    check(selected && selected->device_id == network.device_id && error.empty(),
          "unique usbmux Network connection remains usable when USB is absent");
    auto duplicate_usb = usb;
    duplicate_usb.device_id = 13;
    check(!select({usb, duplicate_usb, network}) && !error.empty() && error.find(first) == std::string::npos,
          "multiple USB connections for one UDID are rejected rather than choosing first or falling back");
    check(!select({network, duplicate_usb, usb}, first),
          "explicit serial cannot hide duplicate USB connections");
    auto duplicate_network = network;
    duplicate_network.device_id = 14;
    check(!select({network, duplicate_network}),
          "multiple Network connections for one UDID are rejected without USB");
    selected = select({network, duplicate_network, usb});
    check(selected && selected->device_id == usb.device_id,
          "duplicate lower-priority Network entries do not prevent choosing the unique USB connection");
    selected = select({network, usb}, {}, true);
    check(selected && selected->device_id == usb.device_id,
          "USB-only mode filters Network before grouping");
    check(!select({network}, first, true), "USB-only mode never selects a Network connection");
    check(!select({usb, duplicate_usb}, first, true), "USB-only mode rejects duplicate USB connections");
    check(!select({}), "empty list cannot select a device");
    const transport::DeviceRecord empty_usb{20, "", "USB", 0};
    const transport::DeviceRecord empty_network{21, "", "Network", 0};
    check(!select({empty_usb}) && !error.empty(), "empty UDID is not a unique real USB device");
    check(!select({empty_network}) && !error.empty(), "empty UDID is not a unique real Network device");
    check(!select({empty_usb, empty_network}, {}, true), "USB-only filtering cannot make an empty UDID usable");
    selected = select({empty_usb, usb});
    check(selected && selected->device_id == usb.device_id && selected->udid == first,
          "unidentifiable entries cannot replace a uniquely identified device");
    selected = select({empty_usb, other, network, usb}, second, true);
    check(selected && selected->device_id == other.device_id,
          "explicit USB-only selection preserves exact serial among other devices and empty IDs");
}
} // namespace

int main() {
    test_record_selection();
    test_discovered_selection();
    test_connection_attempts();
    test_usbmux_precedence();
    test_usbmux_device_selection();
    std::printf("device_selection: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
