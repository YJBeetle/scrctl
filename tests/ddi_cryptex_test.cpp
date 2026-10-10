#include "ddi/Cryptex.h"
#include "http2/Framing.h"
#include "remote/RemoteXpc.h"
#include <cstdio>
#include <deque>
#include <sstream>

namespace {
using namespace scrctl;
int failures = 0;
void check(bool pass, const char *message) {
    std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", message);
    if (!pass) ++failures;
}
class Stream final : public net::ByteStream {
public:
    std::deque<std::vector<uint8_t>> incoming;
    std::vector<http2::Frame> sent;
    uint32_t grant_stream = 0;
    int64_t receive_window = http2::kDefaultInitialWindowSize;
    bool send(std::string_view data, std::string &error) override {
        if (data == http2::kClientPreface) return true;
        auto bytes = std::span(reinterpret_cast<const uint8_t *>(data.data()), data.size());
        while (!bytes.empty()) {
            http2::Frame frame; std::size_t n = 0;
            if (http2::parse_frame(bytes, frame, n, error) != http2::Status::Ok) return false;
            if (frame.type == http2::kData) {
                if (static_cast<int64_t>(frame.payload.size()) > receive_window) {
                    error = "sender exceeded server connection window, including handshake DATA";
                    return false;
                }
                receive_window -= static_cast<int64_t>(frame.payload.size());
            }
            sent.push_back(frame); bytes = bytes.subspan(n);
        }
        return true;
    }
    bool recv(std::vector<uint8_t> &out, int, std::string &error, bool *timeout) override {
        out.clear(); error.clear();
        if (incoming.empty() && grant_stream) {
            receive_window += 100000;
            incoming.push_back(http2::window_update_frame(0, 100000));
            incoming.push_back(http2::window_update_frame(grant_stream, 100000));
        }
        if (timeout) *timeout = incoming.empty();
        if (incoming.empty()) return false;
        out = std::move(incoming.front()); incoming.pop_front(); return true;
    }
};
void transfers() {
    Stream stream; stream.incoming.push_back(http2::settings_frame({}));
    std::string error;
    auto channel = remote::Channel::open(stream, error);
    check(channel.has_value(), "mock RemoteXPC handshake succeeds");
    if (!channel) return;
    stream.sent.clear(); stream.grant_stream = 5;
    std::string payload(150000, 'x'); std::istringstream input(payload);
    check(channel->send_file_transfer(7, payload.size(), input, 1000, {}, {}, error),
          "large attachment pushes immediately and replenishes outbound windows");
    std::vector<uint8_t> data; bool headers = false, ended = false; xpc::Message preamble;
    for (const auto &f : stream.sent) {
        if (f.stream_id != 5) continue;
        headers |= f.type == http2::kHeaders;
        if (f.type != http2::kData) continue;
        ended |= (f.flags & http2::kFlagEndStream) != 0;
        data.insert(data.end(), f.payload.begin(), f.payload.end());
    }
    std::size_t n = 0;
    check(headers && ended && xpc::decode_message(data, preamble, n, error) == xpc::Status::Ok &&
          !preamble.has_body && preamble.message_id == 7 &&
          preamble.flags == (xpc::kFlagAlwaysSet | xpc::kFlagFileTxRequest) &&
          std::string(data.begin() + n, data.end()) == payload,
          "odd stream preamble correlates transfer id and delivers exact raw bytes with END_STREAM");
    std::istringstream empty;
    check(!channel->send_file_transfer(7, 0, empty, 1000, {}, {}, error), "duplicate attachment id rejected");
    stream.grant_stream = 7;
    std::istringstream next("hi");
    check(channel->send_file_transfer(8, 2, next, 1000, {}, {}, error), "next attachment uses a fresh odd stream");
    // The daemon may reset a completed attachment after acknowledging it. This
    // must not invalidate the root-stream installation reply.
    const auto ack = xpc::encode_message(xpc::kFlagAlwaysSet | xpc::kFlagFileTxResponse, 7, nullptr);
    stream.incoming.push_back(http2::data_frame(5, std::span(ack.data(), 10)));
    stream.incoming.push_back(http2::data_frame(5, std::span(ack.data() + 10, ack.size() - 10), http2::kFlagEndStream));
    stream.incoming.push_back(http2::serialize(http2::kRstStream, 0, 5, std::array<uint8_t,4>{0,0,0,5}));
    auto reply = xpc::make_dict(); xpc::dict_set(reply, "error", xpc::make_int64(0));
    stream.incoming.push_back(http2::data_frame(1, xpc::encode_message(xpc::kFlagDataPresent | xpc::kFlagAlwaysSet, 1, &reply)));
    xpc::Value result;
    check(channel->receive(result, 1000, error) && result.find("error"), "fragmented upload acknowledgement and completed reset do not hide installation reply");
    std::istringstream too_short("x");
    check(!channel->send_file_transfer(9, 2, too_short, 1000, {}, {}, error), "short upload file fails without retrying");
    check(!channel->send_request(reply, true, error), "partial attachment makes channel unusable");
}
void messages() {
    std::string error;
    std::vector<uint8_t> nonce(56, 7); nonce[52] = 48; nonce[53] = nonce[54] = nonce[55] = 0;
    auto decoded = ddi::detail::unwrap_cryptex_nonce(nonce, error);
    check(decoded && decoded->size() == 48 && (*decoded)[0] == 7, "Cryptex nonce unwrap uses handle structure with exact length");
    nonce[52] = 250;
    check(!ddi::detail::unwrap_cryptex_nonce(nonce, error), "malformed nonce length rejected before TSS");
    auto reply = xpc::make_dict(), argv = xpc::make_dict(), out = xpc::make_dict();
    xpc::dict_set(reply, "argv", argv);
    check(ddi::detail::unwrap_cryptex_reply(reply, out, error), "read-personalization-id may omit error field");
    xpc::dict_set(reply, "error", xpc::make_uint64(70));
    check(!ddi::detail::unwrap_cryptex_reply(reply, out, error), "device errno rejected");
    xpc::dict_set(reply, "error", xpc::make_int64(0)); xpc::dict_set(reply, "cferr", xpc::make_dict());
    check(!ddi::detail::unwrap_cryptex_reply(reply, out, error), "cferr wins over apparent success");
    ddi::detail::CryptexMetadata metadata; metadata.properties = xpc::make_dict();
    auto request = ddi::detail::cryptex_install_request(metadata, {10,20,30,40,50});
    const auto &a = request.at("argv");
    check(a.at("image-type-index").type == xpc::Type::Int64 && a.at("image-type-index").int64 == 10 &&
          a.at("persistence").type == xpc::Type::UInt64 && a.at("persistence").uint64 == 2 &&
          a.at("nonce-persistence").uint64 == 1 && a.at("client-version").uint64 == 3 &&
          a.at("image").transfer_id == 1 && a.at("volumehash").transfer_id == 5 &&
          a.at("im4m").file_size == 30, "install request preserves Apple integer types, persistence and transfer order");
    auto identity = plist::Value::Dict(), info = plist::Value::Dict(), manifest = plist::Value::Dict();
    info.set("Variant", plist::Value::Str("iOS Customer Developer Disk Image Cryptex")); identity.set("Info", info);
    identity.set("Cryptex1,UseProductClass", plist::Value::Bool(true));
    identity.set("Cryptex1,SubType", plist::Value::Int(2)); identity.set("Cryptex1,NonceDomain", plist::Value::Int(4));
    identity.set("Cryptex1,Version", plist::Value::Str("39.999.999.0.0,0"));
    identity.set("Cryptex1,PreauthorizationVersion", plist::Value::Str("39.999.999.0.0,0"));
    auto identities = plist::Value::Array(); identities.push(identity); manifest.set("BuildIdentities", identities);
    auto cinfo = plist::Value::Dict(); cinfo.set("CFBundleIdentifier", plist::Value::Str("com.apple.MobileAsset.DDI"));
    cinfo.set("CFBundleVersion", plist::Value::Str("27.1.5228.8")); cinfo.set("RequiredMountPath", plist::Value::Str("/System/Developer"));
    auto parsed = ddi::detail::parse_cryptex_metadata(manifest, cinfo, error);
    check(parsed && parsed->properties.at("Cryptex1,NonceDomain").type == xpc::Type::UInt64 &&
          parsed->nonce_domain == 4 && parsed->version == "27.1.5228.8", "generic identity properties and installed version come from pinned manifest/assets");
    cinfo.set("RequiredMountPath", plist::Value::Str("/other"));
    check(!ddi::detail::parse_cryptex_metadata(manifest, cinfo, error), "unrelated mount path rejected");
}
}
int main() { transfers(); messages(); return failures ? 1 : 0; }
