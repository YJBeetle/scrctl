#include "remote/Pairing.h"
#include "transport/Usbmux.h"
#include "wifi/PairRecord.h"
#include "wifi/PairSetup.h"
#include "wifi/PairVerify.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace scrctl;
int checks = 0, failures = 0;

void check(bool ok, const char *label) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", label); }
}

wifi::PairRecord synthetic_record(std::string udid = "device-with:punctuation") {
    wifi::PairRecord record;
    record.udid = std::move(udid);
    record.host_identifier = "saved-host-identifier";
    record.host_private_key.assign(32, 1);
    record.host_public_key.assign(32, 2);
    record.peer_identifier = wifi::bytes_of("synthetic-peer");
    record.peer_public_key.assign(32, 3);
    record.peer_alt_irk.assign(16, 4);
    return record;
}

struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("scrctl-pairing-workflow-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    remote::UsbPairingOptions options;
    std::string udid = "device-with:punctuation";
    std::vector<std::string> trace;
    std::vector<wifi::VerifyOutcome> verify_outcomes{wifi::VerifyOutcome::Paired};
    std::function<void(wifi::PairSetupResult &)> mutate_setup;
    std::function<void(const wifi::PairRecord &)> on_verify;
    bool setup_ok = true;
    int setups = 0, verifies = 0;
    std::vector<std::string> messages;

    Fixture() {
        std::filesystem::create_directory(root);
        options.pairing_directory = (root / "records").string();
        options.progress = [&](std::string_view message) { messages.emplace_back(message); };
    }
    ~Fixture() { std::error_code ec; std::filesystem::remove_all(root, ec); }
    std::string path() const { return wifi::record_path(options.pairing_directory, udid); }

    void existing(const wifi::PairRecord &record) {
        std::string error;
        if (!wifi::save_record(path(), record, error)) throw std::runtime_error("Cannot write synthetic record");
    }
    void text(const std::string &value) {
        std::filesystem::create_directories(options.pairing_directory);
        std::ofstream out(path(), std::ios::binary);
        out << value;
        if (!out) throw std::runtime_error("Cannot write synthetic malformed record");
    }
    std::string contents() const {
        std::ifstream in(path(), std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }
    remote::PairingResult run(std::string_view hostname = "synthetic-hostname") {
        remote::detail::PairingWorkflowOperations operations;
        operations.setup = [&](std::string_view host_identifier, std::string_view hostname_arg,
                               std::string_view selected_udid, std::string &error) {
            ++setups;
            trace.push_back("setup");
            check(!host_identifier.empty() && !hostname_arg.empty() && selected_udid == udid,
                  "setup receives the selected USB UDID and stable host identity");
            wifi::PairSetupResult result;
            result.ok = setup_ok;
            result.record = synthetic_record(std::string(selected_udid));
            result.record.host_identifier = host_identifier;
            result.record.host_private_key.assign(32, 9);
            result.record.host_public_key.assign(32, 8);
            if (!setup_ok) result.error = error = "synthetic M6 proof rejection";
            if (mutate_setup) mutate_setup(result);
            return result;
        };
        operations.verify = [&](const wifi::PairRecord &record, std::string &error) {
            trace.push_back("verify");
            if (on_verify) on_verify(record);
            wifi::PairVerifyResult result;
            result.outcome = verify_outcomes[std::min<size_t>(verifies, verify_outcomes.size() - 1)];
            ++verifies;
            if (result.outcome != wifi::VerifyOutcome::Paired)
                result.error = error = "synthetic verify failure";
            return result;
        };
        return remote::detail::run_usb_pairing_workflow(udid, hostname, options, operations);
    }
};

void test_usb_selection() {
    std::string error;
    std::vector<transport::DeviceRecord> devices{{1, "same-device", "Network", 0},
                                                {2, "same-device", "USB", 0}};
    auto selected = remote::detail::select_pairing_usb_device(devices, {}, error);
    check(selected && selected->device_id == 2 && selected->udid == "same-device" && error.empty(),
          "unique USB selection ignores preceding usbmux Network entry");
    devices.push_back({3, "other-device", "USB", 0});
    check(!remote::detail::select_pairing_usb_device(devices, {}, error) && !error.empty(),
          "multiple USB devices are ambiguous without a UDID filter");
    selected = remote::detail::select_pairing_usb_device(devices, "other-device", error);
    check(selected && selected->device_id == 3 && error.empty(), "explicit original UDID selects the intended USB device");
    check(!remote::detail::select_pairing_usb_device(devices, "missing", error), "missing UDID never falls back to first device");
    devices.push_back({4, "other-device", "USB", 0});
    check(!remote::detail::select_pairing_usb_device(devices, "other-device", error), "duplicate matching USB entries remain ambiguous");
    check(!remote::detail::select_pairing_usb_device({{1, "network-only", "Network", 0}}, {}, error),
          "network-only device cannot establish first pairing trust");
    check(!remote::detail::select_pairing_usb_device({{1, {}, "USB", 0}}, {}, error), "empty USB UDID cannot bind a record");
}

void test_new_pairing() {
    Fixture good;
    good.on_verify = [&](const wifi::PairRecord &) {
        check(!std::filesystem::exists(good.path()), "new record remains absent until fresh-connection verify succeeds");
    };
    const auto result = good.run();
    check(result.ok && !result.reused && result.udid == good.udid && result.path == good.path() && result.error.empty(),
          "new successful pairing returns only after verified record is saved");
    check(good.trace == std::vector<std::string>{"setup", "verify"}, "setup precedes independent verify exactly once");
    std::string error;
    const auto saved = wifi::load_record(result.path, error);
    check(saved && saved->udid == good.udid && saved->complete() && saved->has_peer_identity(),
          "saved record retains original punctuated UDID and pinned device identity");

    Fixture setup_failed;
    setup_failed.setup_ok = false;
    const auto failed = setup_failed.run();
    check(!failed.ok && setup_failed.verifies == 0 && !std::filesystem::exists(setup_failed.path()),
          "M6 or SRP setup failure never verifies or publishes a record");
    for (const auto outcome : {wifi::VerifyOutcome::NotPaired, wifi::VerifyOutcome::TransportFailure}) {
        Fixture rejected;
        rejected.verify_outcomes = {outcome};
        const auto rejected_result = rejected.run();
        check(!rejected_result.ok && rejected.verifies == 1 && !std::filesystem::exists(rejected.path()),
              "new record verify failure never writes a pairing file");
    }
    for (const int defect : {0, 1, 2}) {
        Fixture mismatch;
        mismatch.mutate_setup = [defect](wifi::PairSetupResult &setup) {
            if (defect == 0) setup.record.peer_public_key.clear();
            if (defect == 1) setup.record.udid = "different-device";
            if (defect == 2) setup.record.host_identifier = "different-host";
        };
        const auto mismatch_result = mismatch.run();
        check(!mismatch_result.ok && mismatch.verifies == 0 && !std::filesystem::exists(mismatch.path()),
              "incomplete identity or mismatched setup record fails before verify and save");
    }
    Fixture cannot_save;
    cannot_save.on_verify = [&](const wifi::PairRecord &) {
        std::filesystem::create_directories(cannot_save.path());
    };
    const auto save_failure = cannot_save.run();
    check(!save_failure.ok && cannot_save.verifies == 1 && !save_failure.error.empty() &&
              std::filesystem::is_directory(cannot_save.path()),
          "save failure after device verification does not return success");
}

void test_existing_records() {
    for (const bool allow_repair : {false, true}) {
        Fixture reusable;
        reusable.existing(synthetic_record());
        const auto original = reusable.contents();
        reusable.options.allow_repair = allow_repair;
        const auto result = reusable.run("");
        check(result.ok && result.reused && reusable.setups == 0 && reusable.verifies == 1 && reusable.contents() == original,
              "usable existing record verifies without generating keys, rewriting file or needing hostname");
    }
    Fixture incomplete;
    auto legacy = synthetic_record();
    legacy.peer_public_key.clear();
    legacy.peer_identifier.clear();
    incomplete.existing(legacy);
    const auto original = incomplete.contents();
    auto result = incomplete.run();
    check(!result.ok && incomplete.setups == 0 && incomplete.verifies == 0 && incomplete.contents() == original,
          "incomplete existing record requires explicit repair and remains unchanged");
    incomplete.options.allow_repair = true;
    incomplete.on_verify = [&](const wifi::PairRecord &record) {
        check(record.host_identifier == legacy.host_identifier && incomplete.contents() == original,
              "repair preserves saved host identifier and leaves old file until verify succeeds");
    };
    result = incomplete.run();
    check(result.ok && !result.reused && incomplete.trace == std::vector<std::string>{"setup", "verify"},
          "explicit repair can replace a legacy record after verification");

    Fixture refused;
    refused.existing(synthetic_record());
    refused.verify_outcomes = {wifi::VerifyOutcome::NotPaired};
    result = refused.run();
    check(!result.ok && refused.setups == 0, "device rejection does not automatically rotate keys");
    Fixture repair;
    repair.existing(synthetic_record());
    repair.options.allow_repair = true;
    repair.verify_outcomes = {wifi::VerifyOutcome::NotPaired, wifi::VerifyOutcome::Paired};
    result = repair.run();
    check(result.ok && repair.trace == std::vector<std::string>{"verify", "setup", "verify"},
          "explicit repair verifies existing record then uses setup and fresh verify");

    Fixture transport_failure;
    transport_failure.existing(synthetic_record());
    const auto transport_original = transport_failure.contents();
    transport_failure.options.allow_repair = true;
    transport_failure.verify_outcomes = {wifi::VerifyOutcome::TransportFailure};
    result = transport_failure.run();
    check(!result.ok && transport_failure.setups == 0 && transport_failure.contents() == transport_original,
          "transport or identity verification failure cannot trigger repair even when allowed");
    Fixture failed_repair;
    failed_repair.existing(synthetic_record());
    const auto failed_original = failed_repair.contents();
    failed_repair.options.allow_repair = true;
    failed_repair.verify_outcomes = {wifi::VerifyOutcome::NotPaired, wifi::VerifyOutcome::TransportFailure};
    result = failed_repair.run();
    check(!result.ok && failed_repair.contents() == failed_original,
          "new repair record is not persisted when fresh verify fails");

    Fixture collision;
    collision.existing(synthetic_record("device-with/punctuation"));
    collision.options.allow_repair = true;
    const auto collision_original = collision.contents();
    result = collision.run();
    check(!result.ok && collision.setups == 0 && collision.verifies == 0 && collision.contents() == collision_original,
          "sanitized filename collision never overwrites a different original UDID");

    Fixture malformed;
    malformed.text("private-test-secret is not a record");
    const auto malformed_original = malformed.contents();
    result = malformed.run();
    check(!result.ok && malformed.setups == 0 && malformed.contents() == malformed_original &&
              result.error.find("private-test-secret") == std::string::npos,
          "malformed existing record is not overwritten or printed without explicit repair");
    malformed.options.allow_repair = true;
    result = malformed.run();
    check(result.ok && !result.reused, "explicit repair can replace malformed local record after setup and verify");
}

void test_early_failures() {
    for (const int timeout : {0, -1, 300001}) {
        remote::UsbPairingOptions invalid;
        invalid.timeout_ms = timeout;
        const auto result = remote::pair_usb_remote(invalid);
        check(!result.ok && !result.error.empty() && result.path.empty(), "invalid timeout rejects before device enumeration or record access");
    }
    Fixture no_host;
    const auto result = no_host.run("");
    check(!result.ok && no_host.setups == 0 && no_host.verifies == 0 && !std::filesystem::exists(no_host.path()),
          "new pairing fails before channel opening when hostname is unavailable");
    Fixture thrown;
    thrown.mutate_setup = [](wifi::PairSetupResult &) { throw std::runtime_error("private-test-secret"); };
    const auto exception_result = thrown.run();
    check(!exception_result.ok && exception_result.error.find("private-test-secret") == std::string::npos &&
              !std::filesystem::exists(thrown.path()),
          "callback exception does not publish a record or leak exception contents");
}
} // namespace

int main() {
    try {
        test_usb_selection();
        test_new_pairing();
        test_existing_records();
        test_early_failures();
    } catch (const std::exception &error) {
        ++failures;
        std::printf("FAIL: pairing workflow fixture: %s\n", error.what());
    }
    std::printf("pairing_workflow: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
