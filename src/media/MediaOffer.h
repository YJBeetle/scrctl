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
    /// 本腿在 offer 里声明的 SSRC（视频放 `VideoSettings.f1`、音频放 f3.f1）。
    ///
    /// 名字里的 "session" 是历史遗留，语义后来查清了：设备会把它原样回成 answer 里的
    /// `RemoteSSRC`，而我们发 RTCP 时的发送者 SSRC 用的就是 `RemoteSSRC`——三处对得上，
    /// 所以这不是一个随手换的追踪号，而是**本腿自己的 SSRC**。两条腿必须各用一个
    /// （苹果那份抓包里视频 6667224、音频 1640585081，互不相同）。
    uint32_t session_id = 0;
    /// avcMediaStreamOptionCallID，一次调用的追踪号。
    std::string call_id;

    /// 这条 offer 是给**音频腿**的还是视频腿的。
    ///
    /// 为什么要有：Xcode DeviceHub 的抓包里它是**先起音频再起视频**，两条腿共用同一个
    /// `avcMediaStreamOptionClientSessionID`，而那条精确 1.000Hz、整场从不空档的
    /// `RR+SDES` 发在**音频腿**上（视频腿上整场几乎没有 RR）。如果设备的超时计时器挂在
    /// "这条 ClientSessionID 的会话"而不是"这条腿"上，那视频能活 74 秒靠的就是音频腿在喂它
    /// ——这是最后一个还没控住的结构性差异，所以要能把音频腿单独起起来。
    ///
    /// 两条腿在 offer 里的差别只有三处（逐字节对比过苹果那两份）：容器里
    /// `avcMediaStreamNegotiatorMode` 音频 6 / 视频 5；设置消息视频放 f5
    /// （`VideoSettings`）、音频放 f3（小得多，`{f1=本腿 SSRC, f4=24191}`）；
    /// `avcMediaStreamOptionCallID` 每条腿各一个。码率阶梯 `f9`、`f6='Viceroy 1.7.0'`、
    /// `f8/f13/f14/f16/f18` 两份**完全一致**，所以这里复用同一个构造器而不是复制一份。
    bool is_audio = false;

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

    /// offer 里 `VideoSettings` 的第 2 个字段，`allowRTCPFB`。默认 0 = 不申报。
    ///
    /// 为什么要能改它：设备那条 20 秒租期明写着是 `RTCPTimeoutInterval`（docs §13），
    /// 而我们把 RR 的字节数（长度域是 16 位，不是 32 位）、SSRC（用 answer 分配的
    /// `RemoteSSRC` 当发送者）、目的端口全都对成和参考实现一模一样之后，**仍然 20.0 秒
    /// 死**。到这一步我们与 Apple 客户端已知的差别就只剩 offer 里这两个开关。如果设备
    /// 是因为这一位为 0 而根本不理会我们发过去的 RTCP，那这里才是那个门。
    ///
    /// 另一个已知的连带后果（参考实现的抓包笔记原文）："the device ignores RTCP PLI for
    /// refresh; it honors **FIR (PT=206 FMT=4, requires `allowRTCPFB`)**"。也就是说这一位
    /// 是"要不要受理关键帧请求"的总闸——我们的 fir_probe 当年是在闸关着的情况下测 FIR 的。
    bool allow_rtcp_fb = false;
    /// `VideoSettings` 第 7 个字段 `ltrpEnabled`（长期参考图）。参考实现记着苹果 Xcode
    /// 抓包里的 offer 用的是 1，而我们的观测值是 0（当时照抄观测）。做成开关是为了把
    /// "与 Apple 的差异"这一列清干净，不是因为我们猜它管租期。
    ///
    /// 顺带两条实测口径（参考实现在 iPhone18,4/iOS 27.0 上）：这一位设备是**受理并照办**的
    /// （answer 的 streamConfig 里会回 `IsltrpEnabled`），而且 LTRP 开/关不改变编码器丢帧数。
    bool ltrp_enabled = false;

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
