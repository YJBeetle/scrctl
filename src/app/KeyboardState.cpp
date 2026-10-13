#include "app/KeyboardState.h"

#include <cstddef>
#include <utility>

namespace scrctl::app {
namespace {

struct Modifier {
    SDL_Scancode scancode;
    uint16_t mask;
    uint16_t usage;
};

constexpr Modifier Modifiers[] = {
    {SDL_SCANCODE_LCTRL, KMOD_LCTRL, 0xE0},
    {SDL_SCANCODE_LSHIFT, KMOD_LSHIFT, 0xE1},
    {SDL_SCANCODE_LALT, KMOD_LALT, 0xE2},
    {SDL_SCANCODE_LGUI, KMOD_LGUI, 0xE3},
    {SDL_SCANCODE_RCTRL, KMOD_RCTRL, 0xE4},
    {SDL_SCANCODE_RSHIFT, KMOD_RSHIFT, 0xE5},
    {SDL_SCANCODE_RALT, KMOD_RALT, 0xE6},
    {SDL_SCANCODE_RGUI, KMOD_RGUI, 0xE7},
};

bool valid_index(SDL_Scancode scancode) {
    const int index = static_cast<int>(scancode);
    return index > 0 && index < SDL_NUM_SCANCODES;
}

const Modifier *modifier_for(SDL_Scancode scancode) {
    for (const auto &modifier : Modifiers)
        if (modifier.scancode == scancode) return &modifier;
    return nullptr;
}

/// 这里只接受 SDL 已明确对应 USB keyboard page 0x07 的常用键。
/// 媒体键、系统电源键和扩展 scancode 不能仅凭数值落在位图内就接受。
uint16_t ordinary_usage(SDL_Scancode scancode) {
    switch (scancode) {
        case SDL_SCANCODE_A: case SDL_SCANCODE_B: case SDL_SCANCODE_C:
        case SDL_SCANCODE_D: case SDL_SCANCODE_E: case SDL_SCANCODE_F:
        case SDL_SCANCODE_G: case SDL_SCANCODE_H: case SDL_SCANCODE_I:
        case SDL_SCANCODE_J: case SDL_SCANCODE_K: case SDL_SCANCODE_L:
        case SDL_SCANCODE_M: case SDL_SCANCODE_N: case SDL_SCANCODE_O:
        case SDL_SCANCODE_P: case SDL_SCANCODE_Q: case SDL_SCANCODE_R:
        case SDL_SCANCODE_S: case SDL_SCANCODE_T: case SDL_SCANCODE_U:
        case SDL_SCANCODE_V: case SDL_SCANCODE_W: case SDL_SCANCODE_X:
        case SDL_SCANCODE_Y: case SDL_SCANCODE_Z:
        case SDL_SCANCODE_1: case SDL_SCANCODE_2: case SDL_SCANCODE_3:
        case SDL_SCANCODE_4: case SDL_SCANCODE_5: case SDL_SCANCODE_6:
        case SDL_SCANCODE_7: case SDL_SCANCODE_8: case SDL_SCANCODE_9:
        case SDL_SCANCODE_0:
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_ESCAPE:
        case SDL_SCANCODE_BACKSPACE: case SDL_SCANCODE_TAB: case SDL_SCANCODE_SPACE:
        case SDL_SCANCODE_MINUS: case SDL_SCANCODE_EQUALS:
        case SDL_SCANCODE_LEFTBRACKET: case SDL_SCANCODE_RIGHTBRACKET:
        case SDL_SCANCODE_BACKSLASH: case SDL_SCANCODE_NONUSHASH:
        case SDL_SCANCODE_SEMICOLON: case SDL_SCANCODE_APOSTROPHE:
        case SDL_SCANCODE_GRAVE: case SDL_SCANCODE_COMMA:
        case SDL_SCANCODE_PERIOD: case SDL_SCANCODE_SLASH:
        case SDL_SCANCODE_CAPSLOCK:
        case SDL_SCANCODE_F1: case SDL_SCANCODE_F2: case SDL_SCANCODE_F3:
        case SDL_SCANCODE_F4: case SDL_SCANCODE_F5: case SDL_SCANCODE_F6:
        case SDL_SCANCODE_F7: case SDL_SCANCODE_F8: case SDL_SCANCODE_F9:
        case SDL_SCANCODE_F10: case SDL_SCANCODE_F11: case SDL_SCANCODE_F12:
        case SDL_SCANCODE_F13: case SDL_SCANCODE_F14: case SDL_SCANCODE_F15:
        case SDL_SCANCODE_F16: case SDL_SCANCODE_F17: case SDL_SCANCODE_F18:
        case SDL_SCANCODE_F19: case SDL_SCANCODE_F20: case SDL_SCANCODE_F21:
        case SDL_SCANCODE_F22: case SDL_SCANCODE_F23: case SDL_SCANCODE_F24:
        case SDL_SCANCODE_PRINTSCREEN: case SDL_SCANCODE_SCROLLLOCK: case SDL_SCANCODE_PAUSE:
        case SDL_SCANCODE_INSERT: case SDL_SCANCODE_HOME: case SDL_SCANCODE_PAGEUP:
        case SDL_SCANCODE_DELETE: case SDL_SCANCODE_END: case SDL_SCANCODE_PAGEDOWN:
        case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_LEFT:
        case SDL_SCANCODE_DOWN: case SDL_SCANCODE_UP:
        case SDL_SCANCODE_NUMLOCKCLEAR:
        case SDL_SCANCODE_KP_DIVIDE: case SDL_SCANCODE_KP_MULTIPLY:
        case SDL_SCANCODE_KP_MINUS: case SDL_SCANCODE_KP_PLUS:
        case SDL_SCANCODE_KP_ENTER:
        case SDL_SCANCODE_KP_1: case SDL_SCANCODE_KP_2: case SDL_SCANCODE_KP_3:
        case SDL_SCANCODE_KP_4: case SDL_SCANCODE_KP_5: case SDL_SCANCODE_KP_6:
        case SDL_SCANCODE_KP_7: case SDL_SCANCODE_KP_8: case SDL_SCANCODE_KP_9:
        case SDL_SCANCODE_KP_0: case SDL_SCANCODE_KP_PERIOD:
        case SDL_SCANCODE_KP_EQUALS: case SDL_SCANCODE_KP_COMMA:
        case SDL_SCANCODE_KP_EQUALSAS400:
        case SDL_SCANCODE_NONUSBACKSLASH: case SDL_SCANCODE_APPLICATION:
            return static_cast<uint16_t>(scancode);
        default:
            return 0;
    }
}

} // namespace

KeyboardState::KeyboardState(uint16_t shortcut_mods) : shortcut_mods_(shortcut_mods) {}

KeyboardState::Report KeyboardState::held() const {
    Report result;
    for (std::size_t usage = 0; usage < held_.size(); ++usage)
        if (held_[usage]) result.push_back(static_cast<uint16_t>(usage));
    return result;
}

bool KeyboardState::is_pressed(SDL_Scancode scancode) const {
    return valid_index(scancode) &&
           owners_[static_cast<std::size_t>(scancode)] != Owner::None;
}

KeyboardState::Reports KeyboardState::changes_from(const Report &previous) const {
    auto current = held();
    if (current == previous) return {};
    return {std::move(current)};
}

void KeyboardState::sync_modifiers(uint16_t modifiers) {
    for (const auto &modifier : Modifiers) {
        auto &owner = owners_[static_cast<std::size_t>(modifier.scancode)];
        const bool down = (modifiers & modifier.mask) != 0 &&
                          (shortcut_mods_ & modifier.mask) == 0 && owner != Owner::Local;
        held_[modifier.usage] = down;
        // 允许新的主键带入尚未收到 DOWN 的修饰键，后续该修饰键的 UP 仍须生效。
        if (down && owner == Owner::None) owner = Owner::Device;
        // 快照已确认设备修饰键松开，归属也需清除，下一次真实 DOWN 才会重新生效。
        // 本地键仍保留到自己的 UP，避免后续快照把快捷键重新引入设备集合。
        if (!down && owner == Owner::Device) owner = Owner::None;
    }
}

KeyboardState::Reports KeyboardState::key_down(SDL_Scancode scancode, uint16_t modifiers,
                                               bool repeat, bool consumed) {
    if (repeat || !valid_index(scancode)) return {};
    auto &owner = owners_[static_cast<std::size_t>(scancode)];
    if (owner != Owner::None) return {};
    const auto *modifier = modifier_for(scancode);
    const auto usage = modifier ? modifier->usage : ordinary_usage(scancode);
    if (usage == 0) return {};
    const bool local = consumed || (modifier && (shortcut_mods_ & modifier->mask) != 0);
    owner = local ? Owner::Local : Owner::Device;
    if (local) return {};

    const auto previous = held();
    if (modifier) {
        // SDL 的 modifier 快照可能不含本次 DOWN；键事件本身决定自己的状态。
        sync_modifiers(static_cast<uint16_t>(modifiers | modifier->mask));
        return changes_from(previous);
    }

    sync_modifiers(modifiers);
    auto reports = changes_from(previous);
    held_[usage] = true;
    reports.push_back(held());
    return reports;
}

KeyboardState::Reports KeyboardState::key_up(SDL_Scancode scancode, uint16_t modifiers) {
    if (!valid_index(scancode)) return {};
    auto &owner = owners_[static_cast<std::size_t>(scancode)];
    const auto previous_owner = owner;
    owner = Owner::None;
    // 未见 DOWN 或本地消费的键，不会因为 UP 的 modifier 快照产生新设备报告。
    if (previous_owner != Owner::Device) return {};
    const auto previous = held();
    if (const auto *modifier = modifier_for(scancode)) {
        // 同理，UP 的快照仍带自己的位时也必须松开，不能重新引入。
        sync_modifiers(static_cast<uint16_t>(modifiers & ~modifier->mask));
    } else {
        held_[ordinary_usage(scancode)] = false;
        sync_modifiers(modifiers);
    }
    return changes_from(previous);
}

KeyboardState::Reports KeyboardState::release_all() {
    const bool had_device_keys = !held().empty();
    owners_.fill(Owner::None);
    held_.fill(false);
    return had_device_keys ? Reports{Report{}} : Reports{};
}

KeyboardState::Reports KeyboardState::release_device_keys() {
    const bool had_device_keys = !held().empty();
    for (auto &owner : owners_)
        if (owner == Owner::Device) owner = Owner::None;
    held_.fill(false);
    return had_device_keys ? Reports{Report{}} : Reports{};
}

} // namespace scrctl::app
