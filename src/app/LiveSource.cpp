#include "app/LiveSource.h"
#include "app/SourcePick.h"
#include "i18n/Translation.h"
#include <SDL.h>
#include <cstdio>
#include <filesystem>

namespace scrctl::app {
using Json = xdb::client::Json;
using Response = xdb::client::Response;
LiveSource::~LiveSource() {
    std::string ignored;
    finish_recording(ignored);
}
bool LiveSource::attached(xdb::client::Client &client, std::string &error) {
    return client.connect(error) && client.request("connection.attach", {{"attachment", attachment_}}, error);
}
bool LiveSource::start(const Options &options, std::string &error) {
    options_ = options;
    want_video_ = options.want_video;
    decode_video_ = options.decode_video;
    if (!options.test_degrade.empty()) {
        std::vector<uint64_t> marks;
        if (!parse_degrade_marks(options.test_degrade, marks, error))
            return false;
    }
    if (options.should_cancel && options.should_cancel()) {
        error = "Cancelled";
        return false;
    }
    if (!control_.connect(error))
        return false;
    std::jthread monitor([&](std::stop_token stop) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!stop.stop_requested()) {
            if ((options.should_cancel && options.should_cancel()) ||
                std::chrono::steady_clock::now() >= deadline) {
                control_.interrupt();
                frames_.interrupt();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    Response opened;
    if (!control_.request("connection.open",
                          {{"udid", options.serial}, {"wifi", options.wifi}, {"port", options.wifi_port}},
                          opened, error))
        return false;
    attachment_ = opened.value.at("attachment").get<std::string>();
    auto container = record_container_format(options.record_path, options.record_format);
    if (container) {
        auto path = std::filesystem::absolute(std::filesystem::path(std::u8string(options.record_path.begin(),
                                                                                  options.record_path.end())))
                        .u8string();
        if (!control_.request("recording.configure",
                              {{"path", std::string(path.begin(), path.end())},
                               {"format", *container == RecordFormat::Mp4 ? "mp4" : "mkv"},
                               {"video", options.want_video},
                               {"audio", options.want_audio},
                               {"orientation", options.record_orientation},
                               {"orientation_from_device", options.record_orientation_from_device}},
                              error))
            return false;
    }
    if (want_video_) {
        if (options.video_source != "screenshot") {
            Json params{{"hardware_decode", options.hw_decode}, {"decode", options.decode_video}};
            if (!container && !options.record_path.empty()) {
                auto path =
                    std::filesystem::absolute(std::filesystem::path(std::u8string(options.record_path.begin(),
                                                                                  options.record_path.end())))
                        .u8string();
                params["record_path"] = std::string(path.begin(), path.end());
            }
            if (!control_.request("video.start", params, error)) {
                if (!decode_video_ || error.find("requires iOS") == std::string::npos)
                    return false;
                options_.video_source = "screenshot";
            }
        }
        if (decode_video_) {
            if (!attached(frames_, error))
                return false;
            receiver_ = std::jthread([this](std::stop_token stop) { receive(stop); });
            std::unique_lock lock(mutex_);
            if (!ready_.wait_for(lock, std::chrono::seconds(5),
                                 [this] { return serial_ || ended_.load(); }) ||
                !serial_) {
                error = error_.empty() ? "No first frame from xdb within five seconds" : error_;
                return false;
            }
        }
    }
    if (options.want_audio && options_.video_source != "screenshot") {
        Response info;
        if (control_.request("audio.start",
                             {{"decode", options.decode_audio},
                              {"duplicate", options.audio_dup},
                              {"buffer_ms", options.audio_buffer_ms}},
                             info, error)) {
            audio_enabled_ = true;
            if (options.decode_audio) {
                audio_ = std::make_unique<RemoteAudio>();
                if (!audio_->start(attachment_, info.value.at("sample_rate"), info.value.at("channels"),
                                   info.value.at("preroll"), error))
                    return false;
            }
        } else {
            if (!want_video_ || container)
                return false;
            std::fprintf(stderr, "Audio unavailable: %s (video continues)\n", error.c_str());
            error.clear();
        }
    }
    monitor.request_stop();
    monitor.join();
    if (!receiver_.joinable()) {
        if (!attached(frames_, error))
            return false;
        receiver_ = std::jthread([this](std::stop_token stop) { receive(stop); });
    }
    return true;
}
void LiveSource::fail(std::string error) {
    std::lock_guard lock(mutex_);
    error_ = std::move(error);
    ended_ = true;
    ready_.notify_all();
}
void LiveSource::receive(std::stop_token stop) {
    uint64_t video_serial = 0, status_at = 0;
    bool screenshot = options_.video_source == "screenshot";
    std::vector<uint64_t> marks;
    std::string ignored;
    parse_degrade_marks(options_.test_degrade, marks, ignored);
    const auto started = SDL_GetTicks64();
    size_t mark = 0;
    bool forced = false;
    while (!stop.stop_requested()) {
        const auto now = SDL_GetTicks64();
        while (mark < marks.size() && now - started >= marks[mark]) {
            forced = !forced;
            ++mark;
        }
        if (now >= status_at) {
            Response status;
            std::string error;
            if (!frames_.request("media.status", Json::object(), status, error)) {
                if (!stop.stop_requested())
                    fail(error);
                return;
            }
            error = status.value.value("error", std::string());
            if (!error.empty()) {
                fail(error);
                return;
            }
            const auto recording_error = status.value.value("recording_error", std::string());
            if (!recording_error.empty() && (!want_video_ || !decode_video_)) {
                fail(recording_error);
                return;
            }
            screenshot = options_.video_source == "screenshot" || forced ||
                         status.value.value("video_unusable", false);
            status_at = now + 1000;
        }
        if (!want_video_ || !decode_video_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        Response response;
        std::string error;
        bool ok = screenshot ? frames_.request("screenshot", {{"format", "bgra"}}, response, error)
                             : frames_.request("video.frame", {{"since", video_serial}, {"timeout_ms", 50}},
                                               response, error);
        if (!ok) {
            if (error.find("socket") != std::string::npos || response.code == "not_connected" ||
                response.code == "transport_error") {
                if (!stop.stop_requested())
                    fail(error);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(screenshot ? 100 : 5));
            continue;
        }
        if (!screenshot)
            video_serial = response.value.at("serial");
        auto w = response.value.value("width", 0), h = response.value.value("height", 0),
             pitch = response.value.value("row_pitch", 0);
        if (w <= 0 || h <= 0 || int64_t(pitch) < int64_t(w) * 4 ||
            response.data.size() != uint64_t(pitch) * h) {
            fail("Invalid xdb image dimensions");
            return;
        }
        scrctl::Frame frame;
        frame.width = w;
        frame.height = h;
        frame.row_pitch = pitch;
        frame.pixels = std::move(response.data);
        auto display = response.value.value("display", Json::object());
        FrameGeometry geometry{display.value("width", 0), display.value("height", 0),
                               parse_orientation_degrees(display.value("orientation", std::string())),
                               screenshot};
        {
            std::lock_guard lock(mutex_);
            latest_ = std::move(frame);
            latest_geometry_ = geometry;
            ++serial_;
        }
        ready_.notify_all();
    }
}
bool LiveSource::next(scrctl::Frame &frame, int timeout) {
    std::unique_lock lock(mutex_);
    if (!ready_.wait_for(lock, std::chrono::milliseconds(timeout),
                         [this] { return serial_ > delivered_ || ended_.load(); }) ||
        serial_ == delivered_)
        return false;
    frame = latest_;
    delivered_geometry_ = latest_geometry_;
    delivered_ = serial_;
    return true;
}
std::string LiveSource::end_reason() const {
    std::lock_guard lock(mutex_);
    return error_;
}
bool LiveSource::start_playback(std::string &error) { return audio_ && audio_out_.open(*audio_, error); }
void LiveSource::abandon_audio() {
    audio_out_.close();
    audio_.reset();
    audio_enabled_ = false;
    std::string ignored;
    control_.request("audio.stop", Json::object(), ignored);
}
bool LiveSource::finish_recording(std::string &error) {
    if (!finished_recording_) {
        receiver_.request_stop();
        frames_.interrupt();
        if (receiver_.joinable())
            receiver_.join();
        frames_.close();
        audio_out_.close();
        audio_.reset();
        if (control_.connected()) {
            finish_ok_ = control_.request("media.finish", Json::object(), finish_error_);
            std::string ignored;
            control_.request("connection.close", Json::object(), ignored);
            control_.close();
        }
        finished_recording_ = true;
    }
    error = finish_error_;
    return finish_ok_;
}
bool LiveSource::control(double x, double y, bool down, std::string &e) {
    return control_.request("input.touch", {{"x", x}, {"y", y}, {"down", down}}, e);
}
bool LiveSource::keyboard_state(const std::vector<uint16_t> &usages, std::string &e) {
    return control_.request("input.keyboard", {{"usages", usages}}, e);
}
bool LiveSource::type_text(const std::string &text, int hold, std::string &e) {
    return control_.request("input.text", {{"text", text}, {"hold_ms", hold}}, e);
}
bool LiveSource::button_state(uint16_t page, uint16_t code, bool down, std::string &e) {
    return control_.request("input.button", {{"page", page}, {"usage", code}, {"down", down}}, e);
}
bool LiveSource::button(uint16_t page, uint16_t code, std::string &e) {
    if (!button_state(page, code, true, e))
        return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(90));
    return button_state(page, code, false, e);
}
bool LiveSource::job(const char *method, const Json &params, Response &out, std::string &e,
                     std::stop_token stop) {
    xdb::client::Client client;
    if (!attached(client, e))
        return false;
    std::stop_callback cancel(stop, [&] { client.interrupt(); });
    return !stop.stop_requested() && client.request(method, params, out, e);
}
bool LiveSource::clipboard_set(const std::string &text, std::string &e, std::stop_token stop) {
    Response out;
    return job("clipboard.set", {{"text", text}}, out, e, stop);
}
bool LiveSource::clipboard_get(std::string &text, std::string &e, std::stop_token stop) {
    Response out;
    if (!job("clipboard.get", Json::object(), out, e, stop))
        return false;
    text = out.value.at("text");
    return true;
}
bool LiveSource::rotate(std::string &target, std::string &e, std::stop_token stop) {
    Response out;
    if (!job("display.rotate", Json::object(), out, e, stop))
        return false;
    target = out.value.at("target");
    return true;
}
bool LiveSource::debug_network(bool enabled, std::string &e) {
    return control_.request("network.debug", {{"enabled", enabled}}, e);
}
void LiveSource::print_stats() {
    std::lock_guard lock(mutex_);
    std::printf("xdb frames=%llu delivered=%llu\n", static_cast<unsigned long long>(serial_),
                static_cast<unsigned long long>(delivered_));
}
} // namespace scrctl::app
