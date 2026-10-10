#pragma once

#include "remote/Pairing.h"
#include "wifi/PairRecord.h"
#include "wifi/PairVerify.h"

namespace scrctl::remote {

struct WifiPairingOptions {
    std::string udid;  ///< Optional expected device UDID; checked before saving.
    std::string pairing_directory;
    /// Cooperative total deadline including discovery, PIN and verification.
    /// Socket waits check it every 50ms; callbacks must return promptly.
    /// Filesystem calls cannot be preempted; expiry is checked before publishing.
    int timeout_ms = 120000;
    std::function<bool()> should_cancel;
    std::function<void(std::string_view)> progress;
    /// Display the one-use six-digit PIN after the phone starts pairing.
    /// It is not a diagnostic message and must not be saved in logs.
    std::function<bool(std::string_view, std::string &)> display_pin;
};

/// Advertise a temporary computer on the LAN, accept one phone-initiated PIN
/// pairing, reconnect to authenticate the signed device identity, then save.
/// Requires the device's Privacy & Security / Developer Mode pairing UI. Does not start media
/// or use USB. Existing record files are never replaced, even on a concurrent
/// write. The phone may have registered trust before a later step fails.
PairingResult pair_wifi_remote(const WifiPairingOptions &options = {});

namespace detail {
/// Persistence gate used by the production listener after authenticated M5/M6.
/// Verification is deliberately a new connection, not the setup carrier.
PairingResult save_wifi_pairing(const wifi::PairRecord &record,
                               const WifiPairingOptions &options,
                               const std::function<wifi::PairVerifyResult(
                                   const wifi::PairRecord &, std::string &)> &verify);
} // namespace detail
} // namespace scrctl::remote
