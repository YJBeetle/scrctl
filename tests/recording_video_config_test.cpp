#include "media/RecordingVideoConfig.h"
#include "media/RecordingMuxer.h"

#include <array>
#include <cstdio>
#include <string_view>
#include <utility>

namespace {
using Config = scrctl::media::RecordingVideoConfig;
using Status = Config::Status;
using Parameters = std::array<scrctl::Nal, 3>;
int checks = 0;
int failures = 0;

void check(bool value, const char* message) {
    ++checks;
    if (!value) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

scrctl::Nal hex(std::string_view text) {
    scrctl::Nal bytes;
    for (std::size_t i = 0; i < text.size(); i += 2) {
        unsigned byte = 0;
        std::sscanf(text.data() + i, "%2x", &byte);
        bytes.push_back(static_cast<uint8_t>(byte));
    }
    return bytes;
}

// 三组仅包含参数，不含媒体帧/SEI 或设备身份；保留原始 EPB。
// real: 2026-10-08 USB capture, SPS 导出 1136x2464、重排序限额 0。
// 合成: FFmpeg 9.0.1 + x265 4.3+1-e9b8812 (build 217), 64x64 / 30fps,
// keyint=25 / rc-lookahead=5，各用 bframes=0 和 bframes=3。trace_headers
// 独立确认最高 temporal layer 的 sps_max_num_reorder_pics 分别为 0 和 2；
// bframes 数不是重排序深度。测试只需要这些固定参数，不运行编码器。
// 参数串联 SHA256 (VPS||SPS||PPS):
// real 756bb92a916fb73d77b4beeb367e08b4ebf7650032711dacdfde72818f30e835
// bf0  b9e65f9733c167c24363eb72b255578813a34807d322ecfb5d12ae084236fe56
// bf3  0394e66b22d07b5e4c2f98866a8d5cb62de5f70ad3c5d1bbf7e49674ed90852c
const Parameters real{
    hex("40011c01ffff016000000300b000000300000300960cc090"),
    hex("420111016000000300b0000003000003009648008e2002685880cee45218b9fc4fc2fe86fd43fea823f5504feaa82bf55505feaaa833f555529b8101010040"),
    hex("440148072f05b240")};
const Parameters bf0{
    hex("40010c01ffff01600000030090000003000003001eba0240"),
    hex("42010101600000030090000003000003001ea020810596e92930bc05a020000003002000000303c1"),
    hex("4401c073c089")};
const Parameters bf3{
    hex("40010c01ffff01600000030090000003000003001e959409"),
    hex("42010101600000030090000003000003001ea0208105965654a4c2f0168080000003008000000f04"),
    hex("4401c073c089")};

Config inspect(const Parameters& parameters) {
    return scrctl::media::inspect_recording_video_config(
        parameters[0], parameters[1], parameters[2]);
}

void rejected(const Parameters& parameters, Status status, const char* message) {
    const auto result = inspect(parameters);
    check(result.status == status, message);
    check(!result.permits_equal_dts_pts() && result.reorder_depth == -1 &&
              result.width == 0 && result.height == 0 && !result.error.empty(),
          "rejected configuration never exposes default zero as no-reorder evidence");
    check(result.vps.empty() && result.sps.empty() && result.pps.empty(),
          "rejected configuration has no approved parameter snapshot");
}

void valid_configurations() {
    const Config initial;
    check(initial.status == Status::Invalid && initial.reorder_depth == -1 &&
              !initial.permits_equal_dts_pts(), "default result does not permit DTS=PTS");
#ifdef SCRCTL_HAVE_LIBAV
    const auto captured = inspect(real);
    check(captured.status == Status::NoReorder && captured.permits_equal_dts_pts() &&
              captured.reorder_depth == 0 && captured.width == 1136 && captured.height == 2464 &&
              captured.error.empty(), "real USB parameters export valid dimensions and no reorder");
    check(captured.vps == real[0] && captured.sps == real[1] && captured.pps == real[2],
          "raw real parameter bytes and EPB are preserved");
    const auto low_delay = inspect(bf0);
    check(low_delay.status == Status::NoReorder && low_delay.permits_equal_dts_pts() &&
              low_delay.reorder_depth == 0 && low_delay.width == 64 && low_delay.height == 64,
          "independent x265 bframes=0 parameter set permits equal DTS and PTS");
    const auto reordered = inspect(bf3);
    check(reordered.status == Status::Reorder && !reordered.permits_equal_dts_pts() &&
              reordered.reorder_depth == 2 && reordered.width == 64 && reordered.height == 64 &&
              reordered.error.empty(), "independent x265 bframes=3 parameter set requires reorder depth 2");
    check(reordered.vps == bf3[0] && reordered.sps == bf3[1] && reordered.pps == bf3[2],
          "a reordered configuration still owns its exact checked parameters");
    check(inspect(bf0).status == Status::NoReorder,
          "a fresh context does not inherit reorder state from the preceding configuration");
    auto changed = bf0;
    const auto snapshot = inspect(changed);
    changed[1][2] |= 2;
    check(snapshot.sps == bf0[1] && snapshot.permits_equal_dts_pts(),
          "returned snapshot owns the inspected bytes after caller input changes");
    rejected(changed, Status::Unsupported,
             "changed temporal configuration cannot reuse a previous successful result");
#else
    for (const auto& parameters : {real, bf0, bf3}) {
        rejected(parameters, Status::Unsupported,
                 "without libav no valid-looking configuration permits DTS=PTS");
    }
#endif
}

void missing_and_invalid_headers() {
    rejected({}, Status::Invalid, "empty extradata is not a valid configuration");
    for (std::size_t i = 0; i < bf0.size(); ++i) {
        auto parameters = bf0;
        parameters[i].clear();
        rejected(parameters, Status::Invalid, "every parameter type is required");
        parameters = bf0;
        parameters[i].resize(2);
        rejected(parameters, Status::Invalid, "a NAL header without payload is rejected");
        parameters = bf0;
        parameters[i][0] |= 0x80;
        rejected(parameters, Status::Invalid, "forbidden_zero_bit must be zero");
        parameters = bf0;
        parameters[i][1] &= 0xf8;
        rejected(parameters, Status::Invalid, "temporal_id_plus1 must not be zero");
        parameters = bf0;
        parameters[i][0] = 2;
        rejected(parameters, Status::Invalid, "parameter slot requires its exact NAL type");
        parameters = bf0;
        parameters[i].insert(parameters[i].begin(), {0, 0, 0, 1});
        rejected(parameters, Status::Invalid, "input requires raw NAL without an Annex-B prefix");
        parameters = bf0;
        parameters[i].insert(parameters[i].end(), {0, 0, 0, 1});
        parameters[i].insert(parameters[i].end(), bf3[i].begin(), bf3[i].end());
        rejected(parameters, Status::Invalid, "a concatenated second parameter set is rejected");
    }
    auto parameters = bf0;
    std::swap(parameters[0], parameters[1]);
    rejected(parameters, Status::Invalid, "VPS and SPS cannot be swapped");
    parameters = bf0;
    parameters[0].resize(3);
    rejected(parameters, Status::Invalid, "VPS fixed layer fields must be present");

    for (std::size_t i = 0; i < bf0.size(); ++i) {
        parameters = bf0;
        parameters[i].resize(3);
#ifdef SCRCTL_HAVE_LIBAV
        rejected(parameters, Status::Invalid, "public HEVC syntax gate rejects truncated parameter syntax");
#else
        rejected(parameters, i == 0 ? Status::Invalid : Status::Unsupported,
                 "without libav truncated syntax cannot become no-reorder evidence");
#endif
    }
    parameters = real;
    parameters[1].resize(parameters[1].size() / 2);
#ifdef SCRCTL_HAVE_LIBAV
    rejected(parameters, Status::Invalid, "real SPS truncated halfway fails public syntax validation");
#else
    rejected(parameters, Status::Unsupported, "no-libav build cannot approve a truncated SPS");
#endif
}

void unsupported_layers_and_bounded_input() {
    for (std::size_t i = 0; i < bf0.size(); ++i) {
        auto parameters = bf0;
        parameters[i][0] |= 1;
        rejected(parameters, Status::Unsupported, "nonzero high layer bits are unsupported");
        parameters = bf0;
        parameters[i][1] |= 8;
        rejected(parameters, Status::Unsupported, "nonzero low layer bits are unsupported");
        parameters = bf0;
        parameters[i][1] = 2;
        rejected(parameters, Status::Unsupported, "higher temporal NAL IDs are unsupported");
    }
    auto parameters = bf0;
    parameters[0][2] |= 1;
    rejected(parameters, Status::Unsupported, "VPS high max_layers bits reject multilayer configuration");
    parameters = bf0;
    parameters[0][3] |= 16;
    rejected(parameters, Status::Unsupported, "VPS low max_layers bits reject multilayer configuration");
    parameters = bf0;
    parameters[0][3] |= 2;
    rejected(parameters, Status::Unsupported, "VPS temporal sublayers are unsupported");
    parameters = bf0;
    parameters[1][2] |= 2;
    rejected(parameters, Status::Unsupported, "SPS temporal sublayers are unsupported");
    parameters = bf0;
    parameters[0].resize(scrctl::media::kMaxRecordingVideoParameterBytes + 1);
    rejected(parameters, Status::Invalid, "one oversized parameter is rejected before allocation");
    parameters = bf0;
    parameters[0].resize(scrctl::media::kMaxRecordingVideoParameterBytes / 2 + 1);
    parameters[1].resize(scrctl::media::kMaxRecordingVideoParameterBytes / 2);
    rejected(parameters, Status::Invalid, "aggregate parameter size has the same bounded budget");
}

void idr_syntax_checks() {
    using IdStatus = scrctl::media::RecordingIdrSyntax::Status;
    std::string capability_error = "stale";
#ifdef SCRCTL_HAVE_LIBAV
    check(scrctl::media::recording_idr_checks_available(capability_error) && capability_error.empty(),
          "public software-configuration and BSF capabilities can be checked before device startup");
    // 已有recorder_test的64x64、单层无B帧配置和原IDR；此测试不调用encoder或decoder。
    const auto idr = hex("2801ac21800e7ffeebf349ac");
    auto config = inspect(bf0);
    const std::vector<scrctl::Nal> picture{idr};
    const auto original = picture;
    auto accepted = scrctl::media::inspect_recording_idr(picture, config);
    check(accepted.status == IdStatus::Valid && accepted.valid() && accepted.error.empty(),
          "actual public HEVC BSF accepts an original type20 IDR with current parameters");
    check(picture == original && config.vps == bf0[0] && config.sps == bf0[1] && config.pps == bf0[2],
          "IDR syntax filtering does not replace caller AU or parameter bytes with rewritten output");
    auto type19 = idr; type19[0] = 19 << 1;
    check(scrctl::media::inspect_recording_idr({type19}, config).valid(),
          "the same independent slice syntax is accepted with actual type19 IDR semantics");
    check(scrctl::media::inspect_recording_idr({bf0[0], bf0[1], bf0[2], idr}, config).valid(),
          "matching raw parameter prefixes do not invalidate the current IDR");

    // FFmpeg9.0.2/libx265，black 256x256单帧，bframes=0:slices=2:keyint=25:
    // repeat-headers=1:pools=1:frame-threads=1:wpp=1。独立解码无诊断；固定参数
    // 与两个slice，不在测试中启动encoder，也不修改first-slice或slice address。
    const Parameters two_slice_parameters{
        hex("40010c01ffff01600000030090000003000003003c928090"),
        hex("42010101600000030090000003000003003ca008080405964a924caf016808000003000800000300f040"),
        hex("4401c172b44240")};
    const std::vector<scrctl::Nal> two_slices{
        hex("2801af082274f929e3ffecb5957fd0d4d6119054c0a90dc806e8e78880"),
        hex("280130f0822740f929e3ffecb5957fd0d4d6119054c0a90dc806e8e78880")};
    const auto two_slice_config = inspect(two_slice_parameters);
    check(two_slice_config.permits_equal_dts_pts() && two_slice_config.width == 256 &&
          two_slice_config.height == 256, "independent two-slice fixture has a valid current configuration");
    check(scrctl::media::inspect_recording_idr(two_slices, two_slice_config).valid(),
          "actual public BSF accepts first slice followed by a complete continuation slice");

    const auto denied = [&](const std::vector<scrctl::Nal>& nals, const Config& cfg,
                            IdStatus status, const char* label) {
        const auto snapshot = nals;
        const auto result = scrctl::media::inspect_recording_idr(nals, cfg);
        check(result.status == status && !result.valid() && !result.error.empty(), label);
        check(nals == snapshot, "rejected IDR keeps its original input bytes");
    };
    denied({}, config, IdStatus::Invalid, "empty AU cannot make encoded capture ready");
    denied({bf0[0], bf0[1], bf0[2]}, config, IdStatus::Invalid,
           "parameter-only AU does not contain an independent IDR picture");
    denied(picture, {}, IdStatus::Invalid, "a default unchecked configuration cannot approve an IDR");
    auto changed = config; changed.pps.clear();
    denied(picture, changed, IdStatus::Invalid, "missing PPS cannot be hidden by a previously valid status");
    changed = config; changed.sps.resize(3);
    denied(picture, changed, IdStatus::Invalid, "truncated current parameter syntax is rejected by the actual public BSF");
    auto altered_prefix = bf0[2]; altered_prefix.back() ^= 1;
    denied({altered_prefix, idr}, config, IdStatus::Invalid,
           "AU parameter bytes must match the supplied current configuration");
    denied({idr, bf0[1]}, config, IdStatus::Invalid,
           "next-picture parameter prefixes cannot trail the already complete IDR");
    auto broken = idr; broken.resize(2);
    denied({broken}, config, IdStatus::Invalid, "raw NAL header alone is not a slice");
    for (const std::size_t bytes : {3u, 4u, 5u}) {
        broken = idr; broken.resize(bytes);
        denied({broken}, config, IdStatus::Invalid,
               "transport-complete but truncated IDR slice syntax never passes BSF validation");
    }
    // first_slice=1、no_output_of_prior_pics=0后，ue(v)的010指向PPS1；配置只有PPS0。
    broken = idr; broken[2] = 0x94;
    denied({broken}, config, IdStatus::Invalid,
           "actual slice-header parsing rejects a reference to an unavailable PPS");
    broken = idr; broken[2] &= 0x7f;
    denied({broken}, config, IdStatus::Invalid, "orphan continuation slice cannot open an IDR AU");
    denied({idr, idr}, config, IdStatus::Invalid, "two first slices are two pictures, not one complete AU");
    for (const uint8_t type : {0, 16, 17, 18, 21, 22}) {
        broken = idr; broken[0] = type << 1;
        denied({broken}, config, IdStatus::Invalid, "P/B/CRA/BLA or reserved IRAP cannot stand in for type19/20 IDR");
        broken[2] &= 0x7f;
        denied({idr, broken}, config, IdStatus::Invalid, "mixing IDR with another VCL type cannot approve an AU");
    }
    broken = type19; broken[2] &= 0x7f;
    denied({idr, broken}, config, IdStatus::Invalid, "type19 and type20 cannot be mixed within a single IDR picture");
    broken = idr; broken[0] |= 0x80;
    denied({broken}, config, IdStatus::Invalid, "forbidden_zero_bit rejects even a complete-looking IDR");
    broken = idr; broken[1] = 0;
    denied({broken}, config, IdStatus::Invalid, "zero temporal_id_plus1 rejects the NAL header");
    for (const bool high : {false, true}) {
        broken = idr; if (high) broken[0] |= 1; else broken[1] |= 8;
        denied({broken}, config, IdStatus::Unsupported, "non-base layer IDR is outside the recording contract");
    }
    broken = idr; broken[1] = 2;
    denied({broken}, config, IdStatus::Unsupported, "higher temporal layer IDR is outside the recording contract");
    broken = idr; broken.insert(broken.end(), {0,0,1});
    broken.insert(broken.end(), idr.begin(), idr.end());
    denied({broken}, config, IdStatus::Invalid, "an embedded Annex-B separator cannot smuggle a second raw NAL");
    changed = config; changed.vps.resize(scrctl::media::kMaxRecordingVideoParameterBytes + 1);
    denied(picture, changed, IdStatus::Invalid, "oversized current parameters fail before BSF allocation");
    changed = config; changed.vps.resize(scrctl::media::kMaxRecordingVideoParameterBytes / 2);
    changed.sps.resize(scrctl::media::kMaxRecordingVideoParameterBytes / 2);
    denied(picture, changed, IdStatus::Invalid, "aggregate current parameter bytes preserve the 1MiB budget");
    broken = idr; broken.resize(scrctl::media::kMaxRecordingMuxerPacketBytes - 3, 0x55);
    denied({broken}, config, IdStatus::Invalid, "single IDR plus Annex-B prefix exceeding packet budget fails before allocation");
    broken = idr; broken.resize(scrctl::media::kMaxRecordingMuxerPacketBytes / 2, 0x55);
    auto continuation = broken; continuation[2] &= 0x7f;
    denied({broken, continuation}, config, IdStatus::Invalid,
           "aggregate AU budget includes every four-byte start code before allocation");
    denied(std::vector<scrctl::Nal>(scrctl::media::kMaxRecordingIdrNals + 1, idr), config,
           IdStatus::Invalid, "excessive tiny NAL count fails before CBS allocates unit metadata");
#else
    check(!scrctl::media::recording_idr_checks_available(capability_error) && !capability_error.empty(),
          "without libav the required syntax capability fails before any device request");
    const auto unavailable = inspect(bf0);
    const auto result = scrctl::media::inspect_recording_idr({hex("2801ac21800e7ffeebf349ac")}, unavailable);
    check(result.status == IdStatus::Unsupported && !result.valid() && !result.error.empty(),
          "without actual BSF an IDR never becomes successful encoded health");
#endif
}
}  // namespace

int main() {
    valid_configurations();
    missing_and_invalid_headers();
    unsupported_layers_and_bounded_input();
    idr_syntax_checks();
    std::printf("recording video config: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
