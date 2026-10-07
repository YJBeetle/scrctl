// RemotePairing 研究探针：验证已有记录、检查 Wi-Fi 隧道，或通过 USB/字节流建立配对。
// 默认验证路径需要可达的设备地址和已有记录；独立模式由命令行帮助说明。
// --record 读取 scrctl 格式；--pmd3-record 用于与外部 plist 记录互操作，需明确提供
// host identifier，因为该格式没有保存这一字段，而签名计算必须使用原配对时的值。
// 标识符使用 mask 遮蔽，密钥摘要只显示长度与是否存在；verbose 保留协议字节跟踪。
#include <unistd.h>

#include <CLI/CLI.hpp>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "i18n/CliLanguage.h"
#include "i18n/Translation.h"
#include "json/Json.h"
#include "net/Stack.h"
#include "remote/PairingChannel.h"
#include "remote/RemoteXpc.h"
#include "remote/Rsd.h"
#include "plist/Plist.h"
#include "transport/Lockdown.h"
#include "transport/TcpConnect.h"
#include "transport/Tunnel.h"
#include "transport/Usbmux.h"
#include "wifi/Crypto.h"
#include "wifi/PairRecord.h"
#include "wifi/PairSetup.h"
#include "wifi/PairVerify.h"
#include "wifi/Rppairing.h"

namespace {

using scrctl::wifi::Bytes;
using scrctl::wifi::j_bool;
using scrctl::wifi::j_int;
using scrctl::wifi::j_obj;
using scrctl::wifi::j_str;

std::string mask(std::string_view s, size_t keep = 4) {
    if (s.size() <= keep) {
        return std::string(s);
    }
    return "****" + std::string(s.substr(s.size() - keep));
}

std::optional<std::string> read_file(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

scrctl::json::Value j_arr(std::vector<scrctl::json::Value> items) {
    return scrctl::json::Value(std::move(items));
}

/// 将外部 plist 配对记录转换成探针使用的记录结构；产品连接路径不调用此适配器。
std::optional<scrctl::wifi::PairRecord> from_foreign_record(const std::string &text,
                                                           std::string_view host_id,
                                                           std::string &err) {
    const std::optional<scrctl::plist::Value> parsed = scrctl::plist::parse(text);
    if (!parsed) {
        err = SCRCTL_TR("Foreign pairing record is not a valid plist");
        return std::nullopt;
    }
    const scrctl::plist::Value *priv = parsed->find("private_key");
    const scrctl::plist::Value *pub = parsed->find("public_key");
    if (priv == nullptr || pub == nullptr || priv->kind != scrctl::plist::Kind::Data ||
        pub->kind != scrctl::plist::Kind::Data) {
        err = SCRCTL_TR("Foreign pairing record requires private_key and public_key Data fields");
        return std::nullopt;
    }
    scrctl::wifi::PairRecord record;
    record.host_private_key = priv->data;
    record.host_public_key = pub->data;
    record.host_identifier = host_id;
    // 此格式未保存 UDID；调用方须提供设备标识符，再决定记录文件名。
    return record;
}

void print_handshake(const scrctl::json::Value &device_handshake) {
    const scrctl::json::Value *version = scrctl::json::find(device_handshake, "wireProtocolVersion");
    std::printf(SCRCTL_TR("  Device wireProtocolVersion = %lld\n"),
                static_cast<long long>(version != nullptr ? scrctl::json::as_int_or(*version, 0) : 0));
    const scrctl::json::Value *options = scrctl::json::find(device_handshake, "deviceOptions");
    if (options != nullptr) {
        // 字段名和布尔值保留协议形式，避免语言选择改变结构化诊断的含义。
        std::printf("  deviceOptions:");
        for (const auto &key : options->items()) {
            std::printf(" %s=%s", key.key().c_str(),
                        key.value().is_boolean()
                            ? (key.value().get<bool>() ? "true" : "false")
                            : "?");
        }
        std::printf("\n");
    }
    const scrctl::json::Value *peer = scrctl::json::find(device_handshake, "peerDeviceInfo");
    if (peer != nullptr) {
        const scrctl::json::Value *identifier = scrctl::json::find(*peer, "identifier");
        std::printf(SCRCTL_TR("  Device identifier on this channel = %s\n"),
                    identifier != nullptr ? mask(scrctl::json::as_string_or(*identifier)).c_str() : SCRCTL_TR("(not provided)"));
    }
}

/// 记录字节流每次读写的长度和前 400 字节的可打印 ASCII。
/// 跟踪仅用于观察线上的原始内容，不解码或翻译协议字段。
class TracingStream final : public scrctl::wifi::ByteStream {
public:
    TracingStream(scrctl::wifi::ByteStream &inner, bool verbose) : inner_(inner), verbose_(verbose) {}

    bool write_all(const void *data, size_t len, std::string &err) override {
        if (verbose_) {
            const auto *p = static_cast<const uint8_t *>(data);
            std::fprintf(stderr, SCRCTL_TR("  Send %zu bytes:"), len);
            for (size_t i = 0; i < len && i < 400; ++i) {
                std::fputc(p[i] >= 0x20 && p[i] < 0x7F ? p[i] : '.', stderr);
            }
            std::fprintf(stderr, "\n");
        }
        return inner_.write_all(data, len, err);
    }
    bool read_exact(void *data, size_t len, std::string &err) override {
        if (!inner_.read_exact(data, len, err)) {
            return false;
        }
        if (verbose_) {
            const auto *p = static_cast<const uint8_t *>(data);
            std::fprintf(stderr, SCRCTL_TR("  Receive %zu bytes:"), len);
            for (size_t i = 0; i < len && i < 400; ++i) {
                std::fputc(p[i] >= 0x20 && p[i] < 0x7F ? p[i] : '.', stderr);
            }
            std::fprintf(stderr, "\n");
        }
        return true;
    }

private:
    scrctl::wifi::ByteStream &inner_;
    bool verbose_ = false;
};

/// 将 lockdown 服务的 TLS 连接适配为完整读写字节流。
/// RPPairing 不处理 SSL 的分段读写，这里负责累计到请求的字节数。
class TlsByteStream final : public scrctl::wifi::ByteStream {
public:
    TlsByteStream(SSL *ssl, scrctl::transport::Socket &sock) : ssl_(ssl), sock_(sock) {}

    bool write_all(const void *data, size_t len, std::string &err) override {
        const auto *p = static_cast<const char *>(data);
        for (size_t off = 0; off < len;) {
            const int n = SSL_write(ssl_, p + off, static_cast<int>(len - off));
            if (n <= 0) {
                err = SCRCTL_TR("TLS write failed (SSL_get_error=") + std::to_string(SSL_get_error(ssl_, n)) + ")";
                return false;
            }
            off += static_cast<size_t>(n);
        }
        return true;
    }
    bool read_exact(void *data, size_t len, std::string &err) override {
        auto *p = static_cast<char *>(data);
        for (size_t off = 0; off < len;) {
            const int n = SSL_read(ssl_, p + off, static_cast<int>(len - off));
            if (n <= 0) {
                err = SCRCTL_TR("TLS read failed (SSL_get_error=") + std::to_string(SSL_get_error(ssl_, n)) + ")";
                return false;
            }
            off += static_cast<size_t>(n);
        }
        return true;
    }

    bool wait_readable(int ms, std::string &err) override {
        // SSL 已解密到内部缓冲的字节也算可读，不能只等待底层 socket 产生新数据。
        if (SSL_pending(ssl_) > 0) {
            return true;
        }
        return sock_.wait_readable(ms, err);
    }

private:
    SSL *ssl_;
    scrctl::transport::Socket &sock_;
};

/// USB lockdown 上的 RemotePairing 字节流服务。
constexpr char kPairingService[] = "com.apple.dt.remotepairingdeviced.lockdown";
constexpr char kCoreDeviceProxy[] = "com.apple.internal.devicecompute.CoreDeviceProxy";
/// 从所测设备的隧道内 RSD 服务表取得的 RemoteXPC 配对入口；记录见 docs §25.8。
constexpr char kXpcPairingService[] = "com.apple.internal.dt.coredevice.untrusted.tunnelservice";

/// 一条字节流配对连接及其底层资源。
/// 所测设备在配对结束后关闭连接，后续验证需要重新建立连接。
struct PairingPlane {
    scrctl::transport::Socket sock;
    scrctl::transport::TlsChannel tls;
    /// socket/TLS 字节流；开启跟踪时 stream 引用它，故 inner 须先声明、后析构。
    std::unique_ptr<scrctl::wifi::ByteStream> inner;
    std::unique_ptr<scrctl::wifi::ByteStream> stream;
    /// channel 引用 carrier；按声明逆序析构，先销毁 channel，再释放载体。
    std::unique_ptr<scrctl::wifi::FramedCarrier> carrier;
    std::unique_ptr<scrctl::wifi::Rppairing> channel;
};

/// 配对连接参数：address 非空时使用 Wi-Fi TCP，否则经 usbmux 连接 USB 服务。
/// USB 服务是否使用 TLS 由 lockdown 响应决定；两种路径共用 RPPairing 帧格式。
struct PlaneSpec {
    std::string address;
    uint32_t device_id = 0;
    uint16_t port = 0;
    bool use_tls = false;
    scrctl::transport::PemIdentity identity;
};

bool open_plane(const PlaneSpec &spec, bool verbose, PairingPlane &out, std::string &err) {
    if (spec.address.empty()) {
        auto mux = scrctl::transport::Usbmux::open(err);
        if (!mux) {
            return false;
        }
        std::optional<scrctl::transport::Socket> sock =
            mux->connect(spec.device_id, spec.port, err);
        if (!sock) {
            return false;
        }
        out.sock = std::move(*sock);
        if (spec.use_tls && !out.tls.handshake(out.sock, spec.identity, err)) {
            return false;
        }
    } else {
        std::optional<scrctl::transport::Socket> sock = scrctl::transport::connect_tcp(
            spec.address, spec.port, 8000, err);
        if (!sock) {
            return false;
        }
        out.sock = std::move(*sock);
    }
    // 所测 USB 连接失效时未必及时收到 FIN，读超时避免永久等待。
    // 同时配对流程可能等待设备上的用户确认，因此保留两分钟的等待上限。
    if (!out.sock.set_read_timeout(120000, err)) {
        return false;
    }
    std::unique_ptr<scrctl::wifi::ByteStream> base =
        spec.use_tls ? std::unique_ptr<scrctl::wifi::ByteStream>(
                           std::make_unique<TlsByteStream>(out.tls.handle(), out.sock))
                     : std::make_unique<scrctl::wifi::SocketStream>(out.sock);
    if (verbose) {
        out.inner = std::move(base);
        out.stream = std::make_unique<TracingStream>(*out.inner, true);
    } else {
        out.stream = std::move(base);
    }
    out.carrier = std::make_unique<scrctl::wifi::FramedCarrier>(*out.stream);
    out.channel = std::make_unique<scrctl::wifi::Rppairing>(*out.carrier);
    return true;
}

/// 只选择 USB 设备，可按 UDID 过滤，并要求候选唯一；网络条目不能用于首次身份绑定。
bool pick_usb_device(const std::string &udid_filter, scrctl::transport::DeviceRecord &out,
                     std::string &err) {
    auto mux = scrctl::transport::Usbmux::open(err);
    if (!mux) {
        err = SCRCTL_TR("Failed to connect to usbmuxd: ") + err;
        return false;
    }
    std::vector<scrctl::transport::DeviceRecord> devices;
    if (!mux->list_devices(devices, err)) {
        return false;
    }
    std::vector<const scrctl::transport::DeviceRecord *> candidates;
    for (const auto &d : devices) {
        if (d.is_usb() && (udid_filter.empty() || d.udid == udid_filter)) {
            candidates.push_back(&d);
        }
    }
    if (candidates.empty()) {
        err = udid_filter.empty() ? SCRCTL_TR("No USB device found") : SCRCTL_TR("No USB device matches --udid");
        return false;
    }
    if (candidates.size() > 1) {
        err = SCRCTL_TR("Found ") + std::to_string(candidates.size()) + SCRCTL_TR(" devices; select one with --udid");
        return false;
    }
    out = *candidates.front();
    std::printf(SCRCTL_TR("Device %s (%s)\n"), mask(out.udid, 8).c_str(), out.connection_type.c_str());
    return true;
}

/// 配对通道及维持其有效所需的资源。
/// finish_pair_setup 接收连接工厂，是因为所测设备在 setup 后关闭连接，verify
/// 必须重连。字节流路径持有 socket/TLS；RemoteXPC 路径还持有隧道、网络栈和服务连接。
/// owner 擦除具体资源类型，实际依赖顺序由对应结构体保证。这里 channel 先析构，
/// 再释放 carrier，最后释放 owner，避免载体引用的底层连接提前销毁。
struct OpenChannel {
    std::shared_ptr<void> owner;
    std::unique_ptr<scrctl::wifi::EnvelopeCarrier> carrier;
    std::unique_ptr<scrctl::wifi::Rppairing> channel;
};

using ChannelOpener = std::function<std::unique_ptr<OpenChannel>(std::string &err)>;

/// 配对通道等待上限，包含设备上用户确认所需的时间。
constexpr int kPairingTimeoutMs = 120000;

/// 为 Wi-Fi 手动配对或 USB lockdown 服务创建新的字节流载体。
ChannelOpener byte_opener(const PlaneSpec &spec, bool verbose) {
    return [spec, verbose](std::string &err) -> std::unique_ptr<OpenChannel> {
        auto plane = std::make_shared<PairingPlane>();
        if (!open_plane(spec, verbose, *plane, err)) {
            return nullptr;
        }
        auto out = std::make_unique<OpenChannel>();
        out->carrier = std::make_unique<scrctl::wifi::FramedCarrier>(*plane->stream);
        out->channel = std::make_unique<scrctl::wifi::Rppairing>(*out->carrier);
        out->owner = std::move(plane);
        return out;
    };
}

/// USB 服务目录路径：lockdown → CoreDeviceProxy → 包隧道 → 用户态网络栈 → RSD。
/// 成员按依赖顺序声明、按逆序析构：RSD 的 TcpStream 引用 stack，stack 引用 tunnel，
/// tunnel 使用 lockdown 的连接身份。若先销毁 stack，TcpStream 注销端点时会访问
/// 已销毁的同步对象；此前真机曾在该错误顺序下触发 mutex 异常。
struct UsbTunnelPlane {
    scrctl::transport::DeviceRecord device;
    std::optional<scrctl::transport::Lockdown> lockdown;
    std::optional<scrctl::transport::PacketTunnel> tunnel;
    std::unique_ptr<scrctl::net::Stack> stack;
    std::optional<scrctl::remote::Rsd> rsd;
};

bool open_usb_rsd(const std::string &udid_filter, UsbTunnelPlane &out, std::string &err) {
    if (!pick_usb_device(udid_filter, out.device, err)) {
        return false;
    }
    out.lockdown = scrctl::transport::Lockdown::establish(out.device.device_id, out.device.udid,
                                                          err);
    if (!out.lockdown) {
        err = SCRCTL_TR("Failed to establish lockdown session: ") + err;
        return false;
    }
    const auto endpoint = out.lockdown->start_service(kCoreDeviceProxy, err);
    if (!endpoint) {
        err = std::string(SCRCTL_TR("Start service ")) + kCoreDeviceProxy + SCRCTL_TR(" failed: ") + err;
        return false;
    }
    out.tunnel = scrctl::transport::PacketTunnel::establish(
        out.device.device_id, endpoint->port, out.lockdown->identity(), endpoint->requires_tls,
        err);
    if (!out.tunnel) {
        err = SCRCTL_TR("Failed to establish tunnel: ") + err;
        return false;
    }
    const auto params = out.tunnel->params();
    out.stack = std::make_unique<scrctl::net::Stack>(*out.tunnel, params.client_address,
                                                     params.server_address);
    if (!out.stack->addresses_ok() || !out.stack->start_pump(err)) {
        err = SCRCTL_TR("Failed to start network stack inside tunnel: ") + err;
        return false;
    }
    scrctl::remote::PeerIdentity identity;
    const auto uuid = scrctl::remote::parse_uuid_text(out.lockdown->host_id());
    if (!uuid) {
        err = SCRCTL_TR("host_id is not a valid UUID");
        return false;
    }
    identity.uuid = *uuid;
    out.rsd = scrctl::remote::Rsd::open(*out.stack, *out.tunnel, identity, err);
    if (!out.rsd) {
        err = SCRCTL_TR("Failed to open RSD: ") + err;
        return false;
    }
    return true;
}

/// RemoteXPC 服务连接与其 USB 隧道资源。conn 引用 base 内的网络栈，须先析构。
struct XpcPairPlane {
    std::unique_ptr<UsbTunnelPlane> base;
    std::unique_ptr<scrctl::remote::ServiceConnection> conn;
};

ChannelOpener xpc_opener(const std::string &service_name, const std::string &udid_filter,
                         bool verbose) {
    return [service_name, udid_filter, verbose](std::string &err)
               -> std::unique_ptr<OpenChannel> {
        auto plane = std::make_shared<XpcPairPlane>();
        plane->base = std::make_unique<UsbTunnelPlane>();
        if (!open_usb_rsd(udid_filter, *plane->base, err)) {
            return nullptr;
        }
        const auto service = plane->base->rsd->service(service_name);
        if (!service) {
            err = SCRCTL_TR("RSD service not found: ") + service_name + SCRCTL_TR(" (use --usb-services to list the directory)");
            return nullptr;
        }
        if (!service->uses_remote_xpc) {
            err = service_name + SCRCTL_TR(" is not a RemoteXPC service (UsesRemoteXPC=false); cannot use this carrier");
            return nullptr;
        }
        std::printf(SCRCTL_TR("  Pairing service %s, port %u\n"), service->name.c_str(),
                    static_cast<unsigned>(service->port));
        plane->conn = scrctl::remote::ServiceConnection::open(*plane->base->stack, *service, err,
                                                             verbose);
        if (plane->conn == nullptr) {
            return nullptr;
        }
        // 所测设备先发送 ServiceVersion，再处理配对握手，故先消费初始消息。
        // 未收到时保留原有行为：继续握手，由后续响应决定连接能否用于配对。
        scrctl::xpc::Value first;
        if (plane->conn->wait_message(first, 5000, err) ==
            scrctl::remote::Channel::Wait::Message) {
            std::printf(SCRCTL_TR("  Initial device message: %s\n"), scrctl::xpc::describe(first).c_str());
        } else {
            std::printf(SCRCTL_TR("  No initial version message (%s); proceeding with handshake\n"), err.c_str());
            err.clear();
        }
        auto out = std::make_unique<OpenChannel>();
        out->carrier = std::make_unique<scrctl::remote::XpcPairingCarrier>(*plane->conn,
                                                                          kPairingTimeoutMs);
        out->channel = std::make_unique<scrctl::wifi::Rppairing>(*out->carrier);
        out->owner = std::move(plane);
        return out;
    };
}

/// 建立配对、按需保存记录，再重新连接并验证设备是否接受生成的主机密钥。
/// setup 和 verify 使用同一连接工厂，保持所选载体一致；验证使用内存中的新记录，
/// 不以文件写入成功替代协议验证，也不把主机密钥被接受等同于已校验设备长期身份。
int finish_pair_setup(const ChannelOpener &open, std::string udid,
                      const std::string &host_id_override, bool save, bool probe_verify_first,
                      const std::string &host_name_override, const std::string &pairing_kind) {
    std::string err;
    std::string hostname = scrctl::wifi::local_hostname();
    if (hostname.empty()) {
        std::fprintf(stderr, SCRCTL_TR("Failed to obtain local hostname\n"));
        return 1;
    }
    if (!host_name_override.empty()) {
        // sendingHost 是握手中的主机名元信息；覆盖它用于协议对照，
        // 不能仅凭这一字段推断设备按主机名采取了冷却或撤销策略。
        hostname = host_name_override;
    }
    // 默认用 hostname + ".scrctl" 派生 identifier，将探针记录与 Xcode/macOS 区分。
    // 已有设备会在相同 identifier 的 setup 中替换该主机密钥，因此显式覆盖时必须
    // 使用目标记录原来的 identifier；与外部记录互操作时由 --host-id 提供。
    const std::string identifier = host_id_override.empty()
                                       ? scrctl::wifi::host_identifier_uuid3(hostname + ".scrctl")
                                       : host_id_override;
    if (identifier.empty()) {
        std::fprintf(stderr, SCRCTL_TR("Failed to derive host identifier\n"));
        return 1;
    }
    std::printf(SCRCTL_TR("Host identifier = %s (%s)\n"), mask(identifier, 8).c_str(),
                host_id_override.empty() ? SCRCTL_TR("uuid3(hostname + \".scrctl\")") : SCRCTL_TR("provided on command line"));

    auto plane = open(err);
    if (plane == nullptr) {
        std::fprintf(stderr, SCRCTL_TR("Failed to connect to pairing channel: %s\n"), err.c_str());
        return 1;
    }
    const scrctl::wifi::ProgressFn progress = [](std::string_view msg) {
        std::printf(SCRCTL_TR("  · %.*s\n"), static_cast<int>(msg.size()), msg.data());
        std::fflush(stdout);
    };
    scrctl::wifi::PairSetupOptions setup_options;
    setup_options.probe_verify_first = probe_verify_first;
    if (!pairing_kind.empty()) {
        setup_options.pairing_kind = pairing_kind;
    }
    std::printf(SCRCTL_TR("Pairing kind = %s\n"), setup_options.pairing_kind.c_str());
    const scrctl::wifi::PairSetupResult setup = scrctl::wifi::pair_setup(
        *plane->channel, identifier, hostname, udid, progress, setup_options, err);
    if (!setup.ok) {
        std::fprintf(stderr, SCRCTL_TR("Pair setup failed: %s\n"), setup.error.c_str());
        return 1;
    }
    std::printf(SCRCTL_TR("Pair setup completed\n"));
    print_handshake(setup.device_handshake);
    std::printf(SCRCTL_TR("  Host private/public key lengths: %zu/%zu bytes; peer altIRK: %s; remote unlock key: %s\n"),
                setup.record.host_private_key.size(), setup.record.host_public_key.size(),
                setup.record.peer_alt_irk.size() == 16 ? SCRCTL_TR("present") : SCRCTL_TR("missing"),
                setup.record.remote_unlock_host_key.empty() ? SCRCTL_TR("missing") : SCRCTL_TR("present"));

    if (save) {
        const std::string path =
            scrctl::wifi::record_path(scrctl::wifi::default_record_dir(), udid);
        if (!scrctl::wifi::save_record(path, setup.record, err)) {
            std::fprintf(stderr, SCRCTL_TR("Failed to save pairing record: %s\n"), err.c_str());
            return 1;
        }
        std::printf(SCRCTL_TR("Pairing record saved to %s\n"), path.c_str());
    }

    // 所测设备在 setup 完成后关闭连接；verify 使用新连接和刚生成的记录。
    std::printf(SCRCTL_TR("Reconnecting to verify the new pairing record\n"));
    auto verify_plane = open(err);
    if (verify_plane == nullptr) {
        std::fprintf(stderr, SCRCTL_TR("Failed to reconnect to pairing channel: %s\n"), err.c_str());
        return 1;
    }
    const scrctl::wifi::PairVerifyResult verified =
        scrctl::wifi::pair_verify(*verify_plane->channel, setup.record, err);
    if (verified.outcome != scrctl::wifi::VerifyOutcome::Paired) {
        std::fprintf(stderr, SCRCTL_TR("  New record pair-verify failed: %s\n"), verified.error.c_str());
        return 1;
    }
    std::printf(SCRCTL_TR("  New record pair-verify completed; device identity verified and host key accepted\n"));
    print_handshake(verified.device_handshake);
    return 0;
}

/// 字节流配对入口。指定 address 时连接 Wi-Fi 手动配对端口，否则使用 USB lockdown。
/// 设备是否接受 setup、是否要求用户确认取决于控制面和 pairing kind；已有 iOS 27
/// 字节流样本在 M1 后关闭通道，不能将该样本外推为所有设备或所有载体的规则。
int run_pair_setup(const std::string &address, int port, const std::string &udid_filter,
                   const std::string &host_id_override, bool verbose, bool save,
                   bool probe_verify_first, const std::string &host_name_override,
                   const std::string &pairing_kind) {
    std::string err;
    PlaneSpec spec;
    spec.address = address;
    spec.port = static_cast<uint16_t>(port);

    if (address.empty()) {
        scrctl::transport::DeviceRecord device;
        if (!pick_usb_device(udid_filter, device, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        std::optional<scrctl::transport::Lockdown> lockdown =
            scrctl::transport::Lockdown::establish(device.device_id, device.udid, err);
        if (!lockdown) {
            std::fprintf(stderr, SCRCTL_TR("Failed to establish lockdown session: %s\n"), err.c_str());
            return 1;
        }
        const std::optional<scrctl::transport::Lockdown::ServiceEndpoint> endpoint =
            lockdown->start_service(kPairingService, err);
        if (!endpoint) {
            std::fprintf(stderr, SCRCTL_TR("Failed to start service %s: %s\n"), kPairingService, err.c_str());
            return 1;
        }
        std::printf(SCRCTL_TR("Pairing channel port %u (TLS: %s)\n"), endpoint->port,
                    endpoint->requires_tls ? SCRCTL_TR("required") : SCRCTL_TR("not required"));
        spec.device_id = device.device_id;
        spec.port = endpoint->port;
        spec.use_tls = endpoint->requires_tls;
        spec.identity = lockdown->identity();
        return finish_pair_setup(byte_opener(spec, verbose), device.udid, host_id_override,
                                 save, probe_verify_first, host_name_override, pairing_kind);
    }

    // Wi-Fi 连接本身不提供保存记录所需的 UDID。沿用探针行为：未显式指定时，
    // 尝试取唯一 USB 候选的 UDID；这里只借用标识符，不证明它与 Wi-Fi 地址是同一设备。
    std::string udid = udid_filter;
    if (udid.empty()) {
        scrctl::transport::DeviceRecord plugged;
        if (pick_usb_device("", plugged, err)) {
            udid = plugged.udid;
            std::printf(SCRCTL_TR("Using connected USB device %s as the pairing record identifier\n"), mask(udid, 8).c_str());
        }
        if (udid.empty()) {
            std::fprintf(stderr, SCRCTL_TR("Wi-Fi pairing requires --udid when no USB device is selected\n"));
            return 2;
        }
    }
    std::printf(SCRCTL_TR("Pairing channel %s:%d (Wi-Fi manual pairing)\n"), address.c_str(), port);
    return finish_pair_setup(byte_opener(spec, verbose), udid, host_id_override, save,
                             probe_verify_first, host_name_override, pairing_kind);
}

/// USB RemoteXPC 配对入口，与字节流路径复用同一套 setup/verify 流程。
/// 该载体在已有 iOS 27 样本中通过了配对，而部分字节流入口关闭了通道；
/// 具体控制面、kind 和确认流程的实测记录见 docs §25.6、§25.8。
int run_pair_setup_xpc(const std::string &service_name, const std::string &udid_filter,
                       const std::string &host_id_override, bool verbose, bool save,
                       bool probe_verify_first, const std::string &host_name_override,
                       const std::string &pairing_kind) {
    std::string err;
    scrctl::transport::DeviceRecord device;
    if (!pick_usb_device(udid_filter, device, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    std::printf(SCRCTL_TR("Carrier = RemoteXPC (%s)\n"), service_name.c_str());
    return finish_pair_setup(xpc_opener(service_name, device.udid, verbose), device.udid,
                             host_id_override, save, probe_verify_first, host_name_override,
                             pairing_kind);
}

}  // namespace

/// 输出 USB CoreDeviceProxy 隧道中的完整 RSD 服务目录，便于查找配对入口。
int run_usb_services(bool verbose, const std::string &udid_filter) {
    std::string err;
    UsbTunnelPlane plane;
    if (!open_usb_rsd(udid_filter, plane, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    std::printf(SCRCTL_TR("USB tunnel RSD directory: %zu services\n"), plane.rsd->services().size());
    for (const auto &svc : plane.rsd->services()) {
        // 服务名、能力列和 entitlement 均来自协议目录，按原值输出。
        std::printf("  %-60s port=%-5u remotexpc=%d tls=%d%s%s\n", svc.name.c_str(),
                    static_cast<unsigned>(svc.port), svc.uses_remote_xpc ? 1 : 0,
                    svc.encrypt_socket_data ? 1 : 0,
                    svc.entitlement.empty() ? "" : " ent=", svc.entitlement.c_str());
    }
    (void)verbose;
    return 0;
}

int main(int argc, char **argv) {
    std::string address, record_path, foreign_path, host_id, udid;
    bool verbose = false;
    bool want_tunnel = false;
    bool want_rsd = false;
    bool want_pair_setup = false;
    bool want_pair_setup_xpc = false;
    bool want_usb_services = false;
    std::string xpc_service = kXpcPairingService;
    bool save_record_to_disk = true;
    bool probe_verify_first = true;
    std::string host_name_override;
    std::string pairing_kind;
    int port = 49152;
    bool no_save = false;
    bool no_verify_probe = false;
    CLI::App app{SCRCTL_N_("Inspect RemotePairing connections and USB pairing services")};
    app.footer(SCRCTL_N_(
        "By default, verify an existing record over Wi-Fi using --address and either --record "
        "or --pmd3-record. --pair-setup uses a byte stream over USB, or Wi-Fi when --address is "
        "given. --pair-setup-xpc uses USB RemoteXPC. --help does not connect to a device."));
    app.set_help_flag("-h,--help", SCRCTL_N_("Show help"));
    auto *address_option = app.add_option("--address", address,
        SCRCTL_N_("Device address for Wi-Fi verification or byte-stream pairing"));
    auto *port_option = app.add_option("--port", port,
        SCRCTL_N_("Wi-Fi RemotePairing port (1..65535; default: 49152)"))
        ->check(CLI::Range(1, 65535));
    auto *record_option = app.add_option("--record", record_path,
        SCRCTL_N_("scrctl pairing record for Wi-Fi verification"));
    auto *foreign_option = app.add_option("--pmd3-record", foreign_path,
        SCRCTL_N_("Import pymobiledevice3 host keys (no device identity; USB pairing required)"));
    auto *host_id_option = app.add_option("--host-id", host_id,
        SCRCTL_N_("Host identifier for pairing setup or a pymobiledevice3 record"));
    auto *udid_option = app.add_option("--udid", udid,
        SCRCTL_N_("USB device filter, or device identifier for Wi-Fi pairing and foreign records"));
    app.add_flag("-v,--verbose", verbose, SCRCTL_N_("Print connection and pairing details"));
    auto *tunnel_option = app.add_flag("--tunnel", want_tunnel,
        SCRCTL_N_("Establish the tunnel after Wi-Fi verification"));
    auto *rsd_option = app.add_flag("--rsd", want_rsd,
        SCRCTL_N_("Read the tunnel RSD service directory (implies --tunnel)"));
    auto *setup_option = app.add_flag("--pair-setup", want_pair_setup,
        SCRCTL_N_("Pair over USB, or experiment over Wi-Fi with --no-save"));
    auto *xpc_option = app.add_flag("--pair-setup-xpc", want_pair_setup_xpc,
        SCRCTL_N_("Create a pairing record over USB RemoteXPC"));
    auto *xpc_service_option = app.add_option("--xpc-service", xpc_service,
        SCRCTL_N_("RemoteXPC pairing service (only with --pair-setup-xpc)"));
    auto *services_option = app.add_flag("--usb-services", want_usb_services,
        SCRCTL_N_("List services in the USB tunnel RSD directory"));
    auto *no_save_option = app.add_flag("--no-save", no_save,
        SCRCTL_N_("Do not save the new pairing record (pairing setup only)"));
    auto *no_verify_option = app.add_flag("--no-verify-probe", no_verify_probe,
        SCRCTL_N_("Skip the initial verification attempt (pairing setup only)"));
    auto *host_name_option = app.add_option("--host-name", host_name_override,
        SCRCTL_N_("Override the pairing host name (pairing setup only)"));
    auto *kind_option = app.add_option("--pairing-kind", pairing_kind,
        SCRCTL_N_("Override the pairingData kind (pairing setup only)"));
    // 保留原解析器对重复标量选项取最后一个值的行为。
    for (auto *option : {address_option, port_option, record_option, foreign_option,
                         host_id_option, udid_option, xpc_service_option,
                         host_name_option, kind_option}) {
        option->multi_option_policy(CLI::MultiOptionPolicy::TakeLast);
    }
    setup_option->excludes(xpc_option)->excludes(services_option);
    xpc_option->excludes(services_option);
    record_option->excludes(foreign_option);
    xpc_service_option->needs(xpc_option);
    foreign_option->needs(host_id_option);
    scrctl::i18n::CliLanguage language(app);
    try {
        app.parse(argc, argv);
        if (!language.select()) return 2;
        const bool setup = want_pair_setup || want_pair_setup_xpc;
        const bool verification = !setup && !want_usb_services;
        // 验证显式传入的选项，避免把其他模式的参数静默忽略。
        // 所有检查都在文件读取、USB 枚举和网络连接之前完成。
        if (!setup && (no_save_option->count() || no_verify_option->count() ||
                       host_name_option->count() || kind_option->count())) {
            throw CLI::ValidationError(SCRCTL_TR(
                "--no-save, --no-verify-probe, --host-name and --pairing-kind require "
                "--pair-setup or --pair-setup-xpc"));
        }
        if (!verification && (record_option->count() || foreign_option->count() ||
                              tunnel_option->count() || rsd_option->count())) {
            throw CLI::ValidationError(SCRCTL_TR(
                "--record, --pmd3-record, --tunnel and --rsd are only for Wi-Fi verification"));
        }
        if ((want_pair_setup_xpc || want_usb_services) && address_option->count()) {
            throw CLI::ValidationError(SCRCTL_TR(
                "--address cannot be combined with --pair-setup-xpc or --usb-services"));
        }
        if (port_option->count() && (want_pair_setup_xpc || want_usb_services || address.empty())) {
            throw CLI::ValidationError(SCRCTL_TR("--port requires a Wi-Fi --address"));
        }
        if (host_id_option->count() && !setup && !foreign_option->count()) {
            throw CLI::ValidationError(SCRCTL_TR(
                "--host-id requires pairing setup or --pmd3-record"));
        }
        if (udid_option->count() && verification && !foreign_option->count()) {
            throw CLI::ValidationError(SCRCTL_TR(
                "--udid is only used with USB modes, Wi-Fi pairing setup or --pmd3-record"));
        }
        if (verification && address.empty()) {
            throw CLI::ValidationError(SCRCTL_TR("Wi-Fi verification requires --address"));
        }
        if (verification && record_path.empty() && foreign_path.empty()) {
            throw CLI::ValidationError(SCRCTL_TR(
                "Wi-Fi verification requires --record or --pmd3-record"));
        }
        if (foreign_option->count() && host_id.empty()) {
            throw CLI::ValidationError(SCRCTL_TR("--pmd3-record requires a nonempty --host-id"));
        }
        if (want_pair_setup && !address.empty() && !no_save) {
            throw CLI::ValidationError(SCRCTL_TR(
                "Wi-Fi pair setup requires --no-save; pair over USB to save a trusted device identity"));
        }
    } catch (const CLI::CallForHelp &) {
        if (!language.select()) return 2;
        std::printf("%s", language.help().c_str());
        return 0;
    } catch (const CLI::ParseError &e) {
        if (language.select()) {
            std::fprintf(stderr, SCRCTL_TR("Invalid arguments: %s\n"), e.what());
        }
        return 2;
    }
    want_tunnel = want_tunnel || want_rsd;
    save_record_to_disk = !no_save;
    probe_verify_first = !no_verify_probe;
    if (want_usb_services) {
        return run_usb_services(verbose, udid);
    }
    if (want_pair_setup_xpc) {
        return run_pair_setup_xpc(xpc_service, udid, host_id, verbose, save_record_to_disk,
                                  probe_verify_first, host_name_override, pairing_kind);
    }
    if (want_pair_setup) {
        return run_pair_setup(address, port, udid, host_id, verbose, save_record_to_disk,
                              probe_verify_first, host_name_override, pairing_kind);
    }

    std::string err;
    scrctl::wifi::PairRecord record;
    if (!record_path.empty()) {
        const std::optional<scrctl::wifi::PairRecord> loaded =
            scrctl::wifi::load_record(record_path, err);
        if (!loaded) {
            std::fprintf(stderr, SCRCTL_TR("Failed to load pairing record: %s\n"), err.c_str());
            return 1;
        }
        record = *loaded;
    } else if (!foreign_path.empty()) {
        const std::optional<std::string> text = read_file(foreign_path);
        if (!text) {
            std::fprintf(stderr, SCRCTL_TR("Failed to open %s\n"), foreign_path.c_str());
            return 1;
        }
        if (host_id.empty()) {
            std::fprintf(stderr, SCRCTL_TR("Foreign record does not store a host identifier; provide --host-id\n"));
            return 2;
        }
        const std::optional<scrctl::wifi::PairRecord> loaded =
            from_foreign_record(*text, host_id, err);
        if (!loaded) {
            std::fprintf(stderr, SCRCTL_TR("Failed to load foreign pairing record: %s\n"), err.c_str());
            return 1;
        }
        record = *loaded;
        record.udid = udid;
    } else {
        std::fprintf(stderr, SCRCTL_TR("Provide --record or --pmd3-record\n"));
        return 2;
    }
    if (!record.complete()) {
        std::fprintf(stderr, SCRCTL_TR("Pairing record incomplete (udid=%s host_identifier=%s private key: %zu bytes, public key: %zu bytes)\n"),
                     record.udid.empty() ? SCRCTL_TR("(empty)") : mask(record.udid, 8).c_str(),
                     record.host_identifier.empty() ? SCRCTL_TR("(empty)") : mask(record.host_identifier, 8).c_str(),
                     record.host_private_key.size(), record.host_public_key.size());
        return 1;
    }
    if (!record.has_peer_identity()) {
        std::fprintf(stderr, "%s\n", SCRCTL_TR("Pairing record has no device identity; pair again over USB"));
        return 1;
    }

    std::printf(SCRCTL_TR("Connecting to %s:%d (record %s)\n"), address.c_str(), port,
                record.udid.empty() ? SCRCTL_TR("(unknown device)") : mask(record.udid, 8).c_str());
    auto sock = scrctl::transport::connect_tcp(address, static_cast<uint16_t>(port), 5000, err);
    if (!sock) {
        std::fprintf(stderr, SCRCTL_TR("  Connection failed: %s\n"), err.c_str());
        return 1;
    }
    scrctl::wifi::SocketStream raw_stream(*sock);
    TracingStream stream(raw_stream, verbose);
    scrctl::wifi::FramedCarrier carrier(stream);
    scrctl::wifi::Rppairing channel(carrier);

    const scrctl::wifi::PairVerifyResult verified = scrctl::wifi::pair_verify(channel, record, err);
    if (verified.outcome != scrctl::wifi::VerifyOutcome::Paired) {
        const char *tag = verified.outcome == scrctl::wifi::VerifyOutcome::NotPaired
                              ? SCRCTL_TR("device rejected the pairing record")
                              : SCRCTL_TR("verification did not complete");
        std::fprintf(stderr, SCRCTL_TR("  Pair-verify failed (%s): %s\n"), tag, verified.error.c_str());
        return 1;
    }
    std::printf(SCRCTL_TR("  Pair-verify completed; device identity verified and host key accepted\n"));
    print_handshake(verified.device_handshake);
    std::printf(SCRCTL_TR("  Shared secret: %zu bytes (key derivation and tunnel PSK; contents hidden)\n"),
                verified.shared_secret.size());

    // createListener 请求设备创建隧道监听端口。key 字段和后续 TCP TLS-PSK
    // 握手使用同一共享密钥；这里只打印长度，不输出密钥内容。
    const scrctl::json::Value peer_info = j_arr({j_obj({{"owningPID", j_int(static_cast<int64_t>(
                                                   ::getpid()))},
                                               {"owningProcessName", j_str("CoreDeviceService")}})});
    const scrctl::json::Value listener =
        j_obj({{"key", j_str(scrctl::wifi::b64_encode(verified.shared_secret))},
               {"peerConnectionsInfo", peer_info},
               {"transportProtocolType", j_str("tcp")}});
    const scrctl::json::Value request =
        j_obj({{"request", j_obj({{"_0", j_obj({{"createListener", listener}})}})}});
    const std::optional<scrctl::json::Value> reply = channel.encrypted_roundtrip(request, err);
    if (!reply) {
        std::fprintf(stderr, SCRCTL_TR("  createListener failed: %s\n"), err.c_str());
        return 1;
    }
    const scrctl::json::Value *created = scrctl::json::find(*reply, "createListener");
    const scrctl::json::Value *listener_port = created != nullptr ? scrctl::json::find(*created, "port") : nullptr;
    if (listener_port == nullptr) {
        std::fprintf(stderr, SCRCTL_TR("  Response missing createListener.port; actual fields:"));
        for (const auto &kv : reply->items()) {
            std::fprintf(stderr, " %s", kv.key().c_str());
        }
        std::fprintf(stderr, "\n");
        return 1;
    }
    std::printf(SCRCTL_TR("  createListener port = %lld\n"),
                static_cast<long long>(scrctl::json::as_int_or(*listener_port, 0)));

    if (!want_tunnel) {
        const auto probe = scrctl::transport::connect_tcp(
            address, static_cast<uint16_t>(scrctl::json::as_int_or(*listener_port, 0)), 3000, err);
        std::printf(SCRCTL_TR("  Listener port connection: %s\n"), probe ? SCRCTL_TR("connected") : err.c_str());
        return 0;
    }

    // --tunnel 完成 TLS-PSK 与 CDTunnel 握手，用于检查派生的共享密钥能否建立隧道。
    // 监听端口的 TCP 可连接性本身不能证明密钥有效，因此两步结果分别输出。
    const uint16_t tunnel_port = static_cast<uint16_t>(scrctl::json::as_int_or(*listener_port, 0));
    auto tunnel_sock = scrctl::transport::connect_tcp(address, tunnel_port, 5000, err);
    if (!tunnel_sock) {
        std::fprintf(stderr, SCRCTL_TR("  Failed to connect to tunnel port %u: %s\n"), tunnel_port, err.c_str());
        return 1;
    }
    std::printf(SCRCTL_TR("  Connected to tunnel port %u; starting TLS-PSK and CDTunnel handshakes\n"), tunnel_port);
    auto tunnel =
        scrctl::transport::PacketTunnel::establish_psk(std::move(*tunnel_sock),
                                                      verified.shared_secret, err);
    if (!tunnel) {
        std::fprintf(stderr, SCRCTL_TR("  Failed to establish tunnel: %s\n"), err.c_str());
        return 1;
    }
    const auto &p = tunnel->params();
    std::printf(SCRCTL_TR("  Tunnel established: local %s / device %s / RSD port %u / MTU %u\n"),
                p.client_address.c_str(), p.server_address.c_str(), p.rsd_port, p.mtu);
    if (!want_rsd) {
        return 0;
    }

    // --rsd 在已建立的 Wi-Fi 隧道内启动用户态 IPv6/TCP 栈并读取服务目录。
    // 目录访问与后续媒体服务使用相同传输接口，但不能替代起流或长期恢复的验证。
    scrctl::net::Stack stack(*tunnel, p.client_address, p.server_address);
    if (!stack.addresses_ok()) {
        std::fprintf(stderr, SCRCTL_TR("  Failed to parse tunnel addresses\n"));
        return 1;
    }
    if (!stack.start_pump(err)) {
        std::fprintf(stderr, SCRCTL_TR("  Failed to start tunnel packet pump: %s\n"), err.c_str());
        return 1;
    }
    scrctl::remote::PeerIdentity identity;
    // 使用记录中的 host identifier 作为稳定的 RSD peer UUID。
    // 已有设备在 UUID 变化时重新 attach 并关闭旧服务端口；相关约束见 remote/Device.cpp。
    const auto uuid = scrctl::remote::parse_uuid_text(record.host_identifier);
    if (!uuid) {
        std::fprintf(stderr, SCRCTL_TR("  Host identifier is not a valid UUID: %s\n"),
                     mask(record.host_identifier, 8).c_str());
        return 1;
    }
    identity.uuid = *uuid;
    const auto rsd = scrctl::remote::Rsd::open(stack, *tunnel, identity, err);
    if (!rsd) {
        std::fprintf(stderr, SCRCTL_TR("  Failed to open RSD: %s\n"), err.c_str());
        return 1;
    }
    std::printf(SCRCTL_TR("  RSD directory: %zu services\n"), rsd->services().size());
    for (const char *name : {"com.apple.coredevice.displayservice",
                             "com.apple.coredevice.screencaptureservice",
                             "com.apple.coredevice.hid.indigo",
                             "com.apple.coredevice.appservice"}) {
        std::printf("    %-46s %s\n", name, rsd->has_service(name) ? SCRCTL_TR("available") : SCRCTL_TR("not available"));
    }
    return 0;
}
