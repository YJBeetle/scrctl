#pragma once

#include <openssl/ssl.h>

#include <string>
#include <vector>

#include "transport/Usbmux.h"

namespace scrctl::transport {

/// 配对记录里用于双向 TLS 的三块 PEM 材料。
struct PemIdentity {
    std::vector<uint8_t> host_cert;
    std::vector<uint8_t> host_key;
    std::vector<uint8_t> root_cert;

    [[nodiscard]] bool complete() const {
        return !host_cert.empty() && !host_key.empty() && !root_cert.empty();
    }
};

/// 把一条已连接的 socket 升级为 lockdown 风格的 TLS 通道。
///
/// lockdown 主会话和 EnableServiceSSL 的服务连接（如 CoreDeviceProxy）用的是
/// 同一套证书，所以两边共用这里，避免复制一份容易写错的 TLS 建立代码。
class TlsChannel {
public:
    TlsChannel();
    ~TlsChannel();
    TlsChannel(TlsChannel &&) noexcept;
    TlsChannel &operator=(TlsChannel &&) noexcept;
    TlsChannel(const TlsChannel &) = delete;
    TlsChannel &operator=(const TlsChannel &) = delete;

    [[nodiscard]] bool valid() const { return ssl_ != nullptr; }
    [[nodiscard]] SSL *handle() const { return ssl_; }

    bool handshake(Socket &sock, const PemIdentity &id, std::string &err);

    /// 升级到 **TLS 1.2 / PSK** 通道：Wi-Fi 隧道那条路上，密钥不是证书，而是
    /// pair-verify 那一步的 X25519 共享密钥（32 字节），身份为空串。
    ///
    /// 与上面那条 lockdown 路子的区别值得记一句：lockdown 是"主机出示配对证书、
    /// 设备不认证自己"，而隧道这条是"两边都没有身份，只有共享的这把密钥"。所以
    /// 这里既不发证书也不验证书，握手能不能成完全取决于 PSK 对不对——这也意味着
    /// **它同时是一条很便宜的 pair-verify 判据**：密钥派生错了一个字节都握不上。
    ///
    /// 只支持 TLS 1.2：设备的隧道监听器只给 PSK 密码套件，且那些套件在 TLS 1.3
    /// 下是另一套（external PSK），发过去会被当成没有共同密码。
    bool handshake_psk(Socket &sock, const std::vector<uint8_t> &psk, std::string &err);

private:
    void release();
    SSL_CTX *ctx_ = nullptr;
    SSL *ssl_ = nullptr;
    /// PSK 回调里只能拿到 `SSL*`，所以密钥挂在对象上、用 ex_data 递进去。
    std::vector<uint8_t> psk_;
};

}  // namespace scrctl::transport
