#include "i18n/Translation.h"
#include "remote/RemoteXpc.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace scrctl::remote {
namespace {

/// 每条流的初始接收窗口为 16 MiB。服务目录可能超过默认的 65535 字节，
/// 较大的窗口减少接收大回复时的流控等待。
constexpr uint32_t kGrantWindow = 16u << 20;
constexpr uint32_t kWindowIncr = kGrantWindow - http2::kDefaultInitialWindowSize;


/// 累计消费 1 MiB 后补充窗口，减少 WINDOW_UPDATE 帧数量。
constexpr uint64_t kReplenishThreshold = 1u << 20;

bool write_all(net::ByteStream &sock, std::span<const uint8_t> data, std::string &err) {
    return sock.send(std::string_view(reinterpret_cast<const char *>(data.data()), data.size()),
                     err);
}

bool write_all(net::ByteStream &sock, std::string_view data, std::string &err) {
    return sock.send(data, err);
}

/// 非空载荷设置 DATA_PRESENT。当前 RemoteXPC 约定中，空字典仅设置
/// ALWAYS_SET；WANTING_REPLY 独立由调用方指定。
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
    // 请求展示敏感属性；已验证的服务发现路径依赖此标志。
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
        // 写入前检查数字数量，避免超过 16 字节数组。
        if (digits >= out.size() * 2) {
            return std::nullopt;
        }
        out[digits / 2] = static_cast<uint8_t>(digits % 2 == 0 ? (v << 4)
                                                              : (out[digits / 2] | v));
        ++digits;
    }
    if (digits != 32) {
        return std::nullopt;
    }
    return out;
}

bool Channel::send_bytes(std::span<const uint8_t> data, std::string &err) {
    if (verbose_) {
        std::fprintf(stderr, SCRCTL_TR("    >> %zu bytes: "), data.size());
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
        err = SCRCTL_TR("Failed to send HTTP/2 preface: ") + err;
        return false;
    }
    // 2. 声明流级接收窗口和并发流数量，沿用已验证的 RemoteXPC 设置。
    if (!send_bytes(
            http2::settings_frame({{http2::kSettingMaxConcurrentStreams, 100},
                                   {http2::kSettingInitialWindowSize, kGrantWindow}}),
            err)) {
        err = SCRCTL_TR("Failed to send SETTINGS: ") + err;
        return false;
    }
    // 3. 将连接级接收窗口增加到 16 MiB。
    //    发送窗口仍使用默认值，直到对端 SETTINGS / WINDOW_UPDATE 更新。
    if (!send_bytes(http2::window_update_frame(0, kWindowIncr), err)) {
        err = SCRCTL_TR("Failed to send WINDOW_UPDATE: ") + err;
        return false;
    }
    // 4–8. 按设备要求建立主通道和回信通道。
    //
    // XPC wrapper 必须放入 DATA 帧。直接发送 wrapper 会使其 magic 被当作
    // HTTP/2 帧头，可能表现为帧长度错误。
    if (!send_bytes(http2::headers_frame(kRootStream), err)) {
        err = SCRCTL_TR("Failed to send primary channel HEADERS: ") + err;
        return false;
    }
    auto empty_dict = xpc::make_dict();
    auto opener = xpc::encode_message(wrapper_flags(&empty_dict, false), 0, &empty_dict);
    if (!send_bytes(http2::data_frame(kRootStream, opener), err)) {
        err = SCRCTL_TR("Failed to send first frame: ") + err;
        return false;
    }
    next_message_id_ = 1;
    if (!send_bytes(http2::headers_frame(kReplyStream), err)) {
        err = SCRCTL_TR("Failed to send reply channel HEADERS: ") + err;
        return false;
    }
    // 主通道的「终止帧」：空载荷，flags = ALWAYS_SET | 0x200（见常量注释）。
    auto term = xpc::encode_message(xpc::kFlagAlwaysSet | xpc::kFlagTermChannel, 0, nullptr);
    if (!send_bytes(http2::data_frame(kRootStream, term), err)) {
        err = SCRCTL_TR("Failed to send terminating frame: ") + err;
        return false;
    }
    auto init = xpc::encode_message(xpc::kFlagAlwaysSet | xpc::kFlagInitHandshake, 0, nullptr);
    if (!send_bytes(http2::data_frame(kReplyStream, init), err)) {
        err = SCRCTL_TR("Failed to send INIT_HANDSHAKE frame: ") + err;
        return false;
    }

    // 9. 等对端 SETTINGS 并 ACK。它可能夹在 WINDOW_UPDATE 后面来，所以按类型筛。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!settings_received_) {
        const auto left = remaining_ms(deadline);
        if (left == 0) {
            err = SCRCTL_TR("Timed out waiting for device SETTINGS");
            return false;
        }
        if (!pump(static_cast<int>(left), err)) {
            err = SCRCTL_TR("Disconnected while waiting for device SETTINGS: ") + err;
            return false;
        }
    }

    return true;
}

bool Channel::announce_device(const PeerIdentity &identity, std::string &err) {
    auto hs = build_handshake(identity);
    if (!send_request(hs, false, err)) {
        err = SCRCTL_TR("Failed to send RemoteXPC identity: ") + err;
        return false;
    }
    xpc::Value info;
    if (!receive(info, 5000, err)) {
        err = SCRCTL_TR("Failed to read peer_info: ") + err;
        return false;
    }
    peer_info_ = std::move(info);
    return true;
}

std::optional<Channel> Channel::open(net::ByteStream &socket, std::string &err, bool verbose) {
    std::optional<Channel> ch;
    ch.emplace(socket);
    ch->verbose_ = verbose;
    if (!ch->start(err)) {
        return std::nullopt;
    }
    return ch;
}

bool Channel::pump(int timeout_ms, std::string &err) {
    maybe_open_dump();
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
            // 记录缓冲前缀及流内偏移，便于与原始 dump 对照。
            // 这些信息用于定位问题，不足以单独判断是设备、传输还是本地解析错误。
            std::fprintf(stderr,
                         SCRCTL_TR(
                             "    HTTP/2 parse failed: %s; buffered %zu bytes, stream offset %llu (total "
                             "received %llu), first 64 bytes:\n      "),
                         perr.c_str(), rx_.size(),
                         static_cast<unsigned long long>(rx_total_ - rx_.size()),
                         static_cast<unsigned long long>(rx_total_));
            for (std::size_t i = 0; i < 64 && i < rx_.size(); ++i) {
                std::fprintf(stderr, "%02x", rx_[i]);
                if (i % 16 == 15) {
                    std::fprintf(stderr, "\n      ");
                }
            }
            std::fprintf(stderr, "\n");
            return false;
        }
        break;  // 缓冲里只剩半帧，去收字节
    }
    // 已处理完整帧时先交回控制权，让调用方读取可能已经收全的回复，
    // 避免继续阻塞等待下一批数据。
    if (processed) {
        return true;
    }

    std::vector<uint8_t> got;
    bool timed_out = false;
    if (!socket_.recv(got, timeout_ms, err, &timed_out)) {
        if (timed_out) {
            // 单次读取超时不判定为断连。调用方按自己的总 deadline 决定是否继续等待。
            return true;
        }
        return false;
    }
    rx_.insert(rx_.end(), got.begin(), got.end());
    rx_total_ += got.size();
    if (dump_ != nullptr) {
        std::fwrite(got.data(), 1, got.size(), dump_.get());
        std::fflush(dump_.get());
    }
    return true;
}

/// 每条连接使用独立 dump 序号，避免不同服务的字节混合。
void Channel::maybe_open_dump() {
    if (dump_ != nullptr || opened_dump_) {
        return;
    }
    opened_dump_ = true;
    const char *prefix = std::getenv("SCRCTL_H2_DUMP");
    if (prefix == nullptr || prefix[0] == '\0') {
        return;
    }
    static std::atomic<int> seq { 0 };
    const std::string path = std::string(prefix) + "." + std::to_string(seq.fetch_add(1)) + ".bin";
    dump_.reset(std::fopen(path.c_str(), "wb"));
    if (dump_ != nullptr) {
        std::fprintf(stderr, SCRCTL_TR("    HTTP/2 input dump -> %s\n"), path.c_str());
    }
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
                        // RFC 7540 §6.9.2：INITIAL_WINDOW_SIZE 的变化作用于各流的发送窗口，
                        // 不修改连接级窗口；后者仅由 WINDOW_UPDATE 更新。
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
            // 非 ACK 的 PING 原样应答，维持设备的连接活性检查。
            if ((f.flags & http2::kFlagAck) != 0) {
                return true;
            }
            if (f.payload.size() < 8) {
                err = SCRCTL_TR("PING payload shorter than 8 bytes");
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
            err = std::string(SCRCTL_TR("Device sent GOAWAY ")) + http2::error_code_name(g.error_code);
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
                err = std::string(SCRCTL_TR("Primary channel reset: ")) + http2::error_code_name(code);
                return false;
            }
            return true;
        }
        case http2::kData: {
            std::span<const uint8_t> body;
            if (!http2::data_payload(f, body, err)) {
                return false;
            }
            // 偶数号流是设备发起的，上面跑的是文件裸字节，不是 XPC 消息。
            auto &buf = (f.stream_id % 2 == 0) ? raw_[f.stream_id] : pending_[f.stream_id];
            buf.insert(buf.end(), body.begin(), body.end());
            consumed_per_stream_[f.stream_id] += body.size();
            consumed_connection_ += body.size();
            replenish_inbound_window(f.stream_id);
            if ((f.flags & http2::kFlagEndStream) != 0 && f.stream_id == kRootStream) {
                terminated_ = true;
                err = SCRCTL_TR("Device set END_STREAM on primary channel; connection ended");
                return false;
            }
            return true;
        }
        default:
            // 本适配不消费 HEADERS / CONTINUATION / PRIORITY 的载荷；
            // RemoteXPC 根据流号路由 XPC 消息。未识别帧也按当前策略忽略。
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
            // 发送窗口耗尽后最多等待五轮，每轮读超时上限 1 秒。
            // 这是轮数限制，实际总时长仍受底层读写耗时影响。
            if (++idle_rounds > 5) {
                err = SCRCTL_TR("Peer did not replenish flow-control window; sent ") + std::to_string(off) + "/" +
                      std::to_string(payload.size()) + SCRCTL_TR(" bytes");
                return false;
            }
            if (!pump(1000, err)) {
                err = SCRCTL_TR("Disconnected while waiting for flow-control window: ") + err;
                return false;
            }
            continue;
        }
        idle_rounds = 0;
        const auto n = std::min<uint64_t>(static_cast<uint64_t>(budget),
                                          static_cast<uint64_t>(payload.size() - off));
        auto frame = http2::data_frame(stream_id, payload.subspan(off, n));
        if (!send_bytes(frame, err)) {
            err = SCRCTL_TR("Failed to send DATA: ") + err;
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
        err = SCRCTL_TR("XPC encoding failed (depth or length limit exceeded)");
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

void Channel::collect_files(const xpc::Value &v, std::vector<xpc::Value *> &out) {
    for (auto &entry : const_cast<xpc::Value &>(v).dict) {
        if (entry.value.type == xpc::Type::FileTransfer) {
            out.push_back(&entry.value);
        } else if (entry.value.is_dict() || entry.value.is_array()) {
            collect_files(entry.value, out);
        }
    }
    for (auto &item : const_cast<xpc::Value &>(v).array) {
        if (item.type == xpc::Type::FileTransfer) {
            out.push_back(&item);
        } else if (item.is_dict() || item.is_array()) {
            collect_files(item, out);
        }
    }
}

bool Channel::receive_file(uint32_t stream_id, uint64_t size,
                           std::chrono::steady_clock::time_point deadline,
                           std::vector<uint8_t> &out, std::string &err) {
    // 按当前文件流约定，先发送 HEADERS 和 FILE_TX_STREAM_RESPONSE 接受帧，
    // 再接收该流的原始文件数据。
    std::vector<uint8_t> frames = http2::headers_frame(stream_id);
    auto accept = xpc::encode_message(xpc::kFlagAlwaysSet | xpc::kFlagFileTxResponse, 0, nullptr);
    auto ack_frame = http2::data_frame(stream_id, accept);
    frames.insert(frames.end(), ack_frame.begin(), ack_frame.end());
    if (!send_bytes(frames, err)) {
        err = SCRCTL_TR("Failed to accept file stream: ") + err;
        return false;
    }
    out.clear();
    out.reserve(static_cast<std::size_t>(std::min<uint64_t>(size, 64u << 20)));
    while (raw_[stream_id].size() < size) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        if (left <= 0) {
            err = SCRCTL_TR("File receive timed out: stream ") + std::to_string(stream_id) + SCRCTL_TR(" has received ") +
                  std::to_string(raw_[stream_id].size()) + "/" + std::to_string(size) + SCRCTL_TR(" bytes");
            return false;
        }
        if (!pump(static_cast<int>(left), err)) {
            if (raw_[stream_id].size() >= size) {
                break;
            }
            err = SCRCTL_TR("Disconnected while receiving file: ") + err;
            return false;
        }
    }
    out.assign(raw_[stream_id].begin(), raw_[stream_id].begin() + static_cast<std::ptrdiff_t>(size));
    raw_[stream_id].erase(raw_[stream_id].begin(),
                          raw_[stream_id].begin() + static_cast<std::ptrdiff_t>(size));
    return true;
}

bool Channel::materialize_files(xpc::Value &reply,
                                std::chrono::steady_clock::time_point deadline,
                                std::string &err) {
    std::vector<xpc::Value *> files;
    collect_files(reply, files);
    if (files.empty()) {
        return true;
    }
    // 按 FileTransfer 在回复中的遍历顺序分配偶数流 2、4、6……。
    // 此映射仍需真机文件子流验证，verbose 日志记录请求接收的流号和长度。
    for (std::size_t i = 0; i < files.size(); ++i) {
      const auto stream_id = static_cast<uint32_t>((i + 1) * 2);
        if (files[i]->file_size == 0) {
            continue;
        }
        if (verbose_) {
            std::fprintf(stderr, SCRCTL_TR("    Receiving file: %llu bytes on stream %u\n"),
                         static_cast<unsigned long long>(files[i]->file_size), stream_id);
        }
        if (!receive_file(stream_id, files[i]->file_size, deadline, files[i]->data, err)) {
            return false;
        }
    }
    return true;
}

bool Channel::take_message(xpc::Value &out,
                           std::chrono::steady_clock::time_point deadline, std::string &err) {
    for (auto &[stream, buf] : pending_) {
        for (;;) {
            xpc::Message m;
            std::size_t used = 0;
            std::string derr;
            const auto st = xpc::decode_message(buf, m, used, derr);
            if (st == xpc::Status::Malformed) {
                err = SCRCTL_TR("Stream ") + std::to_string(stream) + SCRCTL_TR(" contains malformed message: ") + derr;
                return false;
            }
            if (st == xpc::Status::NeedMore) {
                // verbose 记录消息欠缺字节数，便于结合原始入流排查未完成的回复。
                // 仅凭本地缓冲不足不能断定对端是否已发完。
                if (verbose_) {
                    std::fprintf(stderr, SCRCTL_TR("    Stream %u message incomplete: %s\n"), stream, derr.c_str());
                }
                break;
            }
            buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(used));
            // 跳过握手 ACK 使用的空载荷和空字典；仅向调用方返回非空字典消息。
            if (m.has_body && m.body.is_dict() && !m.body.dict.empty()) {
                if (verbose_) {
                    std::fprintf(stderr, SCRCTL_TR("    => stream %u id=%llu %s\n"), stream,
                                 static_cast<unsigned long long>(m.message_id),
                                 xpc::describe(m.body).substr(0, 300).c_str());
                }
                out = std::move(m.body);
                return materialize_files(out, deadline, err);
            }
        }
    }
    return false;
}

Channel::Wait Channel::wait(xpc::Value &out, int timeout_ms, std::string &err) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        if (take_message(out, deadline, err)) {
            return Wait::Message;
        }
        if (!err.empty()) {
            return Wait::Broken;  // 解析或文件接收失败。
        }
        const auto left = remaining_ms(deadline);
        if (left == 0) {
            err = SCRCTL_TR("Timed out waiting for device reply");
            return Wait::Timeout;
        }
        if (terminated_) {
            err = SCRCTL_TR("Connection terminated");
            return Wait::Broken;
        }
        if (!pump(static_cast<int>(left), err)) {
            // 连接失败前可能已处理并缓冲一条完整回复，最后检查一次。
            // 读超时由 pump 返回 true；这里处理的是其他连接或协议错误。
            if (take_message(out, deadline, err)) {
                return Wait::Message;
            }
            err = SCRCTL_TR("Disconnected while waiting for device reply: ") + err;
            return Wait::Broken;
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
