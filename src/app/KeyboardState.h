#pragma once

#include <SDL_keycode.h>

#include <array>
#include <cstdint>
#include <vector>

namespace scrctl::app {

/// 窗口物理键盘的纯状态：每条报告包含所有仍按住的设备 keyboard usages。
/// 不连接 HID、不读取文字或剪贴板；返回报告表示应发送的状态，不代表设备已接收。
class KeyboardState {
public:
    using Report = std::vector<uint16_t>;
    using Reports = std::vector<Report>;

    explicit KeyboardState(uint16_t shortcut_mods = KMOD_LALT | KMOD_LGUI);

    /// consumed 由窗口标记本地快捷键（包含未分配动作的 MOD 组合及 F11）。
    /// 选定 MOD 自身总是本地键；普通键与本地键的所有权保留到 UP。
    /// 设备修饰键也会在事件快照确认已松开时清除归属，允许后续新的 DOWN。
    /// repeat 或重复 DOWN 不改变状态。新普通键之前先同步左右修饰键，并保留其它主键。
    Reports key_down(SDL_Scancode scancode, uint16_t modifiers, bool repeat,
                     bool consumed = false);
    Reports key_up(SDL_Scancode scancode, uint16_t modifiers);

    /// 清除设备键和本地所有权。只有存在设备按住状态时返回一条空报告，重复调用为空。
    Reports release_all();

    /// 完整 usage 集合按数值升序排列；锁定状态和选定 MOD 不属于设备集合。
    Report held() const;

    /// 是否已有设备或本地 DOWN 归属。窗口先查询新按下，再执行本地动作，
    /// 避免重复 DOWN 在修饰键改变后把原本的设备按键误当成新的快捷键。
    bool is_pressed(SDL_Scancode scancode) const;

private:
    enum class Owner : uint8_t { None, Device, Local };

    void sync_modifiers(uint16_t modifiers);
    Reports changes_from(const Report &previous) const;

    uint16_t shortcut_mods_;
    std::array<Owner, SDL_NUM_SCANCODES> owners_{};
    std::array<bool, 240> held_{};
};

} // namespace scrctl::app
