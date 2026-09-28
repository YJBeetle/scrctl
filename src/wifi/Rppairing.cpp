#include "wifi/Rppairing.h"

#include <cstring>
#include <utility>

namespace scrctl::wifi {

json::Value j_str(std::string_view s) {
    json::Value v;
    v.kind = json::Kind::String;
    v.string = s;
    return v;
}

json::Value j_int(int64_t v) {
    json::Value out;
    out.kind = json::Kind::Int;
    out.integer = v;
    return out;
}

json::Value j_bool(bool v) {
    json::Value out;
    out.kind = json::Kind::Bool;
    out.boolean = v;
    return out;
}

json::Value j_arr(std::vector<json::Value> items) {
    json::Value out;
    out.kind = json::Kind::Array_;
    out.array = std::move(items);
    return out;
}

json::Value j_obj(std::vector<std::pair<std::string, json::Value>> kv) {
    json::Value out;
    out.kind = json::Kind::Object_;
    for (auto &[k, v] : kv) {
        out.object.emplace(std::move(k), std::move(v));
    }
    return out;
}

void Rppairing::install_main_keys(Bytes client_key, Bytes server_key) {
    client_main_ = std::move(client_key);
    server_main_ = std::move(server_key);
}

bool Rppairing::send_envelope(const json::Value &message, std::string &err) {
    const std::string text = json::write(message);
    if (text.size() > 0xFFFF) {
        err = "帧太长，u16 长度字段放不下";
        return false;
    }
    Bytes buf;
    buf.reserve(kMagic.size() + 2 + text.size());
    buf.insert(buf.end(), kMagic.begin(), kMagic.end());
    buf.push_back(static_cast<uint8_t>(text.size() >> 8));
    buf.push_back(static_cast<uint8_t>(text.size() & 0xFF));
    buf.insert(buf.end(), text.begin(), text.end());
    return io_.write_all(buf.data(), buf.size(), err);
}

bool Rppairing::send_plain(const json::Value &inner, std::string &err) {
    json::Value slot = j_obj({{"_0", inner}});
    json::Value message = j_obj({{"plain", std::move(slot)}});
    const json::Value envelope =
        j_obj({{"message", std::move(message)},
               {"originatedBy", j_str("host")},
               {"sequenceNumber", j_int(static_cast<int64_t>(sequence_))}});
    // sequenceNumber 在这里推进，而加密请求不推进——见类注释那条规矩的由来。
    if (!send_envelope(envelope, err)) {
        return false;
    }
    ++sequence_;
    return true;
}

std::optional<json::Value> Rppairing::receive(std::string &err) {
    uint8_t header[kMagic.size() + 2];
    if (!io_.read_exact(header, sizeof(header), err)) {
        return std::nullopt;
    }
    if (std::memcmp(header, kMagic.data(), kMagic.size()) != 0) {
        err = "帧头不是 RPPairing，说明我们对上了一个不对的端口或者流错位了";
        return std::nullopt;
    }
    const size_t len = (static_cast<size_t>(header[kMagic.size()]) << 8) |
                       static_cast<size_t>(header[kMagic.size() + 1]);
    Bytes body(len);
    if (!io_.read_exact(body.data(), len, err)) {
        return std::nullopt;
    }
    json::Value envelope;
    const std::optional<json::Value> parsed =
        json::parse(std::string_view(reinterpret_cast<const char *>(body.data()), body.size()), &err);  // NOLINT
    if (!parsed) {
        return std::nullopt;
    }
    envelope = *parsed;

    const json::Value *message = envelope.find("message");
    if (message == nullptr) {
        err = "信封里没有 message 字段";
        return std::nullopt;
    }
    if (const json::Value *plain = message->find("plain")) {
        const json::Value *inner = plain->find("_0");
        if (inner == nullptr) {
            err = "plain 里没有 _0";
            return std::nullopt;
        }
        return *inner;
    }
    const json::Value *encrypted = message->find("streamEncrypted");
    if (encrypted == nullptr) {
        err = "信封既不是 plain 也不是 streamEncrypted";
        return std::nullopt;
    }
    const json::Value *payload = encrypted->find("_0");
    if (payload == nullptr || !payload->is_string()) {
        err = "streamEncrypted 的 _0 不是字符串";
        return std::nullopt;
    }
    if (server_main_.size() != 32 || last_nonce_.size() != 12) {
        err = "收到加密帧，但主密钥/nonce 还没就绪（配对没走通就发东西了）";
        return std::nullopt;
    }
    const std::optional<Bytes> sealed = b64_decode(payload->as_string_or(), err);
    if (!sealed) {
        return std::nullopt;
    }
    const std::optional<Bytes> plain_bytes = chacha_open(sv(server_main_), last_nonce_, *sealed, err);
    if (!plain_bytes) {
        return std::nullopt;
    }
    const std::optional<json::Value> decrypted = json::parse(
        std::string_view(reinterpret_cast<const char *>(plain_bytes->data()), plain_bytes->size()),  // NOLINT
        &err);
    if (!decrypted) {
        return std::nullopt;
    }
    const json::Value *response = decrypted->find("response");
    if (response == nullptr) {
        err = "解密出来的不是 response";
        return std::nullopt;
    }
    const json::Value *body_of_response = response->find("_1");
    if (body_of_response == nullptr) {
        err = "response 里没有 _1";
        return std::nullopt;
    }
    // 设备把错误也塞在加密回复里：不挑出来的话，调用方会拿一个没有期待字段的对象
    // 去报"缺字段"，把一个本来很直白的拒绝变成看不懂的话。
    if (const json::Value *extended = body_of_response->find("errorExtended")) {
        const json::Value *info = extended->find("_0");
        const json::Value *user = info != nullptr ? info->find("userInfo") : nullptr;
        const json::Value *why = user != nullptr ? user->find("NSLocalizedDescription") : nullptr;
        err = "设备拒绝: " + (why != nullptr ? why->as_string_or() : std::string("(没有描述)"));
        return std::nullopt;
    }
    return *body_of_response;
}

std::optional<json::Value> Rppairing::plain_roundtrip(const json::Value &inner, std::string &err) {
    if (!send_plain(inner, err)) {
        return std::nullopt;
    }
    return receive(err);
}

bool send_pairing_data(Rppairing &channel, const Bytes &tlv, std::string_view kind,
                       bool start_new_session, std::string &err,
                       std::string_view sending_host) {
    std::vector<std::pair<std::string, json::Value>> fields = {
        {"data", j_str(b64_encode(tlv))},
        {"kind", j_str(kind)},
        {"startNewSession", j_bool(start_new_session)}};
    if (!sending_host.empty()) {
        fields.emplace_back("sendingHost", j_str(sending_host));
    }
    const json::Value payload = j_obj(std::move(fields));
    // 外面那层 `_0` 是 event 的联合体包装，不是 pairingData 的：少了它设备直接关连接
    // （真机现场：握手答得好好的，第一条 pairingData 发出去就没有然后了）。
    json::Value wrapper = j_obj({{"pairingData", j_obj({{"_0", payload}})}});
    const json::Value inner = j_obj({{"event", j_obj({{"_0", std::move(wrapper)}})}});
    return channel.send_plain(inner, err);
}

std::optional<Bytes> pairing_data_roundtrip(Rppairing &channel, const Bytes &tlv,
                                            std::string_view kind, bool start_new_session,
                                            std::string &err, std::string_view sending_host,
                                            const ProgressFn &progress) {
    if (!send_pairing_data(channel, tlv, kind, start_new_session, err, sending_host)) {
        return std::nullopt;
    }

    bool consent_pending = true;  // 第一帧可能是 awaitingUserConsent，之后就不是了
    while (true) {
        const std::optional<json::Value> reply = channel.receive(err);
        if (!reply) {
            return std::nullopt;
        }
        const json::Value *event = reply->find("event");
        if (event == nullptr) {
            err = "配对过程中设备回的不是 event";
            return std::nullopt;
        }
        const json::Value *zero = event->find("_0");
        if (zero == nullptr) {
            err = "event 里没有 _0";
            return std::nullopt;
        }
        if (const json::Value *rejected = zero->find("pairingRejectedWithError")) {
            const json::Value *wrapped = rejected->find("wrappedError");
            const json::Value *user = wrapped != nullptr ? wrapped->find("userInfo") : nullptr;
            const json::Value *why = user != nullptr ? user->find("NSLocalizedDescription") : nullptr;
            err = "设备拒绝: " + (why != nullptr ? why->as_string_or() : std::string("(没有描述)"));
            return std::nullopt;
        }
        if (zero->find("awaitingUserConsent") != nullptr) {
            if (!consent_pending) {
                err = "设备连着两次说要等用户同意";
                return std::nullopt;
            }
            consent_pending = false;
            if (progress) {
                progress("设备在等你在屏幕上点「信任」——点了这一步才会继续");
            }
            continue;
        }
        const json::Value *data = zero->find("pairingData");
        const json::Value *inner_data = data != nullptr ? data->find("_0") : nullptr;
        const json::Value *bytes = inner_data != nullptr ? inner_data->find("data") : nullptr;
        if (bytes == nullptr || !bytes->is_string()) {
            err = "event 里没有 pairingData._0.data，实际字段：";
            for (const auto &kv : zero->object) {
                err += " " + kv.first;
            }
            return std::nullopt;
        }
        return b64_decode(bytes->as_string_or(), err);
    }
}

std::optional<json::Value> Rppairing::encrypted_roundtrip(const json::Value &request,
                                                          std::string &err) {
    if (client_main_.size() != 32) {
        err = "还没装主密钥，发不了加密帧";
        return std::nullopt;
    }
    // nonce = u64 小端计数 + 4 个零字节。计数在**往返成功之后**才推进。
    uint8_t nonce[12] = {0};
    const uint64_t counter = encrypted_sequence_;
    for (size_t i = 0; i < 8; ++i) {
        nonce[i] = static_cast<uint8_t>((counter >> (8 * i)) & 0xFF);
    }
    last_nonce_.assign(reinterpret_cast<const char *>(nonce), sizeof(nonce));  // NOLINT
    const std::string plain_text = json::write(request);
    const Bytes plain(plain_text.begin(), plain_text.end());
    const std::optional<Bytes> sealed = chacha_seal(sv(client_main_), last_nonce_, plain, err);
    if (!sealed) {
        return std::nullopt;
    }
    json::Value slot = j_obj({{"_0", j_str(b64_encode(*sealed))}});
    json::Value message = j_obj({{"streamEncrypted", std::move(slot)}});
    const json::Value envelope =
        j_obj({{"message", std::move(message)},
               {"originatedBy", j_str("host")},
               {"sequenceNumber", j_int(static_cast<int64_t>(sequence_))}});
    if (!send_envelope(envelope, err)) {
        return std::nullopt;
    }
    std::optional<json::Value> response = receive(err);
    if (!response) {
        return std::nullopt;
    }
    ++encrypted_sequence_;
    return response;
}

}  // namespace scrctl::wifi
