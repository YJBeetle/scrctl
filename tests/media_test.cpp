// 媒体协商 offer 的离线自检。这块必须在接上真机之前就能自己证明自己，
// 因为一旦上线，"流起不来"的原因可能是 offer 里任何一个字节。
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include "media/MediaOffer.h"
#include "plist/Bplist.h"
#include "util/Deflate.h"

namespace {

int Failures = 0;

void check(bool ok, const std::string &what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) {
        ++Failures;
    }
}

// ------------------------------------------------------- stored 块的反向读 ------

/// 把 zlib_store 产出的东西还原：验头、逐块取长度、验 NLEN 与 Adler-32。
/// 有了它，"我写的压缩容器对不对"就不必等到接上真机才知道。
bool zlib_unstore(const std::vector<uint8_t> &in, std::vector<uint8_t> &out, std::string &err) {
    if (in.size() < 7) {
        err = "太短，连头和尾校验都放不下";
        return false;
    }
    // CM（压缩方法）在 CMF 的低 4 位，不在 FLG 里；FLG 的高 5 位是窗口大小。
    // 头两字节的组合值还要能被 31 整除，这是 zlib 自己的头校验。
    if (in[0] != 0x78 || (in[0] * 256 + in[1]) % 31 != 0 || (in[0] & 0x0F) != 8) {
        err = "zlib 头不对";
        return false;
    }
    std::size_t pos = 2;
    bool final_seen = false;
    while (!final_seen) {
        if (pos + 5 > in.size()) {
            err = "块头被截断";
            return false;
        }
        const uint8_t hdr = in[pos++];
        final_seen = (hdr & 0x01) != 0;
        if ((hdr >> 1 & 0x03) != 0) {
            err = "不是 stored 块";
            return false;
        }
        const std::size_t len = in[pos] | static_cast<std::size_t>(in[pos + 1]) << 8;
        const std::size_t nlen = in[pos + 2] | static_cast<std::size_t>(in[pos + 3]) << 8;
        pos += 4;
        if (static_cast<uint16_t>(~len) != nlen) {
            err = "LEN 与 NLEN 不互补";
            return false;
        }
        if (pos + len > in.size()) {
            err = "块载荷越界";
            return false;
        }
        out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(pos),
                   in.begin() + static_cast<std::ptrdiff_t>(pos + len));
        pos += len;
    }
    // Adler-32 在流末尾，大端：倒数第 4 字节是最高位。
    const uint32_t want = static_cast<uint32_t>(in[in.size() - 4]) << 24 |
                          static_cast<uint32_t>(in[in.size() - 3]) << 16 |
                          static_cast<uint32_t>(in[in.size() - 2]) << 8 | in[in.size() - 1];
    if (want != scrctl::util::adler32(out.data(), out.size())) {
        err = "Adler-32 校验不符";
        return false;
    }
    return true;
}

// ------------------------------------------------------------ protobuf 走查 ----

struct Field {
    uint32_t number = 0;
    uint8_t wire = 0;
    uint64_t value = 0;  ///< wire 0
    std::vector<uint8_t> bytes;
};

bool parse_fields(const std::vector<uint8_t> &buf, std::vector<Field> &out) {
    std::size_t i = 0;
    while (i < buf.size()) {
        Field f;
        uint64_t key = 0;
        int sh = 0;
        do {
            if (i >= buf.size()) {
                return false;
            }
            key |= static_cast<uint64_t>(buf[i] & 0x7F) << sh;
            sh += 7;
        } while (buf[i++] & 0x80);
        f.number = static_cast<uint32_t>(key >> 3);
        f.wire = static_cast<uint8_t>(key & 7);
        if (f.wire == 0) {
            sh = 0;
            do {
                if (i >= buf.size()) {
                    return false;
                }
                f.value |= static_cast<uint64_t>(buf[i] & 0x7F) << sh;
                sh += 7;
            } while (buf[i++] & 0x80);
        } else if (f.wire == 2) {
            uint64_t len = 0;
            sh = 0;
            do {
                if (i >= buf.size()) {
                    return false;
                }
                len |= static_cast<uint64_t>(buf[i] & 0x7F) << sh;
                sh += 7;
            } while (buf[i++] & 0x80);
            if (i + len > buf.size()) {
                return false;
            }
            f.bytes.assign(buf.begin() + static_cast<std::ptrdiff_t>(i),
                           buf.begin() + static_cast<std::ptrdiff_t>(i + len));
            i += len;
        } else {
            return false;
        }
        out.push_back(std::move(f));
    }
    return true;
}

std::size_t count_of(const std::vector<Field> &fields, uint32_t number) {
    std::size_t n = 0;
    for (const auto &f : fields) {
        if (f.number == number) {
            ++n;
        }
    }
    return n;
}

const Field *only(const std::vector<Field> &fields, uint32_t number) {
    for (const auto &f : fields) {
        if (f.number == number) {
            return &f;
        }
    }
    return nullptr;
}

std::string as_text(const std::vector<uint8_t> &b) {
    return std::string(b.begin(), b.end());
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    using namespace scrctl::media;

    std::printf("== zlib stored 容器自身 ==\n");
    {
        std::string err;
        std::vector<uint8_t> back;
        const auto empty = scrctl::util::zlib_store({});
        check(zlib_unstore(empty, back, err) && back.empty(), "空输入也能自洽: " + err);
        const auto small = scrctl::util::zlib_store({'a', 'b', 'c'});
        back.clear();
        check(zlib_unstore(small, back, err) && as_text(back) == "abc", "小包往返: " + err);
        std::vector<uint8_t> big(70000, 0x41);  // 超过单块 65535 上限，必须分块
        const auto packed = scrctl::util::zlib_store(big);
        back.clear();
        check(zlib_unstore(packed, back, err) && back == big,
              "跨块拼接还原一致: " + err);
        // 篡改一个载荷字节，Adler-32 必须发现
        auto broken = packed;
        broken[100] ^= 0xFF;
        check(!zlib_unstore(broken, back, err), "载荷被改后校验失败: " + err);
    }

    std::printf("\n== offer 的 bplist 外壳 ==\n");
    Offer offer;
    offer.session_id = 2368635137;
    offer.call_id = "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE";
    const auto blob = build_negotiator_offer(offer);
    std::string err;
    auto parsed = scrctl::plist::parse_binary(blob, &err);
    check(parsed.has_value(), "offer 是合法 bplist: " + err);
    if (!parsed) {
        return 1;
    }
    check(parsed->keys.size() == 4, "四个键");
    check(parsed->find("avcMediaStreamNegotiatorMode")->integer == 5, "mode=5");
    check(parsed->find("avcMediaStreamOptionCallID")->string == offer.call_id, "callID 原样");

    std::printf("\n== mediaBlob 的 protobuf 结构 ==\n");
    std::vector<uint8_t> payload;
    check(zlib_unstore(parsed->find("avcMediaStreamNegotiatorMediaBlob")->data, payload, err),
          "blob 可解: " + err);
    std::vector<Field> top;
    check(parse_fields(payload, top), "顶层字段可解析");
    check(only(top, 1) && only(top, 1)->value == 1, "f1=1");
    check(count_of(top, 9) == 10, "码率阶梯 10 条");
    check(only(top, 6) && as_text(only(top, 6)->bytes) == "Viceroy 1.7.0", "编码器名");
    check(only(top, 14) && only(top, 14)->value == 2, "f14=2");

    std::vector<Field> session;
    check(only(top, 5) && parse_fields(only(top, 5)->bytes, session), "会话层可解析");
    check(only(session, 1) && only(session, 1)->value == offer.session_id, "session_id 落位");
    check(count_of(session, 3) == 2, "两个编码器条目（HEVC + AVC）");

    std::vector<Field> hevc, avc;
    {
        std::vector<Field> banks;
        for (const auto &f : session) {
            if (f.number == 3) {
                banks.push_back(f);
            }
        }
        check(banks.size() == 2, "取到两个 bank");
        if (banks.size() == 2) {
            parse_fields(banks[0].bytes, hevc);
            parse_fields(banks[1].bytes, avc);
        }
    }
    check(only(hevc, 1) && only(hevc, 1)->value == 123, "第一个是 HEVC(PT=123)");
    check(only(avc, 1) && only(avc, 1)->value == 100, "第二个是 AVC(PT=100)");
    // 两个编码器的分辨率条目数不同，这不是笔误：整条 bank 的字节长度要等于
    // 可用会话里的观测值，多一条少一条设备看到的长度就对不上。
    check(count_of(hevc, 2) == 4, "HEVC 带 4 条分辨率条目");
    check(count_of(avc, 2) == 2, "AVC 带 2 条分辨率条目");
    check(only(hevc, 3) && as_text(only(hevc, 3)->bytes) == "FLS;SW:1;", "HEVC 能力串");
    check(only(avc, 3) && as_text(only(avc, 3)->bytes) == "FLS;SW:1;", "AVC 能力串");
    const auto *avc_features = only(avc, 3);
    check(avc_features != nullptr && as_text(avc_features->bytes).find("VRAE") == std::string::npos,
          "能力串里不能出现 VRAE:0（带上它编码器改丢帧控码率，实测掉到 ~42fps）");

    std::printf("\n== 主机身份与能力串可注入 ==\n");
    Offer other = offer;
    other.host_model = "Mac9,1";
    other.avc_features = "FLS;SW:1;LTRP:1;";
    const auto other_parsed = scrctl::plist::parse_binary(build_negotiator_offer(other));
    check(other_parsed.has_value(), "改过的 offer 仍是合法 bplist");
    if (other_parsed) {
        std::vector<Field> ep_fields;
        check(parse_fields(other_parsed->find("avcMediaStreamOptionRemoteEndpointInfo")->data,
                           ep_fields) &&
                  ep_fields.size() >= 3 && as_text(ep_fields[2].bytes) == "Mac9,1",
              "主机型号写进 endpoint info");

        std::vector<uint8_t> other_payload;
        std::string e;
        check(zlib_unstore(other_parsed->find("avcMediaStreamNegotiatorMediaBlob")->data,
                           other_payload, e),
              "改过的 blob 可解: " + e);
        std::vector<Field> other_top;
        std::vector<Field> other_session;
        std::vector<Field> other_banks;
        parse_fields(other_payload, other_top);
        if (only(other_top, 5)) {
            parse_fields(only(other_top, 5)->bytes, other_session);
        }
        for (const auto &f : other_session) {
            if (f.number == 3) {
                other_banks.push_back(f);
            }
        }
        std::vector<Field> other_avc;
        if (other_banks.size() == 2) {
            parse_fields(other_banks[1].bytes, other_avc);
        }
        const auto *features = only(other_avc, 3);
        check(features != nullptr && as_text(features->bytes) == "FLS;SW:1;LTRP:1;",
              "改能力串能落到 AVC bank 里（--bit-rate / 编解码开关以后就靠这条口子）");
    }

    std::printf("\n%s (失败 %d 项)\n", Failures == 0 ? "全部通过" : "存在失败", Failures);
    return Failures == 0 ? 0 : 1;
}
