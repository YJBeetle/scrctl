#include "ddi/DownloadInternal.h"
#include "plist/Plist.h"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <array>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
namespace fs = std::filesystem;
namespace ddi = scrctl::ddi;
int failures = 0;
void check(bool ok, const char *label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
std::vector<uint8_t> digest(const std::string &text, const EVP_MD *algorithm) {
    std::vector<uint8_t> result(EVP_MAX_MD_SIZE);
    unsigned size = 0;
    if (EVP_Digest(text.data(), text.size(), result.data(), &size, algorithm, nullptr) != 1)
        throw std::runtime_error("fixture digest failed");
    result.resize(size); return result;
}
std::string hex(const std::vector<uint8_t> &bytes) {
    std::string result;
    for (auto b : bytes) { result += "0123456789abcdef"[b >> 4]; result += "0123456789abcdef"[b & 15]; }
    return result;
}
struct Fixture {
    ddi::detail::Catalog catalog{"TEST42", "https://example.invalid/pinned-commit", {}};
    std::map<std::string, std::string> contents{
        {"Image.dmg", std::string("synthetic image\0tail", 20)},
        {"Image.dmg.trustcache", "synthetic trust cache"},
        {"Image.dmg.cryptex_info", "synthetic cryptex info"},
        {"Image.dmg.root_hash", "synthetic root hash"},
    };
    int calls = 0;
    Fixture() {
        using V = scrctl::plist::Value;
        auto manifest = V::Dict();
        manifest.set("ProductBuildVersion", V::Str(catalog.build));
        auto identity = V::Dict(); auto info = V::Dict(); auto entries = V::Dict();
        info.set("Variant", V::Str("iOS Customer Developer Disk Image Cryptex"));
        identity.set("Info", std::move(info));
        const std::array<std::pair<const char *, const char *>, 4> pairs{{
            {"Cryptex1,GenericDmg", "Image.dmg"}, {"Cryptex1,GenericTrustCache", "Image.dmg.trustcache"},
            {"Cryptex1,CryptexInfoPlist", "Image.dmg.cryptex_info"}, {"Cryptex1,GenericVolume", "Image.dmg.root_hash"},
        }};
        for (const auto &[key, name] : pairs) {
            auto entry = V::Dict(); auto entry_info = V::Dict();
            entry_info.set("Path", V::Str(name)); entry.set("Info", std::move(entry_info));
            entry.set("Digest", V::OfData(digest(contents.at(name), EVP_sha384())));
            entries.set(key, std::move(entry));
        }
        identity.set("Manifest", std::move(entries));
        auto identities = V::Array(); identities.push(std::move(identity));
        manifest.set("BuildIdentities", std::move(identities));
        contents["BuildManifest.plist"] = scrctl::plist::write(manifest);
        refresh_catalog();
    }
    void refresh_catalog() {
        catalog.assets.clear();
        for (const auto &[name, bytes] : contents)
            catalog.assets.push_back({name, bytes.size(), hex(digest(bytes, EVP_sha256()))});
    }
    ddi::detail::Transfer transfer() {
        return [&](const std::string &url, uint64_t, std::chrono::milliseconds,
                   const std::function<bool()> &cancelled, const ddi::detail::Sink &sink, std::string &) {
            ++calls;
            const auto &bytes = contents.at(url.substr(url.find_last_of('/') + 1));
            return !cancelled() && sink(bytes.data(), bytes.size());
        };
    }
};

struct PersonalizedFixture : Fixture {
    PersonalizedFixture() {
        catalog.kind = ddi::Kind::Personalized;
        catalog.source_url = "https://example.invalid/pinned-personalized";
        contents.erase("Image.dmg.cryptex_info");
        contents.erase("Image.dmg.root_hash");
        using V = scrctl::plist::Value;
        auto manifest = V::Dict();
        manifest.set("ProductBuildVersion", V::Str(catalog.build));
        auto supported = V::Array(); supported.push(V::Str("iPad11,2")); supported.push(V::Str("iPad5,1"));
        manifest.set("SupportedProductTypes", std::move(supported));
        auto identity = V::Dict(); auto info = V::Dict(); auto entries = V::Dict();
        identity.set("Ap,ProductType", V::Str("iPad11,2"));
        identity.set("ApBoardID", V::Str("0x16"));
        identity.set("ApChipID", V::Str("0x8020"));
        info.set("DeviceClass", V::Str("j211ap"));
        info.set("Variant", V::Str("Customer iOS Developer PDI"));
        identity.set("Info", std::move(info));
        for (const auto &[key, name] : std::array<std::pair<const char *, const char *>, 2>{{
                 {"PersonalizedDMG", "Image.dmg"}, {"LoadableTrustCache", "Image.dmg.trustcache"}}}) {
            auto entry = V::Dict(); auto entry_info = V::Dict();
            entry_info.set("Path", V::Str(name)); entry.set("Info", std::move(entry_info));
            entry.set("Digest", V::OfData(digest(contents.at(name), EVP_sha384())));
            entries.set(key, std::move(entry));
        }
        identity.set("Manifest", std::move(entries));
        auto identities = V::Array();
        // An old class can advertise the model yet reference another payload.
        auto obsolete = identity;
        obsolete.set("Ap,ProductType", V::Str("iPad5,1"));
        auto obsolete_entries = *obsolete.find("Manifest");
        auto obsolete_dmg = *obsolete_entries.find("PersonalizedDMG");
        obsolete_dmg.set("Digest", V::OfData(std::vector<uint8_t>(48, 0)));
        obsolete_entries.set("PersonalizedDMG", std::move(obsolete_dmg));
        obsolete.set("Manifest", std::move(obsolete_entries));
        identities.push(std::move(obsolete)); identities.push(std::move(identity));
        manifest.set("BuildIdentities", std::move(identities));
        contents["BuildManifest.plist"] = scrctl::plist::write(manifest);
        refresh_catalog();
    }
};

struct ClassicFixture : Fixture {
    ClassicFixture() {
        catalog.kind = ddi::Kind::Classic; catalog.build = "16.7";
        catalog.source_url = "https://example.invalid/pinned-classic/16.7";
        contents = {{"DeveloperDiskImage.dmg", std::string("classic\0image", 13)},
                    {"DeveloperDiskImage.dmg.signature", "synthetic signature"}};
        catalog.assets.clear();
        for (const auto &[name, bytes] : contents) {
            std::string git_object = "blob " + std::to_string(bytes.size());
            git_object += '\0'; git_object += bytes;
            catalog.assets.push_back({name, bytes.size(), {}, hex(digest(git_object, EVP_sha1()))});
        }
    }
};
struct Temp {
    fs::path path;
    Temp() {
        std::vector<uint8_t> random(12);
        if (RAND_bytes(random.data(), random.size()) != 1) throw std::runtime_error("temp nonce failed");
        path = fs::temp_directory_path() / ("scrctl-ddi-test-" + hex(random));
        fs::create_directory(path);
    }
    ~Temp() { std::error_code ec; fs::remove_all(path, ec); }
};
bool empty(const fs::path &root) { return fs::directory_iterator(root) == fs::directory_iterator{}; }
void write(const fs::path &path, const std::string &bytes) {
    std::ofstream file(path, std::ios::binary); file << bytes;
}
}

int main() {
    try {
        Fixture fixture; Temp temp; ddi::DownloadOptions opts; opts.cache_root = temp.path;
        std::string error;
        auto result = ddi::detail::download(opts, fixture.catalog, fixture.transfer(), error);
        check(result && !result->cache_hit && fixture.calls == 5, "five files verified before atomic cache publication");
        check(result && ddi::detail::validate(result->directory, fixture.catalog, error), "complete cache includes manifest SHA-384 validation");
        result = ddi::detail::download(opts, fixture.catalog, fixture.transfer(), error);
        check(result && result->cache_hit && fixture.calls == 5, "valid cache reuse performs no network request");
        write(result->directory / "Image.dmg", std::string(20, 'x'));
        result = ddi::detail::download(opts, fixture.catalog, fixture.transfer(), error);
        check(!result && error.find("SHA-256") != std::string::npos && fixture.calls == 5,
              "same-size corrupted cache fails closed without overwrite");

        Temp partial; opts.cache_root = partial.path;
        int calls = 0;
        auto fail_halfway = [&](const std::string &, uint64_t, std::chrono::milliseconds,
                                const std::function<bool()> &, const ddi::detail::Sink &sink, std::string &err) {
            ++calls; sink("partial", 7); err = "connection interrupted"; return false;
        };
        result = ddi::detail::download(opts, fixture.catalog, fail_halfway, error);
        check(!result && empty(partial.path), "interrupted transfer cleans staging and never publishes completion");
        auto oversize = [](const std::string &, uint64_t max, std::chrono::milliseconds,
                           const std::function<bool()> &, const ddi::detail::Sink &sink, std::string &) {
            const std::string bytes(static_cast<size_t>(max + 1), 'x'); return sink(bytes.data(), bytes.size());
        };
        result = ddi::detail::download(opts, fixture.catalog, oversize, error);
        check(!result && empty(partial.path) && error.find("exceeds") != std::string::npos,
              "streaming size limit rejects oversized response without completion");
        auto truncated = [](const std::string &, uint64_t, std::chrono::milliseconds,
                            const std::function<bool()> &, const ddi::detail::Sink &sink, std::string &) { return sink("x", 1); };
        result = ddi::detail::download(opts, fixture.catalog, truncated, error);
        check(!result && empty(partial.path) && error.find("Incomplete") != std::string::npos,
              "HTTP success with truncated body is rejected");
        auto wrong_hash = fixture.transfer();
        fixture.contents["Image.dmg"][0] ^= 1; // Catalog remains anchored to the original payload.
        result = ddi::detail::download(opts, fixture.catalog, wrong_hash, error);
        check(!result && empty(partial.path) && error.find("SHA-256") != std::string::npos,
              "downloaded payload must match pinned SHA-256");
        fixture.refresh_catalog(); // Now catalog accepts changed bytes, manifest must still reject them.
        result = ddi::detail::download(opts, fixture.catalog, fixture.transfer(), error);
        check(!result && empty(partial.path) && error.find("manifest digest") != std::string::npos,
              "manifest SHA-384 binds payload even when transport catalog digest matches");

        Fixture cancel_fixture; opts.cancelled = [] { return true; };
        result = ddi::detail::download(opts, cancel_fixture.catalog, cancel_fixture.transfer(), error);
        check(!result && cancel_fixture.calls == 0 && error.find("cancelled") != std::string::npos,
              "pre-cancellation avoids network and file creation");
        bool cancelled = false; opts.cancelled = [&] { return cancelled; };
        opts.progress = [&](const std::string &, uint64_t, uint64_t) { cancelled = true; };
        result = ddi::detail::download(opts, cancel_fixture.catalog, cancel_fixture.transfer(), error);
        check(!result && empty(partial.path) && cancel_fixture.calls == 1,
              "cancellation between files cleans partial bundle");
        opts.cancelled = {}; opts.progress = {};
        opts.timeout = std::chrono::seconds(1);
        int timeout_calls = 0;
        auto wait_until_deadline = [&](const std::string &, uint64_t, std::chrono::milliseconds remaining,
                                       const std::function<bool()> &stop, const ddi::detail::Sink &, std::string &) {
            ++timeout_calls;
            if (remaining.count() > 1000) throw std::runtime_error("timeout budget was reset");
            while (!stop()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return false;
        };
        result = ddi::detail::download(opts, cancel_fixture.catalog, wait_until_deadline, error);
        check(!result && timeout_calls == 1 && empty(partial.path) && error.find("timed out") != std::string::npos,
              "one total deadline interrupts transfer and cleans partial cache");
        opts.timeout = std::chrono::seconds(300);
        opts.progress = [](const std::string &, uint64_t, uint64_t) { throw std::runtime_error("callback failed"); };
        result = ddi::detail::download(opts, cancel_fixture.catalog, cancel_fixture.transfer(), error);
        check(!result && empty(partial.path), "throwing progress callback cleans staging and lock");
        opts.progress = {};
        Fixture invalid_manifest;
        invalid_manifest.contents["BuildManifest.plist"] = "not a plist";
        invalid_manifest.refresh_catalog();
        result = ddi::detail::download(opts, invalid_manifest.catalog, invalid_manifest.transfer(), error);
        check(!result && empty(partial.path) && error.find("plist") != std::string::npos,
              "hash-correct malformed plist never publishes cache");
        Fixture wrong_build; wrong_build.catalog.build = "OTHER42";
        result = ddi::detail::download(opts, wrong_build.catalog, wrong_build.transfer(), error);
        check(!result && empty(partial.path) && error.find("pinned build") != std::string::npos,
              "manifest build must match the selected catalog");

        Fixture concurrent_fixture; Temp concurrent; opts.cache_root = concurrent.path;
        std::mutex mutex; std::condition_variable condition; bool entered = false, release = false;
        std::optional<ddi::DownloadResult> first_result; std::string first_error;
        auto held_transfer = [&](const std::string &url, uint64_t max, std::chrono::milliseconds timeout,
                                 const std::function<bool()> &stop, const ddi::detail::Sink &sink, std::string &err) {
            { std::unique_lock lock(mutex); entered = true; condition.notify_all(); condition.wait(lock, [&] { return release; }); }
            return concurrent_fixture.transfer()(url, max, timeout, stop, sink, err);
        };
        std::thread first([&] { first_result = ddi::detail::download(opts, concurrent_fixture.catalog, held_transfer, first_error); });
        { std::unique_lock lock(mutex); condition.wait(lock, [&] { return entered; }); }
        result = ddi::detail::download(opts, concurrent_fixture.catalog, concurrent_fixture.transfer(), error);
        const bool rejected_second = !result && error.find("lock") != std::string::npos;
        { std::lock_guard lock(mutex); release = true; } condition.notify_all(); first.join();
        check(rejected_second && first_result && concurrent_fixture.calls == 5,
              "concurrent downloader cannot overwrite or consume another staging bundle");
        fs::remove(first_result->directory / ".complete");
        result = ddi::detail::download(opts, concurrent_fixture.catalog, concurrent_fixture.transfer(), error);
        check(!result && concurrent_fixture.calls == 5, "missing completion marker is never treated as cached success");

        opts.cache_root = "relative/path";
        result = ddi::detail::download(opts, concurrent_fixture.catalog, concurrent_fixture.transfer(), error);
        check(!result && error.find("absolute") != std::string::npos, "relative cache root rejected");
        opts.cache_root = partial.path; opts.timeout = std::chrono::seconds(0);
        result = ddi::detail::download(opts, concurrent_fixture.catalog, concurrent_fixture.transfer(), error);
        check(!result && empty(partial.path), "invalid total timeout rejected");
        auto insecure = concurrent_fixture.catalog; insecure.source_url = "http://example.invalid";
        opts.timeout = std::chrono::seconds(300);
        result = ddi::detail::download(opts, insecure, concurrent_fixture.transfer(), error);
        check(!result && error.find("catalog") != std::string::npos, "catalog cannot opt out of HTTPS");

        Temp kinds; opts.cache_root = kinds.path;
        Fixture cryptex;
        auto cryptex_result = ddi::detail::download(opts, cryptex.catalog, cryptex.transfer(), error);
        PersonalizedFixture personalized;
        ddi::DownloadTarget ipad_target{"18.7.8", "iPad11,2", "J211AP", 0x16, 0x8020};
        auto personalized_result = ddi::detail::download(opts, personalized.catalog, personalized.transfer(), error, &ipad_target);
        check(personalized_result && personalized_result->kind == ddi::Kind::Personalized && personalized.calls == 3,
              "Personalized validates one model, hardware class, board and chip identity with both SHA-384 payloads");
        if (!personalized_result) throw std::runtime_error("Personalized fixture failed: " + error);
        check(cryptex_result && personalized_result && cryptex_result->directory != personalized_result->directory &&
                  fs::exists(cryptex_result->directory) && fs::exists(personalized_result->directory),
              "Personalized and Cryptex caches stay separate even with the same build");
        auto bad_target = ipad_target; bad_target.product_type = "iPad99,1";
        check(!ddi::detail::validate(personalized_result->directory, personalized.catalog, error, &bad_target) &&
                  error.find("hardware identity") != std::string::npos,
              "unknown product type cannot reuse another model's Personalized identity");
        check(!ddi::detail::download(opts, personalized.catalog, personalized.transfer(), error, &bad_target) &&
                  error.find("remove") == std::string::npos && fs::exists(personalized_result->directory) && personalized.calls == 3,
              "valid cache incompatible with another model is preserved without suggesting deletion");
        bad_target = ipad_target; bad_target.board_id = 0x17;
        check(!ddi::detail::validate(personalized_result->directory, personalized.catalog, error, &bad_target),
              "wrong board ID fails Personalized validation");
        bad_target = ipad_target; bad_target.chip_id = 0x8021;
        check(!ddi::detail::validate(personalized_result->directory, personalized.catalog, error, &bad_target),
              "wrong chip ID fails Personalized validation");
        bad_target = ipad_target; bad_target.hardware_model = "anotherap";
        check(!ddi::detail::validate(personalized_result->directory, personalized.catalog, error, &bad_target),
              "wrong hardware model fails Personalized validation");
        bad_target = ddi::DownloadTarget{"18.7", "iPad5,1", {}, {}, {}};
        check(!ddi::detail::validate(personalized_result->directory, personalized.catalog, error, &bad_target) &&
                  error.find("digest mismatch") != std::string::npos,
              "supported product list alone cannot hide a mismatched identity payload");
        check(ddi::detail::validate(personalized_result->directory, personalized.catalog, error),
              "untargeted Personalized validation accepts a valid identity despite stale other identities");
        auto reused_personalized = ddi::detail::download(opts, personalized.catalog, personalized.transfer(), error, &ipad_target);
        check(reused_personalized && reused_personalized->cache_hit && personalized.calls == 3,
              "targeted Personalized cache reuse revalidates hardware without network");

        ClassicFixture legacy;
        auto classic_result = ddi::detail::download(opts, legacy.catalog, legacy.transfer(), error);
        check(classic_result && classic_result->kind == ddi::Kind::Classic && legacy.calls == 2 &&
                  classic_result->directory.filename() == "classic-16.7",
              "Classic verifies Git blob SHA-1 over object header and payload");
        auto reused_classic = ddi::detail::download(opts, legacy.catalog, legacy.transfer(), error);
        check(reused_classic && reused_classic->cache_hit && legacy.calls == 2,
              "Classic cache reuse validates both assets without network");
        Temp legacy_bad; opts.cache_root = legacy_bad.path;
        legacy.contents["DeveloperDiskImage.dmg"][0] ^= 1;
        check(!ddi::detail::download(opts, legacy.catalog, legacy.transfer(), error) &&
                  error.find("Git blob SHA-1") != std::string::npos && empty(legacy_bad.path),
              "same-size Classic payload change cannot publish a cache");
        auto unsafe_catalog = legacy.catalog; unsafe_catalog.assets[0].name = "../escape";
        const auto classic_calls = legacy.calls;
        check(!ddi::detail::download(opts, unsafe_catalog, legacy.transfer(), error) && legacy.calls == classic_calls,
              "unsafe asset name is rejected before network or staging");
        unsafe_catalog = legacy.catalog; unsafe_catalog.assets[1] = unsafe_catalog.assets[0];
        check(!ddi::detail::download(opts, unsafe_catalog, legacy.transfer(), error) && legacy.calls == classic_calls,
              "duplicate assets cannot replace the expected two-file Classic structure");
        unsafe_catalog = legacy.catalog; unsafe_catalog.assets[0].git_blob_sha1 = std::string(40, 'g');
        check(!ddi::detail::download(opts, unsafe_catalog, legacy.transfer(), error) && legacy.calls == classic_calls,
              "invalid catalog checksum alphabet is rejected");
        const auto classic_16_7 = ddi::detail::classic_catalog({16, 7, 12});
        check(classic_16_7 && classic_16_7->build == "16.7" && !ddi::detail::classic_catalog({16, 8, 0}),
              "Classic catalog ignores patch and never falls back to an older minor");
        check(!ddi::download_for_target(opts, ddi::DownloadTarget{"28.0", {}, {}, {}, {}}, error) &&
                  error.find("future") != std::string::npos && empty(legacy_bad.path),
              "unknown future major fails public selection before network");
        check(!ddi::download_for_target(opts, ddi::DownloadTarget{"16.8", {}, {}, {}, {}}, error) &&
                  error.find("No pinned Classic") != std::string::npos && empty(legacy_bad.path),
              "unknown Classic catalog version fails before network");
    } catch (const std::exception &e) { std::fprintf(stderr, "exception: %s\n", e.what()); ++failures; }
    return failures ? 1 : 0;
}
