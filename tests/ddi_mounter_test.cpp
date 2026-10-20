#include "ddi/Mounter.h"
#include "ddi/DownloadInternal.h"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <stdexcept>
#include <thread>

namespace {
namespace ddi = scrctl::ddi;
using V = scrctl::plist::Value;
int failures = 0;
void check(bool value, const char *label) { std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label); if (!value) ++failures; }
std::vector<uint8_t> digest(const std::string &data, const EVP_MD *alg) {
    std::vector<uint8_t> bytes(EVP_MAX_MD_SIZE); unsigned size = 0;
    if (EVP_Digest(data.data(), data.size(), bytes.data(), &size, alg, nullptr) != 1) throw std::runtime_error("hash failed");
    bytes.resize(size); return bytes;
}
std::string hex(const std::vector<uint8_t> &data) {
    std::string result; for (auto b : data) { result += "0123456789abcdef"[b >> 4]; result += "0123456789abcdef"[b & 15]; } return result;
}
struct Fixture {
    ddi::detail::Catalog catalog;
    ddi::DownloadResult download;
    ddi::DownloadTarget target;
    std::filesystem::path root;
    std::map<std::string, std::string> data;
    Fixture(ddi::Kind kind = ddi::Kind::Classic, std::string os = "16.7.12") {
        std::vector<uint8_t> random(12); if (RAND_bytes(random.data(), random.size()) != 1) throw std::runtime_error("random failed");
        root = std::filesystem::temp_directory_path() / ("scrctl-mounter-test-" + hex(random));
        std::filesystem::create_directory(root);
        catalog.kind = kind; catalog.build = kind == ddi::Kind::Classic ? os.substr(0, os.rfind('.')) : "TEST42";
        catalog.source_url = "https://example.invalid/pinned";
        target.system_version = std::move(os);
        if (kind == ddi::Kind::Classic) data = {{"DeveloperDiskImage.dmg", std::string(150000, 'i')}, {"DeveloperDiskImage.dmg.signature", "classic signature"}};
        else {
            target.product_type = "iPad11,2"; target.hardware_model = "j211ap"; target.board_id = 22; target.chip_id = 32800;
            data = {{"Image.dmg", std::string(150000, 'i')}, {"Image.dmg.trustcache", "trust cache"}};
            auto manifest = V::Dict(); manifest.set("ProductBuildVersion", V::Str(catalog.build));
            auto identity = V::Dict(); identity.set("Ap,ProductType", V::Str(target.product_type));
            identity.set("ApBoardID", V::Int(22)); identity.set("ApChipID", V::Int(32800));
            auto info = V::Dict(); info.set("DeviceClass", V::Str(target.hardware_model)); info.set("Variant", V::Str("Customer iOS Developer PDI")); identity.set("Info", std::move(info));
            auto payloads = V::Dict();
            for (auto [key, name] : {std::pair{"PersonalizedDMG", "Image.dmg"}, {"LoadableTrustCache", "Image.dmg.trustcache"}}) {
                auto entry = V::Dict(); auto info = V::Dict(); info.set("Path", V::Str(name)); entry.set("Info", std::move(info));
                entry.set("Digest", V::OfData(digest(data.at(name), EVP_sha384()))); payloads.set(key, std::move(entry));
            }
            identity.set("Manifest", std::move(payloads)); auto identities = V::Array(); identities.push(std::move(identity));
            manifest.set("BuildIdentities", std::move(identities)); data["BuildManifest.plist"] = scrctl::plist::write(manifest);
        }
        for (auto &[name, bytes] : data) {
            std::ofstream(root / name, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            if (kind == ddi::Kind::Classic) {
                std::string object = "blob " + std::to_string(bytes.size()); object.push_back('\0'); object += bytes;
                catalog.assets.push_back({name, bytes.size(), {}, hex(digest(object, EVP_sha1()))});
            } else catalog.assets.push_back({name, bytes.size(), hex(digest(bytes, EVP_sha256())), {}});
        }
        auto marker = catalog.build + "\n" + catalog.source_url + "\n";
        std::ofstream(root / ".complete", std::ios::binary).write(marker.data(), static_cast<std::streamsize>(marker.size()));
        download.directory = root; download.kind = kind; download.build = catalog.build;
    }
    ~Fixture() { std::error_code ec; std::filesystem::remove_all(root, ec); }
};
V status(const char *name) { auto reply = V::Dict(); reply.set("Status", V::Str(name)); return reply; }
struct Fake {
    ddi::PlistService service;
    std::vector<std::string> commands;
    std::vector<uint8_t> expected_signature;
    std::vector<uint8_t> received;
    bool mounted = false, copy_unknown = false, developer = true, empty_signature = false;
    bool bad_ack = false, bad_complete = false, bad_mount = false, bad_post_signature = false;
    bool upload_failure = false, nonce_changed = false, invalid_identifiers = false;
    bool missing_post_mount = false, oversized = false, after_mount = false;
    bool attached_copy_entry = false, copy_entry_unmounted = false, wrong_mount_path = false;
    bool missing_copy_mount_state = false, missing_copy_mount_path = false;
    int reconnects = 0, nonce_queries = 0, writes = 0;
    uint64_t last_budget = 0;
    Fake() {
        service.request = [&](const V &request, std::chrono::milliseconds timeout, std::string &) -> std::optional<V> {
            last_budget = static_cast<uint64_t>(timeout.count());
            auto name = request.find("Command")->string; commands.push_back(name);
            if (oversized) { auto reply = V::Dict(); reply.set("X", V::OfData(std::vector<uint8_t>(2 * 1024 * 1024 + 1))); return reply; }
            if (name == "LookupImage") {
                auto reply = V::Dict(); reply.set("ImagePresent", V::Bool(mounted)); auto signatures = V::Array();
                if (mounted && !empty_signature) signatures.push(V::OfData(bad_post_signature && after_mount ? std::vector<uint8_t>{99} : expected_signature));
                reply.set("ImageSignature", std::move(signatures)); return reply;
            }
            if (name == "CopyDevices") {
                auto reply = V::Dict();
                if (copy_unknown) { reply.set("Error", V::Str("UnknownCommand")); return reply; }
                auto entries = V::Array();
                // The fake derives type from the expected signature and may
                // expose an attached-but-unmounted image independently.
                if (mounted || attached_copy_entry) {
                    auto entry = V::Dict();
                    const bool personalized = expected_signature == std::vector<uint8_t>{1,2,3};
                    entry.set("DiskImageType", V::Str(personalized ? "Personalized" : "Developer"));
                    if (!missing_copy_mount_state) entry.set("IsMounted", V::Bool(!copy_entry_unmounted));
                    if (!missing_copy_mount_path) entry.set("MountPath", V::Str(wrong_mount_path ? "/private/other" :
                        personalized ? "/System/Developer" : "/Developer"));
                    entries.push(std::move(entry));
                }
                reply.set("EntryList", std::move(entries)); return reply;
            }
            if (name == "QueryDeveloperModeStatus") { auto reply = V::Dict(); reply.set("DeveloperModeStatus", V::Bool(developer)); return reply; }
            if (name == "QueryPersonalizationIdentifiers") {
                auto ids = V::Dict(); ids.set("BoardId", V::UInt(invalid_identifiers ? 23 : 22)); ids.set("ChipID", V::UInt(32800));
                auto reply = V::Dict(); reply.set("PersonalizationIdentifiers", std::move(ids)); return reply;
            }
            if (name == "QueryNonce") {
                auto reply = V::Dict(); reply.set("PersonalizationNonce", V::OfData({static_cast<uint8_t>(nonce_changed && ++nonce_queries > 1 ? 8 : 7)})); return reply;
            }
            if (name == "UnmountImage") { mounted = false; return status("Complete"); }
            if (name == "ReceiveBytes") { expected_signature = request.find("ImageSignature")->data; return status(bad_ack ? "Complete" : "ReceiveBytesAck"); }
            if (name == "MountImage") { after_mount = true; mounted = !missing_post_mount; return status(bad_mount ? "Failed" : "Complete"); }
            throw std::runtime_error("unexpected fake command");
        };
        service.receive = [&](std::chrono::milliseconds, std::string &) { return std::optional<V>(status(bad_complete ? "Failed" : "Complete")); };
        service.send_bytes = [&](const uint8_t *data, size_t size, std::chrono::milliseconds, std::string &) {
            ++writes; if (upload_failure) return false; received.insert(received.end(), data, data + size); return true;
        };
        service.reconnect = [&](std::chrono::milliseconds, std::string &) { ++reconnects; return true; };
    }
    bool called(std::string name) { return std::find(commands.begin(), commands.end(), name) != commands.end(); }
};
std::optional<ddi::MountResult> run(Fixture &f, Fake &fake, ddi::MountOptions options, std::string &error) {
    if (!options.personalize) options.personalize = [](const V &, const V &ids, uint64_t ecid, const std::vector<uint8_t> &nonce,
                                               std::chrono::milliseconds left, std::string &) -> std::optional<std::vector<uint8_t>> {
        if (ecid != UINT64_MAX || !ids.find("BoardId") || nonce != std::vector<uint8_t>{7} || left.count() <= 0) throw std::runtime_error("bad signer input");
        return std::vector<uint8_t>{1,2,3};
    };
    return ddi::detail::mount_image_with_catalog(fake.service, f.download, f.target, UINT64_MAX, options, f.catalog, error);
}
} // namespace
int main() {
    std::string error;
    {
        Fixture f; Fake fake; uint64_t final_progress = 0; ddi::MountOptions options;
        options.progress = [&](uint64_t done, uint64_t total) { if (total != 150000) throw std::runtime_error("bad progress"); final_progress = done; };
        auto result = run(f, fake, options, error);
        check(result && result->state == ddi::MountState::Mounted && result->signature_matches == true && fake.received.size() == 150000 && fake.writes == 3 && final_progress == 150000, "Classic upload requires ack, transfers chunks and verifies matching signature");
    }
    {
        Fixture f(ddi::Kind::Personalized, "18.7.8"); Fake fake;
        auto result = run(f, fake, {}, error);
        check(result && result->state == ddi::MountState::Mounted && fake.called("QueryNonce") && fake.called("QueryPersonalizationIdentifiers"), "Personalized uses live hardware/nonce, full-width ECID and signed ticket");
    }
    {
        Fixture f; Fake fake; fake.mounted = true; fake.expected_signature = {42};
        auto result = run(f, fake, {}, error);
        check(!result && error.find("signature does not match") != std::string::npos && !fake.called("ReceiveBytes") && !fake.called("UnmountImage"), "Existing mismatched Classic image fails without mutation");
    }
    {
        Fixture f; Fake fake; fake.mounted = true;
        const auto &signature = f.data.at("DeveloperDiskImage.dmg.signature");
        fake.expected_signature.assign(signature.begin(), signature.end());
        auto result = run(f, fake, {}, error);
        check(result && result->state == ddi::MountState::AlreadyMounted && result->signature_matches == true &&
              !fake.called("ReceiveBytes") && !fake.called("UnmountImage"), "Matching Classic signature reuses mounted image");
        fake.empty_signature = true;
        check(!run(f, fake, {}, error) && !fake.called("ReceiveBytes") && !fake.called("UnmountImage"),
              "Mounted Classic image without a verifiable signature fails without mutation");
    }
    {
        Fixture f; Fake fake; fake.mounted = true; fake.expected_signature = {42}; ddi::MountOptions options; options.replace_existing = true;
        auto result = run(f, fake, options, error);
        check(result && result->state == ddi::MountState::Mounted && fake.called("UnmountImage"), "Explicit replacement verifies unmount then mounts prepared image");
    }
    {
        Fixture f(ddi::Kind::Personalized, "18.7.8"); Fake fake; fake.mounted = true; fake.expected_signature = {42}; ddi::MountOptions options; options.replace_existing = true;
        options.personalize = [](const V &, const V &, uint64_t, const std::vector<uint8_t> &, std::chrono::milliseconds, std::string &e) -> std::optional<std::vector<uint8_t>> { e = "signing refused"; return std::nullopt; };
        check(!run(f, fake, options, error) && !fake.called("UnmountImage"), "Replacement retains existing image if ticket preparation fails");
    }
    {
        Fixture f; Fake fake; std::ofstream(f.root / "DeveloperDiskImage.dmg", std::ios::binary) << "bad";
        check(!run(f, fake, {}, error) && fake.commands.empty(), "Corrupt cache rejected before any device operation");
    }
    {
        Fixture f; Fake fake; fake.developer = false;
        check(!run(f, fake, {}, error) && !fake.called("ReceiveBytes"), "Disabled Developer Mode prevents upload");
    }
    {
        Fixture f(ddi::Kind::Classic, "12.5.7"); Fake fake; fake.copy_unknown = true;
        auto result = run(f, fake, {}, error);
        check(result && fake.reconnects == 1 && !fake.called("QueryDeveloperModeStatus") && std::count(fake.commands.begin(), fake.commands.end(), "CopyDevices") == 1, "Legacy unsupported CopyDevices reconnects once and skips Developer Mode query");
    }
    for (int gate = 0; gate < 6; ++gate) {
        Fixture f; Fake fake;
        if (gate == 0) fake.bad_ack = true; if (gate == 1) fake.upload_failure = true;
        if (gate == 2) fake.bad_complete = true; if (gate == 3) fake.bad_mount = true;
        if (gate == 4) fake.missing_post_mount = true; if (gate == 5) fake.bad_post_signature = true;
        auto result = run(f, fake, {}, error);
        check(!result && (gate != 0 || fake.writes == 0) && (gate >= 3 || !fake.called("MountImage")), "Upload/mount failure gates never report installation success");
    }
    {
        Fixture f(ddi::Kind::Personalized, "18.7.8"); Fake fake; fake.nonce_changed = true;
        check(!run(f, fake, {}, error) && !fake.called("ReceiveBytes"), "Nonce change during signing prevents stale-ticket upload");
    }
    {
        Fixture f(ddi::Kind::Personalized, "18.7.8"); Fake fake; fake.invalid_identifiers = true;
        check(!run(f, fake, {}, error) && !fake.called("ReceiveBytes") && !fake.called("QueryNonce"), "Hardware identity change blocks signing and upload");
    }
    {
        Fixture f(ddi::Kind::Personalized, "18.7.8"); Fake fake; fake.empty_signature = true; fake.expected_signature = {1,2,3};
        auto result = run(f, fake, {}, error);
        check(result && result->state == ddi::MountState::Mounted && !result->signature_matches && result->verification == "CopyDevices entry", "Empty Personalized lookup signature requires CopyDevices mounted evidence");
    }
    for (auto kind : {ddi::Kind::Classic, ddi::Kind::Personalized}) {
        Fake fake; fake.attached_copy_entry = true; fake.copy_entry_unmounted = true;
        fake.expected_signature = kind == ddi::Kind::Personalized ? std::vector<uint8_t>{1,2,3} : std::vector<uint8_t>{42};
        auto result = ddi::inspect_mounted(fake.service, kind, {}, error);
        check(result && result->state == ddi::MountState::NotMounted,
              "CopyDevices attached but unmounted image is not a mounted DDI");
        fake.copy_entry_unmounted = false; fake.wrong_mount_path = true;
        check(!ddi::inspect_mounted(fake.service, kind, {}, error),
              "CopyDevices matching image type at another mount path cannot verify developer mount");
        fake.wrong_mount_path = false; fake.missing_copy_mount_state = true;
        check(!ddi::inspect_mounted(fake.service, kind, {}, error),
              "CopyDevices entry without definite mounted state cannot verify developer mount");
        fake.missing_copy_mount_state = false; fake.missing_copy_mount_path = true;
        check(!ddi::inspect_mounted(fake.service, kind, {}, error),
              "CopyDevices entry without a mount path cannot verify developer mount");
    }
    {
        Fixture f; Fake fake; ddi::MountOptions options; options.cancelled = [] { return true; };
        check(!run(f, fake, options, error) && fake.commands.empty(), "Cancellation prevents all operations");
    }
    {
        Fixture f; Fake fake; ddi::MountOptions options; options.cancelled = [&] { return fake.writes > 0; };
        check(!run(f, fake, options, error) && fake.writes == 1 && !fake.called("MountImage"), "Cancellation during raw transfer stops remaining chunks");
    }
    {
        Fake fake; ddi::MountOptions options; options.timeout = std::chrono::seconds(1);
        fake.service.request = [](const V &, std::chrono::milliseconds left, std::string &) -> std::optional<V> {
            std::this_thread::sleep_for(left + std::chrono::milliseconds(10));
            auto reply = V::Dict(); reply.set("ImagePresent", V::Bool(false)); return reply;
        };
        check(!ddi::inspect_mounted(fake.service, ddi::Kind::Classic, options, error) &&
              error.find("timed out") != std::string::npos, "Expired deadline after service callback cannot report success");
    }
    {
        Fake fake; fake.oversized = true;
        check(!ddi::inspect_mounted(fake.service, ddi::Kind::Classic, {}, error) && error.size() < 1024, "Oversized reply rejected with bounded diagnostic");
    }
    return failures ? 1 : 0;
}
