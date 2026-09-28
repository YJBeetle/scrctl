#include "TlsChannel.h"

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <csignal>
#include <cstring>
#include <mutex>
#include <utility>

namespace scrctl::transport {
namespace {

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
/// self-signed，且 subject 与 issuer 都是空串）——Apple 的设计是只让主机侧
/// 用配对记录做认证，设备侧不自证身份。
///
/// 所以必须无条件接受对端证书，否则握手永远失败。这不是偷懒：
///  - 设备证书不携带任何可校验的身份信息，不存在"验对了该是什么"；
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

/// ex_data 的槽位全进程申请一次。PSK 回调只能拿到 `SSL*`，而密钥得从我们的对象上取。
int psk_ex_index() {
    static const int index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return index;
}

unsigned int psk_client_callback(SSL *ssl, const char *hint, char *identity,
                                 unsigned int max_identity_len, unsigned char *psk,
                                 unsigned int max_psk_len) {
    (void)hint;  // 设备的隧道监听器不发 hint
    const auto *stored = static_cast<const std::vector<uint8_t> *>(
        SSL_get_ex_data(ssl, psk_ex_index()));
    if (stored == nullptr || stored->empty() || stored->size() > max_psk_len ||
        max_identity_len < 1) {
        return 0;
    }
    // 身份是**空串**：参考实现发的就是空身份（它把 identity 传成 None），这边照抄。
    // 传别的会怎样没测过，所以不给自己留一个"看起来能用、实际是不是设备说了算"的变量。
    identity[0] = '\0';
    std::memcpy(psk, stored->data(), stored->size());
    return static_cast<unsigned int>(stored->size());
}

}  // namespace

TlsChannel::TlsChannel() = default;

TlsChannel::~TlsChannel() { release(); }

TlsChannel::TlsChannel(TlsChannel &&other) noexcept : ctx_(other.ctx_), ssl_(other.ssl_) {
    other.ctx_ = nullptr;
    other.ssl_ = nullptr;
}

TlsChannel &TlsChannel::operator=(TlsChannel &&other) noexcept {
    if (this != &other) {
        release();
        ctx_ = other.ctx_;
        ssl_ = other.ssl_;
        other.ctx_ = nullptr;
        other.ssl_ = nullptr;
    }
    return *this;
}

void TlsChannel::release() {
    if (ssl_ != nullptr) {
        SSL_shutdown(ssl_);
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
    if (ctx_ != nullptr) {
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
    }
}

namespace {
#if !defined(SO_NOSIGPIPE) && !defined(_WIN32)
// OpenSSL 的 socket BIO 用它自己的 write() 往我们的 fd 上写字节（SSL_read 途中的握手
// 回写也算），那条路拿不到我们 send 上的 MSG_NOSIGNAL；Linux 又没有 SO_NOSIGPIPE 可以
// 按 fd 关。于是对端 RST 之后 OpenSSL 那一次写就是 SIGPIPE、默认动作杀进程（审查 P1：
// 上一轮的防护只盖住了 Socket::write_all，隧道的 TLS 写绕过了它）。
// macOS/BSD 那边 fd 上开了 SO_NOSIGPIPE，OpenSSL 写同一个 fd 同样受保护，所以只在没有
// 那个选项的平台上把 SIGPIPE 忽略掉——进程级、一次。
void ignore_sigpipe_once() {
    static std::once_flag once;
    std::call_once(once, [] { ::signal(SIGPIPE, SIG_IGN); });
}
#else
void ignore_sigpipe_once() {}
#endif
}  // namespace

bool TlsChannel::handshake(Socket &sock, const PemIdentity &id, std::string &err) {
    ignore_sigpipe_once();
    release();
    ctx_ = SSL_CTX_new(TLS_client_method());
    if (ctx_ == nullptr) {
        return err = openssl_error("SSL_CTX_new 失败"), false;
    }
    // 设备侧 lockdown 仍接受很旧的 TLS，且需要允许不带 SNI 的裸 IP 连接。
    SSL_CTX_set_min_proto_version(ctx_, TLS1_VERSION);
    SSL_CTX_set_options(ctx_, SSL_OP_LEGACY_SERVER_CONNECT);

    X509 *root = read_cert_pem(id.root_cert, err);
    if (root == nullptr) {
        return false;
    }
    const int added = X509_STORE_add_cert(SSL_CTX_get_cert_store(ctx_), root);
    X509_free(root);
    if (added != 1) {
        return err = "加入根证书失败", false;
    }
    SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, verify_accept_peer);

    X509 *host_cert = read_cert_pem(id.host_cert, err);
    if (host_cert == nullptr) {
        return false;
    }
    EVP_PKEY *host_key = read_key_pem(id.host_key, err);
    if (host_key == nullptr) {
        X509_free(host_cert);
        return false;
    }
    const int use_cert = SSL_CTX_use_certificate(ctx_, host_cert);
    const int use_key = SSL_CTX_use_PrivateKey(ctx_, host_key);
    const int match = SSL_CTX_check_private_key(ctx_);
    // 把根证书一并作为链的一部分出示，部分 iOS 版本会要求完整链。
    X509 *root_for_chain = read_cert_pem(id.root_cert, err);
    if (root_for_chain != nullptr) {
        SSL_CTX_add_extra_chain_cert(ctx_, root_for_chain);
    }
    X509_free(host_cert);
    EVP_PKEY_free(host_key);
    if (use_cert != 1) {
        return err = openssl_error("装载客户端证书失败"), false;
    }
    if (use_key != 1) {
        return err = openssl_error("装载客户端私钥失败"), false;
    }
    if (match != 1) {
        return err = "客户端证书与私钥不匹配", false;
    }

    ssl_ = SSL_new(ctx_);
    if (ssl_ == nullptr) {
        return err = "SSL_new 失败", false;
    }
    // 设备证书没有 SAN/IP，所以只验链可信、不做主机名校验
    // （OpenSSL 默认即如此，无需显式关闭）。
    if (SSL_set_fd(ssl_, sock.fd()) != 1) {
        return err = "SSL_set_fd 失败", false;
    }
    if (SSL_connect(ssl_) != 1) {
        const int ssl_err = SSL_get_error(ssl_, -1);
        char buf[256] = {0};
        ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
        err = "TLS 握手失败 ssl_err=" + std::to_string(ssl_err) + " " + buf;
        release();
        return false;
    }
    return true;
}

bool TlsChannel::handshake_psk(Socket &sock, const std::vector<uint8_t> &psk, std::string &err) {
    ignore_sigpipe_once();
    release();
    if (psk.empty()) {
        err = "PSK 是空的，握不上";
        return false;
    }
    psk_ = psk;
    ctx_ = SSL_CTX_new(TLS_client_method());
    if (ctx_ == nullptr) {
        return err = openssl_error("SSL_CTX_new 失败"), false;
    }
    // 钉在 TLS 1.2：设备的隧道监听器只给 PSK 那批密码套件，而 TLS 1.3 里的 PSK 是
    // 另一套机制（external PSK），1.3 的 ClientHello 长那样、对方根本不认。
    SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(ctx_, TLS1_2_VERSION);
    if (SSL_CTX_set_cipher_list(ctx_, "PSK") != 1) {
        err = "这个 TLS 后端没有 PSK 密码套件（macOS 系统自带的 LibreSSL 就是这样）";
        release();
        return false;
    }
    // 两边都没有身份，只有共享密钥：不发证书也不验证书。
    SSL_CTX_set_verify(ctx_, SSL_VERIFY_NONE, nullptr);
    SSL_CTX_set_psk_client_callback(ctx_, psk_client_callback);

    ssl_ = SSL_new(ctx_);
    if (ssl_ == nullptr) {
        return err = "SSL_new 失败", false;
    }
    if (SSL_set_ex_data(ssl_, psk_ex_index(), &psk_) != 1) {
        return err = "给 TLS 通道挂 PSK 失败", false;
    }
    if (SSL_set_fd(ssl_, sock.fd()) != 1) {
        return err = "SSL_set_fd 失败", false;
    }
    if (SSL_connect(ssl_) != 1) {
        const int ssl_err = SSL_get_error(ssl_, -1);
        char buf[256] = {0};
        ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
        // "unknown psk identity" 这一句值单独说：它的意思不是网络不通，而是
        // pair-verify 那一步的共享密钥算错了——差的往往就是某个 HKDF 的 salt/info。
        err = "PSK 握手失败 ssl_err=" + std::to_string(ssl_err) + " " + buf +
              (std::strstr(buf, "psk") != nullptr
                   ? "（设备的隧道监听器说这把 PSK 它不认：回头查 pair-verify 的密钥派生）"
                   : "");
        release();
        return false;
    }
    return true;
}

}  // namespace scrctl::transport
