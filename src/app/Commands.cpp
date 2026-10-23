#include "app/Commands.h"
#include "app/Options.h"
#include "app/SdlRuntime.h"
#include "i18n/Translation.h"
#include <SDL.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <thread>

namespace scrctl::app {
using Json = xdb::client::Json;
using Response = xdb::client::Response;
namespace {
void event(const char *message, void *) {
    const auto value = Json::parse(message, nullptr, false);
    if (!value.is_object())
        return;
    const auto data = value.value("data", Json::object());
    if (value.value("event", std::string()) == "pin")
        std::fprintf(stderr, "Pairing code: %s\n", data.value("pin", std::string()).c_str());
    else if (data.contains("message"))
        std::fprintf(stderr, "%s\n", data.at("message").get<std::string>().c_str());
}
std::string absolute(const std::string &value) {
    if (value.empty())
        return {};
    auto path = std::filesystem::absolute(std::filesystem::path(std::u8string(value.begin(), value.end())))
                    .u8string();
    return std::string(path.begin(), path.end());
}
} // namespace
std::optional<int> run_standalone_command(const Options &o) {
    if (o.show_version) {
        std::printf("scrctl %s\n", SCRCTL_VERSION_STRING);
        return 0;
    }
    if (!o.list_devices && !o.pair && !o.download_ddi && !o.install_ddi && !o.list_apps && !o.copy_text &&
        !o.paste)
        return std::nullopt;
    SdlRuntime runtime;
    if (!runtime.initialize(SDL_INIT_TIMER)) {
        std::fprintf(stderr, "%s\n", SDL_GetError());
        return 1;
    }
    xdb::client::Client client;
    std::string error;
    if (!client.connect(error, 0, event)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    std::jthread monitor([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            if (runtime.stop_requested()) {
                client.interrupt();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    const auto call = [&](const char *method, const Json &params, Response &out) {
        const auto ok = client.request(method, params, out, error);
        if (!ok)
            std::fprintf(stderr, "%s\n", error.c_str());
        return ok;
    };
    Response out;
    if (o.list_devices) {
        if (!call("devices", {{"timeout_ms", o.discovery_timeout_ms}}, out))
            return runtime.stop_requested() ? 130 : 1;
        for (const auto &device : out.value.at("devices")) {
            std::printf("%s  %s\n", device.value("udid", std::string()).c_str(),
                        device.value("name", std::string()).c_str());
            for (const auto &candidate : device.at("candidates"))
                std::printf("  %s\n", candidate.dump().c_str());
        }
        for (const auto &warning : out.value.at("warnings"))
            std::fprintf(stderr, "%s\n", warning.get<std::string>().c_str());
        return 0;
    }
    if (o.pair) {
        if (!call("pair",
                  {{"udid", o.serial},
                   {"transport", o.wifi.empty() ? "usb" : "wifi"},
                   {"repair", o.repair_pairing},
                   {"timeout_ms", o.pairing_timeout_ms}},
                  out))
            return runtime.stop_requested() ? 130 : 1;
        std::printf("Pairing saved by xdb: %s\n", out.value.at("udid").get<std::string>().c_str());
        return 0;
    }
    if (o.install_ddi) {
        std::string serial = o.serial;
        if (serial.empty()) {
            if (!call(
                    "devices",
                    {{"transport", o.wifi.empty() ? "auto" : "wifi"}, {"timeout_ms", o.discovery_timeout_ms}},
                    out))
                return 1;
            const auto &devices = out.value.at("devices");
            if (devices.size() != 1 || devices[0].value("udid", std::string()).empty()) {
                std::fprintf(stderr, "Select one identified device with -s before preparing DDI\n");
                return 1;
            }
            serial = devices[0].at("udid");
        }
        if (!call("ddi.prepare",
                  {{"udid", serial},
                   {"wifi", o.wifi},
                   {"port", o.wifi_port},
                   {"cache_directory", absolute(o.ddi_directory)},
                   {"replace", o.replace_ddi},
                   {"download_timeout_seconds", o.ddi_download_timeout},
                   {"install_timeout_seconds", o.ddi_install_timeout}},
                  out))
            return runtime.stop_requested() ? 130 : 1;
        std::printf("DDI prepared by xdb\n");
        return 0;
    }
    if (o.download_ddi) {
        std::string version = o.ddi_system_version, model = o.ddi_product_type;
        if (version.empty()) {
            if (!call("connection.open", {{"udid", o.serial}, {"wifi", o.wifi}, {"port", o.wifi_port}},
                      out) ||
                !call("device.info", Json::object(), out))
                return 1;
            version = out.value.at("os_version");
            model = out.value.at("product_type");
            if (!call("connection.close", Json::object(), out))
                return 1;
        }
        if (!call("ddi.download",
                  {{"os_version", version},
                   {"product_type", model},
                   {"cache_directory", absolute(o.ddi_directory)},
                   {"timeout_seconds", o.ddi_download_timeout}},
                  out))
            return runtime.stop_requested() ? 130 : 1;
        std::printf("%s\n", out.value.dump().c_str());
        return 0;
    }
    if (!call("connection.open", {{"udid", o.serial}, {"wifi", o.wifi}, {"port", o.wifi_port}}, out))
        return 1;
    if (o.list_apps) {
        if (!call("app.list", Json::object(), out))
            return 1;
        for (const auto &app : out.value)
            std::printf("%s  %s\n", app.at("bundle_id").get<std::string>().c_str(),
                        app.at("name").get<std::string>().c_str());
    }
    if (o.copy_text && !call("clipboard.set", {{"text", *o.copy_text}}, out))
        return 1;
    if (o.paste) {
        if (!call("clipboard.get", Json::object(), out))
            return 1;
        std::printf("%s\n", out.value.at("text").get<std::string>().c_str());
    }
    return 0;
}
int launch_app(xdb::client::Client &client, const std::string &app_spec) {
    std::string spec = app_spec;
    bool by_name = false, terminate = false;
    while (!spec.empty() && (spec.front() == '?' || spec.front() == '+')) {
        if (spec.front() == '?')
            by_name = true;
        else
            terminate = true;
        spec.erase(spec.begin());
    }
    if (spec.empty())
        return 2;
    std::string target = spec, error;
    if (by_name) {
        Response apps;
        if (!client.request("app.list", Json::object(), apps, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        auto lower = [](std::string v) {
            for (auto &c : v)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return v;
        };
        bool found = false;
        for (const auto &app : apps.value)
            if (lower(app.at("name")).starts_with(lower(spec))) {
                target = app.at("bundle_id");
                found = true;
                break;
            }
        if (!found) {
            std::fprintf(stderr, "No app name starts with %s\n", spec.c_str());
            return 1;
        }
    }
    if (!client.request("app.launch", {{"bundle_id", target}, {"terminate_existing", terminate}}, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    return 0;
}
} // namespace scrctl::app
