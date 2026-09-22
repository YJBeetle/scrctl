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

/// 分辨率条目：{1, pair, 50115, 0}。50115 的含义未知，但设备认它。
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

/// 会话本体：session_id + 两个编码器条目 + 四个语义未明的定值。
std::vector<uint8_t> session_blob(const Offer &offer) {
    std::vector<uint8_t> out;
    put_field_varint(out, 1, offer.session_id);
    put_field_varint(out, 2, 0);
    put_field_bytes(out, 3, codec_bank(123, offer.hevc_features, 1, 4));
    put_field_bytes(out, 3, codec_bank(100, offer.avc_features, 14, 2));
    // f7/f8/f10/f12：观测值 0 / 63 / 1 / 1。参考实现把 allowRTCPFB、fecEnabled、
    // tilesPerFrame 这类开关也归在这一带，但对应关系没确认，所以先照观测写死。
    put_field_varint(out, 7, 0);
    put_field_varint(out, 8, 63);
    put_field_varint(out, 10, 1);
    put_field_varint(out, 12, 1);
    return out;
}

/// 码率阶梯。f2 看着是 bps 上限、f3 是缓冲或 QP 相关量；改 --bit-rate 时要动的
/// 应该就是这几对，但先照观测原样发。
std::vector<uint8_t> rate_table() {
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
    entry({{1, 4074}, {2, 0}, {3, 16384}});
    entry({{1, 0}, {2, 75000000}, {3, 524288}});
    entry({{1, 0}, {2, 40000000}, {3, 12288}});
    entry({{1, 16}, {2, 4100}});
    entry({{1, 0}, {2, 20000000}, {3, 98304}});
    entry({{1, 4}, {2, 6500}});
    entry({{1, 0}, {2, 6000000}, {3, 131072}});
    entry({{1, 0}, {2, 100000000}, {3, 1048576}});
    entry({{1, 0}, {2, 60000000}, {3, 262144}});
    entry({{1, 1}, {2, 299}});
    return out;
}

std::vector<uint8_t> media_blob(const Offer &offer) {
    std::vector<uint8_t> out;
    put_field_varint(out, 1, 1);
    put_field_varint(out, 2, 1);
    put_field_bytes(out, 5, session_blob(offer));
    put_field_string(out, 6, "Viceroy 1.7.0");
    put_field_varint(out, 8, 0);
    auto rates = rate_table();
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
