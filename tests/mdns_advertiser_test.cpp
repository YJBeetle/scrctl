#include "wifi/Mdns.h"
#include "transport/SocketPlatform.h"
#include <mdns.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

namespace {
using scrctl::wifi::mdns::detail::HostRecords;
int checks = 0, failures = 0;
void check(bool ok, const char *message) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", message); }
}
constexpr std::string_view service = "_remotepairing-pairable-host._tcp.local.";
HostRecords records() {
    return {{"stable-id", "Test Computer", "TestModel", std::string("\0\xff", 2), 49152},
            "stable-id._remotepairing-pairable-host._tcp.local.", "stable-id.local.",
            {"192.0.2.7", "fd00::7"}};
}
std::vector<uint8_t> question(std::string_view name, uint16_t type, uint16_t cls = 1, uint16_t id = 0) {
    std::array<uint8_t, 1024> bytes{};
    mdns_header_t header{};
    header.query_id = htons(id); header.questions = htons(1);
    std::memcpy(bytes.data(), &header, sizeof(header));
    mdns_string_table_t table{};
    void *end = mdns_string_make(bytes.data(), bytes.size(), bytes.data() + sizeof(header),
                                 name.data(), name.size(), &table);
    end = mdns_htons(end, type); end = mdns_htons(end, cls);
    return {bytes.data(), static_cast<uint8_t *>(end)};
}
struct Parsed {
    std::map<uint16_t, int> types;
    std::map<std::string, std::string> txt;
    uint32_t ttl = 0;
    bool ttl_equal = true, first = true;
    uint16_t port = 0;
    std::string ptr, target;
};
Parsed parse(const std::vector<uint8_t> &bytes) {
    Parsed result;
    size_t offset = sizeof(mdns_header_t);
    const auto questions = mdns_ntohs(bytes.data() + 4);
    for (unsigned i = 0; i < questions; ++i) {
        mdns_string_skip(bytes.data(), bytes.size(), &offset); offset += 4;
    }
    mdns_records_parse(-1, nullptr, 0, bytes.data(), bytes.size(), &offset, MDNS_ENTRYTYPE_ANSWER,
        mdns_ntohs(bytes.data()), mdns_ntohs(bytes.data() + 6),
        [](int, const sockaddr *, size_t, mdns_entry_type_t, uint16_t, uint16_t type,
           uint16_t, uint32_t ttl, const void *data, size_t size, size_t, size_t,
           size_t start, size_t length, void *user) -> int {
            auto &p = *static_cast<Parsed *>(user); ++p.types[type];
            if (p.first) { p.ttl = ttl; p.first = false; } else p.ttl_equal &= p.ttl == ttl;
            std::array<char, 256> text{};
            if (type == MDNS_RECORDTYPE_PTR) {
                const auto value = mdns_record_parse_ptr(data, size, start, length, text.data(), text.size());
                p.ptr.assign(value.str, value.length);
            } else if (type == MDNS_RECORDTYPE_SRV) {
                const auto value = mdns_record_parse_srv(data, size, start, length, text.data(), text.size());
                p.port = value.port; p.target.assign(value.name.str, value.name.length);
            } else if (type == MDNS_RECORDTYPE_TXT) {
                std::array<mdns_record_txt_t, 16> fields{};
                const auto count = mdns_record_parse_txt(data, size, start, length, fields.data(), fields.size());
                for (size_t i = 0; i < count; ++i)
                    p.txt.emplace(std::string(fields[i].key.str, fields[i].key.length),
                                  std::string(fields[i].value.str, fields[i].value.length));
            }
            return 0;
        }, &result);
    return result;
}
}
int main() {
    auto input = records(); std::string error;
    auto packet = scrctl::wifi::mdns::detail::host_announcement(input, 120, error);
    check(!packet.empty() && error.empty(), "pairable host packet is generated offline");
    if (!packet.empty()) {
        const auto p = parse(packet);
        check(p.ttl == 120 && p.ttl_equal, "all announcement records have explicit TTL120");
        check(p.types == std::map<uint16_t, int>{{12, 1}, {33, 1}, {16, 1}, {1, 1}, {28, 1}},
              "PTR/SRV/coalesced TXT/IPv4/IPv6 records are present");
        check(p.ptr == input.service_instance && p.target == input.target && p.port == 49152,
              "service instance and real TCP port/target are retained");
        check(p.ptr == input.host.instance_identifier + "." + std::string(service) &&
              p.target == input.host.instance_identifier + ".local.",
              "wire DNS instance and SRV target share the TXT host identifier");
        check(p.txt == std::map<std::string, std::string>{{"identifier", "stable-id"},
              {"name", "Test Computer"}, {"model", "TestModel"}, {"authTag", std::string("\0\xff", 2)},
              {"flags", "1"}, {"ver", "26"}, {"minVer", "17"}}, "exact TXT keys and binary authTag are retained");
    }
    packet = scrctl::wifi::mdns::detail::host_announcement(input, 0, error);
    check(!packet.empty() && parse(packet).ttl == 0 && parse(packet).ttl_equal, "goodbye sets every record TTL0");
    for (const auto &[name, type] : std::array<std::pair<std::string_view, uint16_t>, 6>{
            std::pair{service, uint16_t{12}}, {input.service_instance, 33}, {input.service_instance, 16},
            {input.target, 1}, {input.target, 28}, {input.target, 255}}) {
        const auto reply = scrctl::wifi::mdns::detail::host_query_reply(input, question(name, type), false, error);
        check(reply.size() == 1 && !reply[0].unicast && !reply[0].packet.empty(), "matching QM query gets multicast response");
    }
    auto replies = scrctl::wifi::mdns::detail::host_query_reply(input, question(service, 12, 0x8001), false, error);
    check(replies.size() == 1 && replies[0].unicast, "QU query gets unicast response");
    replies = scrctl::wifi::mdns::detail::host_query_reply(input, question(service, 12, 1, 73), true, error);
    check(replies.size() == 1 && replies[0].unicast && mdns_ntohs(replies[0].packet.data()) == 73 &&
          mdns_ntohs(replies[0].packet.data() + 4) == 1 && parse(replies[0].packet).ttl == 10,
          "legacy response retains query ID/question and TTL10");
    check(scrctl::wifi::mdns::detail::host_query_reply(input, question("unrelated.local.", 255), false, error).empty(),
          "unrelated query gets no response");
    check(scrctl::wifi::mdns::detail::host_query_reply(input, packet, false, error).empty(),
          "received responses never trigger response loops");
    auto broken = question(service, 12); broken.pop_back();
    check(scrctl::wifi::mdns::detail::host_query_reply(input, broken, false, error).empty() && !error.empty(),
          "truncated question is rejected");
    input.host.advertise_ipv6 = false;
    packet = scrctl::wifi::mdns::detail::host_announcement(input, 120, error);
    check(!packet.empty() && parse(packet).types.count(28) == 0 && parse(packet).types.count(1) == 1,
          "failed IPv6 listener prevents AAAA publication");
    input.host.advertise_ipv6 = true; input.host.advertise_ipv4 = false;
    packet = scrctl::wifi::mdns::detail::host_announcement(input, 120, error);
    check(!packet.empty() && parse(packet).types.count(1) == 0 && parse(packet).types.count(28) == 1,
          "failed IPv4 listener prevents A publication");
    input.host.port = 0;
    check(scrctl::wifi::mdns::detail::host_announcement(input, 120, error).empty() && !error.empty(),
          "zero TCP port is rejected before any network operation");
    check(!scrctl::wifi::mdns::Advertiser::start(input.host, error), "invalid start is offline");
    input = records(); input.host.auth_tag.assign(248, 'x');
    check(scrctl::wifi::mdns::detail::host_announcement(input, 120, error).empty(), "oversized TXT field is rejected");
    input = records(); input.service_instance = "other-id." + std::string(service);
    check(scrctl::wifi::mdns::detail::host_announcement(input, 120, error).empty() && !error.empty(),
          "DNS instance cannot silently differ from TXT identifier");
    input = records(); input.target = "other-id.local.";
    check(scrctl::wifi::mdns::detail::host_announcement(input, 120, error).empty() && !error.empty(),
          "SRV target cannot silently differ from TXT identifier");
    for (const auto &label : std::array<std::string, 10>{"", std::string(64, 'a'), "has.dot", "has space",
            "-leading", "trailing-", "under_score", std::string("a\0b", 3), "\xc3\xa9", "a/b"}) {
        input = records(); input.host.instance_identifier = label;
        input.service_instance = label + "." + std::string(service); input.target = label + ".local.";
        check(scrctl::wifi::mdns::detail::host_announcement(input, 120, error).empty() && !error.empty(),
              "invalid single host label is rejected without network operations");
        check(!scrctl::wifi::mdns::Advertiser::start(input.host, error),
              "invalid host label is rejected before advertiser socket startup");
    }
    for (const auto &label : std::array<std::string, 3>{"A", std::string(63, 'a'), "F914CF5D-479A-4D39-AB92-A472792ABAB6"}) {
        input = records(); input.host.instance_identifier = label;
        input.service_instance = label + "." + std::string(service); input.target = label + ".local.";
        packet = scrctl::wifi::mdns::detail::host_announcement(input, 120, error);
        check(!packet.empty() && error.empty(), "one-byte, maximum label and fresh UUID are valid");
        if (!packet.empty()) {
            const auto p = parse(packet);
            check(p.txt.contains("identifier") && p.txt.at("identifier") == label && p.ptr == label + "." + std::string(service) &&
                  p.target == label + ".local.", "valid identity remains exactly the same across TXT/PTR/SRV");
        }
    }
    std::printf("mdns_advertiser: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
