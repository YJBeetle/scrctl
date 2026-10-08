#include "decode/FFmpegEldDecoder.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <vector>

namespace {
int checks = 0;
int failures = 0;

void check(bool ok, const char *message) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", message);
    }
}

void test_configuration() {
    for (const auto &[rate, channels, frame_length] : {
             std::array{0, 2, 480}, std::array{-1, 2, 480}, std::array{44100, 2, 480},
             std::array{48000, 0, 480}, std::array{48000, 1, 480}, std::array{48000, 6, 480},
             std::array{48000, 2, 0}, std::array{48000, 2, 1024},
         }) {
        std::string err;
        check(!scrctl::create_ffmpeg_eld_decoder(rate, channels, frame_length, err) && !err.empty(),
              "unverified ELD configuration is rejected with a reason");
    }
}

void test_decode(int frame_length) {
    std::string err = "previous error";
    auto decoder = scrctl::create_ffmpeg_eld_decoder(48000, 2, frame_length, err);
    check(decoder != nullptr && err.empty(), "verified configuration creates decoder and clears error");
    if (!decoder) {
        return;
    }
    check(std::string(decoder->backend_name()) == "libavcodec/aac-eld", "backend identity is explicit");
    std::vector<int16_t> pcm = {123, -456};
    err = "previous error";
    check(decoder->decode({}, pcm, err) && pcm == std::vector<int16_t>({123, -456}) && err.empty(),
          "empty input is a no-op without stale error");

    // 设备静音时观察到的完整裸载荷（CoreDevice §17）。它不含 RTP 头。
    // 这一样本验证配置、尺寸与静音；不能证明真实有声音频也解码正确。
    constexpr std::array<uint8_t, 4> silence = {0x00, 0x68, 0x34, 0x00};
    check(decoder->decode(silence, pcm, err) && err.empty(), "observed silence payload decodes");
    check(pcm.size() == static_cast<size_t>(frame_length * 2 + 2),
          "ELD output has configured per-channel frame length");
    check(pcm[0] == 123 && pcm[1] == -456, "decode appends without replacing caller PCM");
    check(std::all_of(pcm.begin() + 2, pcm.end(), [](int16_t n) { return n == 0; }),
          "observed silence yields zero-valued PCM");

    const auto before_error = pcm;
    constexpr std::array<uint8_t, 1> truncated = {0xff};
    check(!decoder->decode(truncated, pcm, err) && !err.empty(),
          "truncated invalid frame reports a codec error");
    check(pcm == before_error, "codec error does not leave partial PCM");
    check(decoder->decode(silence, pcm, err) && err.empty() &&
              pcm.size() == before_error.size() + static_cast<size_t>(frame_length * 2),
          "decoder recovers after an invalid frame");

    const auto before_limit = pcm;
    std::vector<uint8_t> too_big(scrctl::kMaxEldPacketBytes + 1, 0);
    check(!decoder->decode(too_big, pcm, err) && !err.empty() && pcm == before_limit,
          "oversized single-frame input is rejected without changing PCM");
    check(decoder->decode(silence, pcm, err) && err.empty(),
          "size rejection does not disrupt next valid frame");

    bool all_good = true;
    for (int i = 0; i < 100; ++i) {
        pcm.clear();
        if (!decoder->decode(silence, pcm, err) || !err.empty() ||
            pcm.size() != static_cast<size_t>(frame_length * 2) ||
            !std::all_of(pcm.begin(), pcm.end(), [](int16_t n) { return n == 0; })) {
            all_good = false;
            break;
        }
    }
    check(all_good, "reused decoder preserves size and silence across 100 frames");
}
} // namespace

int main() {
    test_configuration();
    test_decode(480);
    test_decode(512);
    std::printf("audio_decoder: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
