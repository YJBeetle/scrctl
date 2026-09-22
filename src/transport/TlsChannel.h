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

private:
    void release();
    SSL_CTX *ctx_ = nullptr;
    SSL *ssl_ = nullptr;
};

}  // namespace scrctl::transport
