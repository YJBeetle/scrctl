#include "remote/Rsd.h"

#include <chrono>
#include <cstdio>
#include <random>
#include <sstream>

namespace scrctl::remote {
namespace {

/// DDI 侧期望的协议版本与 CoreDevice 版本串。设备会按这个决定它讲哪一版方言，
/// 所以是从真机抓来的观测值，不是我们随意的版本号。
constexpr int64_t kDdiProtocolVersion = 2;
constexpr const char *kCoreDeviceVersionString = "629.3";

uint16_t to_port(const xpc::Value *v) {
    if (v == nullptr) {
        return 0;
    }
    if (v->is_string()) {
        // 设备发的是字符串端口。按整数读会拿到 0，然后连接被拒，现场看起来
        // 像"权限不够"，所以这里两种形态都吃。
        unsigned long parsed = 0;
        std::istringstream in(v->string);
        in >> parsed;
        return static_cast<uint16_t>(parsed & 0xFFFF);
    }
    return static_cast<uint16_t>(v->as_int_or(0) & 0xFFFF);
}

std::string uuid_text_from(std::mt19937_64 &rng) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            s += '-';
            continue;
        }
        const unsigned v = i == 14 ? 4 : (i == 19 ? ((rng() & 0x3) | 0x8) : (rng() & 0xF));
        s += kHex[v & 0xF];
    }
    return s;
}

}  // namespace

std::string random_uuid_text() {
    // 追踪号用的随机源，不是密钥；用 random_device 播种即可。
    static thread_local std::mt19937_64 rng(
        std::random_device{}() ^ static_cast<uint64_t>(
                                     std::chrono::steady_clock::now().time_since_epoch().count()));
    return uuid_text_from(rng);
}

xpc::Value core_device_request(std::string_view feature_identifier,
                              std::string_view action_identifier, const xpc::Value &input) {
    auto d = xpc::make_dict();
    xpc::dict_set(d, "CoreDevice.CoreDeviceDDIProtocolVersion",
                  xpc::make_int64(kDdiProtocolVersion));

    auto version = xpc::make_dict();
    auto components = xpc::make_array();
    for (const auto part : {629ULL, 3ULL}) {
        xpc::array_push(components, xpc::make_uint64(part));
    }
    xpc::dict_set(version, "components", std::move(components));
    xpc::dict_set(version, "originalComponentsCount", xpc::make_int64(2));
    xpc::dict_set(version, "stringValue", xpc::make_string(kCoreDeviceVersionString));
    xpc::dict_set(d, "CoreDevice.coreDeviceVersion", std::move(version));

    xpc::dict_set(d, "CoreDevice.deviceIdentifier", xpc::make_string(random_uuid_text()));
    xpc::dict_set(d, "CoreDevice.input", input);
    xpc::dict_set(d, "CoreDevice.invocationIdentifier", xpc::make_string(random_uuid_text()));
    if (!feature_identifier.empty()) {
        xpc::dict_set(d, "CoreDevice.featureIdentifier", xpc::make_string(std::string(feature_identifier)));
        // action 恒为空字典，真正的动作号在 actionIdentifier 里。
        xpc::dict_set(d, "CoreDevice.action", xpc::make_dict());
    }
    if (!action_identifier.empty()) {
        xpc::dict_set(d, "CoreDevice.actionIdentifier", xpc::make_string(std::string(action_identifier)));
    }
    return d;
}

// ------------------------------------------------------------- 服务连接 ------

std::unique_ptr<ServiceConnection> ServiceConnection::open(net::Stack &stack,
                                                           const ServiceInfo &service,
                                                           std::string &err, bool verbose) {
    auto conn = std::unique_ptr<ServiceConnection>(new ServiceConnection());
    conn->tcp_ = std::make_unique<net::TcpStream>(stack);
    if (!conn->tcp_->connect(service.port, err)) {
        err = "连 " + service.name + " 端口 " + std::to_string(service.port) + " 失败: " + err;
        return nullptr;
    }
    if (!service.uses_remote_xpc) {
        // lockdown shim 那批是裸协议（连上就是 lockdown 帧），不套 HTTP/2。
        return conn;
    }
    auto channel = Channel::open(*conn->tcp_, err, verbose);
    if (!channel) {
        err = service.name + " 的 HTTP/2 握手失败: " + err;
        return nullptr;
    }
    // 服务连接上不再喊一次设备身份：那是 RSD 控制通道独有的步骤，设备这边
    // 没有这个预期，发过去会被当成一次普通请求，后面全乱。
    conn->channel_ = std::make_unique<Channel>(std::move(*channel));
    return conn;
}

bool ServiceConnection::call(const xpc::Value &request, xpc::Value &reply, int timeout_ms,
                             std::string &err) {
    if (channel_ == nullptr) {
        err = "这条服务连接不是 RemoteXPC 服务";
        return false;
    }
    return channel_->call(request, reply, timeout_ms, err);
}

bool ServiceConnection::invoke(std::string_view feature_identifier,
                               std::string_view action_identifier, const xpc::Value &input,
                               xpc::Value &output, int timeout_ms, std::string &err) {
    const auto request = core_device_request(feature_identifier, action_identifier, input);
    xpc::Value reply;
    if (!call(request, reply, timeout_ms, err)) {
        return false;
    }
    const auto *out = reply.find("CoreDevice.output");
    if (out != nullptr) {
        output = *out;
        return true;
    }
    // 设备侧的失败写法是 CoreDevice.error = {code, userInfo.NSLocalizedDescription}。
    // 只回一句"调用失败"会把真正的因由丢干净，所以人话必须捞出来。
    output = xpc::make_dict();
    const auto *error = reply.find("CoreDevice.error");
    if (error == nullptr) {
        err = std::string(feature_identifier) + " 失败，回信里没有 CoreDevice.output: " +
              xpc::describe(reply).substr(0, 300);
        return false;
    }
    const std::string detail =
        error->at("userInfo").at("NSLocalizedDescription").as_string_or("");
    const auto code = error->at("code").as_int_or(0);
    err = std::string(feature_identifier) + " 失败";
    if (!detail.empty()) {
        err += "：" + detail;
    }
    err += "（code " + std::to_string(code) + "）";
    return false;
}

// ------------------------------------------------------------------ RSD ------

std::optional<Rsd> Rsd::open(net::Stack &stack, transport::PacketTunnel &tunnel,
                             const PeerIdentity &identity, std::string &err, bool verbose) {
    const auto &p = tunnel.params();
    std::optional<Rsd> rsd;
    rsd.emplace(stack, identity);

    // Channel 借引用用 TcpStream，所以两者的所有权都在 Rsd 上，且声明顺序
    // 决定析构顺序：control_ 必须先于 tcp_ 析构。
    rsd->tcp_ = std::make_unique<net::TcpStream>(stack);
    if (!rsd->tcp_->connect(p.rsd_port, err)) {
        err = "连 RSD 端口 " + std::to_string(p.rsd_port) + " 失败: " + err;
        return std::nullopt;
    }
    auto channel = Channel::open(*rsd->tcp_, err, verbose);
    if (!channel) {
        err = "RSD 控制通道握手失败: " + err;
        return std::nullopt;
    }
    rsd->control_ = std::make_unique<Channel>(std::move(*channel));
    if (!rsd->control_->announce_device(identity, err)) {
        return std::nullopt;
    }
    const auto *info = rsd->control_->peer_info();
    const auto *services = info != nullptr ? info->find("Services") : nullptr;
    if (services == nullptr || !services->is_dict()) {
        err = "peer_info 里没有 Services";
        return std::nullopt;
    }
    for (const auto &entry : services->dict) {
        if (!entry.value.is_dict()) {
            // 目录条目长得不像字典就跳过：少认识一个服务只是功能缺失，
            // 按着猜的形状读下去才是崩溃。
            continue;
        }
        ServiceInfo si;
        si.name = entry.key;
        si.port = to_port(entry.value.find("Port"));
        si.entitlement = entry.value.at("Entitlement").as_string_or("");
        const auto &props = entry.value.at("Properties");
        if (props.is_dict()) {
            si.uses_remote_xpc = props.at("UsesRemoteXPC").as_bool_or(false);
            si.encrypt_socket_data = props.at("EncryptSocketData").as_bool_or(false);
            const auto &features = props.at("Features");
            for (const auto &f : features.array) {
                si.features.push_back(f.as_string_or(""));
            }
        }
        rsd->services_.push_back(std::move(si));
    }
    return rsd;
}

std::optional<ServiceInfo> Rsd::service(std::string_view name) const {
    for (const auto &s : services_) {
        if (s.name == name) {
            return s;
        }
    }
    return std::nullopt;
}

bool Rsd::has_service(std::string_view name) const { return service(name).has_value(); }

const xpc::Value *Rsd::service_entry(std::string_view name) const {
    const auto *info = control_ != nullptr ? control_->peer_info() : nullptr;
    const auto *services = info != nullptr ? info->find("Services") : nullptr;
    return services != nullptr ? services->find(name) : nullptr;
}

bool Rsd::supports(std::string_view service_name, std::string_view feature) const {
    const auto info = service(service_name);
    if (!info) {
        return false;
    }
    for (const auto &f : info->features) {
        if (f == feature) {
            return true;
        }
    }
    return false;
}

std::unique_ptr<ServiceConnection> Rsd::connect_service(std::string_view name, std::string &err,
                                                        bool verbose) {
    const auto info = service(name);
    if (!info) {
        err = "设备目录里没有服务 " + std::string(name);
        return nullptr;
    }
    return ServiceConnection::open(*stack_, *info, err, verbose);
}

const xpc::Value *Rsd::properties() const {
    const auto *info = control_ != nullptr ? control_->peer_info() : nullptr;
    return info != nullptr ? info->find("Properties") : nullptr;
}

}  // namespace scrctl::remote
