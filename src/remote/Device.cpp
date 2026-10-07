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

/// 起隧道的入口服务。iOS 17+ 的 USB 主路径就是它，非 root 可达。
constexpr const char *kCoreDeviceProxy = "com.apple.internal.devicecompute.CoreDeviceProxy";

/// 分阶段进展。这条链上有好几处能静默阻塞（usbmuxd 那条读没有超时），所以
/// 「走到哪了」必须是可见的，否则卡住时只剩一片空白。
void stage(bool verbose, const char *what) {
    if (verbose) {
        std::fprintf(stderr, SCRCTL_TR("  [stage] %s\n"), what);
    }
}

}  // namespace

std::string proxy_failure_hint(const std::string_view lockdown_error) {
    const std::string e(lockdown_error);
    // 锁屏：iOS 只在解锁状态下允许起开发者服务。这一条必须排在最前面——它长得像
    // "权限不够"，而正确答案是"把屏幕解开"，不是去查 DDI。
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
Device &Device::operator=(Device &&) noexcept = default;
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
        // 这句原先只说"没有在连设备"，而最常见的原因根本不在软件层：今天我自己就撞过
        // 一回——线是只供电不传数据的，设备在旁边充了一晚上，我们这边列表是空的。
        err = SCRCTL_TR(
            "No USB device found (usbmux list is empty). Check the data cable and USB "
            "connection, then reconnect the device. Untrusted devices normally appear in "
            "this list too.");
        return std::nullopt;
    }

    // 选择逻辑照 scrcpy 的肌肉记忆：不指定就唯一设备自动选中，多台则要明说。
    // 报错时把候选连尾号一起给出，用户能直接对着 USB 上的设备认。
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
            // 这一句原先写的是"注意信任与锁屏状态"，那是**错的指向**：没点信任、锁着屏的
            // 设备照样会出现在这个列表里（信任是在后面 lockdown 那一步才要的东西）。
            // 报了 UDID 却没匹配上，绝大多数就是"插的不是这台"或"这台掉线了"。
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
    // RSD 身份必须跨进程和重启稳定。本轮 Windows AMDS 记录的 HostID 是
    // 27 字符的不透明标识，不能直接当 UUID 解析；仍从原配对身份确定性生成。
    // 已是 UUID 的 HostID 保持原值，避免改变已有 macOS / Linux 会话的身份。
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
    // 泵线程必须在任何连接之前起来：端点只从自己的队列取数据，没人替它们读隧道。
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
    // 控制通道到此为止：隧道是**另一条** TCP 连接，端口是刚才要来的那个。
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

    // peer UUID 用我们自己注册时的 host identifier。和 USB 那条一样的规矩：这个值
    // 必须跨进程、跨重启稳定，否则设备每次都要把这台机器重新 attach 一遍。
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
