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
    } catch (const std::exception &e) { std::fprintf(stderr, "exception: %s\n", e.what()); ++failures; }
    return failures ? 1 : 0;
}
