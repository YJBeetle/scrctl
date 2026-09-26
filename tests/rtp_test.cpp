// RTP 拆包自检。全部用手工构造的包，不打真机——这样每一处偏移错了都会立刻
// 变成一条明确的失败，而不是"画面有点糊"。
#include <cstdio>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "bitstream/AnnexB.h"
#include "rt/Rtcp.h"
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

/// RTCP 那几种包的字节形状。
///
/// 为什么要钉：产品的会话续命现在依赖每秒发一个 RR（设备的 `RTCPTimeoutInterval` 是
/// "距离上次收到我们 RTCP 多久"的空闲计时器），而这个文件所在的那条链上，"形状写错"
/// 有过两次前科：一次把公共头的 16 位长度字段用 `put32` 写成 32 位（整包从第 3 字节起
/// 错位，于是"设备不理这种包"的结论建立在畸形包上），一次是数据报拼装把载荷留在缓冲区
/// 尾巴上（长度字段与校验和的范围都不对，设备内核静默丢弃）。两种在发送侧都不报错，
/// 只有对面能看出来——所以对面不在场时，必须靠这几条测。
void test_rtcp_shapes() {
    std::printf("\n== RTCP 包形状 ==\n");
    const uint32_t our = 0x11223344u;   // 设备分配给我们这一端的 RemoteSSRC
    const uint32_t media = 0x55667788u;  // 设备自己那条流的 LocalSSRC
    const uint32_t ext_high = 0x0001c8e0u;

    const auto rr = scrctl::rt::build_rr(our, media, ext_high);
    check(rr.size() == 32, "RR 一共 32 字节: " + std::to_string(rr.size()));
    check(rr[0] == 0x81, "RR 首字节 0x81（V=2, RC=1）");
    check(rr[1] == 201, "RR 的 PT=201");
    check((rr[2] << 8 | rr[3]) == 7, "RR 长度字段=7（16 位，头之后 7 个字）");
    check(rr[4] == 0x11 && rr[7] == 0x44, "发送者 SSRC 在偏移 4，填的是设备分配的 RemoteSSRC");
    check(rr[8] == 0x55 && rr[11] == 0x88, "报告块指认的 SSRC 在偏移 8，填设备的 LocalSSRC");
    check(rr[16] == 0x00 && rr[17] == 0x01 && rr[18] == 0xc8 && rr[19] == 0xe0,
          "扩展最高序号在偏移 16");
    // 头 4 + 发送者 SSRC 4 + 报告块 24 = 32，与长度字段自洽：7 个字 + 首字 = 8 字。
    check(rr.size() == std::size_t(4 + 7 * 4), "长度字段 7 与真实字节数自洽");

    const auto sr = scrctl::rt::build_sr(our, 0, 0);
    check(sr.size() == 28, "SR 一共 28 字节: " + std::to_string(sr.size()));
    check(sr[0] == 0x80 && sr[1] == 200, "SR 首两字节 0x80 PT=200（RC=0）");
    check((sr[2] << 8 | sr[3]) == 6, "SR 长度字段=6");
    check(sr.size() == std::size_t(4 + 6 * 4), "SR 长度字段与真实字节数自洽");

    const auto sdes = scrctl::rt::build_sdes(our);
    check(sdes.size() == 12, "空 CNAME 的 SDES 是 12 字节: " + std::to_string(sdes.size()));
    check(sdes[0] == 0x81 && sdes[1] == 202, "SDES 首两字节 0x81 PT=202");
    check((sdes[2] << 8 | sdes[3]) == 2, "SDES 长度字段=2");
    // RR + 空 CNAME 的复合包正是苹果音频腿实测那一种，44 字节一个不多一个不少。
    // 这条是"我们对苹果包形状的对齐"里唯一能离线验的部分。
    std::vector<uint8_t> compound = rr;
    compound.insert(compound.end(), sdes.begin(), sdes.end());
    check(compound.size() == 44, "RR+SDES 复合包 = 44 字节（苹果实测的那个形状）");

    const auto named = scrctl::rt::build_sdes_cname(our, "scrctl");
    check(named.size() % 4 == 0, "带实义 CNAME 的 SDES 补齐到 4 字节边界");
    check((named[2] << 8 | named[3]) == named.size() / 4 - 1,
          "它的长度字段按真实字数走（不是空 CNAME 那档的 2）");
    check(named.size() == 20, "CNAME=\"scrctl\" 时 SDES = 20 字节: " + std::to_string(named.size()));

    // 设备的 SR 和视频共用一个 UDP 端口，靠开头两字节分。判错会把每秒那个心跳记成视频包，
    // "画面静止"和"流死了"就分不开了。
    // 注意这一位认的是**设备那条心跳**：它带 RC=1，所以首字节是 0x81；我们自己发的 SR 是
    // RC=0（首字节 0x80，上面 `build_sr` 的形状）。两者不是同一个字节串，别混用。
    std::vector<uint8_t> dev_sr(28, 0);
    dev_sr[0] = 0x81;
    dev_sr[1] = 0xc8;  // PT=200
    check(scrctl::rt::is_rtcp_sr(dev_sr), "0x81 0xc8 且 >=28 字节的认成 SR（设备那条心跳）");
    check(!scrctl::rt::is_rtcp_sr(sr), "我们自己 RC=0 的 SR（0x80 0xc8）不认成设备心跳");
    const auto pkt = packet(1, 0, true, single(1, {0x01, 0x02}));
    check(!scrctl::rt::is_rtcp_sr(pkt), "RTP 视频包不认成 SR");
    check(!scrctl::rt::is_rtcp_sr(std::span<const uint8_t>(rr)), "RR(PT=201) 不认成 SR");
    const std::vector<uint8_t> tiny = {0x81, 0xc8};
    check(!scrctl::rt::is_rtcp_sr(tiny), "短于 28 字节的 0x81 0xc8 不认成 SR（不越界读）");

    // 关键帧请求的两种包。它们的长度字段以前从来没被钉过，而"设备不理 PLI"这个结论
    // 正是建立在形状未经核对的包上——先把形状钉死，再去问设备。
    const auto pli = scrctl::rt::build_pli(our, media);
    check(pli.size() == 12, "PLI 是 12 字节: " + std::to_string(pli.size()));
    check(pli[0] == 0x81 && pli[1] == 206, "PLI 首两字节 0x81 PT=206（RTPFB）/FMT=1");
    check((pli[2] << 8 | pli[3]) == 2, "PLI 长度字段=2");
    check(pli.size() == std::size_t(4 + 2 * 4), "PLI 长度字段与真实字节数自洽");
    check(pli[4] == 0x11 && pli[11] == 0x88, "PLI 后面是发送者 SSRC + 媒体 SSRC");

    const auto fir = scrctl::rt::build_fir(our, 7, media);
    check(fir.size() == 24, "FIR 是 24 字节: " + std::to_string(fir.size()));
    check(fir[0] == 0x84 && fir[1] == 206, "FIR 首两字节 0x84（FMT=4）PT=206");
    check((fir[2] << 8 | fir[3]) == 5, "FIR 长度字段=5");
    check(fir.size() == std::size_t(4 + 5 * 4), "FIR 长度字段与真实字节数自洽");
    // 布局：0-3 公共头 / 4-7 发送者 SSRC / 8-11 序号（低 8 位有效）/ 12-15 FCI 目标 SSRC
    // / 16-23 FCI 的 8 字节媒体序号。别按"第 8 字节"想它——第 8 字节是那个字的开头。
    check(fir[11] == 7 && fir[8] == 0 && fir[9] == 0 && fir[10] == 0,
          "FIR 序号在那个字（偏移 8 起）的低字节");
    check(fir[4] == 0x11, "FIR 的发送者 SSRC 在偏移 4");
    check(fir[12] == 0x55 && fir[15] == 0x88, "FCI 里指认的目标 SSRC 在偏移 12");
    const auto fir2 = scrctl::rt::build_fir(our, 8, media);
    check(fir2[11] == 8 && fir.size() == fir2.size(), "序号每请求加一（重复序号不会换来新 IDR）");
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
    test_rtcp_shapes();
    std::printf("\n%s (失败 %d 项)\n", Failures == 0 ? "全部通过" : "存在失败", Failures);
    return Failures == 0 ? 0 : 1;
}
