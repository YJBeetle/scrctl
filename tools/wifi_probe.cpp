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

#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "jsonlite/Jsonlite.h"
#include "net/Stack.h"
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
    scrctl::json::Value out;
    out.kind = scrctl::json::Kind::Array_;
    out.array = std::move(items);
    return out;
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
    const scrctl::json::Value *version = device_handshake.find("wireProtocolVersion");
    std::printf("  设备报的 wireProtocolVersion = %lld\n",
                static_cast<long long>(version != nullptr ? version->as_int_or(0) : 0));
    const scrctl::json::Value *options = device_handshake.find("deviceOptions");
    if (options != nullptr) {
        std::printf("  deviceOptions:");
        for (const auto &key : options->object) {
            std::printf(" %s=%s", key.first.c_str(),
                        key.second.kind == scrctl::json::Kind::Bool
                            ? (key.second.boolean ? "是" : "否")
                            : "?");
        }
        std::printf("\n");
    }
    const scrctl::json::Value *peer = device_handshake.find("peerDeviceInfo");
    if (peer != nullptr) {
        const scrctl::json::Value *identifier = peer->find("identifier");
        std::printf("  设备在这条面上自报的 identifier = %s\n",
                    identifier != nullptr ? mask(identifier->as_string_or()).c_str() : "(没给)");
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

/// 一条控制面连接。设备在配对结束后会**关掉**它（实测，与参考实现注释一致），
/// 所以这是一次性对象：要再说话就重开一条。
struct PairingPlane {
    scrctl::transport::Socket sock;
    scrctl::transport::TlsChannel tls;
    /// 真正干活的那层（socket 或 TLS）。`stream` 可能是套在它外面的 TracingStream。
    std::unique_ptr<scrctl::wifi::ByteStream> inner;
    std::unique_ptr<scrctl::wifi::ByteStream> stream;
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
    out.channel = std::make_unique<scrctl::wifi::Rppairing>(*out.stream);
    return true;
}

/// pair-setup 本体：建配对、落盘、再开一条面用新记录 pair-verify 验收。
///
/// 验收这一步不能省：pair-setup 全程我们自己算 SRP/签名，"算对了"只有设备认这把
/// 密钥才算数；而设备认不认，只有拿落盘的记录再握一次手才问得出来。
int finish_pair_setup(const PlaneSpec &spec, std::string udid,
                      const std::string &host_id_override, bool verbose, bool save,
                      bool probe_verify_first, const std::string &host_name_override,
                   const std::string &pairing_kind) {
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

    PairingPlane plane;
    if (!open_plane(spec, verbose, plane, err)) {
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
        *plane.channel, identifier, hostname, udid, progress, setup_options, err);
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
    PairingPlane verify_plane;
    if (!open_plane(spec, verbose, verify_plane, err)) {
        std::fprintf(stderr, "重连控制面失败: %s\n", err.c_str());
        return 1;
    }
    const scrctl::wifi::PairVerifyResult verified =
        scrctl::wifi::pair_verify(*verify_plane.channel, setup.record, err);
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
        auto mux = scrctl::transport::Usbmux::open(err);
        if (!mux) {
            std::fprintf(stderr, "连不上 usbmuxd: %s\n", err.c_str());
            return 1;
        }
        std::vector<scrctl::transport::DeviceRecord> devices;
        if (!mux->list_devices(devices, err)) {
            std::fprintf(stderr, "列设备失败: %s\n", err.c_str());
            return 1;
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
            std::fprintf(stderr, "没找到%s设备\n", udid_filter.empty() ? "USB " : "那台 ");
            return 1;
        }
        if (candidates.size() > 1) {
            std::fprintf(stderr, "插着 %zu 台，得用 --udid 指一台\n", candidates.size());
            return 1;
        }
        const scrctl::transport::DeviceRecord &device = *candidates.front();
        std::printf("设备 %s（%s）\n", mask(device.udid, 8).c_str(),
                    device.connection_type.c_str());

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
        return finish_pair_setup(spec, device.udid, host_id_override, verbose, save,
                                 probe_verify_first, host_name_override, pairing_kind);
    }

    // Wi-Fi 面：连接本身不带 UDID，落盘的文件名要它——插着一台 USB 设备就当是它
    //（这台 iPhone 正是我们要配的），否则让命令行给。
    std::string udid = udid_filter;
    if (udid.empty()) {
        auto mux = scrctl::transport::Usbmux::open(err);
        if (mux) {
            std::vector<scrctl::transport::DeviceRecord> devices;
            if (mux->list_devices(devices, err)) {
                std::vector<const scrctl::transport::DeviceRecord *> usb;
                for (const auto &d : devices) {
                    if (d.is_usb()) {
                        usb.push_back(&d);
                    }
                }
                if (usb.size() == 1) {
                    udid = usb.front()->udid;
                    std::printf("记录按插着的这台设备落盘（%s）\n", mask(udid, 8).c_str());
                }
            }
        }
        if (udid.empty()) {
            std::fprintf(stderr, "Wi-Fi 面不知道设备是谁，得给 --udid\n");
            return 2;
        }
    }
    std::printf("控制面 %s:%d（Wi-Fi 手动配对面）\n", address.c_str(), port);
    return finish_pair_setup(spec, udid, host_id_override, verbose, save, probe_verify_first,
                             host_name_override, pairing_kind);
}

}  // namespace

int main(int argc, char **argv) {
    std::string address, record_path, foreign_path, host_id, udid;
    bool verbose = false;
    bool want_tunnel = false;
    bool want_rsd = false;
    bool want_pair_setup = false;
    bool save_record_to_disk = true;
    bool probe_verify_first = true;
    std::string host_name_override;
    std::string pairing_kind;
    int port = 49152;
    for (int i = 1; i < argc; ++i) {
        auto next = [&](std::string &dst) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s 缺参数\n", argv[i]);
                std::exit(2);
            }
            dst = argv[++i];
        };
        if (std::strcmp(argv[i], "--address") == 0) {
            next(address);
        } else if (std::strcmp(argv[i], "--port") == 0) {
            std::string value;
            next(value);
            port = std::atoi(value.c_str());
        } else if (std::strcmp(argv[i], "--record") == 0) {
            next(record_path);
        } else if (std::strcmp(argv[i], "--pmd3-record") == 0) {
            next(foreign_path);
        } else if (std::strcmp(argv[i], "--host-id") == 0) {
            next(host_id);
        } else if (std::strcmp(argv[i], "--udid") == 0) {
            next(udid);
        } else if (std::strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (std::strcmp(argv[i], "--tunnel") == 0) {
            want_tunnel = true;
        } else if (std::strcmp(argv[i], "--rsd") == 0) {
            want_tunnel = true;
            want_rsd = true;
        } else if (std::strcmp(argv[i], "--pair-setup") == 0) {
            want_pair_setup = true;
        } else if (std::strcmp(argv[i], "--no-save") == 0) {
            save_record_to_disk = false;
        } else if (std::strcmp(argv[i], "--no-verify-probe") == 0) {
            probe_verify_first = false;
        } else if (std::strcmp(argv[i], "--host-name") == 0) {
            next(host_name_override);
        } else if (std::strcmp(argv[i], "--pairing-kind") == 0) {
            next(pairing_kind);
        } else {
            std::fprintf(stderr, "未知参数 %s\n", argv[i]);
            return 2;
        }
    }
    if (want_pair_setup) {
        return run_pair_setup(address, port, udid, host_id, verbose, save_record_to_disk,
                              probe_verify_first, host_name_override, pairing_kind);
    }
    if (address.empty()) {
        std::fprintf(stderr,
                     "用法: wifi_probe --address <ip> [--port 49152] "
                     "(--record <pair> | --pmd3-record <plist> --host-id <ID> [--udid <UDID>])\n"
                     "      wifi_probe --pair-setup [--udid <UDID>] [--host-id <ID>] [--no-save]\n");
        return 2;
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
    scrctl::wifi::Rppairing channel(stream);

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
    const scrctl::json::Value *created = reply->find("createListener");
    const scrctl::json::Value *listener_port = created != nullptr ? created->find("port") : nullptr;
    if (listener_port == nullptr) {
        std::fprintf(stderr, "  回信里没有 createListener.port，实际字段：");
        for (const auto &kv : reply->object) {
            std::fprintf(stderr, " %s", kv.first.c_str());
        }
        std::fprintf(stderr, "\n");
        return 1;
    }
    std::printf("  createListener 给了端口 %lld\n",
                static_cast<long long>(listener_port->as_int_or(0)));

    if (!want_tunnel) {
        const auto probe = scrctl::transport::connect_tcp(
            address, static_cast<uint16_t>(listener_port->as_int_or(0)), 3000, err);
        std::printf("  那个端口连得上吗: %s\n", probe ? "连得上" : err.c_str());
        return 0;
    }

    // --tunnel：真的把隧道起起来。这一步同时是 pair-verify 那把共享密钥的判据——
    // 隧道监听器只认这把 PSK，密钥派生错一个字节就握不上。
    const uint16_t tunnel_port = static_cast<uint16_t>(listener_port->as_int_or(0));
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
