#include "wifi/Mdns.h"
#ifndef _WIN32
#include <arpa/inet.h>
#endif
#include <mdns.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {
using namespace std::chrono_literals;
using scrctl::wifi::mdns::detail::RecordCache;

constexpr const char *service = "_remotepairing._tcp.local.";
constexpr const char *instance = "Unit._remotepairing._tcp.local.";
constexpr const char *target = "Unit-Host.local.";
int checks = 0, failures = 0;

void check(bool value, const char *label) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL: %s\n", label); }
}

mdns_string_t text(std::string_view str) { return {str.data(), str.size()}; }

mdns_record_t ptr(std::string_view value = instance, uint32_t ttl = 120) {
    mdns_record_t record{};
    record.name = text(service);
    record.type = MDNS_RECORDTYPE_PTR;
    record.data.ptr.name = text(value);
    record.rclass = MDNS_CLASS_IN;
    record.ttl = ttl;
    return record;
}

mdns_record_t srv(std::string_view owner = instance, std::string_view host = target, uint16_t port = 55555) {
    mdns_record_t record{};
    record.name = text(owner);
    record.type = MDNS_RECORDTYPE_SRV;
    record.data.srv.name = text(host);
    record.data.srv.port = port;
    record.rclass = MDNS_CLASS_IN | MDNS_CACHE_FLUSH;
    record.ttl = 120;
    return record;
}

mdns_record_t address(std::string_view ip, std::string_view host = target, uint32_t ttl = 120) {
    mdns_record_t record{};
    record.name = text(host);
    record.type = ip.find(':') == std::string_view::npos ? MDNS_RECORDTYPE_A : MDNS_RECORDTYPE_AAAA;
    const std::string input(ip);
    if (record.type == MDNS_RECORDTYPE_A) inet_pton(AF_INET, input.c_str(), &record.data.a.addr.sin_addr);
    else inet_pton(AF_INET6, input.c_str(), &record.data.aaaa.addr.sin6_addr);
    record.rclass = MDNS_CLASS_IN | MDNS_CACHE_FLUSH;
    record.ttl = ttl;
    return record;
}

mdns_record_t txt(std::string_view key, std::string_view value) {
    mdns_record_t record{};
    record.name = text(instance);
    record.type = MDNS_RECORDTYPE_TXT;
    record.data.txt.key = text(key);
    record.data.txt.value = text(value);
    record.rclass = MDNS_CLASS_IN;
    record.ttl = 120;
    return record;
}

// 测试报文也由依赖库编码，覆盖它实际产生的名称压缩与合并 TXT 字段。
std::vector<uint8_t> packet(const std::vector<mdns_record_t> &records) {
    alignas(uint32_t) std::array<uint8_t, 16384> buffer{};
    mdns_header_t header{};
    header.flags = htons(0x8400);
    header.answer_rrs = htons(mdns_answer_get_record_count(records.data(), records.size()));
    std::memcpy(buffer.data(), &header, sizeof(header));
    mdns_string_table_t table{};
    void *end = buffer.data() + sizeof(header);
    for (const auto &record : records) {
        end = mdns_answer_add_record(buffer.data(), buffer.size(), end, record, &table);
        if (!end) throw std::runtime_error("mDNS fixture is too large");
    }
    end = mdns_answer_add_txt_record(buffer.data(), buffer.size(), end, records.data(), records.size(),
                                    MDNS_CLASS_IN, 120, &table);
    if (!end) throw std::runtime_error("mDNS TXT fixture is too large");
    return {buffer.data(), static_cast<uint8_t *>(end)};
}

bool has_query(const std::vector<std::pair<uint16_t, std::string>> &queries, uint16_t type) {
    return std::any_of(queries.begin(), queries.end(), [&](const auto &query) { return query.first == type; });
}

size_t record_offset(const std::vector<uint8_t> &wire, uint16_t wanted) {
    struct Search { uint16_t type; size_t result = 0; } search{wanted};
    size_t offset = 12;
    mdns_records_parse(-1, nullptr, 0, wire.data(), wire.size(), &offset, MDNS_ENTRYTYPE_ANSWER, 0,
                       mdns_ntohs(wire.data() + 6),
                       [](int, const sockaddr *, size_t, mdns_entry_type_t, uint16_t, uint16_t type,
                          uint16_t, uint32_t, const void *, size_t, size_t, size_t, size_t start,
                          size_t, void *user) -> int {
                           auto &search = *static_cast<Search *>(user);
                           if (type == search.type) { search.result = start; return 1; }
                           return 0;
                       }, &search);
    return search.result;
}

void records_and_candidates() {
    const auto now = RecordCache::Clock::now();
    RecordCache cache;
    check(cache.add_packet(7, "en7", packet({ptr()}), now), "PTR accepted");
    auto queries = cache.queries(7, now);
    check(has_query(queries, MDNS_RECORDTYPE_SRV) && has_query(queries, MDNS_RECORDTYPE_TXT), "PTR schedules SRV and TXT");
    check(cache.queries(8, now).empty(), "follow-up queries stay on the receiving interface");
    check(cache.add_packet(7, "en7", packet({srv()}), now), "separate SRV accepted");
    queries = cache.queries(7, now);
    check(has_query(queries, MDNS_RECORDTYPE_A) && has_query(queries, MDNS_RECORDTYPE_AAAA), "SRV schedules both address families");
    const std::string raw_auth("\x00\xfe", 2);
    check(cache.add_packet(7, "en7", packet({txt("identifier", "opaque-uuid"), txt("authTag", raw_auth), txt("name", "Test Phone")}), now), "binary TXT accepted");
    check(cache.add_packet(7, "en7", packet({address("192.168.1.7")}), now), "IPv4 accepted");
    queries = cache.queries(7, now);
    check(!has_query(queries, MDNS_RECORDTYPE_A) && has_query(queries, MDNS_RECORDTYPE_AAAA), "IPv4 does not suppress missing IPv6");
    check(cache.add_packet(7, "en7", packet({address("fe80::7"), address("fd00::7"), address("192.168.1.8")}), now), "multiple addresses accepted");
    auto found = cache.snapshot(now);
    check(found.size() == 1, "one advertisement from separate packets");
    if (found.size() != 1) return;
    check(found[0].identifier == "opaque-uuid" && found[0].auth_tag == raw_auth && found[0].name == "Test Phone", "TXT values preserved and name extracted");
    check(found[0].endpoints.size() == 4, "all connection candidates retained");
    check(std::all_of(found[0].endpoints.begin(), found[0].endpoints.end(), [](const auto &endpoint) {
        return endpoint.port == 55555 && endpoint.interface_index == 7 && endpoint.interface_name == "en7";
    }), "real SRV port and interface retained");
    check(std::any_of(found[0].endpoints.begin(), found[0].endpoints.end(), [](const auto &endpoint) {
        return endpoint.address == "fe80::7%7";
    }), "link-local scope retained");
    check(cache.queries(7, now).empty(), "complete advertisement requires no follow-up");
    check(cache.add_packet(7, "en7", packet({address("192.168.1.7")}), now), "duplicate address accepted");
    check(cache.snapshot(now)[0].endpoints.size() == 4, "duplicate address removed");

    // 相同实例在第二接口公布不同地址：绝不能给第一接口的 link-local 填第二接口 scope。
    check(cache.add_packet(12, "en12", packet({ptr(), srv(), address("fe80::12"), txt("identifier", "opaque-uuid"), txt("authTag", raw_auth), txt("name", "Test Phone")}), now), "second interface accepted");
    found = cache.snapshot(now);
    check(found.size() == 1 && found[0].endpoints.size() == 5, "identical TXT advertisements merge all interfaces");
    check(std::any_of(found[0].endpoints.begin(), found[0].endpoints.end(), [](const auto &endpoint) {
        return endpoint.address == "fe80::12%12" && endpoint.interface_index == 12;
    }), "second interface scope preserved");
}

void interface_isolation() {
    const auto now = RecordCache::Clock::now();
    RecordCache cache;
    cache.add_packet(3, "en3", packet({ptr(), srv()}), now);
    cache.add_packet(4, "en4", packet({address("192.168.4.1")}), now);
    auto found = cache.snapshot(now);
    check(found.size() == 1 && found[0].endpoints.empty(), "address from another interface is not joined");
    cache.add_packet(4, "en4", packet({ptr(), srv(), txt("identifier", "other")}), now);
    found = cache.snapshot(now);
    check(found.size() == 2, "same instance with different TXT is not authenticated or merged");
    check(std::count_if(found.begin(), found.end(), [](const auto &ad) { return ad.endpoints.empty(); }) == 1,
          "isolated advertisements retain only their own address");

    RecordCache orphan;
    orphan.add_packet(3, "en3", packet({srv(), address("192.168.3.1")}), now);
    check(orphan.snapshot(now).empty(), "SRV without service PTR is not listed");
    orphan.add_packet(3, "en3", packet({ptr("UNIT._REMOTEPAIRING._TCP.LOCAL.")}), now);
    check(orphan.snapshot(now).size() == 1 && orphan.snapshot(now)[0].endpoints.size() == 1,
          "DNS names are case insensitive and records may precede PTR");
}

void separate_family_indices() {
    const auto now = RecordCache::Clock::now();
    RecordCache cache;
    cache.add_packet(5, "adapter", packet({ptr(), srv(), address("192.168.5.1")}), now, 42);
    // Windows 同一网卡的 IPv6 socket 也传相同的关联索引，实际 IPv6 索引单独保留。
    cache.add_packet(5, "adapter", packet({address("fe80::42"), address("fd00::42")}), now, 42);
    const auto found = cache.snapshot(now);
    check(found.size() == 1 && found[0].endpoints.size() == 3, "IPv4 and IPv6 records join when family indices differ");
    if (found.size() != 1) return;
    check(std::any_of(found[0].endpoints.begin(), found[0].endpoints.end(), [](const auto &endpoint) {
        return endpoint.address == "192.168.5.1" && endpoint.interface_index == 5;
    }), "IPv4 candidate keeps IPv4 IfIndex");
    check(std::any_of(found[0].endpoints.begin(), found[0].endpoints.end(), [](const auto &endpoint) {
        return endpoint.address == "fe80::42%42" && endpoint.interface_index == 42;
    }), "IPv6 link-local candidate keeps IPv6 IfIndex and scope");
    check(std::any_of(found[0].endpoints.begin(), found[0].endpoints.end(), [](const auto &endpoint) {
        return endpoint.address == "fd00::42" && endpoint.interface_index == 42;
    }), "global IPv6 candidate also keeps IPv6 IfIndex");
    RecordCache ipv4_only;
    ipv4_only.add_packet(5, "adapter", packet({ptr(), srv(), address("192.168.5.1"), address("fe80::42"), address("fd00::42")}), now, 0);
    check(ipv4_only.snapshot(now).size() == 1 && ipv4_only.snapshot(now)[0].endpoints.size() == 1,
          "adapter without IPv6 index does not expose an unusable IPv6 candidate");
}

void ttl_and_goodbye() {
    const auto now = RecordCache::Clock::now();
    RecordCache cache;
    cache.add_packet(2, "en2", packet({ptr(), srv(), address("192.168.2.1", target, 1), address("fd00::2")}), now);
    check(cache.snapshot(now).size() == 1 && cache.snapshot(now)[0].endpoints.size() == 2, "TTL fixture starts with both candidates");
    check(cache.snapshot(now + 2s)[0].endpoints.size() == 1, "expired address removed from snapshot");
    cache.add_packet(2, "en2", packet({address("fd00::2", target, 0)}), now + 2s);
    check(cache.snapshot(now + 2s)[0].endpoints.empty(), "address goodbye removes just that candidate");
    cache.add_packet(2, "en2", packet({ptr(instance, 0)}), now + 2s);
    check(cache.snapshot(now + 2s).empty(), "PTR goodbye removes advertisement");
    cache.add_packet(2, "en2", packet({ptr(instance, 1)}), now + 2s);
    check(cache.snapshot(now + 4s).empty(), "expired PTR is not listed");
}

void bounds_and_untrusted_input() {
    RecordCache cache;
    const auto valid = packet({ptr(), srv(), address("192.168.1.9")});
    check(std::any_of(valid.begin() + 12, valid.end(), [](uint8_t ch) { return ch == 0xc0; }), "fixture contains compressed DNS names");
    for (size_t size = 0; size < valid.size(); ++size) {
        RecordCache truncated;
        check(!truncated.add_packet(9, "en9", {valid.data(), size}) && truncated.snapshot().empty(), "truncated packet rejected before cache mutation");
    }
    auto malformed = valid;
    malformed[12] = 0xc0; malformed[13] = 12;
    check(!cache.add_packet(9, "en9", malformed) && cache.snapshot().empty(), "DNS pointer cycle rejected");
    malformed = valid;
    malformed[12] = 0xff; malformed[13] = 0xff;
    check(!cache.add_packet(9, "en9", malformed), "out-of-range DNS pointer rejected");
    malformed = valid;
    mdns_htons(malformed.data() + 6, 513);
    check(!cache.add_packet(9, "en9", malformed), "record count limit enforced");
    malformed = valid;
    mdns_htons(malformed.data() + 2, 0x8600);
    check(!cache.add_packet(9, "en9", malformed), "truncated flag rejected");
    malformed = valid;
    mdns_htons(malformed.data() + 2, 0x8403);
    check(!cache.add_packet(9, "en9", malformed), "DNS error rejected");
    check(!cache.add_packet(9, "en9", std::vector<uint8_t>(16385)), "datagram size limit enforced");
    malformed = valid;
    const auto srv_offset = record_offset(malformed, MDNS_RECORDTYPE_SRV);
    malformed[srv_offset + 6] = 0xff; malformed[srv_offset + 7] = 0xff;
    check(!cache.add_packet(9, "en9", malformed) && cache.snapshot().empty(), "invalid SRV target does not publish earlier PTR");
    malformed = packet({ptr(), txt("identifier", "value")});
    malformed[record_offset(malformed, MDNS_RECORDTYPE_TXT)] = 255;
    check(!cache.add_packet(9, "en9", malformed) && cache.snapshot().empty(), "invalid TXT field length rejects the whole packet");

    cache.add_packet(9, "en9", packet({ptr(), srv(), address("0.0.0.0"), address("127.0.0.1"),
                                     address("224.0.0.251"), address("::"), address("::1"), address("ff02::fb") }));
    check(cache.snapshot().size() == 1 && cache.snapshot()[0].endpoints.empty(), "unusable destination addresses ignored");
    cache.add_packet(9, "en9", packet({txt("identifier", "first"), txt("IDENTIFIER", "second")}));
    check(cache.snapshot()[0].identifier == "first", "duplicate TXT keys follow first-value RFC rule");

    RecordCache no_scope;
    no_scope.add_packet(0, "", packet({ptr(), srv(), address("fe80::1")}));
    check(no_scope.snapshot().size() == 1 && no_scope.snapshot()[0].endpoints.empty(), "unscoped link-local candidate is not returned");
}

void cache_limits() {
    RecordCache cache;
    for (size_t i = 0; i < 257; ++i) {
        const std::string name = "unit" + std::to_string(i) + "." + service;
        cache.add_packet(1, "en1", packet({ptr(name)}));
    }
    check(cache.snapshot().size() == 256 && !cache.warnings().empty(), "service count is bounded with warning");
    cache.add_packet(2, "en2", packet({ptr("another._remotepairing._tcp.local.")}));
    check(cache.snapshot().size() == 256, "service count limit applies across interfaces");
    RecordCache interfaces;
    for (uint32_t i = 1; i <= 65; ++i) interfaces.add_packet(i, "test", packet({address("192.168.1.1")}));
    check(!interfaces.warnings().empty(), "interface cache count is bounded with warning");
    RecordCache addresses;
    addresses.add_packet(1, "test", packet({ptr(), srv()}));
    for (size_t i = 1; i <= 33; ++i) addresses.add_packet(1, "test", packet({address("192.168.1." + std::to_string(i))}));
    check(addresses.snapshot()[0].endpoints.size() == 32 && !addresses.warnings().empty(), "per-host address count is bounded with warning");
}

void no_io_options() {
    using scrctl::wifi::mdns::browse;
    auto result = browse({0ms, {}});
    check(result.advertisements.empty() && !result.available && !result.cancelled && result.warnings.empty(), "zero timeout is an empty no-IO snapshot");
    result = browse({-1ms, {}});
    check(!result.available && !result.warnings.empty(), "negative timeout rejected without IO");
    result = browse({60001ms, {}});
    check(!result.available && !result.warnings.empty(), "oversized timeout rejected without IO");
    std::stop_source cancel;
    cancel.request_stop();
    const auto start = RecordCache::Clock::now();
    result = browse({60000ms, cancel.get_token()});
    check(result.cancelled && !result.available && result.advertisements.empty(), "pre-cancelled browse does not scan");
    check(RecordCache::Clock::now() - start < 100ms, "pre-cancelled browse returns within 100 ms");
}

} // namespace

int main() {
    try {
        records_and_candidates();
        interface_isolation();
        separate_family_indices();
        ttl_and_goodbye();
        bounds_and_untrusted_input();
        cache_limits();
        no_io_options();
    } catch (const std::exception &error) {
        std::printf("FAIL: fixture exception: %s\n", error.what());
        ++failures;
    }
    std::printf("mDNS: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
