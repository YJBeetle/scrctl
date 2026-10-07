#pragma once

#include "i18n/Translation.h"

#include "decode/Decoder.h"
#include "app/ViewGeom.h"
#include <string>

namespace scrctl::app {

class FrameSource {
  public:
    virtual ~FrameSource() = default;

    /// 返回 false 表示本次没有取得帧，调用方应继续处理窗口事件。
    /// 只有 finished() 为 true 才表示源已结束；实现可短时阻塞，避免消费循环空转。
    virtual bool next(scrctl::Frame &out, int timeout_ms) = 0;

    [[nodiscard]] virtual bool finished() const { return false; }
    /// 已结束的源是否遇到不可恢复的错误；文件正常读完与用户停止不算失败。
    [[nodiscard]] virtual bool failed() const { return false; }
    /// 源结束时提供原因。实时源应区分设备断开和暂时的取帧失败。
    [[nodiscard]] virtual std::string end_reason() const { return SCRCTL_TR("Source ended"); }
    /// 文件回放要自己按标称帧率追节拍；实时流的到达节奏就是设备的节奏。
    [[nodiscard]] virtual bool paces_itself() const { return false; }
    /// 输出统计；实现按各自时间基线计算速率，调用方按固定周期调用。
    virtual void print_stats() {}

    /// 返回最近一次 next() 成功交付帧的几何，不读取后续改变的画面来源。
    /// 实时源保存原始面板角与截图标志的软件快照；显式渲染角不改变这份输入依据。
    /// 文件回放默认无设备尺寸且像素未旋转，由调用方使用裁剪表或编码尺寸。
    virtual FrameGeometry frame_geometry() const { return {}; }
};

} // namespace scrctl::app
