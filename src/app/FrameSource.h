#pragma once

#include "i18n/Translation.h"

#include "decode/Decoder.h"
#include <string>

namespace scrctl::app {

class FrameSource {
  public:
    virtual ~FrameSource() = default;

    /// 返回 false 表示本次没有取得帧，调用方应继续处理窗口事件。
    /// 只有 finished() 为 true 才表示源已结束；实现可短时阻塞，避免消费循环空转。
    virtual bool next(scrctl::Frame &out, int timeout_ms) = 0;

    [[nodiscard]] virtual bool finished() const { return false; }
    /// 源结束时提供原因。实时源应区分设备断开和暂时的取帧失败。
    [[nodiscard]] virtual std::string end_reason() const { return SCRCTL_TR("Source ended"); }
    /// 文件回放要自己按标称帧率追节拍；实时流的到达节奏就是设备的节奏。
    [[nodiscard]] virtual bool paces_itself() const { return false; }
    /// 输出统计；实现按各自时间基线计算速率，调用方按固定周期调用。
    virtual void print_stats() {}

    /// 返回设备可见区尺寸，0/0 表示未知。实时源在起流前查询设备；
    /// 文件回放无设备信息，由调用方使用机型表或编码尺寸。
    virtual void display_size(int &width, int &height) const {
        width = 0;
        height = 0;
    }

    /// 返回将码流转正所需的顺时针角度；未知时返回 0。
    /// 实测设备旋转时编码尺寸仍为 1136x2464，朝向由 currentOrientation 报告，
    /// 不能仅从帧尺寸推断。currentMode.size 仍为 1125x2436，所以旋转不改变裁剪框。
    virtual int orientation_degrees() const { return 0; }
};

} // namespace scrctl::app
