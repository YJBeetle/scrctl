// 离线读取 rr_keepalive_probe --audio-out 保存的音频数据报，提取 PT 101 的 RTP
// 载荷并交给 AAC-ELD 后端，最后保存为交织、16 位整数 PCM 的 WAV。
//
// 当前设备配置按每帧每声道 480 个采样解码。帧数、平均采样数和分段峰值用于核对
// 这份录制的解码结果；它们不能单独证明其他设备或 ELD 配置也使用相同帧长。
// 默认采样率为 48000 Hz、双声道；参数和离线 dump 格式见 --help。
#include <CLI/CLI.hpp>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "decode/AudioDecoder.h"
#if defined(SCRCTL_HAVE_LIBAV)
#include "decode/FFmpegEldDecoder.h"
#endif
#include "i18n/CliLanguage.h"
#include "i18n/Translation.h"

namespace {

/// dump 依次保存 [u16 大端长度][完整 UDP 数据报]。
/// 保留原有有效前缀的读取方式：遇到零长或不完整记录后停止，不解释剩余尾部。
bool load(const char *path, std::vector<std::vector<uint8_t>> &out, std::string &err) {
    err.clear();
    std::FILE *f = std::fopen(path, "rb");
    if (f == nullptr) {
        err = std::string(SCRCTL_TR("Cannot open input file: ")) + std::strerror(errno);
        return false;
    }
    auto close_input = [&] {
        errno = 0;
        if (std::fclose(f) != 0 && err.empty()) {
            err = std::string(SCRCTL_TR("Failed to close input file: ")) +
                  std::strerror(errno == 0 ? EIO : errno);
        }
        return err.empty();
    };
    errno = 0;
    if (std::fseek(f, 0, SEEK_END) != 0) {
        err = std::string(SCRCTL_TR("Cannot seek input file: ")) +
              std::strerror(errno == 0 ? EIO : errno);
        close_input();
        return false;
    }
    errno = 0;
    const long total = std::ftell(f);
    if (total < 0) {
        err = std::string(SCRCTL_TR("Cannot determine input file size: ")) +
              std::strerror(errno == 0 ? EIO : errno);
        close_input();
        return false;
    }
    errno = 0;
    if (std::fseek(f, 0, SEEK_SET) != 0) {
        err = std::string(SCRCTL_TR("Cannot seek input file: ")) +
              std::strerror(errno == 0 ? EIO : errno);
        close_input();
        return false;
    }
    std::vector<uint8_t> all(static_cast<std::size_t>(total));
    errno = 0;
    const std::size_t got = std::fread(all.data(), 1, all.size(), f);
    if (std::ferror(f) != 0) {
        err = std::string(SCRCTL_TR("Failed to read input file: ")) +
              std::strerror(errno == 0 ? EIO : errno);
    } else if (got != all.size()) {
        // 这与 dump 内部的不完整尾记录不同：文件在测量长度后出现短读，
        // 当前读取不是完整快照，不能将它作为正常的有效前缀继续处理。
        err = SCRCTL_TR("Input file ended before its measured size was read");
    }
    if (!close_input()) return false;
    std::size_t i = 0;
    while (i + 2 <= all.size()) {
        const std::size_t n = (static_cast<std::size_t>(all[i]) << 8) | all[i + 1];
        i += 2;
        if (n == 0 || i + n > all.size()) {
            break;
        }
        out.emplace_back(all.begin() + static_cast<std::ptrdiff_t>(i),
                         all.begin() + static_cast<std::ptrdiff_t>(i + n));
        i += n;
    }
    return !out.empty();
}

/// 按 RTP 的 CSRC 数量和扩展长度定位载荷，不能固定跳过 12 字节。
/// 这里沿用工具原有的 RTPv2、CSRC 与扩展头处理，不增加新的解包规则。
std::vector<uint8_t> payload_of(const std::vector<uint8_t> &dgram, uint8_t &payload_type) {
    payload_type = 0;
    if (dgram.size() < 12) {
        return {};
    }
    const uint8_t b0 = dgram[0];
    if (((b0 >> 6) & 3) != 2) {
        return {};  // 不是 RTPv2
    }
    const bool has_ext = (b0 & 0x10) != 0;
    const int cc = b0 & 0x0F;
    payload_type = static_cast<uint8_t>(dgram[1] & 0x7F);
    std::size_t off = 12 + static_cast<std::size_t>(cc) * 4;
    if (has_ext) {
        if (dgram.size() < off + 4) {
            return {};
        }
        const std::size_t words = (static_cast<std::size_t>(dgram[off + 2]) << 8) | dgram[off + 3];
        off += 4 + words * 4;
    }
    if (off >= dgram.size()) {
        return {};
    }
    return std::vector<uint8_t>(dgram.begin() + static_cast<std::ptrdiff_t>(off), dgram.end());
}

void put_u32(std::vector<uint8_t> &out, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));  // WAV 是小端
    }
}

void put_u16(std::vector<uint8_t> &out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

bool write_wav(const char *path, const std::vector<int16_t> &pcm, int rate, int channels,
               std::string &err) {
    err.clear();
    // RIFF 的 chunk size 为 32 位，固定头部占 36 字节；先检查再做乘法，
    // 避免大样本数组被截断成较小长度，生成头部与内容不一致的文件。
    if (pcm.size() > (std::numeric_limits<uint32_t>::max() - 36u) / sizeof(int16_t)) {
        err = SCRCTL_TR("PCM data is too large for a RIFF/WAV file");
        return false;
    }
    const uint32_t data_bytes = static_cast<uint32_t>(pcm.size() * sizeof(int16_t));
    std::vector<uint8_t> h;
    h.insert(h.end(), {'R', 'I', 'F', 'F'});
    put_u32(h, 36 + data_bytes);
    h.insert(h.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put_u32(h, 16);
    put_u16(h, 1);  // PCM
    put_u16(h, static_cast<uint16_t>(channels));
    put_u32(h, static_cast<uint32_t>(rate));
    put_u32(h, static_cast<uint32_t>(static_cast<uint64_t>(rate) * channels * 2));
    put_u16(h, static_cast<uint16_t>(channels * 2));
    put_u16(h, 16);
    h.insert(h.end(), {'d', 'a', 't', 'a'});
    put_u32(h, data_bytes);
    std::FILE *f = std::fopen(path, "wb");
    if (f == nullptr) {
        err = std::string(SCRCTL_TR("Cannot open output file: ")) + std::strerror(errno);
        return false;
    }
    // fwrite 成功可能仅表示数据进入 stdio 缓冲，真正的写入错误也可能在 fclose
    // 刷新时出现。失败时仍关闭文件，并保留最先发生的错误，不能继续报告保存成功。
    errno = 0;
    if (std::fwrite(h.data(), 1, h.size(), f) != h.size()) {
        err = std::string(SCRCTL_TR("Failed to write WAV header: ")) +
              std::strerror(errno == 0 ? EIO : errno);
    } else {
        errno = 0;
        if (std::fwrite(pcm.data(), sizeof(int16_t), pcm.size(), f) != pcm.size()) {
            err = std::string(SCRCTL_TR("Failed to write PCM samples: ")) +
                  std::strerror(errno == 0 ? EIO : errno);
        }
    }
    errno = 0;
    if (std::fclose(f) != 0 && err.empty()) {
        err = std::string(SCRCTL_TR("Failed to close output file: ")) +
              std::strerror(errno == 0 ? EIO : errno);
    }
    return err.empty();
}

}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string input_path;
    std::string output_path;
    std::string backend = "auto";
    int rate = 48000;
    int channels = 2;
    CLI::App app{SCRCTL_N_("Decode a recorded AAC-ELD audio dump to a WAV file")};
    app.footer(SCRCTL_N_(
        "Input records are a 16-bit big-endian length followed by a UDP datagram, as saved by "
        "rr_keepalive_probe --audio-out. Only RTP payload type 101 is decoded, using 480 samples "
        "per channel per frame. Output is interleaved 16-bit PCM. Numeric limits protect WAV "
        "fields; they do not guarantee codec support. --help does not read or write files."));
    app.set_help_flag("-h,--help", SCRCTL_N_("Show help"));
    app.add_option("INPUT", input_path, SCRCTL_N_("Recorded audio dump path"))->required();
    app.add_option("OUTPUT", output_path, SCRCTL_N_("Output WAV path"))->required();
    app.add_option("RATE", rate, SCRCTL_N_("Sample rate in Hz (positive integer; default: 48000)"))
        ->check(CLI::Range(1, std::numeric_limits<int>::max()));
    app.add_option("CHANNELS", channels,
        SCRCTL_N_("Channel count (1-32767; default: 2)"))->check(CLI::Range(1, 32767));
    app.add_option("--backend", backend,
        SCRCTL_N_("Audio decoder backend: auto or ffmpeg (default: auto)"))
        ->check(CLI::IsMember({"auto", "ffmpeg"}));
    scrctl::i18n::CliLanguage language(app);
    if (auto code = language.parse(argc, argv)) return *code;
    // 16 位 PCM 的 blockAlign 已由声道上限保证；byteRate 的 32 位边界还需要
    // 联合检查采样率和声道数。使用 64 位乘法，且在打开输入文件前拒绝越界参数。
    if (static_cast<uint64_t>(rate) * channels * 2 > std::numeric_limits<uint32_t>::max()) {
        std::fprintf(stderr, "%s\n", SCRCTL_TR(
            "Invalid arguments: sample rate and channel count exceed the WAV byte-rate limit"));
        return 2;
    }

    std::vector<std::vector<uint8_t>> dgrams;
    std::string err;
    if (!load(input_path.c_str(), dgrams, err)) {
        if (err.empty()) err = SCRCTL_TR("Input dump contains no complete datagrams");
        std::fprintf(stderr, SCRCTL_TR("Failed to load '%s': %s\n"), input_path.c_str(), err.c_str());
        return 1;
    }
    std::printf(SCRCTL_TR("Loaded %zu datagrams; target format: %d Hz, %d channels\n"),
                dgrams.size(), rate, channels);

    std::unique_ptr<scrctl::AudioDecoder> dec;
    if (backend == "ffmpeg") {
#if defined(SCRCTL_HAVE_LIBAV)
        dec = scrctl::create_ffmpeg_eld_decoder(rate, channels, 480, err);
#else
        err = SCRCTL_TR("FFmpeg AAC-ELD decoding is unavailable in this build; enable libav support");
#endif
    } else {
        dec = scrctl::create_audio_decoder(rate, channels, 480, err);
    }
    if (dec == nullptr) {
        std::fprintf(stderr, SCRCTL_TR("Failed to create audio decoder: %s\n"), err.c_str());
        return 1;
    }
    std::printf(SCRCTL_TR("Audio decoder: %s\n"), dec->backend_name());

    std::vector<int16_t> pcm;
    uint64_t frames = 0;
    uint64_t empty = 0;
    uint64_t failed = 0;
    uint64_t other_pt = 0;
    int64_t peak = 0;
    for (const auto &d : dgrams) {
        uint8_t pt = 0;
        const auto payload = payload_of(d, pt);
        if (payload.empty()) {
            ++empty;
            continue;
        }
        if (pt != 101) {
            ++other_pt;  // 跳过该端口上 PT 不是 101 的包，包括 RTCP。
            continue;
        }
        const std::size_t before = pcm.size();
        std::string derr;
        if (!dec->decode(payload, pcm, derr)) {
            if (failed < 5) {
                std::fprintf(stderr, SCRCTL_TR("  Decode failed after %llu output frames (%zu bytes): %s\n"),
                             static_cast<unsigned long long>(frames), payload.size(),
                             derr.c_str());
            }
            ++failed;
            continue;
        }
        if (pcm.size() > before) {
            ++frames;
        }
        for (std::size_t i = before; i < pcm.size(); ++i) {
            const int64_t a = pcm[i] < 0 ? -pcm[i] : pcm[i];
            if (a > peak) {
                peak = a;
            }
        }
    }

    // 将输出样本平均分成四段统计峰值，可观察后半段是否持续产生非零 PCM。
    // 峰值为零也可能来自静音录制，不能单独据此认定后端解码失败。
    const std::size_t n = pcm.size();
    auto band_peak = [&](std::size_t from, std::size_t to) {
        int64_t p = 0;
        for (std::size_t i = from; i < to && i < n; ++i) {
            const int64_t a = pcm[i] < 0 ? -pcm[i] : pcm[i];
            if (a > p) {
                p = a;
            }
        }
        return p;
    };
    std::printf(SCRCTL_TR("Output frames: %llu; decode failures: %llu; empty payloads: %llu; other payload types: %llu\n"),
                static_cast<unsigned long long>(frames), static_cast<unsigned long long>(failed),
                static_cast<unsigned long long>(empty),
                static_cast<unsigned long long>(other_pt));
    std::printf(SCRCTL_TR("PCM samples: %zu (%.2f seconds); peak: %lld / 32767; quarter peaks:"), n,
                static_cast<double>(n) / (static_cast<double>(rate) * channels),
                static_cast<long long>(peak));
    for (int q = 0; q < 4; ++q) {
        std::printf(" %lld", static_cast<long long>(band_peak(n * q / 4, n * (q + 1) / 4)));
    }
    std::printf("\n");
    if (frames > 0) {
        std::printf(SCRCTL_TR("Average samples per channel per frame: %.1f (expected: 480)\n"),
                    static_cast<double>(n) / static_cast<double>(frames) / channels);
    }
    if (!write_wav(output_path.c_str(), pcm, rate, channels, err)) {
        std::fprintf(stderr, SCRCTL_TR("Failed to write WAV '%s': %s\n"), output_path.c_str(), err.c_str());
        return 1;
    }
    std::printf(SCRCTL_TR("Saved WAV: %s\n"), output_path.c_str());
    return failed == 0 && frames > 0 && peak > 100 ? 0 : 1;
}
