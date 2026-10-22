#pragma once
#include <cstdint>
namespace scrctl::hid {
namespace key {
inline constexpr uint16_t kA = 0x04; ///< a..z 连续排到 0x1D
inline constexpr uint16_t kZ = 0x1D;
inline constexpr uint16_t k1 = 0x1E; ///< 1..9 到 0x26，0 是 0x27
inline constexpr uint16_t k0 = 0x27;
inline constexpr uint16_t kEnter = 0x28;
inline constexpr uint16_t kEsc = 0x29;
inline constexpr uint16_t kBackspace = 0x2A;
inline constexpr uint16_t kTab = 0x2B;
inline constexpr uint16_t kSpace = 0x2C;
inline constexpr uint16_t kShiftLeft = 0xE1; ///< 0xE0..0xE7 为左右修饰键
inline constexpr uint16_t kGuiLeft = 0xE3;   ///< 即 iOS 上的 Command 键
} // namespace key
namespace button {
inline constexpr uint16_t kUsagePageConsumer = 0x0C;
inline constexpr uint16_t kHome = 0x40;
inline constexpr uint16_t kLock = 0x30;
inline constexpr uint16_t kVolumeUp = 0xE9;
inline constexpr uint16_t kVolumeDown = 0xEA;
inline constexpr uint16_t kMute = 0xE2;
} // namespace button
} // namespace scrctl::hid
