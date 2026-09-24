#include "decode/Decoder.h"

namespace scrctl {

/// 非 Apple 平台的"平台后端"就是软解。
///
/// 这一层壳留着而不是让调用方直接调 create_software_decoder()，是因为切换决策
/// 属于构建配置而不属于业务逻辑：接了 VAAPI/VDPAU/DXVA 之后这里会先试硬件。
std::unique_ptr<Decoder> create_platform_decoder() {
    return create_software_decoder();
}

}  // namespace scrctl
