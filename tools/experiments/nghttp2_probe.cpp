// 可选的兼容性探针；不参与 scrctl 的生产传输路径，不输出 XPC 内容或密钥。
#include "app/DeviceConnection.h"
#include "http2/Framing.h"
#include "remote/Rsd.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <nghttp2/nghttp2.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace scrctl;
void require(bool ok, const char *what) {
    if (!ok)
        throw std::runtime_error(what);
}
void api(int code) {
    if (code < 0)
        throw std::runtime_error(nghttp2_strerror(code));
}

class Candidate {
    struct Data {
        std::vector<uint8_t> bytes;
        size_t offset = 0;
    };
    std::vector<std::unique_ptr<Data>> data_;
    std::unique_ptr<nghttp2_session, decltype(&nghttp2_session_del)> session_{nullptr,
                                                                              nghttp2_session_del};
    static Candidate &self(void *p) { return *static_cast<Candidate *>(p); }
    static int received(nghttp2_session *, const nghttp2_frame *f, void *p) {
        if (f->hd.type == NGHTTP2_SETTINGS && !(f->hd.flags & NGHTTP2_FLAG_ACK))
            self(p).settings = true;
        return 0;
    }
    static int sent(nghttp2_session *, const nghttp2_frame *f, void *p) {
        auto &c = self(p);
        c.sent_frames[{f->hd.type, f->hd.stream_id}]++;
        if (f->hd.type == NGHTTP2_DATA)
            c.sent_data += f->hd.length;
        return 0;
    }
    static int invalid(nghttp2_session *, const nghttp2_frame *f, int code, void *p) {
        self(p).invalid_frames++;
        std::printf("invalid incoming frame type=%u stream=%d: %s\n", f->hd.type, f->hd.stream_id,
                    nghttp2_strerror(code));
        return 0;
    }
    static int not_sent(nghttp2_session *, const nghttp2_frame *f, int code, void *p) {
        self(p).unsent_frames++;
        std::printf("unsent frame type=%u stream=%d: %s\n", f->hd.type, f->hd.stream_id,
                    nghttp2_strerror(code));
        return 0;
    }
    static int chunk(nghttp2_session *, uint8_t, int32_t id, const uint8_t *bytes, size_t size,
                     void *p) {
        auto &buffer = self(p).inbound[id];
        buffer.insert(buffer.end(), bytes, bytes + size);
        return 0;
    }
    static ssize_t read(nghttp2_session *, int32_t, uint8_t *out, size_t size, uint32_t *flags,
                        nghttp2_data_source *source, void *) {
        auto &data = *static_cast<Data *>(source->ptr);
        size = std::min(size, data.bytes.size() - data.offset);
        if (size)
            std::memcpy(out, data.bytes.data() + data.offset, size);
        data.offset += size;
        if (data.offset == data.bytes.size())
            *flags |= NGHTTP2_DATA_FLAG_EOF | NGHTTP2_DATA_FLAG_NO_END_STREAM;
        return static_cast<ssize_t>(size);
    }

  public:
    bool settings = false;
    int invalid_frames = 0, unsent_frames = 0;
    size_t sent_data = 0;
    std::map<std::pair<uint8_t, int32_t>, size_t> sent_frames;
    std::map<int32_t, std::vector<uint8_t>> inbound;
    Candidate() {
        nghttp2_session_callbacks *raw_callbacks = nullptr;
        api(nghttp2_session_callbacks_new(&raw_callbacks));
        std::unique_ptr<nghttp2_session_callbacks, decltype(&nghttp2_session_callbacks_del)>
            callbacks(raw_callbacks, nghttp2_session_callbacks_del);
        nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks.get(), received);
        nghttp2_session_callbacks_set_on_frame_send_callback(callbacks.get(), sent);
        nghttp2_session_callbacks_set_on_invalid_frame_recv_callback(callbacks.get(), invalid);
        nghttp2_session_callbacks_set_on_frame_not_send_callback(callbacks.get(), not_sent);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks.get(), chunk);
        nghttp2_option *raw_option = nullptr;
        api(nghttp2_option_new(&raw_option));
        std::unique_ptr<nghttp2_option, decltype(&nghttp2_option_del)> option(raw_option,
                                                                              nghttp2_option_del);
        nghttp2_option_set_no_http_messaging(option.get(), 1);
        nghttp2_session *raw_session = nullptr;
        api(nghttp2_session_client_new2(&raw_session, callbacks.get(), this, option.get()));
        session_.reset(raw_session);
        const nghttp2_settings_entry settings[] = {
            {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100},
            {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, 16u << 20}};
        api(nghttp2_submit_settings(session_.get(), NGHTTP2_FLAG_NONE, settings, 2));
        api(nghttp2_submit_window_update(session_.get(), 0, 0, (16u << 20) - 65535));
    }
    int open() {
        return nghttp2_submit_headers(session_.get(), 0, -1, nullptr, nullptr, 0, nullptr);
    }
    int open_even() {
        return nghttp2_submit_headers(session_.get(), 0, 2, nullptr, nullptr, 0, nullptr);
    }
    int next_even() { return nghttp2_session_set_next_stream_id(session_.get(), 6); }
    void data(int id, std::vector<uint8_t> bytes) {
        auto source = std::make_unique<Data>();
        source->bytes = std::move(bytes);
        nghttp2_data_provider provider{};
        provider.source.ptr = source.get();
        provider.read_callback = read;
        data_.push_back(std::move(source));
        api(nghttp2_submit_data(session_.get(), 0, id, &provider));
    }
    std::vector<uint8_t> drain() {
        std::vector<uint8_t> out;
        const uint8_t *bytes = nullptr;
        for (;;) {
            const auto size = nghttp2_session_mem_send(session_.get(), &bytes);
            api(static_cast<int>(size));
            if (!size)
                return out;
            out.insert(out.end(), bytes, bytes + size);
        }
    }
    void feed(std::span<const uint8_t> bytes) {
        const auto size = nghttp2_session_mem_recv(session_.get(), bytes.data(), bytes.size());
        api(static_cast<int>(size));
        require(size == static_cast<ssize_t>(bytes.size()), "nghttp2 did not consume full input");
    }
    std::vector<uint8_t> bootstrap() {
        auto out = drain();
        auto append = [&] {
            const auto next = drain();
            out.insert(out.end(), next.begin(), next.end());
        };
        require(open() == 1, "root stream ID");
        append();
        auto body = xpc::make_dict();
        data(1, xpc::encode_message(xpc::kFlagAlwaysSet, 0, &body));
        append();
        require(open() == 3, "reply stream ID");
        append();
        data(1, xpc::encode_message(xpc::kFlagAlwaysSet | xpc::kFlagTermChannel, 0, nullptr));
        append();
        data(3, xpc::encode_message(xpc::kFlagAlwaysSet | xpc::kFlagInitHandshake, 0, nullptr));
        append();
        return out;
    }
};

int offline() {
    Candidate c;
    const auto wire = c.bootstrap();
    require(wire.size() > 24 && std::memcmp(wire.data(), http2::kClientPreface, 24) == 0,
            "client preface");
    size_t offset = 24;
    int headers = 0;
    while (offset < wire.size()) {
        http2::Frame f;
        size_t used = 0;
        std::string err;
        require(http2::parse_frame(std::span(wire).subspan(offset), f, used, err) ==
                    http2::Status::Ok,
                "generated frame parses");
        if (f.type == http2::kHeaders) {
            require(f.payload.empty() && f.flags == http2::kFlagEndHeaders,
                    "empty persistent HEADERS");
            ++headers;
        }
        offset += used;
    }
    require(headers == 2, "two odd control streams");
    c.feed(http2::settings_frame({}));
    c.feed(http2::headers_frame(1));
    c.feed(http2::headers_frame(3));
    c.feed(http2::data_frame(1, std::vector<uint8_t>{1, 2}));
    c.feed(http2::data_frame(3, std::vector<uint8_t>{3, 4}));
    require(c.inbound[1].size() == 2 && c.inbound[3].size() == 2 && c.invalid_frames == 0,
            "bidirectional DATA on odd streams");
    c.drain();
    const size_t baseline = c.sent_data;
    c.data(1, std::vector<uint8_t>(150000, 0x42));
    c.drain();
    const size_t blocked = c.sent_data - baseline;
    require(blocked > 0 && blocked < 150000, "DATA obeys peer default window");
    c.feed(http2::window_update_frame(0, 200000));
    c.feed(http2::window_update_frame(1, 200000));
    c.drain();
    require(c.sent_data - baseline == 150000, "WINDOW_UPDATE resumes pending DATA");
    // 接收超过初始窗口的数据，确认库会持续补充连接及流窗口。
    const auto incoming = http2::data_frame(3, std::vector<uint8_t>(16384, 0x43));
    constexpr size_t received_size = 20u << 20;
    for (size_t n = 0; n < received_size / 16384; ++n) {
        c.feed(incoming);
        c.drain();
    }
    require(c.inbound[3].size() == received_size + 2 &&
                c.sent_frames[{NGHTTP2_WINDOW_UPDATE, 3}] > 0 &&
                c.sent_frames[{NGHTTP2_WINDOW_UPDATE, 0}] > 1,
            "large receive replenishes connection and stream windows");
    std::printf("PASS: empty HEADERS, odd-stream bidirectional DATA, SETTINGS and flow control\n");
    Candidate file;
    file.bootstrap();
    file.feed(http2::settings_frame({}));
    const int next = file.next_even();
    const int queued = file.open_even();
    file.drain();
    require(next < 0 && (queued < 0 || file.unsent_frames > 0),
            "client cannot originate even stream HEADERS");
    Candidate peer;
    peer.bootstrap();
    peer.feed(http2::settings_frame({}));
    peer.feed(http2::headers_frame(2));
    peer.feed(http2::data_frame(2, std::vector<uint8_t>{0x89, 0x50}));
    peer.drain();
    require(peer.invalid_frames > 0 && peer.inbound[2].empty(),
            "unpromised server even stream is rejected");
    std::printf("PARTIAL: RemoteXPC even file-stream convention is incompatible with stock client "
                "session\n");
    return 0;
}

int replay(const std::string &path) {
    require(std::filesystem::file_size(path) <= (64u << 20), "replay exceeds 64 MiB");
    std::ifstream input(path, std::ios::binary);
    require(input.is_open(), "cannot open replay input");
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    size_t offset = 0, odd_bytes = 0, even_bytes = 0;
    while (offset < bytes.size()) {
        http2::Frame frame;
        size_t consumed = 0;
        std::string err;
        require(http2::parse_frame(std::span(bytes).subspan(offset), frame, consumed, err) ==
                    http2::Status::Ok,
                "baseline capture contains invalid or incomplete frame");
        if (frame.type == http2::kData) {
            std::span<const uint8_t> body;
            require(http2::data_payload(frame, body, err), "invalid baseline DATA padding");
            (frame.stream_id % 2 ? odd_bytes : even_bytes) += body.size();
        }
        offset += consumed;
    }
    std::printf("baseline DATA: odd=%zu even=%zu bytes\n", odd_bytes, even_bytes);
    for (const size_t chunk : {size_t{1}, size_t{17}, size_t{65536}}) {
        Candidate c;
        c.bootstrap();
        for (size_t offset = 0; offset < bytes.size(); offset += chunk) {
            c.feed(std::span(bytes).subspan(offset, std::min(chunk, bytes.size() - offset)));
            c.drain();
        }
        size_t odd_received = 0, even_received = 0;
        for (const auto &[id, data] : c.inbound)
            (id % 2 ? odd_received : even_received) += data.size();
        std::printf("chunk=%zu nghttp2 DATA: odd=%zu even=%zu invalid=%d\n", chunk, odd_received,
                    even_received, c.invalid_frames);
        if (even_bytes)
            require(c.invalid_frames > 0 && even_received != even_bytes,
                    "file-stream behavior changed; reassess adapter");
        else
            require(c.invalid_frames == 0 && odd_received == odd_bytes,
                    "control-stream replay mismatch");
    }
    std::printf("PASS: replay byte accounting; full compatibility requires separate file-stream "
                "assessment\n");
    return 0;
}

bool pump(Candidate &c, net::TcpStream &socket, std::string &err, int timeout_ms = 250) {
    const auto outgoing = c.drain();
    if (!outgoing.empty() &&
        !socket.send(
            std::string_view(reinterpret_cast<const char *>(outgoing.data()), outgoing.size()),
            err))
        return false;
    std::vector<uint8_t> input;
    bool timeout = false;
    if (!socket.recv(input, timeout_ms, err, &timeout))
        return timeout;
    c.feed(input);
    return true;
}

int live(const std::string &address, bool screenshot) {
    std::string err;
    auto device = app::open_device("", address, err);
    require(device.has_value(), err.c_str());
    const auto service =
        device->rsd().service(screenshot ? "com.apple.coredevice.screencaptureservice"
                                         : "com.apple.coredevice.displayservice");
    require(service && service->uses_remote_xpc && !service->encrypt_socket_data,
            "unencrypted RemoteXPC service available");
    net::TcpStream socket(*device->stack());
    if (!socket.connect(service->port, err))
        throw std::runtime_error(err);
    Candidate c;
    const auto initial = c.bootstrap();
    if (!socket.send(
            std::string_view(reinterpret_cast<const char *>(initial.data()), initial.size()), err))
        throw std::runtime_error(err);
    const auto settings_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!c.settings && std::chrono::steady_clock::now() < settings_deadline)
        if (!pump(c, socket, err))
            throw std::runtime_error(err);
    require(c.settings, "device SETTINGS timeout");
    auto input = xpc::make_dict();
    if (screenshot) {
        xpc::dict_set(input, "displayUniqueID", xpc::make_null());
        xpc::dict_set(input, "requestedFormat", xpc::make_string("png"));
    }
    auto request = remote::core_device_request(
        screenshot ? "com.apple.coredevice.feature.capturescreenshot"
                   : "com.apple.coredevice.feature.getmediasupportinfo",
        screenshot ? "com.apple.coredevice.action.capturescreenshot"
                   : "com.apple.coredevice.action.mediastreamgetsupportinfo",
        input);
    c.data(1,
           xpc::encode_message(xpc::kFlagAlwaysSet | xpc::kFlagDataPresent | xpc::kFlagWantingReply,
                               1, &request));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!pump(c, socket, err))
            throw std::runtime_error(err);
        for (int id : {1, 3}) {
            auto &buffer = c.inbound[id];
            while (!buffer.empty()) {
                xpc::Message message;
                size_t consumed = 0;
                const auto status = xpc::decode_message(buffer, message, consumed, err);
                require(status != xpc::Status::Malformed, "invalid XPC reply");
                if (status == xpc::Status::NeedMore)
                    break;
                buffer.erase(buffer.begin(), buffer.begin() + consumed);
                if (!message.has_body)
                    continue;
                const auto *output = message.body.find("CoreDevice.output");
                require(!message.body.find("CoreDevice.error"), "device rejected feature call");
                if (!output)
                    continue;
                if (!screenshot) {
                    std::printf("PASS: nghttp2 live empty-HEADERS handshake and feature call\n");
                    return 0;
                }
                const auto *image = output->find("image");
                require(image, "screenshot reply contains image");
                std::printf("image type=0x%x inline bytes=%zu\n",
                            static_cast<unsigned>(image->type), image->data.size());
                if (image->type == xpc::Type::Data) {
                    require(image->data.size() >= 8 &&
                                std::memcmp(image->data.data(), "\x89PNG\r\n\x1a\n", 8) == 0,
                            "inline screenshot PNG signature");
                    std::printf("PASS: large inline screenshot on odd control stream; no even file "
                                "stream exercised\n");
                    return 0;
                }
                require(image->type == xpc::Type::FileTransfer,
                        "screenshot image is data or file transfer");
                std::printf("PASS: live screenshot descriptor, file bytes=%llu\n",
                            static_cast<unsigned long long>(image->file_size));
                const int queued = c.open_even();
                c.drain();
                require(queued < 0 || c.unsent_frames > 0,
                        "unexpected even-stream support; reassess adapter");
                std::printf(
                    "BLOCKED: library cannot send RemoteXPC file acceptance HEADERS on stream 2\n");
                return 2;
            }
        }
    }
    throw std::runtime_error("feature reply timeout");
}
} // namespace

int main(int argc, char **argv) {
    std::printf("nghttp2 %s; probe only, production transport unchanged\n",
                nghttp2_version(0)->version_str);
    try {
        if (argc == 1)
            return offline();
        if (argc == 3 && std::string(argv[1]) == "--replay")
            return replay(argv[2]);
        if (argc == 3 && std::string(argv[1]) == "--wifi")
            return live(argv[2], false);
        if (argc == 4 && std::string(argv[1]) == "--wifi" && std::string(argv[3]) == "--screenshot")
            return live(argv[2], true);
        std::fprintf(stderr,
                     "usage: nghttp2_probe [--wifi ADDRESS [--screenshot] | --replay FILE]\n");
        return 1;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "probe failed: %s\n", e.what());
        return 1;
    }
}
