#pragma once
#include "app/AudioOut.h"
#include "app/FrameSource.h"
#include "app/RecordFormat.h"
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <xdb/client.hpp>

namespace scrctl::app {
class LiveSource final : public FrameSource {
  public:
    ~LiveSource() override;
    struct Options {
        std::string serial, wifi, record_path;
        std::optional<RecordFormat> record_format;
        uint16_t wifi_port = 49152;
        bool hw_decode = false;
        bool watch_display = true;
        bool want_video = true;
        bool decode_video = true;
        bool want_audio = true;
        bool decode_audio = true;
        int audio_buffer_ms = 50;
        bool audio_dup = false;
        std::string video_source = "stream";
        std::string test_degrade;
        int record_orientation = 0;
        bool record_orientation_from_device = false; ///< 起录时取设备方向，仅写固定容器元数据。
        std::function<bool()> should_cancel;
    };

    bool start(const Options &, std::string &error);
    bool start_playback(std::string &error);
    void abandon_audio();
    bool finish_recording(std::string &error);
    bool next(scrctl::Frame &, int timeout_ms) override;
    bool finished() const override { return ended_.load(); }
    bool failed() const override { return ended_.load(); }
    std::string end_reason() const override;
    FrameGeometry frame_geometry() const override { return delivered_geometry_; }
    bool has_audio() const { return audio_enabled_; }
    bool has_video() const { return want_video_; }
    bool video_decoding_enabled() const { return want_video_ && decode_video_; }
    bool control(double x, double y, bool down, std::string &error);
    bool keyboard_state(const std::vector<uint16_t> &usages, std::string &error);
    bool type_text(const std::string &, int hold_ms, std::string &error);
    bool button_state(uint16_t page, uint16_t code, bool down, std::string &error);
    bool button(uint16_t page, uint16_t code, std::string &error);
    bool clipboard_set(const std::string &, std::string &error, std::stop_token = {});
    bool clipboard_get(std::string &, std::string &error, std::stop_token = {});
    bool rotate(std::string &target, std::string &error, std::stop_token = {});
    bool debug_network(bool enabled, std::string &error);
    xdb::client::Client &client() { return control_; }
    void print_stats() override;

  private:
    void receive(std::stop_token);
    bool attached(xdb::client::Client &, std::string &error);
    bool job(const char *, const xdb::client::Json &, xdb::client::Response &, std::string &,
             std::stop_token);
    void fail(std::string);
    xdb::client::Client control_, frames_;
    std::string attachment_;
    Options options_;
    std::unique_ptr<RemoteAudio> audio_;
    AudioOut audio_out_;
    bool audio_enabled_ = false, want_video_ = true, decode_video_ = true, finished_recording_ = false;
    bool finish_ok_ = true;
    std::string finish_error_;
    std::jthread receiver_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    scrctl::Frame latest_;
    FrameGeometry latest_geometry_, delivered_geometry_;
    uint64_t serial_ = 0, delivered_ = 0;
    std::string error_;
    std::atomic<bool> ended_{false};
};
} // namespace scrctl::app
