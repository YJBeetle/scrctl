#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "plist/Plist.h"
#include "transport/Usbmux.h"

namespace scrctl::transport {

/// lockdown 客户端：连接、建立配对 session（含双向 TLS）、起服务端口。
///
/// 实测确认 iOS 27 上不经 session 直接 StartService 会得到
/// Error=SessionInactive，所以 StartSession + TLS 这一段无法省。
///
/// 帧格式是 [len:u32 大端][XML plist]——注意字节序和 usbmuxd 帧（小端）相反。
class Lockdown {
public:
    Lockdown();
    ~Lockdown();
    Lockdown(Lockdown &&) noexcept;
    Lockdown &operator=(Lockdown &&) noexcept;
    Lockdown(const Lockdown &) = delete;
    Lockdown &operator=(const Lockdown &) = delete;

    /// 完整建立：连 usbmuxd -> 读配对记录 -> Connect(62078) -> StartSession -> TLS。
    static std::optional<Lockdown> establish(uint32_t device_id, std::string_view udid,
                                             std::string &err);

    /// 发一个 lockdown 请求并等回复。自动补 Label 字段。
    bool request(const plist::Value &req, plist::Value &reply, std::string &err);

    /// 起服务并返回可 Connect 的端口。
    ///
    /// 部分服务（如 debugserver）要求后续连接也套 TLS；返回值不区分这点，
    /// 调用方拿到端口后自行 Connect + 可选 wrap_tls。
    std::optional<uint16_t> start_service(std::string_view name, std::string &err);

    /// 用配对记录里的同一套证书，把一条新 socket 升级为 TLS。
    bool wrap_tls(Socket &sock, std::string &err);

    [[nodiscard]] bool secure() const { return ssl_ != nullptr; }
    [[nodiscard]] const std::string &session_id() const { return session_id_; }

private:
    struct Tls;
    Socket sock_;
    std::unique_ptr<Tls> ssl_;
    std::string host_id_;
    std::string system_buid_;
    std::string session_id_;
    std::vector<uint8_t> host_cert_pem_;
    std::vector<uint8_t> host_key_pem_;
    std::vector<uint8_t> root_cert_pem_;
};

}  // namespace scrctl::transport
