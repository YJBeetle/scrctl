// M5-a 探针：用**我们自己的实现**对设备的 RemotePairing 端口走一遍握手。
//
// 这一段是 M5 的判据所在：pair-verify 成不成、`createListener` 给不给端口。
// 需要设备在同一网络上，且已经有一条远程配对记录。
//
// 记录有两个来源：
//   --record <我们的 .pair>         产品格式，正常路径
//   --pmd3-record <plist> --host-id <ID>
//       与参考实现**互操作**用：那套工具建的记录里不存 host identifier（它是按
//       主机名现算的），所以必须显式给 --host-id，否则签名缓冲就对不上。
//
// 输出的 UDID 一律打码，不打印任何密钥字节。
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

/// 读另一套实现的配对记录（只为互操作判据，产品里不会走这条路）。
std::optional<scrctl::wifi::PairRecord> from_foreign_record(const std::string &text,
                                                           std::string_view host_id,
                                                           std::string &err) {
    const std::optional<scrctl::plist::Value> parsed = scrctl::plist::parse(text);
    if (!parsed) {
        err = "那份记录不是能读的 plist";
        return std::nullopt;
    }
    const scrctl::plist::Value *priv = parsed->find("private_key");
    const scrctl::plist::Value *pub = parsed->find("public_key");
    if (priv == nullptr || pub == nullptr || priv->kind != scrctl::plist::Kind::Data ||
        pub->kind != scrctl::plist::Kind::Data) {
        err = "记录里没有 private_key/public_key 两块 data";
        return std::nullopt;
    }
    scrctl::wifi::PairRecord record;
    record.host_private_key = priv->data;
    record.host_public_key = pub->data;
    record.host_identifier = host_id;
    // 文件名里的 UDID 由调用方给（这套格式的记录本身不记它是谁）。
    return record;
}

void print_handshake(const scrctl::json::Value &device_handshake) {
    const scrctl::json::Value *version = scrctl::json::find(device_handshake, "wireProtocolVersion");
    std::printf("  设备报的 wireProtocolVersion = %lld\n",
                static_cast<long long>(version != nullptr ? scrctl::json::as_int_or(*version, 0) : 0));
    const scrctl::json::Value *options = scrctl::json::find(device_handshake, "deviceOptions");
    if (options != nullptr) {
        std::printf("  deviceOptions:");
        for (const auto &key : options->items()) {
            std::printf(" %s=%s", key.key().c_str(),
                        key.value().is_boolean()
                            ? (key.value().get<bool>() ? "是" : "否")
                            : "?");
        }
        std::printf("\n");
    }
    const scrctl::json::Value *peer = scrctl::json::find(device_handshake, "peerDeviceInfo");
    if (peer != nullptr) {
        const scrctl::json::Value *identifier = scrctl::json::find(*peer, "identifier");
        std::printf("  设备在这条面上自报的 identifier = %s\n",
                    identifier != nullptr ? mask(scrctl::json::as_string_or(*identifier)).c_str() : "(没给)");
    }
}

/// 一帧一帧打字的字节流：M5 这条路第一次对真机时，"我们发的到底是什么"不能靠推理。
class TracingStream final : public scrctl::wifi::ByteStream {
public:
    TracingStream(scrctl::wifi::ByteStream &inner, bool verbose) : inner_(inner), verbose_(verbose) {}

    bool write_all(const void *data, size_t len, std::string &err) override {
        if (verbose_) {
            const auto *p = static_cast<const uint8_t *>(data);
            std::fprintf(stderr, "  → %zu 字节:", len);
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
            std::fprintf(stderr, "  ← %zu 字节:", len);
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

/// lockdown 服务连接上的字节流（可能带 TLS）。RPPairing 只要求"写全 / 读够"，
/// 所以底下是普通 socket 还是 SSL 都由这一层吃掉。
class TlsByteStream final : public scrctl::wifi::ByteStream {
public:
    TlsByteStream(SSL *ssl, scrctl::transport::Socket &sock) : ssl_(ssl), sock_(sock) {}

    bool write_all(const void *data, size_t len, std::string &err) override {
        const auto *p = static_cast<const char *>(data);
        for (size_t off = 0; off < len;) {
            const int n = SSL_write(ssl_, p + off, static_cast<int>(len - off));
            if (n <= 0) {
                err = "TLS 写失败（SSL_get_error=" + std::to_string(SSL_get_error(ssl_, n)) + "）";
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
                err = "TLS 读断了（SSL_get_error=" + std::to_string(SSL_get_error(ssl_, n)) + "）";
                return false;
            }
            off += static_cast<size_t>(n);
        }
        return true;
    }

    bool wait_readable(int ms, std::string &err) override {
        // 已解出但还没被读走的字节也算"可读"，否则会把缓冲里的回信等丢。
        if (SSL_pending(ssl_) > 0) {
            return true;
        }
        return sock_.wait_readable(ms, err);
    }

private:
    SSL *ssl_;
    scrctl::transport::Socket &sock_;
};

/// USB 那条 `remotepairingdeviced.lockdown` 控制面。
constexpr char kPairingService[] = "com.apple.dt.remotepairingdeviced.lockdown";
constexpr char kCoreDeviceProxy[] = "com.apple.internal.devicecompute.CoreDeviceProxy";
/// pair-setup 在 RemoteXPC 载体上的入口（docs §25.8）。名字来自隧道内 RSD 服务表实测。
constexpr char kXpcPairingService[] = "com.apple.internal.dt.coredevice.untrusted.tunnelservice";

/// 一条控制面连接。设备在配对结束后会**关掉**它（实测，与参考实现注释一致），
/// 所以这是一次性对象：要再说话就重开一条。
struct PairingPlane {
    scrctl::transport::Socket sock;
    scrctl::transport::TlsChannel tls;
    /// 真正干活的那层（socket 或 TLS）。`stream` 可能是套在它外面的 TracingStream。
    std::unique_ptr<scrctl::wifi::ByteStream> inner;
    std::unique_ptr<scrctl::wifi::ByteStream> stream;
    /// 信封载体。声明顺序在 channel 之前：析构按声明逆序，channel 先走，
    /// 它引用的载体才不会先没。
    std::unique_ptr<scrctl::wifi::FramedCarrier> carrier;
    std::unique_ptr<scrctl::wifi::Rppairing> channel;
};

/// 一条控制面怎么开：给了 address 走普通 TCP（Wi-Fi 手动配对面），否则走 usbmux
/// 转发（USB lockdown 面，可能要 TLS）。两条面说的都是同一套 RPPairing 帧。
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
    // 设备掐连接时可能不发 FIN（USB 面实测），没有读超时这里会永久挂住，症状像
    // 我们卡死。配对要等人点「信任」，给两分钟。
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

/// 挑一台设备。给了 udid 就按它找，否则要求"插着的正好一台 USB 设备"。
bool pick_usb_device(const std::string &udid_filter, scrctl::transport::DeviceRecord &out,
                     std::string &err) {
    auto mux = scrctl::transport::Usbmux::open(err);
    if (!mux) {
        err = "连不上 usbmuxd: " + err;
        return false;
    }
    std::vector<scrctl::transport::DeviceRecord> devices;
    if (!mux->list_devices(devices, err)) {
        return false;
    }
    std::vector<const scrctl::transport::DeviceRecord *> candidates;
    for (const auto &d : devices) {
        if (!udid_filter.empty()) {
            if (d.udid == udid_filter) {
                candidates.push_back(&d);
            }
        } else if (d.is_usb()) {
            candidates.push_back(&d);
        }
    }
    if (candidates.empty()) {
        err = udid_filter.empty() ? "没找到 USB 设备" : "没找到 --udid 指的那台设备";
        return false;
    }
    if (candidates.size() > 1) {
        err = "插着 " + std::to_string(candidates.size()) + " 台，得用 --udid 指一台";
        return false;
    }
    out = *candidates.front();
    std::printf("设备 %s（%s）\n", mask(out.udid, 8).c_str(), out.connection_type.c_str());
    return true;
}

/// 一条开好的配对通道，连着它底下那些必须活着的东西。
///
/// 为什么交给 finish_pair_setup 的是"每次开一条"这个动作而不是一条现成的通道：设备在
/// 配对结束后会**关掉**这条连接（实测，与参考实现注释一致），验收那一步必须重开；而
/// 两种载体底下的东西完全不同（一边是 socket/TLS，一边是整条隧道 + RSD + 服务连接），
/// 要共用同一套 pair-setup 流程就只能把"怎么开"抽出来。
///
/// owner 用 shared_ptr<void> 类型擦除，底下那些对象的析构顺序由具体结构体内部的声明
/// 顺序保证；carrier/channel 声明在它之后，所以先于它析构。
struct OpenChannel {
    std::shared_ptr<void> owner;
    std::unique_ptr<scrctl::wifi::EnvelopeCarrier> carrier;
    std::unique_ptr<scrctl::wifi::Rppairing> channel;
};

using ChannelOpener = std::function<std::unique_ptr<OpenChannel>(std::string &err)>;

/// 配对通道上的等待上限。中间要等人在屏幕上点「信任」，给两分钟。
constexpr int kPairingTimeoutMs = 120000;

/// 字节流载体（Wi-Fi 手动配对面 / USB lockdown 服务）。
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

/// USB 那条：lockdown → CoreDeviceProxy → 包隧道 → 用户态栈 → RSD 目录。
///
/// 声明顺序**就是**依赖顺序，因为析构按逆序走：rsd 里的服务连接持有引用栈的
/// TcpStream，必须先于 stack 析构，否则它注销端点时锁的是一个已经销毁的 mutex
///（真机现场：`mutex lock failed: Invalid argument` 直接 abort）。同理 stack 先于
/// tunnel、tunnel 先于 lockdown。
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
        err = "lockdown 建立失败: " + err;
        return false;
    }
    const auto endpoint = out.lockdown->start_service(kCoreDeviceProxy, err);
    if (!endpoint) {
        err = std::string("起 ") + kCoreDeviceProxy + " 失败: " + err;
        return false;
    }
    out.tunnel = scrctl::transport::PacketTunnel::establish(
        out.device.device_id, endpoint->port, out.lockdown->identity(), endpoint->requires_tls,
        err);
    if (!out.tunnel) {
        err = "隧道建立失败: " + err;
        return false;
    }
    const auto params = out.tunnel->params();
    out.stack = std::make_unique<scrctl::net::Stack>(*out.tunnel, params.client_address,
                                                     params.server_address);
    if (!out.stack->addresses_ok() || !out.stack->start_pump(err)) {
        err = "隧道内栈失败: " + err;
        return false;
    }
    scrctl::remote::PeerIdentity identity;
    const auto uuid = scrctl::remote::parse_uuid_text(out.lockdown->host_id());
    if (!uuid) {
        err = "host_id 不是能用的 UUID";
        return false;
    }
    identity.uuid = *uuid;
    out.rsd = scrctl::remote::Rsd::open(*out.stack, *out.tunnel, identity, err);
    if (!out.rsd) {
        err = "RSD 打不开: " + err;
        return false;
    }
    return true;
}

/// RemoteXPC 载体：RSD 目录里的配对服务连接 + 信封载体。
/// 同样按依赖顺序声明：conn 引用 base 里的栈，必须先析构。
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
            err = "RSD 目录里没有 " + service_name + "（--usb-services 能打整张表）";
            return nullptr;
        }
        if (!service->uses_remote_xpc) {
            err = service_name + " 不是 RemoteXPC 服务（UsesRemoteXPC=false），这条载体走不了";
            return nullptr;
        }
        std::printf("  配对服务 %s 端口 %u\n", service->name.c_str(),
                    static_cast<unsigned>(service->port));
        plane->conn = scrctl::remote::ServiceConnection::open(*plane->base->stack, *service, err,
                                                             verbose);
        if (plane->conn == nullptr) {
            return nullptr;
        }
        // 设备在这条通道上说的第一句是 ServiceVersion（参考实现按这个顺序读）。读不到
        // 不致命：接下来的 handshake 自己会把话说明白，别在这儿就把整趟判死。
        scrctl::xpc::Value first;
        if (plane->conn->wait_message(first, 5000, err) ==
            scrctl::remote::Channel::Wait::Message) {
            std::printf("  设备第一句: %s\n", scrctl::xpc::describe(first).c_str());
        } else {
            std::printf("  设备没先自报版本（%s），直接发 handshake\n", err.c_str());
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

/// pair-setup 本体：建配对、落盘、再开一条面用新记录 pair-verify 验收。
///
/// 验收这一步不能省：pair-setup 全程我们自己算 SRP/签名，"算对了"只有设备认这把
/// 密钥才算数；而设备认不认，只有拿落盘的记录再握一次手才问得出来。
int finish_pair_setup(const ChannelOpener &open, std::string udid,
                      const std::string &host_id_override, bool save, bool probe_verify_first,
                      const std::string &host_name_override, const std::string &pairing_kind) {
    std::string err;
    std::string hostname = scrctl::wifi::local_hostname();
    if (hostname.empty()) {
        std::fprintf(stderr, "取不到本机主机名\n");
        return 1;
    }
    if (!host_name_override.empty()) {
        // sendingHost 是设备做"同一主机"判定的名字（docs §25）：换一个新名字可以绕开
        // 按主机名的冷却/撤销策略，也是把这条策略单独 isolating 出来的判据。
        hostname = host_name_override;
    }
    // identifier 默认**不**用苹果那个 uuid3(hostname)。理由值得记一句：identifier 相同
    // 在设备那边就是同一条配对记录，pair-setup 会把 Xcode/macOS 自己那把 Ed25519 顶掉
    //（症状是无线调试忽然要重新配对）。加个后缀就多一条属于 scrctl 的记录，谁也不动谁。
    // 要跟参考实现的记录互操作时用 --host-id 显式给。
    const std::string identifier = host_id_override.empty()
                                       ? scrctl::wifi::host_identifier_uuid3(hostname + ".scrctl")
                                       : host_id_override;
    if (identifier.empty()) {
        std::fprintf(stderr, "算不出 host identifier\n");
        return 1;
    }
    std::printf("host identifier = %s（%s）\n", mask(identifier, 8).c_str(),
                host_id_override.empty() ? "uuid3(主机名 + \".scrctl\")" : "命令行给的");

    auto plane = open(err);
    if (plane == nullptr) {
        std::fprintf(stderr, "连控制面失败: %s\n", err.c_str());
        return 1;
    }
    const scrctl::wifi::ProgressFn progress = [](std::string_view msg) {
        std::printf("  · %.*s\n", static_cast<int>(msg.size()), msg.data());
        std::fflush(stdout);
    };
    scrctl::wifi::PairSetupOptions setup_options;
    setup_options.probe_verify_first = probe_verify_first;
    if (!pairing_kind.empty()) {
        setup_options.pairing_kind = pairing_kind;
    }
    std::printf("pairing kind = %s\n", setup_options.pairing_kind.c_str());
    const scrctl::wifi::PairSetupResult setup = scrctl::wifi::pair_setup(
        *plane->channel, identifier, hostname, udid, progress, setup_options, err);
    if (!setup.ok) {
        std::fprintf(stderr, "pair-setup 失败: %s\n", setup.error.c_str());
        return 1;
    }
    std::printf("pair-setup 走通了\n");
    print_handshake(setup.device_handshake);
    std::printf("  host 密钥 %zu/%zu 字节，peer altIRK %s，远程解锁密钥 %s\n",
                setup.record.host_private_key.size(), setup.record.host_public_key.size(),
                setup.record.peer_alt_irk.size() == 16 ? "拿到了" : "没有",
                setup.record.remote_unlock_host_key.empty() ? "没有" : "拿到了");

    if (save) {
        const std::string path =
            scrctl::wifi::record_path(scrctl::wifi::default_record_dir(), udid);
        if (!scrctl::wifi::save_record(path, setup.record, err)) {
            std::fprintf(stderr, "写记录失败: %s\n", err.c_str());
            return 1;
        }
        std::printf("记录写到 %s\n", path.c_str());
    }

    // 设备在配对结束后会关掉那条连接，所以验收要重开一条。
    std::printf("重开一条控制面，用新记录走 pair-verify\n");
    auto verify_plane = open(err);
    if (verify_plane == nullptr) {
        std::fprintf(stderr, "重连控制面失败: %s\n", err.c_str());
        return 1;
    }
    const scrctl::wifi::PairVerifyResult verified =
        scrctl::wifi::pair_verify(*verify_plane->channel, setup.record, err);
    if (verified.outcome != scrctl::wifi::VerifyOutcome::Paired) {
        std::fprintf(stderr, "  新记录 pair-verify 没过: %s\n", verified.error.c_str());
        return 1;
    }
    std::printf("  新记录 pair-verify 通过（设备认这把密钥了）\n");
    print_handshake(verified.device_handshake);
    return 0;
}

/// `--pair-setup`：把配对**从零建起来**，然后用新记录当场 pair-verify 验收。跑通这条，
/// M5 就不再需要"先用参考实现配一次对"这个前置条件。
///
/// 两条控制面：
///   给了 --address —— 设备 mDNS 广播的那个手动配对端口（Wi-Fi 面）。pair-setup 的
///       正路：设备会弹「信任」框，点了才继续。
///   没给 —— USB 那条 `remotepairingdeviced.lockdown` 面。iOS 27 上实测**不收**
///       pair-setup：M1 一到就被 `Invalidating control channel` 掐掉（设备 oslog 量
///       出来的，见 docs），所以这条路只用来复现那个症状。
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
            std::fprintf(stderr, "lockdown 建立失败: %s\n", err.c_str());
            return 1;
        }
        const std::optional<scrctl::transport::Lockdown::ServiceEndpoint> endpoint =
            lockdown->start_service(kPairingService, err);
        if (!endpoint) {
            std::fprintf(stderr, "起 %s 失败: %s\n", kPairingService, err.c_str());
            return 1;
        }
        std::printf("控制面端口 %u（TLS %s）\n", endpoint->port,
                    endpoint->requires_tls ? "要" : "不要");
        spec.device_id = device.device_id;
        spec.port = endpoint->port;
        spec.use_tls = endpoint->requires_tls;
        spec.identity = lockdown->identity();
        return finish_pair_setup(byte_opener(spec, verbose), device.udid, host_id_override,
                                 save, probe_verify_first, host_name_override, pairing_kind);
    }

    // Wi-Fi 面：连接本身不带 UDID，落盘的文件名要它——插着一台 USB 设备就当是它
    //（这台 iPhone 正是我们要配的），否则让命令行给。
    std::string udid = udid_filter;
    if (udid.empty()) {
        scrctl::transport::DeviceRecord plugged;
        if (pick_usb_device("", plugged, err)) {
            udid = plugged.udid;
            std::printf("记录按插着的这台设备落盘（%s）\n", mask(udid, 8).c_str());
        }
        if (udid.empty()) {
            std::fprintf(stderr, "Wi-Fi 面不知道设备是谁，得给 --udid\n");
            return 2;
        }
    }
    std::printf("控制面 %s:%d（Wi-Fi 手动配对面）\n", address.c_str(), port);
    return finish_pair_setup(byte_opener(spec, verbose), udid, host_id_override, save,
                             probe_verify_first, host_name_override, pairing_kind);
}

/// `--pair-setup-xpc`：同一套 pair-setup，走 RemoteXPC 载体。
///
/// 为什么要多这一条：iOS 27 上字节流载体（Wi-Fi 手动口 49152、USB lockdown 的
/// remotepairingdeviced）一律 M1 即 `Invalidating control channel`，而 Xcode 成功那趟
/// 设备侧的通道名是 `remotexpc-8`（docs §25.6/§25.8）——门在传输层，不在字段上。
/// 所以把那十一条已否证的字段假设全部原样保留，只换载体，才是干净的判据。
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
    std::printf("载体 = RemoteXPC（%s）\n", service_name.c_str());
    return finish_pair_setup(xpc_opener(service_name, udid_filter, verbose), device.udid,
                             host_id_override, save, probe_verify_first, host_name_override,
                             pairing_kind);
}

}  // namespace

/// 把 USB CoreDeviceProxy 隧道里那张 RSD 服务表整张打出来：pair-setup 的 RemoteXPC
/// 入口（docs §25.7）如果存在，就该在这张表里。
int run_usb_services(bool verbose, const std::string &udid_filter) {
    std::string err;
    UsbTunnelPlane plane;
    if (!open_usb_rsd(udid_filter, plane, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    std::printf("隧道内 RSD 服务共 %zu 个：\n", plane.rsd->services().size());
    for (const auto &svc : plane.rsd->services()) {
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
        SCRCTL_N_("pymobiledevice3 plist record; requires --host-id"));
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
        SCRCTL_N_("Create a pairing record over USB or the given Wi-Fi address"));
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
            std::fprintf(stderr, "读配对记录失败: %s\n", err.c_str());
            return 1;
        }
        record = *loaded;
    } else if (!foreign_path.empty()) {
        const std::optional<std::string> text = read_file(foreign_path);
        if (!text) {
            std::fprintf(stderr, "打不开 %s\n", foreign_path.c_str());
            return 1;
        }
        if (host_id.empty()) {
            std::fprintf(stderr, "那种格式不存 host identifier，必须给 --host-id\n");
            return 2;
        }
        const std::optional<scrctl::wifi::PairRecord> loaded =
            from_foreign_record(*text, host_id, err);
        if (!loaded) {
            std::fprintf(stderr, "读那份记录失败: %s\n", err.c_str());
            return 1;
        }
        record = *loaded;
        record.udid = udid;
    } else {
        std::fprintf(stderr, "要 --record 或 --pmd3-record 之一\n");
        return 2;
    }
    if (!record.complete()) {
        std::fprintf(stderr, "记录不完整（udid=%s host_identifier=%s 私钥 %zu 字节 公钥 %zu 字节）\n",
                     record.udid.empty() ? "(空)" : mask(record.udid, 8).c_str(),
                     record.host_identifier.empty() ? "(空)" : mask(record.host_identifier, 8).c_str(),
                     record.host_private_key.size(), record.host_public_key.size());
        return 1;
    }

    std::printf("连 %s:%d（记录 %s）\n", address.c_str(), port,
                record.udid.empty() ? "(未知设备)" : mask(record.udid, 8).c_str());
    auto sock = scrctl::transport::connect_tcp(address, static_cast<uint16_t>(port), 5000, err);
    if (!sock) {
        std::fprintf(stderr, "  连接失败: %s\n", err.c_str());
        return 1;
    }
    scrctl::wifi::SocketStream raw_stream(*sock);
    TracingStream stream(raw_stream, verbose);
    scrctl::wifi::FramedCarrier carrier(stream);
    scrctl::wifi::Rppairing channel(carrier);

    const scrctl::wifi::PairVerifyResult verified = scrctl::wifi::pair_verify(channel, record, err);
    if (verified.outcome != scrctl::wifi::VerifyOutcome::Paired) {
        const char *tag = verified.outcome == scrctl::wifi::VerifyOutcome::NotPaired
                              ? "设备答了但不认这条记录"
                              : "没答上";
        std::fprintf(stderr, "  pair-verify 未通过（%s）: %s\n", tag, verified.error.c_str());
        return 1;
    }
    std::printf("  pair-verify 通过：设备认了我们的 host 密钥\n");
    print_handshake(verified.device_handshake);
    std::printf("  共享密钥 %zu 字节（既做主密钥的输入，也做隧道 PSK；字节不打印）\n",
                verified.shared_secret.size());

    // createListener：设备据此开一个隧道监听端口。TCP 那条要把共享密钥原样当 PSK 发过去。
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
        std::fprintf(stderr, "  createListener 失败: %s\n", err.c_str());
        return 1;
    }
    const scrctl::json::Value *created = scrctl::json::find(*reply, "createListener");
    const scrctl::json::Value *listener_port = created != nullptr ? scrctl::json::find(*created, "port") : nullptr;
    if (listener_port == nullptr) {
        std::fprintf(stderr, "  回信里没有 createListener.port，实际字段：");
        for (const auto &kv : reply->items()) {
            std::fprintf(stderr, " %s", kv.key().c_str());
        }
        std::fprintf(stderr, "\n");
        return 1;
    }
    std::printf("  createListener 给了端口 %lld\n",
                static_cast<long long>(scrctl::json::as_int_or(*listener_port, 0)));

    if (!want_tunnel) {
        const auto probe = scrctl::transport::connect_tcp(
            address, static_cast<uint16_t>(scrctl::json::as_int_or(*listener_port, 0)), 3000, err);
        std::printf("  那个端口连得上吗: %s\n", probe ? "连得上" : err.c_str());
        return 0;
    }

    // --tunnel：真的把隧道起起来。这一步同时是 pair-verify 那把共享密钥的判据——
    // 隧道监听器只认这把 PSK，密钥派生错一个字节就握不上。
    const uint16_t tunnel_port = static_cast<uint16_t>(scrctl::json::as_int_or(*listener_port, 0));
    auto tunnel_sock = scrctl::transport::connect_tcp(address, tunnel_port, 5000, err);
    if (!tunnel_sock) {
        std::fprintf(stderr, "  连隧道端口 %u 失败: %s\n", tunnel_port, err.c_str());
        return 1;
    }
    std::printf("  连上隧道端口 %u，开始 TLS-PSK + CDTunnel 握手\n", tunnel_port);
    auto tunnel =
        scrctl::transport::PacketTunnel::establish_psk(std::move(*tunnel_sock),
                                                      verified.shared_secret, err);
    if (!tunnel) {
        std::fprintf(stderr, "  隧道建立失败: %s\n", err.c_str());
        return 1;
    }
    const auto &p = tunnel->params();
    std::printf("  隧道通了：本机 %s / 设备 %s / 隧道内 RSD 端口 %u / MTU %u\n",
                p.client_address.c_str(), p.server_address.c_str(), p.rsd_port, p.mtu);
    if (!want_rsd) {
        return 0;
    }

    // --rsd：复用隧道内那套用户态 IPv6+TCP 栈，读 RSD 目录。这一段跑通，Wi-Fi
    // 那条路就与 USB 那条在同一个层次上了（后面的起流是同一份代码）。
    scrctl::net::Stack stack(*tunnel, p.client_address, p.server_address);
    if (!stack.addresses_ok()) {
        std::fprintf(stderr, "  隧道内地址解析失败\n");
        return 1;
    }
    if (!stack.start_pump(err)) {
        std::fprintf(stderr, "  起泵失败: %s\n", err.c_str());
        return 1;
    }
    scrctl::remote::PeerIdentity identity;
    // peer UUID 要**稳定**：设备每条隧道只留一个 RSD 连接，UUID 一变它就把这台机器
    // 重新 attach、并关掉已公布的服务端口（见 remote/Device.cpp 里那段注释）。
    // Wi-Fi 这条用我们自己 host identifier 的那个 UUID。
    const auto uuid = scrctl::remote::parse_uuid_text(record.host_identifier);
    if (!uuid) {
        std::fprintf(stderr, "  host identifier 不是一个能用的 UUID: %s\n",
                     mask(record.host_identifier, 8).c_str());
        return 1;
    }
    identity.uuid = *uuid;
    const auto rsd = scrctl::remote::Rsd::open(stack, *tunnel, identity, err);
    if (!rsd) {
        std::fprintf(stderr, "  RSD 打不开: %s\n", err.c_str());
        return 1;
    }
    std::printf("  RSD 目录共 %zu 个服务\n", rsd->services().size());
    for (const char *name : {"com.apple.coredevice.displayservice",
                             "com.apple.coredevice.screencaptureservice",
                             "com.apple.coredevice.hid.indigo",
                             "com.apple.coredevice.appservice"}) {
        std::printf("    %-46s %s\n", name, rsd->has_service(name) ? "在" : "不在");
    }
    return 0;
}
