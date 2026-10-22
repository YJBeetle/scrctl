#pragma once
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <xdb/client.hpp>

namespace scrctl::app {
class RemoteAudio {
  public:
    ~RemoteAudio() { stop(); }
    bool start(const std::string &attachment, int rate, int channels, size_t /*preroll*/,
               std::string &error) {
        rate_ = rate;
        channels_ = channels;
        preroll_ = 960; // The server owns the requested jitter buffer; keep only one IPC chunk locally.
        if (!client_.connect(error) ||
            !client_.request("connection.attach", {{"attachment", attachment}}, error))
            return false;
        worker_ = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested()) {
                if (buffered_frames() < std::max(preroll_, size_t(960))) {
                    xdb::client::Response out;
                    std::string error;
                    if (!client_.request("audio.read", {{"frames", 960}}, out, error))
                        break;
                    std::lock_guard lock(mutex_);
                    for (size_t i = 0; i + 1 < out.data.size(); i += 2)
                        pcm_.push_back(
                            static_cast<int16_t>(uint16_t(out.data[i]) | uint16_t(out.data[i + 1]) << 8));
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
        return true;
    }
    void stop() {
        worker_.request_stop();
        client_.interrupt();
        if (worker_.joinable())
            worker_.join();
        client_.close();
    }
    int channels() const { return channels_; }
    int sample_rate() const { return rate_; }
    bool decoding_enabled() const { return true; }
    size_t preroll_frames() const { return preroll_; }
    size_t buffered_frames() const {
        std::lock_guard lock(mutex_);
        return pcm_.size() / channels_;
    }
    size_t read(int16_t *dst, size_t frames) {
        std::lock_guard lock(mutex_);
        const auto count = std::min(frames, pcm_.size() / channels_) * channels_;
        for (size_t i = 0; i < count; ++i) {
            dst[i] = pcm_.front();
            pcm_.pop_front();
        }
        return count / channels_;
    }

  private:
    xdb::client::Client client_;
    std::jthread worker_;
    mutable std::mutex mutex_;
    std::deque<int16_t> pcm_;
    int rate_ = 48000, channels_ = 2;
    size_t preroll_ = 2400;
};
} // namespace scrctl::app
