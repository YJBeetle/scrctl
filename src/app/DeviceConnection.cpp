#include "app/DeviceConnection.h"
#include "i18n/Translation.h"
#include "wifi/PairRecord.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <tuple>

namespace scrctl::app {
namespace {
bool ipv6_link_local(std::string_view address) {
    // fe80::/10，包含 fe80..febf。地址可带数字或接口名 scope。
    if (address.size() < 4) return false;
    auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
    return lower(address[0]) == 'f' && lower(address[1]) == 'e' &&
           (address[2] == '8' || address[2] == '9' || lower(address[2]) == 'a' || lower(address[2]) == 'b');
}

int address_priority(const remote::DiscoveryCandidate &candidate) {
    if (candidate.address.find(':') == std::string::npos)
        return candidate.address.starts_with("169.254.") ? 2 : 0;
    return ipv6_link_local(candidate.address) ? 3 : 1;
}

bool wifi_interface(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
    return name.find("wifi") != std::string::npos || name.find("wi-fi") != std::string::npos ||
           name.find("wireless") != std::string::npos || name.starts_with("wlan") || name == "en0";
}

void preserve_scope(remote::DiscoveryCandidate &candidate) {
    if (!ipv6_link_local(candidate.address) || candidate.address.find('%') != std::string::npos) return;
    if (candidate.interface_index) candidate.address += "%" + std::to_string(candidate.interface_index);
    else if (!candidate.interface_name.empty()) candidate.address += "%" + candidate.interface_name;
}

std::optional<wifi::PairRecord> load_selected_record(std::string_view serial, std::string &error) {
    std::vector<std::string> warnings;
    bool cancelled = false;
    const auto records = remote::detail::load_discovery_records(wifi::default_record_dir(), {}, warnings, cancelled);
    auto selected = detail::select_pairing_record(records, serial, error);
    if (!selected && !warnings.empty()) {
        for (const auto &warning : warnings) error += "\n" + warning;
    }
    return selected;
}

std::optional<remote::Device> open_discovered_device(const std::string &serial, std::string &error) {
    remote::DiscoveryOptions options;
    options.include_usb = false;
    const auto discovery = remote::discover_devices(options);
    auto selection = detail::select_paired_wireless_device(discovery.devices, serial, error);
    if (!selection) {
        for (const auto &warning : discovery.warnings) error += "\n" + warning;
        return std::nullopt;
    }
    const auto record = load_selected_record(selection->udid, error);
    if (!record) return std::nullopt;
    return detail::connect_wireless_candidates(*selection, *record, error,
        [](const std::string &address, const wifi::PairRecord &paired, uint16_t port, std::string &err) {
            return remote::Device::establish_wifi(address, paired, err, false, port);
        });
}
} // namespace

bool detail::has_usbmux_match(const std::vector<transport::DeviceRecord> &devices, std::string_view serial) {
    return std::any_of(devices.begin(), devices.end(), [&](const auto &device) {
        return serial.empty() || device.udid == serial;
    });
}

std::optional<wifi::PairRecord> detail::select_pairing_record(const std::vector<wifi::PairRecord> &records,
                                                           std::string_view serial, std::string &error) {
    error.clear();
    const wifi::PairRecord *selected = nullptr;
    for (const auto &record : records) {
        if (!record.complete() || !record.has_peer_identity() ||
            (!serial.empty() && record.udid != serial)) continue;
        if (selected) {
            error = SCRCTL_TR("Multiple usable remote pairing records match; select a unique device with -s");
            return std::nullopt;
        }
        selected = &record;
    }
    if (!selected) {
        error = SCRCTL_TR("No usable remote pairing record matches; pair the device over USB first");
        return std::nullopt;
    }
    return *selected;
}

std::optional<detail::WirelessSelection> detail::select_paired_wireless_device(
    const std::vector<remote::DiscoveredDevice> &devices, std::string_view serial, std::string &error) {
    error.clear();
    std::map<std::string, WirelessSelection> choices;
    for (const auto &device : devices) {
        if (device.udid.empty() || (!serial.empty() && device.udid != serial)) continue;
        for (auto candidate : device.candidates) {
            if (candidate.transport != remote::DiscoveryTransport::remote_pairing ||
                candidate.pairing != remote::DiscoveryPairing::ready ||
                candidate.address.empty() || candidate.port == 0) continue;
            preserve_scope(candidate);
            auto &choice = choices[device.udid];
            choice.udid = device.udid;
            if (std::find(choice.candidates.begin(), choice.candidates.end(), candidate) == choice.candidates.end())
                choice.candidates.push_back(std::move(candidate));
        }
    }
    if (choices.empty()) {
        error = SCRCTL_TR("No paired wireless device is available; check the network and pair over USB first");
        return std::nullopt;
    }
    if (choices.size() > 1) {
        error = SCRCTL_TR("Multiple paired wireless devices are available; select one with -s");
        return std::nullopt;
    }
    auto selected = std::move(choices.begin()->second);
    std::stable_sort(selected.candidates.begin(), selected.candidates.end(), [](const auto &left, const auto &right) {
        return std::tuple(address_priority(left), !wifi_interface(left.interface_name)) <
               std::tuple(address_priority(right), !wifi_interface(right.interface_name));
    });
    return selected;
}

std::optional<remote::Device> detail::connect_wireless_candidates(const WirelessSelection &selection,
                                                               const wifi::PairRecord &record,
                                                               std::string &error,
                                                               const WifiConnector &connect) {
    error.clear();
    if (selection.udid.empty() || record.udid != selection.udid ||
        !record.complete() || !record.has_peer_identity()) {
        error = SCRCTL_TR("Selected wireless device does not match a usable pairing record");
        return std::nullopt;
    }
    if (!connect || selection.candidates.empty()) {
        error = SCRCTL_TR("No usable wireless connection candidate is available");
        return std::nullopt;
    }
    std::string failures;
    for (const auto &candidate : selection.candidates) {
        if (candidate.transport != remote::DiscoveryTransport::remote_pairing ||
            candidate.pairing != remote::DiscoveryPairing::ready ||
            candidate.address.empty() || !candidate.port) continue;
        std::string candidate_error;
        auto device = connect(candidate.address, record, candidate.port, candidate_error);
        if (device) return device;
        failures += "\n  " + candidate.address + ":" + std::to_string(candidate.port) + ": " + candidate_error;
    }
    error = SCRCTL_TR("Cannot connect to any discovered wireless address");
    error += failures;
    return std::nullopt;
}

std::optional<remote::Device> open_device(const std::string &serial, const std::string &wifi,
                                          std::string &error, uint16_t wifi_port) {
    error.clear();
    if (wifi == "auto") return open_discovered_device(serial, error);
    if (!wifi.empty()) {
        if (!wifi_port) {
            error = SCRCTL_TR("Wi-Fi port must be between 1 and 65535");
            return std::nullopt;
        }
        const auto record = load_selected_record(serial, error);
        if (!record) return std::nullopt;
        return remote::Device::establish_wifi(wifi, *record, error, false, wifi_port);
    }
    std::string usb_error;
    const auto devices = remote::Device::list(usb_error);
    if (detail::has_usbmux_match(devices, serial)) {
        // 设备存在时，任何 USB 协议失败都保留原错误，不能据此改连另一无线会话。
        return remote::Device::establish(serial, error);
    }
    auto wireless = open_discovered_device(serial, error);
    if (!wireless && !usb_error.empty())
        error += "\n" + std::string(SCRCTL_TR("USB device enumeration was unavailable: ")) + usb_error;
    return wireless;
}
} // namespace scrctl::app
