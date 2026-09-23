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

/// 合成码流上的自检：不依赖录制文件，所以任何时候都跑。
///
/// 要守住的是"NAL 字节原样进出"。曾经解析器在 emit 之前去掉 emulation
/// prevention 字节，喂给 VideoToolbox 的长度前缀样本因此不再是码流里的字节；
/// 去掉之后 RBSP 里还可能凭空出现 00 00 01，是否踩到取决于内容——出错是概率性
/// 的，拿真机录屏反而看不出来。这里把 00 00 03 直接写进 NAL，断言它原样传出。
void test_epb_kept() {
    std::printf("\n== NAL 字节原样保留 ==\n");

    auto with_header = [](uint8_t type, const std::vector<uint8_t> &body) {
        std::vector<uint8_t> v = {static_cast<uint8_t>((type << 1) & 0x7E), 0x01};
        v.insert(v.end(), body.begin(), body.end());
        return v;
    };
    // 载荷里两处 00 00 03：后随 0x01/0x00 的按规范是插进来的防 emulation 字节，
    // 去掉就会改变 NAL 的字节数与内容。
    const std::vector<uint8_t> param_body{0xAA, 0x00, 0x00, 0x03, 0x01, 0xBB,
                                          0x00, 0x00, 0x03, 0x00, 0xCC};
    // first_slice_segment_in_pic_flag 是 NAL 头之后的第一个 bit，所以取 0x80 开头。
    // 结尾刻意不是 0x00：末尾 0 与后继起始码之间的归属本就模糊，不该拿来断言。
    const std::vector<uint8_t> slice_body{0x80, 0x01, 0x00, 0x00, 0x03, 0x02, 0x55};

    std::vector<std::vector<uint8_t>> want;
    want.push_back(with_header(32, param_body));
    want.push_back(with_header(33, param_body));
    want.push_back(with_header(34, param_body));
    want.push_back(with_header(19, slice_body));

    static const std::vector<uint8_t> kStart{0, 0, 0, 1};
    std::vector<uint8_t> stream;
    for (const auto &nal : want) {
        stream.insert(stream.end(), kStart.begin(), kStart.end());
        stream.insert(stream.end(), nal.begin(), nal.end());
    }
    // 再加一个起始码把最后一个 NAL 定界（也顺便覆盖"参数集不单独成 AU"的规则）。
    stream.insert(stream.end(), kStart.begin(), kStart.end());

    std::vector<std::vector<uint8_t>> got;
    size_t aus = 0;
    scrctl::AnnexBParser parser([&](std::vector<scrctl::Nal> &&au, bool keyframe) {
        ++aus;
        check(keyframe, "合成流的 IRAP AU 被标成关键帧");
        for (auto &nal : au) {
            got.push_back(std::move(nal));
        }
    });
    parser.feed(stream.data(), stream.size());
    parser.flush();

    check(aus == 1, "切出 1 个 AU: " + std::to_string(aus));
    check(got == want, "4 个 NAL 与输入字节逐一相同（00 00 03 未被去掉）");
    if (got.size() == want.size()) {
        std::printf("  NAL 长度 期望=");
        for (const auto &n : want) std::printf("%zu ", n.size());
        std::printf(" 实际=");
        for (const auto &n : got) std::printf("%zu ", n.size());
        std::printf("\n");
    }

    // unescape_nal 仍然要留给读 RBSP 语法的人用（SPS 的 conformance window 之类），
    // 顺带钉住它的边界：03 后随 >0x03 不是 EPB，末尾孤立的 03 也不能删。
    const std::vector<uint8_t> in{0, 0, 3, 0x40, 0, 0, 3, 0x02, 0, 0, 3};
    const std::vector<uint8_t> rbsp{0, 0, 3, 0x40, 0, 0, 0x02, 0, 0, 3};
    check(scrctl::unescape_nal(in.data(), in.size()) == rbsp,
          "只在 03 后随 <=0x03 时去掉，末尾孤立的 03 保留");
}

int main(int argc, char **argv) {
    test_epb_kept();
    if (argc < 2) {
        // 没录制文件也要能全绿跑完：合成部分已经覆盖了这次守住的不变量。
        std::printf("\n未给 .hevc 参数，跳过真机码流的 AU 断言。\n");
        std::printf("\n%s (失败 %d 项)\n", Failures == 0 ? "全部通过" : "存在失败", Failures);
        return Failures == 0 ? 0 : 1;
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
