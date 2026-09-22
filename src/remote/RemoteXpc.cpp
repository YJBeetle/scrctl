#include "remote/RemoteXpc.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string_view>

namespace scrctl::remote {
namespace {

/// 一次授予的接收窗口。65535 是 RFC 默认值，服务目录本身就有几十 KB，
/// 不提前放量设备写到一半就会卡在流控上不动。
constexpr uint32_t kGrantWindow = 16u << 20;
constexpr uint32_t kWindowIncr = kGrantWindow - http2::kDefaultInitialWindowSize;


/// 攒够这么多就补一次窗口。太小会把帧头开销放大成噪声，太大则要等。
constexpr uint64_t kReplenishThreshold = 1u << 20;

bool write_all(net::TcpStream &sock, std::span<const uint8_t> data, std::string &err) {
    return sock.send(std::string_view(reinterpret_cast<const char *>(data.data()), data.size()),
                     err);
}

bool write_all(net::TcpStream &sock, std::string_view data, std::string &err) {
    return sock.send(data, err);
}

/// 载荷里有没有内容决定 DATA_PRESENT。设备对这一位是有判断的：`{}` 这种空字典
/// 走的是「有载荷但没数据」的形态，标志位只剩 ALWAYS_SET。
uint32_t wrapper_flags(const xpc::Value *body, bool want_reply) {
    uint32_t flags = xpc::kFlagAlwaysSet;
    if (body != nullptr && (body->type != xpc::Type::Dict || !body->dict.empty())) {
        flags |= xpc::kFlagDataPresent;
    }
    if (want_reply) {
        flags |= xpc::kFlagWantingReply;
    }
    return flags;
}

xpc::Value build_handshake(const PeerIdentity &identity) {
    auto d = xpc::make_dict();
    xpc::dict_set(d, "MessageType", xpc::make_string("Handshake"));
    xpc::dict_set(d, "MessagingProtocolVersion",
                  xpc::make_uint64(identity.messaging_protocol_version));
    xpc::dict_set(d, "UUID",
                  xpc::make_uuid(std::span<const uint8_t>(identity.uuid.data(), identity.uuid.size())));
    auto props = xpc::make_dict();
    xpc::dict_set(props, "RemoteXPCVersionFlags", xpc::make_uint64(identity.version_flags));
    // 不给这一位，设备会把带 entitlement 的服务从目录里默默摘掉。
    xpc::dict_set(props, "SensitivePropertiesVisible", xpc::make_bool(true));
    xpc::dict_set(d, "Properties", std::move(props));
    xpc::dict_set(d, "Services", xpc::make_dict());
    return d;
}

int64_t remaining_ms(std::chrono::steady_clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                          deadline - std::chrono::steady_clock::now())
                          .count();
    return left > 0 ? left : 0;
}

int nibble_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

}  // namespace

std::optional<std::array<uint8_t, 16>> parse_uuid_text(std::string_view text) {
    // 32 个十六进制字符，允许中间夹 '-'（标准 8-4-4-4-12 写法）。
    std::array<uint8_t, 16> out{};
    std::size_t digits = 0;
    for (const char c : text) {
        if (c == '-') {
            continue;
        }
        const int v = nibble_value(c);
        if (v < 0) {
            return std::nullopt;
        }
        out[digits / 2] = static_cast<uint8_t>(digits % 2 == 0 ? (v << 4)
                                                              : (out[digits / 2] | v));
        ++digits;
        if (digits > 32) {
            return std::nullopt;
        }
    }
    if (digits != 32) {
        return std::nullopt;
    }
    return out;
}

bool Channel::send_bytes(std::span<const uint8_t> data, std::string &err) {
    if (verbose_) {
        std::fprintf(stderr, "    >> %zu 字节: ", data.size());
        for (const uint8_t b : data) {
            std::fprintf(stderr, "%02x", b);
        }
        std::fprintf(stderr, "\n");
    }
    return write_all(socket_, data, err);
}

bool Channel::start(std::string &err) {
    // 1. 客户端前置签名（不是帧，24 字节裸串）。
    if (!write_all(socket_, std::string_view(http2::kClientPreface, http2::kClientPrefaceSize),
                   err)) {
        err = "发 HTTP/2 前置签名失败: " + err;
        return false;
    }
    // 2. 我们的 SETTINGS：一次把窗口和并发数放到宽，省掉后续来回。
    //    实测多报一项 MAX_FRAME_SIZE 并不会改变设备的行为（下面那个 GOAWAY 照样
    //    来），所以这里只报参考实现报的那两项，不发明设置。
    if (!send_bytes(
            http2::settings_frame({{http2::kSettingMaxConcurrentStreams, 100},
                                   {http2::kSettingInitialWindowSize, kGrantWindow}}),
            err)) {
        err = "发 SETTINGS 失败: " + err;
        return false;
    }
    // 3. 给对端放行 16 MiB 的接收窗口（连接级）。
    //    我方发出去的可用量此刻仍是 RFC 默认的 65535，要等对端的
    //    SETTINGS / WINDOW_UPDATE 来改。
    if (!send_bytes(http2::window_update_frame(0, kWindowIncr), err)) {
        err = "发 WINDOW_UPDATE 失败: " + err;
        return false;
    }
    // 4–8. 建流与终止帧。顺序不能改，设备侧会校验。
    //
    // 每个 XPC 消息都必须套在 DATA 帧里再写。少了帧头，设备会把 wrapper 的
    // magic 0x29B00B92 当成帧头解析：长度读成 9572528、类型读成未定义的 0x29，
    // 于是回一个 GOAWAY "too large frame size"——报错的位置离真正的错因很远，
    // 光看错误信息完全猜不到是自家帧头没写。
    if (!send_bytes(http2::headers_frame(kRootStream), err)) {
        err = "发主通道 HEADERS 失败: " + err;
        return false;
    }
    auto empty_dict = xpc::make_dict();
    auto opener = xpc::encode_message(wrapper_flags(&empty_dict, false), 0, &empty_dict);
    if (!send_bytes(http2::data_frame(kRootStream, opener), err)) {
        err = "发首帧失败: " + err;
        return false;
    }
    next_message_id_ = 1;
    if (!send_bytes(http2::headers_frame(kReplyStream), err)) {
        err = "发回信通道 HEADERS 失败: " + err;
        return false;
    }
    // 主通道的「终止帧」：空载荷，flags = ALWAYS_SET | 0x200（见常量注释）。
    auto term = xpc::encode_message(xpc::kFlagAlwaysSet | xpc::kFlagTermChannel, 0, nullptr);
    if (!send_bytes(http2::data_frame(kRootStream, term), err)) {
        err = "发终止帧失败: " + err;
        return false;
    }
    auto init = xpc::encode_message(xpc::kFlagAlwaysSet | xpc::kFlagInitHandshake, 0, nullptr);
    if (!send_bytes(http2::data_frame(kReplyStream, init), err)) {
        err = "发 INIT_HANDSHAKE 帧失败: " + err;
        return false;
    }

    // 9. 等对端 SETTINGS 并 ACK。它可能夹在 WINDOW_UPDATE 后面来，所以按类型筛。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!settings_received_) {
        const auto left = remaining_ms(deadline);
        if (left == 0) {
            err = "等设备 SETTINGS 超时（前面发的帧被拒了吗）";
            return false;
        }
        if (!pump(static_cast<int>(left), err)) {
            err = "等设备 SETTINGS 时断开: " + err;
            return false;
        }
    }

    return true;
}

bool Channel::announce_device(const PeerIdentity &identity, std::string &err) {
    auto hs = build_handshake(identity);
    if (!send_request(hs, false, err)) {
        err = "发 RemoteXPC 身份申报失败: " + err;
        return false;
    }
    xpc::Value info;
    if (!receive(info, 5000, err)) {
        err = "读 peer_info 失败: " + err;
        return false;
    }
    peer_info_ = std::move(info);
    return true;
}

std::optional<Channel> Channel::open(net::TcpStream &socket, std::string &err, bool verbose) {
    std::optional<Channel> ch;
    ch.emplace(socket);
    ch->verbose_ = verbose;
    if (!ch->start(err)) {
        return std::nullopt;
    }
    return ch;
}

bool Channel::pump(int timeout_ms, std::string &err) {
    bool processed = false;
    for (;;) {
        http2::Frame f;
        std::size_t used = 0;
        std::string perr;
        const auto st = http2::parse_frame(rx_, f, used, perr);
        if (st == http2::Status::Ok) {
            rx_.erase(rx_.begin(), rx_.begin() + static_cast<std::ptrdiff_t>(used));
            if (verbose_) {
                std::fprintf(stderr, "    <- %s\n", http2::describe(f).c_str());
            }
            if (!handle_frame(f, err)) {
                return false;
            }
            processed = true;
            continue;
        }
        if (st == http2::Status::Malformed) {
            err = perr;
            return false;
        }
        break;  // 缓冲里只剩半帧，去收字节
    }
    // 处理过帧就先交回控制权，让调用方看看有没有攒出完整消息，再去阻塞读。
    // 不这样返回的话，「回信随最后一批字节到齐」这个最常见的时序会被读超时
    // 吃掉：明明收到了，却报成没收到。
    if (processed) {
        return true;
    }

    std::vector<uint8_t> got;
    if (!socket_.recv(got, timeout_ms, err)) {
        return false;
    }
    rx_.insert(rx_.end(), got.begin(), got.end());
    return true;
}

bool Channel::handle_frame(const http2::Frame &f, std::string &err) {
    switch (f.type) {
        case http2::kSettings: {
            bool ack = false;
            if (!http2::parse_settings(f, peer_settings_, ack, err)) {
                return false;
            }
            if (ack) {
                return true;
            }
            for (const auto &[id, value] : peer_settings_) {
                switch (id) {
                    case http2::kSettingInitialWindowSize: {
                        // §6.9.2：这个设置只追溯地挪动**各条流**的窗口，连接级
                        // 窗口不受它影响（那个只由 WINDOW_UPDATE 改）。把增量
                        // 也加到连接窗口上，会让发出去的量超出对端实际允许的额度。
                        const int64_t delta =
                            static_cast<int64_t>(value) - static_cast<int64_t>(peer_initial_window_);
                        peer_initial_window_ = value;
                        for (auto &[stream, window] : outbound_streams_) {
                            window += delta;
                        }
                        break;
                    }
                    case http2::kSettingMaxFrameSize:
                        if (value >= 16384 && value <= http2::kMaxFrameSize) {
                            peer_max_frame_ = value;
                        }
                        break;
                    default:
                        break;
                }
            }
            settings_received_ = true;
            return send_bytes(http2::settings_ack_frame(), err);
        }
        case http2::kWindowUpdate: {
            uint32_t inc = 0;
            if (!http2::parse_window_update(f, inc, err)) {
                return false;
            }
            if (f.stream_id == 0) {
                outbound_connection_ += inc;
            } else {
                auto it = outbound_streams_.find(f.stream_id);
                const int64_t base =
                    it != outbound_streams_.end() ? it->second : peer_initial_window_;
                outbound_streams_[f.stream_id] = base + inc;
            }
            return true;
        }
        case http2::kPing:
            // 对端的活性探测必须照样式 ACK 回去，不理它的话设备会在超时后拆连接。
            if ((f.flags & http2::kFlagAck) != 0) {
                return true;
            }
            if (f.payload.size() < 8) {
                err = "PING 载荷不足 8 字节";
                return false;
            }
            {
                uint64_t opaque = 0;
                for (int i = 0; i < 8; ++i) {
                    opaque = opaque << 8 | f.payload[static_cast<std::size_t>(i)];
                }
                return send_bytes(http2::ping_frame(opaque, true), err);
            }
        case http2::kGoAway: {
            http2::GoAway g;
            if (!http2::parse_goaway(f, g, err)) {
                return false;
            }
            terminated_ = true;
            err = std::string("设备发来 GOAWAY ") + http2::error_code_name(g.error_code);
            if (!g.debug_data.empty()) {
                err += "：\"" + g.debug_data + "\"";
            }
            return false;
        }
        case http2::kRstStream: {
            uint32_t code = 0;
            if (!http2::parse_rst_stream(f, code, err)) {
                return false;
            }
            if (f.stream_id == kRootStream) {
                terminated_ = true;
                err = std::string("主通道被 RST：") + http2::error_code_name(code);
                return false;
            }
            return true;
        }
        case http2::kData: {
            std::span<const uint8_t> body;
            if (!http2::data_payload(f, body, err)) {
                return false;
            }
            auto &buf = pending_[f.stream_id];
            buf.insert(buf.end(), body.begin(), body.end());
            consumed_per_stream_[f.stream_id] += body.size();
            consumed_connection_ += body.size();
            replenish_inbound_window(f.stream_id);
            if ((f.flags & http2::kFlagEndStream) != 0 && f.stream_id == kRootStream) {
                terminated_ = true;
                err = "设备在主通道上置了 END_STREAM，连接已经结束";
                return false;
            }
            return true;
        }
        default:
            // HEADERS / CONTINUATION / PRIORITY 以及未定义的帧类型：载荷对我们
            // 没有信息量（路由在流号里），整帧丢掉即可。
            return true;
    }
}

void Channel::replenish_inbound_window(uint32_t stream_id) {
    if (consumed_connection_ >= kReplenishThreshold) {
        const auto n = static_cast<uint32_t>(std::min<uint64_t>(consumed_connection_, 0x7FFFFFFF));
        std::string ignored;
        write_all(socket_, http2::window_update_frame(0, n), ignored);
        consumed_connection_ -= n;
    }
    auto it = consumed_per_stream_.find(stream_id);
    if (it != consumed_per_stream_.end() && it->second >= kReplenishThreshold) {
        const auto n = static_cast<uint32_t>(std::min<uint64_t>(it->second, 0x7FFFFFFF));
        std::string ignored;
        write_all(socket_, http2::window_update_frame(stream_id, n), ignored);
        it->second -= n;
    }
}

bool Channel::send_data(uint32_t stream_id, std::span<const uint8_t> payload, std::string &err) {
    std::size_t off = 0;
    int idle_rounds = 0;
    while (off < payload.size()) {
        auto &stream_window =
            outbound_streams_.try_emplace(stream_id, peer_initial_window_).first->second;
        const auto budget = std::min({outbound_connection_, stream_window,
                                      static_cast<int64_t>(peer_max_frame_)});
        if (budget <= 0) {
            // 没有窗口就等对端补。这里只等得有限次：设备要是铁了心不放量，
            // 无限等会变成挂死，不如报出来。
            if (++idle_rounds > 5) {
                err = "对端迟迟不补发流控窗口，已发 " + std::to_string(off) + "/" +
                      std::to_string(payload.size()) + " 字节";
                return false;
            }
            if (!pump(1000, err)) {
                err = "等流控窗口时断开: " + err;
                return false;
            }
            continue;
        }
        idle_rounds = 0;
        const auto n = std::min<uint64_t>(static_cast<uint64_t>(budget),
                                          static_cast<uint64_t>(payload.size() - off));
        auto frame = http2::data_frame(stream_id, payload.subspan(off, n));
        if (!send_bytes(frame, err)) {
            err = "发 DATA 失败: " + err;
            return false;
        }
        outbound_connection_ -= static_cast<int64_t>(n);
        stream_window -= static_cast<int64_t>(n);
        off += static_cast<std::size_t>(n);
    }
    return true;
}

bool Channel::send_request(const xpc::Value &body, bool want_reply, std::string &err) {
    const auto flags = wrapper_flags(&body, want_reply);
    auto wire = xpc::encode_message(flags, next_message_id_, &body);
    if (wire.empty()) {
        err = "XPC 编码失败（深度或长度超限）";
        return false;
    }
    if (verbose_) {
        std::fprintf(stderr, "    -> id=%llu flags=0x%x %s\n",
                     static_cast<unsigned long long>(next_message_id_), flags,
                     xpc::describe(body).substr(0, 200).c_str());
    }
    if (!send_data(kRootStream, wire, err)) {
        return false;
    }
    ++next_message_id_;
    return true;
}

bool Channel::take_message(xpc::Value &out, std::string &err) {
    for (auto &[stream, buf] : pending_) {
        for (;;) {
            xpc::Message m;
            std::size_t used = 0;
            std::string derr;
            const auto st = xpc::decode_message(buf, m, used, derr);
            if (st == xpc::Status::Malformed) {
                err = "流 " + std::to_string(stream) + " 上的消息畸形: " + derr;
                return false;
            }
            if (st == xpc::Status::NeedMore) {
                break;
            }
            buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(used));
            // 只有「非空字典」才算回信。设备对我们每个握手帧各回一个空字典或
            // 空载荷帧当 ACK，把 `{}` 交上去的话调用方只看到一个没有 Services
            // 的对象，而真正的回信还在后面排队。
            if (m.has_body && m.body.is_dict() && !m.body.dict.empty()) {
                if (verbose_) {
                    std::fprintf(stderr, "    => 流%u id=%llu %s\n", stream,
                                 static_cast<unsigned long long>(m.message_id),
                                 xpc::describe(m.body).substr(0, 300).c_str());
                }
                out = std::move(m.body);
                return true;
            }
        }
    }
    return false;
}

bool Channel::receive(xpc::Value &out, int timeout_ms, std::string &err) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        if (take_message(out, err)) {
            return true;
        }
        if (!err.empty()) {
            return false;  // 畸形消息：字节流已经错位，再等只会更错
        }
        const auto left = remaining_ms(deadline);
        if (left == 0) {
            err = "等设备回信超时";
            return false;
        }
        if (terminated_) {
            err = "连接已终止";
            return false;
        }
        if (!pump(static_cast<int>(left), err)) {
            // pump 失败常常只是这一次 socket 读超时，而上一轮处理帧时攒下的完整
            // 消息还在缓冲里。不回头再看一眼，就会把已经收到的回信报成"没回信"——
            // 真机上正是这个次序：回信到齐、读超时、于是报超时。
            if (take_message(out, err)) {
                return true;
            }
            err = "等设备回信时断开: " + err;
            return false;
        }
    }
}

bool Channel::call(const xpc::Value &request, xpc::Value &reply, int timeout_ms,
                   std::string &err) {
    if (!send_request(request, true, err)) {
        return false;
    }
    return receive(reply, timeout_ms, err);
}

}  // namespace scrctl::remote
