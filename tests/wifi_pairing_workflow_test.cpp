#include "remote/WifiPairing.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace {
using namespace scrctl;
int checks = 0, failures = 0;
void check(bool ok, const char *label) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", label); }
}
struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("scrctl-wifi-pair-workflow-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    remote::WifiPairingOptions options;
    wifi::PairRecord record;
    int verifies = 0;
    bool verified = true;
    std::function<void()> during_verify;
    Fixture() {
        options.pairing_directory = root.string();
        record.udid = "test-device";
        record.host_identifier = "new-host";
        record.host_private_key.assign(32, 1);
        record.host_public_key.assign(32, 2);
        record.peer_identifier = wifi::bytes_of("signed-peer");
        record.peer_public_key.assign(32, 3);
        record.peer_alt_irk.assign(16, 4);
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
    std::string path() const { return wifi::record_path(root.string(), record.udid); }
    bool admit(std::string &err) {
        return remote::detail::admit_wifi_pairing(record, options, err);
    }
    remote::PairingResult run() {
        return remote::detail::save_wifi_pairing(record, options,
            [&](const wifi::PairRecord &actual, std::string &) {
                ++verifies;
                check(actual.peer_identifier == record.peer_identifier &&
                      actual.peer_public_key == record.peer_public_key,
                      "fresh verify receives the exact signed device identity");
                if (during_verify) during_verify();
                wifi::PairVerifyResult result;
                result.outcome = verified ? wifi::VerifyOutcome::Paired : wifi::VerifyOutcome::NotPaired;
                result.error = verified ? "" : "synthetic rejection";
                return result;
            });
    }
    std::string contents() const {
        std::ifstream input(path(), std::ios::binary);
        return {std::istreambuf_iterator<char>(input), {}};
    }
};
} // namespace

int main() {
    {
        Fixture f;
        f.record.host_identifier.clear();
        f.record.host_private_key.clear();
        f.record.host_public_key.clear();
        std::string error = "old error";
        check(f.admit(error) && error.empty() && f.verifies == 0 && !std::filesystem::exists(f.root),
              "authenticated M5 peer is admitted without host M6 fields, verification, or file creation");
        f.options.udid = f.record.udid;
        check(f.admit(error), "admission accepts the explicitly expected authenticated device");
        f.options.udid = "different-device";
        check(!f.admit(error) && error.find("unexpected device identity") != std::string::npos &&
                  !std::filesystem::exists(f.root),
              "wrong authenticated device is refused before successful M6 without creating a directory");
    }
    {
        Fixture f;
        std::filesystem::create_directories(f.root);
        std::ofstream(f.path()) << "preexisting trust bytes";
        std::string error;
        check(f.options.udid.empty() && !f.admit(error) &&
                  error.find("already exists") != std::string::npos && f.verifies == 0 &&
                  f.contents() == "preexisting trust bytes",
              "omitting -s still rejects an existing authenticated device record before successful M6");
    }
#ifndef _WIN32
    for (const bool dangling : {false, true}) {
        Fixture f;
        std::filesystem::create_directories(f.root);
        const auto target = f.root / "symlink-target";
        if (!dangling) std::ofstream(target) << "external trust bytes";
        std::filesystem::create_symlink(target, f.path());
        std::string error;
        check(!f.admit(error) && error.find("already exists") != std::string::npos &&
                  std::filesystem::is_symlink(std::filesystem::symlink_status(f.path())) &&
                  f.verifies == 0 && (dangling ? !std::filesystem::exists(target) :
                                     f.contents() == "external trust bytes"),
              "admission rejects existing and dangling symlinks without following or changing trust");
    }
    {
        Fixture f;
        std::ofstream(f.root) << "not a directory";
        std::string error;
        check(!f.admit(error) && error.find("Cannot inspect") != std::string::npos && f.verifies == 0,
              "target inspection I/O failure refuses successful M6 rather than treating it as an absent record");
    }
#endif
    {
        Fixture f;
        f.options.should_cancel = [] { return true; };
        std::string error;
        check(!f.admit(error) && error.find("cancelled") != std::string::npos &&
                  !std::filesystem::exists(f.root) && f.verifies == 0,
              "pre-M6 admission cancellation has no verification or file side effects");
    }
    for (int missing = 0; missing < 4; ++missing) {
        Fixture f;
        if (missing == 0) f.record.udid.clear();
        if (missing == 1) f.record.peer_identifier.clear();
        if (missing == 2) f.record.peer_public_key.clear();
        if (missing == 3) f.record.peer_alt_irk.clear();
        std::string error;
        check(!f.admit(error) && error.find("incomplete") != std::string::npos &&
                  !std::filesystem::exists(f.root),
              "admission refuses incomplete peer identity before touching a record path");
    }
    for (const char *udid : {" device", "device ", "device\nmetadata=injected"}) {
        Fixture f;
        f.record.udid = udid;
        std::string error;
        check(!f.admit(error) && error.find("unexpected device identity") != std::string::npos &&
                  !std::filesystem::exists(f.root),
              "admission rejects non-roundtrippable device metadata before successful M6");
    }
    {
        Fixture f;
        auto result = f.run();
        std::string error;
        const auto saved = wifi::load_record(f.path(), error);
        check(result.ok && f.verifies == 1 && saved && saved->udid == f.record.udid &&
              saved->peer_public_key == f.record.peer_public_key, "new verified record is persisted");
#ifndef _WIN32
        const auto permissions = std::filesystem::status(f.path()).permissions();
        check((permissions & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) ==
              std::filesystem::perms::none, "saved wireless private key has no group or other access");
#endif
    }
    {
        Fixture f;
        f.verified = false;
        check(!f.run().ok && f.verifies == 1 && !std::filesystem::exists(f.path()),
              "failed fresh verify does not publish a record");
    }
    {
        Fixture f;
        f.options.udid = "a-different-device";
        check(!f.run().ok && f.verifies == 0 && !std::filesystem::exists(f.root),
              "unexpected device is rejected without verification or directory creation");
    }
    for (int field = 0; field < 4; ++field) {
        Fixture f;
        if (field == 0) f.record.peer_public_key.clear();
        if (field == 1) f.record.peer_identifier.clear();
        if (field == 2) f.record.peer_alt_irk.clear();
        if (field == 3) f.record.host_private_key.clear();
        check(!f.run().ok && f.verifies == 0, "incomplete setup cannot reach verify or save");
    }
    {
        Fixture f;
        f.record.udid = "device\nhost_identifier=injected";
        check(!f.run().ok && f.verifies == 0 && !std::filesystem::exists(f.root),
              "device metadata cannot inject lines into the record text format");
    }
    for (const char *udid : {" device", "device ", " "}) {
        Fixture f;
        f.record.udid = udid;
        check(!f.run().ok && f.verifies == 0 && !std::filesystem::exists(f.root),
              "authenticated metadata cannot change identity when the record parser trims values");
    }
    {
        Fixture f;
        f.options.should_cancel = [] { return true; };
        check(!f.run().ok && f.verifies == 0 && !std::filesystem::exists(f.root),
              "cancelled workflow has no file side effects");
    }
    {
        Fixture f;
        bool cancelled = false;
        f.options.should_cancel = [&] { return cancelled; };
        f.during_verify = [&] { cancelled = true; };
        check(!f.run().ok && f.verifies == 1 && !std::filesystem::exists(f.path()),
              "cancellation during verify prevents persistence");
    }
    {
        Fixture f;
        std::filesystem::create_directories(f.root);
        std::ofstream(f.path()) << "preexisting trust bytes";
        check(!f.run().ok && f.verifies == 0 && f.contents() == "preexisting trust bytes",
              "existing record is preserved without attempting verification");
    }
    {
        Fixture f;
        f.during_verify = [&] {
            std::filesystem::create_directories(f.root);
            std::ofstream(f.path()) << "concurrent trust bytes";
        };
        check(!f.run().ok && f.verifies == 1 && f.contents() == "concurrent trust bytes",
              "atomic exclusive publication preserves a concurrent writer's record");
    }
    {
        Fixture f;
        std::filesystem::create_directories(f.root);
        std::string error;
        check(wifi::save_record_new(f.path(), f.record, error), "exclusive save publishes a fresh record");
        const auto original = f.contents();
        f.record.host_identifier = "different-host";
        check(!wifi::save_record_new(f.path(), f.record, error) && f.contents() == original,
              "exclusive save never replaces an existing record");
        size_t files = 0;
        for (const auto &entry : std::filesystem::directory_iterator(f.root)) { (void)entry; ++files; }
        check(files == 1, "exclusive save cleans temporary files on success and collision");
    }
    {
        remote::WifiPairingOptions o;
        o.timeout_ms = 0;
        check(!remote::pair_wifi_remote(o).ok, "invalid timeout fails before network setup");
        o.timeout_ms = 1000;
        check(!remote::pair_wifi_remote(o).ok, "missing PIN display fails before network setup");
        o.display_pin = [](std::string_view, std::string &) { return true; };
        o.should_cancel = [] { return true; };
        check(!remote::pair_wifi_remote(o).ok, "pre-cancelled listener never advertises");
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
