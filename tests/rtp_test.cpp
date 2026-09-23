// RTP 拆包自检。全部用手工构造的包，不打真机——这样每一处偏移错了都会立刻
// 变成一条明确的失败，而不是"画面有点糊"。
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include "bitstream/AnnexB.h"
#include "rt/RtpHevc.h"

namespace {

int Failures = 0;

void check(bool ok, const std::string &what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) {
        ++Failures;
    }
}

using scrctl::rt::HevcRtpDepacketizer;

/// 一个 RTP 包，按真机的样子构造：12 字节头（X=1）+ 8 字节扩展头 + HEVC 载荷。
/// 扩展头的形状是从真机包上抄的（profile 0x9011、长度 1 个 32 位字），不是编的——
/// 上一版测试用了"X=0 + 8 字节私有子头"，正好和被测代码里多跳 8 字节的错误自洽，
/// 于是测试全绿、真机全废。
std::vector<uint8_t> packet(uint16_t seq, uint32_t ts, bool marker,
                            const std::vector<uint8_t> &payload, uint8_t pt = 100) {
    std::vector<uint8_t> p;
    p.push_back(0x90);  // V=2, P=0, X=1, CC=0
    p.push_back(pt | (marker ? 0x80 : 0));
    p.push_back(static_cast<uint8_t>(seq >> 8));
    p.push_back(static_cast<uint8_t>(seq));
    for (int i = 3; i >= 0; --i) {
        p.push_back(static_cast<uint8_t>(ts >> (8 * i)));
    }
    for (int i = 0; i < 4; ++i) {
        p.push_back(static_cast<uint8_t>(0xDEADBEEF >> (8 * (3 - i))));
    }
    p.push_back(0x90);
    p.push_back(0x11);  // profile
    p.push_back(0x00);
    p.push_back(0x01);  // 长度：1 个 32 位字
    for (int i = 0; i < 4; ++i) {
        p.push_back(static_cast<uint8_t>(0x00107954 + i));  // 扩展内容，我们不读
    }
    p.insert(p.end(), payload.begin(), payload.end());
    return p;
}

/// 一个 HEVC NAL 头。
std::vector<uint8_t> nal_header(uint8_t type) {
    return {static_cast<uint8_t>((type << 1) & 0x7E), 0x01};
}

std::vector<uint8_t> aggregation(const std::vector<std::vector<uint8_t>> &nals) {
    std::vector<uint8_t> out = nal_header(48);
    for (const auto &n : nals) {
        out.push_back(static_cast<uint8_t>(n.size() >> 8));
        out.push_back(static_cast<uint8_t>(n.size()));
        out.insert(out.end(), n.begin(), n.end());
    }
    return out;
}

std::vector<uint8_t> fragment(uint8_t type, const std::vector<uint8_t> &body, bool start,
                              bool end) {
    std::vector<uint8_t> out = nal_header(49);
    out.push_back(static_cast<uint8_t>((start ? 0x80 : 0) | (end ? 0x40 : 0) | type));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

std::vector<uint8_t> single(uint8_t type, const std::vector<uint8_t> &body) {
    auto out = nal_header(type);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

bool at_start_code(const std::vector<uint8_t> &s, std::size_t k) {
    return s[k] == 0 && s[k + 1] == 0 && s[k + 2] == 0 && s[k + 3] == 1;
}

std::size_t next_start_code(const std::vector<uint8_t> &s, std::size_t from) {
    for (std::size_t k = from; k + 4 <= s.size(); ++k) {
        if (at_start_code(s, k)) {
            return k;
        }
    }
    return s.size();  // 最后一个 NAL 一直到串尾，不能少算尾巴那几字节
}

/// 把解出来的 Annex-B 串切成 NAL，方便断言。
std::vector<std::vector<uint8_t>> split_annexb(const std::vector<uint8_t> &stream) {
    std::vector<std::vector<uint8_t>> out;
    std::size_t i = 0;
    while (i + 4 <= stream.size()) {
        if (!at_start_code(stream, i)) {
            ++i;
            continue;
        }
        const std::size_t begin = i + 4;
        const std::size_t end = next_start_code(stream, begin);
        out.emplace_back(stream.begin() + static_cast<long>(begin),
                         stream.begin() + static_cast<long>(end));
        i = end;
    }
    return out;
}

uint8_t nal_type(const std::vector<uint8_t> &nal) {
    return nal.empty() ? 0xFF : static_cast<uint8_t>((nal[0] >> 1) & 0x3F);
}

void test_aggregation() {
    std::printf("\n== 聚合包 ==\n");
    std::vector<uint8_t> vps = nal_header(32);
    for (int i = 0; i < 22; ++i) {
        vps.push_back(static_cast<uint8_t>(i));
    }
    std::vector<uint8_t> sps = nal_header(33);
    for (int i = 0; i < 61; ++i) {
        sps.push_back(static_cast<uint8_t>(0x40 + i));
    }
    auto p = packet(1, 0, false, aggregation({vps, sps}));
    HevcRtpDepacketizer d;
    std::string err;
    std::vector<uint8_t> out;
    check(d.push(p, out, err), "聚合包可解: " + err);
    auto nals = split_annexb(out);
    check(nals.size() == 2, "出两个 NAL");
    if (nals.size() == 2) {
        check(nal_type(nals[0]) == 32 && nals[0].size() == 24, "VPS 类型与长度");
        check(nal_type(nals[1]) == 33 && nals[1] == sps, "SPS 内容逐字节一致");
    }
}

void test_fragmentation() {
    std::printf("\n== 分片 ==\n");
    std::vector<uint8_t> body;
    for (int i = 0; i < 300; ++i) {
        body.push_back(static_cast<uint8_t>(i * 3 + 1));
    }
    HevcRtpDepacketizer d;
    std::string err;
    std::vector<uint8_t> out;
    const auto f1 = fragment(20, {body.begin(), body.begin() + 120}, true, false);
    const auto f2 = fragment(20, {body.begin() + 120, body.begin() + 240}, false, false);
    const auto f3 = fragment(20, {body.begin() + 240, body.end()}, false, true);
    d.push(packet(10, 5000, false, f1), out, err);
    check(d.mid_fragment(), "起始分片后处于半成品状态");
    d.push(packet(11, 5000, false, f2), out, err);
    d.push(packet(12, 5000, true, f3), out, err);
    check(!d.mid_fragment(), "结尾分片后清空");
    auto nals = split_annexb(out);
    check(nals.size() == 1, "三片拼成一个 NAL");
    if (nals.size() == 1) {
        check(nal_type(nals[0]) == 20, "TU 还原成 NAL 类型");
        check(nals[0].size() == 2 + 300, "长度 = NAL 头 + 全部分片数据");
        // 这一条专门钉住"没有 DONL"：起始分片 FU 头之后的两字节就是码流本身。
        // 若按 RFC 7798 把它当 DONL 跳掉，这里会少 2 字节且整体错位。
        check(std::equal(body.begin(), body.end(), nals[0].begin() + 2),
              "起始分片 FU 头后的字节被当码流保留（这条流没有 DONL 字段）");
    }
}

void test_lost_fragments() {
    std::printf("\n== 丢分片 ==\n");
    HevcRtpDepacketizer d;
    std::string err;
    std::vector<uint8_t> out;
    // 中间片先到（起始片丢了）：整个 NAL 拼不出来，必须丢，不能交半截给解码器
    d.push(packet(1, 100, false, fragment(1, {0xAA, 0xBB}, false, false)), out, err);
    check(split_annexb(out).empty(), "缺起始片的中间片不产出 NAL");
    d.push(packet(2, 100, true, fragment(1, {0xCC}, false, true)), out, err);
    check(split_annexb(out).empty(), "缺起始片的结尾片也不产出 NAL");
    check(d.stats().dropped_fragments == 2,
          "两次都记进 dropped_fragments: " + std::to_string(d.stats().dropped_fragments));

    // 起始片来了两次（上一帧的结尾片丢了）：旧的半成品要作废，不能串帧
    out.clear();
    d.push(packet(3, 200, false, fragment(1, {0x11, 0x11}, true, false)), out, err);
    d.push(packet(4, 300, false, fragment(1, {0x22, 0x22}, true, false)), out, err);
    d.push(packet(5, 300, true, fragment(1, {0x33, 0x33}, false, true)), out, err);
    auto nals = split_annexb(out);
    check(nals.size() == 1 && nals[0].size() == 2 + 4, "新起始片丢弃上一帧的半成品");
    check(nals.size() == 1 && nals[0][2] == 0x22, "产出的是第二个 NAL 的内容");
    d.reset();
    check(!d.mid_fragment(), "reset 清空半成品");
}

void test_single_and_offsets() {
    std::printf("\n== 单一 NAL 与头偏移 ==\n");
    HevcRtpDepacketizer d;
    std::string err;
    std::vector<uint8_t> out;
    d.push(packet(7, 900, true, single(1, {0xA4, 0x01, 0x02})), out, err);
    auto nals = split_annexb(out);
    check(nals.size() == 1 && nal_type(nals[0]) == 1 && nals[0].size() == 5, "单一 NAL 直通");

    // 带 CSRC 与扩展头的包：偏移算错就会把 CSRC/扩展当载荷，解出垃圾 NAL 类型
    auto base = single(1, {0xA4});
    std::vector<uint8_t> with_csrc;
    with_csrc.push_back(0x92);  // V=2, X=1, CC=2 -> 8 字节 CSRC 再接 8 字节扩展头
    with_csrc.push_back(100);
    with_csrc.push_back(0);
    with_csrc.push_back(3);
    for (int i = 0; i < 8; ++i) {  // 补齐到 12 字节 RTP 头
        with_csrc.push_back(0);
    }
    for (int i = 0; i < 8; ++i) {  // CC=2 的 CSRC 列表
        with_csrc.push_back(static_cast<uint8_t>(0xC0 + i));
    }
    with_csrc.push_back(0x90);
    with_csrc.push_back(0x11);  // 扩展头 profile
    with_csrc.push_back(0x00);
    with_csrc.push_back(0x01);  // 长度 1 个 32 位字 -> 扩展共 8 字节
    for (int i = 0; i < 4; ++i) {
        with_csrc.push_back(0x5A);
    }
    with_csrc.insert(with_csrc.end(), base.begin(), base.end());
    std::vector<uint8_t> out2;
    check(d.push(with_csrc, out2, err), "带 CSRC 的包可解");
    auto n2 = split_annexb(out2);
    check(n2.size() == 1 && nal_type(n2[0]) == 1, "CSRC 被正确跳过");

    std::vector<uint8_t> with_ext;
    with_ext.push_back(0x90);  // V=2, X=1, CC=0
    with_ext.push_back(100);
    with_ext.push_back(0);
    with_ext.push_back(4);
    for (int i = 0; i < 8; ++i) {
        with_ext.push_back(0);
    }
    with_ext.push_back(0xBE);
    with_ext.push_back(0xDE);
    with_ext.push_back(0x00);
    with_ext.push_back(0x02);  // 扩展长度 2 个 32 位字 -> 共 12 字节
    for (int i = 0; i < 8; ++i) {
        with_ext.push_back(0x77);
    }
    // 这个包自己带一个 2 字的扩展，覆盖"按扩展自身长度跳"的分支
    with_ext.insert(with_ext.end(), base.begin(), base.end());
    std::vector<uint8_t> out3;
    check(d.push(with_ext, out3, err), "带扩展头的包可解");
    auto n3 = split_annexb(out3);
    check(n3.size() == 1 && nal_type(n3[0]) == 1, "扩展头按自身长度跳过");

    // 非 RTP / 太短
    std::vector<uint8_t> junk = {1, 2, 3};
    check(!d.push(junk, out3, err), "3 字节的垃圾被拒");
    std::vector<uint8_t> v1(30, 0x40);
    check(!d.push(v1, out3, err), "RTP 版本不是 2 被拒");
}

void test_payload_type_filter() {
    std::printf("\n== 只收视频 PT ==\n");
    // RTCP 与视频同端口到达（真机实测 PT=72）。不过滤就会被当 HEVC 载荷解出
    // 假 NAL 混进码流，而这条流不周期发 IDR，一个假 NAL 就把参考链永久打断。
    std::vector<uint8_t> rtcp;
    rtcp.push_back(0x82);
    rtcp.push_back(72);
    rtcp.insert(rtcp.end(), 20, 0x00);
    HevcRtpDepacketizer d;
    std::string err;
    std::vector<uint8_t> out;
    d.push(packet(1, 0, false, aggregation({nal_header(32)})), out, err);
    const auto after_video = out.size();
    d.push(rtcp, out, err);
    check(out.size() == after_video, "RTCP 包不产出任何字节");
    check(d.stats().other_payload == 1 && d.stats().packets == 1,
          "RTCP 记成 other_payload，不计入视频包数与序号");
    // 序号也不能被它带跳：再来一个正常的下一号包，不该算丢包
    d.push(packet(2, 100, true, single(1, {0xA4})), out, err);
    check(d.stats().seq_gaps == 0, "夹了 RTCP 之后视频序号仍然连续");

    // 配成别的 PT（比如协商到 96）时，100 的包不该被解
    HevcRtpDepacketizer other(96);
    std::vector<uint8_t> none;
    other.push(packet(1, 0, false, single(1, {0xA4})), none, err);
    check(none.empty() && other.stats().other_payload == 1, "PT 不匹配就整包跳过");
}

void test_feeds_annexb_parser() {
    std::printf("\n== 与 M1 的 AU 切分器对接 ==\n");
    // 拆包输出直接喂给 AnnexBParser，验证一帧一包 + 一帧多包都能切成 AU
    HevcRtpDepacketizer d;
    std::vector<std::size_t> au_sizes;
    std::size_t keyframes = 0;
    scrctl::AnnexBParser parser([&](std::vector<scrctl::Nal> &&au, bool key) {
        au_sizes.push_back(au.size());
        if (key) {
            ++keyframes;
        }
    });
    std::string err;
    std::vector<uint8_t> out;
    std::vector<uint8_t> vps_nal = nal_header(32);
    vps_nal.push_back(0x01);
    std::vector<uint8_t> sps_nal = nal_header(33);
    sps_nal.push_back(0x02);
    std::vector<uint8_t> idr = nal_header(20);
    idr.push_back(0x80);
    d.push(packet(1, 0, false, aggregation({vps_nal, sps_nal})), out, err);
    d.push(packet(2, 0, false, fragment(20, {0x80, 0x01}, true, false)), out, err);
    d.push(packet(3, 0, true, fragment(20, {0x02, 0x03}, false, true)), out, err);
    parser.feed(out.data(), out.size());
    out.clear();
    d.push(packet(4, 400, true, single(1, {0xA4, 0x11})), out, err);
    parser.feed(out.data(), out.size());
    parser.flush();
    check(au_sizes.size() == 2, "切出 2 个 AU: " + std::to_string(au_sizes.size()));
    check(keyframes == 1, "其中 1 个是关键帧");
    check(!au_sizes.empty() && au_sizes[0] == 3, "关键帧 AU = VPS + SPS + IDR");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    test_aggregation();
    test_fragmentation();
    test_lost_fragments();
    test_single_and_offsets();
    test_payload_type_filter();
    test_feeds_annexb_parser();
    std::printf("\n%s (失败 %d 项)\n", Failures == 0 ? "全部通过" : "存在失败", Failures);
    return Failures == 0 ? 0 : 1;
}
