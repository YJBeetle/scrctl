#include "Lockdown.h"

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <cstring>

namespace scrctl::transport {
namespace {

constexpr uint16_t kLockdownPort = 62078;
constexpr const char *kLabel = "scrctl";

std::string openssl_error(std::string_view what) {
    const unsigned long code = ERR_get_error();
    char buf[256] = {0};
    if (code != 0) {
        ERR_error_string_n(code, buf, sizeof(buf));
    }
    return std::string(what) + ": " + buf;
}

/// 从内存里的 PEM 读对象。用 BIO 而不是文件，因为配对记录是从 usbmuxd
/// 拿到的字节流，落盘会短暂把私钥暴露在文件系统上。
/// 不用模板是因为 PEM_read_bio_* 实际签名带 callback/u 默认参数，
/// 函数指针类型对不上。
X509 *read_cert_pem(const std::vector<uint8_t> &pem, std::string &err) {
    BIO *bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (bio == nullptr) {
        err = "BIO_new_mem_buf 失败";
        return nullptr;
    }
    X509 *cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (cert == nullptr) {
        err = openssl_error("证书 PEM 解析失败");
    }
    return cert;
}

EVP_PKEY *read_key_pem(const std::vector<uint8_t> &pem, std::string &err) {
    BIO *bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (bio == nullptr) {
        err = "BIO_new_mem_buf 失败";
        return nullptr;
    }
    EVP_PKEY *key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (key == nullptr) {
        err = openssl_error("私钥 PEM 解析失败");
    }
    return key;
}

/// lockdown 的 TLS 里设备故意出示一张空证书（实测 depth=0、err=18
/// self-signed、且 subject 与 issuer 都是空串）——Apple 的设计是只让主机侧
/// 用配对记录做认证，设备侧不自证身份。
///
/// 所以这里必须无条件接受对端证书，否则握手永远失败。这不是偷懒：
///  - 设备证书不携带任何可校验的身份信息，没有"正确的值"可验；
///  - 真正起作用的是反向认证——我们用 HostCertificate/HostPrivateKey 向设备
///    证明"这台 Mac 曾被该 iPhone 信任过"，设备据此才肯起 CoreDeviceProxy；
///  - 配对材料取自本机 usbmuxd（本地 socket），不经过网络。
/// 净效果是这条 TLS 提供机密性与主机认证，不提供设备认证，与 Apple 自身
/// 工具的行为一致。
int verify_accept_peer(int preverify_ok, X509_STORE_CTX *ctx) {
    (void)preverify_ok;
    (void)ctx;
    return 1;
}

bool read_all_raw(Socket &sock, void *dst, size_t len, std::string &err) {
    return sock.read_exact(dst, len, err);
}

}  // namespace

struct Lockdown::Tls {
    SSL_CTX *ctx = nullptr;
    SSL *ssl = nullptr;
    ~Tls() {
        if (ssl != nullptr) {
            SSL_shutdown(ssl);
            SSL_free(ssl);
        }
        if (ctx != nullptr) {
            SSL_CTX_free(ctx);
        }
    }
};

Lockdown::Lockdown() = default;
Lockdown::~Lockdown() = default;
Lockdown::Lockdown(Lockdown &&) noexcept = default;
Lockdown &Lockdown::operator=(Lockdown &&) noexcept = default;

// ------------------------------------------------------------- 读写帧 ------

static bool write_frame(SSL *ssl, Socket &sock, std::string_view body, std::string &err) {
    uint8_t hdr[4];
    const uint32_t n = static_cast<uint32_t>(body.size());
    hdr[0] = static_cast<uint8_t>(n >> 24);
    hdr[1] = static_cast<uint8_t>(n >> 16);
    hdr[2] = static_cast<uint8_t>(n >> 8);
    hdr[3] = static_cast<uint8_t>(n);
    if (ssl != nullptr) {
        if (SSL_write(ssl, hdr, 4) != 4) {
            err = openssl_error("TLS 写头失败");
            return false;
        }
        if (SSL_write(ssl, body.data(), static_cast<int>(body.size())) !=
            static_cast<int>(body.size())) {
            err = openssl_error("TLS 写体失败");
            return false;
        }
        return true;
    }
    return sock.write_len_prefixed_be(body, err);
}

static bool read_frame(SSL *ssl, Socket &sock, std::vector<uint8_t> &out, std::string &err) {
    uint8_t hdr[4];
    if (ssl != nullptr) {
        if (SSL_read(ssl, hdr, 4) != 4) {
            err = openssl_error("TLS 读头失败");
            return false;
        }
    } else if (!read_all_raw(sock, hdr, 4, err)) {
        return false;
    }
    const uint32_t len =
        uint32_t(hdr[0]) << 24 | uint32_t(hdr[1]) << 16 | uint32_t(hdr[2]) << 8 | hdr[3];
    if (len < 1 || len > (32u << 20)) {
        err = "lockdown 帧长度异常: " + std::to_string(len);
        return false;
    }
    out.resize(len);
    if (ssl != nullptr) {
        size_t got = 0;
        while (got < len) {
            const int n = SSL_read(ssl, out.data() + got, static_cast<int>(len - got));
            if (n <= 0) {
                err = openssl_error("TLS 读体失败");
                return false;
            }
            got += static_cast<size_t>(n);
        }
        return true;
    }
    return read_all_raw(sock, out.data(), len, err);
}

// --------------------------------------------------------------- 请求 ------

bool Lockdown::request(const plist::Value &req, plist::Value &reply, std::string &err) {
    plist::Value full = req;
    if (full.find("Label") == nullptr) {
        full.set("Label", plist::Value::Str(kLabel));
    }
    const std::string body = plist::write(full);
    SSL *ssl = ssl_ ? ssl_->ssl : nullptr;
    if (!write_frame(ssl, sock_, body, err)) {
        return false;
    }
    std::vector<uint8_t> frame;
    if (!read_frame(ssl, sock_, frame, err)) {
        return false;
    }
    auto parsed = plist::parse(
        std::string_view(reinterpret_cast<const char *>(frame.data()), frame.size()));
    if (!parsed) {
        err = "lockdown 回复不是合法 plist";
        return false;
    }
    reply = std::move(*parsed);
    return true;
}

// ----------------------------------------------------------------- TLS -----

bool Lockdown::wrap_tls(Socket &sock, std::string &err) {
    auto tls = std::make_unique<Tls>();
    tls->ctx = SSL_CTX_new(TLS_client_method());
    if (tls->ctx == nullptr) {
        return err = openssl_error("SSL_CTX_new 失败"), false;
    }
    // 设备侧 lockdown 仍接受很旧的 TLS，且需要允许不带 SNI 的裸 IP 连接。
    SSL_CTX_set_min_proto_version(tls->ctx, TLS1_VERSION);
    SSL_CTX_set_options(tls->ctx, SSL_OP_LEGACY_SERVER_CONNECT);

    // 用配对记录的 RootCertificate 作为唯一可信 CA 来验设备证书。
    X509 *root = read_cert_pem(root_cert_pem_, err);
    if (root == nullptr) {
        return false;
    }
    X509_STORE *store = SSL_CTX_get_cert_store(tls->ctx);
    const int added = X509_STORE_add_cert(store, root);
    X509_free(root);
    if (added != 1) {
        return err = "加入根证书失败", false;
    }
    SSL_CTX_set_verify(tls->ctx, SSL_VERIFY_PEER, verify_accept_peer);
    // 设备把自签的 RootCertificate 直接当叶子证书出示（实测校验失败码 18 =
    // DEPTH_ZERO_SELF_SIGNED_CERT）。不开 PARTIAL_CHAIN 的话 OpenSSL 只接受
    // "叶子由受信 CA 签发"，叶子本身是受信证书反而不通过。
    X509_VERIFY_PARAM *chain_param = SSL_CTX_get0_param(tls->ctx);
    X509_VERIFY_PARAM_set_flags(chain_param, X509_V_FLAG_PARTIAL_CHAIN);

    X509 *host_cert = read_cert_pem(host_cert_pem_, err);
    if (host_cert == nullptr) {
        return false;
    }
    EVP_PKEY *host_key = read_key_pem(host_key_pem_, err);
    if (host_key == nullptr) {
        X509_free(host_cert);
        return false;
    }
    if (SSL_CTX_use_certificate(tls->ctx, host_cert) != 1) {
        X509_free(host_cert);
        EVP_PKEY_free(host_key);
        return err = openssl_error("装载客户端证书失败"), false;
    }
    if (SSL_CTX_use_PrivateKey(tls->ctx, host_key) != 1) {
        X509_free(host_cert);
        EVP_PKEY_free(host_key);
        return err = openssl_error("装载客户端私钥失败"), false;
    }
    if (SSL_CTX_check_private_key(tls->ctx) != 1) {
        X509_free(host_cert);
        EVP_PKEY_free(host_key);
        return err = "客户端证书与私钥不匹配", false;
    }
    // 把根证书一并作为链的一部分出示，部分 iOS 版本会要求完整链。
    X509 *root_for_chain = read_cert_pem(root_cert_pem_, err);
    if (root_for_chain != nullptr) {
        SSL_CTX_add_extra_chain_cert(tls->ctx, root_for_chain);
    }
    X509_free(host_cert);
    EVP_PKEY_free(host_key);

    tls->ssl = SSL_new(tls->ctx);
    if (tls->ssl == nullptr) {
        return err = "SSL_new 失败", false;
    }
    // 设备证书没有 SAN/IP，所以只验链可信、不做主机名校验
    // （OpenSSL 默认即如此，无需显式关闭）。
    if (SSL_set_fd(tls->ssl, sock.fd()) != 1) {
        return err = "SSL_set_fd 失败", false;
    }
    const int rc = SSL_connect(tls->ssl);
    if (rc != 1) {
        const int ssl_err = SSL_get_error(tls->ssl, rc);
        char buf[256] = {0};
        ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
        X509 *peerd = SSL_get_peer_certificate(tls->ssl);
        const long vx = SSL_get_verify_result(tls->ssl);
        std::string detail = "SSL_connect 失败 rc=" + std::to_string(rc) +
                             " ssl_err=" + std::to_string(ssl_err) + " verify=" +
                             std::to_string(vx) + " " + buf;
        if (peerd != nullptr) {
            char subj[256] = {0};
            char iss[256] = {0};
            X509_NAME_oneline(X509_get_subject_name(peerd), subj, sizeof(subj));
            X509_NAME_oneline(X509_get_issuer_name(peerd), iss, sizeof(iss));
            detail += "  对端 subject=" + std::string(subj) + " issuer=" + std::string(iss);
            X509_free(peerd);
        }
        return err = detail, false;
    }
    ssl_ = std::move(tls);
    return true;
}

// ------------------------------------------------------------- 建立连接 ----

std::optional<Lockdown> Lockdown::establish(uint32_t device_id, std::string_view udid,
                                            std::string &err) {
    // 1) 读配对记录。必须在 Connect 之前，用另一条 mux 连接。
    auto rec_mux = Usbmux::open(err);
    if (!rec_mux) {
        return std::nullopt;
    }
    std::vector<uint8_t> record;
    if (!rec_mux->read_pair_record(udid, record, err)) {
        return std::nullopt;
    }
    auto parsed = plist::parse(
        std::string_view(reinterpret_cast<const char *>(record.data()), record.size()));
    if (!parsed) {
        err = "配对记录无法解析";
        return std::nullopt;
    }
    auto take = [&](std::string_view key) -> std::vector<uint8_t> {
        const auto *v = parsed->find(key);
        return v != nullptr ? v->data : std::vector<uint8_t>{};
    };
    Lockdown ld;
    ld.host_id_ = parsed->find("HostID") ? parsed->find("HostID")->as_string_or("") : "";
    ld.system_buid_ =
        parsed->find("SystemBUID") ? parsed->find("SystemBUID")->as_string_or("") : "";
    ld.host_cert_pem_ = take("HostCertificate");
    ld.host_key_pem_ = take("HostPrivateKey");
    ld.root_cert_pem_ = take("RootCertificate");
    if (ld.host_id_.empty() || ld.host_cert_pem_.empty() || ld.host_key_pem_.empty() ||
        ld.root_cert_pem_.empty()) {
        err = "配对记录缺少必要字段";
        return std::nullopt;
    }

    // 2) 连 lockdown。
    auto conn_mux = Usbmux::open(err);
    if (!conn_mux) {
        return std::nullopt;
    }
    auto sock = conn_mux->connect(device_id, kLockdownPort, err);
    if (!sock) {
        return std::nullopt;
    }
    ld.sock_ = std::move(*sock);

    // 3) StartSession
    plist::Value start = plist::Value::Dict();
    start.set("Request", plist::Value::Str("StartSession"));
    start.set("HostID", plist::Value::Str(ld.host_id_));
    start.set("SystemBUID", plist::Value::Str(ld.system_buid_));
    plist::Value reply;
    if (!ld.request(start, reply, err)) {
        return std::nullopt;
    }
    if (const auto *e = reply.find("Error"); e != nullptr) {
        err = "StartSession 被拒: " + e->as_string_or("?");
        return std::nullopt;
    }
    if (const auto *sid = reply.find("SessionID")) {
        ld.session_id_ = sid->as_string_or("");
    }
    const bool want_ssl = reply.find("EnableSessionSSL")
                              ? reply.find("EnableSessionSSL")->as_bool_or(false)
                              : true;
    if (want_ssl && !ld.wrap_tls(ld.sock_, err)) {
        return std::nullopt;
    }
    return ld;
}

std::optional<uint16_t> Lockdown::start_service(std::string_view name, std::string &err) {
    plist::Value req = plist::Value::Dict();
    req.set("Request", plist::Value::Str("StartService"));
    req.set("Service", plist::Value::Str(std::string(name)));
    plist::Value reply;
    if (!request(req, reply, err)) {
        return std::nullopt;
    }
    if (const auto *e = reply.find("Error"); e != nullptr) {
        err = "StartService(" + std::string(name) + ") 被拒: " + e->as_string_or("?");
        return std::nullopt;
    }
    const auto *port = reply.find("Port");
    if (port == nullptr) {
        err = "StartService 回复里没有 Port";
        return std::nullopt;
    }
    return static_cast<uint16_t>(port->as_int_or(0));
}

}  // namespace scrctl::transport
