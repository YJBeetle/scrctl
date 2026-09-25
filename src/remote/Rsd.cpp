#include "remote/Rsd.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

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
    // 版本号原则上是写死的 629.3（这是我们工具链里 CoreDevice 框架的版本），但留一个
    // 环境变量口子是有原因的：苹果自己的客户端在抓包里申报的是 **642.16**，而它的会话
    // 报着同样的 `timeout:20` 却从不到期。这一位在很长一份"假设清单"里被判过"不是它"，
    // 但那条判据是推断（"p3 也发 629.3 且也死"）——p3 发 629.3 会死只能说明 629.3 不被
    // 豁免，**说明不了 642.16 会怎样**。设备按客户端申报的版本号来决定要不要回收会话，
    // 对苹果这种兼容性策略来说是完全正常的做法，所以必须真发一次 642.16 才算判掉。
    //
    // `SCRCTL_COREDEVICE_VERSION="major.minor[.patch...]"`，逗号分隔也行；不设就是 629.3。
    std::vector<uint64_t> parts {629, 3};
    const char *override = std::getenv("SCRCTL_COREDEVICE_VERSION");
    if (override != nullptr && override [0] != '\0') {
        std::vector<uint64_t> parsed;
        std::string_view sv(override);
        std::size_t pos = 0;
        while (pos < sv.size()) {
            const auto sep = sv.find_first_of(". ", pos);
            // sep==npos 时要取"到串尾"，不是取长度 0——写成 `std::size_t{}` 会让最后一段
            // 变成空串，于是整份申报被当成非法而退回 629.3，而表面上实验是"跑了"的。
            // （第一版就是这个错，导致 642.16 那一臂其实发的还是 629.3。）
            const auto token = sep == std::string_view::npos ? sv.substr(pos)
                                                             : sv.substr(pos, sep - pos);
            bool bad = token.empty();
            uint64_t value = 0;
            for (char c : token) {
                if (c < '0' || c > '9') {
                    bad = true;
                    break;
                }
                value = value * 10 + static_cast<uint64_t>(c - '0');
            }
            if (bad) {
                std::fprintf(stderr, "SCRCTL_COREDEVICE_VERSION 里有非数字段: %s（按 629.3 走）\n",
                             override);
                parsed.clear();
                break;
            }
            parsed.push_back(value);
            if (sep == std::string_view::npos) {
                break;
            }
            pos = sep + 1;
        }
        if (!parsed.empty()) {
            parts = std::move(parsed);
            std::fprintf(stderr, "  [注] coreDeviceVersion 按环境变量的申报改成了 %s（默认 629.3）\n",
                         override);
        }
    }
    for (const uint64_t part : parts) {
        xpc::array_push(components, xpc::make_uint64(part));
    }
    xpc::dict_set(version, "components", std::move(components));
    xpc::dict_set(version, "originalComponentsCount",
                  xpc::make_int64(static_cast<int64_t>(parts.size())));
    // stringValue 必须和 components 一起改：只换 components 而文本还写着 629.3，等于给
    // 设备两条互相矛盾的申报，测出来的结果没法归因到"版本号"这一位上。
    std::string version_text = kCoreDeviceVersionString;
    if (override != nullptr && override [0] != '\0') {
        version_text = override;
        for (auto &c : version_text) {
            if (c == ' ') {
                c = '.';
            }
        }
    }
    xpc::dict_set(version, "stringValue", xpc::make_string(version_text));
    xpc::dict_set(d, "CoreDevice.coreDeviceVersion", std::move(version));

    // `CoreDevice.deviceIdentifier`：抓包里苹果两次请求用的是**同一个** UUID
    // （0E81B5E0-…，即这台设备在 CoreDevice 里的稳定标识），而我们一直每次调用换一个随机数。
    // 这个字段的语义是"我在跟哪台设备说话"，随手换等于每次自称是新设备——设备完全可能
    // 因此按"陌生来客"的策略处置这条会话（比如套上那个到点回收的租期）。
    //
    // 留一个环境变量口子 `SCRCTL_DEVICE_IDENTIFIER` 把它钉住，是为了能单独判这一位；
    // 没设就还是原来的每次随机（不拿一个未验证的假设直接改掉产品行为）。
    const char *pinned = std::getenv("SCRCTL_DEVICE_IDENTIFIER");
    const std::string device_id =
        pinned != nullptr && pinned [0] != '\0' ? std::string(pinned) : random_uuid_text();
    xpc::dict_set(d, "CoreDevice.deviceIdentifier", xpc::make_string(device_id));
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

bool ServiceConnection::send_only(const xpc::Value &request, std::string &err) {
    if (channel_ == nullptr) {
        err = "这条服务连接不是 RemoteXPC 服务";
        return false;
    }
    return channel_->send_request(request, false, err);
}

bool ServiceConnection::service(int timeout_ms, std::string &err) {
    if (channel_ == nullptr) {
        err = "这条服务连接不是 RemoteXPC 服务";
        return false;
    }
    return channel_->service(timeout_ms, err);
}

namespace {

/// 设备侧的失败写法是 CoreDevice.error = {code, userInfo.NSLocalizedDescription}。
/// 只回一句"调用失败"会把真正的因由丢干净，所以人话必须捞出来。
std::string device_error_text(std::string_view feature_identifier, const xpc::Value &error) {
    const std::string detail =
        error.at("userInfo").at("NSLocalizedDescription").as_string_or("");
    const auto code = error.at("code").as_int_or(0);
    // 设备答了，而且答的是"不同意"：这是语义结果，重试只会再拿到同一句话。
    std::string err = std::string(feature_identifier) + " 失败";
    if (!detail.empty()) {
        err += "：" + detail;
    }
    err += "（code " + std::to_string(code) + "）";
    // NSDebugDescription 是"缺哪个键 / 哪个类型不对"的正式答案，NSCodingPath 指出
    // 是哪一个键。这两个必须**原样、不截断**地交出去：整个 error 字典的 describe
    // 会把长字符串掐掉（深层路径正好是最长的那段），而探协议时恰恰要读那半截。
    const std::string debug = error.at("userInfo").at("NSDebugDescription").as_string_or("");
    if (!debug.empty()) {
        err += "\n  NSDebugDescription: " + debug;
        // NSCodingPath 是个数组，不是字符串。
        err += "\n  NSCodingPath: " + xpc::describe(error.at("userInfo").at("NSCodingPath"));
    }
    if (detail.empty() && debug.empty()) {
        // 什么话都没有就把整个 error 交出去，别只报一个数字。
        err += "；error 原文: " + xpc::describe(error).substr(0, 600);
    }
    return err;
}

}  // namespace

CallResult ServiceConnection::invoke(std::string_view feature_identifier,
                                     std::string_view action_identifier, const xpc::Value &input,
                                     xpc::Value &output, int timeout_ms, std::string &err) {
    const auto request = core_device_request(feature_identifier, action_identifier, input);
    xpc::Value reply;
    if (!call(request, reply, timeout_ms, err)) {
        // 连回信都没拿到，无从判断设备同不同意——这正是"换条连接再试一次"可能有
        // 结果的那一类。
        return CallResult::TransportError;
    }
    const auto *out = reply.find("CoreDevice.output");
    if (out != nullptr) {
        output = *out;
        return CallResult::Ok;
    }
    output = xpc::make_dict();
    const auto *error = reply.find("CoreDevice.error");
    if (error == nullptr) {
        err = std::string(feature_identifier) + " 失败，回信里没有 CoreDevice.output: " +
              xpc::describe(reply).substr(0, 300);
        return CallResult::DeviceError;
    }
    err = device_error_text(feature_identifier, *error);
    return CallResult::DeviceError;
}

CallResult ServiceConnection::stream(std::string_view feature_identifier,
                                     std::string_view action_identifier, const xpc::Value &input,
                                     const std::function<bool(const xpc::Value &)> &on_element,
                                     int timeout_ms, std::string &err) {
    if (channel_ == nullptr) {
        err = "这条服务连接不是 XPC 通道，流式 feature 走不了";
        return CallResult::TransportError;
    }
    const auto request = core_device_request(feature_identifier, action_identifier, input);
    if (!channel_->send_request(request, true, err)) {
        return CallResult::TransportError;
    }
    for (;;) {
        xpc::Value reply;
        if (!channel_->receive(reply, timeout_ms, err)) {
            return CallResult::TransportError;
        }
        const auto *status = reply.find("CoreDevice.XPCMessageKey.sideChannelStatus");
        if (status == nullptr) {
            // 整条流失败时设备发的是一个普通 error 回信，不是 sideChannelStatus。
            const auto *error = reply.find("CoreDevice.error");
            if (error != nullptr) {
                err = device_error_text(feature_identifier, *error);
            } else {
                err = std::string(feature_identifier) +
                      " 的回信既没有 sideChannelStatus 也没有 error: " +
                      xpc::describe(reply).substr(0, 300);
            }
            return CallResult::DeviceError;
        }
        if (status->find("receivedError") != nullptr) {
            err = std::string(feature_identifier) + " 中途失败：" +
                  xpc::describe(status->at("receivedError")).substr(0, 400);
            return CallResult::DeviceError;
        }
        if (status->find("finishStreaming") != nullptr) {
            return CallResult::Ok;
        }
        const auto *pushing = status->find("pushing");
        const auto *elements = pushing == nullptr ? nullptr : pushing->find("elements");
        if (elements == nullptr) {
            continue;  // 空批次
        }
        for (const auto &element : elements->array) {
            if (!on_element(element)) {
                return CallResult::Ok;  // 调用方说够了
            }
        }
    }
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
