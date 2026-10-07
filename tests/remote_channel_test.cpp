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
        auto bytes = std::span(reinterpret_cast<const uint8_t *>(data.data()), data.size());
        while (!bytes.empty()) {
            http2::Frame frame;
            std::size_t used = 0;
            if (http2::parse_frame(bytes, frame, used, err) != http2::Status::Ok || used == 0) {
                err = "test stream received incomplete frame";
                return false;
            }
            if (frame.type == http2::kWindowUpdate &&
                static_cast<int>(frame.stream_id) == fail_window_stream) {
                err = "injected transport write failure";
                return false;
            }
            sent.push_back(std::move(frame));
            bytes = bytes.subspan(used);
        }
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

void test_rejected_frame(std::vector<uint8_t> wire, const char *reason) {
    Stream stream;
    auto channel = open(stream);
    if (!channel) return;
    stream.incoming.push_back(std::move(wire));
    std::string err;
    check(channel->service(0, err), "read invalid control frame for validation");
    check(!channel->service(0, err) && err.find(reason) != std::string::npos,
          "invalid control frame reports its cause");
    const auto before = stream.sent.size();
    auto request = xpc::make_dict();
    xpc::dict_set(request, "request", xpc::make_bool(true));
    check(!channel->send_request(request, true, err) && stream.sent.size() == before,
          "protocol failure prevents further requests");
}

void append(std::vector<uint8_t> &batch, const std::vector<uint8_t> &frame) {
    batch.insert(batch.end(), frame.begin(), frame.end());
}
std::vector<uint8_t> file_reply(std::initializer_list<uint64_t> sizes) {
    auto body = xpc::make_dict();
    auto files = xpc::make_array();
    for (auto size : sizes) files.array.push_back(xpc::make_file_transfer(size));
    xpc::dict_set(body, "files", std::move(files));
    return http2::data_frame(1, xpc::encode_message(xpc::kFlagAlwaysSet | xpc::kFlagDataPresent, 1, &body));
}
std::vector<uint8_t> reset(uint32_t id) {
    return http2::serialize(http2::kRstStream, 0, id, std::vector<uint8_t>{0, 0, 0, 8});
}
void test_file_failure(std::vector<uint8_t> batch, const char *reason, const char *what,
                       bool expect_accept = false) {
    Stream stream;
    auto channel = open(stream);
    if (!channel) return;
    stream.incoming.push_back(std::move(batch));
    std::string err;
    xpc::Value reply;
    check(channel->wait(reply, 100, err) == remote::Channel::Wait::Broken &&
              err.find(reason) != std::string::npos, what);
    const auto accepts = std::count_if(stream.sent.begin(), stream.sent.end(), [](const auto &f) {
        return f.type == http2::kHeaders && f.stream_id % 2 == 0;
    });
    check((accepts != 0) == expect_accept, "file preflight rejects invalid batch before acceptance");
    const auto before = stream.sent.size();
    check(!channel->send_request(xpc::make_dict(), false, err) && stream.sent.size() == before &&
              channel->wait(reply, 0, err) == remote::Channel::Wait::Broken,
          "file failure stays Broken and prevents further writes");
}
void test_files() {
    auto extra = file_reply({3});
    append(extra, http2::data_frame(2, std::vector<uint8_t>{'O','L','D','N','E','W'}, http2::kFlagEndStream));
    test_file_failure(std::move(extra), "exceeds declared", "extra file bytes cannot spill into next reply");
    auto short_file = file_reply({3});
    append(short_file, http2::data_frame(2, std::vector<uint8_t>{'x'}, http2::kFlagEndStream));
    test_file_failure(std::move(short_file), "ended before", "short END_STREAM fails immediately");
    auto rst = file_reply({3});
    append(rst, http2::data_frame(2, std::vector<uint8_t>{'b','a','d'}));
    append(rst, reset(2));
    test_file_failure(std::move(rst), "stream reset", "RST_STREAM cannot be masked by complete file bytes");
    test_file_failure(reset(3), "stream reset", "reply stream reset is Broken");
    auto ended_reply = xpc::make_dict();
    xpc::dict_set(ended_reply, "ok", xpc::make_bool(true));
    for (uint32_t id : {1u, 3u}) {
        test_file_failure(http2::data_frame(id, xpc::encode_message(
                              xpc::kFlagAlwaysSet | xpc::kFlagDataPresent, 1, &ended_reply),
                              http2::kFlagEndStream),
                          "END_STREAM", "message channel END_STREAM is explicit termination");
    }
    test_file_failure(file_reply({xpc::kMaxBuffer + 1}), "attachments exceed", "oversize file rejected before acceptance");
    test_file_failure(file_reply({xpc::kMaxBuffer / 2, xpc::kMaxBuffer / 2 + 1}),
                      "attachments exceed", "attachment sum is bounded before acceptance");
    // 描述符先到、DATA 后到：处理终结或超量数据时已知声明长度。
    for (bool too_long : {false, true}) {
        Stream stream;
        auto channel = open(stream);
        if (!channel) return;
        stream.incoming.push_back(file_reply({3}));
        stream.incoming.push_back(http2::data_frame(2, std::vector<uint8_t>(too_long ? 4 : 1, 'x'),
                                                    http2::kFlagEndStream));
        std::string err;
        xpc::Value reply;
        check(channel->wait(reply, 100, err) == remote::Channel::Wait::Broken &&
                  err.find(too_long ? "exceeds declared" : "ended before") != std::string::npos,
              "active file stream validates later DATA length and END_STREAM");
    }
    Stream stream;
    auto channel = open(stream);
    if (!channel) return;
    stream.incoming.push_back(file_reply({3, 2, 0}));
    stream.incoming.push_back(http2::data_frame(2, std::vector<uint8_t>{'a'}));
    auto batch = http2::data_frame(2, std::vector<uint8_t>{'b','c'}, http2::kFlagEndStream);
    append(batch, http2::data_frame(4, std::vector<uint8_t>{'d','e'}, http2::kFlagEndStream));
    stream.incoming.push_back(std::move(batch));
    std::string err;
    xpc::Value reply;
    check(channel->wait(reply, 100, err) == remote::Channel::Wait::Message && err.empty() &&
              reply.at("files").array[0].data == std::vector<uint8_t>({'a','b','c'}) &&
              reply.at("files").array[1].data == std::vector<uint8_t>({'d','e'}) &&
              reply.at("files").array[2].data.empty(),
          "fragmented files and zero-length attachment receive exact bytes");
    stream.incoming.push_back(file_reply({3}));
    check(channel->wait(reply, 100, err) == remote::Channel::Wait::Broken &&
              err.find("cannot be reused") != std::string::npos,
          "consumed file stream cannot provide a later reply attachment");
    // 数据先按声明长度收齐，设备随后才用独立的空 DATA 终结文件流。
    for (int followup = 0; followup < 4; ++followup) {
        Stream trailing_stream;
        auto trailing = open(trailing_stream);
        if (!trailing) return;
        auto exact = file_reply({1});
        append(exact, http2::data_frame(2, std::vector<uint8_t>{'x'}));
        trailing_stream.incoming.push_back(std::move(exact));
        check(trailing->wait(reply, 100, err) == remote::Channel::Wait::Message &&
                  reply.at("files").array[0].data == std::vector<uint8_t>{'x'},
              "declared file length remains sufficient without END_STREAM");
        if (followup == 3) {
            trailing_stream.incoming.push_back(http2::data_frame(2, std::vector<uint8_t>{'y'}));
            check(trailing->service(0, err) && !trailing->service(0, err) &&
                      err.find("cannot be reused") != std::string::npos,
                  "late payload before END_STREAM is rejected after consumption");
            continue;
        }
        trailing_stream.incoming.push_back(http2::data_frame(2, {}, http2::kFlagEndStream));
        check(trailing->service(0, err) && trailing->service(0, err) && err.empty(),
              "one delayed empty END_STREAM closes a consumed file normally");
        if (followup == 2) {
            trailing_stream.incoming.push_back(file_reply({1}));
            check(trailing->wait(reply, 100, err) == remote::Channel::Wait::Broken &&
                      err.find("cannot be reused") != std::string::npos,
                  "new attachment metadata cannot reuse a file after delayed END_STREAM");
        } else {
            trailing_stream.incoming.push_back(followup == 0
                ? http2::data_frame(2, {}, http2::kFlagEndStream)
                : http2::data_frame(2, std::vector<uint8_t>{'y'}));
            check(trailing->service(0, err) && !trailing->service(0, err) &&
                      err.find("cannot be reused") != std::string::npos,
                  followup == 0 ? "duplicate delayed END_STREAM is rejected"
                                : "late payload after delayed END_STREAM is rejected");
        }
    }
}
void test_stream_count() {
    Stream stream;
    auto channel = open(stream);
    if (!channel) return;
    std::vector<uint8_t> batch;
    for (uint32_t i = 1; i <= 100; ++i) append(batch, http2::data_frame(2*i, {}));
    stream.incoming.push_back(std::move(batch));
    std::string err;
    check(channel->service(0, err) && channel->service(0, err), "100 even file stream slots accepted");
    stream.incoming.push_back(http2::data_frame(202, {}));
    check(channel->service(0, err) && !channel->service(0, err) &&
              err.find("stream limit") != std::string::npos,
          "101st even file stream rejected even with empty DATA");
    test_rejected_frame(http2::data_frame(5, {}), "Unsupported");
    auto body = xpc::make_dict();
    auto files = xpc::make_array();
    for (int i = 0; i < 101; ++i) files.array.push_back(xpc::make_file_transfer(0));
    xpc::dict_set(body, "files", std::move(files));
    test_file_failure(http2::data_frame(1, xpc::encode_message(
                          xpc::kFlagAlwaysSet | xpc::kFlagDataPresent, 1, &body)),
                      "stream limit", "101 zero-length attachment descriptors rejected");
}
// 只调用 service() 缓冲、不解码 XPC，确认空闲订阅也受输入上限保护。
// 分批送入数据，避免测试自身一次保留过大的输入。
bool feed(Stream &stream, remote::Channel &channel, uint32_t id, std::size_t count, std::string &err) {
    while (count != 0) {
        const auto n = std::min(count, std::size_t(1u << 20));
        stream.incoming.push_back(http2::data_frame(id, std::vector<uint8_t>(n, 42)));
        if (!channel.service(0, err) || !channel.service(0, err)) return false;
        count -= n;
    }
    return true;
}
void test_buffer_limits() {
    std::string err;
    {
        Stream stream;
        auto channel = open(stream);
        if (!channel) return;
        check(feed(stream, *channel, 1, xpc::kMaxBuffer + 24, err), "XPC stream permits 32 MiB plus wrapper");
        check(!feed(stream, *channel, 1, 1, err) && err.find("message stream input exceeds") != std::string::npos,
              "service without decoder enforces XPC stream byte limit");
    }
    {
        Stream stream;
        auto channel = open(stream);
        if (!channel) return;
        check(feed(stream, *channel, 2, xpc::kMaxBuffer, err), "raw file stream permits 32 MiB");
        check(!feed(stream, *channel, 2, 1, err) && err.find("file data exceeds") != std::string::npos,
              "raw file stream byte limit applies before descriptor arrives");
    }
    {
        Stream stream;
        auto channel = open(stream);
        if (!channel) return;
        check(feed(stream, *channel, 2, xpc::kMaxBuffer - 32, err) &&
                  feed(stream, *channel, 4, xpc::kMaxBuffer - 32, err),
              "independent streams fit within connection input budget");
        stream.incoming.push_back(http2::data_frame(6, std::vector<uint8_t>(56, 42)));
        check(!channel->service(0, err) && err.find("connection input exceeds") != std::string::npos,
              "connection limit includes rx bytes and all raw stream buffers");
    }
    {
        Stream stream;
        auto channel = open(stream);
        if (!channel) return;
        // 未知流不能保留窗口状态：若保存了 +1，后续初始窗口调整就会溢出。
        // 同时确认这些更新没有占用随后 100 个实际文件流的记录。
        std::vector<uint8_t> batch;
        for (uint32_t i = 1; i <= 1000; ++i) append(batch, http2::window_update_frame(2*i, 1));
        append(batch, http2::settings_frame({{http2::kSettingInitialWindowSize, 0x7FFFFFFFu}}));
        for (uint32_t i = 1; i <= 100; ++i) append(batch, http2::data_frame(2*i, {}));
        stream.incoming.push_back(std::move(batch));
        check(channel->service(0, err) && channel->service(0, err),
              "unknown WINDOW_UPDATEs leave file stream slots available");
    }
}
} // namespace

int main() {
    test_wait();
    test_files();
    test_stream_count();
    test_buffer_limits();
    test_window_padding();
    test_window_failure(0);
    test_window_failure(2);
    test_rejected_frame(http2::serialize(http2::kSettings, http2::kFlagAck, 0,
                                        std::vector<uint8_t>{1}), "ACK");
    test_rejected_frame(http2::settings_frame({{http2::kSettingInitialWindowSize, 0x80000000u}}),
                        "INITIAL_WINDOW_SIZE");
    test_rejected_frame(http2::window_update_frame(0, 0), "nonzero");
    test_rejected_frame(http2::window_update_frame(0, 0x7FFFFFFFu), "2^31");
    test_rejected_frame(http2::window_update_frame(1, 0x7FFFFFFFu), "2^31");
    test_rejected_frame(http2::serialize(http2::kPing, http2::kFlagAck, 0,
                                        std::vector<uint8_t>(9)), "exactly 8");
    test_rejected_frame(http2::data_frame(0, std::vector<uint8_t>{1}), "nonzero");
    return failures ? 1 : 0;
}
