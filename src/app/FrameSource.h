#pragma once

#include "decode/Decoder.h"
#include <string>

namespace scrctl::app {

class FrameSource {
  public:
    virtual ~FrameSource() = default;

    /// 返回 false 表示这次没取到（超时），调用方应该去泵一遍事件循环。
    /// `finished()` 为真才是真的结束。实现可以阻塞一小会儿再返回 false——
    /// 否则消费循环会在两帧之间空转，吃满一颗核还把窗口饿出事件事件。
    virtual bool next(scrctl::Frame &out, int timeout_ms) = 0;

    [[nodiscard]] virtual bool finished() const { return false; }
    /// `finished()` 为真时打给用户的最后一句。默认那句适合文件回放；实时源要说清
    /// **为什么**结束（拔线与"截图暂时失败"对用户是两件完全不同的事）。
    [[nodiscard]] virtual std::string end_reason() const { return "源已结束"; }
    /// 文件回放要自己按标称帧率追节拍；实时流的到达节奏就是设备的节奏。
    [[nodiscard]] virtual bool paces_itself() const { return false; }
    /// 打一段读数。实现方自己按调用间隔算速率，所以调用方只管按秒催。
    virtual void print_stats() {}

    /// 这块画面在设备上真正占多大（**可见区**，不是编码帧）。0/0 = 问不到。
    ///
    /// 只有实时源问得到：它是起流之前向设备的 `displayinfoupdates` 要来的。文件回放
    /// 没有设备可问，退回默认实现给 0，调用方再退回兜底表。
    virtual void display_size(int &width, int &height) const {
        width = 0;
        height = 0;
    }

    /// 画面要**顺时针**转多少度才正立。0 = 竖屏，或问不到。
    ///
    /// 为什么必须由源来报而不是由窗口自己看：编码帧**永远不转**（这台设备上横竖屏
    /// 都是 1136x2464），转屏只体现在设备报的 `currentOrientation` 上。所以窗口里
    /// 没有任何线索可推——不问就是横屏 App 躺倒。
    ///
    /// 注意它**不影响可见区尺寸**：实测界面转到 rot270 时 `currentMode.size` 仍是
    /// 1125x2436，只有朝向字段变了。所以裁剪框不用跟着换向（docs §16）。
    virtual int orientation_degrees() const { return 0; }
};

} // namespace scrctl::app
