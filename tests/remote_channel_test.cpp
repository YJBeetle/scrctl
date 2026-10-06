#include "http2/Framing.h"
#include "remote/RemoteXpc.h"

#include <algorithm>
#include <cstdio>
#include <deque>

namespace {
using namespace scrctl;
int failures = 0;
void check(bool ok, const char *what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

// 模拟字节传输，记录客户端帧并允许在指定 WINDOW_UPDATE 上注入写失败。
// 测试只经过公开 Channel API，不访问窗口计数等内部状态。
class Stream final : public net::ByteStream {
public:
    std::deque<std::vector<uint8_t>> incoming;
    std::vector<http2::Frame> sent;
    int fail_window_stream = -1;
    bool send(std::string_view data, std::string &err) override {
        err.clear();
        if (data == http2::kClientPreface) return true;
        http2::Frame frame;
        std::size_t used = 0;
        const auto bytes = std::span(reinterpret_cast<const uint8_t *>(data.data()), data.size());
        if (http2::parse_frame(bytes, frame, used, err) != http2::Status::Ok || used != bytes.size()) {
            err = "test stream received incomplete frame";
            return false;
        }
        if (frame.type == http2::kWindowUpdate &&
            static_cast<int>(frame.stream_id) == fail_window_stream) {
            err = "injected transport write failure";
            return false;
        }
        sent.push_back(std::move(frame));
        return true;
    }
    bool recv(std::vector<uint8_t> &out, int, std::string &err, bool *timed_out) override {
        err.clear();
        out.clear();
        if (timed_out) *timed_out = incoming.empty();
        if (incoming.empty()) {
            if (!timed_out) err = "test read timeout";
            return false;
        }
        out = std::move(incoming.front());
        incoming.pop_front();
        return true;
    }
};

std::optional<remote::Channel> open(Stream &stream) {
    stream.incoming.push_back(http2::settings_frame({}));
    std::string err;
    auto channel = remote::Channel::open(stream, err);
    check(channel.has_value(), "handshake accepts empty peer SETTINGS");
    stream.sent.clear();
    return channel;
}

void test_wait() {
    Stream stream;
    auto channel = open(stream);
    if (!channel) return;
    std::string err;
    check(channel->service(0, err) && err.empty(), "read timeout does not break service");
    xpc::Value reply;
    check(channel->wait(reply, 0, err) == remote::Channel::Wait::Timeout && !err.empty(),
          "wait reports timeout");
    auto body = xpc::make_dict();
    xpc::dict_set(body, "ok", xpc::make_bool(true));
    const auto message = xpc::encode_message(xpc::kFlagAlwaysSet | xpc::kFlagDataPresent, 1, &body);
    const auto frame = http2::data_frame(1, message);
    // 将同一帧拆成多个 recv 返回，覆盖帧头和 XPC 的半包路径。
    for (std::size_t i = 0; i < frame.size(); i += 7) {
        stream.incoming.emplace_back(frame.begin() + i, frame.begin() + std::min(i + 7, frame.size()));
    }
    check(channel->wait(reply, 100, err) == remote::Channel::Wait::Message &&
              reply.at("ok").boolean && err.empty(),
          "next wait discards previous timeout error and reads fragmented reply");
}

void queue_padded_data(Stream &stream) {
    // 整帧载荷恰好到补充窗口阈值，业务字节略少于阈值。
    // RFC 9113 §6.1 要求 Pad Length 和填充也计入流控。
    std::vector<uint8_t> payload(1u << 20, 42);
    payload[0] = 16;
    std::fill(payload.end() - 16, payload.end(), 0);
    stream.incoming.push_back(http2::data_frame(2, payload, http2::kFlagPadded));
}

void test_window_padding() {
    Stream stream;
    auto channel = open(stream);
    if (!channel) return;
    queue_padded_data(stream);
    std::string err;
    check(channel->service(0, err) && channel->service(0, err), "padded DATA accepted");
    std::uint32_t connection = 0, per_stream = 0;
    for (const auto &f : stream.sent) {
        std::uint32_t increment = 0;
        if (f.type != http2::kWindowUpdate || !http2::parse_window_update(f, increment, err)) continue;
        if (f.stream_id == 0) connection += increment;
        if (f.stream_id == 2) per_stream += increment;
    }
    check(connection == (1u << 20) && per_stream == connection,
          "connection and stream windows include DATA padding bytes");
}

void test_window_failure(int stream_id) {
    Stream stream;
    auto channel = open(stream);
    if (!channel) return;
    stream.fail_window_stream = stream_id;
    // 使用未填充载荷，使失败用例独立于填充计数修复。
    stream.incoming.push_back(http2::data_frame(2, std::vector<uint8_t>(1u << 20, 42)));
    std::string err;
    check(channel->service(0, err), "service receives DATA bytes before parsing");
    check(!channel->service(0, err) && err.find("injected transport write failure") != std::string::npos,
          stream_id == 0 ? "connection WINDOW_UPDATE write failure is reported"
                         : "stream WINDOW_UPDATE write failure is reported");
    const auto sent_count = stream.sent.size();
    auto request = xpc::make_dict();
    xpc::dict_set(request, "request", xpc::make_bool(true));
    check(!channel->send_request(request, true, err) && stream.sent.size() == sent_count,
          "failed channel rejects further requests");
    check(!channel->service(0, err), "failed channel stays terminated");
}
} // namespace

int main() {
    test_wait();
    test_window_padding();
    test_window_failure(0);
    test_window_failure(2);
    return failures ? 1 : 0;
}
