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
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "jsonlite/Jsonlite.h"
#include "plist/Plist.h"
#include "transport/TcpConnect.h"
#include "wifi/Crypto.h"
#include "wifi/PairRecord.h"
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

}  // namespace

int main(int argc, char **argv) {
    std::string address, record_path, foreign_path, host_id, udid;
    bool verbose = false;
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
        } else {
            std::fprintf(stderr, "未知参数 %s\n", argv[i]);
            return 2;
        }
    }
    if (address.empty()) {
        std::fprintf(stderr,
                     "用法: wifi_probe --address <ip> [--port 49152] "
                     "(--record <pair> | --pmd3-record <plist> --host-id <ID> [--udid <UDID>])\n");
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

    const auto probe = scrctl::transport::connect_tcp(
        address, static_cast<uint16_t>(listener_port->as_int_or(0)), 3000, err);
    std::printf("  那个端口连得上吗: %s\n", probe ? "连得上" : err.c_str());
    return 0;
}
