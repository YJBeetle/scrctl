#include "i18n/Translation.h"
#include "wifi/RemotePairing.h"

#include <unistd.h>

#include "wifi/PairRecord.h"

namespace scrctl::wifi {

std::optional<uint16_t> request_tcp_listener(Rppairing &channel, const Bytes &tunnel_key,
                                             std::string &err) {
    const json::Value connection =
        j_obj({{"owningPID", j_int(static_cast<int64_t>(::getpid()))},
               {"owningProcessName", j_str("CoreDeviceService")}});
    const json::Value listener =
        j_obj({{"key", j_str(b64_encode(tunnel_key))},
               {"peerConnectionsInfo", j_arr({connection})},
               {"transportProtocolType", j_str("tcp")}});
    const json::Value request =
        j_obj({{"request", j_obj({{"_0", j_obj({{"createListener", listener}})}})}});

    const std::optional<json::Value> reply = channel.encrypted_roundtrip(request, err);
    if (!reply) {
        return std::nullopt;
    }
    const json::Value *created = json::find(*reply, "createListener");
    const json::Value *port = created != nullptr ? json::find(*created, "port") : nullptr;
    if (port == nullptr) {
        // 把实际收到的键打出来：设备在"配好了但不肯给你隧道"的时候回的东西是有名字的，
        // 只说"没有 port"的话，下一次还是要从头猜。
        std::string keys;
        for (const auto &entry : reply->items()) {
            keys += " " + entry.key();
        }
        err = SCRCTL_TR("createListener response missing port (actual fields:") + keys + SCRCTL_TR(")");
        return std::nullopt;
    }
    const int64_t value = json::as_int_or(*port, 0);
    if (value <= 0 || value > 0xFFFF) {
        err = SCRCTL_TR("createListener returned an invalid port: ") + std::to_string(value);
        return std::nullopt;
    }
    return static_cast<uint16_t>(value);
}

}  // namespace scrctl::wifi
