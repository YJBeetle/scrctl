#include "media/Recorder.h"
#include "media/RecordingMuxer.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <thread>
#include <vector>

#ifdef SCRCTL_HAVE_LIBAVFORMAT
extern "C" {
#include <libavcodec/version.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/mathematics.h>
}
#endif

namespace {
using Recorder = scrctl::media::Recorder;
using Track = Recorder::Track;
using scrctl::Nal;
using namespace std::chrono_literals;
int checks = 0, failures = 0;
void check(bool value, const char* message) {
    ++checks;
    if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
#ifdef SCRCTL_HAVE_LIBAVFORMAT
Nal hex(std::string_view text) {
    Nal bytes;
    for (std::size_t i = 0; i < text.size(); i += 2) {
        unsigned byte = 0;
        std::sscanf(text.data() + i, "%2x", &byte);
        bytes.push_back(static_cast<uint8_t>(byte));
    }
    return bytes;
}
// 与 recording_muxer_test 相同的 FFmpeg 9.0.1 / x265 4.3 黑色 64x64
// bframes=0 参数和 IDR。固定原文不要求 CI 在运行时提供 x265 编码器。
const Nal vps = hex("40010c01ffff01600000030090000003000003001eba0240");
const Nal sps = hex("42010101600000030090000003000003001ea020810596e92930bc05a020000003002000000303c1");
const Nal pps = hex("4401c073c089");
const Nal idr = hex("2801ac21800e7ffeebf349ac");
const Nal silence{0x00, 0x68, 0x34, 0x00};
const std::array<uint8_t, 16> video_session{1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
const std::array<uint8_t, 16> audio_session{16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1};
#endif

struct Directory {
    std::filesystem::path path;
    Directory() {
        path = std::filesystem::temp_directory_path() /
            ("scrctl-recorder-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(path);
    }
    ~Directory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
Recorder::Options options(const std::filesystem::path& path, bool audio = false,
                          scrctl::media::RecordingMuxer::Format format =
                              scrctl::media::RecordingMuxer::Format::Matroska) {
    Recorder::Options result;
    result.path = path.string(); result.include_audio = audio; result.format = format;
    return result;
}
#ifdef SCRCTL_HAVE_LIBAVFORMAT
scrctl::rt::SenderReport sr(uint32_t source, uint32_t ticks, uint32_t seconds,
                            uint32_t fraction = 0) {
    scrctl::rt::SenderReport result;
    result.ssrc = source; result.rtp_timestamp = ticks;
    result.ntp_seconds = seconds; result.ntp_fraction = fraction;
    return result;
}
void video(Recorder& recorder, int64_t ticks, uint32_t source = 0) {
    check(recorder.video(video_session, source, ticks, {idr}, vps, sps, pps), "complete IDR is admitted");
}
void report(Recorder& recorder, Track track, const scrctl::rt::SenderReport& value) {
    check(recorder.sender_report(track, track == Track::Video ? video_session : audio_session, value),
          "sender report is admitted without I/O on the caller");
}
void begin(Recorder& recorder, bool audio, std::optional<uint32_t> source = 0) {
    check(recorder.begin_track(Track::Video, video_session, source), "video session is admitted");
    if (audio) check(recorder.begin_track(Track::Audio, audio_session, 9), "audio session is admitted");
}
bool wait_for_error(Recorder& recorder) {
    for (int i = 0; i < 200 && recorder.error().empty(); ++i) std::this_thread::sleep_for(5ms);
    return !recorder.error().empty();
}

struct Packet { int64_t pts, dts, duration; Nal bytes; };
struct File {
    std::vector<Packet> video, audio;
    unsigned tracks = 0;
    std::optional<int> clockwise_orientation;
};
File read(const std::filesystem::path& path) {
    File result;
    AVFormatContext* input = nullptr;
    const int opened = avformat_open_input(&input, path.string().c_str(), nullptr, nullptr);
    check(opened >= 0 && input != nullptr, "successful Recorder output is a readable container");
    if (!input) return result;
    result.tracks = input->nb_streams;
    for (unsigned i = 0; i < input->nb_streams; ++i) {
        const auto* stream = input->streams[i];
        if (stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) continue;
        const uint8_t* data = nullptr;
        std::size_t size = 0;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(60, 30, 100)
        const auto* side = av_packet_side_data_get(stream->codecpar->coded_side_data,
            stream->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
        if (side != nullptr) { data = side->data; size = side->size; }
#else
        data = av_stream_get_side_data(stream, AV_PKT_DATA_DISPLAYMATRIX, &size);
#endif
        if (data != nullptr && size == 9 * sizeof(int32_t)) {
            std::array<int32_t, 9> matrix;
            std::memcpy(matrix.data(), data, size);
            const double degrees = av_display_rotation_get(matrix.data());
            check(std::isfinite(degrees), "Recorder display matrix has a finite rotation");
            if (std::isfinite(degrees))
                result.clockwise_orientation = (360 - static_cast<int>(std::lround(degrees))) % 360;
        }
    }
    AVPacket* packet = av_packet_alloc();
    check(packet != nullptr, "demux packet can be allocated");
    int ret = AVERROR_EOF;
    if (packet) {
        while ((ret = av_read_frame(input, packet)) >= 0) {
            auto* stream = input->streams[packet->stream_index];
            Packet sample{av_rescale_q(packet->pts, stream->time_base, AVRational{1, 1000000}),
                          av_rescale_q(packet->dts, stream->time_base, AVRational{1, 1000000}),
                          av_rescale_q(packet->duration, stream->time_base, AVRational{1, 1000000}),
                          Nal(packet->data, packet->data + packet->size)};
            if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                check(stream->codecpar->codec_id == AV_CODEC_ID_HEVC, "Recorder writes a HEVC track");
                result.video.push_back(std::move(sample));
            } else {
                check(stream->codecpar->codec_id == AV_CODEC_ID_AAC, "Recorder writes a raw AAC-ELD track");
                result.audio.push_back(std::move(sample));
            }
            av_packet_unref(packet);
        }
        check(ret == AVERROR_EOF, "all container packets demux before normal EOF");
    }
    av_packet_free(&packet); avformat_close_input(&input);
    return result;
}
bool near(int64_t a, int64_t b) { return a >= b - 500 && a <= b + 500; }
void verify_video(const File& file, std::span<const int64_t> pts) {
    check(file.video.size() == pts.size(), "every approved HEVC AU is retained");
    Nal expected{0,0,0,static_cast<uint8_t>(idr.size())};
    expected.insert(expected.end(), idr.begin(), idr.end());
    for (std::size_t i = 0; i < file.video.size() && i < pts.size(); ++i) {
        check(near(file.video[i].pts, pts[i]), "video PTS follows the measured SR interval");
        check(file.video[i].dts == file.video[i].pts, "no-reorder video preserves equal PTS and DTS");
        check(file.video[i].bytes == expected, "HEVC encoded bytes are unchanged");
    }
}
void verify_audio(const File& file, std::span<const int64_t> pts) {
    check(file.audio.size() == pts.size(), "every raw AAC-ELD packet is retained");
    for (std::size_t i = 0; i < file.audio.size() && i < pts.size(); ++i) {
        check(near(file.audio[i].pts, pts[i]), "audio preserves its shared-origin SR timing");
        check(file.audio[i].bytes == silence, "AAC-ELD is not resampled or reencoded");
    }
}

void recorded_orientation(const Directory& directory) {
    std::optional<File> baseline;
    for (const int degrees : {0, 90}) {
        const auto path = directory.path / ("recorder-oriented-" + std::to_string(degrees) + ".mp4");
        auto config = options(path, true, scrctl::media::RecordingMuxer::Format::Mp4);
        config.video_orientation = degrees;
        std::string error;
        auto recorder = Recorder::start(config, error);
        check(recorder != nullptr && error.empty(), "Recorder accepts a supported container orientation");
        if (!recorder) continue;
        begin(*recorder, true);
        video(*recorder, 0); video(*recorder, 1200); video(*recorder, 3000);
        for (const uint32_t ticks : {1200u, 1680u, 2160u})
            check(recorder->audio(audio_session, 9, ticks, silence), "oriented recording admits unchanged AAC");
        report(*recorder, Track::Video, sr(0, 0, 1000));
        report(*recorder, Track::Audio, sr(9, 0, 1000));
        report(*recorder, Track::Video, sr(0, 24000, 1001));
        report(*recorder, Track::Audio, sr(9, 48000, 1001));
        check(recorder->finish(error) && error.empty(), "oriented Recorder finishes normally");
        const auto file = read(path);
        check(file.clockwise_orientation.value_or(0) == degrees,
              "Recorder passes its clockwise orientation through to the muxer");
        const std::array<int64_t, 3> video_pts{0, 50000, 125000};
        const std::array<int64_t, 3> audio_pts{25000, 35000, 45000};
        verify_video(file, video_pts); verify_audio(file, audio_pts);
        if (!baseline) { baseline = file; continue; }
        // 同一 SR 和 AU 输入，只有展示元数据不同；时钟量化和末帧规则不应变化。
        for (const bool audio : {false, true}) {
            const auto& actual = audio ? file.audio : file.video;
            const auto& expected = audio ? baseline->audio : baseline->video;
            check(actual.size() == expected.size(), "Recorder orientation retains every approved packet");
            for (std::size_t i = 0; i < actual.size() && i < expected.size(); ++i)
                check(actual[i].pts == expected[i].pts && actual[i].dts == expected[i].dts &&
                      actual[i].duration == expected[i].duration && actual[i].bytes == expected[i].bytes,
                      "Recorder rotation metadata leaves encoded bytes and all timing unchanged");
        }
    }
}

void common_origin(const Directory& directory) {
    const auto path = directory.path / "common.mkv";
    std::string error;
    auto recorder = Recorder::start(options(path, true), error);
    check(recorder != nullptr && error.empty(), "A/V Recorder starts before opening the output file");
    if (!recorder) return;
    begin(*recorder, true);
    // 早到 SR 及显式 SSRC 0；视频 24 kHz / 音频 48 kHz，不硬编码视频频率。
    report(*recorder, Track::Video, sr(0, 1000, 1000));
    report(*recorder, Track::Audio, sr(9, 5000, 1000));
    video(*recorder, 1480);
    video(*recorder, 2480);
    video(*recorder, 7480);
    for (uint32_t i = 0; i < 6; ++i)
        check(recorder->audio(audio_session, 9, 5000 + i * 480, silence), "audio packet is admitted");
    report(*recorder, Track::Video, sr(0, 25000, 1001));
    report(*recorder, Track::Audio, sr(9, 53000, 1001));
    check(recorder->finish(error) && error.empty(), "both tracks finish with a shared nonnegative origin");
    check(recorder->finish(error) && error.empty(), "successful Recorder finish is idempotent");
    check(!recorder->audio({}, 9, 0, {}), "input after completion is rejected");
    recorder->fail("late misuse");
    check(recorder->finish(error) && error.empty(), "post-finish misuse cannot invalidate a completed file");
    const auto file = read(path);
    check(file.tracks == 2, "A/V output has exactly two tracks");
    const std::array<int64_t,3> video_pts{20000, 61667, 270000};
    const std::array<int64_t,6> audio_pts{0,10000,20000,30000,40000,50000};
    verify_video(file, video_pts); verify_audio(file, audio_pts);
}

void wrapping_audio_and_first_idr(const Directory& directory) {
    const auto path = directory.path / "wrap.mkv";
    std::string error;
    auto recorder = Recorder::start(options(path, true), error);
    check(recorder != nullptr, "wrap fixture starts"); if (!recorder) return;
    begin(*recorder, true, std::nullopt);
    // 无来源时 foreign SR 不得绑定；选中源首媒体到达后丢弃这些早到报告。
    report(*recorder, Track::Video, sr(77, 0, 1));
    report(*recorder, Track::Video, sr(0, 0, 1000));
    report(*recorder, Track::Video, sr(0, 24000, 1001));
    check(recorder->video(video_session, 0, -100, {vps,sps,pps}, {}, {}, {}),
          "parameter-only AU before first IDR is admitted and ignored");
    Nal non_idr = idr; non_idr[0] = 0x02;
    check(recorder->video(video_session, 0, 0, {non_idr}, {}, {}, {}),
          "non-IDR AU before first IDR keeps the source reference without starting recording");
    video(*recorder, 240);
    constexpr uint32_t first = 0xfffffff0u;
    report(*recorder, Track::Audio, sr(9, first, 1000));
    for (uint32_t i = 0; i < 3; ++i)
        check(recorder->audio(audio_session, 9, first + i * 480, silence), "audio wrap packet is admitted");
    report(*recorder, Track::Audio, sr(9, first + 48000, 1001));
    check(recorder->finish(error), "raw audio timestamps unwrap over 32-bit rollover");
    const auto file = read(path);
    const std::array<int64_t,1> video_pts{10000};
    const std::array<int64_t,3> audio_pts{0,10000,20000};
    verify_video(file, video_pts); verify_audio(file, audio_pts);
}

void still_video(const Directory& directory, scrctl::media::RecordingMuxer::Format format =
                     scrctl::media::RecordingMuxer::Format::Matroska) {
    const bool mp4 = format == scrctl::media::RecordingMuxer::Format::Mp4;
    const auto path = directory.path / (mp4 ? "still.mp4" : "still.mkv");
    auto config = options(path, true, format); config.clock_wait = 100ms;
    std::string error;
    auto recorder = Recorder::start(config, error);
    check(recorder != nullptr, "still-video fixture starts"); if (!recorder) return;
    begin(*recorder, true);
    video(*recorder, 0);
    check(recorder->audio(audio_session, 9, 0, silence), "first still-video audio packet is admitted");
    report(*recorder, Track::Video, sr(0, 0, 1000));
    report(*recorder, Track::Audio, sr(9, 0, 1000));
    report(*recorder, Track::Video, sr(0, 24000, 1001));
    report(*recorder, Track::Audio, sr(9, 48000, 1001));
    std::this_thread::sleep_for(150ms);
    check(recorder->error().empty(), "a mapped still video does not wait for SR or the next picture on a clock deadline");
    // 超过八个 SR 仍应及时淘汰已用锚点，音频不等下一视频 AU。
    for (uint32_t second = 1; second <= 12; ++second) {
        check(recorder->audio(audio_session, 9, second * 48000, silence), "audio continues independently of video");
        report(*recorder, Track::Video, sr(0, (second + 1) * 24000, 1001 + second));
        report(*recorder, Track::Audio, sr(9, (second + 1) * 48000, 1001 + second));
    }
    std::this_thread::sleep_for(150ms);
    check(recorder->error().empty(), "continued audio drains before finish while the mapped video tail remains still");
    check(recorder->finish(error), "still video and continued audio finish after many SR intervals");
    const auto file = read(path);
    const std::array<int64_t,1> video_pts{0}; verify_video(file, video_pts);
    if (mp4 && file.video.size() == 1)
        check(file.video[0].duration == 100000, "a static MP4 tail uses its explicit 100 ms display policy");
    std::array<int64_t,13> audio_pts{};
    for (std::size_t i = 0; i < audio_pts.size(); ++i) audio_pts[i] = static_cast<int64_t>(i) * 1000000;
    verify_audio(file, audio_pts);
}

bool wait_for_file(const std::filesystem::path& path, Recorder& recorder) {
    for (int i = 0; i < 200 && recorder.error().empty(); ++i) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(path, ec)) return true;
        std::this_thread::sleep_for(5ms);
    }
    return false;
}

void mp4_video_timing(const Directory& directory) {
    using Format = scrctl::media::RecordingMuxer::Format;
    for (bool single : {true, false}) {
        const auto path = directory.path / (single ? "single.mp4" : "vfr.mp4");
        std::string error;
        auto recorder = Recorder::start(options(path, false, Format::Mp4), error);
        check(recorder != nullptr, "MP4 starts before any source clock is approved");
        if (!recorder) continue;
        begin(*recorder, false); video(*recorder, 0);
        // 两条 SR 确定 24 kHz 视频时钟。下面故意采用不同采样间隔，
        // 验证录制时长来自这些时间点，而不是配置中声明的帧率。
        report(*recorder, Track::Video, sr(0, 0, 1000));
        report(*recorder, Track::Video, sr(0, 24000, 1001));
        if (!single) {
            video(*recorder, 500); video(*recorder, 2900); video(*recorder, 9400);
        }
        check(recorder->finish(error) && error.empty(), "normal MP4 finish writes the final tail before media validation");
        check(recorder->finish(error) && error.empty(), "MP4 success remains stable on repeated finish");
        const auto file = read(path);
        const std::vector<int64_t> pts = single ? std::vector<int64_t>{0}
            : std::vector<int64_t>{0, 20833, 120833, 391667};
        verify_video(file, pts);
        if (file.video.size() == pts.size()) {
            for (std::size_t i = 0; i < pts.size(); ++i) {
                check(file.video[i].pts == pts[i], "MP4 retains each approved sampling point after one final quantization");
                const auto expected = i + 1 < pts.size() ? pts[i + 1] - pts[i] : 100000;
                check(file.video[i].duration == expected, "MP4 keeps actual VFR gaps and gives only its final AU 100 ms");
            }
        }
    }
}

void mp4_failure_paths(const Directory& directory) {
    using Format = scrctl::media::RecordingMuxer::Format;
    std::string error;
    {
        const auto path = directory.path / "mapped-tail-budget.mp4";
        auto config = options(path, false, Format::Mp4);
        const auto parameters = (vps.size() + sps.size() + pps.size()) * 3;
        const auto packet = (idr.size() + 4) * 2;
        // 若已映射的旧尾包被误计为释放，这个预算足够再接收一个 AU；
        // 正确保留旧包时，两份编码同时占用预算，必须拒绝新输入。
        config.encoded_budget = parameters * 2 + packet + 1;
        auto recorder = Recorder::start(config, error);
        check(recorder != nullptr, "mapped-tail shared-budget fixture starts");
        if (recorder) {
            begin(*recorder, false); video(*recorder, 0);
            report(*recorder, Track::Video, sr(0, 0, 1000));
            report(*recorder, Track::Video, sr(0, 24000, 1001));
            check(wait_for_file(path, *recorder), "approved MP4 clocks open the file while retaining its first tail");
            check(!recorder->video(video_session, 0, 240, {idr}, vps, sps, pps),
                  "a mapped MP4 tail remains in the shared encoded-byte budget");
            const auto first = recorder->error();
            recorder->fail("later error");
            check(!recorder->finish(error) && error == first && error.find("budget") != std::string::npos,
                  "tail-budget failure survives cleanup without being replaced by missing video or trailer errors");
            check(!recorder->finish(error) && error == first, "failed MP4 tail cleanup is idempotent");
        }
    }
    {
        auto config = options(directory.path / "unmapped-next.mp4", false, Format::Mp4);
        config.clock_wait = 40ms;
        auto recorder = Recorder::start(config, error);
        check(recorder != nullptr, "unmapped MP4 successor fixture starts");
        if (recorder) {
            begin(*recorder, false); video(*recorder, 0);
            report(*recorder, Track::Video, sr(0, 0, 1000));
            report(*recorder, Track::Video, sr(0, 24000, 1001));
            video(*recorder, 30000);
            check(wait_for_error(*recorder), "an unapproved successor still has the bounded SR wait deadline");
            const auto first = recorder->error();
            check(!recorder->finish(error) && error == first,
                  "the 100 ms display rule does not authorize a video point without a trusted clock");
        }
    }
    {
        auto recorder = Recorder::start(options(directory.path / "quantized-same.mp4", false, Format::Mp4), error);
        check(recorder != nullptr, "quantized interval fixture starts");
        if (recorder) {
            begin(*recorder, false); video(*recorder, 0);
            report(*recorder, Track::Video, sr(0, 0, 1000));
            report(*recorder, Track::Video, sr(0, 24000000, 1001));
            video(*recorder, 1); // 不同 ticks 在微秒量化后重合，不能写成零时长。
            check(!recorder->finish(error) && error.find("advance") != std::string::npos,
                  "distinct RTP points cannot create a zero MP4 duration after quantization");
        }
    }
    {
        auto recorder = Recorder::start(options(directory.path / "mux-range.mp4", false, Format::Mp4), error);
        check(recorder != nullptr, "actual MP4 mux rejection fixture starts");
        if (recorder) {
            begin(*recorder, false); video(*recorder, 0);
            report(*recorder, Track::Video, sr(0, 0, 1000));
            for (uint32_t second = 5; second <= 2150; second += 5)
                report(*recorder, Track::Video, sr(0, second * 24000, 1000 + second));
            video(*recorder, 2150 * 24000);
            check(!recorder->finish(error) && error.find("MP4 range") != std::string::npos,
                  "an actual muxer duration-range failure seals recording instead of changing the measured VFR interval");
            const auto first = error;
            recorder->fail("late");
            check(!recorder->finish(error) && error == first, "MP4 write failure is preserved through repeat finish");
        }
    }
}

void final_mapping(const Directory& directory) {
    for (const auto [ticks, success] : {std::pair<int64_t,bool>{30000,true}, {70000,false}}) {
        const auto path = directory.path / (success ? "final-good.mkv" : "final-bad.mkv");
        std::string error;
        auto recorder = Recorder::start(options(path), error); if (!recorder) { check(false,"Final fixture starts"); continue; }
        begin(*recorder, false); video(*recorder, 0);
        report(*recorder, Track::Video, sr(0, 0, 1000));
        report(*recorder, Track::Video, sr(0, 24000, 1001));
        video(*recorder, ticks);
        check(recorder->finish(error) == success, "Final uses the explicit 1.5-second extrapolation limit");
        const auto saved = error;
        check(recorder->finish(error) == success && error == saved, "Final failure/success result remains stable");
        if (success) { const auto file = read(path); const std::array<int64_t,2> pts{0,1250000}; verify_video(file,pts); }
    }
}

void early_reports_and_parameter_epoch(const Directory& directory) {
    std::string error;
    {
        const auto path = directory.path / "early-eight.mkv";
        auto recorder = Recorder::start(options(path), error);
        check(recorder != nullptr, "bounded early-SR fixture starts"); if (!recorder) return;
        begin(*recorder, false, std::nullopt);
        for (uint32_t i = 0; i < 8; ++i) report(*recorder, Track::Video, sr(0, i * 24000, 1000 + i));
        report(*recorder, Track::Video, sr(0, 7 * 24000, 1007));
        video(*recorder, 0);
        check(recorder->finish(error), "duplicate at the bounded early-SR limit does not consume another slot");
        const auto file = read(path); const std::array<int64_t,1> pts{0}; verify_video(file,pts);
    }
    {
        auto recorder = Recorder::start(options(directory.path / "early-overflow.mkv"), error);
        check(recorder != nullptr, "early-SR overflow fixture starts"); if (!recorder) return;
        begin(*recorder, false, std::nullopt);
        for (uint32_t i = 0; i < 9; ++i)
            (void)recorder->sender_report(Track::Video,video_session,sr(i,i,1000+i));
        check(!recorder->finish(error) && error.find("budget") != std::string::npos,
              "distinct early sender reports are bounded before any source is selected");
    }
    {
        auto recorder = Recorder::start(options(directory.path / "parameter-epoch.mkv"), error);
        check(recorder != nullptr, "parameter-only epoch-change fixture starts"); if (!recorder) return;
        begin(*recorder,false); video(*recorder,0);
        report(*recorder,Track::Video,sr(0,0,1000)); report(*recorder,Track::Video,sr(0,24000,1001));
        Nal changed = pps; changed.back() ^= 1;
        check(recorder->video(video_session,0,240,{vps,sps,changed},vps,sps,changed),
              "a parameter-only AU is admitted for epoch validation");
        check(!recorder->finish(error) && error.find("parameter") != std::string::npos,
              "parameter-only changes after the initial IDR fail the recording");
    }
    for (uint8_t header : {uint8_t{0}, uint8_t{2}, uint8_t{9}}) {
        auto recorder = Recorder::start(options(directory.path / "bad-header.mkv"),error);
        check(recorder != nullptr,"unsupported HEVC layer-header fixture starts"); if (!recorder) continue;
        begin(*recorder,false);
        Nal invalid = idr; invalid[1] = header;
        check(!recorder->video(video_session,0,0,{invalid},vps,sps,pps),
              "zero temporal ID, other temporal layer and other base layer are rejected");
        check(!recorder->finish(error),"invalid HEVC headers cannot become a successful file");
    }
}

void delayed_au_history(const Directory& directory) {
    std::string error;
    for (const auto [last_second, success] : {std::pair<uint32_t,bool>{3,true},{12,false}}) {
        const auto path = directory.path / (success ? "delayed-au.mkv" : "evicted-au.mkv");
        auto recorder = Recorder::start(options(path),error);
        check(recorder != nullptr,"delayed in-order AU fixture starts"); if (!recorder) continue;
        begin(*recorder,false); video(*recorder,0);
        report(*recorder,Track::Video,sr(0,0,1000));
        report(*recorder,Track::Video,sr(0,24000,1001));
        for (uint32_t second = 2; second <= last_second; ++second)
            report(*recorder,Track::Video,sr(0,second*24000,1000+second));
        // AU sampling time is 0.1s but completion follows SR 3s: 2.9s late,
        // strictly in-order after the preceding IDR. SR arrivals alone must not
        // discard its recent interval. A sample older than retained history
        // still fails explicitly, rather than extrapolating into evicted time.
        video(*recorder,2400);
        check(recorder->finish(error)==success,
              "recent SR history permits bounded delayed AU and rejects evicted intervals");
        if (success) {
            const auto file=read(path); const std::array<int64_t,2> pts{0,100000}; verify_video(file,pts);
        } else check(!error.empty(),"an evicted historical AU supplies a clear failure");
    }
}

void failure_paths(const Directory& directory) {
    std::string error;
    {
        const auto path = directory.path / "never-opened.mkv";
        { std::ofstream existing(path); existing << "existing content"; }
        auto config = options(path); config.clock_wait = 40ms;
        auto recorder = Recorder::start(config,error); check(recorder != nullptr,"missing-SR fixture starts");
        if (recorder) {
            begin(*recorder,false); video(*recorder,0);
            check(wait_for_error(*recorder),"worker detects missing SR even without a subsequent input");
            const auto first = recorder->error(); recorder->fail("second error");
            check(!recorder->finish(error) && error == first,"timeout preserves the first failure through finish");
        }
        std::ifstream existing(path); std::string bytes((std::istreambuf_iterator<char>(existing)),{});
        check(bytes == "existing content","unapproved clocks never truncate an existing output path");
    }
    {
        auto recorder = Recorder::start(options(directory.path / "no-media.mkv"),error);
        check(recorder && !recorder->finish(error),"no approved media is a clear recording failure");
    }
    {
        auto config = options(directory.path / "budget.mkv"); config.encoded_budget = 32;
        auto recorder = Recorder::start(config,error); check(recorder != nullptr,"small-budget fixture starts");
        if (recorder) {
            begin(*recorder,false);
            check(!recorder->video(video_session,0,0,{idr},vps,sps,pps),"bounded admission rejects a packet without waiting for space");
            recorder->fail("later");
            check(!recorder->finish(error) && error.find("budget") != std::string::npos,"budget failure survives sealing and finalization");
        }
    }
    {
        auto config = options(directory.path / "two-producers.mkv", true); config.encoded_budget = 1024;
        auto recorder = Recorder::start(config,error);
        check(recorder != nullptr,"shared-budget two-producer fixture starts");
        if (recorder) {
            begin(*recorder,true);
            unsigned video_admitted = 0, audio_admitted = 0;
            std::thread video_producer([&] {
                for (int64_t i = 0; i < 64; ++i) {
                    if (!recorder->video(video_session,0,i*240,{idr},vps,sps,pps)) break;
                    ++video_admitted;
                }
            });
            std::thread audio_producer([&] {
                for (uint32_t i = 0; i < 256; ++i) {
                    if (!recorder->audio(audio_session,9,i*480,silence)) break;
                    ++audio_admitted;
                }
            });
            video_producer.join(); audio_producer.join();
            check(video_admitted + audio_admitted > 0,"simultaneous producers can admit bounded copies");
            check(!recorder->finish(error) && error.find("budget") != std::string::npos,
                  "video/audio share one bound and seal instead of waiting for SR or queue space");
            check(!std::filesystem::exists(config.path),"budget overflow before trusted clocks does not open an output file");
        }
    }
    for (int kind = 0; kind < 4; ++kind) {
        auto recorder = Recorder::start(options(directory.path / ("failure-" + std::to_string(kind))),error);
        check(recorder != nullptr,"configuration/session/source/clock failure fixture starts"); if (!recorder) continue;
        begin(*recorder,false); video(*recorder,0);
        report(*recorder,Track::Video,sr(0,0,1000)); report(*recorder,Track::Video,sr(0,24000,1001));
        if (kind == 0) { Nal changed = pps; changed.back() ^= 1; check(recorder->video(video_session,0,240,{idr},vps,sps,changed),"changed parameters admitted for worker validation"); }
        if (kind == 1) check(recorder->video(audio_session,0,240,{idr},vps,sps,pps),"changed session admitted for worker validation");
        if (kind == 2) video(*recorder,240,1);
        if (kind == 3) report(*recorder,Track::Video,sr(0,240000,1010));
        check(!recorder->finish(error) && !error.empty(),"actual configuration/session/source/clock change seals the whole recording");
        const auto first = error; check(!recorder->finish(error) && error == first,"failure finalization is idempotent");
    }
    {
        auto recorder = Recorder::start(options(directory.path / "missing" / "output.mkv"),error);
        check(recorder != nullptr,"file-open error is deferred until approved first media");
        if (recorder) {
            begin(*recorder,false); video(*recorder,0);
            report(*recorder,Track::Video,sr(0,0,1000)); report(*recorder,Track::Video,sr(0,24000,1001));
            check(!recorder->finish(error),"a real local output-open failure reaches final status");
        }
    }
}
#endif
} // namespace

int main() {
    Directory directory;
    std::string error;
    for (const int degrees : {-90, 45, 360}) {
        auto config = options(directory.path / "invalid-orientation.mp4", false,
                              scrctl::media::RecordingMuxer::Format::Mp4);
        config.video_orientation = degrees;
        check(!Recorder::start(config, error) && !error.empty(),
              "invalid Recorder orientation is rejected before creating a worker");
        check(!std::filesystem::exists(config.path), "invalid Recorder orientation creates no output");
    }
    if (!scrctl::media::RecordingMuxer::available()) {
        auto rotated = options(directory.path / "unavailable-rotation.mp4", false,
                               scrctl::media::RecordingMuxer::Format::Mp4);
        rotated.video_orientation = 90;
        check(!Recorder::start(rotated, error) && !error.empty() && !std::filesystem::exists(rotated.path),
              "without the muxing library Recorder refuses rotation without creating output");
        check(!Recorder::start(options(directory.path / "unavailable.mkv"),error),"missing libavformat rejects MKV explicitly");
        check(!error.empty(),"unavailable Recorder provides a reason");
        check(!std::filesystem::exists(directory.path / "unavailable.mkv"),"unavailable Recorder does not create an output file");
    } else {
#ifdef SCRCTL_HAVE_LIBAVFORMAT
        common_origin(directory);
        recorded_orientation(directory);
        wrapping_audio_and_first_idr(directory);
        still_video(directory);
        still_video(directory, scrctl::media::RecordingMuxer::Format::Mp4);
        mp4_video_timing(directory);
        mp4_failure_paths(directory);
        final_mapping(directory);
        early_reports_and_parameter_epoch(directory);
        delayed_au_history(directory);
        failure_paths(directory);
        auto invalid = options(directory.path / "invalid.mkv"); invalid.clock_wait = 5001ms;
        check(!Recorder::start(invalid,error),"clock budget cannot exceed the first-version bound");
        invalid = options(directory.path / "invalid.mkv"); invalid.encoded_budget = 16u*1024u*1024u+1;
        check(!Recorder::start(invalid,error),"encoded budget cannot exceed 16 MiB");
        invalid = options(directory.path / "invalid.mp4");
        invalid.format = static_cast<scrctl::media::RecordingMuxer::Format>(99);
        check(!Recorder::start(invalid,error) && !error.empty(), "an invalid Recorder container enum is rejected before a worker starts");
#endif
    }
    std::printf("recorder: %d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
