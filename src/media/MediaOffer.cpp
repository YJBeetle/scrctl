#include "media/MediaOffer.h"

#include "plist/Bplist.h"
#include "util/Deflate.h"

namespace scrctl::media {
namespace {

// ---------------------------------------------------------- protobuf 写入 ------
// 只需要 varint 与长度定界两种线型：这套 offer 里没有浮点、没有定宽数。

void put_varint(std::vector<uint8_t> &out, uint64_t v) {
    do {
        const auto byte = static_cast<uint8_t>(v & 0x7F);
        v >>= 7;
        out.push_back(v ? byte | 0x80 : byte);
    } while (v);
}

void put_field_varint(std::vector<uint8_t> &out, uint32_t field, uint64_t v) {
    put_varint(out, field << 3 | 0);
    put_varint(out, v);
}

void put_field_bytes(std::vector<uint8_t> &out, uint32_t field, const std::vector<uint8_t> &b) {
    put_varint(out, field << 3 | 2);
    put_varint(out, b.size());
    out.insert(out.end(), b.begin(), b.end());
}

void put_field_string(std::vector<uint8_t> &out, uint32_t field, std::string_view s) {
    put_varint(out, field << 3 | 2);
    put_varint(out, s.size());
    out.insert(out.end(), s.begin(), s.end());
}

std::vector<uint8_t> msg(std::initializer_list<std::vector<uint8_t>> parts) {
    std::vector<uint8_t> out;
    for (const auto &p : parts) {
        out.insert(out.end(), p.begin(), p.end());
    }
    return out;
}

std::vector<uint8_t> field_varint(uint32_t field, uint64_t v) {
    std::vector<uint8_t> out;
    put_field_varint(out, field, v);
    return out;
}

// ------------------------------------------------------------- 各层结构 --------
// 下面这些字段号与取值全部来自一次可用会话的观测。语义不明的都标了出来。

/// 分辨率条目：{1, pair, 50115, 0}。50115 与 pair 的含义未知，但设备认它。
///
/// 别指望 pair 是"挑一档分辨率"：把它从 0 扫到 6，编码尺寸一直是 1136x2464。
std::vector<uint8_t> res_entry(uint64_t pair_index) {
    return msg({field_varint(1, 1), field_varint(2, pair_index), field_varint(3, 50115),
                field_varint(4, 0)});
}

/// 一个编码器的能力条目。PT=123 是 HEVC，PT=100 是 AVC——实测设备最终协商的是
/// PT=100 那一组，所以 features 字符串要改就得改它。
///
/// res_entries 两个编码器不一样（HEVC 4 条、AVC 2 条），不是笔误：整条 bank 的
/// 字节长度要等于观测值，多一条少一条都会让设备看到的长度对不上。
std::vector<uint8_t> codec_bank(uint64_t payload_type, std::string_view features, uint64_t f4,
                                uint64_t res_entries) {
    std::vector<uint8_t> out = field_varint(1, payload_type);
    for (uint64_t i = 0; i < res_entries; ++i) {
        put_field_bytes(out, 2, res_entry(i % 2 + 1));
    }
    put_field_string(out, 3, features);
    auto tail = field_varint(4, f4);
    out.insert(out.end(), tail.begin(), tail.end());
    return out;
}

/// 会话本体：session_id、两个开关、两个编码器条目、三个定值。
std::vector<uint8_t> session_blob(const Offer &offer) {
    std::vector<uint8_t> out;
    put_field_varint(out, 1, offer.session_id);
    // 字段名是别人从 `VCMediaNegotiationBlobVideoSettings` 的 `__objc_methname` 方法名表
    // 里恢复出来的，整张表：f1 SSRC(=session_id)、f2 allowRTCPFB、f3 videoPayloadCollections
    // （即编码器 bank）、f4 customVideoWidth、f5 customVideoHeight、f6 tilesPerFrame、
    // f7 ltrpEnabled、f8 pixelFormats、f9 hdrModesSupported、f10 fecEnabled、f11 rtxEnabled、
    // f12 blackFrameOnClearScreenEnabled、f13 foveationSupported、f14 enableInterleavedEncoding。
    // 于是我们那几个"语义未明的定值"有了名字：f8=63 按名字读是像素格式位掩码（低 6 位全
    // 开，具体哪 6 种没查），f10=1 是打开 FEC，f12=1 是"清屏时输出黑帧"。为什么仍然照观测发而不是发我们以为更好
    // 的值：f11 rtxEnabled 发 1 会被设备判 Invalid Parameter，说明这张表里不是每个字段都能
    // 随手改。f6/f9/f13/f14 我们的观测里没有，就不写（省略与写零在语义上等价，但字节长度不等价）。
    //
    // 字段号要按升序写，且两个开关默认关时整个 blob 必须和观测字节完全一致——
    // 起流这件事只验证过"逐字节照抄观测"这一种写法。
    put_field_varint(out, 2, offer.allow_rtcp_fb ? 1 : 0);
    put_field_bytes(out, 3, codec_bank(123, offer.hevc_features, 1, 4));
    put_field_bytes(out, 3, codec_bank(100, offer.avc_features, 14, 2));
    put_field_varint(out, 7, offer.ltrp_enabled ? 1 : 0);
    put_field_varint(out, 8, 63);
    put_field_varint(out, 10, 1);
    put_field_varint(out, 12, 1);
    return out;
}

/// 码率阶梯。f2 看着是 bps 上限、f3 是缓冲或 QP 相关量。
///
/// **改它没有用，别再试了**：把 f2、f3 各自缩到 0.25（以及同时缩）再下发，主屏
/// IDR 是 49652 / 49789 / 49852 字节，与不缩时的 49652 没有区别。设备不照这张表
/// 编，单帧多大由它自己定。所以"压小关键帧"这条路是死的，超大帧只能靠软解后端。
/// 观测值原样发，是唯一验证过能起流的一组数。
std::vector<uint8_t> rate_table(const Offer &offer) {
    std::vector<uint8_t> out;
    auto entry = [&out](std::vector<std::pair<uint32_t, uint64_t>> fields) {
        std::vector<uint8_t> body;
        for (const auto [field, value] : fields) {
            put_field_varint(body, field, value);
        }
        put_field_bytes(out, 9, body);
    };
    // 观测里每条的 f1 都在（值为 0 也写出来）。protobuf 语义上省略零字段与写
    // 零等价，但字节长度就不同了，所以照观测原样写，不做"优化"。
    auto f2_of = [](const std::vector<std::pair<uint32_t, uint64_t>> &e) {
        for (const auto [f, v] : e) {
            if (f == 2) {
                return v;
            }
        }
        return uint64_t { 0 };
    };
    const std::vector<std::vector<std::pair<uint32_t, uint64_t>>> observed = {
        {{1, 4074}, {2, 0}, {3, 16384}},        {{1, 0}, {2, 75000000}, {3, 524288}},
        {{1, 0}, {2, 40000000}, {3, 12288}},    {{1, 16}, {2, 4100}},
        {{1, 0}, {2, 20000000}, {3, 98304}},    {{1, 4}, {2, 6500}},
        {{1, 0}, {2, 6000000}, {3, 131072}},    {{1, 0}, {2, 100000000}, {3, 1048576}},
        {{1, 0}, {2, 60000000}, {3, 262144}},   {{1, 1}, {2, 299}},
    };
    for (auto e : observed) {
        if (offer.rate_variant == 1 && f2_of(e) == 6000000) {
            continue;  // 去掉 6M 这一档，看设备改挑哪一档
        }
        if (offer.rate_variant == 2 && f2_of(e) == 6000000) {
            for (auto &[f, v] : e) {
                if (f == 2) {
                    v = 60000000;
                }
            }
        }
        if (offer.rate_variant == 3 && f2_of(e) >= 1000 && f2_of(e) < 20000000) {
            continue;  // 只留 >=20M 的档
        }
        entry(e);
    }
    return out;
}

std::vector<uint8_t> media_blob(const Offer &offer) {
    std::vector<uint8_t> out;
    put_field_varint(out, 1, 1);
    put_field_varint(out, 2, 1);
    put_field_bytes(out, 5, session_blob(offer));
    put_field_string(out, 6, "Viceroy 1.7.0");
    put_field_varint(out, 8, 0);
    auto rates = rate_table(offer);
    out.insert(out.end(), rates.begin(), rates.end());
    // f13 是从可用会话里带出来的一个时间戳常量。设备不校验它（参考实现同样
    // 用固定值），所以照抄，不去编一个"现在的时间"——编错了反而没人能解释。
    put_field_varint(out, 13, 17137042128614416384ULL);
    put_field_varint(out, 14, 2);
    put_field_varint(out, 16, 0);
    put_field_varint(out, 18, 1);
    return out;
}

std::vector<uint8_t> remote_endpoint_info(const Offer &offer) {
    std::vector<uint8_t> out;
    put_field_varint(out, 1, 0);
    put_field_varint(out, 2, 1);
    put_field_string(out, 3, offer.host_model);
    put_field_string(out, 4, offer.host_os_version);
    put_field_string(out, 5, offer.host_build);
    return out;
}

}  // namespace

std::vector<uint8_t> build_negotiator_offer(const Offer &offer) {
    auto d = plist::Value::Dict();
    d.set("avcMediaStreamNegotiatorMediaBlob",
          plist::Value::OfData(util::zlib_store(media_blob(offer))));
    d.set("avcMediaStreamNegotiatorMode", plist::Value::Int(5));
    d.set("avcMediaStreamOptionCallID", plist::Value::Str(offer.call_id));
    d.set("avcMediaStreamOptionRemoteEndpointInfo",
          plist::Value::OfData(remote_endpoint_info(offer)));
    return plist::write_binary(d);
}

}  // namespace scrctl::media
