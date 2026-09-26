#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "bitstream/AnnexB.h"

namespace scrctl {

/// 一帧 CPU 侧可读的 BGRA 图像。
///
/// M1 阶段先接受一次拷贝（VideoToolbox 输出 CVPixelBuffer，锁基址后按行拷进
/// pixels）。零拷贝（直接绑成 Metal/SDL GPU 纹理）是后续的性能工作，
/// 不该在链路还没跑通时把它绑进接口。
struct Frame {
    std::vector<uint8_t> pixels;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t row_pitch = 0;  ///< 一行占多少字节，可能大于 width*4

    uint32_t bytes_per_pixel = 4;

    [[nodiscard]] explicit operator bool() const { return width > 0 && height > 0; }
    [[nodiscard]] uint32_t display_width() const { return width; }
};

class Decoder {
public:
    virtual ~Decoder() = default;

    /// 用一组参数集建立（或重建）解码会话。
    virtual bool configure(const Nal &vps, const Nal &sps, const Nal &pps) = 0;

    /// 解一个 access unit。一个 AU 可能不产出任何帧（参考帧尚未就绪），
    /// 此时返回 false 且 out 为空——这不算致命错误。
    virtual bool decode(const std::vector<Nal> &au, Frame &out) = 0;

    /// 这个后端能接受的**单个 NAL** 上限（字节）。
    ///
    /// 为什么要在接口里：长度前缀是后端能力的一部分，而"这一个 AU 里最大的 NAL
    /// 有多大"只有取到完整 AU 的调用方知道。VideoToolbox 只接受 2 字节前缀
    /// （4 字节的会话见一个样本拒一个，docs §11），所以它吃不下 65535 字节以上的
    /// NAL —— 真机主屏的一个 IDR 就实测过 70101 字节。取 SIZE_MAX 表示无此限制。
    [[nodiscard]] virtual size_t max_nal_size() const { return ~size_t { 0 }; }

    [[nodiscard]] virtual const char *backend_name() const = 0;
};

/// 当前平台的默认解码后端。macOS 上是 VideoToolbox（硬件，快）。
std::unique_ptr<Decoder> create_platform_decoder();

/// 软件解码后端（libavcodec）。**没编进来就返回 nullptr**，所以调用方必须能
/// 处理空——平台后端装不下的码流在这种情况下就只能丢帧了。
///
/// 它是"码流超出平台后端能力"时唯一的出路，也是 Linux/Windows 上的主力后端。
std::unique_ptr<Decoder> create_software_decoder();

/// 这个构建里到底有没有能用的 HEVC 后端。
///
/// 为什么要单独问这么一句：两个工厂**都**返回空的组合只有"非 Apple 平台 + 配置构建时
/// 没找到 libavcodec"一种，而它在编译时就完全定下来了。不早问的现场表现是解到第一帧
/// 时空指针——平台后端在别处就是软解的一层壳（`decode/PlatformDecoderFallback.cpp`），
/// 壳里没有东西时它也给不出东西。
#if defined(__APPLE__) || defined(SCRCTL_HAVE_LIBAV)
inline constexpr bool kHaveDecoder = true;
#else
inline constexpr bool kHaveDecoder = false;
#endif

/// 一个后端都没有时给用户的那句话。两条取帧的路共用，免得两边各说一半。
inline constexpr const char *kNoDecoderMessage =
    "这个构建里没有可用的 HEVC 解码后端：非 Apple 平台只能靠 libavcodec 软解，"
    "而配置构建时没找到它。装开发包之后重新配置构建（Debian/Ubuntu: "
    "apt install libavcodec-dev libavutil-dev； Fedora: dnf install ffmpeg-devel）。\n";

}  // namespace scrctl
