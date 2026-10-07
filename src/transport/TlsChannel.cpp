#include "i18n/Translation.h"
#include "TlsChannel.h"

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <climits>
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
        err = SCRCTL_TR("BIO_new_mem_buf failed");
        return nullptr;
    }
    X509 *cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (cert == nullptr) {
        err = openssl_error(SCRCTL_TR("Failed to parse certificate PEM"));
    }
    return cert;
}

EVP_PKEY *read_key_pem(const std::vector<uint8_t> &pem, std::string &err) {
    BIO *bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (bio == nullptr) {
        err = SCRCTL_TR("BIO_new_mem_buf failed");
        return nullptr;
    }
    EVP_PKEY *key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (key == nullptr) {
        err = openssl_error(SCRCTL_TR("Failed to parse private key PEM"));
    }
    return key;
}

/// 已验证的 lockdown 会话中，设备返回空 subject/issuer 的自签证书。
/// 当前兼容策略接受对端证书，以本机 usbmux 配对记录中的主机证书向设备认证。
/// 本回调跳过链校验，不提供独立的设备身份认证；安全边界包括本机配对服务。
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
    // 空身份已通过设备与本地 PSK 服务验证，当前不提供其他身份格式。
    identity[0] = '\0';
    std::memcpy(psk, stored->data(), stored->size());
    return static_cast<unsigned int>(stored->size());
}

}  // namespace

TlsChannel::TlsChannel() = default;

TlsChannel::~TlsChannel() { release(); }

TlsChannel::TlsChannel(TlsChannel &&other) noexcept
    : ctx_(other.ctx_), ssl_(other.ssl_), psk_(std::move(other.psk_)) {
    other.ctx_ = nullptr;
    other.ssl_ = nullptr;
}

TlsChannel &TlsChannel::operator=(TlsChannel &&other) noexcept {
    if (this != &other) {
        release();
        ctx_ = other.ctx_;
        ssl_ = other.ssl_;
        psk_ = std::move(other.psk_);
        other.ctx_ = nullptr;
        other.ssl_ = nullptr;
    }
    return *this;
}

void TlsChannel::release() {
    if (ssl_ != nullptr) {
        if (SSL_is_init_finished(ssl_)) {
            SSL_shutdown(ssl_);
        }
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
    if (ctx_ != nullptr) {
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
    }
    psk_.reset();
}

namespace {
#ifdef SIGPIPE
// OpenSSL 的 socket BIO 不经过 Socket::write_all，无法使用该方法的
// MSG_NOSIGNAL。Socket 接管 fd 时设置 SO_NOSIGPIPE（平台支持时），
// TLS 初始化再忽略 SIGPIPE，覆盖 Linux 及 fd 选项设置失败的情况。
//
// SIG_IGN 会改变整个进程的信号处理；未来嵌入其他宿主时需要重新设计
// 这一边界。目前 scrctl 作为独立程序运行，设置只执行一次。
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
    ERR_clear_error();
    err.clear();
    // 候选对象只在握手成功后交给通道；失败路径直接释放，不对半成品发送 close_notify。
    auto ctx = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>(
        SSL_CTX_new(TLS_client_method()), SSL_CTX_free);
    if (!ctx) {
        return err = openssl_error(SCRCTL_TR("SSL_CTX_new failed")), false;
    }
    // 设备侧 lockdown 仍接受很旧的 TLS，且需要允许不带 SNI 的裸 IP 连接。
    if (SSL_CTX_set_min_proto_version(ctx.get(), TLS1_VERSION) != 1) {
        return err = openssl_error(SCRCTL_TR("Failed to configure TLS protocol version")), false;
    }
    SSL_CTX_set_options(ctx.get(), SSL_OP_LEGACY_SERVER_CONNECT);

    auto root = std::unique_ptr<X509, decltype(&X509_free)>(
        read_cert_pem(id.root_cert, err), X509_free);
    if (!root) {
        return false;
    }
    if (X509_STORE_add_cert(SSL_CTX_get_cert_store(ctx.get()), root.get()) != 1) {
        return err = openssl_error(SCRCTL_TR("Failed to add root certificate")), false;
    }
    SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_PEER, verify_accept_peer);

    auto host_cert = std::unique_ptr<X509, decltype(&X509_free)>(
        read_cert_pem(id.host_cert, err), X509_free);
    if (!host_cert) {
        return false;
    }
    auto host_key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>(
        read_key_pem(id.host_key, err), EVP_PKEY_free);
    if (!host_key) {
        return false;
    }
    if (SSL_CTX_use_certificate(ctx.get(), host_cert.get()) != 1) {
        return err = openssl_error(SCRCTL_TR("Failed to load client certificate")), false;
    }
    if (SSL_CTX_use_PrivateKey(ctx.get(), host_key.get()) != 1) {
        return err = openssl_error(SCRCTL_TR("Failed to load client private key")), false;
    }
    if (SSL_CTX_check_private_key(ctx.get()) != 1) {
        return err = SCRCTL_TR("Client certificate does not match private key"), false;
    }
    // 把根证书一并作为链的一部分出示，部分 iOS 版本会要求完整链。
    auto root_for_chain = std::unique_ptr<X509, decltype(&X509_free)>(
        read_cert_pem(id.root_cert, err), X509_free);
    if (!root_for_chain) {
        return false;
    }
    if (SSL_CTX_add_extra_chain_cert(ctx.get(), root_for_chain.get()) != 1) {
        return err = openssl_error(SCRCTL_TR("Failed to add root certificate")), false;
    }
    root_for_chain.release();  // 成功后根证书由 SSL_CTX 持有。

    auto ssl = std::unique_ptr<SSL, decltype(&SSL_free)>(SSL_new(ctx.get()), SSL_free);
    if (!ssl) {
        return err = openssl_error(SCRCTL_TR("SSL_new failed")), false;
    }
    // 不校验设备证书身份；证书兼容策略见 verify_accept_peer。
    // OpenSSL 的 socket BIO 接口使用 int；拒绝无效或无法无损表示的本机句柄。
    if (!sock.valid() || sock.fd() > static_cast<NativeSocket>(INT_MAX)) {
        return err = SCRCTL_TR("SSL_set_fd failed"), false;
    }
    if (SSL_set_fd(ssl.get(), static_cast<int>(sock.fd())) != 1) {
        return err = openssl_error(SCRCTL_TR("SSL_set_fd failed")), false;
    }
    ERR_clear_error();
    const int connected = SSL_connect(ssl.get());
    if (connected != 1) {
        const int ssl_err = SSL_get_error(ssl.get(), connected);
        char buf[256] = {0};
        ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
        err = SCRCTL_TR("TLS handshake failed, ssl_err=") + std::to_string(ssl_err) + " " + buf;
        return false;
    }
    ctx_ = ctx.release();
    ssl_ = ssl.release();
    return true;
}

bool TlsChannel::handshake_psk(Socket &sock, const std::vector<uint8_t> &psk, std::string &err) {
    ignore_sigpipe_once();
    release();
    ERR_clear_error();
    err.clear();
    if (psk.empty()) {
        err = SCRCTL_TR("PSK is empty");
        return false;
    }
    // SSL ex_data 指向这份独立存储；成功后仅转移所有权，回调使用的地址不变。
    auto stored_psk = std::make_unique<std::vector<uint8_t>>(psk);
    auto ctx = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>(
        SSL_CTX_new(TLS_client_method()), SSL_CTX_free);
    if (!ctx) {
        return err = openssl_error(SCRCTL_TR("SSL_CTX_new failed")), false;
    }
    // 使用已验证的 TLS 1.2 PSK 路径，未接入 TLS 1.3 external PSK。
    if (SSL_CTX_set_min_proto_version(ctx.get(), TLS1_2_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(ctx.get(), TLS1_2_VERSION) != 1) {
        return err = openssl_error(SCRCTL_TR("Failed to configure TLS protocol version")), false;
    }
    if (SSL_CTX_set_cipher_list(ctx.get(), "PSK") != 1) {
        err = SCRCTL_TR("TLS backend has no PSK cipher suite; use an OpenSSL build with PSK support");
        return false;
    }
    // 不使用证书认证；双方通过共享密钥完成认证。
    SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_NONE, nullptr);
    SSL_CTX_set_psk_client_callback(ctx.get(), psk_client_callback);

    auto ssl = std::unique_ptr<SSL, decltype(&SSL_free)>(SSL_new(ctx.get()), SSL_free);
    if (!ssl) {
        return err = openssl_error(SCRCTL_TR("SSL_new failed")), false;
    }
    if (SSL_set_ex_data(ssl.get(), psk_ex_index(), stored_psk.get()) != 1) {
        return err = openssl_error(SCRCTL_TR("Failed to attach PSK to TLS channel")), false;
    }
    // OpenSSL 的 socket BIO 接口使用 int；拒绝无效或无法无损表示的本机句柄。
    if (!sock.valid() || sock.fd() > static_cast<NativeSocket>(INT_MAX)) {
        return err = SCRCTL_TR("SSL_set_fd failed"), false;
    }
    if (SSL_set_fd(ssl.get(), static_cast<int>(sock.fd())) != 1) {
        return err = openssl_error(SCRCTL_TR("SSL_set_fd failed")), false;
    }
    ERR_clear_error();
    const int connected = SSL_connect(ssl.get());
    if (connected != 1) {
        const int ssl_err = SSL_get_error(ssl.get(), connected);
        char buf[256] = {0};
        ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
        // PSK 拒绝可能来自 pair-verify 派生结果；保留 OpenSSL 原始原因供排查。
        err = SCRCTL_TR("PSK handshake failed, ssl_err=") + std::to_string(ssl_err) + " " + buf +
              (std::strstr(buf, "psk") != nullptr
                   ? SCRCTL_TR(" (device tunnel listener rejected the PSK; check pair-verify key derivation)")
                   : "");
        return false;
    }
    ctx_ = ctx.release();
    ssl_ = ssl.release();
    psk_ = std::move(stored_psk);
    return true;
}

}  // namespace scrctl::transport
