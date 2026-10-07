#include "decode/Decoder.h"

namespace scrctl {

/// 未编入 libavcodec 时的软件解码工厂，返回 nullptr。
/// 调用方必须处理空结果。Apple 构建仍可使用 VideoToolbox，但若码流超出当前
/// 适配器的 NAL 长度上限，就无法通过此工厂切换到软件后端。丢弃关键帧可能使
/// 后续参考链不可解码，是否恢复取决于后续完整关键帧及调用方的恢复策略。
std::unique_ptr<Decoder> create_software_decoder() {
    return nullptr;
}

}  // namespace scrctl
