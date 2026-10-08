#include "media/RecordingVideoConfig.h"

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
}  // namespace

int main() {
    valid_configurations();
    missing_and_invalid_headers();
    unsupported_layers_and_bounded_input();
    std::printf("recording video config: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
