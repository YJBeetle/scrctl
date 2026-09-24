#include "decode/Decoder.h"

namespace scrctl {

/// 没编 libavcodec 时的占位：软解后端不存在。
///
/// 调用方必须处理空指针。在 macOS 上的后果是"超过 65535 字节的 NAL 只能整帧丢"
/// ——而这条流不周期发关键帧，丢的又正好是 IDR，于是画面永久灰掉。所以装 ffmpeg
/// 不是可选的性能项，是这类码流能用的前提。
std::unique_ptr<Decoder> create_software_decoder() {
    return nullptr;
}

}  // namespace scrctl
