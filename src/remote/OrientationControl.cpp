#include "remote/OrientationControl.h"

#include "i18n/Translation.h"
#include "remote/Device.h"

namespace scrctl::remote {
namespace {

xpc::Value envelope(xpc::Value payload) {
    auto result = xpc::make_dict();
    xpc::dict_set(result, "featureIdentifier", xpc::make_string(std::string(OrientationControl::kFeature)));
    xpc::dict_set(result, "messageType", xpc::make_string("OrientationRequest"));
    xpc::dict_set(result, "payload", std::move(payload));
    return result;
}

std::optional<DeviceOrientation> parse_orientation(const xpc::Value &value) {
    if (!value.is_string()) return std::nullopt;
    for (const auto orientation : {DeviceOrientation::Unknown, DeviceOrientation::Portrait,
                                  DeviceOrientation::PortraitUpsideDown,
                                  DeviceOrientation::LandscapeLeft, DeviceOrientation::LandscapeRight,
                                  DeviceOrientation::FaceUp, DeviceOrientation::FaceDown}) {
        if (value.string == OrientationControl::name(orientation)) return orientation;
    }
    return std::nullopt;
}

bool cancelled(std::stop_token cancel, std::string &err) {
    if (!cancel.stop_requested()) return false;
    err = SCRCTL_TR("Device orientation request cancelled");
    return true;
}

} // namespace

std::string_view OrientationControl::name(DeviceOrientation orientation) {
    switch (orientation) {
        case DeviceOrientation::Unknown: return "unknown";
        case DeviceOrientation::Portrait: return "portrait";
        case DeviceOrientation::PortraitUpsideDown: return "portraitUpsideDown";
        case DeviceOrientation::LandscapeLeft: return "landscapeLeft";
        case DeviceOrientation::LandscapeRight: return "landscapeRight";
        case DeviceOrientation::FaceUp: return "faceUp";
        case DeviceOrientation::FaceDown: return "faceDown";
    }
    return {};
}

bool OrientationControl::cardinal(DeviceOrientation orientation) {
    return orientation == DeviceOrientation::Portrait ||
           orientation == DeviceOrientation::PortraitUpsideDown ||
           orientation == DeviceOrientation::LandscapeLeft ||
           orientation == DeviceOrientation::LandscapeRight;
}

std::optional<DeviceOrientation> OrientationControl::effective_cardinal(const OrientationState &state) {
    if (cardinal(state.current)) return state.current;
    if (cardinal(state.nonflat)) return state.nonflat;
    return std::nullopt;
}

xpc::Value OrientationControl::build_query() {
    auto payload = xpc::make_dict();
    xpc::dict_set(payload, "currentOrientation", xpc::make_dict());
    return envelope(std::move(payload));
}

std::optional<xpc::Value> OrientationControl::build_change(DeviceOrientation target) {
    if (!cardinal(target)) return std::nullopt;
    auto argument = xpc::make_dict();
    xpc::dict_set(argument, "_0", xpc::make_string(std::string(name(target))));
    auto payload = xpc::make_dict();
    xpc::dict_set(payload, "changeOrientation", std::move(argument));
    return envelope(std::move(payload));
}

std::optional<OrientationState> OrientationControl::parse_state(const xpc::Value &reply) {
    if (!reply.is_dict() || reply.dict.size() != 3) return std::nullopt;
    const auto *current = reply.find("currentDeviceOrientation");
    const auto *nonflat = reply.find("currentDeviceNonFlatOrientation");
    const auto *locked = reply.find("currentDeviceOrientationLocked");
    if (!current || !nonflat || !locked || locked->type != xpc::Type::Bool) return std::nullopt;
    const auto current_orientation = parse_orientation(*current);
    const auto nonflat_orientation = parse_orientation(*nonflat);
    if (!current_orientation || !nonflat_orientation) return std::nullopt;
    return OrientationState{*current_orientation, *nonflat_orientation, locked->boolean};
}

std::unique_ptr<OrientationControl> OrientationControl::connect(Device &device, std::string &err,
                                                               bool verbose, std::stop_token cancel) {
    if (cancelled(cancel, err)) return nullptr;
    const auto service = device.rsd().service(kService);
    if (!service || !service->uses_remote_xpc || service->port == 0 ||
        !device.rsd().supports(kService, kFeature)) {
        err = SCRCTL_TR("Device orientation control is unavailable");
        return nullptr;
    }
    auto connection = device.connect(kService, err, verbose, cancel);
    if (!connection || cancelled(cancel, err)) return nullptr;
    return std::unique_ptr<OrientationControl>(new OrientationControl(std::move(connection), cancel));
}

CallResult OrientationControl::exchange(const xpc::Value &request, OrientationState &out,
                                        std::string &err, int timeout_ms) {
    if (cancelled(cancel_, err)) return CallResult::TransportError;
    if (timeout_ms <= 0) {
        err = SCRCTL_TR("Device orientation reply timeout must be positive");
        return CallResult::DeviceError;
    }
    xpc::Value reply;
    if (!connection_->call(request, reply, timeout_ms, err)) {
        cancelled(cancel_, err);
        return CallResult::TransportError;
    }
    if (cancelled(cancel_, err)) return CallResult::TransportError;
    const auto state = parse_state(reply);
    if (!state) {
        err = SCRCTL_TR("Unrecognized device orientation response");
        return CallResult::DeviceError;
    }
    out = *state;
    return CallResult::Ok;
}

CallResult OrientationControl::query(OrientationState &out, std::string &err, int timeout_ms) {
    return exchange(build_query(), out, err, timeout_ms);
}

CallResult OrientationControl::change(DeviceOrientation target, OrientationState &out,
                                      std::string &err, int timeout_ms) {
    if (cancelled(cancel_, err)) return CallResult::TransportError;
    const auto request = build_change(target);
    if (!request) {
        err = SCRCTL_TR("Device orientation target must be portrait or landscape");
        return CallResult::DeviceError;
    }
    return exchange(*request, out, err, timeout_ms);
}

} // namespace scrctl::remote
