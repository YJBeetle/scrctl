#include "remote/Device.h"

#include <cstdio>
#include <memory>

#include "remote/RemoteXpc.h"

namespace scrctl::remote {
namespace {

/// 起隧道的入口服务。iOS 17+ 的 USB 主路径就是它，非 root 可达。
constexpr const char *kCoreDeviceProxy = "com.apple.internal.devicecompute.CoreDeviceProxy";

/// 分阶段进展。这条链上有好几处能静默阻塞（usbmuxd 那条读没有超时），所以
/// 「走到哪了」必须是可见的，否则卡住时只剩一片空白。
void stage(bool verbose, const char *what) {
    if (verbose) {
        std::fprintf(stderr, "  [阶段] %s\n", what);
    }
}

}  // namespace

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
        err = "没有在连设备";
        return std::nullopt;
    }

    // 选择逻辑照 scrcpy 的肌肉记忆：不指定就唯一设备自动选中，多台则要明说。
    // 报错时把候选连尾号一起给出，用户能直接对着 USB 上的设备认。
    const transport::DeviceRecord *chosen = nullptr;
    if (udid.empty()) {
        if (devices.size() > 1) {
            err = "接着 " + std::to_string(devices.size()) + " 台设备，得指定其中一台：";
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
            err = "没有在连设备匹配 " + std::string(mask(udid)) + "（注意信任与锁屏状态）";
            return std::nullopt;
        }
    }

    std::optional<Device> dev;
    dev.emplace();
    dev->udid_ = chosen->udid;
    dev->connection_type_ = chosen->connection_type;

    stage(verbose, "lockdown 配对 session");
    dev->lockdown_ = transport::Lockdown::establish(chosen->device_id, chosen->udid, err);
    if (!dev->lockdown_) {
        err = "lockdown 会话建立失败: " + err;
        return std::nullopt;
    }
    stage(verbose, "起 CoreDeviceProxy");
    auto ep = dev->lockdown_->start_service(kCoreDeviceProxy, err);
    if (!ep) {
        err = "起 " + std::string(kCoreDeviceProxy) + " 失败: " + err +
              "（DDI 是否已挂载？开发者模式是否开着？）";
        return std::nullopt;
    }
    stage(verbose, "CDTunnel 握手");
    auto tunnel = transport::PacketTunnel::establish(chosen->device_id, ep->port,
                                                    dev->lockdown_->identity(), ep->requires_tls,
                                                    err);
    if (!tunnel) {
        err = "包隧道建立失败: " + err;
        return std::nullopt;
    }
    dev->tunnel_ = std::make_unique<transport::PacketTunnel>(std::move(*tunnel));

    // peer UUID 用配对记录里的 HostID：设备上每条隧道只保留一个 RSD 连接，而且
    // 会记住被它换掉的那个 peer，UUID 一变就把整台机器重新 attach、关掉所有已
    // 公布的服务端口。所以这个值必须跨进程、跨重启稳定。
    auto uuid = parse_uuid_text(dev->lockdown_->host_id());
    if (!uuid) {
        err = "配对记录里的 HostID 不是合法 UUID，无法给出稳定的 peer 身份";
        return std::nullopt;
    }
    PeerIdentity identity;
    identity.uuid = *uuid;

    stage(verbose, "隧道内连 RSD 并读目录");
    dev->stack_ =
        std::make_unique<net::Stack>(*dev->tunnel_, dev->tunnel_->params().client_address,
                                     dev->tunnel_->params().server_address);
    if (!dev->stack_->addresses_ok()) {
        err = "隧道给的地址不是合法 IPv6";
        return std::nullopt;
    }
    dev->rsd_ = Rsd::open(*dev->stack_, *dev->tunnel_, identity, err, verbose);
    if (!dev->rsd_) {
        return std::nullopt;
    }
    stage(verbose, "就绪");
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
        err = "会话没有建立";
        return nullptr;
    }
    return rsd_->connect_service(service_name, err, verbose);
}

bool Device::feature(std::string_view service_name, std::string_view feature_identifier,
                     std::string_view action_identifier, const xpc::Value &input,
                     xpc::Value &output, std::string &err, bool verbose, int timeout_ms) {
    if (rsd_ && !rsd_->supports(service_name, feature_identifier)) {
        err = std::string(service_name) + " 没有声明 feature " + std::string(feature_identifier);
        return false;
    }
    auto conn = connect(service_name, err, verbose);
    if (conn == nullptr) {
        return false;
    }
    return conn->invoke(feature_identifier, action_identifier, input, output, timeout_ms, err);
}

}  // namespace scrctl::remote
