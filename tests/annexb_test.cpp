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
#include <optional>
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

namespace {

using AccessUnit = scrctl::AnnexBParser::AccessUnit;

scrctl::Nal make_nal(uint8_t type, bool first_slice, uint8_t tag) {
    return {static_cast<uint8_t>(type << 1), 0x01,
            static_cast<uint8_t>(first_slice ? 0x80 : 0x00), tag};
}

std::vector<uint8_t> annexb_bytes(const std::vector<scrctl::Nal> &nals) {
    std::vector<uint8_t> bytes;
    for (const auto &nal : nals) {
        bytes.insert(bytes.end(), {0, 0, 0, 1});
        bytes.insert(bytes.end(), nal.begin(), nal.end());
    }
    return bytes;
}

void test_sampling_timestamps() {
    std::printf("\n== 完整 NAL 的采样时间和 AU 边界 ==\n");
    std::vector<AccessUnit> got;
    scrctl::AnnexBParser parser([&](AccessUnit &&au) { got.push_back(std::move(au)); });
    const auto vps = make_nal(32, false, 1);
    const auto sei = make_nal(39, false, 2);
    const auto first = make_nal(19, true, 3);
    const auto second = make_nal(19, false, 4);
    check(parser.push_nal(vps, 9900) && parser.push_nal(sei, std::nullopt) &&
              parser.push_nal(first, 400) && parser.push_nal(second, 400, true),
          "参数/SEI 前缀和同一采样时间的两条 slice 可组成 AU");
    check(got.size() == 1 && got[0].nals == std::vector<scrctl::Nal>{vps, sei, first, second} &&
              got[0].sampling_timestamp == 400 && got[0].keyframe,
          "marker 立即提交完整 AU，时间来自第一条 VCL，原始 NAL 顺序不变");

    got.clear();
    const auto a = make_nal(1, true, 5);
    const auto b = make_nal(1, true, 6);
    const auto c = make_nal(1, true, 7);
    // 模拟一次拆包批次返回多条完整图像；最后一条没有 marker，flush 负责收尾。
    check(parser.push_nal(a, 800) && parser.push_nal(b, 1200) && parser.push_nal(c, 1000),
          "同批多图像允许不同时间，也保留合法的向后采样时间");
    check(got.size() == 2 && got[0].nals == std::vector<scrctl::Nal>{a} &&
              got[0].sampling_timestamp == 800 && got[1].nals == std::vector<scrctl::Nal>{b} &&
              got[1].sampling_timestamp == 1200,
          "后一图像触发提交时，前两条 AU 仍各自保留原采样时间");
    parser.flush();
    check(got.size() == 3 && got.back().nals == std::vector<scrctl::Nal>{c} &&
              got.back().sampling_timestamp == 1000 && !got.back().keyframe,
          "flush 提交最后一条 AU，不把采样 ticks 当作单调 DTS");

    got.clear();
    check(parser.push_nal(a) && parser.push_nal(b, 1600, true) && got.size() == 2 &&
              !got[0].sampling_timestamp && got[1].sampling_timestamp == 1600,
          "不同图像可分别 unknown/known：first slice 先提交旧 AU，再存新图像时间");

    got.clear();
    const auto suffix = make_nal(40, false, 8);
    check(parser.push_nal(a, 2000) && parser.push_nal(sei, 7777), "prefix SEI 关闭前一图像");
    check(got.size() == 1 && got[0].nals == std::vector<scrctl::Nal>{a} &&
              got[0].sampling_timestamp == 2000,
          "下一图像的 prefix SEI 不留在前一 AU，也不改写其时间");
    check(parser.push_nal(b, 2400) && parser.push_nal(suffix, std::nullopt, true),
          "suffix SEI 的 marker 可立即提交已包含 VCL 的 AU");
    check(got.size() == 2 && got.back().nals == std::vector<scrctl::Nal>{sei, b, suffix} &&
              got.back().sampling_timestamp == 2400,
          "prefix/suffix 字节归入对应图像，非 VCL 的 unknown 时间不影响图像");

    got.clear();
    check(parser.push_nal(vps, 9000, true), "仅参数集的 marker 输入可处理");
    parser.flush();
    check(got.empty() && parser.vps() == vps, "参数集不能单独交付成 AU，但仍保留缓存");

    const auto cra = make_nal(21, true, 9);
    check(parser.push_nal(cra, -400, true) && got.size() == 1 && got[0].keyframe &&
              got[0].sampling_timestamp == -400,
          "沿用 CRA 的 IRAP bool 语义，不把负采样 ticks 当作错误");
}

void test_parameter_cache_at_au_callback() {
    std::printf("\n== 提交 AU 时的参数集缓存 ==\n");
    const std::vector<scrctl::Nal> old_parameters{make_nal(32, false, 1),
                                                make_nal(33, false, 2),
                                                make_nal(34, false, 3)};
    const std::vector<scrctl::Nal> new_parameters{make_nal(32, false, 11),
                                                make_nal(33, false, 12),
                                                make_nal(34, false, 13)};
    const auto initial_picture = make_nal(19, true, 4);
    const auto old_picture = make_nal(1, true, 5);
    const auto new_picture = make_nal(19, true, 6);
    struct Snapshot {
        AccessUnit au;
        std::vector<scrctl::Nal> parameters;
    };

    // 三种参数都可能成为下一 AU 的首个前缀。中间的图像不重复参数集，
    // 它的消费者必须在回调中读取缓存，不能借用下一图像的新配置。
    for (size_t first = 0; first < new_parameters.size(); ++first) {
        const std::string label = std::string(first == 0 ? "VPS" : first == 1 ? "SPS" : "PPS");
        std::vector<scrctl::Nal> prefixes{new_parameters[first]};
        for (size_t i = 0; i < new_parameters.size(); ++i) {
            if (i != first) prefixes.push_back(new_parameters[i]);
        }

        std::vector<Snapshot> typed;
        scrctl::AnnexBParser *current = nullptr;
        scrctl::AnnexBParser parser([&](AccessUnit &&au) {
            typed.push_back({std::move(au), {current->vps(), current->sps(), current->pps()}});
        });
        current = &parser;
        bool accepted = true;
        for (const auto &nal : old_parameters) accepted = parser.push_nal(nal) && accepted;
        accepted = parser.push_nal(initial_picture, 0) && accepted;
        accepted = parser.push_nal(old_picture, 400) && accepted;
        accepted = parser.push_nal(prefixes.front(), 9900) && accepted;
        check(accepted && typed.size() == 2 && typed[0].parameters == old_parameters &&
                  typed[1].parameters == old_parameters &&
                  typed[1].au.nals == std::vector<scrctl::Nal>{old_picture} &&
                  typed[1].au.sampling_timestamp == 400,
              label + " 新前缀提交旧 AU 时，回调仍读取旧缓存且图像字节/时间不变");

        auto partially_updated = old_parameters;
        partially_updated[first] = new_parameters[first];
        check(std::vector<scrctl::Nal>{parser.vps(), parser.sps(), parser.pps()} == partially_updated,
              label + " 回调结束后才更新本次前缀对应的参数缓存");
        for (size_t i = 1; i < prefixes.size(); ++i) {
            accepted = parser.push_nal(prefixes[i], 9900) && accepted;
        }
        accepted = parser.push_nal(new_picture, 800, true) && accepted;
        auto new_au_nals = prefixes;
        new_au_nals.push_back(new_picture);
        check(accepted && typed.size() == 3 && typed.back().parameters == new_parameters &&
                  typed.back().au.nals == new_au_nals && typed.back().au.sampling_timestamp == 800 &&
                  typed.back().au.keyframe,
              label + " 新 AU 提交时读取完整新缓存，前缀顺序和采样时间保留");

        auto nals = old_parameters;
        nals.push_back(initial_picture);
        nals.push_back(old_picture);
        nals.insert(nals.end(), prefixes.begin(), prefixes.end());
        nals.push_back(new_picture);
        const auto bytes = annexb_bytes(nals);
        std::vector<Snapshot> streamed;
        scrctl::AnnexBParser *byte_current = nullptr;
        scrctl::AnnexBParser byte_parser([&](AccessUnit &&au) {
            streamed.push_back({std::move(au),
                                {byte_current->vps(), byte_current->sps(), byte_current->pps()}});
        });
        byte_current = &byte_parser;
        bool bytes_accepted = true;
        for (const auto byte : bytes) bytes_accepted = byte_parser.feed(&byte, 1) && bytes_accepted;
        byte_parser.flush();
        bool same = streamed.size() == typed.size();
        for (size_t i = 0; same && i < typed.size(); ++i) {
            same = streamed[i].parameters == typed[i].parameters &&
                   streamed[i].au.nals == typed[i].au.nals &&
                   streamed[i].au.keyframe == typed[i].au.keyframe &&
                   !streamed[i].au.sampling_timestamp;
        }
        check(bytes_accepted && same && streamed.size() == 3 &&
                  streamed[1].parameters == old_parameters &&
                  streamed.back().parameters == new_parameters,
              label + " 逐字节文件输入具有相同的回调缓存、NAL 字节和分组，时间保持 unknown");
    }
}

void test_damaged_au() {
    std::printf("\n== 残缺 AU 不交付 ==\n");
    const auto first = make_nal(1, true, 1);
    const auto next = make_nal(1, false, 2);
    const auto vps = make_nal(32, false, 3);
    const auto sps = make_nal(33, false, 4);
    const auto pps = make_nal(34, false, 5);
    std::vector<AccessUnit> got;
    scrctl::AnnexBParser parser([&](AccessUnit &&au) { got.push_back(std::move(au)); });

    struct Case {
        std::optional<int64_t> first_time;
        std::optional<int64_t> next_time;
        const char *name;
    };
    for (const auto &test : {Case{400, 401, "两条 slice 的已知时间不同"},
                             Case{400, std::nullopt, "第一条已知、后一条 unknown"},
                             Case{std::nullopt, 400, "第一条 unknown、后一条已知"}}) {
        parser.discard_pending();
        check(parser.push_nal(first, test.first_time), std::string(test.name) + "：接收起始 slice");
        check(!parser.push_nal(next, test.next_time, true),
              std::string(test.name) + "：拒绝残缺时间组合");
        parser.flush();
        check(got.empty(), std::string(test.name) + "：marker/flush 均不能提交假完整 AU");
        check(!parser.push_nal(next, test.next_time, true), "丢弃之后的残余 slice 不能重新开启图像");
        check(parser.push_nal(first, 800, true) && got.size() == 1 &&
                  got[0].sampling_timestamp == 800 && got[0].nals == std::vector<scrctl::Nal>{first},
              "后续 first slice 可以开启独立、完整的新 AU");
        got.clear();
    }

    parser.discard_pending();
    check(parser.push_nal(first) && parser.push_nal(next, std::nullopt, true) &&
              got.size() == 1 && !got[0].sampling_timestamp && got[0].nals.size() == 2,
          "所有 slice 均 unknown 时保留无时间的正常 AU");
    got.clear();

    check(parser.push_nal(vps) && parser.push_nal(sps) && parser.push_nal(pps) &&
              !parser.push_nal(next, 1200, true),
          "参数集前缀不能让缺少 first slice 的残片成为图像");
    check(got.empty() && parser.has_parameter_sets(), "残片拒绝不影响参数集缓存");

    check(parser.push_nal(first, 1600), "丢片前的完整 NAL 暂存在 AU 中");
    parser.discard_pending();
    parser.flush();
    check(got.empty() && parser.vps() == vps && parser.sps() == sps && parser.pps() == pps,
          "确认丢片后 discard_pending 不交付旧 AU，保留全部参数集缓存");
    check(!parser.push_nal(next, 1600, true) && parser.push_nal(first, 2000, true) &&
              got.size() == 1 && got[0].nals == std::vector<scrctl::Nal>{first},
          "丢片后的剩余 slice 被拒绝，下一完整图像可恢复");
    got.clear();

    check(parser.push_nal(first, 2400) && !parser.push_nal({0x02, 0x01}, 2400, true),
          "过短的完整 NAL 使待提交 AU 失效");
    parser.flush();
    check(got.empty(), "过短 NAL 后 flush 也不能提交受损图像");
}

void test_input_modes() {
    std::printf("\n== 字节输入与完整 NAL 输入的边界 ==\n");
    const auto first = make_nal(1, true, 1);
    const auto next = make_nal(1, true, 2);
    const auto bytes = annexb_bytes({first});
    std::vector<AccessUnit> got;
    scrctl::AnnexBParser parser([&](AccessUnit &&au) { got.push_back(std::move(au)); });

    check(parser.feed(nullptr, 0) && !parser.feed(nullptr, 1), "空 feed 无副作用，非空空指针被拒绝");
    check(parser.feed(bytes.data(), bytes.size()) && !parser.push_nal(next, 400, true),
          "有未定界字节时拒绝完整 NAL，不能偷用其 marker 提交半条字节 NAL");
    parser.flush();
    check(got.size() == 1 && got[0].nals == std::vector<scrctl::Nal>{first} &&
              !got[0].sampling_timestamp,
          "被拒绝的完整输入不改变字节解析状态，flush 输出仍与字节源一致");
    got.clear();

    check(parser.push_nal(next, 800) && !parser.feed(bytes.data(), bytes.size()),
          "有完整 NAL AU 时拒绝字节输入，不能混入无时间 slice");
    parser.flush();
    check(got.size() == 1 && got[0].nals == std::vector<scrctl::Nal>{next} &&
              got[0].sampling_timestamp == 800,
          "被拒绝的字节输入不改写完整 NAL AU 或时间");
    got.clear();

    check(parser.feed(bytes.data(), 6), "输入未完成的字节 NAL");
    parser.discard_pending();
    parser.flush();
    check(got.empty(), "discard_pending 清除 byte scanner，不输出截断 NAL");
    check(parser.push_nal(first, 1200, true) && got.size() == 1 &&
              got[0].sampling_timestamp == 1200,
          "显式 discard 后可切换到完整 NAL 输入");
    parser.discard_pending();
    got.clear();
    check(parser.feed(bytes.data(), bytes.size()), "显式 discard 后也可切回字节输入");
    parser.flush();
    check(got.size() == 1 && !got[0].sampling_timestamp, "切回后字节 AU 的时间保持 unknown");
}

void test_input_equivalence() {
    std::printf("\n== 两种输入复用同一分组 ==\n");
    const std::vector<scrctl::Nal> nals{make_nal(32, false, 1), make_nal(33, false, 2),
                                      make_nal(34, false, 3), make_nal(39, false, 4),
                                      make_nal(19, true, 5), make_nal(19, false, 6),
                                      make_nal(40, false, 7), make_nal(35, false, 8),
                                      make_nal(39, false, 9), make_nal(1, true, 10)};
    const auto bytes = annexb_bytes(nals);
    std::vector<AccessUnit> typed, streamed;
    scrctl::AnnexBParser complete([&](AccessUnit &&au) { typed.push_back(std::move(au)); });
    scrctl::AnnexBParser chunks([&](AccessUnit &&au) { streamed.push_back(std::move(au)); });
    bool typed_accepted = true;
    for (const auto &nal : nals) {
        typed_accepted = complete.push_nal(nal) && typed_accepted;
    }
    bool bytes_accepted = true;
    for (const auto byte : bytes) {
        bytes_accepted = chunks.feed(&byte, 1) && bytes_accepted;
    }
    check(typed_accepted && bytes_accepted, "完整 NAL 和逐字节路径均接受全部合法输入");
    complete.flush();
    chunks.flush();
    bool same = typed.size() == streamed.size();
    for (size_t i = 0; same && i < typed.size(); ++i) {
        same = typed[i].nals == streamed[i].nals && typed[i].keyframe == streamed[i].keyframe &&
               !typed[i].sampling_timestamp && !streamed[i].sampling_timestamp;
    }
    check(same && typed.size() == 2 && typed[0].nals.size() == 7 && typed[1].nals.size() == 3,
          "参数集、AUD、prefix/suffix SEI 与多 slice 的分组和原字节在两种路径相同");
}

}  // namespace

int main(int argc, char **argv) {
    test_epb_kept();
    test_sampling_timestamps();
    test_parameter_cache_at_au_callback();
    test_damaged_au();
    test_input_modes();
    test_input_equivalence();
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
