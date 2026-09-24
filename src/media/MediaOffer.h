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

    /// 这里**没有**码率/分辨率旋钮，因为实测它们不管用：把码率阶梯里像 bps 的
    /// f2、像缓冲的 f3 各自缩到 0.25（以及两者同时 0.25）之后再下发，主屏 IDR
    /// 分别是 49652 / 49789 / 49852 字节，和不缩一样；把分辨率条目的 pair_index
    /// 从 0 扫到 6，编码尺寸始终是 1136x2464。也就是说设备不照 offer 里这几串数
    /// 编，单帧多大由它自己定。
    ///
    /// 后果要记清楚：单帧能长到 70101 字节，而 VideoToolbox 只吃 2 字节长度前缀
    /// （上限 65535），所以"关键帧大到喂不进去"不是小概率的边角情况，而是常态
    /// 风险——只能靠软解后端兜，见 decode/Decoder.h 的 create_software_decoder。
};

/// negotiatorOffer 的 bplist 字节，直接塞进 startmediastream 请求的
/// `negotiatorOffer` 字段。
[[nodiscard]] std::vector<uint8_t> build_negotiator_offer(const Offer &offer);

}  // namespace scrctl::media
