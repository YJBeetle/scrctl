#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace scrctl::media {

/// CoreDevice 媒体协商的 offer。
///
/// 形态是「bplist 套 zlib 套 protobuf」三层，字段号与取值全部来自一次可用会话的
/// 观测：设备只告诉我们它接受什么，不解释每个数的含义。凡是语义没搞清的，这里
/// 都写成带注释的常量而不是编一个名字糊过去——猜错语义会表现为"流起不来"，
/// 而留着观测值至少能保证起得来。
struct Offer {
    /// 会话号，出现在 blob 内部；每次起流换一个即可。
    uint32_t session_id = 0;
    /// avcMediaStreamOptionCallID，一次调用的追踪号。
    std::string call_id;

    /// 申报的主机身份。设备会按这个挑编码器参数：实测换一个主机型号后编码器
    /// 停顿频率明显不同，所以这不是可以随手填的字段。
    std::string host_model = "Mac15,9";
    std::string host_os_version = "2205.3.1";
    std::string host_build = "25F80";

    /// 能力串。**不能带 `VRAE:0`**：带上后编码器在固定码率上限下靠丢输入帧控
    /// 码率，实测丢 207–219 帧、掉到 ~42fps 且有多帧拖影；去掉后 0 丢帧、
    /// 53–55fps。苹果自己 Xcode 抓包里的 offer 恰恰是带 VRAE:0 的那个慢版本。
    std::string avc_features = "FLS;SW:1;";
    std::string hevc_features = "FLS;SW:1;";

    /// 码率阶梯的变体，**只有 tools/bitrate_probe 会改它，产品路径恒为 0**。
    /// 0 = 原样（唯一验证过能起流的一组数）；1 = 去掉 f2=6000000 那档；
    /// 2 = 把那档改成 60000000；3 = 只留 >=20M 的档。
    ///
    /// 这三个变体是为了验一件事而加的，答案是"别动这张表"，见下面那段。
    int rate_variant = 0;

    /// **这张表既不能调高、也不能删档**（tools/bitrate_probe 实测，docs §11 有表）：
    /// 把 f2=6000000 那档改成 60000000，answer 里依旧回 `TXMaxBitrate: 6000000`，
    /// 所以那个 6 Mbps 不是从我们表里挑的；而把这一档删掉，设备会退到表里
    /// `f2=299` 那条去，实测码率从 5.4Mbps 掉到 0.1Mbps、帧率掉到 8fps，流直接废。
    /// 分辨率同理：`pair_index` 从 0 扫到 6，编码尺寸恒为 1136x2464。
    ///
    /// 早先那轮"缩到 0.25、IDR 字节数不变，所以设备无视 offer"的记法**判据用错了**
    /// （IDR 尺寸由质量决定，不由码率预算决定），结论碰巧没错，但别照着它推理。
    ///
    /// 后果要记清楚：预算锁死 6 Mbps，而单帧能长到 70101 字节，VideoToolbox 只吃
    /// 2 字节长度前缀（上限 65535）——所以"关键帧大到喂不进去"是常态风险，只能靠
    /// 软解后端兜，见 decode/Decoder.h 的 create_software_decoder。
    ///
    /// 别把"镜像只有 12fps"算到这张表头上：那次实测每帧包数在快慢两种状态下都是
    /// ~10，也就是帧变小了而不是变大——等比缩小说明是**我们没把包收完**（每帧重新
    /// 分配并 memset 11MB 的开销），不是设备在保码率砍帧率。数字见 docs §11。
};

/// negotiatorOffer 的 bplist 字节，直接塞进 startmediastream 请求的
/// `negotiatorOffer` 字段。
[[nodiscard]] std::vector<uint8_t> build_negotiator_offer(const Offer &offer);

}  // namespace scrctl::media
