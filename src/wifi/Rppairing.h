#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "jsonlite/Jsonlite.h"
#include "transport/Usbmux.h"
#include "wifi/Crypto.h"

namespace scrctl::wifi {

/// 字节流。RPPairing 通道不关心底下是普通 TCP 还是 lockdown 那条 TLS 服务连接，
/// 所以这里只提"写全 / 读够"两个动作。
class ByteStream {
public:
    virtual ~ByteStream() = default;
    virtual bool write_all(const void *data, size_t len, std::string &err) = 0;
    virtual bool read_exact(void *data, size_t len, std::string &err) = 0;
};

/// 包住一个已连上的 socket。
class SocketStream final : public ByteStream {
public:
    explicit SocketStream(transport::Socket &sock) : sock_(sock) {}
    bool write_all(const void *data, size_t len, std::string &err) override {
        return sock_.write_all(data, len, err);
    }
    bool read_exact(void *data, size_t len, std::string &err) override {
        return sock_.read_exact(data, len, err);
    }

private:
    transport::Socket &sock_;
};

/// 组 JSON 用的小工厂（`json::Value` 是个聚合体，逐个字段赋值读起来太吵）。
json::Value j_str(std::string_view s);
json::Value j_int(int64_t v);
json::Value j_bool(bool v);
json::Value j_obj(std::vector<std::pair<std::string, json::Value>> kv);

/// RemotePairing 控制通道：帧、信封、序号、主密钥。
///
/// 帧格式（实测来自 iOS 27 上的往返，不是文档）：
///   `"RPPairing"` + `长度:u16 大端` + 该长度的 JSON 文本。
/// 信封有两种：`message.plain._0`（明文，里面再套 request/event/response）与
/// `message.streamEncrypted._0`（base64 的一段 ChaCha20-Poly1305）。
///
/// 序号那一条规矩最容易写错：**只有明文发送会推进 `sequenceNumber`**，配对完成后的
/// 加密请求全部沿用当时那个值不变。看着别扭，但对端按这个来——自己"顺手也加一"会
/// 在 createListener 那一步被拒，而且症状像"权限不够"。
class Rppairing {
public:
    static constexpr std::string_view kMagic = "RPPairing";

    explicit Rppairing(ByteStream &io) : io_(io) {}

    /// 装 pair-verify 得到的两条主密钥（各 32 字节），之后才能走加密往返。
    void install_main_keys(Bytes client_key, Bytes server_key);

    /// 发一条明文内部消息（`{"request":…}` 或 `{"event":…}`），不收回复。
    bool send_plain(const json::Value &inner, std::string &err);

    /// 收一条信封。明文返回其内部消息；加密的用**上一次请求的 nonce** 解开后返回。
    ///
    /// 这里刻意不做"超时也算一种结果"：控制通道上的回复要么来要么连接死了，
    /// 中间态没有意义，交给调用方的 socket 超时处理。
    std::optional<json::Value> receive(std::string &err);

    /// 一次明文往返。
    std::optional<json::Value> plain_roundtrip(const json::Value &inner, std::string &err);

    /// 一次加密往返：`request` 形如 `{"request":{…}}`，返回解出来的 `response._1`。
    std::optional<json::Value> encrypted_roundtrip(const json::Value &request, std::string &err);

    [[nodiscard]] uint64_t sequence() const { return sequence_; }
    [[nodiscard]] uint64_t encrypted_sequence() const { return encrypted_sequence_; }

private:
    bool send_envelope(const json::Value &message, std::string &err);

    ByteStream &io_;
    uint64_t sequence_ = 0;
    uint64_t encrypted_sequence_ = 0;
    Bytes client_main_;
    Bytes server_main_;
    /// 上一次加密请求用的 nonce：设备的回信用同一个值，不是各自计数。
    std::string last_nonce_;
};

}  // namespace scrctl::wifi
