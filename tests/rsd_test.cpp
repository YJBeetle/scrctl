#include "MemoryTunnel.h"
#include "i18n/Translation.h"
#include "remote/Rsd.h"

#include <atomic>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

int checks = 0;
int failures = 0;

void check(bool ok, const std::string &message) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
    }
}

using scrctl::xpc::Value;
using namespace scrctl::xpc;

Value service_entry(const Value *port, bool metadata = true) {
    auto entry = make_dict();
    if (port != nullptr) dict_set(entry, "Port", *port);
    if (metadata) {
        dict_set(entry, "Entitlement", make_string("test.entitlement"));
        dict_set(entry, "UnknownMetadata", make_data({4, 5, 6}));
        auto properties = make_dict();
        dict_set(properties, "UsesRemoteXPC", make_bool(true));
        dict_set(properties, "EncryptSocketData", make_bool(true));
        auto features = make_array();
        array_push(features, make_string("feature.alpha"));
        array_push(features, make_string("feature.beta"));
        // 保留现有 as_string_or 语义：非字符串 feature 仍产生空字符串，不在端口修复中调整。
        array_push(features, make_uint64(7));
        dict_set(properties, "Features", std::move(features));
        dict_set(entry, "Properties", std::move(properties));
    }
    return entry;
}

void directory_ports_and_metadata() {
    struct Case {
        const char *name;
        Value value;
        uint16_t expected;
    };
    const std::vector<Case> cases = {
        {"string-zero", make_string("0"), 0},
        {"string-one", make_string("1"), 1},
        {"string-leading-zeroes", make_string("0049152"), 49152},
        {"string-max", make_string("65535"), 65535},
        {"string-over", make_string("65536"), 0},
        {"string-over-wrapped", make_string("65537"), 0},
        {"string-overflow", make_string("18446744073709551616"), 0},
        {"string-negative", make_string("-1"), 0},
        {"string-plus", make_string("+123"), 0},
        {"string-tail", make_string("123x"), 0},
        {"string-decimal", make_string("123.0"), 0},
        {"string-hex", make_string("0x123"), 0},
        {"string-leading-space", make_string(" 123"), 0},
        {"string-trailing-space", make_string("123 "), 0},
        {"string-empty", make_string(""), 0},
        {"string-nul", make_string(std::string("123\0x", 5)), 0},
        {"int-zero", make_int64(0), 0},
        {"int-one", make_int64(1), 1},
        {"int-max", make_int64(65535), 65535},
        {"int-negative", make_int64(-1), 0},
        {"int-min", make_int64(std::numeric_limits<int64_t>::min()), 0},
        {"int-over", make_int64(65536), 0},
        {"int-over-wrapped", make_int64(65537), 0},
        {"int-large", make_int64(std::numeric_limits<int64_t>::max()), 0},
        {"uint-zero", make_uint64(0), 0},
        {"uint-one", make_uint64(1), 1},
        {"uint-max", make_uint64(65535), 65535},
        {"uint-over", make_uint64(65536), 0},
        {"uint-over-wrapped", make_uint64(65537), 0},
        {"uint-large", make_uint64(std::numeric_limits<uint64_t>::max()), 0},
        {"bool-true", make_bool(true), 0},
        {"bool-false", make_bool(false), 0},
        {"null", make_null(), 0},
        {"double", make_double(123), 0},
        {"date", make_date(123), 0},
        {"data", make_data({1, 2}), 0},
        {"dict", make_dict(), 0},
        {"array", make_array(), 0},
    };
    auto services = make_dict();
    dict_set(services, "missing-port", service_entry(nullptr));
    dict_set(services, "skip-string", make_string("not a service dictionary"));
    for (const auto &item : cases) {
        dict_set(services, item.name, service_entry(&item.value));
    }
    dict_set(services, "skip-null", make_null());
    const auto zero = make_uint64(0);
    auto without_properties = service_entry(&zero, false);
    dict_set(without_properties, "Properties", make_bool(true));
    dict_set(services, "default-properties", std::move(without_properties));
    const auto original = encode(services);

    const auto parsed = scrctl::remote::parse_service_directory(services);
    check(parsed.size() == cases.size() + 2, "only non-dictionary entries are skipped");
    check(encode(services) == original, "parsing preserves the original directory and its unknown fields");
    if (parsed.size() != cases.size() + 2) return;
    check(parsed.front().name == "missing-port" && parsed.front().port == 0,
          "service with missing port remains first in the directory");
    for (std::size_t i = 0; i <= cases.size(); ++i) {
        const auto &item = parsed[i];
        if (i != 0) {
            check(item.name == cases[i - 1].name, "service order: " + item.name);
            check(item.port == cases[i - 1].expected, "port boundary/type: " + item.name);
        }
        check(item.entitlement == "test.entitlement" && item.uses_remote_xpc &&
                  item.encrypt_socket_data &&
                  item.features == std::vector<std::string>{"feature.alpha", "feature.beta", ""},
              "metadata retained regardless of port validity: " + item.name);
    }
    const auto &last = parsed.back();
    check(last.name == "default-properties" && last.port == 0 && last.entitlement.empty() &&
              !last.uses_remote_xpc && !last.encrypt_socket_data && last.features.empty(),
          "absent metadata and non-dictionary Properties retain previous defaults");
    check(scrctl::remote::parse_service_directory(make_dict()).empty(), "empty directory");
    check(scrctl::remote::parse_service_directory(make_array()).empty(), "non-dictionary directory");
}

void zero_port_connection() {
    MemoryTunnel tunnel;
    std::atomic<unsigned> packets{0};
    tunnel.deliver = [&](std::vector<uint8_t>) { ++packets; };
    scrctl::net::Stack stack(tunnel, "fd00:9::1", "fd00:9::2");
    std::string err;
    const bool started = stack.start_pump(err);
    check(started, "memory packet stack starts: " + err);
    if (!started) return;

    // 使用真实公开连接入口，覆盖纯 TCP 与 RemoteXPC 服务；不建立任何真实网络连接。
    for (bool remote_xpc : {false, true}) {
        scrctl::remote::ServiceInfo info;
        info.name = remote_xpc ? "test.remote-xpc-zero-port" : "test.tcp-zero-port";
        info.uses_remote_xpc = remote_xpc;
        const auto conn = scrctl::remote::ServiceConnection::open(stack, info, err);
        check(conn == nullptr, "zero port connection is rejected");
        check(err.find(info.name) != std::string::npos &&
                  err.find("no valid TCP port") != std::string::npos &&
                  err.find("1..65535") != std::string::npos &&
                  err.find("lwIP") == std::string::npos,
              "failure names the service and expected range before TCP setup: " + err);
    }
    stack.stop_pump();
    check(packets.load() == 0, "zero port rejection sends no packets");
}

}  // namespace

int main() {
    scrctl::i18n::initialize("en");
    directory_ports_and_metadata();
    zero_port_connection();
    std::printf("%d RSD parser/connection checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
