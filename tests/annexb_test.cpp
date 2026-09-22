// AnnexBParser 的无头自检：用真机录制的 HEVC 验证 AU 分组正确性。
//
// 关键断言是"分块不变性"——同一份码流按 64KB 喂和按 1 字节喂必须得到完全
// 相同的 AU 序列。起始码横跨 feed 边界是流式解析器最容易出错的地方，
// 而这类错误在整块解析时完全看不出来。
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "bitstream/AnnexB.h"

namespace {

int Failures = 0;

void check(bool ok, const std::string &what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) {
        ++Failures;
    }
}

uint8_t nal_type(const scrctl::Nal &n) { return n.size() >= 2 ? uint8_t((n[0] >> 1) & 0x3F) : 0xFF; }
bool is_vcl(uint8_t t) { return t <= 31; }

struct Summary {
    std::vector<size_t> nal_counts;
    std::vector<bool> keyframes;
    size_t total_nal_bytes = 0;
    size_t vcl_nals = 0;
    bool first_au_has_vps = false;
    bool first_au_has_sps = false;
    bool first_au_has_pps = false;
    bool saw_idr = false;
};

Summary run(const std::vector<uint8_t> &file, size_t chunk) {
    Summary s;
    size_t au_index = 0;
    scrctl::AnnexBParser parser([&](std::vector<scrctl::Nal> &&au, bool keyframe) {
        s.nal_counts.push_back(au.size());
        s.keyframes.push_back(keyframe);
        size_t vcl_in_au = 0;
        for (const auto &n : au) {
            s.total_nal_bytes += n.size();
            const uint8_t t = nal_type(n);
            if (is_vcl(t)) {
                ++vcl_in_au;
                ++s.vcl_nals;
            }
            if (t == 19 || t == 20 || t == 21) {
                s.saw_idr = true;
            }
            if (au_index == 0) {
                if (t == 32) s.first_au_has_vps = true;
                if (t == 33) s.first_au_has_sps = true;
                if (t == 34) s.first_au_has_pps = true;
            }
        }
        if (vcl_in_au == 0) {
            std::printf("  !! AU#%zu 不含任何 VCL NAL\n", au_index);
            ++Failures;
        }
        ++au_index;
    });

    for (size_t off = 0; off < file.size(); off += chunk) {
        const size_t n = std::min(chunk, file.size() - off);
        parser.feed(file.data() + off, n);
    }
    parser.flush();
    return s;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "用法: %s <file.hevc>\n", argv[0]);
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "打不开 %s\n", argv[1]);
        return 2;
    }
    std::vector<uint8_t> file{(std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()};
    std::printf("输入 %zu 字节\n", file.size());

    std::printf("\n== 整块解析 ==\n");
    Summary whole = run(file, file.size());
    std::printf("  AU 数=%zu  VCL NAL=%zu  NAL 总字节=%zu  关键帧=%zu\n",
                whole.nal_counts.size(), whole.vcl_nals, whole.total_nal_bytes,
                std::count(whole.keyframes.begin(), whole.keyframes.end(), true));

    check(!whole.nal_counts.empty(), "至少解出一个 AU");
    check(whole.first_au_has_vps && whole.first_au_has_sps && whole.first_au_has_pps,
          "首个 AU 含 VPS/SPS/PPS 前缀");
    check(whole.saw_idr, "码流中存在 IRAP/IDR 帧");
    check(whole.total_nal_bytes < file.size(), "NAL 总字节小于文件（起始码已被剥离）");

    std::printf("\n== 分块不变性 ==\n");
    for (size_t c : {size_t(1), size_t(3), size_t(7), size_t(4096)}) {
        Summary s = run(file, c);
        bool same = s.nal_counts == whole.nal_counts && s.keyframes == whole.keyframes &&
                    s.total_nal_bytes == whole.total_nal_bytes;
        check(same, "按 " + std::to_string(c) + " 字节喂入结果与整块一致 (AU=" +
                        std::to_string(s.nal_counts.size()) + ")");
    }

    std::printf("\n%s (失败 %d 项)\n", Failures == 0 ? "全部通过" : "存在失败", Failures);
    return Failures == 0 ? 0 : 1;
}
