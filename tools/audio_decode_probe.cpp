// 探针：把音频腿的 dump 解成 WAV，判"这条流到底解不解得开、解出来是不是 480 一帧"。
//
// 为什么先做这一步再接产品：音频这条路上有三个可能各自错的环节——后端认不认这份
// 参数、"一包一帧"这个假设、以及输出采样格式。三个串起来之后"没声"这件事完全没有
// 信息量。所以先在离线把前两个判掉：输入是一份**录下来的** dump，输出是一个 WAV。
//
// 判据为什么是"数量"：ELD 一帧固定 480 采样/声道，所以 N 个包必然解出约 N*480*声道
// 个采样。参数错一档（比如被当成 LC 的 1024）在数字上是两倍的差，不是"听起来有点不对"。
//
// 用法：audio_decode_probe IN.rtp OUT.wav [采样率] [声道]
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "decode/AudioDecoder.h"

namespace {

/// dump 的形状：`[u16 大端长度][整条 UDP 数据报原文]` 顺序排
/// （rr_keepalive_probe 的 --audio-out 就是这么写的）。
bool load(const char *path, std::vector<std::vector<uint8_t>> &out, std::string &err) {
    std::FILE *f = std::fopen(path, "rb");
    if (f == nullptr) {
        err = std::string("打不开 ") + path;
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long total = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> all(static_cast<std::size_t>(total < 0 ? 0 : total));
    const std::size_t got = std::fread(all.data(), 1, all.size(), f);
    std::fclose(f);
    all.resize(got);
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

/// 剥 RTP 头。这条流的音频包实测没有扩展头也没有 CSRC（cc=0、X=0），但这里仍然按
/// 头里的字段算，而不是直接跳 12 字节——写死的那一版一旦遇到带 CSRC 的流会把 CSRC
/// 当载荷解，症状是"每帧都解不出"，很难往"跳多了 4 字节"上想。
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

void write_wav(const char *path, const std::vector<int16_t> &pcm, int rate, int channels) {
    const uint32_t data_bytes = static_cast<uint32_t>(pcm.size() * sizeof(int16_t));
    std::vector<uint8_t> h;
    h.insert(h.end(), {'R', 'I', 'F', 'F'});
    put_u32(h, 36 + data_bytes);
    h.insert(h.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put_u32(h, 16);
    put_u16(h, 1);  // PCM
    put_u16(h, static_cast<uint16_t>(channels));
    put_u32(h, static_cast<uint32_t>(rate));
    put_u32(h, static_cast<uint32_t>(rate * channels * 2));
    put_u16(h, static_cast<uint16_t>(channels * 2));
    put_u16(h, 16);
    h.insert(h.end(), {'d', 'a', 't', 'a'});
    put_u32(h, data_bytes);
    std::FILE *f = std::fopen(path, "wb");
    if (f == nullptr) {
        std::fprintf(stderr, "写 %s 失败\n", path);
        return;
    }
    std::fwrite(h.data(), 1, h.size(), f);
    std::fwrite(pcm.data(), sizeof(int16_t), pcm.size(), f);
    std::fclose(f);
}

}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) {
        std::fprintf(stderr, "用法: %s IN.rtp OUT.wav [采样率] [声道]\n", argv[0]);
        return 2;
    }
    const int rate = argc > 3 ? std::atoi(argv[3]) : 48000;
    const int channels = argc > 4 ? std::atoi(argv[4]) : 2;

    std::vector<std::vector<uint8_t>> dgrams;
    std::string err;
    if (!load(argv[1], dgrams, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    std::printf("读入 %zu 条数据报，目标 %d Hz / %d 声道\n", dgrams.size(), rate, channels);

    auto dec = scrctl::create_audio_decoder(rate, channels, 480, err);
    if (dec == nullptr) {
        std::fprintf(stderr, "建音频解码器失败: %s\n", err.c_str());
        return 1;
    }
    std::printf("音频后端: %s\n", dec->backend_name());

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
            ++other_pt;  // 同端口上混来的 RTCP
            continue;
        }
        const std::size_t before = pcm.size();
        std::string derr;
        if (!dec->decode(payload, pcm, derr)) {
            if (failed < 5) {
                std::fprintf(stderr, "  第 %llu 帧解不出（%zu 字节）: %s\n",
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

    // 每段的峰值分开打：整段一个峰值说明不了什么，而"前 1/4 有声、后面全零"这种
    // 形状才是"解码器只认了开头那几帧然后就哑了"的证据。
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
    std::printf("解出 %llu 帧 / 失败 %llu / 空载荷 %llu / 非 101 PT %llu\n",
                static_cast<unsigned long long>(frames), static_cast<unsigned long long>(failed),
                static_cast<unsigned long long>(empty),
                static_cast<unsigned long long>(other_pt));
    std::printf("采样 %zu 个 = %.2f 秒；峰值 %lld / 32767；四段峰值", n,
                channels ? static_cast<double>(n) / (rate * channels) : 0.0,
                static_cast<long long>(peak));
    for (int q = 0; q < 4; ++q) {
        std::printf(" %lld", static_cast<long long>(band_peak(n * q / 4, n * (q + 1) / 4)));
    }
    std::printf("\n");
    if (frames > 0) {
        std::printf("每帧平均 %.1f 个采样/声道（期望 480）\n",
                    static_cast<double>(n) / static_cast<double>(frames) / channels);
    }
    write_wav(argv[2], pcm, rate, channels);
    std::printf("已写 %s\n", argv[2]);
    return failed == 0 && frames > 0 && peak > 100 ? 0 : 1;
}
