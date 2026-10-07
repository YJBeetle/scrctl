#include "i18n/Translation.h"
#include "remote/Device.h"

#include <cstdio>
#include <memory>

#include "remote/RemoteXpc.h"
#include "transport/TcpConnect.h"
#include "wifi/PairVerify.h"
#include "wifi/RemotePairing.h"

namespace scrctl::remote {
namespace {

/// USB 路径通过 lockdown 启动的包隧道入口服务。
constexpr const char *kCoreDeviceProxy = "com.apple.internal.devicecompute.CoreDeviceProxy";

/// verbose 模式报告建立会话的当前阶段，便于定位等待发生在哪一层。
void stage(bool verbose, const char *what) {
    if (verbose) {
        std::fprintf(stderr, SCRCTL_TR("  [stage] %s\n"), what);
    }
}

}  // namespace

std::string proxy_failure_hint(const std::string_view lockdown_error) {
    const std::string e(lockdown_error);
    // 先匹配明确的锁屏和信任错误，再补充服务能力或通用连接提示。
    if (e.find("PasswordProtected") != std::string::npos) {
        return SCRCTL_TR(" (device is locked; unlock it and keep the screen awake before retrying)");
    }
    if (e.find("UserDenied") != std::string::npos || e.find("Trust") != std::string::npos) {
        return SCRCTL_TR(" (computer not trusted; unlock the device and confirm Trust)");
    }
    if (e.find("InvalidService") != std::string::npos) {
        return SCRCTL_TR(" (service unavailable; check Developer Mode and DDI mounting through Xcode)");
    }
    return SCRCTL_TR(" (check DDI mounting and Developer Mode)");
}

std::string mask(std::string_view value, std::size_t keep) {
    if (value.size() <= keep) {
        return std::string(value);
    }
    return "****" + std::string(value.substr(value.size() - keep));
}

Device::Device(Device &&) noexcept = default;
Device &Device::operator=(Device &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    // 先按借用关系释放旧会话；逐成员默认赋值会在旧 RSD 释放前销毁其 Stack。
    rsd_.reset();
    stack_.reset();
    tunnel_.reset();
    lockdown_.reset();
    udid_ = std::move(other.udid_);
    connection_type_ = std::move(other.connection_type_);
    lockdown_ = std::move(other.lockdown_);
    tunnel_ = std::move(other.tunnel_);
    stack_ = std::move(other.stack_);
    rsd_ = std::move(other.rsd_);
    return *this;
}
Device::~Device() = default;

std::vector<transport::DeviceRecord> Device::list(std::string &err) {
    std::vector<transport::DeviceRecord> out;
    auto mux = transport::Usbmux::open(err);
    if (!mux) {
        return out;
    }
    if (!mux->list_devices(out, err)) {
        out.clear();
    }
    return out;
}

std::optional<Device> Device::establish(std::string_view udid, std::string &err, bool verbose) {
    auto mux = transport::Usbmux::open(err);
    if (!mux) {
        return std::nullopt;
    }
    std::vector<transport::DeviceRecord> devices;
    if (!mux->list_devices(devices, err)) {
        return std::nullopt;
    }
    if (devices.empty()) {
        // usbmux 列表为空时提示检查数据线和 USB 连接；配对在后续阶段处理。
        err = SCRCTL_TR(
            "No USB device found (usbmux list is empty). Check the data cable and USB "
            "connection, then reconnect the device. Untrusted devices normally appear in "
            "this list too.");
        return std::nullopt;
    }

    // 未指定 UDID 时只接受唯一设备；选择失败时附带脱敏的候选标识。
    const transport::DeviceRecord *chosen = nullptr;
    if (udid.empty()) {
        if (devices.size() > 1) {
            err = SCRCTL_TR("Connected devices: ") + std::to_string(devices.size()) + SCRCTL_TR("; select one: ");
            for (const auto &d : devices) {
                err += " " + mask(d.udid);
            }
            return std::nullopt;
        }
        chosen = &devices.front();
    } else {
        for (const auto &d : devices) {
            if (d.udid == udid) {
                chosen = &d;
                break;
            }
        }
        if (chosen == nullptr) {
            // UDID 未匹配属于设备选择阶段，此时尚未建立 lockdown 配对会话。
            err = SCRCTL_TR("No connected device matches ") + std::string(mask(udid)) + SCRCTL_TR("; connected devices: ");
            for (const auto &d : devices) {
                err += " " + mask(d.udid);
            }
            err += SCRCTL_TR(" (total ") + std::to_string(devices.size()) + SCRCTL_TR("). Check the selected UDID and USB connection.");
            return std::nullopt;
        }
    }

    std::optional<Device> dev;
    dev.emplace();
    dev->udid_ = chosen->udid;
    dev->connection_type_ = chosen->connection_type;

    stage(verbose, SCRCTL_TR("lockdown pairing session"));
    dev->lockdown_ = transport::Lockdown::establish(chosen->device_id, chosen->udid, err);
    if (!dev->lockdown_) {
        err = SCRCTL_TR("Failed to establish lockdown session: ") + err;
        return std::nullopt;
    }
    stage(verbose, SCRCTL_TR("Start CoreDeviceProxy"));
    auto ep = dev->lockdown_->start_service(kCoreDeviceProxy, err);
    if (!ep) {
        err = SCRCTL_TR("Start ") + std::string(kCoreDeviceProxy) + SCRCTL_TR(" failed: ") + err + proxy_failure_hint(err);
        return std::nullopt;
    }
    stage(verbose, SCRCTL_TR("CDTunnel handshake"));
    auto tunnel = transport::PacketTunnel::establish(chosen->device_id, ep->port,
                                                    dev->lockdown_->identity(), ep->requires_tls,
                                                    err);
    if (!tunnel) {
        err = SCRCTL_TR("Failed to establish packet tunnel: ") + err;
        return std::nullopt;
    }
    // 从配对 HostID 确定性生成稳定 RSD 身份；已有 UUID 保持原值，
    // 非 UUID 标识经同一规则转换，不能为每次连接随机分配 peer UUID。
    auto uuid = peer_uuid_from_host_id(dev->lockdown_->host_id());
    if (!uuid) {
        err = SCRCTL_TR("Cannot construct a stable peer identity from pairing HostID");
        return std::nullopt;
    }
    PeerIdentity identity;
    identity.uuid = *uuid;

    stage(verbose, SCRCTL_TR("Connect to RSD inside tunnel and read directory"));
    if (!dev->finish_session(std::move(*tunnel), identity, verbose, err)) {
        return std::nullopt;
    }
    return dev;
}

bool Device::finish_session(transport::PacketTunnel &&tunnel, PeerIdentity identity, bool verbose,
                            std::string &err) {
    tunnel_ = std::make_unique<transport::PacketTunnel>(std::move(tunnel));
    stage(verbose, SCRCTL_TR("Connect to RSD inside tunnel and read directory"));

    stack_ =
        std::make_unique<net::Stack>(*tunnel_, tunnel_->params().client_address,
                                     tunnel_->params().server_address);
    if (!stack_->addresses_ok()) {
        err = SCRCTL_TR("Tunnel returned an invalid IPv6 address");
        return false;
    }
    // 隧道线程必须先于 TCP/RSD 连接启动，负责将收到的 IPv6 包交给协议栈。
    if (!stack_->start_pump(err)) {
        return false;
    }
    rsd_ = Rsd::open(*stack_, *tunnel_, identity, err, verbose);
    if (!rsd_) {
        return false;
    }
    stage(verbose, SCRCTL_TR("Ready"));
    return true;
}

std::optional<Device> Device::establish_wifi(const std::string &address,
                                             const wifi::PairRecord &record, std::string &err,
                                             bool verbose, uint16_t port) {
    // 本地记录问题应在连接前报出，避免网络不可达掩盖重新配对的原因。
    if (!record.complete()) {
        err = SCRCTL_TR("Pairing record incomplete; cannot sign");
        return std::nullopt;
    }
    if (!record.has_peer_identity()) {
        err = SCRCTL_TR("Pairing record has no device identity; pair again over USB");
        return std::nullopt;
    }
    std::optional<Device> dev;
    dev.emplace();
    dev->udid_ = record.udid;
    dev->connection_type_ = "WiFi";

    stage(verbose, SCRCTL_TR("LAN pair-verify"));
    auto control = transport::connect_tcp(address, port, 5000, err);
    if (!control) {
        err = SCRCTL_TR("Cannot connect to device RemotePairing port ") + address + ":" + std::to_string(port) +
              "：" + err + SCRCTL_TR(" (check that the device is reachable on this network and has been paired)");
        return std::nullopt;
    }
    wifi::SocketStream stream(*control);
    wifi::FramedCarrier carrier(stream);
    wifi::Rppairing channel(carrier);
    const wifi::PairVerifyResult verified = wifi::pair_verify(channel, record, err);
    if (verified.outcome != wifi::VerifyOutcome::Paired) {
        if (verified.outcome == wifi::VerifyOutcome::NotPaired) {
            err = SCRCTL_TR("Device rejected pairing record: ") + mask(record.udid, 8) +
                  SCRCTL_TR(" is not paired with this device, or its pairing was removed");
        } else {
            err = SCRCTL_TR("pair-verify failed: ") + verified.error;
        }
        return std::nullopt;
    }

    stage(verbose, SCRCTL_TR("Request device tunnel port"));
    const auto listener = wifi::request_tcp_listener(channel, verified.shared_secret, err);
    if (!listener) {
        return std::nullopt;
    }
    // 已取得隧道监听端口，关闭配对控制通道；隧道使用独立 TCP 连接。
    control->close();

    stage(verbose, SCRCTL_TR("TLS-PSK and CDTunnel handshake"));
    auto tunnel_sock = transport::connect_tcp(address, *listener, 5000, err);
    if (!tunnel_sock) {
        err = SCRCTL_TR("Cannot connect to tunnel port ") + std::to_string(*listener) + "：" + err;
        return std::nullopt;
    }
    auto tunnel =
        transport::PacketTunnel::establish_psk(std::move(*tunnel_sock), verified.shared_secret,
                                               err);
    if (!tunnel) {
        err = SCRCTL_TR("Failed to establish packet tunnel: ") + err;
        return std::nullopt;
    }

    // 使用配对记录中持久化的 host identifier，保持多次会话的 RSD 身份一致。
    const auto uuid = parse_uuid_text(record.host_identifier);
    if (!uuid) {
        err = SCRCTL_TR("Pairing host identifier is not a valid UUID; cannot construct a stable peer identity");
        return std::nullopt;
    }
    PeerIdentity identity;
    identity.uuid = *uuid;
    if (!dev->finish_session(std::move(*tunnel), identity, verbose, err)) {
        return std::nullopt;
    }
    return dev;
}

std::string Device::property(std::string_view key) const {
    const auto *props = rsd_ ? rsd_->properties() : nullptr;
    const auto *v = props != nullptr ? props->find(key) : nullptr;
    return v != nullptr ? v->as_string_or("") : std::string();
}

std::unique_ptr<ServiceConnection> Device::connect(std::string_view service_name, std::string &err,
                                                   bool verbose) {
    if (!rsd_) {
        err = SCRCTL_TR("Session not established");
        return nullptr;
    }
    return rsd_->connect_service(service_name, err, verbose);
}

CallResult Device::feature_call(std::string_view service_name,
                                std::string_view feature_identifier,
                                std::string_view action_identifier, const xpc::Value &input,
                                xpc::Value &output, std::string &err, bool verbose,
                                int timeout_ms) {
    if (rsd_ && !rsd_->supports(service_name, feature_identifier)) {
        err = std::string(service_name) + SCRCTL_TR(" does not advertise feature ") + std::string(feature_identifier);
        return CallResult::DeviceError;
    }
    auto conn = connect(service_name, err, verbose);
    if (conn == nullptr) {
        return CallResult::TransportError;
    }
    return conn->invoke(feature_identifier, action_identifier, input, output, timeout_ms, err);
}

bool Device::feature(std::string_view service_name, std::string_view feature_identifier,
                     std::string_view action_identifier, const xpc::Value &input,
                     xpc::Value &output, std::string &err, bool verbose, int timeout_ms) {
    return feature_call(service_name, feature_identifier, action_identifier, input, output, err,
                        verbose, timeout_ms) == CallResult::Ok;
}

}  // namespace scrctl::remote
