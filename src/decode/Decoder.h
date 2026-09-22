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

    [[nodiscard]] virtual const char *backend_name() const = 0;
};

/// 当前平台的默认解码后端。macOS 上是 VideoToolbox。
std::unique_ptr<Decoder> create_platform_decoder();

}  // namespace scrctl
