#include "i18n/Translation.h"
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
#ifdef SIGPIPE
// OpenSSL 的 socket BIO 用它自己的 write() 往我们的 fd 上写字节（SSL_read 途中的握手
// 回写也算），那条路拿不到我们 send 上的 MSG_NOSIGNAL。对端 RST 之后 OpenSSL 那一次写
// 就是 SIGPIPE，默认动作杀进程（审查 P1：早先的防护只盖住了 Socket::write_all，隧道的
// TLS 写绕过了它）。
//
// 一共两层，都要，**不再按平台二选一**：
//   * fd 级 `SO_NOSIGPIPE` 由 Socket 接管 fd 时统一设（Usbmux.cpp 的 disable_sigpipe）。
//     曾经只在 TcpConnect 里设，usbmux 自己建的那条 AF_UNIX 隧道就漏了，而
//     lockdown→TLS 用的正是它。
//   * 进程级 `SIG_IGN` 兜住"fd 那层没设上"的情形。这不是假想：实测 macOS 上对端已经
//     关掉时 `setsockopt(SO_NOSIGPIPE)` 直接 EINVAL，而那种 fd 恰恰最需要防护；将来
//     再有绕过 Socket 造 fd 的路也是同一个缺口，而且是静默的（没有日志，只是拔线时
//     进程消失）。Linux 本来也只有这一层可用（没有 fd 级选项）。
// 这一层原先写成"只在没有 SO_NOSIGPIPE 的平台上编进来"，而那个判断在本 TU 里根本
// 读不到 SO_NOSIGPIPE（Usbmux.h 不含 <sys/socket.h>），所以 macOS 上它其实一直是
// 生效的——靠巧合。任何人给这里或 Usbmux.h 加一个 <sys/socket.h>，macOS 的防护就
// 会无声消失。现在条件只看 SIGPIPE 存不存在，不再依赖宏可见性。
// 代价说清楚：SIG_IGN 是进程级的，scrctl 当库被链进别的进程时（MaaFW 控制单元）会
// 一并改掉宿主的 SIGPIPE 处置。scrcpy 也是这么做的；网络代码里这个信号本来就没有
// 可用的默认语义。
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
        return err = openssl_error(SCRCTL_TR("SSL_CTX_new failed")), false;
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
        return err = SCRCTL_TR("Failed to add root certificate"), false;
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
        return err = openssl_error(SCRCTL_TR("Failed to load client certificate")), false;
    }
    if (use_key != 1) {
        return err = openssl_error(SCRCTL_TR("Failed to load client private key")), false;
    }
    if (match != 1) {
        return err = SCRCTL_TR("Client certificate does not match private key"), false;
    }

    ssl_ = SSL_new(ctx_);
    if (ssl_ == nullptr) {
        return err = SCRCTL_TR("SSL_new failed"), false;
    }
    // 设备证书没有 SAN/IP，所以只验链可信、不做主机名校验
    // （OpenSSL 默认即如此，无需显式关闭）。
    if (SSL_set_fd(ssl_, sock.fd()) != 1) {
        return err = SCRCTL_TR("SSL_set_fd failed"), false;
    }
    if (SSL_connect(ssl_) != 1) {
        const int ssl_err = SSL_get_error(ssl_, -1);
        char buf[256] = {0};
        ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
        err = SCRCTL_TR("TLS handshake failed, ssl_err=") + std::to_string(ssl_err) + " " + buf;
        release();
        return false;
    }
    return true;
}

bool TlsChannel::handshake_psk(Socket &sock, const std::vector<uint8_t> &psk, std::string &err) {
    ignore_sigpipe_once();
    release();
    if (psk.empty()) {
        err = SCRCTL_TR("PSK is empty");
        return false;
    }
    psk_ = psk;
    ctx_ = SSL_CTX_new(TLS_client_method());
    if (ctx_ == nullptr) {
        return err = openssl_error(SCRCTL_TR("SSL_CTX_new failed")), false;
    }
    // 钉在 TLS 1.2：设备的隧道监听器只给 PSK 那批密码套件，而 TLS 1.3 里的 PSK 是
    // 另一套机制（external PSK），1.3 的 ClientHello 长那样、对方根本不认。
    SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(ctx_, TLS1_2_VERSION);
    if (SSL_CTX_set_cipher_list(ctx_, "PSK") != 1) {
        err = SCRCTL_TR("TLS backend has no PSK cipher suite; use an OpenSSL build with PSK support");
        release();
        return false;
    }
    // 两边都没有身份，只有共享密钥：不发证书也不验证书。
    SSL_CTX_set_verify(ctx_, SSL_VERIFY_NONE, nullptr);
    SSL_CTX_set_psk_client_callback(ctx_, psk_client_callback);

    ssl_ = SSL_new(ctx_);
    if (ssl_ == nullptr) {
        return err = SCRCTL_TR("SSL_new failed"), false;
    }
    if (SSL_set_ex_data(ssl_, psk_ex_index(), &psk_) != 1) {
        return err = SCRCTL_TR("Failed to attach PSK to TLS channel"), false;
    }
    if (SSL_set_fd(ssl_, sock.fd()) != 1) {
        return err = SCRCTL_TR("SSL_set_fd failed"), false;
    }
    if (SSL_connect(ssl_) != 1) {
        const int ssl_err = SSL_get_error(ssl_, -1);
        char buf[256] = {0};
        ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
        // "unknown psk identity" 这一句值单独说：它的意思不是网络不通，而是
        // pair-verify 那一步的共享密钥算错了——差的往往就是某个 HKDF 的 salt/info。
        err = SCRCTL_TR("PSK handshake failed, ssl_err=") + std::to_string(ssl_err) + " " + buf +
              (std::strstr(buf, "psk") != nullptr
                   ? SCRCTL_TR(" (device tunnel listener rejected the PSK; check pair-verify key derivation)")
                   : "");
        release();
        return false;
    }
    return true;
}

}  // namespace scrctl::transport
