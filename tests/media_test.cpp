// 媒体协商 offer 的离线自检。这块必须在接上真机之前就能自己证明自己，
// 因为一旦上线，"流起不来"的原因可能是 offer 里任何一个字节。
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include "media/AudioPump.h"
#include "media/FramePump.h"
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

    std::printf("\n== 编码帧到逻辑显示的裁剪 ==\n");
    {
        // 真机那对数字：编码 1136x2464，逻辑显示 1125x2436（右 11 / 底 28 是 CTU 填充）。
        const auto c = scrctl::media::display_crop(1136, 2464);
        check(c.x == 0 && c.y == 0 && c.w == 1125 && c.h == 2436,
              "1136x2464 -> 1125x2436");
        // 认不出的尺寸必须原样给回去，不能"顺手"裁一刀。
        const auto passthrough = scrctl::media::display_crop(1290, 2796);
        check(passthrough.w == 1290 && passthrough.h == 2796, "未知分辨率不裁");
        const auto zero = scrctl::media::display_crop(0, 0);
        check(zero.w == 0 && zero.h == 0, "0x0 不崩");
    }

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

    // 音频水位与环容量。这一段是纯算术，不需要设备——而它要防的正是"没设备就测不到"
    // 的那类错：容量写死 500ms、水位随便填，于是 `--audio-buffer 600` 的症状是整条腿
    // 静音（出口那道 `buffered < preroll` 的闸门永远开着），不是"延迟大一点"。
    std::printf("\n== 音频水位与环容量 ==\n");
    {
        using scrctl::media::AudioPump;
        auto opts = [](int ms) {
            AudioPump::Options o;
            o.target_backlog_ms = ms;
            return o;
        };
        const auto def = AudioPump::compute_waterline(opts(50));
        check(def.target_frames == 2400 && def.capacity_frames == 24000,
              "默认 50ms 保持原样（2400 帧水位 / 24000 帧环）");
        check(def.clamped_to_ms == 0, "默认不被收档");

        const auto big = AudioPump::compute_waterline(opts(600));
        check(big.target_frames == 600 * 48, "600ms 仍然是 600ms：容量跟着长，不是把水位压低");
        check(big.capacity_frames > big.target_frames * 2,
              "容量至少在两倍水位之上，否则'超两倍就砍'那一档永远撞不到");

        const auto huge = AudioPump::compute_waterline(opts(999999));
        check(huge.clamped_to_ms == 1000 && huge.target_frames == 48000,
              "离谱的值收到 1000ms 并报出来（一个命令行参数不该决定分配多少内存）");

        check(AudioPump::compute_waterline(opts(0)).target_frames == 0, "0 = 不攒，取多少给多少");
        check(AudioPump::compute_waterline(opts(-5)).target_frames == 0, "负数按 0 处理");

        int bad = 0;
        for (int ms : {-1000, 0, 1, 5, 50, 100, 400, 500, 501, 600, 900, 1000, 1001, 5000,
                       999999, 2000000}) {
            const auto w = AudioPump::compute_waterline(opts(ms));
            if (w.target_frames >= w.capacity_frames || w.capacity_frames < 24000) {
                ++bad;
                std::printf("    水位 %d ms: target=%zu capacity=%zu\n", ms, w.target_frames,
                            w.capacity_frames);
            }
        }
        check(bad == 0, "扫一遍请求值：水位永远够得着（这条挂了就是'设了反而静音'）");
    }

    // "起流后一直解不出关键帧"那把阶梯。这一段真机要跑一分多钟才走得到顶，而它顶上
    // 原本是一个**看不见的终态**：重试满 3 次就停手，SR 每秒还在喂 `last_packet_ms_`
    // 让静默那条永不响，新会话又把后备的武装清了 —— 于是画面永久停在最后一帧好画上，
    // 而进程、隧道全都好着（review 的 P2）。这里判的就是"到顶不许等于停手"。
    std::printf("\n== 解不出关键帧的阶梯：到顶转降级，不许停手 ==\n");
    {
        using scrctl::media::NokeyAction;
        constexpr int kMax = 3;
        check(plan_nokey(0, kMax, false) == NokeyAction::kRetry, "第一次：照正常节拍重起");
        check(plan_nokey(2, kMax, false) == NokeyAction::kRetry, "没到上限就还是重起");
        check(plan_nokey(3, kMax, false) == NokeyAction::kDegrade,
              "到顶这一次：转成降级（而不是什么都不做）");
        check(plan_nokey(4, kMax, false) == NokeyAction::kDegrade,
              "计数万一越过上限也要能补上降级");
        check(plan_nokey(3, kMax, true) == NokeyAction::kWait,
              "已经降级了：这一轮什么都不做，交给退避计时管（两处都排会互相吃掉间隔）");
        // 上限本身不许被写成 0：那等于一上来就降级，正常那一档（开头 IDR 真被丢）就没人救了。
        check(plan_nokey(0, 0, false) == NokeyAction::kDegrade, "上限 0 也要走降级，不能停手");
        int stops = 0;
        for (int r = 0; r <= 12; ++r) {
            // 阶梯走到底之后不该出现"既不是重起也不是降级、还一直不变"的死格。
            const auto a = plan_nokey(r, kMax, r > kMax);
            if (a == NokeyAction::kWait && r <= kMax) {
                ++stops;
            }
        }
        check(stops == 0, "扫一遍次数：没到上限之前永远不会提前躺平");
    }

    std::printf("\n%s (失败 %d 项)\n", Failures == 0 ? "全部通过" : "存在失败", Failures);
    return Failures == 0 ? 0 : 1;
}
