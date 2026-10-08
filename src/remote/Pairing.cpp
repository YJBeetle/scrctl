#include "remote/Pairing.h"

#include "i18n/Translation.h"
#include "remote/Device.h"
#include "remote/PairingChannel.h"
#include "wifi/PairRecord.h"
#include "wifi/PairSetup.h"
#include "wifi/PairVerify.h"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <memory>
#include <utility>

namespace scrctl::remote {
namespace {
constexpr std::string_view kPairingService =
    "com.apple.internal.dt.coredevice.untrusted.tunnelservice";

bool validate(const UsbPairingOptions &options, std::string &error) {
    if (options.timeout_ms < 1 || options.timeout_ms > 300000) {
        error = SCRCTL_TR("USB pairing timeout must be between 1 and 300000 ms");
        return false;
    }
    return true;
}

void progress(const UsbPairingOptions &options, std::string_view message) {
    if (options.progress) options.progress(message);
}

struct UsbPairingChannel {
    // carrier 引用 connection，connection 引用 Device 的 Stack；按逆序释放。
    std::optional<Device> device;
    std::unique_ptr<ServiceConnection> connection;
    std::unique_ptr<XpcPairingCarrier> carrier;
    std::unique_ptr<wifi::Rppairing> channel;
};

std::unique_ptr<UsbPairingChannel> open_pairing_channel(const std::string &udid,
                                                     int timeout_ms, std::string &error) {
    auto opened = std::make_unique<UsbPairingChannel>();
    opened->device = Device::establish(udid, error, false, true);
    if (!opened->device) return nullptr;
    // 除选择阶段的过滤，再检查实际连接归属；热插拔变化不能降级为网络首次信任。
    if (opened->device->udid() != udid || opened->device->connection_type() != "USB") {
        error = SCRCTL_TR("Pairing requires the selected device to remain connected over USB");
        return nullptr;
    }
    const auto service = opened->device->rsd().service(kPairingService);
    if (!service) {
        error = SCRCTL_TR("Device does not provide the USB RemotePairing service");
        return nullptr;
    }
    if (!service->uses_remote_xpc) {
        error = SCRCTL_TR("USB RemotePairing service does not support RemoteXPC");
        return nullptr;
    }
    opened->connection = opened->device->connect(kPairingService, error);
    if (!opened->connection) return nullptr;

    // 设备可能先发送 ServiceVersion。超时允许继续握手，连接断开则不能继续。
    // 版本消息不包含配对信封；未发请求前出现其它消息属于意外协议状态。
    xpc::Value initial;
    const auto status = opened->connection->wait_message(initial, std::min(timeout_ms, 5000), error);
    if (status == Channel::Wait::Broken) return nullptr;
    if (status == Channel::Wait::Message && !initial.find("ServiceVersion")) {
        error = SCRCTL_TR("Unexpected initial USB RemotePairing message");
        return nullptr;
    }
    error.clear();
    opened->carrier = std::make_unique<XpcPairingCarrier>(*opened->connection, timeout_ms);
    opened->channel = std::make_unique<wifi::Rppairing>(*opened->carrier);
    return opened;
}
} // namespace

std::optional<transport::DeviceRecord> detail::select_pairing_usb_device(
    const std::vector<transport::DeviceRecord> &devices, std::string_view udid, std::string &error) {
    error.clear();
    std::optional<transport::DeviceRecord> selected;
    for (const auto &device : devices) {
        if (!device.is_usb() || (!udid.empty() && device.udid != udid)) continue;
        if (selected) {
            error = SCRCTL_TR("Multiple USB devices match; select one by UDID");
            return std::nullopt;
        }
        selected = device;
    }
    if (!selected) error = SCRCTL_TR("No matching USB device is connected");
    else if (selected->udid.empty()) {
        error = SCRCTL_TR("USB device has no UDID; cannot bind a pairing record");
        return std::nullopt;
    }
    return selected;
}

PairingResult detail::run_usb_pairing_workflow(std::string_view selected_udid,
                                             std::string_view hostname,
                                             const UsbPairingOptions &options,
                                             const PairingWorkflowOperations &operations) {
    PairingResult result;
    result.udid = selected_udid;
    if (!validate(options, result.error)) return result;
    if (selected_udid.empty() || (!options.udid.empty() && options.udid != selected_udid)) {
        result.error = SCRCTL_TR("USB pairing device selection is invalid");
        return result;
    }
    if (!operations.verify || !operations.setup) {
        result.error = SCRCTL_TR("USB pairing workflow operations are unavailable");
        return result;
    }

    try {
        const auto directory = options.pairing_directory.empty() ? wifi::default_record_dir()
                                                                 : options.pairing_directory;
        result.path = wifi::record_path(directory, result.udid);
        std::error_code ec;
        const auto status = std::filesystem::symlink_status(result.path, ec);
        const bool exists = status.type() != std::filesystem::file_type::not_found;
        if (ec && ec != std::errc::no_such_file_or_directory) {
            result.error = SCRCTL_TR("Cannot inspect existing USB pairing record");
            return result;
        }
        std::optional<wifi::PairRecord> existing;
        if (exists && !ec) {
            if (!std::filesystem::is_regular_file(status)) {
                result.error = SCRCTL_TR("Existing USB pairing record is not a regular file");
                return result;
            }
            std::string error;
            existing = wifi::load_record(result.path, error);
            if (existing && existing->udid != result.udid) {
                // 文件名净化可能碰撞。该文件属于另一设备时即使 repair 也不能覆盖。
                result.error = SCRCTL_TR("Existing pairing record belongs to a different device; choose another record directory");
                return result;
            }
            if (!existing && !options.allow_repair) {
                result.error = SCRCTL_TR("Existing pairing record cannot be read; enable USB repair explicitly");
                return result;
            }
            if (existing && existing->complete() && existing->has_peer_identity()) {
                progress(options, SCRCTL_TR("Verifying the existing USB pairing record"));
                const auto verified = operations.verify(*existing, error);
                if (verified.outcome == wifi::VerifyOutcome::Paired) {
                    result.ok = true;
                    result.reused = true;
                    return result;
                }
                if (verified.outcome != wifi::VerifyOutcome::NotPaired) {
                    result.error = SCRCTL_TR("Existing pairing record verification failed: ") +
                        (verified.error.empty() ? error : verified.error);
                    return result;
                }
                if (!options.allow_repair) {
                    result.error = SCRCTL_TR("Device rejected the pairing record; enable USB repair explicitly");
                    return result;
                }
            } else if (existing && !options.allow_repair) {
                result.error = SCRCTL_TR("Existing pairing record is incomplete; enable USB repair explicitly");
                return result;
            }
        }

        if (hostname.empty()) {
            result.error = SCRCTL_TR("Cannot obtain the local hostname for USB pairing");
            return result;
        }
        // 重新配对保持已有主机标识；只有首次配对或不可解析的旧文件才重新派生。
        const auto host_identifier = existing && !existing->host_identifier.empty()
                                         ? existing->host_identifier
                                         : wifi::host_identifier_uuid3(std::string(hostname) + ".scrctl");
        if (host_identifier.empty()) {
            result.error = SCRCTL_TR("Cannot generate a host identifier for USB pairing");
            return result;
        }
        progress(options, SCRCTL_TR("Starting USB pairing; confirm the request on the device"));
        std::string error;
        const auto setup = operations.setup(host_identifier, hostname, selected_udid, error);
        if (!setup.ok) {
            result.error = SCRCTL_TR("USB pair setup failed: ") + (setup.error.empty() ? error : setup.error);
            return result;
        }
        if (setup.record.udid != selected_udid || setup.record.host_identifier != host_identifier ||
            !setup.record.complete() || !setup.record.has_peer_identity()) {
            result.error = SCRCTL_TR("USB pair setup returned an incomplete or mismatched record");
            return result;
        }
        progress(options, SCRCTL_TR("Reconnecting to verify the new USB pairing record"));
        const auto verified = operations.verify(setup.record, error);
        if (verified.outcome != wifi::VerifyOutcome::Paired) {
            result.error = SCRCTL_TR("New USB pairing record verification failed: ") +
                (verified.error.empty() ? error : verified.error);
            return result;
        }
        if (!wifi::save_record(result.path, setup.record, error)) {
            result.error = SCRCTL_TR("Cannot save the verified USB pairing record: ") + error;
            return result;
        }
        result.ok = true;
        return result;
    } catch (const std::exception &) {
        // 不转发 callback/文件异常的 what()，避免意外泄露配对载体或记录内容。
        result.error = SCRCTL_TR("USB pairing workflow failed before completion");
        return result;
    }
}

PairingResult pair_usb_remote(const UsbPairingOptions &options) {
    PairingResult result;
    if (!validate(options, result.error)) return result;
    std::string error;
    const auto devices = Device::list(error);
    if (!error.empty()) {
        result.error = SCRCTL_TR("Cannot enumerate devices for USB pairing: ") + error;
        return result;
    }
    const auto selected = detail::select_pairing_usb_device(devices, options.udid, error);
    if (!selected) { result.error = std::move(error); return result; }

    detail::PairingWorkflowOperations operations;
    operations.verify = [&](const wifi::PairRecord &record, std::string &operation_error) {
        auto opened = open_pairing_channel(selected->udid, options.timeout_ms, operation_error);
        if (!opened) {
            wifi::PairVerifyResult failed;
            failed.error = operation_error;
            return failed;
        }
        return wifi::pair_verify(*opened->channel, record, operation_error);
    };
    operations.setup = [&](std::string_view host_identifier, std::string_view hostname,
                           std::string_view udid, std::string &operation_error) {
        auto opened = open_pairing_channel(selected->udid, options.timeout_ms, operation_error);
        if (!opened) {
            wifi::PairSetupResult failed;
            failed.error = operation_error;
            return failed;
        }
        wifi::PairSetupOptions setup_options;
        setup_options.probe_verify_first = false;
        setup_options.pairing_kind = "setupManualPairing";
        return wifi::pair_setup(*opened->channel, host_identifier, hostname, udid,
                               options.progress, setup_options, operation_error);
    };
    return detail::run_usb_pairing_workflow(selected->udid, wifi::local_hostname(), options, operations);
}
} // namespace scrctl::remote
