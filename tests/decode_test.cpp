// 解码链路自检：解析 .hevc -> VideoToolbox 硬解 -> 导出指定帧的原始 BGRA。
//
// 目的有两个：证明参数集/长度前缀/EPB 这套组装方式真能被硬解吃下去，
// 以及给上层渲染一个可以肉眼核对的画面（导出后转成 PNG 人工检查裁剪与色彩）。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "bitstream/AnnexB.h"
#include "decode/Decoder.h"

namespace {

uint8_t nal_type(const scrctl::Nal &n) { return n.size() >= 2 ? uint8_t((n[0] >> 1) & 0x3F) : 0xFF; }

}  // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "用法: %s <in.hevc> <out.raw> [帧序号]\n", argv[0]);
        return 2;
    }
    const int want = argc > 3 ? std::atoi(argv[3]) : 30;

    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "打不开 %s\n", argv[1]);
        return 2;
    }
    std::vector<uint8_t> file{(std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()};

    auto decoder = scrctl::create_platform_decoder();
    std::printf("解码后端: %s\n", decoder->backend_name());

    bool configured = false;
    int au_count = 0;
    int produced = 0;
    int failed = 0;
    scrctl::Frame target;
    scrctl::Frame tmp;

    scrctl::AnnexBParser parser([&](std::vector<scrctl::Nal> &&au, bool keyframe) {
        (void)keyframe;
        ++au_count;

        if (!configured) {
            scrctl::Nal vps, sps, pps;
            for (const auto &n : au) {
                switch (nal_type(n)) {
                    case 32: vps = n; break;
                    case 33: sps = n; break;
                    case 34: pps = n; break;
                    default: break;
                }
            }
            if (vps.empty() || sps.empty() || pps.empty()) {
                return;
            }
            if (!decoder->configure(vps, sps, pps)) {
                std::fprintf(stderr, "configure 失败于 AU#%d\n", au_count);
                std::exit(1);
            }
            configured = true;
            std::printf("已在 AU#%d 建立会话\n", au_count);
        }

        tmp = scrctl::Frame{};
        if (!decoder->decode(au, tmp) || !tmp) {
            ++failed;
            if (failed <= 20) {
                std::printf("  [fail] AU#%d nals=%zu types=", au_count, au.size());
                for (const auto &n : au) {
                    std::printf("%u ", nal_type(n));
                }
                std::printf("\n");
            }
            return;
        }
        ++produced;
        if (produced == want) {
            target = std::move(tmp);
        }
    });

    parser.feed(file.data(), file.size());
    parser.flush();

    std::printf("AU=%d  出帧=%d  未出帧=%d  出帧率=%.1f%%\n", au_count, produced, failed,
                au_count ? 100.0 * produced / au_count : 0.0);

    if (!target) {
        std::fprintf(stderr, "没能取到第 %d 帧\n", want);
        return 1;
    }

    std::printf("目标帧: %ux%u pitch=%u  bytes=%zu\n", target.width, target.height,
                target.row_pitch, target.pixels.size());

    // 头部塞 16 字节元信息，方便下游脚本还原尺寸，不用命令行传参。
    std::ofstream out(argv[2], std::ios::binary);
    if (!out) {
        std::fprintf(stderr, "写不出 %s\n", argv[2]);
        return 1;
    }
    uint32_t hdr[4] = {target.width, target.height, target.row_pitch, target.bytes_per_pixel};
    out.write(reinterpret_cast<const char *>(hdr), sizeof(hdr));
    out.write(reinterpret_cast<const char *>(target.pixels.data()),
              static_cast<std::streamsize>(target.pixels.size()));
    std::printf("已导出 %s\n", argv[2]);
    return 0;
}
