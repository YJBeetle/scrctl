#include "media/RecordingMuxer.h"
#include "app/RecordFormat.h"
#include "RecordingAudioEvidence.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef SCRCTL_HAVE_LIBAVFORMAT
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libavutil/mathematics.h>
#include <libavutil/version.h>
}
#endif

namespace {
using Muxer = scrctl::media::RecordingMuxer;
using scrctl::Nal;
int checks = 0;
int failures = 0;

void check(bool value, const char* message) {
    ++checks;
    if (!value) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

Nal hex(std::string_view text) {
    Nal bytes;
    for (std::size_t i = 0; i < text.size(); i += 2) {
        unsigned byte = 0;
        std::sscanf(text.data() + i, "%2x", &byte);
        bytes.push_back(static_cast<uint8_t>(byte));
    }
    return bytes;
}

// FFmpeg 9.0.1 / x265 4.3+1-e9b8812 build 217：64x64 黑色一帧，30fps，
// ultrafast / bframes=0 / keyint=25 / repeat-headers=1 / info=0 / pools=1。
// 这里只保存原始 VPS/SPS/PPS 和 12 字节 IDR，CI 不需要运行 x265。
const Nal vps = hex("40010c01ffff01600000030090000003000003001eba0240");
const Nal sps = hex("42010101600000030090000003000003001ea020810596e92930bc05a020000003002000000303c1");
const Nal pps = hex("4401c073c089");
const Nal idr = hex("000000012801ac21800e7ffeebf349ac");
// 与 audio_decoder_test 相同的原始双声道静音 AAC-ELD 包；ASC 决定 480/512。
const Nal silence{0x00, 0x68, 0x34, 0x00};

struct Directory {
    std::filesystem::path path;
    Directory() {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("scrctl-recording-muxer-test-" + std::to_string(tick));
        std::filesystem::create_directory(path);
    }
    ~Directory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

Muxer::Options options(const std::filesystem::path& file, Muxer::Format format,
                       std::optional<Muxer::Audio> audio = std::nullopt) {
    Muxer::Options result;
    result.path = file.string();
    result.format = format;
    result.vps = vps;
    result.sps = sps;
    result.pps = pps;
    result.audio = audio;
    return result;
}

Muxer::Options audio_options(const std::filesystem::path& file, Muxer::Format format) {
    Muxer::Options result;
    result.path = file.string();
    result.format = format;
    result.include_video = false;
    result.audio = Muxer::Audio{};
    return result;
}

void selected_tracks_precheck(const Directory& directory) {
    const auto path = directory.path / "selected-tracks-must-not-truncate";
    { std::ofstream file(path); file << "keep"; }
    std::string error;
    auto config = audio_options(path, Muxer::Format::Mp4);
    config.audio.reset();
    check(!Muxer::open(config, error) && error.find("selected track") != std::string::npos,
          "a muxer with no selected track is rejected before opening the file");
    config = audio_options(path, Muxer::Format::Mp4);
    config.video_orientation = 90;
    check(!Muxer::open(config, error) && error.find("zero video orientation") != std::string::npos,
          "an audio-only file refuses meaningless video orientation");
    for (const auto member : {&Muxer::Options::vps, &Muxer::Options::sps, &Muxer::Options::pps}) {
        config = audio_options(path, Muxer::Format::Mp4);
        config.*member = Nal{0};
        check(!Muxer::open(config, error) && error.find("HEVC parameter") != std::string::npos,
              "an audio-only file refuses each unused HEVC parameter instead of silently ignoring it");
    }
    std::ifstream file(path); std::string bytes; file >> bytes;
    check(bytes == "keep", "invalid track selections never truncate an existing output");
}

void orientation_precheck(const Directory& directory) {
    std::string error = "old error";
    for (const auto format : {Muxer::Format::Mp4, Muxer::Format::Matroska}) {
        check(Muxer::validate_video_orientation(format, 0, error) && error.empty(),
              "zero orientation needs no display metadata capability");
    }
    const auto path = directory.path / "orientation-must-not-truncate";
    { std::ofstream file(path); file << "keep"; }
    for (const int degrees : {-90, 1, 360, std::numeric_limits<int>::min(),
                               std::numeric_limits<int>::max()}) {
        check(!Muxer::validate_video_orientation(Muxer::Format::Mp4, degrees, error) && !error.empty(),
              "pure orientation precheck rejects unsupported angles");
        auto config = options(path, Muxer::Format::Mp4);
        config.video_orientation = degrees;
        check(!Muxer::open(config, error) && !error.empty(), "open defensively rejects unsupported orientation");
        std::ifstream file(path); std::string text; file >> text;
        check(text == "keep", "invalid orientation cannot truncate an existing file");
    }
    check(!Muxer::validate_video_orientation(static_cast<Muxer::Format>(99), 0, error) && !error.empty(),
          "orientation precheck rejects an invalid container enum even at zero degrees");
    auto invalid = options(directory.path / "orientation-not-created", Muxer::Format::Mp4);
    invalid.video_orientation = 45;
    check(!Muxer::open(invalid, error) && !std::filesystem::exists(invalid.path),
          "invalid orientation is rejected before creating a file");
}

#ifdef SCRCTL_HAVE_LIBAVFORMAT
struct Packet {
    int64_t pts = 0, dts = 0, duration = 0;
    Nal bytes;
    bool key = false;
    recording_test::AudioPacketEvidence audio_evidence;
};
struct Track {
    AVCodecID codec = AV_CODEC_ID_NONE;
    unsigned tag = 0;
    int width = 0, height = 0, sample_rate = 0, channels = 0;
    Nal extradata;
    std::optional<int> clockwise_orientation;
    std::vector<Packet> packets;
};
struct File {
    Track video, audio;
    std::string format_name;
    unsigned streams = 0;
    recording_test::AudioTailEvidence audio_tail;
};

File read(const std::filesystem::path& path) {
    File result;
    AVFormatContext* input = nullptr;
    const int opened = avformat_open_input(&input, path.string().c_str(), nullptr, nullptr);
    check(opened >= 0 && input != nullptr, "finished file can be reopened by libavformat");
    if (opened < 0 || input == nullptr) return result;
    result.format_name = input->iformat->name;
    result.streams = input->nb_streams;
    int video_index = -1, audio_index = -1;
    for (unsigned i = 0; i < input->nb_streams; ++i) {
        const auto* parameters = input->streams[i]->codecpar;
        Track* track = nullptr;
        if (parameters->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_index = static_cast<int>(i);
            track = &result.video;
        } else if (parameters->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_index = static_cast<int>(i);
            track = &result.audio;
        }
        if (track == nullptr) continue;
        track->codec = parameters->codec_id;
        track->tag = parameters->codec_tag;
        track->width = parameters->width;
        track->height = parameters->height;
        track->sample_rate = parameters->sample_rate;
        if (parameters->codec_type == AVMEDIA_TYPE_VIDEO) {
            const uint8_t* data = nullptr;
            std::size_t size = 0;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(60, 30, 100)
            const auto* side = av_packet_side_data_get(parameters->coded_side_data,
                parameters->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
            if (side != nullptr) { data = side->data; size = side->size; }
#else
            data = av_stream_get_side_data(input->streams[i], AV_PKT_DATA_DISPLAYMATRIX, &size);
#endif
            if (data != nullptr && size == 9 * sizeof(int32_t)) {
                std::array<int32_t, 9> matrix;
                std::memcpy(matrix.data(), data, size);
                const double degrees = av_display_rotation_get(matrix.data());
                check(std::isfinite(degrees), "stored display matrix has a finite rotation");
                if (std::isfinite(degrees)) {
                    track->clockwise_orientation = (360 - static_cast<int>(std::lround(degrees))) % 360;
                }
            }
        }
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 24, 100)
        track->channels = parameters->ch_layout.nb_channels;
#else
        track->channels = parameters->channels;
#endif
        if (parameters->extradata_size > 0) {
            track->extradata.assign(parameters->extradata,
                                    parameters->extradata + parameters->extradata_size);
        }
    }
    AVPacket* packet = av_packet_alloc();
    check(packet != nullptr, "demux packet allocation succeeds");
    if (packet != nullptr) {
        int ret = 0;
        while ((ret = av_read_frame(input, packet)) >= 0) {
            Track* track = packet->stream_index == video_index ? &result.video :
                           packet->stream_index == audio_index ? &result.audio : nullptr;
            if (track != nullptr) {
                const auto time_base = input->streams[packet->stream_index]->time_base;
                Packet sample;
                sample.pts = av_rescale_q(packet->pts, time_base, AVRational{1, 1000000});
                sample.dts = av_rescale_q(packet->dts, time_base, AVRational{1, 1000000});
                sample.duration = av_rescale_q(packet->duration, time_base, AVRational{1, 1000000});
                if (track == &result.audio)
                    sample.audio_evidence = recording_test::inspect_audio_packet(*input, *packet);
                sample.bytes.assign(packet->data, packet->data + packet->size);
                sample.key = (packet->flags & AV_PKT_FLAG_KEY) != 0;
                track->packets.push_back(std::move(sample));
            }
            av_packet_unref(packet);
        }
        check(ret == AVERROR_EOF, "demux ends normally after all packets");
    }
    if (audio_index >= 0)
        result.audio_tail = recording_test::inspect_audio_tail(*input, static_cast<unsigned>(audio_index));
    av_packet_free(&packet);
    avformat_close_input(&input);
    return result;
}

bool near(int64_t a, int64_t b, int64_t tolerance = 500) {
    return a >= b - tolerance && a <= b + tolerance;
}

void check_video(const Track& video, std::span<const int64_t> pts, Muxer::Format format) {
    check(video.codec == AV_CODEC_ID_HEVC && video.width == 64 && video.height == 64,
          "fresh HEVC extradata exports correct codec and dimensions to the container");
    if (format == Muxer::Format::Mp4) check(video.tag == MKTAG('h', 'v', 'c', '1'), "MP4 uses hvc1");
    check(video.packets.size() == pts.size(), "every complete HEVC AU is retained");
    for (std::size_t i = 0; i < video.packets.size() && i < pts.size(); ++i) {
        const auto& packet = video.packets[i];
        check(near(packet.pts, pts[i]) && packet.dts == packet.pts,
              "variable HEVC PTS/DTS are preserved in the container time base");
        // 两种容器将 Annex-B 分隔符转成 4 字节 NAL 长度，编码的 IDR 原文不变。
        Nal expected = idr;
        expected[3] = static_cast<uint8_t>(idr.size() - 4);
        check(packet.bytes == expected && packet.key, "container keeps every HEVC payload byte and key flag");
    }
}

void check_audio(const Track& audio, std::span<const int64_t> pts, int frame_samples) {
    check(audio.codec == AV_CODEC_ID_AAC && audio.sample_rate == 48000 && audio.channels == 2,
          "AAC-ELD track retains codec, sample rate and stereo layout");
    const Nal asc{0xf8, 0xe6, static_cast<uint8_t>(frame_samples == 480 ? 0x50 : 0x40), 0x00};
    check(audio.extradata == asc, "verified AAC-ELD ASC is preserved without priming correction");
    check(audio.packets.size() == pts.size(), "every raw AAC-ELD packet is retained");
    for (std::size_t i = 0; i < audio.packets.size() && i < pts.size(); ++i) {
        const auto& packet = audio.packets[i];
        check(near(packet.pts, pts[i]) && packet.dts == packet.pts,
              "AAC starts at the caller offset and follows mapped packet PTS");
        check(packet.bytes == silence, "AAC-ELD payload is neither dropped nor reencoded");
        if (!near(packet.pts, pts[i]) || packet.bytes != silence)
            std::fprintf(stderr, "AUDIO: %s\n", packet.audio_evidence.diagnostic().c_str());
    }
}

void container_orientations(const Directory& directory) {
    for (const auto format : {Muxer::Format::Mp4, Muxer::Format::Matroska}) {
        std::optional<File> baseline;
        for (const int degrees : {0, 90, 180, 270}) {
            const auto path = directory.path / ((format == Muxer::Format::Mp4 ? "oriented-mp4-" : "oriented-mkv-") +
                                               std::to_string(degrees));
            std::string error;
            const bool supported = Muxer::validate_video_orientation(format, degrees, error);
#if LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(60, 16, 100) || \
    LIBAVCODEC_VERSION_INT < AV_VERSION_INT(60, 30, 100)
            const bool expected_support = format == Muxer::Format::Mp4 || degrees == 0;
#else
            const bool expected_support = true;
#endif
            check(supported == expected_support, "orientation capability follows the supported container version");
            auto config = options(path, format, Muxer::Audio{});
            config.video_orientation = degrees;
            if (!supported) {
                check(!Muxer::open(config, error) && !error.empty() && !std::filesystem::exists(path),
                      "unsupported MKV rotation fails before creating a misleading file");
                continue;
            }
            auto writer = Muxer::open(config, error);
            check(writer != nullptr && error.empty(), "supported display orientation opens");
            if (!writer) continue;
            const std::array<int64_t, 3> video_pts{0, 50000, 125000};
            const std::array<int64_t, 3> audio_pts{25000, 35000, 45000};
            check(writer->write_video(idr, {0, 0, 50000}, true, error), "oriented first VFR AU writes");
            check(writer->write_video(idr, {50000, 50000, 75000}, true, error), "oriented second VFR AU writes");
            for (const auto pts : audio_pts) {
                check(writer->write_audio(silence, {pts, pts, 10000}, error), "oriented file retains raw AAC");
            }
            const int64_t tail = format == Muxer::Format::Mp4 ? 100000 : 0;
            check(writer->write_video(idr, {125000, 125000, tail}, true, error), "orientation preserves tail duration policy");
            check(writer->finish(error) && error.empty(), "oriented file finishes");
            const auto file = read(path);
            check(file.video.clockwise_orientation.value_or(0) == degrees,
                  "container display metadata round-trips the requested clockwise angle");
            check_video(file.video, video_pts, format);
            check_audio(file.audio, audio_pts, 480);
            if (!baseline) { baseline = file; continue; }
            for (const auto audio : {false, true}) {
                const auto& actual = audio ? file.audio : file.video;
                const auto& expected = audio ? baseline->audio : baseline->video;
                check(actual.codec == expected.codec && actual.tag == expected.tag &&
                      actual.width == expected.width && actual.height == expected.height &&
                      actual.sample_rate == expected.sample_rate && actual.channels == expected.channels &&
                      actual.extradata == expected.extradata,
                      "orientation changes no codec configuration or encoded dimensions");
                check(actual.packets.size() == expected.packets.size(), "orientation retains every packet");
                for (std::size_t i = 0; i < actual.packets.size() && i < expected.packets.size(); ++i) {
                    const auto& a = actual.packets[i]; const auto& b = expected.packets[i];
                    check(a.bytes == b.bytes && a.key == b.key && a.pts == b.pts &&
                          a.dts == b.dts && a.duration == b.duration,
                          "rotation metadata changes neither packet bytes nor PTS/DTS/duration");
                }
            }
        }
    }
}

void containers(const Directory& directory) {
    int index = 0;
    for (const auto format : {Muxer::Format::Mp4, Muxer::Format::Matroska}) {
        for (const int frame_samples : {480, 512}) {
            const auto path = directory.path / ("variable-" + std::to_string(index++));
            std::string error = "old error";
            auto writer = Muxer::open(options(path, format, Muxer::Audio{48000, 2, frame_samples}), error);
            check(writer != nullptr && error.empty(), "explicit container format opens without relying on filename extension");
            if (!writer) continue;
            const std::array<int64_t, 3> video_pts{0, 50000, 125000};
            const std::array<int64_t, 3> audio_pts{25000, 35000, 45000};
            check(writer->write_video(idr, {video_pts[0], video_pts[0], 50000}, true, error), "first HEVC AU writes");
            // 每轨有序但调用顺序不是全局 DTS 顺序，由库交错，不能要求同步流。
            check(writer->write_video(idr, {video_pts[1], video_pts[1], 75000}, true, error), "second VFR HEVC AU writes");
            for (const auto pts : audio_pts) {
                check(writer->write_audio(silence, {pts, pts, 10000}, error), "raw AAC-ELD packet writes");
            }
            const int64_t last_duration = format == Muxer::Format::Mp4 ? 25000 : 0;
            check(writer->write_video(idr, {video_pts[2], video_pts[2], last_duration}, true, error),
                  "last HEVC AU uses known MP4 duration or unknown Matroska duration");
            check(writer->finish(error) && error.empty(), "container trailer, flush and close succeed");
            check(writer->finish(error) && error.empty(), "successful finish is idempotent");
            const auto file = read(path);
            check(file.streams == 2, "A/V container exposes exactly two tracks");
            check_video(file.video, video_pts, format);
            check_audio(file.audio, audio_pts, frame_samples);
        }
        const auto path = directory.path / ("video-only-" + std::to_string(index++));
        std::string error;
        auto writer = Muxer::open(options(path, format), error);
        check(writer != nullptr, "video-only container opens");
        if (writer) {
            const int64_t duration = format == Muxer::Format::Mp4 ? 2000000 : 0;
            check(writer->write_video(idr, {0, 0, duration}, true, error),
                  "a single still AU uses the format's explicit duration contract");
            check(writer->finish(error), "video-only container finishes");
            const auto file = read(path);
            check(file.streams == 1, "optional audio does not create an empty second track");
            const std::array<int64_t, 1> pts{0};
            check_video(file.video, pts, format);
        }
    }
}

void explicit_formats(const Directory& directory) {
    using Format = scrctl::app::RecordFormat;
    // 实际封装后由独立解封装器识别，不能只检查选项枚举或文件名。
    for (const auto& [selected, filename] : {
            std::pair{Format::Mp4, "explicit-mp4.mkv"},
            std::pair{Format::Matroska, "explicit-mkv.mp4"},
            std::pair{Format::Mp4, "explicit-mp4-no-extension"}}) {
        const auto path = directory.path / filename;
        const auto format = scrctl::app::record_container_format(path.string(), selected);
        check(format.has_value(), "explicit format selects a container independently of its filename");
        if (!format) continue;
        std::string error;
        auto writer = Muxer::open(options(path, *format, Muxer::Audio{}), error);
        check(writer != nullptr, "selected container opens despite a different or absent extension");
        if (!writer) continue;
        check(writer->write_video(idr, {0, 0, 10000}, true, error) &&
                  writer->write_audio(silence, {0, 0, 10000}, error) && writer->finish(error),
              "selected container writes and finalizes both original encoded tracks");
        const auto file = read(path);
        const auto expected = selected == Format::Mp4 ? "mp4" : "matroska";
        check(file.format_name.find(expected) != std::string::npos,
              "independent demux identifies the explicitly selected format instead of the extension");
        check(file.streams == 2, "explicit format preserves the video and audio consumers");
        const std::array<int64_t, 1> pts{0};
        check_video(file.video, pts, *format);
        check_audio(file.audio, pts, 480);
    }
}

void audio_only_containers(const Directory& directory) {
    int index = 0;
    for (const auto format : {Muxer::Format::Mp4, Muxer::Format::Matroska}) {
        const auto path = directory.path / ("audio-only-" + std::to_string(index++));
        std::string error = "old error";
        auto writer = Muxer::open(audio_options(path, format), error);
        check(writer != nullptr && error.empty(), "audio-only container opens without HEVC parameters or an IDR");
        if (!writer) continue;
        const std::array<int64_t, 3> pts{0, 10000, 20000};
        for (const auto timestamp : pts)
            check(writer->write_audio(silence, {timestamp, timestamp, 10000}, error),
                  "audio-only muxer writes the original AAC-ELD packet with a positive duration");
        check(writer->finish(error) && error.empty(), "audio-only header, packets and trailer finish normally");
        check(writer->finish(error) && error.empty(), "audio-only muxer finish is idempotent");
        const auto file = read(path);
        check(file.streams == 1 && file.video.codec == AV_CODEC_ID_NONE && file.video.packets.empty(),
              "audio-only output has exactly one audio stream and no dummy video stream");
        check_audio(file.audio, pts, 480);
        for (const auto& packet : file.audio.packets) {
            const auto& evidence = packet.audio_evidence;
            check(packet.dts == packet.pts, "audio-only PTS and DTS remain equal");
            check(evidence.frame_valid(), "audio-only packets actually decode to 480 samples at 48kHz stereo");
            check(evidence.duration_valid(), "present packet duration is exactly 10ms; missing MKV duration needs real AAC proof");
            if (packet.dts != packet.pts || !evidence.frame_valid() || !evidence.duration_valid())
                std::fprintf(stderr, "AUDIO: %s\n", evidence.diagnostic().c_str());
        }
        check(file.audio_tail.valid(30000), "audio-only container ends exactly 10ms after its final packet PTS");
        if (!file.audio_tail.valid(30000))
            std::fprintf(stderr, "TAIL: %s\n", file.audio_tail.diagnostic(30000).c_str());

        writer = Muxer::open(audio_options(directory.path / ("audio-only-misuse-" + std::to_string(index)), format), error);
        check(writer != nullptr, "audio-only wrong-track fixture opens");
        if (!writer) continue;
        check(writer->write_audio(silence, {0, 0, 10000}, error), "an audio-only packet precedes wrong-track misuse");
        check(!writer->write_video(idr, {10000, 10000, 10000}, true, error) &&
              error.find("no video track") != std::string::npos, "audio-only output rejects video writes explicitly");
        const auto first = error;
        check(!writer->write_audio(silence, {10000, 10000, 10000}, error) && error == first,
              "later audio cannot replace the wrong-track first error");
        check(!writer->finish(error) && error == first, "audio-only wrong-track error survives file cleanup");
        check(!writer->finish(error) && error == first, "audio-only failed finish is idempotent");
    }
    std::string error;
    check(!Muxer::open(audio_options(directory.path / "missing" / "audio.mkv", Muxer::Format::Matroska), error) &&
          error.find("open the recording file") != std::string::npos,
          "audio-only container propagates a real local file-open failure");
}

void audio_evidence_negative_cases(const Directory& directory) {
    enum class Case { Asc512, Asc512Trimmed, CorruptPacket, Mp4Duration, MkvTail, MissingMp4Duration, MissingMkvEnd };
    for (const auto failure : {Case::Asc512, Case::Asc512Trimmed, Case::CorruptPacket, Case::Mp4Duration, Case::MkvTail,
                               Case::MissingMp4Duration, Case::MissingMkvEnd}) {
        const auto format = failure == Case::MkvTail || failure == Case::MissingMkvEnd ?
                            Muxer::Format::Matroska : Muxer::Format::Mp4;
        const auto path = directory.path / ("audio-evidence-negative-" + std::to_string(static_cast<int>(failure)));
        auto config = audio_options(path, format);
        if (failure == Case::Asc512 || failure == Case::Asc512Trimmed) config.audio->frame_samples = 512;
        std::string error;
        auto writer = Muxer::open(config, error);
        check(writer != nullptr, "negative evidence fixture opens an actual selected-track container");
        if (!writer) continue;
        const Nal corrupt{0xff, 0x00, 0xff, 0xff};  // Invalid ELD scalefactor-band count; not an ADTS prefix.
        const auto& bytes = failure == Case::CorruptPacket ? corrupt : silence;
        const int64_t duration = failure == Case::Asc512 ? 10667 :
                                 failure == Case::Mp4Duration || failure == Case::MkvTail ? 20000 : 10000;
        check(writer->write_audio(bytes, {0, 0, duration}, error) && writer->finish(error),
              "negative evidence uses actual original AAC/ASC/timing submitted through the public muxer");
        AVFormatContext* input = nullptr;
        check(avformat_open_input(&input, path.string().c_str(), nullptr, nullptr) >= 0 && input,
              "negative evidence container is independently readable");
        if (!input) continue;
        AVPacket* packet = av_packet_alloc();
        check(packet && av_read_frame(input, packet) >= 0, "negative evidence reads the actual encoded packet");
        if (packet && packet->size) {
            if (failure == Case::MissingMp4Duration) packet->duration = 0;  // Explicit public packet-field seam.
            const auto evidence = recording_test::inspect_audio_packet(*input, *packet);
            if (failure == Case::Asc512)
                check(evidence.decoded && evidence.samples == 512 && !evidence.frame_valid(),
                      "actual 512-sample ASC decoding cannot satisfy the 480-sample proof");
            else if (failure == Case::Asc512Trimmed)
                check(!evidence.configuration_valid && !evidence.frame_valid() && !evidence.duration_valid(),
                      "wrong 512-sample ASC cannot pass when MP4 trims its decoded output to a 10ms duration");
            else if (failure == Case::CorruptPacket)
                check(!evidence.decoded && !evidence.duration_valid(),
                      "corrupt original AAC cannot be excused by a plausible container duration");
            else if (failure == Case::Mp4Duration || failure == Case::MissingMp4Duration)
                check(evidence.frame_valid() && !evidence.duration_valid(),
                      "MP4 rejects wrong or missing packet duration even when actual AAC yields 480 samples");
            else if (failure == Case::MkvTail)
                check(evidence.frame_valid() && !recording_test::inspect_audio_tail(*input, 0).valid(10000),
                      "MKV's actual 20ms container tail cannot satisfy the original 10ms sample boundary");
            else {
                const int64_t saved = input->duration;
                input->duration = AV_NOPTS_VALUE;  // Explicit public parsed-metadata seam.
                check(!recording_test::inspect_audio_tail(*input, 0).valid(10000),
                      "a missing MKV Info end cannot be excused by an intact track duration");
                input->duration = saved;
                av_dict_set(&input->streams[0]->metadata, "DURATION", nullptr, 0);
                check(!recording_test::inspect_audio_tail(*input, 0).valid(10000),
                      "a missing MKV track end cannot be excused by intact Info duration");
            }
        }
        av_packet_free(&packet);
        avformat_close_input(&input);
    }
}

void still_video_and_negative_audio(const Directory& directory) {
    int index = 0;
    for (const auto format : {Muxer::Format::Mp4, Muxer::Format::Matroska}) {
        const auto path = directory.path / ("still-audio-" + std::to_string(index++));
        std::string error;
        auto writer = Muxer::open(options(path, format, Muxer::Audio{}), error);
        check(writer != nullptr, "still video with continuous AAC opens");
        if (!writer) continue;
        const int64_t video_duration = format == Muxer::Format::Mp4 ? 1500000 : 0;
        check(writer->write_video(idr, {0, 0, video_duration}, true, error),
              "still image uses caller duration for MP4 and explicit unknown duration for Matroska");
        std::vector<int64_t> audio_pts;
        for (int i = 0; i < 150; ++i) {
            // Matroska 不能表达零点之前的包；选共同原点保持两轨相对关系。
            const int64_t pts = (format == Muxer::Format::Mp4 ? -10000 : 10000) + i * 10000;
            audio_pts.push_back(pts);
            check(writer->write_audio(silence, {pts, pts, 10000}, error),
                  "continuous AAC preserves the caller starting offset");
        }
        check(writer->finish(error), "missing later video does not block A/V finish");
        const auto file = read(path);
        const std::array<int64_t, 1> video_pts{0};
        check_video(file.video, video_pts, format);
        check_audio(file.audio, audio_pts, 480);
        // 不断言末帧 duration：libavformat/解封装器可能推断值，它不是源端证据。
    }
}

void rejected_before_open(const Directory& directory) {
    const auto path = directory.path / "untouched";
    { std::ofstream stream(path); stream << "keep"; }
    auto invalid = options(path, Muxer::Format::Mp4);
    std::string error;
    invalid.pps.resize(3);
    check(!Muxer::open(invalid, error) && !error.empty(), "truncated PPS fails before opening the file");
    invalid = options(path, Muxer::Format::Mp4);
    invalid.audio = Muxer::Audio{44100, 2, 480};
    check(!Muxer::open(invalid, error), "unverified AAC sample rate is refused");
    invalid.audio = Muxer::Audio{48000, 1, 480};
    check(!Muxer::open(invalid, error), "unverified AAC channel count is refused");
    invalid.audio = Muxer::Audio{48000, 2, 1024};
    check(!Muxer::open(invalid, error), "unverified AAC frame length is refused");
    invalid = options(path, static_cast<Muxer::Format>(-1));
    check(!Muxer::open(invalid, error), "unknown container enum is refused");
    invalid = options(path, Muxer::Format::Mp4);
    invalid.vps = hex("40010c01ffff01600000030090000003000003001e959409");
    invalid.sps = hex("42010101600000030090000003000003001ea0208105965654a4c2f0168080000003008000000f04");
    check(!Muxer::open(invalid, error), "HEVC reordering is refused instead of assigning DTS=PTS");
    std::ifstream stream(path);
    std::string existing;
    stream >> existing;
    check(existing == "keep", "configuration validation does not truncate an existing file");
    invalid = options({}, Muxer::Format::Mp4);
    check(!Muxer::open(invalid, error), "empty path is refused");
    invalid.path = std::string("a\0b", 3);
    check(!Muxer::open(invalid, error), "embedded NUL path is refused");
    invalid.path = "data:text/plain,not-a-local-file";
    check(!Muxer::open(invalid, error), "non-file protocol is refused without network access");
    invalid.path = "pipe:1";
    check(!Muxer::open(invalid, error), "writable non-file protocol is also refused");
    invalid.path = (directory.path / "missing" / "output").string();
    check(!Muxer::open(invalid, error), "file open failure is reported");
}

void invalid_packets(const Directory& directory) {
    enum class Case { Empty, Oversize, Prefix, Reorder, NoPts, ZeroVideo, NegativeDuration, Overflow,
                      MissingAudio, ZeroAudio, DuplicateDts, Rollback, Quantization,
                      NegativeMatroska, NegativeMatroskaAudio, DurationRange, IntervalRange };
    int index = 0;
    for (const auto failure : {Case::Empty, Case::Oversize, Case::Prefix, Case::Reorder, Case::NoPts, Case::ZeroVideo,
             Case::NegativeDuration, Case::Overflow, Case::MissingAudio, Case::ZeroAudio,
             Case::DuplicateDts, Case::Rollback, Case::Quantization,
             Case::NegativeMatroska, Case::NegativeMatroskaAudio,
             Case::DurationRange, Case::IntervalRange}) {
        std::string error;
        auto writer = Muxer::open(options(directory.path / ("invalid-" + std::to_string(index++)),
                                 failure == Case::Quantization || failure == Case::NegativeMatroska ||
                                     failure == Case::NegativeMatroskaAudio
                                     ? Muxer::Format::Matroska : Muxer::Format::Mp4,
                                 failure == Case::ZeroAudio || failure == Case::NegativeMatroskaAudio
                                     ? std::optional{Muxer::Audio{}} : std::nullopt), error);
        check(writer != nullptr, "valid initial configuration for a packet failure opens");
        if (!writer) continue;
        bool written = true;
        switch (failure) {
            case Case::Empty: written = writer->write_video({}, {0, 0, 0}, true, error); break;
            case Case::Oversize: {
                const Nal bytes(scrctl::media::kMaxRecordingMuxerPacketBytes + 1);
                written = writer->write_video(bytes, {0, 0, 0}, true, error);
                break;
            }
            case Case::Prefix: written = writer->write_video(silence, {0, 0, 0}, true, error); break;
            case Case::Reorder: written = writer->write_video(idr, {1, 0, 0}, true, error); break;
            case Case::NoPts: written = writer->write_video(idr, {AV_NOPTS_VALUE, AV_NOPTS_VALUE, 10000}, true, error); break;
            case Case::ZeroVideo: written = writer->write_video(idr, {0, 0, 0}, true, error); break;
            case Case::NegativeDuration: written = writer->write_video(idr, {0, 0, -1}, true, error); break;
            case Case::Overflow: written = writer->write_video(idr, {INT64_MAX - 1, INT64_MAX - 1, 2}, true, error); break;
            case Case::MissingAudio: written = writer->write_audio(silence, {0, 0, 10000}, error); break;
            case Case::ZeroAudio: written = writer->write_audio(silence, {0, 0, 0}, error); break;
            case Case::NegativeMatroska: written = writer->write_video(idr, {-10000, -10000, 0}, true, error); break;
            case Case::NegativeMatroskaAudio: written = writer->write_audio(silence, {-10000, -10000, 10000}, error); break;
            case Case::DurationRange: written = writer->write_video(idr, {0, 0, int64_t{INT32_MAX} + 1}, true, error); break;
            case Case::DuplicateDts:
            case Case::Rollback:
            case Case::Quantization:
            case Case::IntervalRange:
                check(writer->write_video(idr, {10000, 10000, 10000}, true, error), "first packet establishes each-track order");
                {
                    const int64_t pts = failure == Case::Rollback ? 9000 : failure == Case::Quantization ? 10001 :
                                        failure == Case::IntervalRange ? int64_t{10000} + INT32_MAX + 1 : 10000;
                    written = writer->write_video(idr, {pts, pts, 10000}, true, error);
                }
                break;
        }
        check(!written && !error.empty(), "invalid packet fails explicitly");
        const auto first_error = error;
        check(writer->error() == first_error, "the first write error is latched");
        check(!writer->write_video(idr, {20000, 20000, 10000}, true, error) && error == first_error,
              "later writes cannot erase or replace the first failure");
        check(!writer->finish(error) && error == first_error, "finish closes but cannot turn failure into success");
        check(!writer->finish(error) && error == first_error, "failed finish is idempotent");
    }
    std::string error;
    auto writer = Muxer::open(options(directory.path / "finished", Muxer::Format::Mp4), error);
    check(writer != nullptr, "post-finish case opens");
    if (writer) {
        check(writer->write_video(idr, {0, 0, 10000}, true, error) && writer->finish(error), "post-finish case closes normally");
        check(!writer->write_video(idr, {10000, 10000, 0}, true, error) && !error.empty(),
              "writing to a finished recording fails clearly");
        check(writer->finish(error) && error.empty() && writer->error().empty(),
              "post-finish misuse does not invalidate the completed file or change finish's result");
    }
}
#endif
}  // namespace

int main() {
    Directory directory;
    orientation_precheck(directory);
    selected_tracks_precheck(directory);
#ifdef SCRCTL_HAVE_LIBAVFORMAT
    check(Muxer::available(), "libavformat build reports container recording support");
    containers(directory);
    explicit_formats(directory);
    audio_only_containers(directory);
    audio_evidence_negative_cases(directory);
    container_orientations(directory);
    still_video_and_negative_audio(directory);
    rejected_before_open(directory);
    invalid_packets(directory);
#else
    check(!Muxer::available(), "without libavformat container recording is explicitly unavailable");
    std::string error;
    for (const auto format : {Muxer::Format::Mp4, Muxer::Format::Matroska})
        for (const int degrees : {90, 180, 270})
            check(!Muxer::validate_video_orientation(format, degrees, error) && !error.empty(),
                  "without a muxing library nonzero orientation is explicitly unavailable");
    const auto path = directory.path / "not-created";
    check(!Muxer::open(options(path, Muxer::Format::Mp4), error) && !error.empty(),
          "without libavformat open fails clearly");
    check(!std::filesystem::exists(path), "unavailable recording does not create or truncate files");
    check(!Muxer::open(audio_options(path, Muxer::Format::Matroska), error) && !error.empty() &&
          !std::filesystem::exists(path), "audio-only muxing also requires libavformat without creating a file");
#endif
    std::printf("recording_muxer: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
