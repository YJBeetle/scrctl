#pragma once

#include "decode/Decoder.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace scrctl::media {

// A serial, fixed-canvas HEVC encoder for explicitly normalized recordings.
// CPU BGRA pixels are cropped, rotated clockwise, fitted with black bars, then
// encoded. It never substitutes a raw/unrotated recording if encoding fails.
class RecordingVideoEncoder {
public:
    struct Options {
        uint32_t width = 0, height = 0;
        int clock_rate = 24000;
        int64_t bit_rate = 8000000;
        bool prefer_hardware = true;
    };
    struct Crop {
        uint32_t x = 0, y = 0, width = 0, height = 0;
    };
    struct Packet {
        int64_t pts_ticks = 0;
        std::vector<Nal> nals;
        Nal vps, sps, pps;
    };
    // Availability means an encoder is registered; create also validates that
    // a concrete encoder session can actually be opened for this canvas.
    static bool available(std::string& error);
    static std::unique_ptr<RecordingVideoEncoder> create(const Options&, std::string& error);
    ~RecordingVideoEncoder();
    RecordingVideoEncoder(const RecordingVideoEncoder&) = delete;
    RecordingVideoEncoder& operator=(const RecordingVideoEncoder&) = delete;
    const char* backend_name() const;
    // Zero crop width AND height mean the full frame. PTS is in clock_rate units
    // and must strictly increase. Outputs retain encoder PTS, without synthesis.
    bool encode(const Frame&, int clockwise, Crop, int64_t pts_ticks,
                std::vector<Packet>& output, std::string& error);
    bool finish(std::vector<Packet>& output, std::string& error);
private:
    struct Impl;
    explicit RecordingVideoEncoder(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};

} // namespace scrctl::media
